// TEMP(transformers): human playtest helpers - F12 problem marks with a screenshot, F11 fps/vsync overlay, a periodic
// [playtest] stats line in stderr. See ps2_playtest.h and PLAYTEST_README.md (workspace root).
#include "ps2_playtest.h"
#include "ps2_perfscope.h" // TEMP(transformers): qualification (flip timing, F11 top-3 buckets)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

int ps2PadActiveGamepad(); // ps2_pad.cpp
std::string ps2PadMarkInfo(); // ps2_pad.cpp, TEMP(transformers): qualification - pad bytes + game walk/run for F12 marks
void ps2PadSetTestOverlay(bool on);               // ps2_pad.cpp, TEMP(transformers): qualification F7 PAD TEST
bool ps2PadTestOverlayOn();                        // ps2_pad.cpp
void ps2PadDrawTestOverlay(int fontSize, int top); // ps2_pad.cpp
namespace ps2_perfscope { uint64_t vu1MidFallbacks(); uint64_t vu1DeclinedCalls(); } // TEMP(transformers) qualification (release fix): ps2_perfscope.cpp

// TEMP(transformers): exact native width of the consumed PGS presentation frame.
float ps2PgsNativeFrameWidth(float);
namespace ps2_playtest
{
    namespace
    {
        std::atomic<uint64_t> g_flips{0};

        std::string wallClock()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            return buf;
        }

        // TEMP(transformers) qualification: + pads/s (game frames = the unit of every scripted benchmark; flips/s is the
        // DISPFB flip rate, ~0.8x pads/s in play) and host frames/s counted here (raylib GetFPS() averages only over the
        // calls made to it, so read once per 30 s it printed 900/1800 - VERDICT release fix).
        struct Rate
        {
            std::chrono::steady_clock::time_point t0{};
            uint64_t vs0 = 0, flips0 = 0, pads0 = 0, host0 = 0;
            float vsPerSec = 0.0f, flipsPerSec = 0.0f, padsPerSec = 0.0f, hostPerSec = 0.0f;
            bool init = false;
            void update(uint64_t vs, uint64_t flips, uint64_t pads, uint64_t host, float window)
            {
                const auto now = std::chrono::steady_clock::now();
                if (!init)
                {
                    t0 = now;
                    vs0 = vs;
                    flips0 = flips;
                    pads0 = pads;
                    host0 = host;
                    init = true;
                    return;
                }
                const float dt = std::chrono::duration<float>(now - t0).count();
                if (dt < window)
                    return;
                vsPerSec = static_cast<float>(vs - vs0) / dt;
                flipsPerSec = static_cast<float>(flips - flips0) / dt;
                padsPerSec = static_cast<float>(pads - pads0) / dt;
                hostPerSec = static_cast<float>(host - host0) / dt;
                t0 = now;
                vs0 = vs;
                flips0 = flips;
                pads0 = pads;
                host0 = host;
            }
        };

        struct State
        {
            bool overlay = false;
            int marks = 0;
            Rate fast;   // overlay (0.5 s window)
            // TEMP(transformers) qualification: the stats line used a separate 30 s Rate whose window was not aligned with
            // the line's own 30 s timer, so each line printed the rate of an older window (playtest #1: "vs/s=76.2" on a
            // line whose own interval ran at 6.5 vs/s). The line now measures exactly the interval since the last line.
            uint64_t lineVs = 0, lineFlips = 0, linePads = 0, lineHost = 0;
            uint64_t hostFrames = 0; // TEMP(transformers) qualification: frame() calls = presented host frames
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point lastStats = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point toastUntil{};
            std::string toast;
            bool announced = false;
        } g;

        bool scripted()
        {
            static const bool s = []()
            {
                const char *a = std::getenv("PS2X_AUTOPRESS");
                const char *b = std::getenv("PS2X_SCRIPT");
                return (a && *a) || (b && *b);
            }();
            return s;
        }

        void writeMark(const Texture &frameTex, uint32_t w, uint32_t h, uint64_t vs, uint64_t padReads,
                       DescribeFn describe, void *user)
        {
            const int n = ++g.marks;
            const std::string &dir = outputDir();
            char name[96];
            std::snprintf(name, sizeof(name), "mark_%03d_vs%llu.png", n, static_cast<unsigned long long>(vs));
            const std::string path = (std::filesystem::path(dir) / name).string();

            bool saved = false;
            if (frameTex.id != 0 && w > 0 && h > 0)
            {
                Image img = LoadImageFromTexture(frameTex);
                if (img.data)
                {
                    if (static_cast<int>(w) < img.width || static_cast<int>(h) < img.height)
                        ImageCrop(&img, Rectangle{0.0f, 0.0f, static_cast<float>(std::min<int>(w, img.width)),
                                                  static_cast<float>(std::min<int>(h, img.height))});
                    // The presenter's texture can carry the GS alpha channel; save opaque so viewers do not show holes.
                    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
                    saved = ExportImage(img, path.c_str());
                    // TEMP(transformers): keep a native-size companion for existing mark-analysis tools.
                    const uint32_t nativeW=uint32_t(ps2PgsNativeFrameWidth(float(w)));
                    if(nativeW && nativeW<w) {
                        const auto nativeH=uint32_t((uint64_t(h)*nativeW+w-1)/w);
                        ImageResize(&img,int(nativeW),int(nativeH));
                        ExportImage(img,(path.substr(0,path.size()-4)+"_native.png").c_str());
                    }
                    UnloadImage(img);
                }
            }
            const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - g.start).count();
            const std::string state = describe ? describe(user) : std::string();
            const std::string padInfo = ps2PadMarkInfo(); // qualification: what the game read from the pad at this mark
            char line[768];
            std::snprintf(line, sizeof(line),
                          "[MARK %d] wall=%s t=%.1fs vs=%llu pads=%llu vs/s=%.1f gamefps(pads/s)=%.1f flips/s=%.1f gamepad=%d %s %s screenshot=%s size=%ux%u",
                          n, wallClock().c_str(), elapsed, static_cast<unsigned long long>(vs),
                          static_cast<unsigned long long>(padReads), g.fast.vsPerSec, g.fast.padsPerSec, g.fast.flipsPerSec,
                          ps2PadActiveGamepad(), padInfo.c_str(), state.c_str(), saved ? name : "FAILED",w,h);
            std::cerr << line << std::endl;
            std::cout << line << std::endl; // both logs carry the marker, so either can be searched for [MARK
            {
                std::ofstream marks(std::filesystem::path(dir) / "marks.txt", std::ios::app);
                if (marks)
                    marks << line << "\n";
            }
            g.toast = "MARK " + std::to_string(n) + (saved ? " saved" : " logged (screenshot failed)");
            g.toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    }

    const std::string &outputDir()
    {
        static const std::string dir = []()
        {
            for (const char *var : {"PS2X_PLAYTEST_DIR", "PS2X_DUMP_DIR"})
            {
                if (const char *v = std::getenv(var); v && *v)
                {
                    std::error_code ec;
                    std::filesystem::create_directories(v, ec);
                    return std::string(v);
                }
            }
            return std::string(".");
        }();
        return dir;
    }

    void noteFlip()
    {
        g_flips.fetch_add(1u, std::memory_order_relaxed);
        ps2_perfscope::noteFlip(); // TEMP(transformers) qualification
    }

    void frame(const Texture &frameTex, uint32_t presentWidth, uint32_t presentHeight, uint64_t vsyncTick,
               uint64_t padReads, DescribeFn describe, void *user)
    {
        const uint64_t flips = g_flips.load(std::memory_order_relaxed);
        ++g.hostFrames;
        g.fast.update(vsyncTick, flips, padReads, g.hostFrames, 0.5f);

        if (!g.announced)
        {
            g.announced = true;
            (void)ps2_perfscope::sinceLast(0); // TEMP(transformers) qualification: start the [playtest] line's window
            if (!scripted())
                std::cerr << "[playtest] F12 = flag a problem (mark + screenshot), F11 = fps overlay, F7 = pad test; output dir "
                          << outputDir() << std::endl;
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - g.lastStats).count() >= 30.0f)
        {
            const float lineSecs = std::chrono::duration<float>(now - g.lastStats).count();
            const float lineVsPerSec = static_cast<float>(vsyncTick - g.lineVs) / lineSecs;
            const float lineFlipsPerSec = static_cast<float>(flips - g.lineFlips) / lineSecs;
            // TEMP(transformers) qualification (VERDICT release fix item 3): game fps = pads/s (scripted benchmark unit),
            // flips/s kept under its own name, host fps counted over the same interval, and the EE-side load per vsync.
            const float linePadsPerSec = static_cast<float>(padReads - g.linePads) / lineSecs;
            const float lineHostPerSec = static_cast<float>(g.hostFrames - g.lineHost) / lineSecs;
            g.lastStats = now;
            g.lineVs = vsyncTick;
            g.lineFlips = flips;
            g.linePads = padReads;
            g.lineHost = g.hostFrames;
            const ps2_perfscope::Counters pc = ps2_perfscope::sinceLast(0);
            // TEMP(transformers) qualification (release fix): vu1fb = VU1 calls that started in the AOT and fell back mid-call.
            static uint64_t s_lastDecl=0; // TEMP(transformers)
            const uint64_t decl=ps2_perfscope::vu1DeclinedCalls(),declDelta=decl-s_lastDecl;s_lastDecl=decl;
            static uint64_t s_lastMidFb = 0;
            const uint64_t midFb = ps2_perfscope::vu1MidFallbacks();
            const uint64_t midFbDelta = midFb - s_lastMidFb;
            s_lastMidFb = midFb;
            char load[200] = " gif/vs=- vu1/vs=- vu1interp/vs=- vu1fb/vs=- vu1decl/vs=- eebusy=-"; // PS2X_PERFSCOPE=0
            if (pc.valid)
                std::snprintf(load, sizeof(load), " gif/vs=%.0f vu1/vs=%.0f vu1interp/vs=%.1f vu1fb/vs=%.1f vu1decl/vs=%.1f eebusy=%.0f%% eecpu=%.0f%%",
                              pc.perVs(pc.gif), pc.perVs(pc.vu1), pc.perVs(pc.vu1Interp), pc.perVs(midFbDelta), pc.perVs(declDelta), pc.eeBusyPct,
                              pc.eeCpuPct);
            std::fprintf(stderr,
                         "[playtest] wall=%s t=%.0fs vs=%llu vs/s=%.1f gamefps(pads/s)=%.1f flips/s=%.1f hostfps=%.0f pads=%llu%s\n",
                         wallClock().c_str(), std::chrono::duration<float>(now - g.start).count(),
                         static_cast<unsigned long long>(vsyncTick), lineVsPerSec, linePadsPerSec, lineFlipsPerSec,
                         lineHostPerSec, static_cast<unsigned long long>(padReads), load);
        }

        if (IsKeyPressed(KEY_F11))
            g.overlay = !g.overlay;
        if (IsKeyPressed(KEY_F7)) // TEMP(transformers): qualification - PAD TEST overlay
            ps2PadSetTestOverlay(!ps2PadTestOverlayOn());
        bool mark = IsKeyPressed(KEY_F12);
        { // TEMP(transformers): PS2X_PLAYTEST_AUTOMARK_VS=<vs>[,<vs>...] presses F12 at those guest vsyncs (unattended check)
            static std::vector<uint64_t> s_auto = []()
            {
                std::vector<uint64_t> out;
                for (const char *p = std::getenv("PS2X_PLAYTEST_AUTOMARK_VS"); p && *p;)
                {
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    out.push_back(v);
                    p = (*end == ',') ? end + 1 : end;
                }
                return out;
            }();
            if (!s_auto.empty() && vsyncTick >= s_auto.front())
            {
                s_auto.erase(s_auto.begin());
                mark = true;
            }
        }
        if (mark)
            writeMark(frameTex, presentWidth, presentHeight, vsyncTick, padReads, describe, user);

        const int fontSize = std::max(14, GetScreenHeight() / 45);
        int overlayExtra = 0; // TEMP(transformers) qualification: height of the second overlay line
        if (g.overlay)
        {
            char text[200];
            // TEMP(transformers) qualification: "game fps" is now pads/s (the benchmark unit); flips/s shown separately.
            std::snprintf(text, sizeof(text), "game %.1f fps (pads/s) | flips %.1f/s | vsync %.1f/s (%.0f%%) | host %.0f fps | vs %llu",
                          g.fast.padsPerSec, g.fast.flipsPerSec, g.fast.vsPerSec, g.fast.vsPerSec * 100.0f / 59.94f,
                          g.fast.hostPerSec, static_cast<unsigned long long>(vsyncTick));
            const int tw = MeasureText(text, fontSize);
            DrawRectangle(6, 6, tw + 12, fontSize + 8, Color{0, 0, 0, 160});
            DrawText(text, 12, 10, fontSize, Color{120, 255, 120, 255});
            // TEMP(transformers) qualification: where the EE thread's time went in the last second (top 3 buckets).
            const std::string perf = ps2_perfscope::overlayText();
            if (!perf.empty())
            {
                const int y2 = fontSize + 20;
                const int tw2 = MeasureText(perf.c_str(), fontSize);
                DrawRectangle(6, y2 - 4, tw2 + 12, fontSize + 8, Color{0, 0, 0, 160});
                DrawText(perf.c_str(), 12, y2, fontSize, Color{150, 210, 255, 255});
                overlayExtra = fontSize + 14;
            }
        }
        if (now < g.toastUntil)
        {
            const int y = (g.overlay ? fontSize + 20 : 6) + overlayExtra;
            const int tw = MeasureText(g.toast.c_str(), fontSize);
            DrawRectangle(6, y, tw + 12, fontSize + 8, Color{0, 0, 0, 160});
            DrawText(g.toast.c_str(), 12, y + 4, fontSize, Color{255, 220, 80, 255});
        }
        ps2PadDrawTestOverlay(fontSize, (g.overlay ? fontSize + 20 : 6) + overlayExtra + (now < g.toastUntil ? fontSize + 14 : 0));
    }
}
