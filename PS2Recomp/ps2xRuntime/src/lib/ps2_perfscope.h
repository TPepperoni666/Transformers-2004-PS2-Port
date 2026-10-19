#pragma once
// TEMP(transformers): qualification - always-on, low-cost frame-time breakdown telemetry (host-only, never changes guest
// state). Every registered host thread (EE executor, GS worker, render/main, audio callback) keeps EXCLUSIVE time per
// bucket: entering a Scope charges the time since the last mark to the bucket on top of the thread's stack, pushes the
// new bucket; leaving charges and pops. All time is __rdtsc ticks (converted with a QPC-calibrated rate by the reporter
// thread). A reporter thread prints one "[perfscope]" line every PS2X_PERFSCOPE_SEC (default 5) seconds on stderr,
// writes a CSV row to PS2X_PERFSCOPE_CSV (if set) and keeps a short text for the F11 overlay.
// PS2X_PERFSCOPE=0 disables everything (no thread registers -> every Scope is one TLS load + branch).
// See private qualification evidence for the bucket list and how to read the line.
#include <atomic>
#include <cstdint>
#include <string>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace ps2_perfscope
{
    enum Lane : uint8_t
    {
        LaneEE = 0,
        LaneGS,
        LaneRender,
        LaneAudio,
        LaneVu, // TEMP(transformers) qualification: VU1 worker thread (PS2X_VU1_THREAD=3)
        LaneCount
    };

    enum Bucket : uint8_t
    {
        // EE executor: work
        EeGuest = 0,  // recompiled guest code + scheduler (base bucket of the EE thread = everything not below)
        EeIop,        // IOP CPU batches (runEeCycles: IOP interpreter, SPU2 mix, CD DMA)   [sampled 1/16, scaled]
        EeIopRpc,     // IOP RPC calls / SIF transfers handled synchronously
        EeVif1,       // VIF1 packet processing (unpack, MSCAL dispatch), exclusive of VU1 below
        EeVu1,        // VU1 microprograms (AOT or interpreter), exclusive of XGKICK GIF processing
        EeVu0,        // VU0 micro mode
        EeGif,        // GS frontend GIF packet processing on the EE (PATH1/2/3 -> backend queue)
        EeMpeg,       // MPEG/IPU stubs (demux, FFmpeg decode, picture copy)
        EePad,        // pad read (scePadRead -> PSPadBackend::readState)
        EeVu1Admit,   // qualification: EE GIF packet queued behind VU1 worker output (copy into a batch, mutex, insertion scan)
        EeVu1Replay,  // qualification: EE replay of queued worker output / EE batches (excl. the GIF processing = gif)
        // EE executor: waits
        EeWaitCap,    // real-time cap: condition-variable sleep before the VBlank slot (PS2X_REALTIME=1)
        EeWaitSpin,   // real-time cap: final _mm_pause spin to the slot
        EeWaitLegacy, // legacy pacing wait (cap off = scripted runs): host deadline not reached yet
        EeWaitIdle,   // no runnable guest thread: waitForEvent (next timer / vblank / external wake)
        EeWaitGsFull, // GS worker ring full (backpressure from the GS thread)
        EeWaitGsSync, // synchronous GS worker call (readback / local->host / Sync / Present / ReadVram)
        EeWaitGsLock, // GS frontend state mutex held by another thread (render-thread latch, debug UI)
        // EE executor: VU1-worker fences (qualification, PS2X_VU1_THREAD=3), by the reason the EE had to wait for the worker
        EeWaitVu1Csr,   // raw GS CSR / SIGLBLID read or write (FINISH/SIGNAL/LABEL may sit in queued work; FIELD-only reads never wait)
        EeWaitVu1Mem,   // VU1 memory / VU1 state / VIF register read (true data dependency)
        EeWaitVu1Gs,    // GS frontend needed the queue empty (GIF packet off the queue, readback, VRAM, settings)
        EeWaitVu1Cap,   // a third DMA stream waited for queue capacity
        EeWaitVu1Other, // every other fence (D/T, STMOD2 fallback, scheduler, reset, ...)
        // GS worker
        GsBusy,       // base bucket of the GS worker = executing commands
        GsIdle,       // worker waiting for work
        // render (main) thread
        RnOther,      // base bucket: raylib frame setup, clears, overlay text, everything not below
        RnUpload,     // UploadFrame: take the latched guest frame, convert, UpdateTexture
        RnUpscale,    // FSR1/CAS upscale capture + draw
        RnUi,         // debug UI callback + playtest overlays (F11/F7/F12)
        RnSwap,       // EndDrawing: SwapBuffers (+ driver vsync) + raylib SetTargetFPS(60) wait + input poll
        RnPad,        // ps2PadSampleHostInput (gamepad/keyboard sample for the game)
        RnAudioPump,  // hostAudio.pump (stats print / silent drain)
        // audio device thread
        AuIdle,       // base: device thread outside our callback
        AuCallback,   // SPU2 ring drain + jitter buffer render
        // VU1 worker thread (VU1 worker); its VIF1/VU1/GIF work is charged to EeVif1/EeVu1/EeGif in its own lane
        VuBusy,       // base bucket: running a job (not inside a VIF1/VU1/GIF scope)
        VuIdle,       // waiting for a job
        RnSharpPublish, RnTexUpload, GsReadback, GsF60, GsVsync, // TEMP(transformers): presentation kernels
        BucketCount
    };

    constexpr int kLastEeBucket = EeWaitVu1Other; // qualification
    constexpr int kMaxDepth = 24;

    struct ThreadState
    {
        std::atomic<uint64_t> acc[BucketCount];   // exclusive tsc ticks per bucket
        std::atomic<uint64_t> calls[BucketCount]; // scope entries (sampled buckets: every call, see sampledCalls)
        std::atomic<uint64_t> sampled[BucketCount];
        std::atomic<uint64_t> unsampledParent[BucketCount]; // SampledScope calls NOT timed, by the bucket that was current
        std::atomic<uint64_t> vu1Interp{0}; // VU1 calls that ran in the interpreter (AOT declined / AOT off)
        std::atomic<uint64_t> markTsc{0};
        std::atomic<uint8_t> cur{0};
        uint8_t stack[kMaxDepth]{};
        int depth = 0;
        uint8_t lane = 0;
    };

    // Set from PS2X_PERFSCOPE at static initialisation (default on).
    extern const bool g_enabled;
    extern thread_local ThreadState *t_state;

    // Register the calling thread for `lane` (its base bucket becomes the default one of that lane). No-op when disabled.
    void registerThread(Lane lane);
    // Start/stop the reporter thread (PS2Runtime::run). stop() prints a last line.
    void start();
    void stop();

    // EE executor hooks.
    void onVSync(uint64_t vsyncTick); // guest VBlankStart (EE thread): vsync interval, EE core, clock probe
    void noteFlip();                  // a new game frame was flipped in (render thread)
    // A timed host wait returned. requestedNs = what the code asked for, actualNs = how long it really took,
    // timedOut = the wait ended because the deadline passed (only those count for oversleep statistics).
    void noteTimedWait(int64_t requestedNs, int64_t actualNs, bool timedOut);
    void noteGsWaitCount(bool full);  // one GS ring-full / sync wait happened (counts, the time is in the buckets)

    // Audio statistics provider (render-thread object PS2HostAudio): returns false when no stream is active.
    struct AudioStats
    {
        uint64_t underruns = 0, droppedFrames = 0;
        double queuedMs = 0.0, targetMs = 0.0, drcPercent = 0.0;
    };
    using AudioStatsFn = bool (*)(void *user, AudioStats &out);
    void setAudioStatsProvider(AudioStatsFn fn, void *user);

    // F11 overlay text (EE busy%, top 3 EE buckets, GIF/VU1 per vsync, GS/render share), refreshed every second; empty
    // when disabled.
    std::string overlayText();

    // Window counters for the [playtest] stats line: everything since the previous call with the same slot (0..3).
    // valid=false when perfscope is disabled or on the first call of a slot (that call only starts the window).
    struct Counters
    {
        bool valid = false;
        double secs = 0.0;
        uint64_t vsyncs = 0;                   // guest vsyncs in the window
        uint64_t gif = 0, vu1 = 0, vu1Interp = 0; // EE-thread GIF packets, VU1 calls (MSCAL+MSCNT), interpreter VU1 calls
        double eeBusyPct = 0.0;                // EE thread: 100 - wait buckets (cap/spin/legacy/idle/gs*)
        double eeCpuPct = 0.0;                 // EE thread: time the OS actually ran it
        double perVs(uint64_t n) const { return vsyncs ? static_cast<double>(n) / static_cast<double>(vsyncs) : 0.0; }
    };
    Counters sinceLast(int slot);

    // ---- hot path ---------------------------------------------------------------------------------------------------
    // A VU1 call is about to run in the interpreter (ps2_vu1_core.cpp, after tryAot declined). One TLS load when disabled.
    inline void noteVu1Interp()
    {
        if (ThreadState *s = t_state)
            s->vu1Interp.store(s->vu1Interp.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    }

    inline void charge(ThreadState *s, uint64_t now)
    {
        const uint64_t mark = s->markTsc.load(std::memory_order_relaxed);
        const uint8_t c = s->cur.load(std::memory_order_relaxed);
        s->acc[c].store(s->acc[c].load(std::memory_order_relaxed) + (now - mark), std::memory_order_relaxed);
        s->markTsc.store(now, std::memory_order_relaxed);
    }
    inline void enter(ThreadState *s, Bucket b)
    {
        charge(s, __rdtsc());
        if (s->depth < kMaxDepth)
            s->stack[s->depth] = s->cur.load(std::memory_order_relaxed);
        ++s->depth;
        s->cur.store(b, std::memory_order_relaxed);
        s->calls[b].store(s->calls[b].load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    }
    inline void leave(ThreadState *s)
    {
        charge(s, __rdtsc());
        --s->depth;
        if (s->depth >= 0 && s->depth < kMaxDepth)
            s->cur.store(s->stack[s->depth], std::memory_order_relaxed);
        if (s->depth < 0)
            s->depth = 0;
    }

    // RAII scope: exclusive time of the enclosed code goes to `b` (exceptions such as EeDispatcherTransfer unwind it).
    class Scope
    {
    public:
        explicit Scope(Bucket b) : m_s(t_state)
        {
            if (m_s)
                enter(m_s, b);
        }
        ~Scope()
        {
            if (m_s)
                leave(m_s);
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        ThreadState *m_s;
    };

    // Sampled scope for very frequent calls (IOP batches every 1024 EE cycles = up to ~290k/s): only one call in 2^Shift is
    // timed; the reporter estimates the untimed calls as (unsampled calls x average timed call) and moves that time out of
    // the bucket that was current around each untimed call (unsampledParent). Unsampled calls cost one TLS load and two
    // counter increments. Only ONE bucket per thread may use SampledScope (unsampledParent is shared) - today EeIop.
    template <int Shift>
    class SampledScope
    {
    public:
        explicit SampledScope(Bucket b) : m_s(t_state)
        {
            if (!m_s)
                return;
            const uint64_t n = m_s->calls[b].load(std::memory_order_relaxed);
            m_s->calls[b].store(n + 1u, std::memory_order_relaxed);
            if ((n & ((1u << Shift) - 1u)) != 0u)
            {
                const uint8_t p = m_s->cur.load(std::memory_order_relaxed);
                m_s->unsampledParent[p].store(m_s->unsampledParent[p].load(std::memory_order_relaxed) + 1u,
                                              std::memory_order_relaxed);
                m_s = nullptr;
                return;
            }
            m_s->sampled[b].store(m_s->sampled[b].load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
            charge(m_s, __rdtsc());
            if (m_s->depth < kMaxDepth)
                m_s->stack[m_s->depth] = m_s->cur.load(std::memory_order_relaxed);
            ++m_s->depth;
            m_s->cur.store(b, std::memory_order_relaxed);
        }
        ~SampledScope()
        {
            if (m_s)
                leave(m_s);
        }
        SampledScope(const SampledScope &) = delete;
        SampledScope &operator=(const SampledScope &) = delete;

    private:
        ThreadState *m_s;
    };
}
