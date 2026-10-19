#include "../ps2_present_trace.h" // TEMP(transformers)
#ifdef PS2X_WITH_PGS
#include "../ps2_present_gpu.h"
#endif
#include "../ps2_audio_movie_queue.h"
// TEMP(transformers): M1 GPU GS backend (parallel-gs, LGPL-3.0+, port/deps/parallel-gs @ cc6184a + our patch
// port/pgs_m1_parallel_gs.patch). Selected with PS2X_GS_BACKEND=PGS in a -DPS2X_WITH_PGS=ON build; the default build
// and the default runtime stay on GSCpuBackend. Design: private qualification evidence section 4; API usage follows the
// proven M0.2 spike (private qualification evidence, RESULTS.md). Notes: private qualification evidence
//
// Data flow: GS (gs_frontend.cpp) forwards every raw GIF packet with its path id (RawGIF), direct A+D writes (RawAD)
// and, at guest VBlankStart, the privileged register block (RawVsync). GSThreadedBackend runs all of it on its worker,
// which owns the Vulkan device and the GSInterface (all Granite calls happen on that one thread, thread index 0).
// The frontend keeps decoding the stream for EE-visible state (CSR SIGNAL/FINISH/LABEL, transfer bookkeeping), but no
// decoded draws reach this backend unless PS2X_GS_SHADOW=1 (then a private GSCpuBackend replays them for diffing).
// Presentation: vsync() -> PCRTC scanout image -> vkCmdCopyImageToBuffer into a host buffer (fence polled on the
// worker) -> mailbox -> render thread (Present) -> glTexSubImage2D in ps2_runtime.cpp UploadFrame. No GL interop.
#include "runtime/gs/gs_pgs_backend.h"
#include "pdiet_pixels.h"
#include "../ps2_perfscope.h"
#include "runtime/ps2_resolution.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/ps2_settings.h"
#include <sstream>
#include "runtime/gs/gs_cpu_backend.h"
#include <algorithm>
#include <array>
#include <atomic>
#include "../ps2_audio_present_clock.h" // TEMP(transformers)
#include "../ps2_audio_video_trace.h" // TEMP(transformers)
#include "../ps2_audio_picture_clock.h" // TEMP(transformers)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// TEMP(transformers) qualification I1a: PS2X_FIELD60 field-split 60 fps presentation (private qualification evidence, design
// private qualification evidence 6.1a). The game draws every 3D object twice per 30 Hz tick: field 1 (t = N) and
// field 2 (t = N - 0.5, the engine's own motion interpolation), and the SCANMSK composite weaves them into the 448-line
// display buffer (field 2 on one row parity, field 1 on the other), then bloom + HUD are drawn on top at full height.
// On a CRT the interlaced scanout showed those two row sets on consecutive vsyncs = 60 Hz motion. FIELD60 presents the
// two row sets of each new scanout as two progressive frames, older field first, half a tick apart (host-paced), with
// the missing rows reconstructed (PS2X_FIELD60_FILTER). Host-side only: the guest stream, VRAM and readbacks are not
// touched (mode 2 / PS2X_FIELD60_PRECAP additionally read FBP0 at the composite point, a GPU wait on the GS worker).
//   PS2X_FIELD60=1            split the final scanout (HUD/bloom included on both frames)
//   PS2X_FIELD60=2            present the pre-composite FBP0 fields instead (diagnostic: no HUD, no bloom)
//   PS2X_FIELD60_FILTER=      double | bob | adaptive | median (default adaptive: static pixels keep the woven row,
//                             moving pixels bob; median = static weave else median3 - sharper, but pulls the older frame
//                             toward the newer one: Optimus at 0.56/0.57 of the step instead of 0.48/0.52 in the hub turn)
//   PS2X_FIELD60_PARITY=0|1   scanout row parity that holds the OLDER field (default 0 = even rows, I0 finding)
//   PS2X_FIELD60_ADAPT_T=n    adaptive threshold per channel (default 6)
//   PS2X_FIELD60_PRECAP=a:b   guest vsync window: capture FBP0 at the composite, dump f60pre_*.ppm, log parity check
//   PS2X_FIELD60_LOG=file     TSV of every ingested scanout (I) and presented frame (P) with host times (pacing)
//   PS2X_FIELD60_WEAVE_VS=n   a new scanout is only split when the SCANMSK field composite was drawn within the last n
//                             guest vsyncs (default 8); otherwise it is progressive content (FMV, 2D screens) and is shown
//                             whole, once (kind P) - splitting it would only halve its vertical resolution and make
//                             horizontal edges shimmer by half a line at 60 Hz. 0 = always split (prototype behaviour).
//   PS2X_FIELD60_NODROP=1     diagnostic: never drop a built frame (queue up to 128), so PS2X_FIELD60_DUMP sequences
//                             are complete even when PNG export makes the render thread slow (pacing is then wrong)
bool ps2Field60Enabled();
void ps2Field60LastPresented(uint64_t &seq, uint64_t &tick, int &kind);
// gs_frontend.cpp GS::rawGuestVsync stores the guest vsync tick of the VBlankStart here (a reserved, never-read
// offset of the privileged block copy) so the worker knows which guest vsync a scanout belongs to.
constexpr size_t kField60TickOffset = 0x0F00u;
#ifdef PS2X_WITH_PGS
#include "gs_interface.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"
#include "logging.hpp" // TEMP(transformers) qualification: Util::LoggingInterface (stall counter)
#include <condition_variable>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <thread>
#include "runtime/gs/ps2_gs_psmct32.h"

namespace
{
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kFrameStride = 640u; // GSCpuBackend kHostFrameWidth: GS::copyLatchedHostPresentationFrame reads this stride
    constexpr uint32_t kFrameMaxHeight = 512u;

    uint32_t envU32(const char *name, uint32_t fallback)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : fallback;
    }

    bool envOff(const char *name)
    {
        const char *v = std::getenv(name);
        return v && v[0] == '0';
    }

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

// ---- TEMP(transformers) qualification: persistent Vulkan pipeline cache for parallel-gs (private qualification evidence) ----
// parallel-gs compiles every ubershader/binning/upload variant at GSInterface::init() on background threads, and any
// variant it did not pre-compile (or that is not finished yet) is compiled SYNCHRONOUSLY on this worker at its first draw
// ("[WARN]: Stalled compile"). Granite only persists a VkPipelineCache through its filesystem layer
// (GRANITE_VULKAN_SYSTEM_HANDLES), which the standalone parallel-gs build turns off, so until now every pipeline came from
// the driver's own disk cache or a full compile. The driver caches are keyed per executable (a new bin_* folder = a cold
// driver cache): the first run of each new build waited the full PS2X_PGS_WARMUP_MS (20 s) at boot and then took ~45
// stalls of 10-780 ms in its first minutes. This keeps our own VkPipelineCache in a per-user folder shared by every build
// (the driver validates each entry by its shader hash; Granite validates the file by the device's pipelineCacheUUID + a
// content hash, so a new build or a new driver simply misses or starts fresh):
//   PS2X_PGS_PIPECACHE=1         default on; 0 = no VkPipelineCache at all (= the old behaviour, for A/B)
//   PS2X_PGS_PIPECACHE_DIR=dir   folder (default %LOCALAPPDATA%\PS2Recomp\pipecache; $XDG_CACHE_HOME or ~/.cache on Linux)
//   PS2X_PGS_PIPECACHE_SAVE_SEC  how often the saver thread checks whether the cache grew (default 20 s; it also saves
//                                2 s after a completed boot warm-up and at shutdown). Saving never runs on the GS worker.
//   PS2X_PGS_PIPECACHE_MAX_MB    a file larger than this is ignored and rebuilt (default 512)
// File: pgs_pipeline_<vendor>_<device>_<driver>.bin (one per GPU + driver), written as <file>.tmp<n> then renamed.
// The worker's "Stalled compile" warnings are also counted (count / total / worst): "[gs:pgs] pipecache stalls=" lines.
namespace pipecache
{
    struct StallLog final : Util::LoggingInterface
    {
        std::atomic<uint32_t> stalls{0};
        std::atomic<uint64_t> totalUs{0}, maxUs{0};
        bool log(const char *tag, const char *fmt, va_list va) override
        {
            char buf[1024];
            std::vsnprintf(buf, sizeof(buf), fmt, va);
            if (std::strncmp(buf, "Stalled compile", 15) == 0)
            {
                const char *p = std::strstr(buf, " - ");
                const uint64_t us = p ? uint64_t(std::strtod(p + 3, nullptr)) : 0u;
                stalls.fetch_add(1u, std::memory_order_relaxed);
                totalUs.fetch_add(us, std::memory_order_relaxed);
                uint64_t m = maxUs.load(std::memory_order_relaxed);
                while (us > m && !maxUs.compare_exchange_weak(m, us, std::memory_order_relaxed))
                {
                }
            }
            std::fputs(tag, stderr); // same output as Granite's own fallback
            std::fputs(buf, stderr);
            std::fflush(stderr);
            return true;
        }
    };

    StallLog &stallLog()
    {
        static StallLog *s = new StallLog(); // leaked on purpose: a thread_local pointer to it may outlive statics
        return *s;
    }

    std::filesystem::path cacheDir()
    {
#ifdef _WIN32
        if (const wchar_t *e = _wgetenv(L"PS2X_PGS_PIPECACHE_DIR"); e && *e)
            return std::filesystem::path(e);
        if (const wchar_t *e = _wgetenv(L"LOCALAPPDATA"); e && *e)
            return std::filesystem::path(e) / L"PS2Recomp" / L"pipecache";
#else
        if (const char *e = std::getenv("PS2X_PGS_PIPECACHE_DIR"); e && *e)
            return std::filesystem::path(e);
        if (const char *e = std::getenv("XDG_CACHE_HOME"); e && *e)
            return std::filesystem::path(e) / "PS2Recomp" / "pipecache";
        if (const char *e = std::getenv("HOME"); e && *e)
            return std::filesystem::path(e) / ".cache" / "PS2Recomp" / "pipecache";
#endif
        return std::filesystem::path("pipecache");
    }

    class Cache
    {
    public:
        // GS worker, right after Device::set_context() and before GSInterface::init() (the init-time compile tasks
        // capture the device's VkPipelineCache handle when they are recorded).
        void Load(Vulkan::Device &dev)
        {
            Util::set_thread_logging_interface(&stallLog()); // counts this worker's sync-compile stalls
            enabled = !envOff("PS2X_PGS_PIPECACHE");
            if (!enabled)
            {
                std::cout << "[gs:pgs] pipecache off (PS2X_PGS_PIPECACHE=0): no VkPipelineCache" << std::endl;
                return;
            }
            const auto tl = std::chrono::steady_clock::now();
            const VkPhysicalDeviceProperties &gp = dev.get_gpu_properties();
            char name[96];
            std::snprintf(name, sizeof(name), "pgs_pipeline_%04x_%04x_%08x.bin", gp.vendorID, gp.deviceID, gp.driverVersion);
            std::error_code ec;
            const std::filesystem::path dir = cacheDir();
            std::filesystem::create_directories(dir, ec);
            path = dir / name;
            const uint64_t maxBytes = uint64_t(envU32("PS2X_PGS_PIPECACHE_MAX_MB", 512u)) << 20;
            std::vector<uint8_t> blob;
            const char *what = "no file yet, fresh cache";
            {
                std::ifstream in(path, std::ios::binary | std::ios::ate);
                if (in)
                {
                    const std::streamoff n = in.tellg();
                    if (n > 0 && uint64_t(n) <= maxBytes)
                    {
                        blob.resize(size_t(n));
                        in.seekg(0);
                        if (in.read(reinterpret_cast<char *>(blob.data()), n))
                            what = "loaded";
                        else
                        {
                            blob.clear();
                            what = "read failed, fresh cache";
                        }
                    }
                    else if (n > 0)
                        what = "file over PS2X_PGS_PIPECACHE_MAX_MB, fresh cache";
                }
            }
            const double readMs = msSince(tl);
            // Granite checks pipelineCacheUUID + its content hash and falls back to an empty cache by itself
            // ("[INFO]: Initializing pipeline cache." / "Pipeline cache UUID changed." / "[WARN]: ... corrupt").
            bool ok = dev.init_pipeline_cache(blob.empty() ? nullptr : blob.data(), blob.size());
            if (!ok && !blob.empty()) // the driver refused the blob: start empty instead of running without a cache
            {
                what = "driver refused the file, fresh cache";
                blob.clear();
                ok = dev.init_pipeline_cache(nullptr, 0);
            }
            savedBytes = blob.size();
            if (!ok)
            {
                enabled = false;
                std::cout << "[gs:pgs] pipecache: vkCreatePipelineCache failed - running without a cache" << std::endl;
                return;
            }
            std::cout << "[gs:pgs] pipecache " << what << ": " << path.string() << " " << blob.size() / 1024u
                      << " KB, read " << readMs << " ms, create " << (msSince(tl) - readMs) << " ms" << std::endl;
        }

        // GS worker, after the boot warm-up wait.
        void Start(Vulkan::Device &dev, bool warmupComplete)
        {
            if (!enabled)
                return;
            device = &dev;
            saveEverySec = std::max<uint32_t>(2u, envU32("PS2X_PGS_PIPECACHE_SAVE_SEC", 20u));
            t0 = std::chrono::steady_clock::now();
            const uint32_t first = warmupComplete ? 2u : saveEverySec;
            saver = std::thread([this, first]() { SaverLoop(first); });
        }

        // Backend teardown (after device.wait_idle()): stop the saver and write what is new.
        void Stop()
        {
            if (saver.joinable())
            {
                {
                    std::lock_guard<std::mutex> l(m);
                    stopping = true;
                }
                cv.notify_all();
                saver.join();
            }
            if (enabled && device)
                SaveIfGrown("shutdown");
            PrintStalls(true, "exit");
            device = nullptr;
        }

    private:
        void SaverLoop(uint32_t firstDelaySec)
        {
            uint32_t delay = firstDelaySec;
            for (;;)
            {
                std::unique_lock<std::mutex> l(m);
                if (cv.wait_for(l, std::chrono::seconds(delay), [this]() { return stopping; }))
                    return;
                l.unlock();
                SaveIfGrown("periodic");
                PrintStalls(false, "periodic");
                delay = saveEverySec;
            }
        }

        void PrintStalls(bool always, const char *when)
        {
            StallLog &s = stallLog();
            const uint32_t n = s.stalls.load(std::memory_order_relaxed);
            if (n == printedStalls && !always)
                return;
            printedStalls = n;
            std::cout << "[gs:pgs] pipecache stalls=" << n << " totalMs=" << s.totalUs.load() / 1000.0
                      << " maxMs=" << s.maxUs.load() / 1000.0 << " saves=" << saves << " fileKB=" << savedBytes / 1024u
                      << " (" << when << ", t=" << int(msSince(t0) / 1000.0) << "s)" << std::endl;
        }

        // Saver thread / teardown: vkGetPipelineCacheData needs no external synchronisation (a VkPipelineCache is
        // internally synchronised), and Granite's two helpers only touch the device table + the cache handle.
        void SaveIfGrown(const char *why)
        {
            const auto ts = std::chrono::steady_clock::now();
            size_t size = device->get_pipeline_cache_size();
            const double sizeMs = msSince(ts);
            if (size == 0 || size == savedBytes)
                return;
            std::vector<uint8_t> data;
            bool got = false;
            for (int attempt = 0; attempt < 3 && !got; ++attempt) // VK_INCOMPLETE if it grew between the two calls
            {
                data.resize(size);
                got = device->get_pipeline_cache_data(data.data(), data.size());
                if (!got)
                    size = device->get_pipeline_cache_size();
            }
            if (!got)
            {
                std::cout << "[gs:pgs] pipecache: vkGetPipelineCacheData failed (" << why << ")" << std::endl;
                return;
            }
            const double getMs = msSince(ts) - sizeMs;
            std::filesystem::path tmp = path;
            tmp += ".tmp" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) % 1000000007ull);
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size())))
                {
                    std::cout << "[gs:pgs] pipecache: write failed: " << tmp.string() << std::endl;
                    return;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec); // replaces the old file (MoveFileExW REPLACE_EXISTING on Windows)
            if (ec)
            {
                std::cout << "[gs:pgs] pipecache: rename failed (" << ec.message() << ")" << std::endl;
                std::filesystem::remove(tmp, ec); // our own temp file (absolute path)
                return;
            }
            ++saves;
            std::cout << "[gs:pgs] pipecache saved (" << why << ", t=" << int(msSince(t0) / 1000.0) << "s): "
                      << savedBytes / 1024u << " -> " << data.size() / 1024u << " KB, size query " << sizeMs
                      << " ms, data " << getMs << " ms, total " << msSince(ts) << " ms" << std::endl;
            savedBytes = data.size();
        }

        bool enabled = false;
        std::filesystem::path path;
        Vulkan::Device *device = nullptr;
        size_t savedBytes = 0;
        uint32_t saveEverySec = 20, saves = 0, printedStalls = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::thread saver;
        std::mutex m;
        std::condition_variable cv;
        bool stopping = false;
    };

    Cache &cache()
    {
        // One GS backend per process. Leaked on purpose: if the process exits without destroying the backend, a
        // joinable std::thread member would call std::terminate during static destruction; this way the saver simply
        // dies with the process (an interrupted save leaves only a .tmp file, the real file is replaced by rename).
        static Cache *c = new Cache();
        return *c;
    }
} // namespace pipecache

    // ---- TEMP(transformers) qualification I1a: PS2X_FIELD60 configuration + render-thread side channel -------------
    struct Field60Config
    {
        int mode = 0;   // 0 off, 1 split scanout, 2 pre-composite fields
        int filter = 2; // 0 double, 1 bob, 2 adaptive (static weave + bob), 3 median (static weave + median3)
        int olderParity = 0;
        int adaptT = 6;
        uint32_t weaveVs = 8; // PS2X_FIELD60_WEAVE_VS
        bool noDrop = false;  // PS2X_FIELD60_NODROP=1: diagnostic, present every built frame in order (complete dumps)
        uint64_t precapLo = 1, precapHi = 0;
        std::string dir;
        FILE *log = nullptr;
        std::mutex logMutex;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    };

    std::pair<uint64_t, uint64_t> envWindow(const char *name)
    {
        const char *e = std::getenv(name);
        if (!e || !*e)
            return {1, 0};
        char *n = nullptr;
        const uint64_t lo = std::strtoull(e, &n, 10);
        return {lo, (n && *n == ':') ? std::strtoull(n + 1, nullptr, 10) : lo};
    }

    Field60Config &field60Cfg()
    {
        static Field60Config *s_cfg = []()
        {
            auto *c = new Field60Config(); // leaked on purpose (worker may log during static teardown)
            const char *backend = std::getenv("PS2X_GS_BACKEND");
            const bool pgs = backend && (std::strcmp(backend, "PGS") == 0 || std::strcmp(backend, "pgs") == 0);
            const uint32_t rate = envU32("PS2X_PRESENT_RATE", 0u); // TEMP(transformers): 60/30 overrides legacy switches
            const uint32_t m = rate == 30 ? 0u : rate == 60 ? 1u : envU32("PS2X_FIELD60", 0u);
            c->mode = (pgs && m <= 2u) ? int(m) : 0;
            if (const char *f = std::getenv("PS2X_FIELD60_FILTER"))
                c->filter = (std::strcmp(f, "double") == 0 || f[0] == '0')   ? 0
                            : (std::strcmp(f, "bob") == 0 || f[0] == '1')    ? 1
                            : (std::strcmp(f, "median") == 0 || f[0] == '3') ? 3
                                                                             : 2;
            c->olderParity = envU32("PS2X_FIELD60_PARITY", 0u) ? 1 : 0;
            c->adaptT = int(envU32("PS2X_FIELD60_ADAPT_T", 6u));
            c->weaveVs = envU32("PS2X_FIELD60_WEAVE_VS", 8u);
            c->noDrop = envU32("PS2X_FIELD60_NODROP", 0u) != 0u;
            const auto w = envWindow("PS2X_FIELD60_PRECAP");
            c->precapLo = w.first;
            c->precapHi = w.second;
            if (const char *d = std::getenv("PS2X_DUMP_DIR"))
                c->dir = d;
            if (c->mode)
            {
                if (const char *l = std::getenv("PS2X_FIELD60_LOG"))
                    if (*l)
                        c->log = std::fopen(l, "w");
                // TEMP(transformers): origin permits joining FIELD60 with WAV and MPEG clocks.
                if (c->log && std::getenv("PS2X_AUD1B_SYNC"))
                    std::fprintf(stderr, "[aud1b:field60-origin] steady_us=%lld\n",
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>(c->t0.time_since_epoch()).count());
                if (c->log)
                    std::fprintf(c->log, "ev\thost_us\ttick\tfield\tkind\tseq\tgap\tdue_us\tlate_us\tema_us\tqueue\tdropped\n");
                std::cout << "[field60] mode=" << c->mode << " filter=" << (c->filter == 0 ? "double" : c->filter == 1 ? "bob" : c->filter == 2 ? "adaptive" : "median")
                          << " olderParity=" << c->olderParity << " adaptT=" << c->adaptT << " weaveVs=" << c->weaveVs << " noDrop=" << c->noDrop << " precap=" << c->precapLo << ":" << c->precapHi
                          << " log=" << (c->log ? "on" : "off") << std::endl;
            }
            if(ps2PresentTrace::enabled()) ps2PresentTrace::event(ps2PresentTrace::Origin,0,0,
                uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(c->t0.time_since_epoch()).count())); // TEMP(transformers)
            return c;
        }();
        return *s_cfg;
    }

    int64_t field60Us(std::chrono::steady_clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(t - field60Cfg().t0).count();
    }

    std::atomic<uint64_t> g_field60Seq{0}, g_field60Tick{0};
    std::atomic<int> g_field60Kind{0};
    // GAME(transformers): classification of the image actually returned by Present(), not the worker's newest frame.
    std::atomic<int> g_wsPresentedScene{-1};
    // TEMP(transformers): presentation-only sharp image; native GS mailbox ABI stays unchanged.
    std::mutex g_sharpMutex;
    std::vector<uint8_t> g_sharpPixels;
    uint32_t g_sharpWidth=0, g_sharpHeight=0;
    bool g_sharpReady=false; // TEMP(transformers): retain vector size without re-serving old pixels
    // TEMP(transformers): native descriptor backing survives SharpTake. Publish/consume run on render thread;
    // g_sharpMutex also protects explicit GS native-copy callers. No pointer escapes this lock.
    std::vector<uint8_t> g_lazyNativeSource;
    uint32_t g_lazyWidth=0,g_lazyHeight=0;
    std::atomic<uint32_t> g_nativePresentedWidth{0};

    void field60WritePpm(const std::string &name, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t strideBytes)
    {
        const Field60Config &c = field60Cfg();
        const std::string path = c.dir.empty() ? name : c.dir + "/" + name;
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(size_t(w) * 3u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *in = rgba + size_t(y) * strideBytes;
            for (uint32_t x = 0; x < w; ++x)
            {
                row[x * 3u + 0] = in[x * 4u + 0];
                row[x * 3u + 1] = in[x * 4u + 1];
                row[x * 3u + 2] = in[x * 4u + 2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
}

bool ps2Field60Enabled()
{
    return field60Cfg().mode != 0;
}

// TEMP(transformers): narrow render-only opt-out; ordinary GS copy callers always materialize.
thread_local bool g_pdietSkipNative=false;
bool ps2PgsSetSkipNative(bool skip) {const bool old=g_pdietSkipNative;g_pdietSkipNative=skip;return old;}
bool ps2PgsLazyNativeCopy(std::vector<uint8_t>& out,uint32_t w,uint32_t h) {
    std::lock_guard<std::mutex> l(g_sharpMutex);
    if(!pdiet::lazy() || w!=g_lazyWidth/2 || h!=(g_lazyHeight+1)/2 || g_lazyNativeSource.empty())return false;
    out.clear();if(g_pdietSkipNative)return true;
    out.resize(size_t(w)*h*4);
    const size_t stride=size_t(std::max(kFrameStride,g_lazyWidth))*4;
    for(uint32_t y=0;y<h;++y)pdiet::downRow(g_lazyNativeSource.data()+size_t(y*2)*stride,
        g_lazyNativeSource.data()+size_t(std::min(y*2+1,g_lazyHeight-1))*stride,out.data()+size_t(y)*w*4,w);
    return true;
}
// TEMP(transformers): called only after consuming the corresponding native presentation latch.
bool ps2PgsSharpTake(std::vector<uint8_t>& pixels,uint32_t& w,uint32_t& h) {
    std::lock_guard<std::mutex> lock(g_sharpMutex);
    if(pdiet::rn()) {
        if(!g_sharpReady){w=h=0;pixels.clear();return false;}
        w=g_sharpWidth;h=g_sharpHeight;pixels.swap(g_sharpPixels);g_sharpReady=false;
    } else {w=g_sharpWidth;h=g_sharpHeight;pixels.swap(g_sharpPixels);g_sharpPixels.clear();}
    return !pixels.empty();
}
float ps2PgsNativeFrameWidth(float fallback) {
    const auto w=g_nativePresentedWidth.load(std::memory_order_acquire);
    return w ? float(w) : fallback;
}
int ps2PgsPresentedScene()
{
    return g_wsPresentedScene.load(std::memory_order_acquire);
}

// Render thread: identity of the field-60 frame handed out by the last successful Present() (UploadFrame dumps).
// kind: 0 = older field (A), 1 = newer field (B), 2 = progressive (no split), 3 = pre-composite A, 4 = pre-composite B.
void ps2Field60LastPresented(uint64_t &seq, uint64_t &tick, int &kind)
{
    seq = g_field60Seq.load(std::memory_order_acquire);
    tick = g_field60Tick.load(std::memory_order_relaxed);
    kind = g_field60Kind.load(std::memory_order_relaxed);
}

static std::atomic<const uint8_t*> resoptVram{nullptr};
class GSPgsBackend final : public GSRasterBackend
{
public:
    GSPgsBackend()
    {
        // Read here, not in Initialize: GSThreadedBackend caches WantsDecodedRaster() at construction.
        const char *shadow = std::getenv("PS2X_GS_SHADOW");
        shadowEnabled = shadow && shadow[0] == '1';
    }
    ~GSPgsBackend() override
    {
        resoptVram.store(nullptr,std::memory_order_release);
        if (!initialized)
            return;
        try
        {
            ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
            gs->flush();
            device.wait_idle();
            pipecache::cache().Stop(); // TEMP(transformers) lane pipecache: final save
        }
        catch (...)
        {
        }
        if(traceGpu) { traceGpu->poll(true);traceGpu.reset(); } // TEMP(transformers): existing idle precedes release
        pending.clear();
        readbackSlots.clear();
        gs.reset(); // before the device (members are destroyed after this body)
    }

    bool UsesRawGIF() const override { return true; }
    bool WantsDecodedRaster() const override { return shadowEnabled; }

    void Initialize(uint8_t *vram, uint32_t size) override
    {
        if (!vram || size == 0u)
            return; // hand-off without storage: nothing to mirror
        if (size < kVramBytes)
            throw std::runtime_error("PGS requires 4 MiB GS VRAM");
        if (!initialized)
            InitDevice();

        if(resoptLive)resoptVram.store(vram,std::memory_order_release);
        readVram.assign(kVramBytes, 0u);
        reader.Initialize(readVram.data(), kVramBytes);
        if (shadowEnabled)
        {
            shadowVram.assign(vram, vram + kVramBytes);
            shadowCpu.Initialize(shadowVram.data(), kVramBytes);
            shadowEvery = std::max<uint32_t>(1u, envU32("PS2X_GS_SHADOW_EVERY", 30u));
            std::cout << "[gs:pgs] shadow CPU backend enabled, compare every " << shadowEvery << " vsyncs" << std::endl;
        }

        // Mirror the hand-off VRAM (zeros at boot).
        void *p = gs->map_vram_write(0, kVramBytes);
        if (!p)
            throw std::runtime_error("PGS VRAM upload failed");
        std::memcpy(p, vram, kVramBytes);
        gs->end_vram_write(0, kVramBytes);
        gs->write_register(ParallelGS::RegisterAddr::TEXFLUSH, uint64_t(0));
        ResetRegisterState();
        gs->flush();
        Invalidate();
    }

    void Reset() override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        if (!initialized)
            return; // GS::GS() resets before init()
        gs->flush();
        device.wait_idle();
        PollFrames(false);
        gs->write_register(ParallelGS::RegisterAddr::TRXDIR, uint64_t(3));
        gs->reset_context_state();
        ResetRegisterState();
        if (shadowEnabled)
            shadowCpu.Reset();
        transfer = {};
        fifoOffset = fifoSize = 0;
        {
            std::lock_guard<std::mutex> lock(mailboxMutex);
            mailbox = {};
            mailboxNext = {}; // TEMP(transformers) qualification
            f60Queue.clear(); // TEMP(transformers) qualification I1a
        }
        f60Prev.clear();
        f60Caps.clear();
        f60CompositeSeen = false;
        Invalidate();
    }

    // ---- raw stream (worker) -------------------------------------------------------------------------------------
    void RawGIF(uint32_t path, const uint8_t *data, uint32_t size) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        if (!initialized || !data)
            return;
        if (path < 1u || path > 3u)
            path = 3u;
        size &= ~15u; // spike: parallel-gs takes whole qwords
        if (!size)
            return;
        // GAME(transformers): this call and RawVsync execute in GS stream order, including CMSAR1 vortex packets.
        if (path == 1u)
            wsLastPath1Tick = f60CurTick + 1u;
        // Spike problem 1: a packet may end with a bare IMAGE tag (no data) and the next packet repeats tag + data.
        // Our frontend treats every packet independently; parallel-gs keeps path state across gif_transfer calls and
        // would eat the repeated tag as image data. Reset a path left mid-tag (pgs_replay.cpp PgsReplayer::gif).
        ParallelGS::GIFPath &gp = gs->get_gif_path(path);
        if (gp.loop != gp.tag.NLOOP)
        {
            gp = {};
            ++statCarries;
        }
        if (f60Scan)
        {
            // TEMP(transformers) qualification I1a: capture FBP0 (both fields) right before the SCANMSK composite's first
            // GIF tag. Splitting a packet at a GIF tag boundary is the same stream for parallel-gs.
            uint32_t tbw = 0;
            const long split = F60ScanForComposite(data, size, tbw);
            if (split >= 0)
            {
                f60PrecapArmed = false;
                if (split > 0)
                    gs->gif_transfer(path, data, uint32_t(split));
                F60CaptureFields(tbw);
                data += split;
                size -= uint32_t(split);
                if (!size)
                {
                    ++statPackets;
                    Invalidate();
                    return;
                }
            }
        }
        gs->gif_transfer(path, data, size);
        ++statPackets;
        statBytes += size;
        Invalidate();
        if ((statPackets & 511u) == 0u)
            PollFrames(false);
    }

    void RawAD(uint8_t reg, uint64_t value) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        if (!initialized || reg >= 128u)
            return;
        const bool f60Comp = f60Scan && F60Track(reg, value); // TEMP(transformers) lane interp I1a
        if (f60Comp)
            F60NoteComposite();
        const bool f60Hit = f60Comp && f60PrecapArmed;
        gs->write_register(static_cast<ParallelGS::RegisterAddr>(reg), value);
        if (f60Hit)
        {
            f60PrecapArmed = false;
            F60CaptureFields(uint32_t((f60Tex0[f60HitCtx] >> 14) & 0x3Fu));
        }
        Invalidate();
    }

    void RawVsync(const std::array<uint8_t, 8192> &privIn) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        ps2_perfscope::Scope dietScope(ps2_perfscope::GsVsync); // TEMP(transformers)
        if (!initialized)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        ++statVsyncs;
        PollFrames(false);

        // TEMP(transformers) qualification I1a: guest vsync tick stashed by GS::rawGuestVsync in a reserved offset;
        // cleared in the copy handed to parallel-gs.
        std::array<uint8_t, 8192> priv = privIn;
        uint64_t guestTick = 0;
        std::memcpy(&guestTick, priv.data() + kField60TickOffset, sizeof(guestTick));
        std::memset(priv.data() + kField60TickOffset, 0, sizeof(guestTick));
        f60CurTick = guestTick;
        if(traceGpu)traceGpu->advance(guestTick); // TEMP(transformers)
        if (resoptLive) ResoptBoundary(guestTick);
        {
            const Field60Config &fc = field60Cfg();
            f60PrecapArmed = fc.mode == 2 || (guestTick + 2u >= fc.precapLo && guestTick <= fc.precapHi + 2u);
        }

        auto &p = gs->get_priv_register_state();
        std::memcpy(p.qwords_lo, priv.data(), 4096);
        std::memcpy(p.qwords_hi, priv.data() + 4096, 4096);
        // TEMP(transformers) qualification: PS2X_LOAD_SKIP hold tag (GS::rawGuestVsync) - run this vsync, publish no scanout.
        uint64_t holdTag = 0;
        std::memcpy(&holdTag, priv.data() + 0x0FF8, sizeof(holdTag));
        const bool holdScanout = holdTag == 0x504C4F4144484C44ull;
        // TEMP(transformers): raw register heartbeat every guest vsync, including disabled/held scanouts.
        if(ps2PresentTrace::enabled()) {
            uint64_t pmode,fb1,fb2,d1,d2;
            std::memcpy(&pmode,priv.data(),8);std::memcpy(&fb1,priv.data()+0x70,8);
            std::memcpy(&fb2,priv.data()+0x90,8);std::memcpy(&d1,priv.data()+0x80,8);std::memcpy(&d2,priv.data()+0xa0,8);
            ps2PresentTrace::event(ps2PresentTrace::Display,guestTick,0,pmode,fb1,fb2,holdScanout);
            ps2PresentTrace::event(ps2PresentTrace::Display,guestTick,1,d1,d2);
        }
        if (holdScanout)
            p.qwords_lo[0x0FF8 / 8] = 0;
        // Safety net (spike problem 3): our libgs HLE now programs SMODE1/SYNCV (sceGsResetGraph / GsSetCrt), but if a
        // path leaves SMODE1 unknown parallel-gs returns no scanout ("Unknown video format"). Substitute NTSC analog.
        if (!CrtModeKnown(p) && (p.pmode.EN1 || p.pmode.EN2))
        {
            if (statCrtFallback++ == 0u)
                std::cout << "[gs:pgs] SMODE1=0x" << std::hex << p.qwords_lo[2] << std::dec
                          << " is not a known video mode; scanning out as NTSC (CMOD=2 LC=32)" << std::endl;
            p.smode1.CMOD = ParallelGS::SMODE1Bits::CMOD_NTSC;
            p.smode1.LC = ParallelGS::SMODE1Bits::LC_ANALOG;
        }

        if (shadowEnabled && (statVsyncs % shadowEvery) == 0u)
            CompareShadow();

        ParallelGS::VSyncInfo info = {};
        info.phase = static_cast<uint32_t>(p.csr.FIELD);
        info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
        info.adapt_to_internal_horizontal_resolution = true;
        info.crtc_offsets = true;
        info.force_progressive = true;
        info.anti_blur = true;
        // TEMP(transformers): renderer rejects unsupported modes itself (EXTWRITE, one-axis grids).
        const auto sharp=ps2x::settings::get("renderer.pgs_hires_scanout");
        const int samples=ps2x::resolution::mailbox().status().samples;
        info.high_resolution_scanout = sharp=="on" || (sharp=="auto" && samples>=4);
        { ps2PresentTrace::Scope trace(ps2PresentTrace::ScanoutFlush,guestTick); gs->flush(); } // TEMP(transformers)
        ParallelGS::ScanoutResult result;
        { ps2PresentTrace::Scope trace(ps2PresentTrace::Scanout,guestTick); result = gs->vsync(info); } // TEMP(transformers)
        { ps2PresentTrace::Scope trace(ps2PresentTrace::ScanoutSubmit,guestTick); gs->flush(); } // TEMP(transformers)
        ps2PresentTrace::event(ps2PresentTrace::NullScanout,guestTick,0,!result.image,holdScanout,
            info.high_resolution_scanout,samples); // TEMP(transformers)

        if (result.image && !holdScanout)
            QueueScanoutCopy(*result.image, p.pmode.EN1 ? uint32_t(p.dispfb1.FBP) : uint32_t(p.dispfb2.FBP), guestTick,
                             uint32_t(p.csr.FIELD), result.high_resolution_scanout);
        else if (!result.image)
            ++statEmptyScanouts;
        Invalidate();
        statVsyncMs += msSince(t0);
        MaybePrintStats();
    }

    // ---- decoded stream (only for the shadow backend / EE bookkeeping) -------------------------------------------
    void Submit(const GSPrimitiveBatch &b) override
    {
        if (shadowEnabled)
            shadowCpu.Submit(b);
    }
    void LoadClut(const GSTex0Reg &t, const GSTexClutReg &c) override
    {
        if (shadowEnabled)
            shadowCpu.LoadClut(t, c);
    }
    void BeginTransfer(const GSTransferCommand &c) override
    {
        // The raw TRXDIR write already reached parallel-gs (RawGIF/RawAD are queued before the decoded call).
        if (shadowEnabled)
            shadowCpu.BeginTransfer(c);
        fifoOffset = fifoSize = 0;
        transfer = {};
        transfer.direction = c.direction;
        transfer.x = c.trxpos.dsax;
        transfer.y = c.trxpos.dsay;
        transfer.totalPixels = uint32_t(c.trxreg.rrw) * uint32_t(c.trxreg.rrh);
        if (c.direction == 1u)
        {
            transfer.x = c.trxpos.ssax;
            transfer.y = c.trxpos.ssay;
            const uint32_t psm = c.bitbltbuf.spsm;
            const uint32_t bits = (psm == 0x14u || psm == 0x24u || psm == 0x2cu)                     ? 4u
                                  : (psm == 0x13u || psm == 0x1bu)                                    ? 8u
                                  : (psm == 0x02u || psm == 0x0au || psm == 0x32u || psm == 0x3au)    ? 16u
                                  : (psm == 0x01u || psm == 0x31u)                                    ? 24u
                                                                                                      : 32u;
            // Same rounding as parallel-gs init_transfer (whole bytes; the FIFO serves whole qwords).
            transfer.localToHostPendingBytes = (size_t(transfer.totalPixels) * bits) / 8u;
            ++statReadbacks;
        }
    }
    void UploadImage(const uint8_t *p, uint32_t n) override
    {
        if (shadowEnabled)
            shadowCpu.UploadImage(p, n);
    }
    void Flush() override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        if (initialized)
            gs->flush();
    }
    void TextureFlush() override
    {
        // TEXFLUSH is in the raw stream for parallel-gs; the shadow CPU backend invalidates its texture page cache here
        // (missing it made the shadow sample stale textures: large false diffs in pgs_m1f).
        if (shadowEnabled)
            shadowCpu.TextureFlush();
    }
    void Sync(GSSyncReason reason) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        // Flush() already submitted for Finish; Presentation/DebugReadback/Reset submit pending work.
        if (initialized && reason != GSSyncReason::Finish)
            gs->flush();
    }
    PresentationFrame Present(const GSPresentationRequest &) override
    {
        ps2PresentTrace::Scope traceSelection(ps2PresentTrace::Select); // TEMP(transformers): includes mailbox lock acquisition
        // Render thread (GSThreadedBackend::Present bypasses the queue for raw backends): mailbox only, no Vulkan.
        std::lock_guard<std::mutex> lock(mailboxMutex);
        if (f60Mode)
            return F60Present(); // TEMP(transformers) qualification I1a: paced field queue instead of the mailbox
        PresentationFrame f = std::move(mailbox);
        const bool sharp=mailboxSharp;
        if (f)
            g_wsPresentedScene.store(mailboxScene ? 1 : 0, std::memory_order_release);
        mailbox = std::move(mailboxNext); // TEMP(transformers) qualification: empty unless PS2X_PRESENT_ALLFRAMES=1
        mailboxScene = mailboxNextScene;
        mailboxSharp = mailboxNextSharp;
        mailboxNext = {};
        return PublishSharp(std::move(f),sharp);
    }
    bool ClearFramebuffer(const GSContext &c, uint32_t v) override
    {
        // libgs HLE also emits the GIF clear sprite (Kernel/Stubs/GS.cpp applyGsClearPacket), which is in the stream.
        if (shadowEnabled)
            shadowCpu.ClearFramebuffer(c, v);
        return true;
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t n) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        // parallel-gs serves the FIFO in 16-byte units; the runtime API is byte-granular (spike: PgsReplayer::readback).
        if (!dst || !initialized)
            return 0;
        n = static_cast<uint32_t>(std::min<size_t>(n, transfer.localToHostPendingBytes));
        uint32_t done = 0;
        while (done < n)
        {
            if (fifoOffset == fifoSize)
            {
                gs->read_transfer_fifo(fifo, 1);
                fifoOffset = 0;
                fifoSize = 16;
            }
            const uint32_t count = std::min(n - done, fifoSize - fifoOffset);
            std::memcpy(dst + done, fifo + fifoOffset, count);
            fifoOffset += count;
            done += count;
        }
        transfer.localToHostPendingBytes -= done;
        statReadbackBytes += done;
        if (shadowEnabled && done)
        {
            // TEMP(transformers): PS2X_GS_SHADOW=1 - the shadow CPU backend saw the same TRXDIR=1; compare the bytes the
            // EE receives (sun-occlusion readback = 16 KiB per game frame).
            std::vector<uint8_t> ref(done);
            const uint32_t got = shadowCpu.ConsumeLocalToHostBytes(ref.data(), done);
            uint32_t mism = got == done ? 0u : done;
            for (uint32_t i = 0; i < got && i < done; ++i)
                mism += ref[i] != dst[i];
            ++statShadowReadbacks;
            if (mism)
            {
                ++statShadowReadbackMismatch;
                if (statShadowReadbackMismatch <= 8u)
                    std::cout << "[gs:shadow] readback mismatch: " << mism << " of " << done << " bytes (cpu returned " << got
                              << ")" << std::endl;
            }
        }
        if (!transfer.localToHostPendingBytes)
            transfer.copiedPixels = transfer.totalPixels;
        return done;
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        auto *self = const_cast<GSPgsBackend *>(this);
        self->Refresh();
        return reader.ReadVram(psm, base, bw, x, y);
    }
    bool ReadRectAsync(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       std::function<void(std::vector<uint8_t>)> complete) override
    {
        // PS2X_STOREIMAGE_LAG=1 (Kernel/Stubs/GS.cpp sceGsExecStoreImage): runs on the worker in stream order, the EE
        // does not wait. map_vram_read on just the rect's pages is the spike's proven readback path (readbacks
        // byte-exact); it waits for the GPU up to this stream position on the GS thread only.
        if (!initialized || psm != 0u || !w || !h || !complete)
            return false;
        uint32_t lo = kVramBytes, hi = 0;
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx)
            {
                const uint32_t addr = GSPSMCT32::addrPSMCT32(base, bw, x + xx, y + yy) & (kVramBytes - 1u);
                lo = std::min(lo, addr);
                hi = std::max(hi, addr + 4u);
            }
        const auto t0 = std::chrono::steady_clock::now();
        const uint8_t* mapped;
        { ps2PresentTrace::Scope trace(ps2PresentTrace::GuestReadback); // TEMP(transformers)
          ps2PresentTrace::GpuBind mapBind(device,traceGpu); mapped = static_cast<const uint8_t *>(gs->map_vram_read(lo, hi - lo)); } // TEMP(transformers)
        if (!mapped)
            return false;
        std::vector<uint8_t> bytes(size_t(w) * h * 4u);
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx)
            {
                const uint32_t addr = GSPSMCT32::addrPSMCT32(base, bw, x + xx, y + yy) & (kVramBytes - 1u);
                std::memcpy(bytes.data() + (size_t(yy) * w + xx) * 4u, mapped + (addr - lo), 4u);
            }
        statLagReadbacks++;
        statLagReadbackMs += msSince(t0);
        complete(std::move(bytes));
        return true;
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        ps2PresentTrace::GpuBind traceBind(device,traceGpu); // TEMP(transformers)
        if (!initialized)
            return;
        // Rare HLE/debug VRAM poke (none in the game stream): keep every other byte, re-upload, invalidate PGS caches.
        Refresh();
        reader.WriteVram(psm, base, bw, x, y, value);
        void *ptr = gs->map_vram_write(0, kVramBytes);
        std::memcpy(ptr, readVram.data(), kVramBytes);
        gs->end_vram_write(0, kVramBytes);
        if (shadowEnabled)
            shadowCpu.WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &v) const override
    {
        auto *self = const_cast<GSPgsBackend *>(this);
        if (!initialized)
        {
            v.clear();
            return;
        }
        self->Refresh();
        v = readVram;
    }
    GSTransferSnapshot GetTransferSnapshot() const override { return transfer; }

    void ApplyHostResolution(){if(resoptLive)ResoptBoundary(f60CurTick);}
private:
    Vulkan::Context context;
    Vulkan::Device device;
    std::unique_ptr<ps2PresentTrace::GpuTrace> traceGpu; // TEMP(transformers): released before device
    std::unique_ptr<ParallelGS::GSInterface> gs;
    bool initialized = false;

    GSCpuBackend reader;           // addressing helpers over readVram (debug ReadVram/WriteVram)
    std::vector<uint8_t> readVram; // host copy of PGS VRAM, refreshed on demand
    bool readValid = false;

    bool shadowEnabled = false;
    uint32_t shadowEvery = 30;
    GSCpuBackend shadowCpu;
    std::vector<uint8_t> shadowVram;
    uint32_t shadowReports = 0;

    GSTransferSnapshot transfer{};
    alignas(16) uint8_t fifo[16]{};
    uint32_t fifoOffset = 0, fifoSize = 0;

    // Scanout readback ring (host-visible buffers reused by size).
    struct ReadbackSlot
    {
        Vulkan::BufferHandle buffer;
        VkDeviceSize size = 0;
        bool busy = false;
    };
    struct PendingFrame
    {
        size_t slot = 0;
        Vulkan::Fence fence;
        uint32_t width = 0, height = 0, displayFbp = 0;
        uint64_t guestTick = 0; // TEMP(transformers) qualification I1a
        uint32_t field = 0;     // CSR.FIELD at that VBlankStart
        uint64_t resolutionGeneration=0; // TEMP(transformers): immutable SSAA generation through readback
        bool sharp = false; // TEMP(transformers): actual scanout mode
        bool scene = false;    // GAME(transformers): follows this scanout through the GPU readback queue
    };
    std::vector<ReadbackSlot> readbackSlots;
    std::deque<PendingFrame> pending;

    // TEMP(transformers): bounded reusable worker/render storage. Separate mutex permits recycling while
    // mailboxMutex is held; no pool lock is held across mailbox operations. Ownership transfers by move only.
    std::mutex dietPoolMutex;
    std::vector<std::vector<uint8_t>> dietPool;
    std::vector<uint8_t> DietTake() {
        if(!pdiet::gs())return {};
        std::lock_guard<std::mutex> l(dietPoolMutex);
        if(dietPool.empty())return {};
        auto v=std::move(dietPool.back());dietPool.pop_back();return v;
    }
    void DietRecycle(std::vector<uint8_t> v) {
        if(!pdiet::gs()||v.capacity()==0)return;
        std::lock_guard<std::mutex> l(dietPoolMutex);
        if(dietPool.size()<8)dietPool.push_back(std::move(v)); // Keep size: resize must not zero-fill reused pixels.
    }
    std::mutex mailboxMutex;
    PresentationFrame PublishSharp(PresentationFrame f,bool sharp) {
        ps2_perfscope::Scope dietScope(ps2_perfscope::RnSharpPublish); // TEMP(transformers)
        if(!f)return f;
        std::lock_guard<std::mutex> lock(g_sharpMutex);
        if(!pdiet::rn())g_sharpPixels.clear(); // TEMP(transformers): retain size as well as capacity
        g_sharpReady=false;g_sharpWidth=g_sharpHeight=0;
        if(!sharp) {
            g_lazyNativeSource.clear();g_lazyWidth=g_lazyHeight=0; // TEMP(transformers)
            g_nativePresentedWidth.store(f.width,std::memory_order_release);return f;
        }
        const uint32_t w=f.width,h=f.height,stride=std::max(kFrameStride,w);
        g_sharpReady=true;g_sharpWidth=w;g_sharpHeight=h;g_sharpPixels.resize(size_t(w)*h*4);
        for(uint32_t y=0;y<h;++y)std::memcpy(g_sharpPixels.data()+size_t(y)*w*4,f.pixels.data()+size_t(y)*stride*4,size_t(w)*4);
        // Preserve native-size script/mark consumers without changing guest VRAM or STOREIMAGE.
        f.width=w/2;f.height=(h+1)/2;
        if(pdiet::rn()) {
            if(pdiet::lazy()) {
                // TEMP(transformers): one-byte descriptor cannot alias a valid RGBA raster (minimum four bytes).
                auto spare=std::move(g_lazyNativeSource);g_lazyNativeSource=std::move(f.pixels);
                g_lazyWidth=w;g_lazyHeight=h;f.pixels.assign(1,0);
                DietRecycle(std::move(spare));
                g_nativePresentedWidth.store(f.width,std::memory_order_release);return f;
            }
            auto oldPixels=std::move(f.pixels);
            f.pixels=DietTake();
            // TEMP(transformers): native consumers require only active rows plus zero stride padding.
            f.pixels.resize(size_t(kFrameStride)*f.height*4);
            for(uint32_t y=0;y<f.height;++y) {
                auto *o=f.pixels.data()+size_t(y)*kFrameStride*4;
                pdiet::downRow(g_sharpPixels.data()+size_t(y*2)*w*4,
                    g_sharpPixels.data()+size_t(std::min(y*2+1,h-1))*w*4,o,f.width);
                std::memset(o+size_t(f.width)*4,0,size_t(kFrameStride-f.width)*4);
            }
            DietRecycle(std::move(oldPixels));
            g_nativePresentedWidth.store(f.width,std::memory_order_release);
            return f;
        }
        f.pixels.assign(size_t(kFrameStride)*kFrameMaxHeight*4,0);
        for(uint32_t y=0;y<f.height;++y)for(uint32_t x=0;x<f.width;++x)for(uint32_t c=0;c<4;++c) {
            unsigned sum=0;
            for(uint32_t dy=0;dy<2;++dy)for(uint32_t dx=0;dx<2;++dx)
                sum+=g_sharpPixels[(size_t(std::min(y*2+dy,h-1))*w+x*2+dx)*4+c];
            f.pixels[(size_t(y)*kFrameStride+x)*4+c]=uint8_t((sum+2)/4);
        }
        g_nativePresentedWidth.store(f.width,std::memory_order_release);
        return f;
    }
    PresentationFrame mailbox;
    bool mailboxSharp=false,mailboxNextSharp=false; // TEMP(transformers): travels with the mailbox image
    // TEMP(transformers) qualification: PS2X_PRESENT_ALLFRAMES=1 (default off) - newer distinct guest frame waiting
    // behind `mailbox`; statLostFrames = distinct guest frames dropped anyway (render thread more than one frame behind).
    PresentationFrame mailboxNext;
    bool mailboxScene = false, mailboxNextScene = false;
    uint64_t wsLastPath1Tick = 0;
    const uint32_t presentRate = envU32("PS2X_PRESENT_RATE", 0u); // TEMP(transformers)
    const bool presentAllFrames = presentRate == 30 ? false : presentRate == 60 ? true : envU32("PS2X_PRESENT_ALLFRAMES", 0u) != 0u;
    uint64_t statLostFrames = 0;

    // Stats ("[gs:pgs]" line every ~5 s of activity).
    uint64_t statPackets = 0, statBytes = 0, statCarries = 0, statVsyncs = 0, statFrames = 0, statEmptyScanouts = 0;
    uint64_t statCrtFallback = 0, statReadbacks = 0, statReadbackBytes = 0, statLagReadbacks = 0, statDropped = 0;
    uint64_t statShadowReadbacks = 0, statShadowReadbackMismatch = 0;
    double statVsyncMs = 0.0, statLagReadbackMs = 0.0;
    std::chrono::steady_clock::time_point statLast = std::chrono::steady_clock::now();
    uint64_t statLastVsyncs = 0, statLastFrames = 0;

    // ---- TEMP(transformers) qualification I1a: PS2X_FIELD60 state ----------------------------------------------------
    struct F60Entry
    {
        PresentationFrame frame;
        bool sharp=false; // TEMP(transformers): actual per-frame scanout result
        uint64_t tick = 0;
        int kind = 0; // see ps2Field60LastPresented
        uint32_t field = 0;
        uint32_t gap = 0;
        bool audioMovie = false; // TEMP(transformers): retain final movie entries through handoff
        bool scene = false; // GAME(transformers): both split fields inherit their source image classification
        std::chrono::steady_clock::time_point due;
    };
    const int f60Mode = field60Cfg().mode;
    // The GIF stream is scanned for the SCANMSK composite in every mode (weave detection, PS2X_FIELD60_WEAVE_VS); the
    // packet split + FBP0 capture only happens while armed (mode 2 / PS2X_FIELD60_PRECAP window).
    const bool f60Scan = f60Mode != 0;
    bool f60PrecapArmed = false;
    bool f60CompositeSeen = false;
    uint64_t f60LastCompositeTick = 0; // guest vsync of the last SCANMSK composite draw
    uint64_t f60StatFlat = 0;          // new scanouts shown whole because no composite was drawn recently
    uint64_t f60CurTick = 0;
    uint64_t f60Frame[2] = {0, 0}, f60Tex0[2] = {0, 0}; // FRAME_1/2, TEX0_1/2 shadow (A+D / PACKED / REGLIST writes)
    int f60HitCtx = 0;
    uint32_t f60SceneFbw = 8u;
    // Pre-composite fields: [0] = field 1 (FBP0 rows 0..223, t = N), [1] = field 2 (rows 224..447, t = N - 0.5).
    // The scanout that shows a composite arrives about one game frame (2 vsyncs) after the capture (the next frame's
    // composite may be captured before that scanout is ingested), so keep the last 3 and pick the best match.
    struct F60Capture
    {
        std::vector<uint8_t> f[2];
        uint32_t w = 0, h = 0;
        uint64_t tick = 0;
    };
    std::deque<F60Capture> f60Caps;
    std::vector<uint8_t> f60Prev; // previous distinct scanout (w*h*4) for repeat detection + temporal-static test
    uint32_t f60PrevW = 0, f60PrevH = 0;
    uint64_t f60LastDistinctTick = 0;
    bool f60HaveArrival = false;
    std::chrono::steady_clock::time_point f60LastArrival, f60LastDue;
    bool f60LastDueDelayed = false; // TEMP(transformers): aud3e - last due included movie AV delay
    double f60EmaUs = 33367.0; // host time per game tick (2 guest vsyncs), EMA
    std::deque<F60Entry> f60Queue; // guarded by mailboxMutex
    uint64_t f60PresentSeq = 0;    // render thread (under mailboxMutex)
    std::chrono::steady_clock::time_point f60LastPresent;
    uint64_t f60StatDistinct = 0, f60StatRepeat = 0, f60StatPairs = 0, f60StatProg = 0, f60StatPrecap = 0;
    std::atomic<uint64_t> f60StatPresented{0}, f60StatDropped{0};

    static bool F60IsComposite(uint64_t frame, uint64_t tex0)
    {
        // SCANMSK field composite (INTERPOLATION_DESIGN 6.1a, critic_stream_order vs5200 draws 69,124-69,187): sprites
        // into the CT24 display buffer (FBP 112/224) sampling FBP0 as CT24. Earlier draws that use the display buffer
        // pages as scratch are CT32/CT16/Z16; the scene downsample samples FBP0 too but renders CT16S into FBP464.
        const uint32_t fbp = uint32_t(frame & 0x1FFu), fpsm = uint32_t((frame >> 24) & 0x3Fu);
        const uint32_t tbp = uint32_t(tex0 & 0x3FFFu), tpsm = uint32_t((tex0 >> 20) & 0x3Fu);
        return fpsm == 1u && fbp != 0u && tbp == 0u && tpsm == 1u;
    }

    bool F60Track(uint32_t reg, uint64_t v)
    {
        int ctx;
        if (reg == 0x06u || reg == 0x07u)
        {
            ctx = int(reg - 0x06u);
            f60Tex0[ctx] = v;
        }
        else if (reg == 0x4Cu || reg == 0x4Du)
        {
            ctx = int(reg - 0x4Cu);
            f60Frame[ctx] = v;
            if ((v & 0x1FFu) == 0u && ((v >> 16) & 0x3Fu) != 0u)
                f60SceneFbw = uint32_t((v >> 16) & 0x3Fu); // scene target FBP0: its buffer width (8 = 512 px)
        }
        else
            return false;
        if (!F60IsComposite(f60Frame[ctx], f60Tex0[ctx]))
            return false;
        f60HitCtx = ctx;
        return true;
    }

    void F60NoteComposite()
    {
        f60CompositeSeen = true;
        f60LastCompositeTick = f60CurTick;
    }

    // Walks the GIF tags of one packet, tracking FRAME/TEX0 writes (every composite hit is noted for weave detection).
    // Returns the byte offset of the tag in which the composite state is first reached while armed (the packet is split
    // there), else -1. tbw = TEX0.TBW at the hit.
    long F60ScanForComposite(const uint8_t *data, uint32_t size, uint32_t &tbw)
    {
        uint32_t off = 0;
        while (off + 16u <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + off, 8);
            std::memcpy(&hi, data + off + 8, 8);
            const uint32_t tagOff = off;
            const uint32_t nloop = uint32_t(lo & 0x7FFFu);
            const uint32_t flg = uint32_t((lo >> 58) & 3u);
            uint32_t nreg = uint32_t((lo >> 60) & 0xFu);
            if (!nreg)
                nreg = 16u;
            off += 16u;
            bool hit = false;
            // Fast path: a PACKED/REGLIST tag whose descriptors hold no A+D (0xE) and no TEX0_1/2 (6/7) cannot change
            // FRAME/TEX0 - skip its data (vertex packets: STQ/RGBAQ/XYZ2...).
            bool relevant = false;
            for (uint32_t r = 0; r < nreg && !relevant; ++r)
            {
                const uint32_t desc = uint32_t((hi >> (4u * r)) & 0xFu);
                relevant = desc == 0x6u || desc == 0x7u || (desc == 0xEu && flg == 0u);
            }
            if (flg == 0u && !relevant)
                off += nloop * nreg * 16u;
            else if (flg == 1u && !relevant)
            {
                const uint64_t n = uint64_t(nloop) * nreg;
                off += uint32_t(n * 8u + ((n & 1u) ? 8u : 0u));
            }
            else if (flg == 0u) // PACKED
            {
                for (uint32_t i = 0; i < nloop; ++i)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (off + 16u > size)
                            return -1;
                        const uint32_t desc = uint32_t((hi >> (4u * r)) & 0xFu);
                        uint64_t v;
                        std::memcpy(&v, data + off, 8);
                        off += 16u;
                        if (desc == 0xEu)
                            hit |= F60Track(data[off - 8u], v);
                        else if (desc == 0x6u || desc == 0x7u)
                            hit |= F60Track(desc, v);
                    }
            }
            else if (flg == 1u) // REGLIST
            {
                const uint64_t n = uint64_t(nloop) * nreg;
                for (uint64_t k = 0; k < n; ++k)
                {
                    if (off + 8u > size)
                        return -1;
                    const uint32_t desc = uint32_t((hi >> (4u * uint32_t(k % nreg))) & 0xFu);
                    uint64_t v;
                    std::memcpy(&v, data + off, 8);
                    off += 8u;
                    if (desc == 0x6u || desc == 0x7u)
                        hit |= F60Track(desc, v);
                }
                if (n & 1u)
                    off += 8u;
            }
            else // IMAGE / disabled
                off += nloop * 16u;
            if (hit)
            {
                F60NoteComposite();
                if (f60PrecapArmed)
                {
                    tbw = uint32_t((f60Tex0[f60HitCtx] >> 14) & 0x3Fu);
                    return long(tagOff);
                }
            }
        }
        return -1;
    }

    // Worker, at the composite point: read FBP0 (stream-ordered GPU wait) and keep both 224-line fields. The width is
    // the scene FRAME's FBW (the composite itself samples FBP0 in 64-px TBW=1 strips, so TEX0.TBW is not it).
    void F60CaptureFields(uint32_t /*compositeTbw*/)
    {
        PollFrames(false); // finished scanouts of the previous frame are ingested with the previous fields
        const uint32_t bw = f60SceneFbw ? f60SceneFbw : 8u;
        const uint32_t w = std::min<uint32_t>(bw * 64u, kFrameStride), fh = 224u;
        const size_t bytes = std::min<size_t>(size_t(bw) * (448u / 32u) * 8192u, kVramBytes);
        const uint8_t* mapped;
        { ps2PresentTrace::Scope trace(ps2PresentTrace::FieldCapture,f60CurTick); // TEMP(transformers)
          ps2PresentTrace::GpuBind mapBind(device,traceGpu); mapped = static_cast<const uint8_t *>(gs->map_vram_read(0, bytes)); } // TEMP(transformers)
        if (!mapped)
            return;
        F60Capture cap;
        for (int f = 0; f < 2; ++f)
        {
            cap.f[f].assign(size_t(w) * fh * 4u, 0u);
            for (uint32_t y = 0; y < fh; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t addr = GSPSMCT32::addrPSMCT32(0u, bw, x, y + uint32_t(f) * fh) & (kVramBytes - 1u);
                    uint8_t *o = cap.f[f].data() + (size_t(y) * w + x) * 4u;
                    if (addr + 4u <= bytes)
                        std::memcpy(o, mapped + addr, 4u);
                    o[3] = 255u;
                }
        }
        cap.w = w;
        cap.h = fh;
        cap.tick = f60CurTick;
        f60Caps.push_back(std::move(cap));
        while (f60Caps.size() > 3u)
            f60Caps.pop_front();
        ++f60StatPrecap;
        const Field60Config &fc = field60Cfg();
        if (f60CurTick >= fc.precapLo && f60CurTick <= fc.precapHi)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "f60pre_vs%05llu_f1.ppm", static_cast<unsigned long long>(f60CurTick));
            field60WritePpm(name, f60Caps.back().f[0].data(), w, fh, w * 4u);
            std::snprintf(name, sizeof(name), "f60pre_vs%05llu_f2.ppm", static_cast<unsigned long long>(f60CurTick));
            field60WritePpm(name, f60Caps.back().f[1].data(), w, fh, w * 4u);
        }
    }

    // One output frame from a woven 448-line image S (w x h, stride kFrameStride): rows of parity `own` are kept, the
    // other rows are rebuilt (0 double = copy the own row above, 1 bob = average of the own rows above/below,
    // 2 adaptive = keep the woven row where S and the previous scanout agree on it and both neighbours (static pixel:
    // HUD, still scenery), else bob - moving pixels only ever show this field's own time; 3 median = the same static
    // test, else median3(above, woven, below)).
    void F60BuildField(const uint8_t *S, const uint8_t *prev, uint32_t w, uint32_t h, int own, bool sharp, std::vector<uint8_t> &out) const
    {
        const Field60Config &fc = field60Cfg();
        const size_t stride = size_t(std::max(kFrameStride,w)) * 4u;
        const uint32_t rowScale=sharp?2u:1u; // TEMP(transformers): preserve native field parity
        const int T = fc.adaptT;
        if(pdiet::gs()) {
            out.resize(stride*h);
            for(uint32_t y=0;y<h;++y)std::memset(out.data()+size_t(y)*stride+size_t(w)*4,0,stride-size_t(w)*4);
        } else out.assign(stride * h, 0u); // TEMP(transformers)
        for (uint32_t y = 0; y < h; ++y)
        {
            uint8_t *o = out.data() + y * stride;
            const uint8_t *row = S + y * stride;
            if (int((y / rowScale) & 1u) == own || h < 2u*rowScale)
            {
                std::memcpy(o, row, size_t(w) * 4u);
                continue;
            }
            const uint32_t yu = y >= rowScale ? y - rowScale : std::min(h-1,y + rowScale);
            const uint32_t yd = y + rowScale < h ? y + rowScale : (y>=rowScale?y-rowScale:0u);
            const uint8_t *up = S + yu * stride, *dn = S + yd * stride;
            if (pdiet::gs() && !pdiet::scalar() && fc.filter != 0) {
                pdiet::fieldRow(row,up,dn,prev?prev+size_t(y)*w*4:nullptr,
                    prev?prev+size_t(yu)*w*4:nullptr,prev?prev+size_t(yd)*w*4:nullptr,
                    o,w,fc.filter,T,true);
                continue;
            }
            if (fc.filter == 0)
                std::memcpy(o, up, size_t(w) * 4u);
            else if (fc.filter == 1)
            {
                for (uint32_t i = 0; i < w * 4u; ++i)
                    o[i] = uint8_t((uint32_t(up[i]) + dn[i] + 1u) >> 1);
            }
            else
            {
                const uint8_t *prow = prev ? prev + size_t(y) * w * 4u : nullptr;
                const uint8_t *pup = prev ? prev + size_t(yu) * w * 4u : nullptr;
                const uint8_t *pdn = prev ? prev + size_t(yd) * w * 4u : nullptr;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t i = x * 4u;
                    bool still = prow != nullptr;
                    for (int c = 0; c < 3 && still; ++c)
                        still = std::abs(int(row[i + c]) - int(prow[i + c])) <= T && std::abs(int(up[i + c]) - int(pup[i + c])) <= T &&
                                std::abs(int(dn[i + c]) - int(pdn[i + c])) <= T;
                    for (int c = 0; c < 3; ++c)
                    {
                        if (still)
                        {
                            o[i + c] = row[i + c];
                            continue;
                        }
                        const uint8_t a = up[i + c], b = row[i + c], d = dn[i + c];
                        o[i + c] = fc.filter == 2 ? uint8_t((uint32_t(a) + d + 1u) >> 1)
                                                  : std::max(std::min(a, b), std::min(std::max(a, b), d)); // median3
                    }
                }
            }
            for (uint32_t x = 0; x < w; ++x)
                o[x * 4u + 3u] = 255u;
        }
    }

    // Worker (PollFrames): a finished scanout. Repeat scans of the same buffer are skipped; each new 30 Hz frame becomes
    // two queued frames (older field first, due half a tick later), a new frame on consecutive vsyncs stays progressive.
    void F60Ingest(PresentationFrame &&frame, uint64_t tick, uint32_t field, bool scene, bool sharp, bool knownRepeat=false)
    {
        ps2PresentTrace::Scope traceIngest(ps2PresentTrace::Ingest,tick); // TEMP(transformers)
        ps2_perfscope::Scope dietScope(ps2_perfscope::GsF60); // TEMP(transformers)
        const Field60Config &fc = field60Cfg();
        const auto now = std::chrono::steady_clock::now();
        const uint32_t w = frame.width, h = frame.height;
        const size_t stride = size_t(std::max(kFrameStride,w)) * 4u;
        bool repeat = knownRepeat || !f60Prev.empty() && f60PrevW == w && f60PrevH == h;
        for (uint32_t y = 0; y < h && repeat && !knownRepeat; ++y)
            repeat = std::memcmp(frame.pixels.data() + y * stride, f60Prev.data() + size_t(y) * w * 4u, size_t(w) * 4u) == 0;
        const uint32_t gap = f60LastDistinctTick && tick > f60LastDistinctTick ? uint32_t(tick - f60LastDistinctTick) : 2u;
        if (repeat)
        {
            ++f60StatRepeat;
            if (fc.log)
            {
                std::lock_guard<std::mutex> l(field60Cfg().logMutex);
                std::fprintf(fc.log, "I\t%lld\t%llu\t%u\trepeat\t0\t%u\t0\t0\t%.0f\t0\t0\n", (long long)field60Us(now),
                             (unsigned long long)tick, field, gap, f60EmaUs);
            }
            DietRecycle(std::move(frame.pixels)); // TEMP(transformers)
            return;
        }
        ++f60StatDistinct;
        f60LastDistinctTick = tick;
        if (f60HaveArrival && gap >= 2u)
        {
            const double dtUs = std::chrono::duration<double, std::micro>(now - f60LastArrival).count() * 2.0 / double(gap);
            f60EmaUs = std::min(250000.0, std::max(16683.0, 0.75 * f60EmaUs + 0.25 * dtUs));
        }
        f60HaveArrival = true;
        f60LastArrival = now;

        // Woven source: the scanout (mode 1), or the pre-composite fields rewoven onto the scanout raster (mode 2:
        // field 2 on the older parity, field 1 on the other; 512 -> w columns linear, no HUD/bloom/temporal blend).
        const uint8_t *S = frame.pixels.data();
        std::vector<uint8_t> weave;
        const bool usePrecap = !sharp && !f60Caps.empty() && (fc.mode == 2 || (tick >= fc.precapLo && tick <= fc.precapHi));
        if (usePrecap)
        {
            // Mean |S row - field row| (sampled; field column x * fw / w) of the scanout's older-parity rows against
            // field 2 and field 1 of each kept capture: the best field-2 match picks the capture, and field 2 < field 1
            // there confirms that the older-parity rows hold field 2 (bloom/HUD/temporal blend add a common floor).
            auto madRows = [&](const F60Capture &c, int f, int parity)
            {
                double sum = 0.0;
                uint64_t n = 0;
                for (uint32_t y = uint32_t(parity) + 8u; y + 8u < h; y += 4u)
                {
                    const uint8_t *a = S + y * stride;
                    const uint8_t *b = c.f[f].data() + size_t(std::min<uint32_t>(y / 2u, c.h - 1u)) * c.w * 4u;
                    for (uint32_t x = 16u; x + 16u < w; x += 3u)
                    {
                        const uint32_t fx = std::min<uint32_t>(c.w - 1u, x * c.w / w);
                        for (int ch = 0; ch < 3; ++ch, ++n)
                            sum += std::abs(int(a[x * 4u + ch]) - int(b[fx * 4u + ch]));
                    }
                }
                return n ? sum / double(n) : 0.0;
            };
            size_t bestI = 0;
            double bestF2 = 1e30;
            for (size_t i = 0; i < f60Caps.size(); ++i)
            {
                const double m = madRows(f60Caps[i], 1, fc.olderParity);
                if (m < bestF2)
                {
                    bestF2 = m;
                    bestI = i;
                }
            }
            const F60Capture &cap = f60Caps[bestI];
            const double olderF1 = madRows(cap, 0, fc.olderParity);
            const double newerF1 = madRows(cap, 0, 1 - fc.olderParity), newerF2 = madRows(cap, 1, 1 - fc.olderParity);
            std::cout << "[field60] precap scanTick=" << tick << " capTick=" << cap.tick << " (of " << f60Caps.size()
                      << ") scan " << w << "x" << h << " field " << cap.w << "x" << cap.h << " olderRows: f2 " << bestF2
                      << " f1 " << olderF1 << " | newerRows: f1 " << newerF1 << " f2 " << newerF2 << std::endl;
            weave.assign(stride * h, 0u);
            for (uint32_t y = 0; y < h; ++y)
            {
                const int f = (int(y & 1u) == fc.olderParity) ? 1 : 0; // older parity <- field 2
                const uint32_t fy = std::min<uint32_t>(y / 2u, cap.h - 1u);
                const uint8_t *src = cap.f[f].data() + size_t(fy) * cap.w * 4u;
                uint8_t *o = weave.data() + y * stride;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const double sx = (double(x) + 0.5) * double(cap.w) / double(w) - 0.5;
                    const int x0 = std::max(0, std::min(int(cap.w) - 1, int(std::floor(sx))));
                    const int x1 = std::min(int(cap.w) - 1, x0 + 1);
                    const double a = std::max(0.0, std::min(1.0, sx - double(x0)));
                    for (int c = 0; c < 3; ++c)
                        o[x * 4u + c] = uint8_t(src[x0 * 4 + c] * (1.0 - a) + src[x1 * 4 + c] * a + 0.5);
                    o[x * 4u + 3u] = 255u;
                }
            }
            if (tick >= fc.precapLo && tick <= fc.precapHi)
            {
                char name[96];
                std::snprintf(name, sizeof(name), "f60pre_vs%05llu_scan.ppm", static_cast<unsigned long long>(tick));
                field60WritePpm(name, frame.pixels.data(), w, h, uint32_t(stride));
            }
            if (fc.mode == 2)
                S = weave.data();
        }

        std::vector<F60Entry> out;
        // Progressive: a new image on consecutive vsyncs (60 Hz content: loading vortex), or no SCANMSK composite drawn
        // within the last weaveVs guest vsyncs (FMV, 2D screens: the rows are one picture, not two fields).
        const bool flat = fc.mode != 2 && fc.weaveVs != 0u &&
                          !(f60CompositeSeen && f60LastCompositeTick + fc.weaveVs >= tick);
        const bool prog = (gap == 1u && fc.mode != 2) || flat;
        if (flat && gap != 1u)
            ++f60StatFlat;
        const uint8_t *prev = (!f60Prev.empty() && f60PrevW == w && f60PrevH == h) ? f60Prev.data() : nullptr;
        if (prog)
        {
            F60Entry e;
            e.kind = 2;
            e.frame.pixels=DietTake(); // TEMP(transformers)
            e.frame.pixels.assign(frame.pixels.begin(), frame.pixels.end());
            out.push_back(std::move(e));
            ++f60StatProg;
        }
        else
        {
            for (int k = 0; k < 2; ++k)
            {
                F60Entry e;
                e.frame.pixels=DietTake(); // TEMP(transformers)
                e.kind = (fc.mode == 2 ? 3 : 0) + k;
                const int own = k == 0 ? fc.olderParity : 1 - fc.olderParity;
                F60BuildField(S, fc.mode == 2 ? nullptr : prev, w, h, own, sharp, e.frame.pixels);
                out.push_back(std::move(e));
            }
            ++f60StatPairs;
        }
        // Keep this scanout for the next repeat test / temporal-static test (compact w*h*4).
        f60Prev.resize(size_t(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(f60Prev.data() + size_t(y) * w * 4u, frame.pixels.data() + y * stride, size_t(w) * 4u);
        f60PrevW = w;
        f60PrevH = h;

        // Pacing: first frame due now (or right after the previous due), the newer field half a tick (EMA) later.
        const auto half = std::chrono::microseconds(int64_t(f60EmaUs * 0.5));
        size_t qsize = 0;
        // TEMP(transformers): compensate only an audio-bearing movie, on the host presentation timeline.
        // The queue needs room for delayed pictures; the ordinary eight-entry cap would drop them before due.
        const uint64_t movieTick=ps2AudioPresentClock::movieTick.load(std::memory_order_acquire);
        const bool audioMovie=ps2AudioPresentClock::enabled() && ps2AudioPresentClock::movieAudio.load(std::memory_order_relaxed)
            && movieTick != 0 && tick <= movieTick + 8u && movieTick <= tick + 8u;
        const auto audioDelay=std::chrono::microseconds(audioMovie ?
            ps2AudioPresentClock::delayUs() : 0u);
        // TEMP(transformers): image identity recovers the GetPicture timestamp of this scanout.
        const int64_t movieAnchor=audioMovie ? ps2AudioPictureClock::anchor(frame.pixels.data(),w,h,stride,tick,
            0) : 0;
        DietRecycle(std::move(frame.pixels)); // TEMP(transformers): after RC1b audio anchor reads pixels.
        {
            std::lock_guard<std::mutex> lock(mailboxMutex);
            for (size_t k = 0; k < out.size(); ++k)
            {
                F60Entry &e = out[k];
                e.sharp=sharp;
                e.frame.width = w;
                e.frame.height = h;
                e.frame.displayFbp = frame.displayFbp;
                e.frame.sourceFbp = frame.sourceFbp;
                e.tick = tick;
                e.field = field;
                e.gap = gap;
                e.scene = scene;
                const auto baseDue = (k == 0 ? now : now + half);
                e.audioMovie=audioMovie;
                const int64_t baseUs=std::chrono::duration_cast<std::chrono::microseconds>(baseDue.time_since_epoch()).count();
                const int64_t previousUs=std::chrono::duration_cast<std::chrono::microseconds>(f60LastDue.time_since_epoch()).count();
                const auto due=std::chrono::steady_clock::time_point(std::chrono::microseconds(
                    ps2AudioMovieQueue::dueUs(baseUs,previousUs,movieAnchor,uint32_t(audioDelay.count()),audioMovie,f60LastDueDelayed)));
                f60LastDue = due;
                f60LastDueDelayed = audioMovie && movieAnchor > 0;
                e.due = due;
                f60Queue.push_back(std::move(e));
            }
            f60StatDropped += ps2AudioMovieQueue::trim(f60Queue,fc.noDrop,[this](auto& e){DietRecycle(std::move(e.frame.pixels));});
            qsize = f60Queue.size();
        }
        if (fc.log)
        {
            std::lock_guard<std::mutex> l(field60Cfg().logMutex);
            std::fprintf(fc.log, "I\t%lld\t%llu\t%u\t%s\t0\t%u\t0\t0\t%.0f\t%zu\t%llu\n", (long long)field60Us(now),
                         (unsigned long long)tick, field, prog ? (flat ? "flat" : "prog") : "pair", gap, f60EmaUs, qsize,
                         (unsigned long long)f60StatDropped.load());
        }
    }

    // Render thread, under mailboxMutex: the frame to show now, if one is due - one frame per host frame, in order.
    // The host loop polls at its own cadence (SetTargetFPS(60)), so a frame is often found due together with its
    // successor; it is still shown (one poll later at most). Only a real backlog (more than 2 frames already due:
    // the host loop runs slower than the field rate) drops the oldest.
    PresentationFrame F60Present()
    {
        const auto now = std::chrono::steady_clock::now();
        f60StatDropped += ps2AudioMovieQueue::discardLate(f60Queue,now,field60Cfg().noDrop,[this](auto& e){DietRecycle(std::move(e.frame.pixels));});
        if (f60Queue.empty() || f60Queue.front().due > now) {
            // TEMP(transformers): RN polls; no extra wait is introduced to observe a missing/due image.
            if(ps2PresentTrace::enabled()) ps2PresentTrace::event(ps2PresentTrace::ImagePoll,0,f60PresentSeq,
                f60Queue.size(),f60Queue.empty()?0:uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                    f60Queue.front().due.time_since_epoch()).count()));
            return {};
        }
        F60Entry e = std::move(f60Queue.front());
        ps2PresentTrace::event(ps2PresentTrace::Select,e.tick,f60PresentSeq+1,f60Queue.size()); // TEMP(transformers)
        ps2PresentTrace::brightness(e.frame.pixels.data(),e.frame.width,e.frame.height,
            size_t(std::max(kFrameStride,e.frame.width))*4,e.tick,f60PresentSeq+1); // TEMP(transformers)
        f60Queue.pop_front();
        ps2AudioVideoTrace::log(1,e.frame.pixels.data(),e.frame.width,e.frame.height,size_t(kFrameStride)*4,
            e.tick,-1,uint32_t(f60PresentSeq+1),0); // TEMP(transformers)
        ++f60PresentSeq;
        ++f60StatPresented;
        g_wsPresentedScene.store(e.scene ? 1 : 0, std::memory_order_release);
        g_field60Tick.store(e.tick, std::memory_order_relaxed);
        g_field60Kind.store(e.kind, std::memory_order_relaxed);
        g_field60Seq.store(f60PresentSeq, std::memory_order_release);
        const Field60Config &fc = field60Cfg();
        if (fc.log)
        {
            const int64_t sinceLast = f60PresentSeq > 1u ? int64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - f60LastPresent).count()) : 0;
            std::lock_guard<std::mutex> l(field60Cfg().logMutex);
            std::fprintf(fc.log, "P\t%lld\t%llu\t%u\t%d\t%llu\t%u\t%lld\t%lld\t%lld\t%zu\t%llu\n", (long long)field60Us(now),
                         (unsigned long long)e.tick, e.field, e.kind, (unsigned long long)f60PresentSeq, e.gap,
                         (long long)field60Us(e.due), (long long)std::chrono::duration_cast<std::chrono::microseconds>(now - e.due).count(),
                         (long long)sinceLast, f60Queue.size(), (unsigned long long)f60StatDropped.load());
        }
        f60LastPresent = now;
        return PublishSharp(std::move(e.frame),e.sharp);
    }

    void InitDevice()
    {
        const auto t0 = std::chrono::steady_clock::now();
        Util::register_thread_index(0); // this worker is the only thread that touches Granite
        if (!Vulkan::Context::init_loader(nullptr))
            throw std::runtime_error("PGS: Vulkan loader init failed");
        context.set_num_thread_indices(1);
        if (!context.init_instance_and_device(nullptr, 0, nullptr, 0,
                                              Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                                  Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                                  Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
            throw std::runtime_error("PGS: Vulkan instance/device creation failed");
        device.set_context(context);
        pipecache::cache().Load(device); // TEMP(transformers) qualification: before gs->init() kicks the compiles
        device.init_frame_contexts(std::max<uint32_t>(2u, envU32("PS2X_PGS_FRAME_CONTEXTS", 12u)));
        gs = std::make_unique<ParallelGS::GSInterface>();
        ParallelGS::GSOptions options = {};
        // TEMP(transformers): pre-reserve all SSAA planes for safe in-flight rate changes.
        resoptLive = envU32("PS2X_RESOPT2",0)!=0;
        options.dynamic_super_sampling = resoptLive;
        if (!gs->init(&device, options))
            throw std::runtime_error("PGS: GSInterface::init failed");
        if (resoptLive) {
            const int cap=device.supports_subgroup_size_log2(true,4,6) ? 16 : device.supports_subgroup_size_log2(true,3,6) ? 8 : 4;
            ps2x::resolution::mailbox().ready(cap);
            ps2x::settings::declareWired("pgs-live-ssaa");
            ps2x::resolution::mailbox().request(ps2x::settings::getInt("renderer.pgs_ssaa"));
        }
        ParallelGS::Hacks hacks = {};
        hacks.ignore_complex_scanmsk = !envOff("PS2X_PGS_SCANMSK_HACK"); // our patch; see gs_interface.hpp
        gs->set_hacks(hacks);
        const double deviceMs = msSince(t0);
        // Pipeline warm-up: parallel-gs kicks async compiles of every ubershader/binning variant (all SSAA rates) in
        // init(); the spike saw the first frame stall 1.36 s on them. Wait for them here, at boot, but bounded: on a
        // machine without a warm driver cache the full set took 95 s (pgs_m1a), and the compiles keep running in the
        // background anyway. PS2X_PGS_WARMUP_MS (default 20000; 0 = no wait).
        const auto tw = std::chrono::steady_clock::now();
        const uint32_t warmupMs = envU32("PS2X_PGS_WARMUP_MS", 20000u);
        const bool warm = warmupMs ? gs->wait_pipeline_warmup(warmupMs) : false;
        initialized = true;
        std::cout << "[gs:pgs] parallel-gs on '" << device.get_gpu_properties().deviceName << "' device+init "
                  << deviceMs << " ms, pipeline warm-up " << msSince(tw) << " ms ("
                  << (warm ? "complete" : (warmupMs ? "timed out, continuing in background" : "skipped"))
                  << "), scanmsk hack " << (hacks.ignore_complex_scanmsk ? "on" : "off") << std::endl;
        pipecache::cache().Start(device, warm); // TEMP(transformers) qualification: saver thread
    }

    void ResetRegisterState()
    {
        // Mirrors GS::reset() defaults (spike PgsReplayer::reset_state): FRAME FBW=10, 640x448 scissor, PRMODECONT=1,
        // TRXDIR=3, RGBAQ 0x80808080/Q=1. Direct state writes, no CLD/TRXDIR side effects.
        auto &R = gs->get_register_state();
        R.prmodecont.bits = 1;
        R.trxdir.bits = 3;
        for (int c = 0; c < 2; c++)
        {
            R.ctx[c].frame.bits = 10ull << 16;
            R.ctx[c].scissor.bits = (639ull << 16) | (447ull << 48);
        }
        R.rgbaq.bits = 0x80808080ull | (uint64_t(0x3f800000) << 32);
        R.internal_q = 1.0f;
        for (uint32_t i = 0; i < 4; i++)
            gs->get_gif_path(i) = {};
        gs->clobber_register_state();
    }

    static bool CrtModeKnown(const ParallelGS::PrivRegisterState &p)
    {
        using ParallelGS::SMODE1Bits;
        const uint32_t cmod = p.smode1.CMOD, lc = p.smode1.LC;
        if ((cmod == SMODE1Bits::CMOD_NTSC || (cmod == SMODE1Bits::CMOD_PROGRESSIVE && !p.smode2.INT)) && lc == SMODE1Bits::LC_ANALOG)
            return true;
        if (cmod == SMODE1Bits::CMOD_PAL && lc == SMODE1Bits::LC_ANALOG)
            return true;
        if (cmod == SMODE1Bits::CMOD_PROGRESSIVE && (lc == SMODE1Bits::LC_HDTV || lc == SMODE1Bits::LC_VGA))
            return true;
        return lc == SMODE1Bits::LC_UNKNOWN_29;
    }

    // TEMP(transformers): switches happen in GS stream order before the next scanout.
    bool resoptLive=false;
    uint64_t resoptRasterGeneration=0,resoptHistoryGeneration=0;
    struct ResoptStep { uint64_t tick; int samples; };
    std::vector<ResoptStep> resoptSequence;
    size_t resoptSequenceIndex=0;
    bool resoptSequenceLoaded=false;
    void ResoptBoundary(uint64_t tick) {
        namespace res=ps2x::resolution;
        if(!resoptSequenceLoaded) {
            resoptSequenceLoaded=true;
            if(const char* e=std::getenv("PS2X_RESOPT2_SEQUENCE")) {
                std::istringstream in(e);std::string item;
                while(std::getline(in,item,',')) { unsigned long long t=0;int n=0,end=0;
                    if(std::sscanf(item.c_str(),"%llu:%d%n",&t,&n,&end)==2 && end==int(item.size())) resoptSequence.push_back({t,n});
                }
            }
        }
        while(resoptSequenceIndex<resoptSequence.size() && tick>=resoptSequence[resoptSequenceIndex].tick)
            res::mailbox().request(resoptSequence[resoptSequenceIndex++].samples);
        uint64_t generation=0;int samples=1;
        if(!res::mailbox().next(generation,samples))return;
        const auto before=res::mailbox().status();std::string error;
        if(samples!=1&&samples!=2&&samples!=4&&samples!=8&&samples!=16)error="invalid SSAA rate";
        else if(samples>before.maxSamples)error="unsupported SSAA rate";
        else if(generation==envU32("PS2X_RESOPT2_FAIL_GENERATION",0))error="injected rejection before mutation";
        if(error.empty()) {
            // Flush old draws before changing the rate; retain old scanout fences until their own retirement.
            gs->flush();
            gs->set_super_sampling_rate(static_cast<ParallelGS::SuperSampling>(samples),true,false);
            Invalidate();
            // FIELD60 comparison history cannot weave across different SSAA rates.
            resoptRasterGeneration=generation;
        }
        res::mailbox().complete(generation,samples,error);
        std::fprintf(stderr,"[resopt2:gs] vs=%llu generation=%llu samples=%d applied=%d status=%s\n",
            (unsigned long long)tick,(unsigned long long)generation,samples,error.empty()?samples:before.samples,error.empty()?"applied":error.c_str());
    }

    void Invalidate() { readValid = false; }

    void Refresh()
    {
        if (readValid || !initialized)
            return;
        ps2PresentTrace::GpuBind mapBind(device,traceGpu); // TEMP(transformers)
        const void *p = gs->map_vram_read(0, kVramBytes); // flushes and waits (synchronous; debug/rare paths)
        if (!p)
            throw std::runtime_error("PGS VRAM map failed");
        std::memcpy(readVram.data(), p, kVramBytes);
        readValid = true;
    }

    void QueueScanoutCopy(const Vulkan::Image &image, uint32_t displayFbp, uint64_t guestTick, uint32_t field, bool sharp)
    {
        const uint32_t w = image.get_width(), h = image.get_height();
        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4u;
        // Bounded ring: if all slots are in flight, wait for the oldest instead of growing.
        if (pending.size() >= 3u)
            PollFrames(true);
        size_t slot = readbackSlots.size();
        for (size_t i = 0; i < readbackSlots.size(); ++i)
            if (!readbackSlots[i].busy && readbackSlots[i].size == bytes)
            {
                slot = i;
                break;
            }
        if (slot == readbackSlots.size())
        {
            for (size_t i = 0; i < readbackSlots.size(); ++i)
                if (!readbackSlots[i].busy)
                {
                    slot = i;
                    break;
                }
            if (slot == readbackSlots.size())
                readbackSlots.emplace_back();
            Vulkan::BufferCreateInfo bi = {};
            bi.domain = Vulkan::BufferDomain::CachedHost;
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            readbackSlots[slot].buffer = device.create_buffer(bi);
            readbackSlots[slot].size = bytes;
            if (!readbackSlots[slot].buffer)
                throw std::runtime_error("PGS scanout readback buffer allocation failed");
        }
        ReadbackSlot &s = readbackSlots[slot];
        auto cmd = device.request_command_buffer();
        { ps2PresentTrace::GpuScope trace(*cmd,ps2PresentTrace::GpuReadback,(uint64_t(w)<<32)|h); // TEMP(transformers)
        cmd->copy_image_to_buffer(*s.buffer, image, 0, {0, 0, 0}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}); }
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        PendingFrame pf;
        pf.sharp = sharp;
        pf.slot = slot;
        pf.width = w;
        pf.height = h;
        pf.displayFbp = displayFbp;
        pf.guestTick = guestTick;
        pf.field = field;
        pf.resolutionGeneration=resoptRasterGeneration;
        pf.scene = wsLastPath1Tick != 0u &&
                   (guestTick <= wsLastPath1Tick || guestTick - wsLastPath1Tick < 8u);
        { ps2PresentTrace::Scope trace(ps2PresentTrace::CopySubmit,guestTick); device.submit(cmd, &pf.fence); } // TEMP(transformers)
        ps2PresentTrace::event(ps2PresentTrace::CopySubmit,guestTick,0,pending.size()+1,w,h,pf.slot); // TEMP(transformers)
        s.busy = true;
        pending.push_back(std::move(pf));
    }

    void PollFrames(bool waitOldest)
    {
        ps2_perfscope::Scope dietScope(ps2_perfscope::GsReadback); // TEMP(transformers)
        while (!pending.empty())
        {
            PendingFrame &pf = pending.front();
            if (waitOldest)
            {
                ps2PresentTrace::Scope trace(ps2PresentTrace::FenceWait,pf.guestTick); // TEMP(transformers)
                pf.fence->wait();
                waitOldest = false;
            }
            else {
                ps2PresentTrace::Scope trace(ps2PresentTrace::FencePoll,pf.guestTick); // TEMP(transformers)
                if (!pf.fence->wait_timeout(0)) break;
            }
            ReadbackSlot &s = readbackSlots[pf.slot];
            const uint8_t* src;
            { ps2PresentTrace::Scope trace(ps2PresentTrace::Map,pf.guestTick); // TEMP(transformers)
              src = static_cast<const uint8_t *>(device.map_host_buffer(*s.buffer, Vulkan::MEMORY_ACCESS_READ_BIT)); }
            ps2PresentTrace::Scope traceCopy(ps2PresentTrace::Copy,pf.guestTick); // TEMP(transformers)
            if (!src)
                throw std::runtime_error("PGS scanout mapping failed");
            PresentationFrame frame;
            frame.width = pf.width;
            frame.height = pf.height;
            frame.displayFbp = pf.displayFbp;
            frame.sourceFbp = pf.displayFbp;
            bool dietRepeat=pdiet::gs() && f60Mode && pf.resolutionGeneration==resoptHistoryGeneration &&
                !f60Prev.empty() && f60PrevW==frame.width && f60PrevH==frame.height;
            for(uint32_t y=0;y<frame.height && dietRepeat;++y)
                dietRepeat=pdiet::rgbEqual(src+size_t(y)*frame.width*4,f60Prev.data()+size_t(y)*frame.width*4,size_t(frame.width)*4);
            if(!dietRepeat) {
            const size_t frameBytes=size_t(std::max(kFrameStride,frame.width))*frame.height*4u;
            if(pdiet::gs()) {frame.pixels=DietTake();frame.pixels.resize(frameBytes);}
            else frame.pixels.assign(frameBytes,0u); // TEMP(transformers): preserve original OFF fill cost
            for (uint32_t y = 0; y < frame.height; ++y)
            {
                const uint8_t *in = src + size_t(y) * pf.width * 4u;
                uint8_t *out = frame.pixels.data() + size_t(y) * std::max(kFrameStride,frame.width) * 4u;
                if(pdiet::gs()) { pdiet::alphaCopy(out,in,size_t(frame.width)*4); std::memset(out+size_t(frame.width)*4,0,size_t(std::max(kFrameStride,frame.width)-frame.width)*4); continue; } // TEMP(transformers)
                std::memcpy(out, in, size_t(frame.width) * 4u);
                for (uint32_t x = 0; x < frame.width; ++x)
                    out[x * 4u + 3u] = 255u; // like GSCpuBackend normalizePresentationAlpha; raylib draws with blending
            }
            } // TEMP(transformers): no materialization for an RGB-identical repeated scanout
            device.unmap_host_buffer(*s.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
            s.busy = false;
            const uint64_t pfTick = pf.guestTick;
            const uint32_t pfField = pf.field;
            const bool pfScene = pf.scene;
            const bool pfSharp = pf.sharp;
            const uint64_t pfGeneration=pf.resolutionGeneration;
            pending.pop_front();
            ps2PresentTrace::event(ps2PresentTrace::Retire,pfTick,0,pending.size(),pfField,pfGeneration,pfSharp); // TEMP(transformers)
            ++statFrames;
            if (f60Mode)
            {
                if(pfGeneration!=resoptHistoryGeneration){f60Prev.clear();resoptHistoryGeneration=pfGeneration;}
                F60Ingest(std::move(frame), pfTick, pfField, pfScene, pfSharp, dietRepeat); // TEMP(transformers) lane interp I1a
                continue;
            }
            std::lock_guard<std::mutex> lock(mailboxMutex);
            if (presentAllFrames && mailbox) // TEMP(transformers) qualification: PS2X_PRESENT_ALLFRAMES=1
            {
                // Scanouts arrive per guest vsync, often several in one host frame (the EE fast-forwards the frame
                // wait, the worker drains a frame's draws then its vsyncs back to back). Newest-wins dropped whole guest
                // frames (playtest #2: 622 of 1088 scanouts, ~1 in 4 game frames never shown). A scanout with the same
                // display buffer as the newest untaken one is the same guest frame (no flip in between): replace it.
                // A different buffer = the guest flipped: queue it behind (max 2 distinct frames, then drop the oldest).
                PresentationFrame &newest = mailboxNext ? mailboxNext : mailbox;
                if (newest.displayFbp == frame.displayFbp)
                {
                    ++statDropped;
                    newest = std::move(frame);
                    (mailboxNext ? mailboxNextSharp : mailboxSharp)=pfSharp;
                    (mailboxNext ? mailboxNextScene : mailboxScene) = pfScene;
                }
                else if (!mailboxNext)
                {
                    mailboxNext = std::move(frame);
                    mailboxNextScene = pfScene;
                    mailboxNextSharp = pfSharp;
                }
                else
                {
                    ++statDropped;
                    ++statLostFrames;
                    mailbox = std::move(mailboxNext);
                    mailboxScene = mailboxNextScene;
        mailboxSharp = mailboxNextSharp;
                    mailboxNext = std::move(frame);
                    mailboxNextScene = pfScene;
                    mailboxNextSharp = pfSharp;
                }
                continue;
            }
            if (mailbox)
                ++statDropped; // render thread did not take the previous one yet: newest wins
            mailbox = std::move(frame);
            mailboxScene = pfScene;
            mailboxSharp = pfSharp;
        }
    }

    void CompareShadow()
    {
        // TEMP(transformers): PS2X_GS_SHADOW=1 lockstep diagnostic (DECISION section 7 step 4). Both sides are at the
        // same stream position (this command is ordered after every packet before the vsync).
        Refresh();
        uint64_t diffBytes = 0;
        uint32_t diffPages = 0, firstPage = UINT32_MAX, maxd = 0;
        for (uint32_t page = 0; page < kVramBytes / 8192u; ++page)
        {
            const uint8_t *a = readVram.data() + size_t(page) * 8192u, *b = shadowVram.data() + size_t(page) * 8192u;
            if (std::memcmp(a, b, 8192u) == 0)
                continue;
            ++diffPages;
            if (firstPage == UINT32_MAX)
                firstPage = page;
            for (uint32_t i = 0; i < 8192u; ++i)
                if (a[i] != b[i])
                {
                    ++diffBytes;
                    maxd = std::max<uint32_t>(maxd, uint32_t(std::abs(int(a[i]) - int(b[i]))));
                }
        }
        std::cout << "[gs:shadow] vs=" << statVsyncs << " diffbytes=" << diffBytes << " pages=" << diffPages
                  << " maxd=" << maxd << " firstpage=" << (firstPage == UINT32_MAX ? -1 : int(firstPage)) << std::endl;
        const uint32_t dumps = envU32("PS2X_GS_SHADOW_DUMPS", 0u);
        if (diffBytes && shadowReports < dumps)
        {
            ++shadowReports;
            char name[96];
            std::snprintf(name, sizeof(name), "shadow_vs%llu_pgs.bin", static_cast<unsigned long long>(statVsyncs));
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fwrite(readVram.data(), 1, kVramBytes, f);
                std::fclose(f);
            }
            std::snprintf(name, sizeof(name), "shadow_vs%llu_cpu.bin", static_cast<unsigned long long>(statVsyncs));
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fwrite(shadowVram.data(), 1, kVramBytes, f);
                std::fclose(f);
            }
        }
    }

    void MaybePrintStats()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - statLast < std::chrono::seconds(5))
            return;
        const double secs = std::chrono::duration<double>(now - statLast).count();
        statLast = now;
        const uint64_t dv = statVsyncs - statLastVsyncs, df = statFrames - statLastFrames;
        statLastVsyncs = statVsyncs;
        statLastFrames = statFrames;
        std::cout << "[gs:pgs] vsyncs=" << statVsyncs << " (+" << dv << ", " << dv / secs << "/s) scanouts=" << statFrames
                  << " (+" << df << ") empty=" << statEmptyScanouts << " dropped=" << statDropped
                  << " packets=" << statPackets << " MB=" << statBytes / 1000000u << " carry=" << statCarries
                  << " crtFallback=" << statCrtFallback << " vsyncMsAvg=" << (statVsyncs ? statVsyncMs / statVsyncs : 0.0)
                  << " readbacks=" << statReadbacks << "/" << statReadbackBytes << "B lagReadbacks=" << statLagReadbacks
                  << " lagMsAvg=" << (statLagReadbacks ? statLagReadbackMs / statLagReadbacks : 0.0);
        if (shadowEnabled)
            std::cout << " shadowReadbacks=" << statShadowReadbacks << " mismatched=" << statShadowReadbackMismatch;
        if (presentAllFrames) // TEMP(transformers) qualification
            std::cout << " allFrames=1 lostFrames=" << statLostFrames;
        if (f60Mode)
            std::cout << " f60{distinct=" << f60StatDistinct << " repeat=" << f60StatRepeat << " pairs=" << f60StatPairs
                      << " prog=" << f60StatProg << " flat=" << f60StatFlat << " presented=" << f60StatPresented << " dropped=" << f60StatDropped
                      << " precap=" << f60StatPrecap << " emaMs=" << f60EmaUs / 1000.0 << "}";
        std::cout << std::endl;
    }
};
#else
bool ps2Field60Enabled() { return false; } // TEMP(transformers) lane interp I1a: PGS-only feature
void ps2Field60LastPresented(uint64_t &seq, uint64_t &tick, int &kind) { seq = tick = 0; kind = 0; }
bool ps2PgsSetSkipNative(bool) {return false;}
bool ps2PgsLazyNativeCopy(std::vector<uint8_t>&,uint32_t,uint32_t) {return false;}
bool ps2PgsSharpTake(std::vector<uint8_t>&,uint32_t&,uint32_t&){return false;}
float ps2PgsNativeFrameWidth(float w){return w;}
#endif

// TEMP(transformers) qualification: PS2X_FRAMETIME_LOG=<file> - one line per scePadRead (= one game frame, the unit of
// every scripted fps figure) with the host time in microseconds since the first one, so 1% / 0.1% lows can be
// computed offline (private qualification evidence). Called on the EE thread only; off = one static check.
void ps2FrameTimeNote()
{
    static FILE *f = []() -> FILE *
    {
        const char *p = std::getenv("PS2X_FRAMETIME_LOG");
        FILE *h = (p && *p) ? std::fopen(p, "wb") : nullptr;
        if (h)
            std::setvbuf(h, nullptr, _IOFBF, 1u << 16);
        return h;
    }();
    if (!f)
        return;
    static const auto t0 = std::chrono::steady_clock::now();
    static uint64_t n = 0;
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(f, "%lld\n", us);
    if ((++n & 63u) == 0u)
        std::fflush(f);
}

// TEMP(transformers): drain requests even while PC1 parks EE; no guest vsync or time advance.
void ps2ResolutionWakeGS() {
#ifdef PS2X_WITH_PGS
    const auto state=ps2x::resolution::mailbox().status();
    if(state.ready && state.requested!=state.acknowledged) {
        if(auto vram=resoptVram.load(std::memory_order_acquire))
            GSThreadedBackend::EnqueueOrderedForVram(vram,[](GSRasterBackend& backend){if(auto p=dynamic_cast<GSPgsBackend*>(&backend))p->ApplyHostResolution();});
    }
#endif
}

std::unique_ptr<GSRasterBackend> CreateSelectedGSBackend()
{
    const char *backend = std::getenv("PS2X_GS_BACKEND");
    if (backend && (std::strcmp(backend, "PGS") == 0 || std::strcmp(backend, "pgs") == 0))
    {
#ifdef PS2X_WITH_PGS
        if (std::getenv("PS2X_GS_RAWREC") || std::getenv("PS2X_GS_RECORD"))
            throw std::runtime_error("PGS: the GS recorders (PS2X_GS_RAWREC/PS2X_GS_RECORD) need PS2X_GS_BACKEND=CPU");
        std::cout << "[gs] backend PGS (parallel-gs, Vulkan)" << std::endl;
        return std::make_unique<GSPgsBackend>();
#else
        throw std::runtime_error("PS2X_GS_BACKEND=PGS requires a build with -DPS2X_WITH_PGS=ON");
#endif
    }
    return std::make_unique<GSCpuBackend>();
}
