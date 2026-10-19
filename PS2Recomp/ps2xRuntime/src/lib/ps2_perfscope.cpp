#include "ps2_core_place.h" // TEMP(transformers): opt-in host placement
// TEMP(transformers): qualification - built-in frame-time breakdown telemetry. See ps2_perfscope.h and
// private qualification evidence Host-only: reads clocks/counters, never touches guest state or guest timing.
#include "ps2_perfscope.h"
#include "debug_counters.h" // ps2dbg::padReads (game frames as the scripted runs count them)

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

namespace ps2_perfscope
{
    // TEMP(transformers) qualification (release fix): VU1 calls that started in the AOT and dropped to the interpreter mid-call
    // (ps2_vu1_aot_bridge.cpp). Declared at the users, not in ps2_perfscope.h (header unchanged -> no runner rebuild).
    // TEMP(transformers): declined identity/entry calls on either execution lane.
    // TEMP(transformers) vu0fast1: actual calls and issued pairs, not stall cycles.
    std::atomic<uint64_t> g_vu0Calls{0}, g_vu0Pairs{0};
    void noteVu0Call(){g_vu0Calls.fetch_add(1,std::memory_order_relaxed);}
    void noteVu0Pairs(uint64_t n){g_vu0Pairs.fetch_add(n,std::memory_order_relaxed);}
    std::atomic<uint64_t> g_vu1Declined{0};
    void noteVu1Declined(){g_vu1Declined.fetch_add(1,std::memory_order_relaxed);}
    uint64_t vu1DeclinedCalls(){return g_vu1Declined.load(std::memory_order_relaxed);}
    std::atomic<uint64_t> g_vu1MidFallback{0};
    void noteVu1MidFallback() { g_vu1MidFallback.fetch_add(1, std::memory_order_relaxed); }
    uint64_t vu1MidFallbacks() { return g_vu1MidFallback.load(std::memory_order_relaxed); }

    namespace
    {
        bool envEnabled()
        {
            const char *v = std::getenv("PS2X_PERFSCOPE");
            return !(v && v[0] == '0');
        }
        // TEMP(transformers): aotcov4 exact worker/interpreter coverage metrics.
        bool aotcov4Metrics()
        {
            static const bool enabled = [] {
                const char *v = std::getenv("PS2X_AOTCOV4_METRICS");
                return v && v[0] == '1';
            }();
            return enabled;
        }
    }

    const bool g_enabled = envEnabled();
    thread_local ThreadState *t_state = nullptr;

    namespace
    {
        constexpr const char *kLaneName[LaneCount] = {"EE", "GS", "RN", "AU", "VU"};
        constexpr Bucket kLaneBase[LaneCount] = {EeGuest, GsBusy, RnOther, AuIdle, VuBusy};
        constexpr const char *kBucketName[BucketCount] = {
            "guest", "iop", "rpc", "vif1", "vu1", "vu0", "gif", "mpeg", "pad", "vu1Admit", "vu1Replay",
            "cap", "spin", "legacy", "idle", "gsFull", "gsSync", "gsLock",
            "vu1Csr", "vu1Mem", "vu1Gs", "vu1Cap", "vu1Other",
            "busy", "idle",
            "other", "upload", "upscale", "ui", "swap", "pad", "audio",
            "idle", "cb",
            "busy", "idle", "sharpPublish", "texUpload", "readback", "f60", "vsync"};
        // Which lane a bucket belongs to (CSV columns, printing). Charges of a bucket on another lane's thread (e.g. a GS
        // sync wait on the render thread) are reported under the thread's own lane with the same bucket name.
        constexpr Lane bucketLane(int b)
        {
            if(b>=RnSharpPublish) return b<=RnTexUpload?LaneRender:LaneGS; // TEMP(transformers)
            return b <= kLastEeBucket ? LaneEE : b <= GsIdle ? LaneGS : b <= RnAudioPump ? LaneRender : b <= AuCallback ? LaneAudio : LaneVu;
        }
        constexpr bool isEeWait(int b) { return b >= EeWaitCap && b <= kLastEeBucket; }

        uint64_t qpcNow()
        {
#if defined(_WIN32)
            LARGE_INTEGER v;
            QueryPerformanceCounter(&v);
            return static_cast<uint64_t>(v.QuadPart);
#else
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }
        uint64_t qpcFreq()
        {
#if defined(_WIN32)
            LARGE_INTEGER v;
            QueryPerformanceFrequency(&v);
            return static_cast<uint64_t>(v.QuadPart);
#else
            return 1000000000ull;
#endif
        }

        struct Registered
        {
            ThreadState *state = nullptr;
            Lane lane = LaneEE;
            uint32_t tid = 0;
#if defined(_WIN32)
            HANDLE handle = nullptr;
#endif
        };

        struct Global
        {
            std::mutex mu; // threads, audio provider, overlay, csv
            std::vector<Registered> threads;
            std::thread reporter;
            std::mutex cvMu;
            std::condition_variable cv;
            bool stopReq = false;
            std::atomic<bool> started{false}, finished{false};
            uint64_t tsc0 = 0, qpc0 = 0, qpcHz = 1;
            std::atomic<double> tscHz{0.0};
            // guest vsync / flips
            std::atomic<uint64_t> vsync{0}, flips{0};
            std::atomic<uint64_t> vsIntervalMax{0}, flipIntervalMax{0}; // tsc ticks, reset per line
            uint64_t lastVsTsc = 0, lastFlipTsc = 0;                    // owner threads only
            int lastCore = -1;                                          // EE only
            std::atomic<int> eeCore{-1};
            std::atomic<uint64_t> eeMigrations{0};
            std::atomic<bool> probeReq{false};
            std::atomic<uint64_t> probeIters{0}, probeTicks{0};
            // timed host waits (EE)
            std::atomic<uint64_t> waits{0}, waitsTimedOut{0}, overNsSum{0}, overNsMax{0}, over1{0}, over4{0}, over10{0};
            std::atomic<uint64_t> reqNsSum{0}, actNsSum{0};
            std::atomic<uint64_t> gsFullN{0}, gsSyncN{0};
            // audio
            AudioStatsFn audioFn = nullptr;
            void *audioUser = nullptr;
            // outputs
            std::string overlay;
            std::FILE *csv = nullptr;
            int periodSec = 5;
            std::atomic<int> eeTidPrio{0};
        };
        Global &G()
        {
            static Global *g = new Global(); // leaked on purpose: threads may still report during static teardown
            return *g;
        }

        // ---- host facts ---------------------------------------------------------------------------------------------
        struct TimerRes
        {
            double curMs = -1.0, finestMs = -1.0, coarsestMs = -1.0;
        };
        TimerRes queryTimerRes()
        {
            TimerRes r;
#if defined(_WIN32)
            using Fn = LONG(WINAPI *)(PULONG, PULONG, PULONG);
            static Fn fn = []() -> Fn
            {
                HMODULE nt = GetModuleHandleA("ntdll.dll");
                return nt ? reinterpret_cast<Fn>(GetProcAddress(nt, "NtQueryTimerResolution")) : nullptr;
            }();
            ULONG mn = 0, mx = 0, cur = 0; // 100 ns units; "minimum" = coarsest, "maximum" = finest
            if (fn && fn(&mn, &mx, &cur) == 0)
            {
                r.coarsestMs = mn / 10000.0;
                r.finestMs = mx / 10000.0;
                r.curMs = cur / 10000.0;
            }
#endif
            return r;
        }

        const char *priorityClassName()
        {
#if defined(_WIN32)
            switch (GetPriorityClass(GetCurrentProcess()))
            {
            case IDLE_PRIORITY_CLASS: return "Idle";
            case BELOW_NORMAL_PRIORITY_CLASS: return "BelowNormal";
            case NORMAL_PRIORITY_CLASS: return "Normal";
            case ABOVE_NORMAL_PRIORITY_CLASS: return "AboveNormal";
            case HIGH_PRIORITY_CLASS: return "High";
            case REALTIME_PRIORITY_CLASS: return "Realtime";
            default: return "?";
            }
#else
            return "n/a";
#endif
        }

        int currentCore()
        {
#if defined(_WIN32)
            return static_cast<int>(GetCurrentProcessorNumber());
#else
            return sched_getcpu();
#endif
        }

        uint64_t threadCycles(const Registered &r)
        {
#if defined(_WIN32)
            ULONG64 cycles = 0;
            if (r.handle && QueryThreadCycleTime(r.handle, &cycles))
                return cycles;
#endif
            return 0;
        }

        int threadPriority(const Registered &r)
        {
#if defined(_WIN32)
            if (r.handle)
                return GetThreadPriority(r.handle);
#endif
            return 0;
        }

        // ---- snapshots ----------------------------------------------------------------------------------------------
        struct ThreadSnap
        {
            uint64_t acc[BucketCount]{};
            uint64_t calls[BucketCount]{};
            uint64_t sampled[BucketCount]{};
            uint64_t unsampledParent[BucketCount]{};
            uint64_t vu1Interp = 0;
            uint64_t cycles = 0;
        };
        struct Snap
        {
            uint64_t tsc = 0, qpc = 0, vsync = 0, flips = 0, pads = 0;
            uint64_t vu0Calls = 0, vu0Pairs = 0; // TEMP(transformers)
            uint64_t vu1Declined = 0; // TEMP(transformers)
            uint64_t vu1MidFb = 0; // TEMP(transformers) qualification: g_vu1MidFallback
            std::vector<ThreadSnap> threads; // index = G().threads index
            uint64_t waits = 0, waitsTimedOut = 0, overNsSum = 0, over1 = 0, over4 = 0, over10 = 0, reqNsSum = 0,
                     actNsSum = 0, gsFullN = 0, gsSyncN = 0, migrations = 0;
            AudioStats audio{};
            bool audioValid = false;
        };

        Snap takeSnap()
        {
            Global &g = G();
            Snap s;
            std::lock_guard<std::mutex> lock(g.mu);
            s.tsc = __rdtsc();
            s.qpc = qpcNow();
            s.vsync = g.vsync.load(std::memory_order_relaxed);
            s.flips = g.flips.load(std::memory_order_relaxed);
            s.pads = ps2dbg::padReads.load(std::memory_order_relaxed);
            s.vu0Calls=g_vu0Calls.load(std::memory_order_relaxed);
            s.vu0Pairs=g_vu0Pairs.load(std::memory_order_relaxed);
            s.vu1Declined=vu1DeclinedCalls();
            s.vu1MidFb = g_vu1MidFallback.load(std::memory_order_relaxed);
            s.threads.resize(g.threads.size());
            for (size_t i = 0; i < g.threads.size(); ++i)
            {
                const ThreadState *st = g.threads[i].state;
                ThreadSnap &t = s.threads[i];
                for (int b = 0; b < BucketCount; ++b)
                {
                    t.acc[b] = st->acc[b].load(std::memory_order_relaxed);
                    t.calls[b] = st->calls[b].load(std::memory_order_relaxed);
                    t.sampled[b] = st->sampled[b].load(std::memory_order_relaxed);
                    t.unsampledParent[b] = st->unsampledParent[b].load(std::memory_order_relaxed);
                }
                t.vu1Interp = st->vu1Interp.load(std::memory_order_relaxed);
                // Charge the open interval (time since the thread's last mark) to its current bucket, so a thread
                // parked in one long wait still shows that wait in this window.
                const uint64_t mark = st->markTsc.load(std::memory_order_relaxed);
                const uint8_t cur = st->cur.load(std::memory_order_relaxed);
                if (mark != 0 && s.tsc > mark && cur < BucketCount)
                    t.acc[cur] += s.tsc - mark;
                t.cycles = threadCycles(g.threads[i]);
            }
            s.waits = g.waits.load(std::memory_order_relaxed);
            s.waitsTimedOut = g.waitsTimedOut.load(std::memory_order_relaxed);
            s.overNsSum = g.overNsSum.load(std::memory_order_relaxed);
            s.over1 = g.over1.load(std::memory_order_relaxed);
            s.over4 = g.over4.load(std::memory_order_relaxed);
            s.over10 = g.over10.load(std::memory_order_relaxed);
            s.reqNsSum = g.reqNsSum.load(std::memory_order_relaxed);
            s.actNsSum = g.actNsSum.load(std::memory_order_relaxed);
            s.gsFullN = g.gsFullN.load(std::memory_order_relaxed);
            s.gsSyncN = g.gsSyncN.load(std::memory_order_relaxed);
            s.migrations = g.eeMigrations.load(std::memory_order_relaxed);
            if (g.audioFn)
                s.audioValid = g.audioFn(g.audioUser, s.audio);
            return s;
        }

        uint64_t sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

        // Per-qualification result (fractions of the lane's tracked wall time).
        struct LaneWin
        {
            bool present = false;
            int threads = 0;
            double wallTicks = 0.0;       // sum over the lane's threads of tracked time
            double bucket[BucketCount]{}; // ticks (sampled buckets already scaled)
            double cpuTicks = 0.0;        // QueryThreadCycleTime delta
            uint64_t calls[BucketCount]{};
            uint64_t vu1Interp = 0;       // VU1 calls run by the interpreter (AOT declined)
            double pct(int b) const { return wallTicks > 0.0 ? 100.0 * bucket[b] / wallTicks : 0.0; }
            double cpuPct() const { return wallTicks > 0.0 ? 100.0 * cpuTicks / wallTicks : 0.0; }
        };
        struct Window
        {
            double secs = 0.0, tscHz = 0.0;
            double vsPerSec = 0.0, flipsPerSec = 0.0, padsPerSec = 0.0;
            uint64_t vsync = 0, vsDelta = 0;
            uint64_t vu0Calls = 0, vu0Pairs = 0; // TEMP(transformers)
            uint64_t vu1Declined = 0; // TEMP(transformers)
            uint64_t vu1MidFb = 0; // TEMP(transformers) qualification: mid-call VU1 AOT fallbacks in the window
            LaneWin lane[LaneCount];
            // EE-thread work per guest vsync (0 when no vsync happened in the window)
            double perVs(uint64_t n) const { return vsDelta ? static_cast<double>(n) / static_cast<double>(vsDelta) : 0.0; }
        };

        Window computeWindow(const Snap &a, const Snap &b)
        {
            Global &g = G();
            Window w;
            w.secs = static_cast<double>(sub(b.qpc, a.qpc)) / static_cast<double>(g.qpcHz);
            w.tscHz = g.tscHz.load(std::memory_order_relaxed);
            if (w.secs > 0.0)
            {
                w.vsPerSec = static_cast<double>(sub(b.vsync, a.vsync)) / w.secs;
                w.flipsPerSec = static_cast<double>(sub(b.flips, a.flips)) / w.secs;
                w.padsPerSec = static_cast<double>(sub(b.pads, a.pads)) / w.secs;
            }
            w.vsync = b.vsync;
            w.vsDelta = sub(b.vsync, a.vsync);
            w.vu0Calls=sub(b.vu0Calls,a.vu0Calls);
            w.vu0Pairs=sub(b.vu0Pairs,a.vu0Pairs);
            w.vu1Declined=sub(b.vu1Declined,a.vu1Declined);
            w.vu1MidFb = sub(b.vu1MidFb, a.vu1MidFb);
            std::vector<Lane> lanes;
            {
                std::lock_guard<std::mutex> lock(g.mu);
                for (const Registered &r : g.threads)
                    lanes.push_back(r.lane);
            }
            for (size_t i = 0; i < b.threads.size() && i < lanes.size(); ++i)
            {
                static const ThreadSnap kZero{};
                const ThreadSnap &t1 = b.threads[i];
                const ThreadSnap &t0 = i < a.threads.size() ? a.threads[i] : kZero;
                LaneWin &L = w.lane[lanes[i]];
                double d[BucketCount];
                double total = 0.0;
                for (int k = 0; k < BucketCount; ++k)
                {
                    d[k] = static_cast<double>(sub(t1.acc[k], t0.acc[k]));
                    total += d[k];
                }
                if (total <= 0.0)
                    continue;
                // Sampled bucket (one per thread, see SampledScope): each untimed call is estimated at the average timed
                // call and that time moved out of the bucket that was current around it (it was charged there).
                for (int k = 0; k < BucketCount; ++k)
                {
                    const uint64_t s = sub(t1.sampled[k], t0.sampled[k]);
                    if (s == 0)
                        continue;
                    const double avg = d[k] / static_cast<double>(s);
                    for (int p = 0; p < BucketCount; ++p)
                    {
                        const uint64_t u = sub(t1.unsampledParent[p], t0.unsampledParent[p]);
                        if (u == 0 || p == k)
                            continue;
                        const double extra = std::min(avg * static_cast<double>(u), d[p]);
                        d[p] -= extra;
                        d[k] += extra;
                    }
                }
                L.present = true;
                ++L.threads;
                L.wallTicks += total;
                for (int k = 0; k < BucketCount; ++k)
                {
                    L.bucket[k] += d[k];
                    L.calls[k] += sub(t1.calls[k], t0.calls[k]);
                }
                L.vu1Interp += sub(t1.vu1Interp, t0.vu1Interp);
                L.cpuTicks += static_cast<double>(sub(t1.cycles, t0.cycles));
            }
            return w;
        }

        std::string fmt(const char *f, double v)
        {
            char b[64];
            std::snprintf(b, sizeof(b), f, v);
            return b;
        }

        // Buckets of `lane` sorted by share, those >= minPct, as "name pct name pct ..."
        std::string laneBuckets(const LaneWin &L, Lane lane, bool waits, double minPct)
        {
            std::vector<std::pair<double, int>> v;
            for (int k = 0; k < BucketCount; ++k)
            {
                if (lane == LaneEE && bucketLane(k) == LaneEE && isEeWait(k) != waits)
                    continue;
                const double p = L.pct(k);
                if (p >= minPct && (bucketLane(k) == lane || p >= 0.05))
                    v.push_back({p, k});
            }
            std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
            std::string out;
            for (const auto &[p, k] : v)
            {
                if (!out.empty())
                    out += ' ';
                out += kBucketName[k];
                out += ' ';
                out += fmt(p >= 10.0 ? "%.0f" : "%.1f", p);
            }
            return out.empty() ? std::string("-") : out;
        }

        double eeWaitPct(const LaneWin &L)
        {
            double p = 0.0;
            for (int k = EeWaitCap; k <= kLastEeBucket; ++k)
                p += L.pct(k);
            return p;
        }

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

        void writeCsvHeader(std::FILE *f)
        {
            std::fprintf(f, "wall,t_s,win_s,vs,vs_per_s,flips_per_s,pads_per_s,vs_max_ms,flip_max_ms");
            for (int lane = 0; lane < LaneCount; ++lane)
            {
                std::fprintf(f, ",%s_threads,%s_cpu_pct", kLaneName[lane], kLaneName[lane]);
                for (int k = 0; k < RnSharpPublish; ++k)
                    if (bucketLane(k) == lane)
                        std::fprintf(f, ",%s_%s_pct", kLaneName[lane], kBucketName[k]);
            }
            std::fprintf(f, ",EE_wait_pct,RN_gsSync_pct,iop_calls,gif_calls,vu1_calls,vif1_calls,gs_full_waits,gs_sync_waits"
                            ",timed_waits,timed_out,oversleep_avg_ms,oversleep_max_ms,oversleep_gt1ms,oversleep_gt4ms,"
                            "oversleep_gt10ms,timer_res_ms,prio_class,ee_thread_prio,ee_core,ee_migrations,ee_clock_ghz,"
                            "audio_underruns,audio_dropped_frames,audio_queued_ms,audio_target_ms,audio_drc_pct,"
                            "vu1_interp_calls,vs_delta,gif_per_vs,vu1_per_vs,vu1_interp_per_vs,ee_busy_pct,"
                            "VU_vif1_pct,VU_vu1_pct,VU_gif_pct,VU_vu1_per_vs,vu1_fence_per_vs,vu1_fence_csr_per_vs,"
                            "vu1_fence_mem_per_vs,vu1_fence_gs_per_vs,vu1_fence_cap_per_vs,vu1_fence_other_per_vs,vu1_fence_wait_pct");
            // TEMP(transformers): preserve shipping CSV schema unless explicitly enabled.
            if (aotcov4Metrics())
                std::fprintf(f, ",VU_interp_calls,vu1_midfall_calls,vu1_declined_calls");
            std::fprintf(f, ",vu0_calls,vu0_pairs,vu0_calls_per_vs,vu0_pairs_per_vs"); // TEMP(transformers)
            for(int k=RnSharpPublish;k<BucketCount;++k) // TEMP(transformers): append after existing VU0 columns.
                std::fprintf(f,",%s_%s_pct",kLaneName[bucketLane(k)],kBucketName[k]);
            std::fprintf(f, "\n");
            std::fflush(f);
        }

        struct LineExtras
        {
            double vsMaxMs = 0.0, flipMaxMs = 0.0, overAvgMs = 0.0, overMaxMs = 0.0, clockGHz = 0.0;
            uint64_t timedWaits = 0, timedOut = 0, over1 = 0, over4 = 0, over10 = 0, gsFull = 0, gsSync = 0, migrations = 0;
            uint64_t underruns = 0, dropped = 0;
            bool audioValid = false;
            AudioStats audio{};
            TimerRes timer{};
        };

        void emit(const char *tag, double tSec, const Window &w, const LineExtras &x, bool csvRow)
        {
            Global &g = G();
            const LaneWin &E = w.lane[LaneEE];
            const LaneWin &S = w.lane[LaneGS];
            const LaneWin &R = w.lane[LaneRender];
            const LaneWin &A = w.lane[LaneAudio];
            const double eeWait = eeWaitPct(E);
            std::string line;
            line.reserve(640);
            char head[200];
            std::snprintf(head, sizeof(head), "[perfscope]%s t=%.0fs vs/s=%.1f flips/s=%.1f pads/s=%.1f vsMaxMs=%.0f flipMaxMs=%.0f", tag,
                          tSec, w.vsPerSec, w.flipsPerSec, w.padsPerSec, x.vsMaxMs, x.flipMaxMs);
            line += head;
            if (E.present)
            {
                char per[160];
                std::snprintf(per, sizeof(per), " gif/vs=%.0f vu1/vs=%.0f vu1interp/vs=%.1f vu1fb/vs=%.1f vu1decl/vs=%.1f", w.perVs(E.calls[EeGif]),
                              w.perVs(E.calls[EeVu1]), w.perVs(E.vu1Interp), w.perVs(w.vu1MidFb), w.perVs(w.vu1Declined));
                line += per;
                line += " vu0calls/vs=" + fmt("%.2f", w.perVs(w.vu0Calls)) +
                        " vu0pairs/vs=" + fmt("%.2f", w.perVs(w.vu0Pairs)); // TEMP(transformers)
                line += " | EE cpu=" + fmt("%.0f", E.cpuPct()) + "% work=" + fmt("%.0f", 100.0 - eeWait) + " (" +
                        laneBuckets(E, LaneEE, false, 0.1) + ") wait=" + fmt("%.0f", eeWait) + " (" +
                        laneBuckets(E, LaneEE, true, 0.1) + ")";
            }
            if (S.present)
                line += " | GS busy=" + fmt("%.0f", S.pct(GsBusy)) + "% cpu=" + fmt("%.0f", S.cpuPct()) + "%";
            if (w.lane[LaneVu].present) // TEMP(transformers) qualification
            {
                const LaneWin &V = w.lane[LaneVu];
                uint64_t fences = 0;
                for (int k = EeWaitVu1Csr; k <= EeWaitVu1Other; ++k)
                    fences += E.calls[k];
                line += " | VU work=" + fmt("%.0f", 100.0 - V.pct(VuIdle)) + "% cpu=" + fmt("%.0f", V.cpuPct()) + "% (" +
                        laneBuckets(V, LaneVu, false, 0.1) + ") vu1/vs=" + fmt("%.0f", w.perVs(V.calls[EeVu1])) +
                        " fences/vs=" + fmt("%.2f", w.perVs(fences));
            }
            if (R.present)
                line += " | RN cpu=" + fmt("%.0f", R.cpuPct()) + "% (" + laneBuckets(R, LaneRender, false, 0.5) + ")";
            if (A.present || x.audioValid)
            {
                line += " | AU cb=" + fmt("%.2f", A.pct(AuCallback)) + "%";
                if (x.audioValid)
                {
                    char au[160];
                    std::snprintf(au, sizeof(au), " ur=+%llu drop=+%llu q=%.0fms drc=%+.2f%%",
                                  static_cast<unsigned long long>(x.underruns), static_cast<unsigned long long>(x.dropped),
                                  x.audio.queuedMs, x.audio.drcPercent);
                    line += au;
                }
            }
            char tail[260];
            std::snprintf(tail, sizeof(tail),
                          " | sleeps n=%llu over avg=%.2f max=%.2fms >1ms=%llu >4ms=%llu | gsWaits full=%llu sync=%llu | "
                          "timer=%.3fms prio=%s/%d core=%d mig=%llu clk=%.2fGHz",
                          static_cast<unsigned long long>(x.timedOut), x.overAvgMs, x.overMaxMs,
                          static_cast<unsigned long long>(x.over1), static_cast<unsigned long long>(x.over4),
                          static_cast<unsigned long long>(x.gsFull), static_cast<unsigned long long>(x.gsSync), x.timer.curMs,
                          priorityClassName(), g.eeTidPrio.load(), g.eeCore.load(),
                          static_cast<unsigned long long>(x.migrations), x.clockGHz);
            line += tail;
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);

            if (!csvRow)
                return;
            std::lock_guard<std::mutex> lock(g.mu);
            if (!g.csv)
                return;
            std::FILE *f = g.csv;
            std::fprintf(f, "%s,%.1f,%.2f,%llu,%.2f,%.2f,%.2f,%.1f,%.1f", wallClock().c_str(), tSec, w.secs,
                         static_cast<unsigned long long>(w.vsync), w.vsPerSec, w.flipsPerSec, w.padsPerSec, x.vsMaxMs,
                         x.flipMaxMs);
            for (int lane = 0; lane < LaneCount; ++lane)
            {
                const LaneWin &L = w.lane[lane];
                std::fprintf(f, ",%d,%.2f", L.threads, L.cpuPct());
                for (int k = 0; k < RnSharpPublish; ++k)
                    if (bucketLane(k) == lane)
                        std::fprintf(f, ",%.3f", L.pct(k));
            }
            std::fprintf(f, ",%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.3f,%llu,%llu,%llu,%.4f,%s,%d,%d,%llu,%.3f",
                         eeWait, R.pct(EeWaitGsSync), static_cast<unsigned long long>(E.calls[EeIop]),
                         static_cast<unsigned long long>(E.calls[EeGif]), static_cast<unsigned long long>(E.calls[EeVu1]),
                         static_cast<unsigned long long>(E.calls[EeVif1]), static_cast<unsigned long long>(x.gsFull),
                         static_cast<unsigned long long>(x.gsSync), static_cast<unsigned long long>(x.timedWaits),
                         static_cast<unsigned long long>(x.timedOut), x.overAvgMs, x.overMaxMs,
                         static_cast<unsigned long long>(x.over1), static_cast<unsigned long long>(x.over4),
                         static_cast<unsigned long long>(x.over10), x.timer.curMs, priorityClassName(), g.eeTidPrio.load(),
                         g.eeCore.load(), static_cast<unsigned long long>(x.migrations), x.clockGHz);
            if (x.audioValid)
                std::fprintf(f, ",%llu,%llu,%.1f,%.1f,%.3f", static_cast<unsigned long long>(x.underruns),
                             static_cast<unsigned long long>(x.dropped), x.audio.queuedMs, x.audio.targetMs, x.audio.drcPercent);
            else
                std::fprintf(f, ",,,,,");
            std::fprintf(f, ",%llu,%llu,%.1f,%.1f,%.2f,%.2f", static_cast<unsigned long long>(E.vu1Interp),
                         static_cast<unsigned long long>(w.vsDelta), w.perVs(E.calls[EeGif]), w.perVs(E.calls[EeVu1]),
                         w.perVs(E.vu1Interp), E.present ? 100.0 - eeWait : 0.0);
            {   // TEMP(transformers) qualification: VU1 worker shares (of the worker's wall time) + EE fence waits per vsync
                const LaneWin &V = w.lane[LaneVu];
                uint64_t fences = 0;
                double fenceWait = 0.0;
                for (int k = EeWaitVu1Csr; k <= EeWaitVu1Other; ++k)
                {
                    fences += E.calls[k];
                    fenceWait += E.pct(k);
                }
                std::fprintf(f, ",%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", V.pct(EeVif1), V.pct(EeVu1), V.pct(EeGif),
                             w.perVs(V.calls[EeVu1]), w.perVs(fences), w.perVs(E.calls[EeWaitVu1Csr]), w.perVs(E.calls[EeWaitVu1Mem]),
                             w.perVs(E.calls[EeWaitVu1Gs]), w.perVs(E.calls[EeWaitVu1Cap]), w.perVs(E.calls[EeWaitVu1Other]), fenceWait);
            }
            // TEMP(transformers): EE-only vu1_interp_calls misses interpreter work on LaneVu.
            if (aotcov4Metrics())
                std::fprintf(f, ",%llu,%llu,%llu", static_cast<unsigned long long>(w.lane[LaneVu].vu1Interp),
                             static_cast<unsigned long long>(w.vu1MidFb),static_cast<unsigned long long>(w.vu1Declined));
            std::fprintf(f, ",%llu,%llu,%.4f,%.4f", static_cast<unsigned long long>(w.vu0Calls),
                         static_cast<unsigned long long>(w.vu0Pairs), w.perVs(w.vu0Calls), w.perVs(w.vu0Pairs)); // TEMP(transformers)
            for(int k=RnSharpPublish;k<BucketCount;++k) // TEMP(transformers): append after existing VU0 columns.
                std::fprintf(f,",%.3f",w.lane[bucketLane(k)].pct(k));
            std::fprintf(f, "\n");
            std::fflush(f);
        }

        // Session maxima for the TOTAL line (the per-window maxima are reset by every periodic line). Written by the
        // reporter thread; read by finalLines() after the reporter has stopped.
        double m_sessVsMaxMs = 0.0, m_sessFlipMaxMs = 0.0, m_sessOverMaxMs = 0.0;

        LineExtras extras(const Snap &a, const Snap &b, bool resetMax)
        {
            Global &g = G();
            LineExtras x;
            const double hz = g.tscHz.load(std::memory_order_relaxed);
            if (resetMax && hz > 0.0)
            {
                x.vsMaxMs = 1000.0 * static_cast<double>(g.vsIntervalMax.exchange(0)) / hz;
                x.flipMaxMs = 1000.0 * static_cast<double>(g.flipIntervalMax.exchange(0)) / hz;
                m_sessVsMaxMs = (std::max)(m_sessVsMaxMs, x.vsMaxMs);
                m_sessFlipMaxMs = (std::max)(m_sessFlipMaxMs, x.flipMaxMs);
            }
            else if (!resetMax && hz > 0.0)
            {
                // TOTAL line: session maxima = max over the emitted windows + the still-open window (no reset).
                x.vsMaxMs = (std::max)(m_sessVsMaxMs, 1000.0 * static_cast<double>(g.vsIntervalMax.load()) / hz);
                x.flipMaxMs = (std::max)(m_sessFlipMaxMs, 1000.0 * static_cast<double>(g.flipIntervalMax.load()) / hz);
            }
            x.timedWaits = sub(b.waits, a.waits);
            x.timedOut = sub(b.waitsTimedOut, a.waitsTimedOut);
            x.overAvgMs = x.timedOut ? static_cast<double>(sub(b.overNsSum, a.overNsSum)) / 1e6 / static_cast<double>(x.timedOut) : 0.0;
            if (resetMax)
            {
                x.overMaxMs = static_cast<double>(g.overNsMax.exchange(0)) / 1e6;
                m_sessOverMaxMs = (std::max)(m_sessOverMaxMs, x.overMaxMs);
            }
            else
                x.overMaxMs = (std::max)(m_sessOverMaxMs, static_cast<double>(g.overNsMax.load()) / 1e6);
            x.over1 = sub(b.over1, a.over1);
            x.over4 = sub(b.over4, a.over4);
            x.over10 = sub(b.over10, a.over10);
            x.gsFull = sub(b.gsFullN, a.gsFullN);
            x.gsSync = sub(b.gsSyncN, a.gsSyncN);
            x.migrations = sub(b.migrations, a.migrations);
            const uint64_t iters = g.probeIters.load(std::memory_order_relaxed);
            const uint64_t ticks = g.probeTicks.load(std::memory_order_relaxed);
            if (iters && ticks && hz > 0.0)
                x.clockGHz = static_cast<double>(iters) / (static_cast<double>(ticks) / hz) / 1e9;
            x.audioValid = b.audioValid;
            x.audio = b.audio;
            if (a.audioValid && b.audioValid)
            {
                x.underruns = sub(b.audio.underruns, a.audio.underruns);
                x.dropped = sub(b.audio.droppedFrames, a.audio.droppedFrames);
            }
            else if (b.audioValid)
            {
                x.underruns = b.audio.underruns;
                x.dropped = b.audio.droppedFrames;
            }
            x.timer = queryTimerRes();
            return x;
        }

        void updateOverlay(const Window &w)
        {
            const LaneWin &E = w.lane[LaneEE];
            std::string text;
            if (E.present)
            {
                std::vector<std::pair<double, int>> v;
                for (int k = 0; k <= kLastEeBucket; ++k)
                    v.push_back({E.pct(k), k});
                std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
                // "game thread busy" = EE work share (100 - waits); cpu = time the OS actually ran the thread.
                text = "EE busy " + fmt("%.0f", 100.0 - eeWaitPct(E)) + "% (cpu " + fmt("%.0f", E.cpuPct()) + "%):";
                for (size_t i = 0; i < 3 && i < v.size(); ++i)
                    text += std::string(" ") + kBucketName[v[i].second] + (isEeWait(v[i].second) ? "(wait)" : "") + " " +
                            fmt("%.0f", v[i].first) + "%";
                text += " | per vsync: GIF " + fmt("%.0f", w.perVs(E.calls[EeGif])) + " VU1 " +
                        fmt("%.0f", w.perVs(E.calls[EeVu1])) + " interp " + fmt("%.1f", w.perVs(E.vu1Interp)) + " midfb " +
                        fmt("%.1f", w.perVs(w.vu1MidFb));
            }
            const LaneWin &S = w.lane[LaneGS];
            const LaneWin &R = w.lane[LaneRender];
            if (S.present)
                text += " | GS " + fmt("%.0f", S.pct(GsBusy)) + "%";
            if (R.present)
                text += " | swap " + fmt("%.0f", R.pct(RnSwap)) + "%";
            Global &g = G();
            std::lock_guard<std::mutex> lock(g.mu);
            g.overlay = text;
        }

        void reporterMain()
        {
            Global &g = G();
            Snap start = takeSnap();
            Snap lastLine = start, lastOverlay = start;
            int tick = 0;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lk(g.cvMu);
                    if (g.cv.wait_for(lk, std::chrono::seconds(1), [&] { return g.stopReq; }))
                        return;
                }
                // TSC rate from the whole run so far (stable; QPC is the reference clock).
                {
                    const uint64_t t = __rdtsc(), q = qpcNow();
                    if (q > g.qpc0 && t > g.tsc0)
                        g.tscHz.store(static_cast<double>(t - g.tsc0) * static_cast<double>(g.qpcHz) /
                                          static_cast<double>(q - g.qpc0),
                                      std::memory_order_relaxed);
                }
                Snap now = takeSnap();
                updateOverlay(computeWindow(lastOverlay, now));
                lastOverlay = now;
                if (++tick % g.periodSec == 0)
                {
                    g.probeReq.store(true, std::memory_order_relaxed); // EE measures its clock at the next vsync
                    const double tSec = static_cast<double>(sub(now.qpc, start.qpc)) / static_cast<double>(g.qpcHz);
                    emit("", tSec, computeWindow(lastLine, now), extras(lastLine, now, true), true);
                    lastLine = now;
                }
            }
        }

        Snap g_startSnap; // whole-run totals for the final TOTAL line (reporter thread start)

        void finalLines()
        {
            Global &g = G();
            if (!g.started.load() || g.finished.exchange(true))
                return;
            const Snap now = takeSnap();
            const double tSec = static_cast<double>(sub(now.qpc, g_startSnap.qpc)) / static_cast<double>(g.qpcHz);
            const Window total = computeWindow(g_startSnap, now);
            emit(" TOTAL", tSec, total, extras(g_startSnap, now, false), false);
            if (aotcov4Metrics()) // TEMP(transformers): exact terminal coverage, including the final partial CSV window.
            {
                const uint64_t ee = total.lane[LaneEE].vu1Interp;
                const uint64_t vu = total.lane[LaneVu].vu1Interp;
                std::fprintf(stderr, "[perfscope:vu1] interp_calls=%llu EE=%llu VU=%llu\n",
                             static_cast<unsigned long long>(ee + vu),
                             static_cast<unsigned long long>(ee), static_cast<unsigned long long>(vu));
                std::fflush(stderr);
            }
        }
    }

    void registerThread(Lane lane)
    {
        ps2_core_place::onThreadStart(lane); // TEMP(transformers): also when telemetry is disabled
        if (!g_enabled || t_state)
            return;
        auto *st = new ThreadState(); // leaked on purpose (reporter may read it after the thread exits)
        for (int b = 0; b < BucketCount; ++b)
        {
            st->acc[b].store(0, std::memory_order_relaxed);
            st->calls[b].store(0, std::memory_order_relaxed);
            st->sampled[b].store(0, std::memory_order_relaxed);
            st->unsampledParent[b].store(0, std::memory_order_relaxed);
        }
        st->lane = lane;
        st->cur.store(kLaneBase[lane], std::memory_order_relaxed);
        st->markTsc.store(__rdtsc(), std::memory_order_relaxed);
        Registered r;
        r.state = st;
        r.lane = lane;
#if defined(_WIN32)
        r.tid = GetCurrentThreadId();
        r.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, r.tid);
#endif
        Global &g = G();
        {
            std::lock_guard<std::mutex> lock(g.mu);
            g.threads.push_back(r);
        }
        if (lane == LaneEE)
            g.eeTidPrio.store(threadPriority(r));
        std::fprintf(stderr, "[perfscope] thread %s tid=%u prio=%d core=%d\n", kLaneName[lane], r.tid, threadPriority(r),
                     currentCore());
        t_state = st;
    }

    void start()
    {
        if (!g_enabled)
            return;
        Global &g = G();
        if (g.started.exchange(true))
            return;
        g.tsc0 = __rdtsc();
        g.qpc0 = qpcNow();
        g.qpcHz = qpcFreq();
        if (const char *v = std::getenv("PS2X_PERFSCOPE_SEC"); v && *v)
            g.periodSec = std::max(1, std::atoi(v));
        if (const char *path = std::getenv("PS2X_PERFSCOPE_CSV"); path && *path)
        {
            const bool fresh = [&]
            {
                std::FILE *probe = std::fopen(path, "rb");
                if (!probe)
                    return true;
                std::fclose(probe);
                return false;
            }();
            g.csv = std::fopen(path, "ab");
            if (g.csv && fresh)
                writeCsvHeader(g.csv);
        }
        // Rough TSC rate until the reporter's first calibration (1 s later): 20 ms busy measurement.
        {
            const uint64_t t0 = __rdtsc(), q0 = qpcNow();
            uint64_t q1 = q0;
            while ((q1 = qpcNow()) - q0 < g.qpcHz / 50)
            {
            }
            g.tscHz.store(static_cast<double>(__rdtsc() - t0) * static_cast<double>(g.qpcHz) / static_cast<double>(q1 - q0));
        }
        const TimerRes tr = queryTimerRes();
        unsigned cpus = std::thread::hardware_concurrency();
        std::fprintf(stderr,
                     "[perfscope] on (PS2X_PERFSCOPE=0 disables): every %ds, csv=%s, tsc=%.3fGHz, timer res cur=%.3fms "
                     "finest=%.3fms coarsest=%.3fms, process priority=%s, cpus=%u\n",
                     g.periodSec, g.csv ? std::getenv("PS2X_PERFSCOPE_CSV") : "(none, set PS2X_PERFSCOPE_CSV)",
                     g.tscHz.load() / 1e9, tr.curMs, tr.finestMs, tr.coarsestMs, priorityClassName(), cpus);
        std::fflush(stderr);
        g_startSnap = takeSnap();
        std::atexit([] { finalLines(); });
        g.reporter = std::thread(reporterMain);
    }

    void stop()
    {
        if (!g_enabled)
            return;
        Global &g = G();
        if (!g.started.load())
            return;
        {
            std::lock_guard<std::mutex> lk(g.cvMu);
            g.stopReq = true;
        }
        g.cv.notify_all();
        if (g.reporter.joinable())
            g.reporter.join();
        finalLines();
        std::lock_guard<std::mutex> lock(g.mu);
        if (g.csv)
        {
            std::fclose(g.csv);
            g.csv = nullptr;
        }
    }

    void onVSync(uint64_t vsyncTick)
    {
        if (!g_enabled)
            return;
        Global &g = G();
        g.vsync.store(vsyncTick, std::memory_order_relaxed);
        const uint64_t now = __rdtsc();
        if (g.lastVsTsc != 0)
        {
            const uint64_t d = now - g.lastVsTsc;
            if (d > g.vsIntervalMax.load(std::memory_order_relaxed))
                g.vsIntervalMax.store(d, std::memory_order_relaxed);
        }
        g.lastVsTsc = now;
        const int core = currentCore();
        if (core != g.lastCore)
        {
            if (g.lastCore >= 0)
                g.eeMigrations.fetch_add(1, std::memory_order_relaxed);
            g.lastCore = core;
            g.eeCore.store(core, std::memory_order_relaxed);
        }
        if (g.probeReq.load(std::memory_order_relaxed))
        {
            // Effective clock of the EE's core: a dependent add chain runs 1 add/cycle on every x86 core we target.
            // ~40 us at 5 GHz, once per report period; host time only.
            g.probeReq.store(false, std::memory_order_relaxed);
            constexpr uint64_t kIters = 200000;
            volatile uint64_t one = 1;
            uint64_t x = 0;
            const uint64_t t0 = __rdtsc();
            for (uint64_t i = 0; i < kIters; ++i)
                x += one;
            const uint64_t t1 = __rdtsc();
            static volatile uint64_t s_sink;
            s_sink = x;
            g.probeIters.store(kIters, std::memory_order_relaxed);
            g.probeTicks.store(t1 - t0, std::memory_order_relaxed);
        }
    }

    void noteFlip()
    {
        if (!g_enabled)
            return;
        Global &g = G();
        g.flips.fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = __rdtsc();
        if (g.lastFlipTsc != 0)
        {
            const uint64_t d = now - g.lastFlipTsc;
            if (d > g.flipIntervalMax.load(std::memory_order_relaxed))
                g.flipIntervalMax.store(d, std::memory_order_relaxed);
        }
        g.lastFlipTsc = now;
    }

    void noteTimedWait(int64_t requestedNs, int64_t actualNs, bool timedOut)
    {
        if (!g_enabled)
            return;
        Global &g = G();
        g.waits.fetch_add(1, std::memory_order_relaxed);
        if (!timedOut || requestedNs <= 0)
            return;
        g.waitsTimedOut.fetch_add(1, std::memory_order_relaxed);
        g.reqNsSum.fetch_add(static_cast<uint64_t>(requestedNs), std::memory_order_relaxed);
        g.actNsSum.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, actualNs)), std::memory_order_relaxed);
        const int64_t over = std::max<int64_t>(0, actualNs - requestedNs);
        g.overNsSum.fetch_add(static_cast<uint64_t>(over), std::memory_order_relaxed);
        if (static_cast<uint64_t>(over) > g.overNsMax.load(std::memory_order_relaxed))
            g.overNsMax.store(static_cast<uint64_t>(over), std::memory_order_relaxed);
        if (over > 1000000)
            g.over1.fetch_add(1, std::memory_order_relaxed);
        if (over > 4000000)
            g.over4.fetch_add(1, std::memory_order_relaxed);
        if (over > 10000000)
            g.over10.fetch_add(1, std::memory_order_relaxed);
    }

    void noteGsWaitCount(bool full)
    {
        if (!g_enabled)
            return;
        (full ? G().gsFullN : G().gsSyncN).fetch_add(1, std::memory_order_relaxed);
    }

    void setAudioStatsProvider(AudioStatsFn fn, void *user)
    {
        if (!g_enabled)
            return;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mu);
        g.audioFn = fn;
        g.audioUser = user;
    }

    std::string overlayText()
    {
        if (!g_enabled)
            return {};
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mu);
        return g.overlay;
    }

    Counters sinceLast(int slot)
    {
        Counters c;
        if (!g_enabled || !G().started.load() || slot < 0 || slot >= 4)
            return c;
        static std::mutex s_mu;
        static Snap s_last[4];
        static bool s_have[4] = {};
        const Snap now = takeSnap();
        std::lock_guard<std::mutex> lock(s_mu);
        if (s_have[slot])
        {
            const Window w = computeWindow(s_last[slot], now);
            const LaneWin &E = w.lane[LaneEE];
            c.valid = E.present;
            c.secs = w.secs;
            c.vsyncs = w.vsDelta;
            c.gif = E.calls[EeGif];
            c.vu1 = E.calls[EeVu1];
            c.vu1Interp = E.vu1Interp;
            c.eeBusyPct = E.present ? 100.0 - eeWaitPct(E) : 0.0;
            c.eeCpuPct = E.cpuPct();
        }
        s_last[slot] = now;
        s_have[slot] = true;
        return c;
    }
}

// TEMP(transformers) qualification: hooks used by ps2_vif1_instant.h (declared there; outside the namespace so the PCH header
// does not need ps2_perfscope.h).
void ps2Vu1PerfWorkerStart() noexcept
{
    ps2_perfscope::registerThread(ps2_perfscope::LaneVu);
}

void ps2Vu1PerfIdle(bool begin) noexcept
{
    if (ps2_perfscope::ThreadState *s = ps2_perfscope::t_state)
    {
        if (begin)
            ps2_perfscope::enter(s, ps2_perfscope::VuIdle);
        else
            ps2_perfscope::leave(s);
    }
}

void ps2Vu1PerfWork(bool begin, uint8_t which) noexcept
{
    if (ps2_perfscope::ThreadState *s = ps2_perfscope::t_state)
    {
        if (begin)
            ps2_perfscope::enter(s, which ? ps2_perfscope::EeVu1Replay : ps2_perfscope::EeVu1Admit);
        else
            ps2_perfscope::leave(s);
    }
}

void ps2Vu1PerfFence(bool begin, uint8_t reason) noexcept
{
    if (ps2_perfscope::ThreadState *s = ps2_perfscope::t_state)
    {
        if (begin)
        {
            static constexpr ps2_perfscope::Bucket kByReason[5] = {ps2_perfscope::EeWaitVu1Other, ps2_perfscope::EeWaitVu1Csr,
                                                                   ps2_perfscope::EeWaitVu1Mem, ps2_perfscope::EeWaitVu1Gs,
                                                                   ps2_perfscope::EeWaitVu1Cap};
            ps2_perfscope::enter(s, kByReason[reason < 5 ? reason : 0]);
        }
        else
            ps2_perfscope::leave(s);
    }
}
