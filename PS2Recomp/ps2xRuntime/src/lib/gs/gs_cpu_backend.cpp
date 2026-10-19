#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_gs_timeline.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include "../debug_counters.h" // TEMP(transformers)
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <fstream>
#include <unordered_set>
#include <iostream>
#include <iomanip> // TEMP(transformers)
#include <stdexcept>
#include <cstdlib>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define PS2X_GS_HAS_MXCSR 1
#endif

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-gs-cpu-rounding.patch (GPL-3.0).
    // The rasteriser must not inherit the caller's SSE rounding mode: with PS2X_GS_THREAD=0 it runs on the EE thread,
    // which may be inside VU1 execution (round toward zero). Only the rounding bits are touched; FTZ/DAZ and the
    // exception masks of the caller stay as they are.
    class GSCpuRoundingScope
    {
#if defined(PS2X_GS_HAS_MXCSR)
        const unsigned m_rounding = _mm_getcsr() & _MM_ROUND_MASK;

    public:
        GSCpuRoundingScope()
        {
            if (m_rounding != _MM_ROUND_NEAREST)
                _mm_setcsr(_mm_getcsr() & ~_MM_ROUND_MASK);
        }
        ~GSCpuRoundingScope()
        {
            if (m_rounding != _MM_ROUND_NEAREST)
                _mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | m_rounding);
        }
#endif
    };

    // Knobs for A/B runs of the rasteriser fix bundle (read once).
    bool envFlagSet(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }

    // COLCLAMP=0 wraps the blend result (PS2X_GS_COLCLAMP_WRAP=0 restores the old "always clamp").
    bool colclampWrapEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *value = std::getenv("PS2X_GS_COLCLAMP_WRAP");
            return value == nullptr || value[0] != '0';
        }();
        return s_enabled;
    }

    // DTHE dithering for 16-bit frame buffers (PS2X_GS_DITHER=0 disables).
    bool ditherEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *value = std::getenv("PS2X_GS_DITHER");
            return value == nullptr || value[0] != '0';
        }();
        return s_enabled;
    }

    // Early depth rejection before texturing (PS2X_GS_DISABLE_EARLY_DEPTH=1 disables, name from upstream PR #246).
    bool earlyDepthEnabled()
    {
        static const bool s_enabled = !envFlagSet("PS2X_GS_DISABLE_EARLY_DEPTH");
        return s_enabled;
    }

    // Per-pixel Z clamp to the ZBUF format maximum, like PCSX2's GSRendererSW zclamp
    // (z_max = 0xFFFFFFFF >> (fmt * 8)): Z24 -> 0xFFFFFF, Z16/Z16S -> 0xFFFF, Z32 unchanged.
    // PS2X_GS_ZCLAMP=0 keeps only the 32-bit bound (still removes the UB of an out-of-range float -> u32 cast).
    uint32_t depthFormatMax(uint8_t zpsm)
    {
        static const bool s_enabled = []()
        {
            const char *value = std::getenv("PS2X_GS_ZCLAMP");
            return value == nullptr || value[0] != '0';
        }();
        if (!s_enabled)
            return 0xFFFFFFFFu;
        switch (zpsm)
        {
        case GS_PSM_Z24:
            return 0x00FFFFFFu;
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 0x0000FFFFu;
        default:
            return 0xFFFFFFFFu;
        }
    }

    // z is the already-rounded interpolated depth; NaN and negatives give 0.
    uint32_t clampDepthToFormat(double z, uint32_t zmax)
    {
        if (!(z > 0.0))
            return 0u;
        if (z >= static_cast<double>(zmax))
            return zmax;
        return static_cast<uint32_t>(z);
    }

    // Ported from ran-j/PS2Recomp issue #273 (PCSX2 PR #13970) (GPL-3.0): the GS texture-coordinate path is a signed
    // 16-bit fixed-point value, so bound S/T (in texels) to (1 << 15) - 2 before any int conversion; NaN -> 0.
    constexpr float kGsTexCoordLimit = static_cast<float>((1 << 15) - 2);

    float clampTexCoord(float c)
    {
        if (!std::isfinite(c))
            return 0.0f;
        return std::fmin(std::fmax(c, -kGsTexCoordLimit), kGsTexCoordLimit);
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    const std::array<uint8_t, 256> &alphaWriteMasks(uint64_t testReg, uint8_t framePsm)
    {
        const uint32_t method = (testReg >> 1u) & 7u;
        const uint32_t reference = (testReg >> 4u) & 255u;
        if ((testReg & 1u) == 0u || method == 1u || (method == 3u && reference == 255u) || (method == 5u && reference == 0u))
        {
            static constexpr auto pass = [] { std::array<uint8_t, 256> masks{}; for (auto &mask : masks) mask = 7u; return masks; }();
            return pass;
        }
        struct CacheEntry { uint32_t key = ~0u; std::array<uint8_t, 256> masks{}; };
        thread_local std::array<CacheEntry, 4> cache{};
        thread_local uint32_t next = 0;
        const uint32_t key = static_cast<uint32_t>(testReg & 0x3FFFu) | (uint32_t(framePsm == GS_PSM_CT32) << 14u);
        for (const auto &entry : cache) if (entry.key == key) return entry.masks;
        auto &entry = cache[next++ & 3u];
        entry.key = key;
        const uint32_t failure = (testReg >> 12u) & 3u;
        const uint8_t failedMask = failure == 0u ? 0u : failure == 2u ? 4u : failure == 3u && framePsm == GS_PSM_CT32 ? 1u : 3u;
        entry.masks.fill(failedMask);
        switch (method)
        {
        case 2: std::fill(entry.masks.begin(), entry.masks.begin() + reference, 7u); break;
        case 3: std::fill(entry.masks.begin(), entry.masks.begin() + reference + 1u, 7u); break;
        case 4: entry.masks[reference] = 7u; break;
        case 5: std::fill(entry.masks.begin() + reference, entry.masks.end(), 7u); break;
        case 6: std::fill(entry.masks.begin() + reference + 1u, entry.masks.end(), 7u); break;
        case 7: entry.masks.fill(7u); entry.masks[reference] = failedMask; break;
        default: break;
        }
        return entry.masks;
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-gs-bilinear-precision.patch (GPL-3.0):
    // the GS filters with 4-bit weights and truncates after each stage (PCSX2 SW lerp16_4). wx, wy are 0..15.
    uint8_t lerpChannel(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, uint32_t wx, uint32_t wy)
    {
        const uint32_t top = ((16u - wx) * c00 + wx * c10) >> 4;
        const uint32_t bottom = ((16u - wx) * c01 + wx * c11) >> 4;
        return static_cast<uint8_t>(((16u - wy) * top + wy * bottom) >> 4);
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        for (uint32_t y = 0; y < height; y += 2u)
        {
            const uint32_t sourceY = std::min(y + (oddField ? 1u : 0u), height - 1u);
            const uint32_t destY = sourceY == y ? y + 1u : y;
            if (destY < height)
                std::memcpy(pixels.data() + destY * kHostFrameWidth * 4u,
                            pixels.data() + sourceY * kHostFrameWidth * 4u, width * 4u);
        }
    }

    // PCRTC interlaced FRAME mode (SMODE2 INT=1, FFMD=1): each field scans (DH+1)/MAGV/2 consecutive buffer rows
    // (parallel-gs gs_renderer.cpp compute_circuit_rect: "every line, but half height (FFMD)"; PCSX2 GSState the same),
    // e.g. Transformers' loading / warp-gate screens: 640x224 buffers, DISPLAY DH 447 (0x26F1F0). The FIELD-mode path
    // (INT=1, FFMD=0: every other line of a full-height buffer, applyFieldPresentation) is separate and unchanged.
    uint32_t interlacedFrameSourceRows(uint64_t display64, uint32_t outHeight)
    {
        const uint32_t magv = static_cast<uint32_t>((display64 >> 27) & 0x3u) + 1u;
        return std::max<uint32_t>(1u, (outHeight / magv + 1u) / 2u);
    }

    // Expand `rows` field rows (already at the top of `pixels`) to `outHeight` output lines in place, like a CRT shows
    // one field: field line f covers output lines 2f+odd and 2f+odd+1 (line doubling / bob; the odd field sits one
    // output line lower, which the game's half-pixel field offsets (sceGsSetHalfOffset) compensate).
    void expandInterlacedFrameField(std::vector<uint8_t> &pixels, uint32_t width, uint32_t rows, uint32_t outHeight,
                                    uint64_t display64, bool oddField)
    {
        if (pixels.empty() || width == 0u || rows == 0u)
            return;
        const uint32_t magv = static_cast<uint32_t>((display64 >> 27) & 0x3u) + 1u;
        const uint32_t odd = oddField ? 1u : 0u;
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
        // Descending: source row r <= o/2 is never overwritten before it is read.
        for (uint32_t o = outHeight; o-- > 0u;)
        {
            const uint32_t fieldLine = o >= odd ? (o - odd) / 2u : 0u;
            const uint32_t r = std::min<uint32_t>(fieldLine / magv, rows - 1u);
            if (r != o)
                std::memmove(pixels.data() + o * kHostFrameWidth * 4u,
                             pixels.data() + r * kHostFrameWidth * 4u, width * 4u);
        }
    }

    // TEMP(transformers): [present:pcrtc] - log the PCRTC setup when it changes (FBPs excluded: the hub flips DISPFB
    // every frame) and, in interlaced FRAME mode, per-circuit mean RGB/alpha every 16th present. Bounded output.
    struct PcrtcCircuitStats
    {
        double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    };

    PcrtcCircuitStats pcrtcCircuitStats(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        PcrtcCircuitStats s{};
        if (pixels.empty() || width == 0u || height == 0u)
            return s;
        uint64_t sum[4] = {0u, 0u, 0u, 0u};
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                for (uint32_t c = 0; c < 4u; ++c)
                    sum[c] += row[x * 4u + c];
        }
        const double n = static_cast<double>(width) * static_cast<double>(height);
        s.r = sum[0] / n;
        s.g = sum[1] / n;
        s.b = sum[2] / n;
        s.a = sum[3] / n;
        return s;
    }

    void logPcrtcSetup(const GSPresentationRequest &request)
    {
        static std::mutex s_mutex;
        static uint64_t s_lastKey[6] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
        static uint32_t s_lines = 0u;
        const uint64_t key[6] = {request.pmode, request.smode2, request.display1, request.display2,
                                 request.dispfb1 & ~0x1FFull, request.dispfb2 & ~0x1FFull};
        std::lock_guard<std::mutex> lock(s_mutex);
        if (std::memcmp(key, s_lastKey, sizeof(key)) == 0 || s_lines >= 96u)
            return;
        std::memcpy(s_lastKey, key, sizeof(key));
        ++s_lines;
        auto disp = [](uint64_t d)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DX%u DY%u MAGH%u MAGV%u DW%u DH%u",
                          static_cast<unsigned>(d & 0xFFFu), static_cast<unsigned>((d >> 12) & 0x7FFu),
                          static_cast<unsigned>((d >> 23) & 0xFu), static_cast<unsigned>((d >> 27) & 0x3u),
                          static_cast<unsigned>((d >> 32) & 0xFFFu), static_cast<unsigned>((d >> 44) & 0x7FFu));
            return std::string(buf);
        };
        std::printf("[present:pcrtc] tick=%llu pmode=0x%llx smode2=0x%llx dispfb1=FBP%u/FBW%u/PSM%u {%s} "
                    "dispfb2=FBP%u/FBW%u/PSM%u {%s} bg=0x%llx\n",
                    static_cast<unsigned long long>(request.vsyncTick), static_cast<unsigned long long>(request.pmode),
                    static_cast<unsigned long long>(request.smode2),
                    static_cast<unsigned>(request.dispfb1 & 0x1FFu), static_cast<unsigned>((request.dispfb1 >> 9) & 0x3Fu),
                    static_cast<unsigned>((request.dispfb1 >> 15) & 0x1Fu), disp(request.display1).c_str(),
                    static_cast<unsigned>(request.dispfb2 & 0x1FFu), static_cast<unsigned>((request.dispfb2 >> 9) & 0x3Fu),
                    static_cast<unsigned>((request.dispfb2 >> 15) & 0x1Fu), disp(request.display2).c_str(),
                    static_cast<unsigned long long>(request.bgcolor));
        std::fflush(stdout);
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t hasNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    return 1u;
            }
        }
        return 0u;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    const GSCpuRoundingScope rounding; // Ported from god-of-war-recomp ps2recomp-gs-cpu-rounding.patch (GPL-3.0)
    DrawPrimitive(batch);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    // TEMP(transformers): dump each indexed texture once (raw index + CLUT-mapped) as PPM.
    static const bool s_texDebugDumps = std::getenv("PS2X_TEX_DEBUG_DUMPS") != nullptr; // TEMP(transformers): off by default
    if (s_texDebugDumps && state.prim.tme && (isFourBitIndexedPsm(ctx.tex0.psm) || isEightBitIndexedPsm(ctx.tex0.psm)) && m_vram)
    {
        static std::unordered_set<uint32_t> s_dumped;
        const uint32_t key = (ctx.tex0.tbp0 << 8) | ctx.tex0.psm;
        if (s_dumped.size() < 16u && s_dumped.insert(key).second)
        {
            const uint32_t w = 1u << ctx.tex0.tw, h = 1u << ctx.tex0.th;
            char name[96];
            std::snprintf(name, sizeof(name), "tex_%u_psm%02x_idx.pgm", ctx.tex0.tbp0, ctx.tex0.psm);
            std::ofstream idx(name, std::ios::binary);
            std::snprintf(name, sizeof(name), "tex_%u_psm%02x_rgb.ppm", ctx.tex0.tbp0, ctx.tex0.psm);
            std::ofstream rgb(name, std::ios::binary);
            idx << "P5\n" << w << " " << h << "\n255\n";
            rgb << "P6\n" << w << " " << h << "\n255\n";
            const bool four = isFourBitIndexedPsm(ctx.tex0.psm);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t i = ReadTextureVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, x, y);
                    idx.put(static_cast<char>(four ? (i & 0xFu) * 17u : (i & 0xFFu)));
                    const uint32_t c = LookupCLUT(state, static_cast<uint8_t>(i), ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                    rgb.put(static_cast<char>(c & 0xFF)).put(static_cast<char>((c >> 8) & 0xFF)).put(static_cast<char>((c >> 16) & 0xFF));
                }
            std::cout << "[tex:dump] tbp0=" << ctx.tex0.tbp0 << " psm=0x" << std::hex << unsigned(ctx.tex0.psm)
                      << " cbp=" << std::dec << ctx.tex0.cbp << " clut0..3=" << std::hex;
            for (int e = 0; e < 4; ++e)
                std::cout << " " << LookupCLUT(state, static_cast<uint8_t>(e), ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
            std::cout << std::dec << std::endl;
        }
    }
    // TEMP(transformers): PS2X_GSCAPTURE=<frame> dumps every distinct texture drawn in that frame (RGBA as PPM + alpha PGM).
    ps2dbg::primsDrawn.fetch_add(1u, std::memory_order_relaxed);
    if (state.prim.tme)
        ps2dbg::primsTextured.fetch_add(1u, std::memory_order_relaxed);
    ps2dbg::lastPrimFbp.store(ctx.frame.fbp, std::memory_order_relaxed);
    ps2dbg::lastPrimZbp.store(ctx.zbuf.zbp, std::memory_order_relaxed);
    // TEMP(transformers): PS2X_GSCAPTURE_SKIPFBP=<fbp> excludes (and doesn't count) prims drawn into that FRAME buffer.
    static const long long s_captureSkipFbp = []()
    {
        const char *e = std::getenv("PS2X_GSCAPTURE_SKIPFBP");
        return e ? std::atoll(e) : -1ll;
    }();
    const bool captureEligible = !(s_captureSkipFbp >= 0 && static_cast<long long>(ctx.frame.fbp) == s_captureSkipFbp) &&
                           ps2dbg::gsCaptureActive();
    // TEMP(transformers): count skipped primitives in the existing VS budget but avoid logs/texture I/O for them.
    // For a 15000-primitive window after 33000 predecessors: GSCAPTURE_VS=3940:48000; GSCAPTURE_SKIP_PRIMS=33000.
    static const uint64_t s_captureSkipPrims = []()
    {
        const char *e = std::getenv("PS2X_GSCAPTURE_SKIP_PRIMS");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    static uint64_t s_captureEligibleSeen = 0;
    const bool gsCapture = captureEligible && s_captureEligibleSeen++ >= s_captureSkipPrims;
    if (gsCapture && state.prim.tme && m_vram)
    {
        static std::unordered_set<uint64_t> s_captured;
        const uint64_t key = (uint64_t(ctx.tex0.tbp0) << 40) ^ (uint64_t(ctx.tex0.psm) << 32) ^ (uint64_t(ctx.tex0.tbw) << 24) ^
                             (uint64_t(ctx.tex0.cbp) << 8) ^ (uint64_t(ctx.tex0.csa) << 3) ^ (uint64_t(ctx.tex0.tw) << 4) ^ ctx.tex0.th;
        if (s_captured.insert(key).second)
        {
            const uint32_t w = std::min(1u << ctx.tex0.tw, 1024u), h = std::min(1u << ctx.tex0.th, 1024u);
            char name[128];
            std::snprintf(name, sizeof(name), "cap_tex_%u_psm%02x_tbw%u_cbp%u_csa%u.ppm", ctx.tex0.tbp0, ctx.tex0.psm,
                          unsigned(ctx.tex0.tbw), ctx.tex0.cbp, unsigned(ctx.tex0.csa));
            std::ofstream rgb(name, std::ios::binary);
            name[std::strlen(name) - 3] = 'p'; name[std::strlen(name) - 2] = 'g'; name[std::strlen(name) - 1] = 'm';
            std::string alphaName(name);
            alphaName.insert(alphaName.size() - 4, "_a");
            std::ofstream alpha(alphaName, std::ios::binary);
            std::string idxName(name);
            idxName.insert(idxName.size() - 4, "_idx");
            std::ofstream idxOut(idxName, std::ios::binary);
            idxOut << "P5\n" << w << " " << h << "\n255\n";
            rgb << "P6\n" << w << " " << h << "\n255\n";
            alpha << "P5\n" << w << " " << h << "\n255\n";
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t raw = ReadTextureVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, x, y);
                    uint32_t c = raw;
                    if (isFourBitIndexedPsm(ctx.tex0.psm) || isEightBitIndexedPsm(ctx.tex0.psm))
                        c = LookupCLUT(state, static_cast<uint8_t>(raw), ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                    else if (ctx.tex0.psm == GS_PSM_CT16 || ctx.tex0.psm == GS_PSM_CT16S)
                        c = Rgba5551ToRgba8888(raw);
                    rgb.put(static_cast<char>(c & 0xFF)).put(static_cast<char>((c >> 8) & 0xFF)).put(static_cast<char>((c >> 16) & 0xFF));
                    alpha.put(static_cast<char>((c >> 24) & 0xFF));
                    idxOut.put(static_cast<char>(isFourBitIndexedPsm(ctx.tex0.psm) ? (raw & 0xFu) * 17u : (raw & 0xFFu)));
                }
            std::cout << "[gs:cap] tex " << name << " " << w << "x" << h << std::endl;
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_debugTexturedCount{0u}; // TEMP(transformers): also log textured prims
        if (primitiveIndex < 64u || gsCapture ||
            (state.prim.tme && s_debugTexturedCount.fetch_add(1u, std::memory_order_relaxed) < 64u))
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " rawOf=(" << ctx.xyoffset.ofx << "," << ctx.xyoffset.ofy << ")" // TEMP(transformers)
                      << " clamp=0x" << std::hex << ctx.clamp << " tex1=0x" << ctx.tex1 << std::dec << " lin=" << state.linearFilter
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      // TEMP(transformers): full post/shadow state for offline replay.
                      << " fbmsk=0x" << std::hex << ctx.frame.fbmsk
                      << " zbuf=(zbp=" << std::dec << ctx.zbuf.zbp << " psm=0x" << std::hex << unsigned(ctx.zbuf.psm)
                      << " zmsk=" << std::dec << ctx.zbuf.zmask << ")"
                      << " scanmsk=" << state.scanmsk << " iip=" << state.prim.iip
                      << " fba=" << ctx.fba << " pabe=" << state.pabe << " dthe=" << state.dthe
                      << " colclamp=" << state.colclamp << " dimx=0x" << std::hex << state.dimx << std::dec
                      << " texa=(ta0=" << unsigned(state.texa.ta0) << " ta1=" << unsigned(state.texa.ta1)
                      << " aem=" << state.texa.aem << ") cld=" << unsigned(ctx.tex0.cld)
                      << " z=(" << std::setprecision(17) << batch.vertices[0].z << "," << batch.vertices[1].z
                      << "," << batch.vertices[2].z << ")" << std::setprecision(9)
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << " fge=" << static_cast<uint32_t>(state.prim.fge) // TEMP(transformers): fog debugging
                      << " fog=(" << static_cast<uint32_t>(batch.vertices[0].fog) << "," << static_cast<uint32_t>(batch.vertices[1].fog)
                      << "," << static_cast<uint32_t>(batch.vertices[2].fog) << ")"
                      << " fogcol=(" << static_cast<uint32_t>(state.fogR) << "," << static_cast<uint32_t>(state.fogG) << ","
                      << static_cast<uint32_t>(state.fogB) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " rawOf=(" << ctx.xyoffset.ofx << "," << ctx.xyoffset.ofy << ")" // TEMP(transformers)
                      << " clamp=0x" << std::hex << ctx.clamp << " tex1=0x" << ctx.tex1 << std::dec << " lin=" << state.linearFilter
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      // TEMP(transformers): full post/shadow state for offline replay.
                      << " fbmsk=0x" << std::hex << ctx.frame.fbmsk
                      << " zbuf=(zbp=" << std::dec << ctx.zbuf.zbp << " psm=0x" << std::hex << unsigned(ctx.zbuf.psm)
                      << " zmsk=" << std::dec << ctx.zbuf.zmask << ")"
                      << " scanmsk=" << state.scanmsk << " iip=" << state.prim.iip
                      << " fba=" << ctx.fba << " pabe=" << state.pabe << " dthe=" << state.dthe
                      << " colclamp=" << state.colclamp << " dimx=0x" << std::hex << state.dimx << std::dec
                      << " texa=(ta0=" << unsigned(state.texa.ta0) << " ta1=" << unsigned(state.texa.ta1)
                      << " aem=" << state.texa.aem << ") cld=" << unsigned(ctx.tex0.cld)
                      << " z=(" << std::setprecision(17) << batch.vertices[0].z << "," << batch.vertices[1].z
                      << "," << batch.vertices[2].z << ")" << std::setprecision(9)
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        // Z clamped to the ZBUF format maximum (PCSX2 zclamp rule).
        const u32 pz = clampDepthToFormat(v.z, depthFormatMax(ctx.zbuf.psm));
        WritePixel(state, px, py, static_cast<int>(pz), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog, uint8_t maskBits)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;
    // SCANMSK: 2 = don't draw even lines, 3 = don't draw odd lines (0/1 = off). Transformers' level composites its two
    // fields into the display buffer as an interleave (SCANMSK 3 + field 1, SCANMSK 2 + field 2); ignoring it let the
    // field-2 pass overwrite every line.
    if ((state.scanmsk & 2u) != 0u && ((static_cast<uint32_t>(y) ^ static_cast<uint32_t>(state.scanmsk)) & 1u) == 0u)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = maskBits == 0xFFu ? classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm))
        : PixelWriteMask{(maskBits & 1u) != 0u, (maskBits & 2u) != 0u, (maskBits & 4u) != 0u};
    if (!writeMask.writesAnything())
    {
        return;
    }

    // TEST.ZTE=0 means no depth test and no Z write (PCSX2: ZTST ALWAYS + ZMSK), not "never pass".
    const bool zte = ((ctx.test >> 16) & 1u) != 0u;
    const uint32_t ztestMethod = zte ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u;
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        // Unclamped colour after blending/dithering; COLCLAMP decides how it is brought back into 0..255.
        int outR = r;
        int outG = g;
        int outB = b;
        bool needsColclamp = false;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                outR = ((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr);
                outG = ((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg);
                outB = ((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db);
                needsColclamp = true;
            }
        }

        // DTHE: the GS adds DIMX[y&3][x&3] (signed 3 bits) to RGB after blending, before COLCLAMP, and only for 16-bit
        // frame buffers (PCSX2 GSDrawScanline: "sel.fpsm == 2 && sel.dthe"; DIMX layout from GSState::ExpandDIMX).
        // With DIMX never written (0) this is a no-op.
        if ((state.dthe & 1u) != 0u && bitsPerPixel(static_cast<uint8_t>(fpsm)) == 16u && ditherEnabled())
        {
            const uint32_t shift = ((static_cast<uint32_t>(y) & 3u) << 4u) + ((static_cast<uint32_t>(x) & 3u) << 2u);
            int dither = static_cast<int>((state.dimx >> shift) & 7u);
            if ((dither & 4) != 0)
                dither -= 8;
            outR += dither;
            outG += dither;
            outB += dither;
            needsColclamp = true;
        }

        if (needsColclamp)
        {
            // Ported from Scotho/socom-unzipped gs_cpu_backend.cpp:901-910 = ran-j/PS2Recomp PR #230 (GPL-3.0):
            // COLCLAMP.CLAMP=1 saturates, COLCLAMP.CLAMP=0 keeps the low 8 bits (wrap).
            if ((state.colclamp & 0x1u) == 0u && colclampWrapEnabled())
            {
                { // TEMP(transformers): report once that a COLCLAMP=0 wrap actually changed a pixel.
                    static std::atomic<bool> s_wrapLogged{false};
                    if (((outR | outG | outB) & ~0xFF) != 0 && !s_wrapLogged.exchange(true, std::memory_order_relaxed))
                        std::cout << "[gs] COLCLAMP=0 blend wrap first used: rgb=(" << outR << "," << outG << "," << outB
                                  << ") alpha=0x" << std::hex << ctx.alpha << std::dec << " fbp=" << ctx.frame.fbp
                                  << " (PS2X_GS_COLCLAMP_WRAP=0 forces clamping)" << std::endl;
                }
                r = static_cast<uint8_t>(outR);
                g = static_cast<uint8_t>(outG);
                b = static_cast<uint8_t>(outB);
            }
            else
            {
                r = clampU8(outR);
                g = clampU8(outG);
                b = clampU8(outB);
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && zte && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    // Ported from ran-j/PS2Recomp issue #273 (GPL-3.0): bound S/T before any int conversion (NaN/huge -> no UB).
    texUf = clampTexCoord(texUf);
    texVf = clampTexCoord(texVf);

    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-gs-nearest-stq.patch + ps2recomp-gs-bilinear-stq.patch
    // (GPL-3.0): both filters go through 16.16 fixed point first (PCSX2 SW). Truncating toward zero straight to an int
    // picked texel 0 for negative coordinates; floor of the 16.16 value picks -1. Bilinear subtracts half a texel
    // AFTER the 16.16 truncation. Done in integers here (exact; |coord| <= 32766 texels fits int32).
    const int32_t fixedU = static_cast<int32_t>(static_cast<double>(texUf) * 65536.0);
    const int32_t fixedV = static_cast<int32_t>(static_cast<double>(texVf) * 65536.0);

    if (!state.linearFilter)
    {
        return samplePoint(fixedU >> 16, fixedV >> 16);
    }

    const int32_t sampleU = fixedU - 0x8000;
    const int32_t sampleV = fixedV - 0x8000;
    const int u0 = sampleU >> 16;
    const int v0 = sampleV >> 16;
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const uint32_t wx = static_cast<uint32_t>(sampleU >> 12) & 0xFu; // 4-bit GS filter weights
    const uint32_t wy = static_cast<uint32_t>(sampleV >> 12) & 0xFu;

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(c00 & 0xFFu, c10 & 0xFFu, c01 & 0xFFu, c11 & 0xFFu, wx, wy);
    const uint8_t g = lerpChannel((c00 >> 8) & 0xFFu, (c10 >> 8) & 0xFFu, (c01 >> 8) & 0xFFu, (c11 >> 8) & 0xFFu, wx, wy);
    const uint8_t b = lerpChannel((c00 >> 16) & 0xFFu, (c10 >> 16) & 0xFFu, (c01 >> 16) & 0xFFu, (c11 >> 16) & 0xFFu, wx, wy);
    const uint8_t a = lerpChannel((c00 >> 24) & 0xFFu, (c10 >> 24) & 0xFFu, (c01 >> 24) & 0xFFu, (c11 >> 24) & 0xFFu, wx, wy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;
    const auto &alphaMasks = alphaWriteMasks(ctx.test, ctx.frame.psm);

    // XYOFFSET is 12.4 fixed point (keep the fraction), and the GS samples pixels at integer coordinates:
    // pixel x is covered when X0 <= x < X1. Each corner keeps its own texture coordinate, so a sprite sent
    // right-to-left / bottom-to-top must not come out mirrored.
    const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
    float X0 = v0.x - ofx;
    float Y0 = v0.y - ofy;
    float X1 = v1.x - ofx;
    float Y1 = v1.y - ofy;
    // Z clamped to the ZBUF format maximum (PCSX2 zclamp rule); also bounds a garbage float Z before the u32 cast.
    const u32 z1 = clampDepthToFormat(v1.z, depthFormatMax(ctx.zbuf.psm));

    // TEST.ZTE=1 with ZTST=NEVER: WritePixel rejects every pixel, so skip the whole primitive (same result).
    if (((ctx.test >> 16) & 1u) != 0u && ((ctx.test >> 17) & 3u) == 0u)
        return;

    const bool flipX = X0 > X1;
    const bool flipY = Y0 > Y1;
    if (flipX)
        std::swap(X0, X1);
    if (flipY)
        std::swap(Y0, Y1);

    const int unclippedX0 = static_cast<int>(std::ceil(X0));
    const int unclippedY0 = static_cast<int>(std::ceil(Y0));
    const int unclippedX1 = static_cast<int>(std::ceil(X1)) - 1;
    const int unclippedY1 = static_cast<int>(std::ceil(Y1)) - 1;
    if (unclippedX1 < unclippedX0 || unclippedY1 < unclippedY0)
        return;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u) / 16.0f;
            v0f = static_cast<float>(v0.v) / 16.0f;
            u1f = static_cast<float>(v1.u) / 16.0f;
            v1f = static_cast<float>(v1.v) / 16.0f;
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }
        if (flipX)
            std::swap(u0f, u1f);
        if (flipY)
            std::swap(v0f, v1f);

        const float invW = 1.0f / (X1 - X0);
        const float invH = 1.0f / (Y1 - Y0);

        // Ported from ran-j/PS2Recomp PR #246 (via Scotho/socom-unzipped gs_cpu_backend.cpp:1277-1308) (GPL-3.0):
        // early depth rejection before texture sampling. Same Z, buffer, width and comparison as WritePixel, and nothing
        // writes VRAM between this read and WritePixel's own, so the result is unchanged; a failed depth test writes
        // neither colour nor Z in WritePixel whatever TEST.AFAIL says.
        const uint32_t depthMethod = static_cast<uint32_t>((ctx.test >> 17u) & 3u);
        const bool earlyDepth = earlyDepthEnabled() && ((ctx.test >> 16u) & 1u) != 0u && depthMethod >= 2u;
        const uint32_t depthBase = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        const uint32_t depthWidth = std::max<uint32_t>(ctx.frame.fbw, 1u);
        const ReadVramFunc depthReader = m_readVramFuncs[ctx.zbuf.psm & 0x3Fu];

        for (int y = drawY0; y <= drawY1; ++y)
        {
            // SCANMSK rows are dropped by WritePixel; skip them before sampling (same result).
            if ((state.scanmsk & 2u) != 0u && ((static_cast<uint32_t>(y) ^ static_cast<uint32_t>(state.scanmsk)) & 1u) == 0u)
                continue;
            float ty = (static_cast<float>(y) - Y0) * invH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                if (earlyDepth)
                {
                    const uint32_t storedZ = depthReader(m_vram, depthBase, depthWidth, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    if (z1 < storedZ || (depthMethod == 3u && z1 == storedZ))
                        continue;
                }
                float tx = (static_cast<float>(x) - X0) * invW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, static_cast<int>(z1), color.r, color.g, color.b, color.a, v1.fog, alphaMasks[color.a]);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
        {
            // SCANMSK rows are dropped by WritePixel; skip them up front (same result).
            if ((state.scanmsk & 2u) != 0u && ((static_cast<uint32_t>(y) ^ static_cast<uint32_t>(state.scanmsk)) & 1u) == 0u)
                continue;
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, static_cast<int>(z1), r, g, b, a, v1.fog, alphaMasks[a]);
        }
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;
    const auto &alphaMasks = alphaWriteMasks(ctx.test, ctx.frame.psm);

    // XYOFFSET is 12.4 fixed point (keep the fraction); the GS samples pixels at integer coordinates and uses a
    // top-left fill rule. Coordinates are multiples of 1/16, so the edge functions are exact in double precision.
    const double ofx = static_cast<double>(ctx.xyoffset.ofx) / 16.0;
    const double ofy = static_cast<double>(ctx.xyoffset.ofy) / 16.0;

    const double fx0 = v0.x - ofx;
    const double fy0 = v0.y - ofy;
    const double fx1 = v1.x - ofx;
    const double fy1 = v1.y - ofy;
    const double fx2 = v2.x - ofx;
    const double fy2 = v2.y - ofy;

    int minX = static_cast<int>(std::ceil(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::floor(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::ceil(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::floor(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    const double area = (fx1 - fx0) * (fy2 - fy0) - (fy1 - fy0) * (fx2 - fx0);
    if (area == 0.0)
        return;
    const double sgn = area > 0.0 ? 1.0 : -1.0;
    const double invAbsArea = 1.0 / std::fabs(area);
    // Edge p->q, evaluated at s; positive inside after orienting by sgn.
    auto edgeFn = [sgn](double px, double py, double qx, double qy, double sx, double sy)
    { return sgn * ((qx - px) * (sy - py) - (qy - py) * (sx - px)); };
    // Samples exactly on an edge belong to the triangle only for left edges and top (horizontal) edges.
    auto ownsEdge = [sgn](double px, double py, double qx, double qy)
    {
        const double nx = -sgn * (qy - py);
        const double ny = sgn * (qx - px);
        return nx > 0.0 || (nx == 0.0 && ny > 0.0);
    };
    const bool own12 = ownsEdge(fx1, fy1, fx2, fy2);
    const bool own20 = ownsEdge(fx2, fy2, fx0, fy0);
    const bool own01 = ownsEdge(fx0, fy0, fx1, fy1);

    // TEST.ZTE=1 with ZTST=NEVER: WritePixel rejects every pixel, so skip the whole primitive (same result).
    const uint32_t depthMethod = static_cast<uint32_t>((ctx.test >> 17u) & 3u);
    const bool depthTestEnabled = ((ctx.test >> 16u) & 1u) != 0u;
    if (depthTestEnabled && depthMethod == 0u)
        return;
    // Ported from ran-j/PS2Recomp PR #246 (via Scotho/socom-unzipped gs_cpu_backend.cpp:1277-1308) (GPL-3.0):
    // early depth rejection before colour, texture and fog work. Same Z, buffer, width and comparison as WritePixel,
    // and nothing writes VRAM between this read and WritePixel's own, so the result is unchanged; a failed depth test
    // writes neither colour nor Z in WritePixel whatever TEST.AFAIL says.
    const bool earlyDepth = earlyDepthEnabled() && depthTestEnabled && depthMethod >= 2u;
    const uint32_t depthBase = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const uint32_t depthWidth = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const ReadVramFunc depthReader = m_readVramFuncs[ctx.zbuf.psm & 0x3Fu];
    const uint32_t depthMax = depthFormatMax(ctx.zbuf.psm);

    for (int y = minY; y <= maxY; ++y)
    {
        // SCANMSK rows are dropped by WritePixel; skip them before any per-pixel work (same result).
        if ((state.scanmsk & 2u) != 0u && ((static_cast<uint32_t>(y) ^ static_cast<uint32_t>(state.scanmsk)) & 1u) == 0u)
            continue;
        const double py = static_cast<double>(y);
        for (int x = minX; x <= maxX; ++x)
        {
            const double px = static_cast<double>(x);

            const double e12 = edgeFn(fx1, fy1, fx2, fy2, px, py); // weight of v0
            const double e20 = edgeFn(fx2, fy2, fx0, fy0, px, py); // weight of v1
            const double e01 = edgeFn(fx0, fy0, fx1, fy1, px, py); // weight of v2
            if (e12 < 0.0 || e20 < 0.0 || e01 < 0.0)
                continue;
            if ((e12 == 0.0 && !own12) || (e20 == 0.0 && !own20) || (e01 == 0.0 && !own01))
                continue;

            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-gs-triangle-constants.patch,
            // -triangle-texcoords.patch and -triangle-sampling.patch (flat Z) (GPL-3.0): interpolate every attribute as
            // v0 + (v1 - v0) * w1 + (v2 - v0) * w2 with double weights, so a constant attribute stays exact even when
            // the rounded weights do not sum to 1 (alpha 128 no longer drops to 127 and fails ATST GEQUAL 128).
            const double w1 = e20 * invAbsArea;
            const double w2 = e01 * invAbsArea;

            const double z = v0.z + (v1.z - v0.z) * w1 + (v2.z - v0.z) * w2;
            // Z clamped to the ZBUF format maximum (PCSX2 zclamp rule); also bounds a garbage Z before the u32 cast.
            const uint32_t pixelZ = clampDepthToFormat(z + 0.5, depthMax);

            if (earlyDepth)
            {
                const uint32_t storedZ = depthReader(m_vram, depthBase, depthWidth, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                if (pixelZ < storedZ || (depthMethod == 3u && pixelZ == storedZ))
                    continue;
            }

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * w1 + (v2.r - v0.r) * w2));
                g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * w1 + (v2.g - v0.g) * w2));
                b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * w1 + (v2.b - v0.b) * w2));
                a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * w1 + (v2.a - v0.a) * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    // Signed differences admit descending UV edges; the bound keeps the cast defined.
                    iu = static_cast<uint16_t>(clampInt(static_cast<int>(v0.u + (v1.u - v0.u) * w1 + (v2.u - v0.u) * w2), 0, 0xFFFF));
                    iv = static_cast<uint16_t>(clampInt(static_cast<int>(v0.v + (v1.v - v0.v) * w1 + (v2.v - v0.v) * w2), 0, 0xFFFF));
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = static_cast<float>(v0.s + (static_cast<double>(v1.s) - v0.s) * w1 + (static_cast<double>(v2.s) - v0.s) * w2);
                    it = static_cast<float>(v0.t + (static_cast<double>(v1.t) - v0.t) * w1 + (static_cast<double>(v2.t) - v0.t) * w2);
                    iq = static_cast<float>(v0.q + (static_cast<double>(v1.q) - v0.q) * w1 + (static_cast<double>(v2.q) - v0.q) * w2);
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * w1 + (v2.fog - v0.fog) * w2));
            WritePixel(state, x, y, static_cast<int>(pixelZ), r, g, b, a, fog, alphaMasks[a]);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        // Z clamped to the ZBUF format maximum (PCSX2 zclamp rule); also bounds a garbage Z before the u32 cast.
        WritePixel(state, x0, y0, static_cast<int>(clampDepthToFormat(z, depthFormatMax(ctx.zbuf.psm))), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    { // TEMP(transformers): trace transfers and how much of the previous one arrived
        static std::atomic<uint32_t> s_xferLog{0u}; // TEMP(transformers)
        const uint32_t xdbp = command.bitbltbuf.dbp;
        static const bool s_xferDuringCapture = std::getenv("PS2X_GSCAPTURE_VS") != nullptr;
        const uint32_t logIndex = s_xferLog.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_xferDir1Log{0u}; // TEMP(transformers): local->host readbacks drain the GS worker
        if ((command.direction == 1u && s_xferDir1Log.fetch_add(1u, std::memory_order_relaxed) < 2000u) ||
            logIndex < 400u || (xdbp >= 12000u && xdbp < 16000u && logIndex + 1u < 4000u) ||
            (s_xferDuringCapture && gsTimeline::tick(ps2dbg::vsyncTick.load(std::memory_order_relaxed)) >= 17400u &&
             gsTimeline::tick(ps2dbg::vsyncTick.load(std::memory_order_relaxed)) < 17700u))
            std::cout << "[gs:xfer] prev " << m_transferState.copiedPixels << "/" << m_transferState.totalPixels
                      << " | new dir=" << command.direction << " dbp=" << command.bitbltbuf.dbp
                      << " dbw=" << unsigned(command.bitbltbuf.dbw) << " dpsm=0x" << std::hex << unsigned(command.bitbltbuf.dpsm)
                      << std::dec << " sbp=" << command.bitbltbuf.sbp << " spsm=0x" << std::hex << unsigned(command.bitbltbuf.spsm) << std::dec
                      << " rect=" << command.trxreg.rrw << "x" << command.trxreg.rrh
                      << " @" << command.trxpos.dsax << "," << command.trxpos.dsay
                      << " src@" << command.trxpos.ssax << "," << command.trxpos.ssay << " sbw=" << unsigned(command.bitbltbuf.sbw)
                      << " trxdir=" << unsigned(command.trxpos.dir) << " vs=" << gsTimeline::tick(ps2dbg::vsyncTick.load()) << std::endl;
    }
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;
    static const bool s_uploadDumps = std::getenv("PS2X_TEX_DEBUG_DUMPS") != nullptr; // TEMP(transformers): was ~600 MB/run, 25% of EE time
    if (s_uploadDumps && (m_transfer.bitbltbuf.dbp == 14532u || m_transfer.bitbltbuf.dbp == 14531u || m_transfer.bitbltbuf.dbp == 14275u ||
        m_transfer.bitbltbuf.dbp == 13703u || m_transfer.bitbltbuf.dbp == 13699u || m_transfer.bitbltbuf.dbp == 15636u)) // TEMP(transformers)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "upload_%u.bin", m_transfer.bitbltbuf.dbp);
        std::ofstream(name, std::ios::binary | std::ios::app).write(reinterpret_cast<const char *>(data), sizeBytes);
    }

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    auto uploadRows = [&]<uint32_t Bpp, auto Write>()
    {
        const uint64_t available = Bpp == 4u ? uint64_t(sizeBytes) * 2u : sizeBytes / (Bpp / 8u);
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(available, m_transferState.totalPixels - m_transferState.copiedPixels));
        if (count == 0u) return;
        const uint32_t initialPixel = m_transferState.copiedPixels;
        uint32_t consumed = 0u;
        uint32_t x = m_transferState.x, y = m_transferState.y;
        while (consumed < count)
        {
            const uint32_t rowCount = std::min(count - consumed, rrw - (x - dsax));
            GSMem::WriteUploadRow(dpsm, m_vram, dbp, dbw, x, y, data, consumed, rowCount);
            consumed += rowCount;
            x = dsax;
            ++y;
        }
        m_transferState.copiedPixels += count;
        if (m_transferState.copiedPixels >= m_transferState.totalPixels)
        {
            const uint32_t lastPixel = initialPixel + (Bpp == 4u ? ((count - 1u) & ~1u) : count - 1u);
            m_transferState.x = dsax + lastPixel % rrw;
            m_transferState.y = m_transfer.trxpos.dsay + lastPixel / rrw;
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
        }
        else
        {
            m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
            m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
        }
    };
    switch (dpsm)
    {
    case GS_PSM_CT32: uploadRows.operator()<32u, GSMem::WriteCT32>(); break;
    case GS_PSM_Z32: uploadRows.operator()<32u, GSMem::WriteZ32>(); break;
    case GS_PSM_CT24: uploadRows.operator()<24u, GSMem::WriteCT24>(); break;
    case GS_PSM_Z24: uploadRows.operator()<24u, GSMem::WriteZ24>(); break;
    case GS_PSM_CT16: uploadRows.operator()<16u, GSMem::WriteCT16>(); break;
    case GS_PSM_CT16S: uploadRows.operator()<16u, GSMem::WriteCT16S>(); break;
    case GS_PSM_Z16: uploadRows.operator()<16u, GSMem::WriteZ16>(); break;
    case GS_PSM_Z16S: uploadRows.operator()<16u, GSMem::WriteZ16S>(); break;
    case GS_PSM_T8: uploadRows.operator()<8u, GSMem::WriteP8>(); break;
    case GS_PSM_T8H: uploadRows.operator()<8u, GSMem::WriteP8H>(); break;
    case GS_PSM_T4: uploadRows.operator()<4u, GSMem::WriteP4>(); break;
    case GS_PSM_T4HL: uploadRows.operator()<4u, GSMem::WriteP4HL>(); break;
    case GS_PSM_T4HH: uploadRows.operator()<4u, GSMem::WriteP4HH>(); break;
    default: return;
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = Rgba8888ToRgba5551(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    // GAME(transformers) / generic PCRTC: interlaced FRAME mode reads half-height fields (loading / warp-gate screens).
    // PS2X_PCRTC_FFMD=0 restores the old behaviour (DH+1 buffer rows) for an A/B.
    static const bool s_honourFfmd = []()
    {
        const char *value = std::getenv("PS2X_PCRTC_FFMD");
        return !(value && value[0] == '0');
    }();
    const bool interlacedFrameMode = s_honourFfmd && smode2.interlaced && smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    logPcrtcSetup(request); // TEMP(transformers)
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    // `height` = output lines of the circuit; in interlaced FRAME mode only `readHeight` buffer rows are scanned and
    // then line-doubled to `height` (expandInterlacedFrameField).
    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint64_t display64,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        const uint32_t readHeight = interlacedFrameMode ? interlacedFrameSourceRows(display64, height) : height;
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, readHeight, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, readHeight, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && hasNonBlackPixels(pixels, width, readHeight) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, readHeight, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (hasNonBlackPixels(candidatePixels, width, readHeight) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        if (interlacedFrameMode)
            expandInterlacedFrameField(pixels, width, readHeight, height, display64, oddField);
        return true;
    };

    // Both CRTs sampling the same image have identical RGB. Blending an
    // image with itself is independent of PMODE's factor and alpha selection;
    // host alpha is normalized below. Keep the existing source/fallback policy.
    if (valid1 && valid2 && !pmode.slbg && request.dispfb1 == request.dispfb2 &&
        width1 == width2 && height1 == height2)
    {
        GSFrameReg selected{};
        bool preferred = false;
        if (copySource(displayFrame1, origin1, request.display1, width1, height1, false, true, selected, result.pixels, preferred))
        {
            result.width = width1;
            result.height = height1;
            normalizePresentationAlpha(result.pixels, width1, height1);
            if (fieldMode) applyFieldPresentation(result.pixels, width1, height1, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected.fbp;
            return result;
        }
    }

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, request.display1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, request.display2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            if (interlacedFrameMode) // TEMP(transformers): [present:pcrtc] per-circuit content, bounded
            {
                static std::atomic<uint32_t> s_statCalls{0u};
                const uint32_t call = s_statCalls.fetch_add(1u, std::memory_order_relaxed);
                if ((call % 16u) == 0u && call < 16u * 80u)
                {
                    const PcrtcCircuitStats s1 = pcrtcCircuitStats(crt1, width1, height1);
                    const PcrtcCircuitStats s2 = pcrtcCircuitStats(crt2, width2, height2);
                    std::printf("[present:pcrtc] ffmd tick=%llu odd=%d c1 FBP%u %ux%u rgb(%.1f,%.1f,%.1f) a=%.1f | "
                                "c2 FBP%u %ux%u rgb(%.1f,%.1f,%.1f) a=%.1f\n",
                                static_cast<unsigned long long>(request.vsyncTick), oddField ? 1 : 0,
                                selected1.fbp, width1, height1, s1.r, s1.g, s1.b, s1.a,
                                selected2.fbp, width2, height2, s2.r, s2.g, s2.b, s2.a);
                    std::fflush(stdout);
                }
            }
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, valid1 ? request.display1 : request.display2, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}

// TEMP(transformers): same-build x64 format; never serialize mutexes/pointers.
std::vector<uint8_t> GSCpuBackend::RecordState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint8_t> out;
    auto add = [&](const auto &v) { const auto *p = reinterpret_cast<const uint8_t *>(&v); out.insert(out.end(), p, p + sizeof(v)); };
    add(m_clut); add(m_clutCbp); add(m_texturePageCache);
    add(m_transfer); add(m_transferState); add(m_localToHostReadPos);
    out.insert(out.end(), m_localToHostBuffer.begin(), m_localToHostBuffer.end());
    return out;
}
void GSCpuBackend::ReplayState(const std::vector<uint8_t> &in)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t pos = 0;
    auto get = [&](auto &v) { if (sizeof(v) > in.size() - pos) throw std::runtime_error("short GS state"); std::memcpy(&v, in.data() + pos, sizeof(v)); pos += sizeof(v); };
    get(m_clut); get(m_clutCbp); get(m_texturePageCache);
    get(m_transfer); get(m_transferState); get(m_localToHostReadPos);
    m_localToHostBuffer.assign(in.begin() + pos, in.end());
    if (m_localToHostReadPos > m_localToHostBuffer.size()) throw std::runtime_error("invalid GS readback cursor");
}

// TEMP(transformers): portable hidden-state prefix; fixed little-endian Windows x64 producer.
std::vector<uint8_t> GSCpuBackend::RawPortableState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);std::vector<uint8_t> out;
    auto add=[&](const auto&v){const auto*p=reinterpret_cast<const uint8_t*>(&v);out.insert(out.end(),p,p+sizeof(v));};
    for(auto v:m_clut)add(v);for(auto v:m_clutCbp)add(v);
    add(m_transferState.x);add(m_transferState.y);add(m_transferState.totalPixels);add(m_transferState.copiedPixels);add(m_transferState.direction);
    const auto&b=m_transfer.bitbltbuf;const auto&p=m_transfer.trxpos;const auto&r=m_transfer.trxreg;
    add(uint64_t(b.sbp)|(uint64_t(b.sbw)<<16)|(uint64_t(b.spsm)<<24)|(uint64_t(b.dbp)<<32)|(uint64_t(b.dbw)<<48)|(uint64_t(b.dpsm)<<56));
    add(uint64_t(p.ssax)|(uint64_t(p.ssay)<<16)|(uint64_t(p.dsax)<<32)|(uint64_t(p.dsay)<<48)|(uint64_t(p.dir)<<59));
    add(uint64_t(r.rrw)|(uint64_t(r.rrh)<<32));
    add(uint64_t(m_localToHostReadPos));add(uint64_t(m_localToHostBuffer.size()));
    out.insert(out.end(),m_localToHostBuffer.begin(),m_localToHostBuffer.end());return out;
}
