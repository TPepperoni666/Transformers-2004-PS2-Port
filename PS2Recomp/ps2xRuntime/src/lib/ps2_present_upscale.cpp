#include "runtime/ps2_settings.h"
// Presentation upscale + sharpen pass (interim, until the native renderer): PS2X_UPSCALE=fsr1|cas|off, PS2X_SHARPNESS=0..1.
//
// The PS2 frame (parallel-gs or the CPU GS: ~512/640 x 448, CPU readback -> glTexSubImage2D into frameTex) used to be
// stretched to the window with GL_NEAREST by DrawTexturePro. This pass replaces that one draw with an upscale that runs
// on the GL presenter's GPU at the window/output resolution, on the final frame (HUD included):
//   fsr1 (default): AMD FidelityFX FSR 1 - EASU (edge adaptive spatial upsampling) into an output-sized render texture,
//                   then RCAS (robust contrast adaptive sharpening) onto the back buffer. PS2X_SHARPNESS=0 skips RCAS.
//   cas:            AMD FidelityFX CAS with its built-in scaling (bilinear blend of four CAS kernels = CAS at the
//                   PS2 resolution + bilinear upscale), one pass; cheaper and softer than fsr1.
//   off:            the old GL_NEAREST stretch, bit-for-bit the previous presenter.
// Purely host-side: it only changes how the already-latched frame texture is drawn, so guest behaviour, timing and the
// test-harness step dumps (native frame) are untouched. Works under Proton (plain GLSL 330, no interop, no compute).
// Any shader/GL failure falls back to the old stretch. F9 cycles off -> fsr1 -> cas at runtime (PS2X_UPSCALE_HOTKEY=0
// disables the key).
//
// EASU/RCAS below are a GLSL 330 port of FsrEasuF / FsrRcasF from AMD FidelityFX FSR 1 (ffx_fsr1.h v1.20210629, as
// vendored in private qualification evidence); gather4 replaced by 12 texelFetch
// taps clamped to the frame rectangle. CAS follows the public FidelityFX CAS algorithm (ffx_cas.h scaling path).
// Both are:
//   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
//   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
//   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "ps2_host_backend.h"
#include "rlgl.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if !defined(PLATFORM_VITA) && !defined(PLATFORM_ANDROID) && !defined(__ANDROID__)
#define PS2X_UPSCALE_GL_TIMER 1 // TEMP(transformers): PS2X_UPSCALE_BENCH GPU timer queries (GLFW desktop builds)
extern "C" void *glfwGetProcAddress(const char *procname);
#endif

namespace
{
    enum class Mode : int
    {
        Off = 0,
        Fsr1 = 1,
        Cas = 2,
    };

    const char *modeName(Mode m)
    {
        return m == Mode::Fsr1 ? "fsr1" : (m == Mode::Cas ? "cas" : "off");
    }

    // ---- shaders (raylib's default GLSL 330 vertex shader feeds these; all addressing is from gl_FragCoord) --------
    const char *kCommon = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 uSrc;  // frame rectangle in texels (x, y, w, h), top-down
uniform vec4 uDst;  // output rectangle origin (x, y) in target pixels top-down, target height, 1 = texture stored bottom-up
uniform vec4 uCon0; // input pixels per output pixel (xy), offset of output pixel 0 in input pixels (zw)
uniform vec4 uCon1; // x: RCAS sharpness (linear), y: CAS peak (-1/mix(8,5,s))
ivec2 outPos() { return ivec2(floor(vec2(gl_FragCoord.x, uDst.z - gl_FragCoord.y) - uDst.xy)); }
vec3 Ld(ivec2 p)
{
    ivec2 lo = ivec2(uSrc.xy);
    ivec2 hi = lo + ivec2(uSrc.zw) - ivec2(1);
    p = clamp(p + lo, lo, hi);
    if (uDst.w > 0.5)
        p.y = textureSize(texture0, 0).y - 1 - p.y;
    return texelFetch(texture0, p, 0).rgb;
}
float rcpLo(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float rcpMed(float a) { float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a)); return b * (-b * a + 2.0); }
float rsqLo(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
float lum2(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); } // FSR "luma times 2"
)GLSL";

    const char *kEasu = R"GLSL(
void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    //    a
    //  b c d
    //    e
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = rcpLo(max(abs(dc), abs(cb)));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = rcpLo(max(abs(ec), abs(ca)));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
void main()
{
    vec2 pp = vec2(outPos()) * uCon0.xy + uCon0.zw;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 q = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = Ld(q + ivec2(0, -1)), c = Ld(q + ivec2(1, -1));
    vec3 e = Ld(q + ivec2(-1, 0)), f = Ld(q), g = Ld(q + ivec2(1, 0)), h = Ld(q + ivec2(2, 0));
    vec3 i = Ld(q + ivec2(-1, 1)), j = Ld(q + ivec2(0, 1)), k = Ld(q + ivec2(1, 1)), l = Ld(q + ivec2(2, 1));
    vec3 n = Ld(q + ivec2(0, 2)), o = Ld(q + ivec2(1, 2));
    float bL = lum2(b), cL = lum2(c), eL = lum2(e), fL = lum2(f), gL = lum2(g), hL = lum2(h);
    float iL = lum2(i), jL = lum2(j), kL = lum2(k), lL = lum2(l), nL = lum2(n), oL = lum2(o);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : rsqLo(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcpLo(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcpLo(lob);
    vec3 mn4 = min(min(f, g), min(j, k));
    vec3 mx4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    finalColor = vec4(min(mx4, max(mn4, aC * (1.0 / aW))), 1.0);
}
)GLSL";

    const char *kRcas = R"GLSL(
void main()
{
    //    b
    //  d e f
    //    h
    ivec2 sp = outPos();
    vec3 b = Ld(sp + ivec2(0, -1)), d = Ld(sp + ivec2(-1, 0)), e = Ld(sp), f = Ld(sp + ivec2(1, 0)), h = Ld(sp + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // Limiters (exact rcp). Guards keep flat black/white areas finite (the reference relies on GPU max() eating NaN).
    vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1.0e-6));
    vec3 hitMax = (vec3(1.0) - mx4) / min(4.0 * mn4 - vec3(4.0), vec3(-1.0e-6));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * uCon1.x;
    float rcpL = rcpMed(4.0 * lobe + 1.0);
    finalColor = vec4((lobe * (b + d + h + f) + e) * rcpL, 1.0);
}
)GLSL";

    const char *kCas = R"GLSL(
void main()
{
    vec2 pp = vec2(outPos()) * uCon0.xy + uCon0.zw;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 q = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = Ld(q + ivec2(0, -1)), c = Ld(q + ivec2(1, -1));
    vec3 e = Ld(q + ivec2(-1, 0)), f = Ld(q), g = Ld(q + ivec2(1, 0)), h = Ld(q + ivec2(2, 0));
    vec3 i = Ld(q + ivec2(-1, 1)), j = Ld(q + ivec2(0, 1)), k = Ld(q + ivec2(1, 1)), l = Ld(q + ivec2(2, 1));
    vec3 n = Ld(q + ivec2(0, 2)), o = Ld(q + ivec2(1, 2));
    // Cross min/max of the four CAS kernels centred on f, g, j, k.
    vec3 mnf = min(min(min(b, e), f), min(g, j)), mxf = max(max(max(b, e), f), max(g, j));
    vec3 mng = min(min(min(c, f), g), min(h, k)), mxg = max(max(max(c, f), g), max(h, k));
    vec3 mnj = min(min(min(f, i), j), min(k, n)), mxj = max(max(max(f, i), j), max(k, n));
    vec3 mnk = min(min(min(g, j), k), min(l, o)), mxk = max(max(max(g, j), k), max(l, o));
    // Amount of sharpening from the distance to the signal limits, shaped by sqrt; peak = -1/mix(8,5,sharpness).
    vec3 wf = sqrt(clamp(min(mnf, vec3(1.0) - mxf) / max(mxf, vec3(1.0e-5)), 0.0, 1.0)) * uCon1.y;
    vec3 wg = sqrt(clamp(min(mng, vec3(1.0) - mxg) / max(mxg, vec3(1.0e-5)), 0.0, 1.0)) * uCon1.y;
    vec3 wj = sqrt(clamp(min(mnj, vec3(1.0) - mxj) / max(mxj, vec3(1.0e-5)), 0.0, 1.0)) * uCon1.y;
    vec3 wk = sqrt(clamp(min(mnk, vec3(1.0) - mxk) / max(mxk, vec3(1.0e-5)), 0.0, 1.0)) * uCon1.y;
    // Plain bilinear weights. (ffx_cas.h also 'thins' them by 1/(1/32 + local contrast), which snaps edges to the
    // nearest source pixel; fine for CAS's intended <=1.5x DRS range, visibly stair-stepped at our ~3.2x.)
    float s = (1.0 - pp.x) * (1.0 - pp.y);
    float t = pp.x * (1.0 - pp.y);
    float u = (1.0 - pp.x) * pp.y;
    float v = pp.x * pp.y;
    vec3 qbe = wf * s, qch = wg * t, qin = wj * u, qlo = wk * v;
    vec3 qf = s + wg * t + wj * u;
    vec3 qg = t + wf * s + wk * v;
    vec3 qj = u + wf * s + wk * v;
    vec3 qk = v + wg * t + wj * u;
    vec3 sum = (b + e) * qbe + (c + h) * qch + (i + n) * qin + (l + o) * qlo + f * qf + g * qg + j * qj + k * qk;
    vec3 wsum = 2.0 * (qbe + qch + qin + qlo) + qf + qg + qj + qk;
    finalColor = vec4(clamp(sum / wsum, 0.0, 1.0), 1.0);
}
)GLSL";

    struct Pass
    {
        Shader shader{};
        int locSrc = -1, locDst = -1, locCon0 = -1, locCon1 = -1;
        bool ok = false;
    };

    struct State
    {
        bool init = false;
        bool glOk = false;
        Mode mode = Mode::Fsr1;
        float sharpness = 0.8f;
        bool hotkey = true;
        Pass easu, rcas, cas;
        RenderTexture2D rt{};
        int rtW = 0, rtH = 0;
        // TEMP(transformers): PS2X_UPSCALE_CAPTURE=<W>x<H> - every PS2X_SCRIPT step dump is also rendered offscreen at WxH
        // through nearest/bilinear/fsr1/cas (step_..._<W>x<H>_<mode>.png) for before/after evidence.
        int capW = 0, capH = 0;
        std::string capPending;
        // TEMP(transformers): PS2X_UPSCALE_BENCH=1 - GPU time of the present draw (GL_TIME_ELAPSED), printed every 300 frames.
        bool bench = false;
    };

    State g;

    float envFloat(const char *name, float fallback)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? static_cast<float>(std::atof(v)) : fallback;
    }

    bool loadPass(Pass &p, const char *body, const char *name)
    {
        std::string src = std::string(kCommon) + body;
        p.shader = LoadShaderFromMemory(nullptr, src.c_str());
        p.ok = p.shader.id != 0u && p.shader.id != rlGetShaderIdDefault();
        if (!p.ok)
        {
            std::cout << "[present:upscale] " << name << " shader failed to compile/link; falling back to the plain stretch"
                      << std::endl;
            return false;
        }
        p.locSrc = GetShaderLocation(p.shader, "uSrc");
        p.locDst = GetShaderLocation(p.shader, "uDst");
        p.locCon0 = GetShaderLocation(p.shader, "uCon0");
        p.locCon1 = GetShaderLocation(p.shader, "uCon1");
        return true;
    }

    void unloadPass(Pass &p)
    {
        if (p.ok)
            UnloadShader(p.shader);
        p = Pass{};
    }

    void initOnce()
    {
        if (g.init)
            return;
        g.init = true;
        if (const char *m = std::getenv("PS2X_UPSCALE"))
        {
            std::string s(m);
            for (char &ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (s == "off" || s == "0" || s == "none" || s == "nearest")
                g.mode = Mode::Off;
            else if (s == "cas")
                g.mode = Mode::Cas;
            else
                g.mode = Mode::Fsr1;
        }
        g.sharpness = std::clamp(envFloat("PS2X_SHARPNESS", 0.8f), 0.0f, 1.0f);
        const char *hk = std::getenv("PS2X_UPSCALE_HOTKEY");
        g.hotkey = !(hk && hk[0] == '0');
        const char *bench = std::getenv("PS2X_UPSCALE_BENCH");
        g.bench = bench && bench[0] == '1';
        if (const char *cap = std::getenv("PS2X_UPSCALE_CAPTURE"))
        {
            int w = 0, h = 0;
            if (std::sscanf(cap, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 8192 && h <= 8192)
            {
                g.capW = w;
                g.capH = h;
            }
        }
        // TEMP(transformers): PS2X_UPSCALE_WINDOW=<W>x<H>[@<x>,<y>] resizes (and optionally moves) the window once, so an
        // unattended run (640x448 window = no upscale) exercises the real on-screen pass; pair with PS2X_UPSCALE_BENCH.
        if (const char *win = std::getenv("PS2X_UPSCALE_WINDOW"))
        {
            int w = 0, h = 0, x = 0, y = 0;
            const int n = std::sscanf(win, "%dx%d@%d,%d", &w, &h, &x, &y);
            if (n >= 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192)
            {
                SetWindowSize(w, h);
                if (n == 4)
                    SetWindowPosition(x, y);
            }
        }
        const int ver = rlGetVersion();
        g.glOk = ver == RL_OPENGL_33 || ver == RL_OPENGL_43;
        if (g.glOk)
        {
            const bool e = loadPass(g.easu, kEasu, "EASU");
            const bool r = loadPass(g.rcas, kRcas, "RCAS");
            const bool c = loadPass(g.cas, kCas, "CAS");
            g.glOk = e && r && c;
        }
        else
            std::cout << "[present:upscale] needs OpenGL 3.3 (raylib GL version " << ver << "); plain stretch only" << std::endl;
        std::cout << "[present:upscale] mode=" << modeName(g.mode) << " sharpness=" << g.sharpness
                  << (g.glOk ? "" : " (unavailable -> off)") << (g.hotkey ? " F9=cycle" : "") << std::endl;
    }

    void setPassUniforms(const Pass &p, const float src[4], const float dst[4], const float con0[4], const float con1[4])
    {
        SetShaderValue(p.shader, p.locSrc, src, SHADER_UNIFORM_VEC4);
        SetShaderValue(p.shader, p.locDst, dst, SHADER_UNIFORM_VEC4);
        SetShaderValue(p.shader, p.locCon0, con0, SHADER_UNIFORM_VEC4);
        SetShaderValue(p.shader, p.locCon1, con1, SHADER_UNIFORM_VEC4);
    }

    // RCAS strength: PS2X_SHARPNESS 1 -> 0 stops (AMD's maximum), 0.5 -> 1 stop; 0 skips RCAS.
    float rcasLinear(float s) { return std::exp2(-2.0f * (1.0f - s)); }
    float casPeak(float s) { return -1.0f / (8.0f + (5.0f - 8.0f) * s); }

    bool ensureRt(int w, int h)
    {
        if (g.rtW == w && g.rtH == h && IsRenderTextureValid(g.rt))
            return true;
        if (IsRenderTextureValid(g.rt))
            UnloadRenderTexture(g.rt);
        g.rt = LoadRenderTexture(w, h);
        g.rtW = g.rtH = 0;
        if (!IsRenderTextureValid(g.rt))
            return false;
        g.rtW = w;
        g.rtH = h;
        return true;
    }

    // Draws tex[src] into the integer rectangle dst of the current target (targetH = its height in pixels).
    void drawMode(Mode mode, const Texture2D &tex, Rectangle src, Rectangle dst, int targetH)
    {
        const bool bottomUp=src.height<0;
        const Rectangle drawSrc=src;
        src.height=std::fabs(src.height);
        const float sourceFlip=bottomUp?1.0f:0.0f;
        const bool upscale = dst.width > src.width || dst.height > src.height;
        if (mode == Mode::Off || !g.glOk || !upscale)
        {
            DrawTexturePro(tex, drawSrc, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            return;
        }
        const float srcU[4] = {src.x, src.y, src.width, src.height};
        const float con0[4] = {src.width / dst.width, src.height / dst.height, 0.5f * src.width / dst.width - 0.5f,
                               0.5f * src.height / dst.height - 0.5f};
        const float con1[4] = {rcasLinear(g.sharpness), casPeak(g.sharpness), 0.0f, 0.0f};
        if (mode == Mode::Cas)
        {
            const float dstU[4] = {dst.x, dst.y, static_cast<float>(targetH), sourceFlip};
            BeginShaderMode(g.cas.shader);
            setPassUniforms(g.cas, srcU, dstU, con0, con1);
            DrawTexturePro(tex, drawSrc, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            return;
        }
        // FSR 1.
        const int w = static_cast<int>(dst.width), h = static_cast<int>(dst.height);
        if (g.sharpness <= 0.0f || !ensureRt(w, h))
        {
            const float dstU[4] = {dst.x, dst.y, static_cast<float>(targetH), sourceFlip};
            BeginShaderMode(g.easu.shader);
            setPassUniforms(g.easu, srcU, dstU, con0, con1);
            DrawTexturePro(tex, drawSrc, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            return;
        }
        // Pass 1: EASU -> output-sized render texture (EndTextureMode returns to the back buffer). Only used for the
        // screen; the PS2X_UPSCALE_CAPTURE path runs the two FSR passes itself because its target is a render texture.
        rlDrawRenderBatchActive();
        {
            const float dstU[4] = {0.0f, 0.0f, static_cast<float>(h), sourceFlip};
            BeginTextureMode(g.rt);
            BeginShaderMode(g.easu.shader);
            setPassUniforms(g.easu, srcU, dstU, con0, con1);
            DrawTexturePro(tex, drawSrc, Rectangle{0.0f, 0.0f, dst.width, dst.height}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();
        }
        // Pass 2: RCAS reads the render texture (stored bottom-up) 1:1 and writes dst.
        {
            const float rtSrc[4] = {0.0f, 0.0f, dst.width, dst.height};
            const float dstU[4] = {dst.x, dst.y, static_cast<float>(targetH), 1.0f};
            BeginShaderMode(g.rcas.shader);
            setPassUniforms(g.rcas, rtSrc, dstU, con0, con1);
            DrawTexturePro(g.rt.texture, Rectangle{0.0f, 0.0f, dst.width, -dst.height}, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
        }
    }

    Rectangle snap(Rectangle r)
    {
        const float x0 = std::round(r.x), y0 = std::round(r.y);
        const float x1 = std::round(r.x + r.width), y1 = std::round(r.y + r.height);
        return Rectangle{x0, y0, std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)};
    }

#ifdef PS2X_UPSCALE_GL_TIMER
    // TEMP(transformers): GL_TIME_ELAPSED ring (results read 4 frames late, never stalls).
    struct GpuTimer
    {
        typedef void (*GenQueries)(int, unsigned int *);
        typedef void (*BeginQuery)(unsigned int, unsigned int);
        typedef void (*EndQuery)(unsigned int);
        typedef void (*GetQueryObjectiv)(unsigned int, unsigned int, int *);
        typedef void (*GetQueryObjectui64v)(unsigned int, unsigned int, uint64_t *);
        GenQueries gen = nullptr;
        BeginQuery begin = nullptr;
        EndQuery end = nullptr;
        GetQueryObjectiv getiv = nullptr;
        GetQueryObjectui64v getui64 = nullptr;
        static constexpr unsigned kTimeElapsed = 0x88BF, kResultAvailable = 0x8867, kResult = 0x8866;
        unsigned int q[8]{};
        bool used[8]{};
        unsigned frame = 0;
        bool ok = false, tried = false;
        double sumMs = 0.0, maxMs = 0.0, sumCpuMs = 0.0;
        unsigned count = 0;
        void init()
        {
            tried = true;
            gen = reinterpret_cast<GenQueries>(glfwGetProcAddress("glGenQueries"));
            begin = reinterpret_cast<BeginQuery>(glfwGetProcAddress("glBeginQuery"));
            end = reinterpret_cast<EndQuery>(glfwGetProcAddress("glEndQuery"));
            getiv = reinterpret_cast<GetQueryObjectiv>(glfwGetProcAddress("glGetQueryObjectiv"));
            getui64 = reinterpret_cast<GetQueryObjectui64v>(glfwGetProcAddress("glGetQueryObjectui64v"));
            ok = gen && begin && end && getiv && getui64;
            if (ok)
                gen(8, q);
        }
    };
    GpuTimer gt;
#endif
}

// Display aspect of the presented PS2 frame (width / height of the picture on screen). The PS2 scans every display mode
// (512x448 in-game/menus, 640x448 FMVs) out across the full width of a 4:3 TV, so the presenter shows the frame at 4:3
// (fit inside the window, letter/pillarboxed) instead of at square pixels (512x448 = 1.14:1, 640x448 = 1.43:1).
// PS2X_PRESENT_ASPECT=4:3 (default) | <w>:<h> | <float> | square (= 0: the old square-pixel fit).
float ps2PresentFrameAspect(float frameWidth); // ps2_runtime.cpp: PS2X_WIDESCREEN (FMV frames 4:3) or ps2PresentDisplayAspect()

float ps2PresentDisplayAspect()
{
    static const float aspect = []() -> float {
        float a = 4.0f / 3.0f;
        const char *e = std::getenv("PS2X_PRESENT_ASPECT");
        if (e && *e)
        {
            if (std::strcmp(e, "square") == 0 || std::strcmp(e, "native") == 0 || std::strcmp(e, "0") == 0)
                a = 0.0f;
            else
            {
                float w = 0.0f, h = 0.0f;
                if (std::sscanf(e, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
                    a = w / h;
                else if (std::sscanf(e, "%f", &w) == 1 && w > 0.5f && w < 8.0f)
                    a = w;
            }
        }
        if (a > 0.0f)
            std::cout << "[present:aspect] display aspect " << a << " (PS2X_PRESENT_ASPECT=square for square pixels)" << std::endl;
        else
            std::cout << "[present:aspect] square pixels (PS2X_PRESENT_ASPECT=square)" << std::endl;
        return a;
    }();
    return aspect;
}

// Render thread, between BeginDrawing/EndDrawing: replaces DrawTexturePro(frameTex, src, dst, {0,0}, 0, WHITE).
void ps2PresentUpscaleDraw(const Texture2D &tex, Rectangle src, Rectangle dst)
{
    initOnce();
    static uint64_t pcVersion = 0;
    if (pcVersion != ps2x::settings::version()) {
        pcVersion = ps2x::settings::version();
        const auto filter = ps2x::settings::get("display.scaling_filter");
        if (ps2x::settings::source("display.scaling_filter") != ps2x::settings::Source::Default) {
            g.mode = filter == "fsr1" ? Mode::Fsr1 : filter == "cas" ? Mode::Cas : Mode::Off;
            SetTextureFilter(tex, filter == "bilinear" ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
        }
        if (ps2x::settings::source("display.sharpness") != ps2x::settings::Source::Default)
            g.sharpness = float(ps2x::settings::getFloat("display.sharpness"));
    }
    if (g.hotkey && IsKeyPressed(KEY_F9))
    {
        g.mode = static_cast<Mode>((static_cast<int>(g.mode) + 1) % 3);
        std::cout << "[present:upscale] F9 -> mode=" << modeName(g.mode) << std::endl;
    }
    const Rectangle d = snap(dst);
#ifdef PS2X_UPSCALE_GL_TIMER
    if (g.bench)
    {
        if (!gt.tried)
            gt.init();
        if (gt.ok)
        {
            rlDrawRenderBatchActive(); // keep earlier batched draws out of the measurement
            const unsigned slot = gt.frame % 8u;
            if (gt.used[slot])
            {
                int avail = 0;
                gt.getiv(gt.q[slot], GpuTimer::kResultAvailable, &avail);
                if (avail)
                {
                    uint64_t ns = 0;
                    gt.getui64(gt.q[slot], GpuTimer::kResult, &ns);
                    const double ms = double(ns) / 1.0e6;
                    gt.sumMs += ms;
                    gt.maxMs = std::max(gt.maxMs, ms);
                    ++gt.count;
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            gt.begin(GpuTimer::kTimeElapsed, gt.q[slot]);
            drawMode(g.mode, tex, src, d, GetRenderHeight());
            rlDrawRenderBatchActive();
            gt.end(GpuTimer::kTimeElapsed);
            gt.sumCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            gt.used[slot] = true;
            if (++gt.frame % 300u == 0u && gt.count)
            {
                std::printf("[present:upscale] bench mode=%s src=%gx%g dst=%gx%g gpu avg %.3f ms max %.3f ms (n=%u), cpu "
                            "submit avg %.3f ms\n",
                            modeName(g.mode), src.width, src.height, d.width, d.height, gt.sumMs / gt.count, gt.maxMs,
                            gt.count, gt.sumCpuMs / 300.0);
                std::fflush(stdout);
                gt.sumMs = gt.maxMs = gt.sumCpuMs = 0.0;
                gt.count = 0;
            }
            return;
        }
    }
#endif
    drawMode(g.mode, tex, src, d, GetRenderHeight());
}

// TEMP(transformers): ps2_runtime.cpp ServiceTestScriptDump reports each step PNG it wrote (render thread).
void ps2PresentUpscaleNoteDump(const char *path)
{
    initOnce();
    if (g.capW > 0 && path)
        g.capPending = path;
}

// TEMP(transformers): render thread, outside BeginDrawing (after UploadFrame): PS2X_UPSCALE_CAPTURE offscreen renders.
void ps2PresentUpscaleCapture(const Texture2D &tex, Rectangle src)
{
    if (g.capPending.empty() || g.capW <= 0)
        return;
    const std::string base = g.capPending.size() > 4 ? g.capPending.substr(0, g.capPending.size() - 4) : g.capPending;
    g.capPending.clear();
    // Same placement rule as PS2Runtime::run: fit at this frame's display aspect (ps2PresentFrameAspect: PS2X_WIDESCREEN
    // or the 4:3 default), centred.
    const float dispAspect = ps2PresentFrameAspect(src.width);
    const float srcW = (dispAspect > 0.0f) ? src.height * dispAspect : src.width;
    const float scale = std::min(float(g.capW) / srcW, float(g.capH) / src.height);
    const Rectangle dst = snap(Rectangle{(g.capW - srcW * scale) * 0.5f, (g.capH - src.height * scale) * 0.5f,
                                         srcW * scale, src.height * scale});
    RenderTexture2D target = LoadRenderTexture(g.capW, g.capH);
    if (!IsRenderTextureValid(target))
        return;
    const char *names[4] = {"nearest", "bilinear", "fsr1", "cas"};
    for (int m = 0; m < 4; ++m)
    {
        if (m >= 2 && !g.glOk)
            break;
        // FSR's EASU pass needs the shared output RT; render the FSR result into it first, then copy into target.
        if (m == 2 && g.sharpness > 0.0f)
        {
            // Same two passes as drawMode, but pass 2 lands in the capture target instead of the back buffer.
            const int w = int(dst.width), h = int(dst.height);
            if (!ensureRt(w, h))
                continue;
            const float srcU[4] = {src.x, src.y, src.width, src.height};
            const float con0[4] = {src.width / dst.width, src.height / dst.height, 0.5f * src.width / dst.width - 0.5f,
                                   0.5f * src.height / dst.height - 0.5f};
            const float con1[4] = {rcasLinear(g.sharpness), casPeak(g.sharpness), 0.0f, 0.0f};
            {
                const float dstU[4] = {0.0f, 0.0f, float(h), 0.0f};
                BeginTextureMode(g.rt);
                BeginShaderMode(g.easu.shader);
                setPassUniforms(g.easu, srcU, dstU, con0, con1);
                DrawTexturePro(tex, src, Rectangle{0.0f, 0.0f, dst.width, dst.height}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
                EndShaderMode();
                EndTextureMode();
            }
            BeginTextureMode(target);
            ClearBackground(BLACK);
            {
                const float rtSrc[4] = {0.0f, 0.0f, dst.width, dst.height};
                const float dstU[4] = {dst.x, dst.y, float(g.capH), 1.0f};
                BeginShaderMode(g.rcas.shader);
                setPassUniforms(g.rcas, rtSrc, dstU, con0, con1);
                DrawTexturePro(g.rt.texture, Rectangle{0.0f, 0.0f, dst.width, -dst.height}, dst, Vector2{0.0f, 0.0f}, 0.0f,
                               WHITE);
                EndShaderMode();
            }
            EndTextureMode();
        }
        else
        {
            if (m == 1)
                SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
            BeginTextureMode(target);
            ClearBackground(BLACK);
            drawMode(m == 3 ? Mode::Cas : (m == 2 ? Mode::Fsr1 : Mode::Off), tex, src, dst, g.capH);
            EndTextureMode();
            if (m == 1)
                SetTextureFilter(tex, TEXTURE_FILTER_POINT);
        }
        Image img = LoadImageFromTexture(target.texture);
        ImageFlipVertical(&img);
        char name[600];
        std::snprintf(name, sizeof(name), "%s_%dx%d_%s.png", base.c_str(), g.capW, g.capH, names[m]);
        ExportImage(img, name);
        UnloadImage(img);
    }
    UnloadRenderTexture(target);
    std::cout << "[present:upscale] capture " << base << "_" << g.capW << "x" << g.capH << "_{nearest,bilinear,fsr1,cas}.png"
              << std::endl;
}

// Render thread, before CloseWindow.
void ps2PresentUpscaleShutdown()
{
    if (!g.init)
        return;
    if (IsRenderTextureValid(g.rt))
        UnloadRenderTexture(g.rt);
    g.rt = RenderTexture2D{};
    g.rtW = g.rtH = 0;
    unloadPass(g.easu);
    unloadPass(g.rcas);
    unloadPass(g.cas);
    g.init = false;
}
