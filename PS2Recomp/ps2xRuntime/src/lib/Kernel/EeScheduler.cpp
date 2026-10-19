#include "runtime/ps2_eeopt.h"
// checkpoint fast path adapted from brad-richardson/PS2Recomp (GPL-3.0); budget design after snuri00/ps2-web-recomp (MIT)
#include "runtime/ps2_pc_live.h"
#include "runtime/ps2_pc_menu_state.h"
#include "../ps2_vu1new.h" // TEMP(transformers)
#include "../warpload2_trace.h" // TEMP(transformers)
#include "runtime/ee_scheduler.h"
#include "../debug_counters.h" // TEMP(transformers)
#include "../ps2_perfscope.h"  // TEMP(transformers): qualification (EE wait buckets, vsync hook)
#include "TestScript.h"        // TEMP(transformers): PS2X_SCRIPT test harness
#include "ps2_kernel_heap.h"   // TEMP(transformers) qualification: PS2X_KERNEL_HEAP pool placement

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(_MSC_VER)
#include <intrin.h> // __rdtsc (TEMP(transformers) eetrim stats)
#else
#include <x86intrin.h>
#endif

void ps2Vif1ChainResumeTick(PS2Memory &mem); // ps2_memory.cpp
void ps2PresentLatchOnVBlank(GS &gs);         // gs/gs_frontend.cpp, TEMP(transformers): PORT_LIST rank 3

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);

    // GENERIC (ported from SSX3 SSX-0013, crossover integration update): PAL titles get 50 fields/s VBlank pacing.
    // Decided once, at the first VBlank schedule (after the ELF path is known): PS2X_PAL=1/0 forces it, otherwise the
    // ELF file name's SLES/SCES prefix. NTSC (SLUS/SCUS/...) keeps the exact constants above.
    bool palVBlankTiming()
    {
        static const bool s_pal = []()
        {
            const char *e = std::getenv("PS2X_PAL");
            if (e && e[0] == '1')
                return true;
            if (e && e[0] == '0')
                return false;
            const std::string name = PS2Runtime::getIoPaths().elfPath.filename().string();
            const bool pal = name.rfind("SLES", 0) == 0 || name.rfind("SCES", 0) == 0;
            if (pal)
                std::fprintf(stderr, "[vblank] PAL timing (50 fields/s) for %s\n", name.c_str());
            return pal;
        }();
        return s_pal;
    }

    uint64_t vblankPeriodCycles()
    {
        static const uint64_t kPalCycles = microsecondsToEeCycles(20000u);
        return palVBlankTiming() ? kPalCycles : kVBlankPeriodCycles;
    }

    std::chrono::microseconds vblankPeriodHost()
    {
        return palVBlankTiming() ? std::chrono::microseconds(20000) : std::chrono::microseconds(kVBlankPeriod);
    }
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    // ---- eetrim (EE-thread trims) -------------------------------------------------------------------------------
    // Knobs are read once during static initialisation, so the hot paths below test a plain bool instead of a
    // function-local static (MSVC guards those with a TLS epoch check on every call).
    bool eetrimEnvFlag(const char *name, bool fallback)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
        {
            return fallback;
        }
        return value[0] != '0';
    }

    // PS2X_EE_IDLESKIP=0 disables the WaitVSync busy-wait fast-forward (see EeScheduler::checkpointDue).
    const bool g_eeIdleSkip = eetrimEnvFlag("PS2X_EE_IDLESKIP", true);
    // TEMP(transformers): PS2X_EETRIM_STATS=1 prints an "[eetrim]" line after every "[perf]" line: host ms spent in
    // the WaitVSync spin, spin checkpoint calls, fast-forwarded iterations, dispatcher loops.
    const bool g_eetrimStats = eetrimEnvFlag("PS2X_EETRIM_STATS", false);
    const bool g_vu1IdleSkipPending = eetrimEnvFlag("PS2X_VU1_IDLESKIP_PENDING", false); // TEMP(transformers) qualification
    const uint32_t g_vu1CkptDrainMin = []{const char *p=std::getenv("PS2X_VU1_CKPT_DRAIN_MIN");const long v=p?std::strtol(p,nullptr,10):1;return static_cast<uint32_t>(v<1?1:v);}(); // TEMP(transformers) qualification
    const bool g_idleSpinDetect = g_eeIdleSkip || g_eetrimStats;
    // PS2X_EE_PPE_FAST=0 disables the processPendingEvents() no-work fast path.
    const bool g_ppeFast = eetrimEnvFlag("PS2X_EE_PPE_FAST", true);

    // ---- realtime cap (TEMP(transformers) qualification, playtest #1 item 2) -------------------------------------
    // Guest events carry a cycle deadline AND a host deadline; processDueDeadlines() fires an event only when both have
    // passed. The VBlankStart host deadline used to be chained (next = previous + 16.667 ms) with no bound, so after a
    // stretch where the guest ran slower than real time (hub/level at 26-30 vs/s) the schedule lagged the wall clock by
    // seconds and the next light scene ran with no cap at all until the debt was repaid (playtest #1 mark 2: warp-gate
    // screen at 103.9 vs/s = 173%, music + scene sped up; ~1.33M audio samples dropped in that 30 s window).
    // PS2X_REALTIME=1 (default for normal play): VBlankStart may not fire before its real-time slot; the slot advances by
    // one real field (NTSC = the guest field, 59.9988 Hz; PAL 50 Hz from SMODE1.CMOD); when the guest is more than
    // PS2X_REALTIME_CATCHUP_MS (default 50 = 3 fields) late the schedule is pulled up to (now - 50 ms), so a slow
    // stretch is never repaid by running fast (at most 50 ms of catch-up, which also keeps uneven 2-field frames free).
    // Default OFF when PS2X_SCRIPT is set: scripted tests keep the LEGACY pacing (chained host deadlines, as before
    // release fix; v4 integration release fix - the lane's unpaced mode made counters host-timing dependent).
    // Host-only: guest cycle deadlines are untouched, so guest counters are identical either way.
    // PS2X_REALTIME_STATS=1 prints a "[realtime]" line every 600 vblanks (rate, waits, slot error percentiles).
    bool realtimeDefault()
    {
        const char *forced = std::getenv("PS2X_REALTIME");
        if (forced && *forced)
        {
            return forced[0] != '0';
        }
        const char *script = std::getenv("PS2X_SCRIPT");
        return !(script && *script);
    }
    const bool g_realtime = realtimeDefault();
    const bool g_realtimeStats = eetrimEnvFlag("PS2X_REALTIME_STATS", false);
    const std::chrono::nanoseconds g_realtimeCatchUp = []()
    {
        const char *v = std::getenv("PS2X_REALTIME_CATCHUP_MS");
        const long ms = (v && *v) ? std::strtol(v, nullptr, 10) : 50L;
        return std::chrono::nanoseconds(static_cast<int64_t>(std::max(0L, ms)) * 1000000ll);
    }();
    // NTSC field = the guest's own field length (kVBlankPeriodCycles = 16.667 ms of EE cycles, 59.9988 Hz): guest time
    // then advances exactly as fast as wall time, so the SPU2 output (48 kHz of guest time) matches the 48 kHz host
    // device with no drift. A true 59.94 Hz (16.683 ms) slot would leave the guest 0.1% slow = ~48 samples/s short.
    // PAL (v4 integration merge with release fix): the slot is ALWAYS the guest's own field, vblankPeriodCycles() - 20 ms of
    // EE cycles for PAL titles (SLES/SCES ELF or PS2X_PAL=1), 16.667 ms otherwise - so guest time == wall time for PAL
    // too. (The lane's SMODE1.CMOD check is dropped: capping at 50 Hz while the guest field stayed 16.667 ms would run a
    // PAL guest at 0.83x wall time.) PS2X_REALTIME_HZ=<hz> overrides (e.g. 59.94).
    const std::chrono::nanoseconds g_realtimeHzOverride = []()
    {
        const char *v = std::getenv("PS2X_REALTIME_HZ");
        const double hz = (v && *v) ? std::strtod(v, nullptr) : 0.0;
        return hz >= 1.0 ? std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz + 0.5)) : std::chrono::nanoseconds(0);
    }();
    // Not a namespace-scope constant: vblankPeriodCycles() decides PAL from the ELF path, known only from reset() on.
    std::chrono::nanoseconds realtimeField()
    {
        return g_realtimeHzOverride.count() != 0 ? g_realtimeHzOverride : eeCyclesToHostDuration(vblankPeriodCycles());
    }
    // Sleep on the condition variable until this much before the slot, then yield-spin: Windows timed waits wake
    // ~0.5-2 ms late even with timeBeginPeriod(1) (raylib), which would put the cap's error in that range.
    constexpr auto kRealtimeSpinMargin = std::chrono::microseconds(2000);

    struct RealtimeStats
    {
        uint64_t vblanks = 0u, waits = 0u, rebases = 0u, catchUps = 0u;
        double waitMs = 0.0;
        std::vector<float> slotErrorUs; // VBlankStart fire time - its slot, only for fields that waited
        std::chrono::steady_clock::time_point t0{};
        uint64_t vs0 = 0u;
    } g_realtimeStatsState;

    void realtimeStatsVBlank(uint64_t vsyncTick)
    {
        RealtimeStats &st = g_realtimeStatsState;
        const auto now = std::chrono::steady_clock::now();
        if (st.t0 == std::chrono::steady_clock::time_point{})
        {
            st.t0 = now;
            st.vs0 = vsyncTick;
            return;
        }
        if (++st.vblanks < 600u)
        {
            return;
        }
        const double secs = std::chrono::duration<double>(now - st.t0).count();
        std::sort(st.slotErrorUs.begin(), st.slotErrorUs.end());
        auto pct = [&](double p)
        { return st.slotErrorUs.empty() ? 0.0 : st.slotErrorUs[static_cast<size_t>(p * (st.slotErrorUs.size() - 1u))]; };
        char line[320];
        std::snprintf(line, sizeof(line),
                      "[realtime] vs=%llu rate=%.3f/s waits=%llu waitMs=%.1f rebases=%llu catchUps=%llu "
                      "slotErrUs p50=%.0f p99=%.0f max=%.0f",
                      static_cast<unsigned long long>(vsyncTick), secs > 0.0 ? (vsyncTick - st.vs0) / secs : 0.0,
                      static_cast<unsigned long long>(st.waits), st.waitMs, static_cast<unsigned long long>(st.rebases),
                      static_cast<unsigned long long>(st.catchUps), pct(0.5), pct(0.99), pct(1.0));
        std::cerr << line << std::endl;
        st = RealtimeStats{};
        st.t0 = now;
        st.vs0 = vsyncTick;
    }

    // GAME(transformers): sub_0025DE90 (WaitVSync) spins on 0x25DEE8-0x25DF1C reading only the vsync counters at
    // gp-0x4220 / gp-0x421C, which change only when an interrupt handler or event runs - i.e. only after
    // checkpointDue() has returned true. Each iteration is 14 guest instructions and one eeCheckpointDue(32) at the
    // back-edge (generated code sets ctx->pc = 0x25DEE8 just before that call).
    constexpr uint32_t kIdleSpinPc = 0x25DEE8u;
    constexpr uint32_t kIdleSpinWords[] = {
        0x8F82BDE0u, 0x0046102Au, 0x14400002u, 0x00000000u, 0x64040001u, 0x10A00006u, 0x0004102Bu, 0x8F82BDE4u,
        0x10430002u, 0x00000000u, 0x0000202Du, 0x0004102Bu, 0x38420001u, 0x1440FFF2u, 0x00000000u,
    };
    int g_idleSpinVerified = -1; // -1 = not checked yet, 0 = guest code differs (other ELF): never skip, 1 = verified

    bool idleSpinCodeMatches(const uint8_t *rdram)
    {
        if (g_idleSpinVerified < 0)
        {
            bool match = rdram != nullptr;
            for (size_t i = 0; match && i < sizeof(kIdleSpinWords) / sizeof(kIdleSpinWords[0]); ++i)
            {
                uint32_t word = 0u;
                std::memcpy(&word, rdram + kIdleSpinPc + i * 4u, sizeof(word));
                match = word == kIdleSpinWords[i];
            }
            g_idleSpinVerified = match ? 1 : 0;
            std::cerr << "[eetrim] WaitVSync idle-skip " << (match ? (g_eeIdleSkip ? "armed" : "verified (skip off)")
                                                                 : "disabled: guest code at 0x25DEE8 differs")
                      << std::endl;
        }
        return g_idleSpinVerified == 1;
    }

    // Current-thread pointer cache: currentThread() is an unordered_map lookup (std::hash FNV + bucket walk) that
    // checkpointDue/accountCycles paid on every call. Element pointers of an unordered_map stay valid until that
    // element is erased, so the cache is keyed on (scheduler, thread id, generation); every m_threads insert/erase/
    // clear bumps the generation.
    // TEMP(transformers) runtime fast path: opt-in short checkpoint accounting; no cadence change.
    inline GuestThread *cachedCurrentThread(EeScheduler &scheduler, int) {
        return scheduler.cachedCurrentThreadMember();
    }

    // TEMP(transformers): "[perf] 1M checkpoints" probe state (was function-local statics inside checkpointDue).
    uint64_t &g_perfCheckpointCalls = ps2_eeopt::checkpointCalls;
    uint64_t g_perfLastYields = 0u;
    std::chrono::steady_clock::time_point g_perfT0 = std::chrono::steady_clock::now();

    // TEMP(transformers): eetrim stats (PS2X_EETRIM_STATS=1), per [perf] window.
    struct EetrimStats
    {
        uint64_t spinEpisodeStartTsc = 0u; // rdtsc at the first spin back-edge of the current episode, 0 = none
        uint64_t spinTicks = 0u;
        uint64_t spinEpisodes = 0u;
        uint64_t spinCalls = 0u;
        uint64_t skippedIterations = 0u;
        uint64_t dispatchLoops = 0u;
        uint64_t ppeFast = 0u;
        uint64_t lastTsc = 0u;
    } g_eetrimStatsState;

    // TEMP(transformers): PS2X_WATCH=hexaddr[,hexaddr...] polls guest words at every checkpoint, logs changes.
    struct WatchSlot
    {
        uint32_t addr;
        uint32_t last;
        uint32_t logged;
    };
    std::vector<WatchSlot> parseWatchSlots()
    {
        std::vector<WatchSlot> out;
        const char *env = std::getenv("PS2X_WATCH");
        for (const char *p = env; p && *p;)
        {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            out.push_back(WatchSlot{static_cast<uint32_t>(v) & 0x1FFFFFFCu, 0u, 0u});
            p = (*end == ',') ? end + 1 : end;
        }
        return out;
    }
    std::vector<WatchSlot> g_watchSlots = parseWatchSlots();
    const bool g_watchOn = !g_watchSlots.empty();

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
    m_checkpointObserversOff = !g_watchOn && !g_eetrimStats;
    m_checkpointIdleDetect = g_idleSpinDetect;
    m_checkpointDrainMin = g_vu1CkptDrainMin;
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    noteThreadTableChanged();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    refreshCurrentThread();
    m_nextAttentionCycle = 0u;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    noteThreadTableChanged();
    m_readyQueues[0].push_back(kMainThreadId);
    std::cerr << "[realtime] cap " << (g_realtime ? "ON" : "OFF")
              << (std::getenv("PS2X_REALTIME") && *std::getenv("PS2X_REALTIME") ? " (PS2X_REALTIME)"
                                                                                : (g_realtime ? " (default)" : " (PS2X_SCRIPT set)"))
              << " fieldUs=" << std::chrono::duration<double, std::micro>(realtimeField()).count()
              << (palVBlankTiming() ? " (PAL)" : " (NTSC)")
              << " catchUpMs=" << std::chrono::duration<double, std::milli>(g_realtimeCatchUp).count()
              << std::endl; // TEMP(transformers) qualification (v4: PAL-aware field via release fix)
    scheduleEvent(m_eeCycle + vblankPeriodCycles(),
                  std::chrono::steady_clock::now() + vblankPeriodHost(),
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                refreshCurrentThread();
                m_nextAttentionCycle = 0u;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                if (invocation.kind == GuestInvocationKind::Interrupt) ps2WarpLoadTrace(23, "irq-begin-detached", m_rdram, &invocation.context, &m_runtime); // TEMP(transformers)
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                if (running->invocations.back().kind == GuestInvocationKind::Interrupt) ps2WarpLoadTrace(3, "irq-end", m_rdram, &context, &m_runtime); // TEMP(transformers)
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            refreshCurrentThread();
            m_nextAttentionCycle = 0u;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        // Interrupt-like work (INTC/DMAC handlers, SIF command handlers) may preempt an ordinary guest
        // callback that is already running on this thread (e.g. an MPEG Background callback spinning while
        // it waits for CD data that only a SIF command handler delivers), like a real interrupt would.
        // Interrupt handlers themselves are never nested.
        const auto isInterruptLike = [](GuestInvocationKind kind)
        {
            return kind == GuestInvocationKind::Interrupt || kind == GuestInvocationKind::SifCommand;
        };
        const auto isPreemptible = [](GuestInvocationKind kind)
        {
            return kind == GuestInvocationKind::RpcCallback || kind == GuestInvocationKind::HleCall;
        };
        const bool nestInterrupt = !m_pendingInvocations.empty() && !running->invocations.empty() &&
                                   isInterruptLike(m_pendingInvocations.front().kind) &&
                                   isPreemptible(running->invocations.back().kind);
        if (!m_pendingInvocations.empty() && (running->invocations.empty() || nestInterrupt))
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                if (nestInterrupt)
                {
                    // Keep clear of the interrupted callback's live stack frames.
                    const uint32_t interruptedSp = getRegU32(&running->activeContext(), 29);
                    SET_GPR_U32(&invocation.context, 29, (interruptedSp - 0x1000u) & ~0xFu);
                }
                else
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
            }
            if (invocation.kind == GuestInvocationKind::Interrupt) ps2WarpLoadTrace(2, "irq-begin", m_rdram, &invocation.context, &m_runtime, static_cast<uint32_t>(running->invocations.size())); // TEMP(transformers)
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                ps2WarpLoadTrace(22, "missing-invocation", m_rdram, &context, &m_runtime, static_cast<uint32_t>(running->invocations.back().kind)); // TEMP(transformers)
                context.pc = 0u;
            }
            else
            {
                { // TEMP(transformers): dump the stack around sp when the main context lands on a bogus pc
                    const uint32_t spDump = getRegU32(&context, 29) & 0x1FFFFFF0u;
                    std::cerr << "[ee-missing:stack] thread=" << running->id << " sp=0x" << std::hex << spDump;
                    for (uint32_t off = 0u; off < 0x200u; off += 4u)
                    {
                        const uint32_t addr = spDump - 0x100u + off;
                        if (addr + 4u > PS2_RAM_SIZE)
                        {
                            break;
                        }
                        uint32_t w = 0u;
                        std::memcpy(&w, m_rdram + addr, 4u);
                        if ((off & 0x1Fu) == 0u)
                        {
                            std::cerr << "\n  " << addr << ":";
                        }
                        std::cerr << " " << w;
                    }
                    std::cerr << std::dec << std::endl;
                }
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
                refreshCurrentThread();
                m_nextAttentionCycle = 0u;
            }
            continue;
        }
        if (!running->invocations.empty()) ps2WarpLoadTrace(21, "active-invocation", m_rdram, &context, &m_runtime, static_cast<uint32_t>(running->invocations.back().kind)); // TEMP(transformers)
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        ++g_eetrimStatsState.dispatchLoops; // TEMP(transformers) eetrim stats
        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            function(m_rdram, &context, &m_runtime);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            refreshCurrentThread();
            m_nextAttentionCycle = 0u;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

namespace
{
    // TEMP(transformers): PS2X_WATCH poller, moved out of checkpointDue (runs only when PS2X_WATCH is set).
    void watchPoll(EeScheduler &scheduler, const uint8_t *rdram)
    {
        if (!rdram)
        {
            return;
        }
        for (WatchSlot &slot : g_watchSlots)
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram + slot.addr, 4u);
            if (value != slot.last)
            {
                if (slot.logged < 400u)
                {
                    ++slot.logged;
                    const GuestThread *running = scheduler.currentThread();
                    std::cerr << "[watch] addr=0x" << std::hex << slot.addr << " 0x" << slot.last << " -> 0x" << value;
                    if (running)
                    {
                        const R5900Context &c = running->invocations.empty() ? running->context
                                                                              : running->invocations.back().context;
                        const uint32_t sp = getRegU32(&c, 29) & 0x1FFFFFF0u;
                        std::cerr << " thr=" << std::dec << running->id << " inv=" << running->invocations.size()
                                  << std::hex << " pc=0x" << c.pc << " ra=0x" << getRegU32(&c, 31) << " sp=0x" << sp
                                  << " a0=0x" << getRegU32(&c, 4) << " a1=0x" << getRegU32(&c, 5)
                                  << " a2=0x" << getRegU32(&c, 6) << " s0=0x" << getRegU32(&c, 16)
                                  << " s1=0x" << getRegU32(&c, 17) << " stk:";
                        for (uint32_t off = 0u; off < 0x100u && sp + off + 4u <= PS2_RAM_SIZE; off += 4u)
                        {
                            uint32_t w = 0u;
                            std::memcpy(&w, rdram + sp + off, 4u);
                            if (w >= 0x00100000u && w < 0x00670000u) // plausible code addresses (return addrs)
                            {
                                std::cerr << " " << off << "=" << w;
                            }
                        }
                    }
                    std::cerr << std::dec << " vs=" << scheduler.currentVSyncTick() << std::endl;
                }
                slot.last = value;
            }
        }
    }

    // TEMP(transformers): checkpoint rate / yield ratio probe ("[perf] 1M checkpoints"), plus the eetrim stats line.
    void perfProbeLine(uint64_t yields, uint64_t eeCycle, uint64_t vsync)
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - g_perfT0).count();
        std::cerr << "[perf] 1M checkpoints in " << ms << " ms, yields=" << (yields - g_perfLastYields)
                  << " eeCycle=" << eeCycle << " vsync=" << vsync << std::endl;
        g_perfT0 = now;
        g_perfLastYields = yields;
        if (g_eetrimStats)
        {
            EetrimStats &st = g_eetrimStatsState;
            const uint64_t tsc = __rdtsc();
            const double ticksPerMs = (st.lastTsc != 0u && ms > 0.0) ? static_cast<double>(tsc - st.lastTsc) / ms : 0.0;
            const double spinMs = ticksPerMs > 0.0 ? static_cast<double>(st.spinTicks) / ticksPerMs : 0.0;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "[eetrim] windowMs=%.1f spinMs=%.2f spinEpisodes=%llu spinCalls=%llu skippedIters=%llu "
                          "dispatchLoops=%llu ppeFast=%llu idleSkip=%d",
                          ms, spinMs, static_cast<unsigned long long>(st.spinEpisodes),
                          static_cast<unsigned long long>(st.spinCalls),
                          static_cast<unsigned long long>(st.skippedIterations),
                          static_cast<unsigned long long>(st.dispatchLoops),
                          static_cast<unsigned long long>(st.ppeFast), g_eeIdleSkip ? 1 : 0);
            std::cerr << line << std::endl;
            const uint64_t openEpisode = st.spinEpisodeStartTsc;
            st = EetrimStats{};
            st.spinEpisodeStartTsc = openEpisode;
            st.lastTsc = tsc;
        }
    }
}

bool EeScheduler::checkpointDueFull(uint32_t cycles)
{
    GuestThread *const running = cachedCurrentThread(*this, m_currentThreadId);

    // WaitVSync idle-skip (PS2X_EE_IDLESKIP, default on). When the call is the back-edge of the game's vsync
    // busy-wait (sub_0025DE90 0x25DEE8-0x25DF1C), fast-forward the k loop iterations that would each have ended in a
    // checkpointDue(32) returning false with no other effect, then handle this call normally. Exactness: the loop
    // body only re-reads RAM that changes when an event/interrupt runs (i.e. after a true return), so its registers
    // and branch outcome repeat; k is capped so that none of the skipped calls would have crossed a timer/IOP batch
    // (1024 cycles), the next deadline, the time-slice end, a pending event or a "[perf]" line. Guest cycles, COP0
    // Count at every observable point, timer/IOP batch boundaries and sizes, yields and [perf] lines are unchanged.
    bool shortAccounting = false; // TEMP(transformers) runtime fast path
    bool spinCall = false;
    if (g_idleSpinDetect && cycles == kGeneratedCheckpointCycles && running != nullptr &&
        running->activeContext().pc == kIdleSpinPc && idleSpinCodeMatches(m_rdram))
    {
        spinCall = true;
        if (g_eetrimStats)
        {
            EetrimStats &st = g_eetrimStatsState;
            ++st.spinCalls;
            if (st.spinEpisodeStartTsc == 0u)
            {
                st.spinEpisodeStartTsc = __rdtsc();
                ++st.spinEpisodes;
            }
        }
        if (g_eeIdleSkip && !g_watchOn &&
            // TEMP(transformers) qualification: PS2X_VU1_IDLESKIP_PENDING=1 lets the exact idle-spin skip run while mode-3 (guest-instant)
            // work/output is still queued (mode 3 never charges guest cycles, so the skip stays guest-exact); 0 = round 4.
            !(!g_vu1IdleSkipPending && m_runtime.memory().m_vif1Instant && m_runtime.memory().m_vif1Instant->pending()) &&
            !(m_runtime.memory().m_vif1Stream && m_runtime.memory().m_vif1Stream->pending()) &&
            !m_checkpointPending.load(std::memory_order_acquire) &&
            !m_stopRequested.load(std::memory_order_acquire))
        {
            constexpr uint64_t kStep = kGeneratedCheckpointCycles;
            const uint64_t e0 = m_eeCycle;
            uint64_t k = (1023u - std::min<uint64_t>(m_subsystemCycleDebt, 1023u)) / kStep; // debt + k*32 < 1024
            const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
            if (nextEventCycle != 0u)
            {
                k = nextEventCycle > e0 ? std::min<uint64_t>(k, (nextEventCycle - e0 - 1u) / kStep) : 0u;
            }
            k = m_sliceEndCycle > e0 ? std::min<uint64_t>(k, (m_sliceEndCycle - e0 - 1u) / kStep) : 0u;
            k = std::min<uint64_t>(k, 0xFFFFFu - (g_perfCheckpointCalls & 0xFFFFFu)); // no [perf] line inside
            if (k != 0u)
            {
                m_eeCycle = e0 + k * kStep;
                m_subsystemCycleDebt += k * kStep;
                g_perfCheckpointCalls += k;
                if (g_eetrimStats)
                {
                    g_eetrimStatsState.skippedIterations += k;
                }
            }
        }
    }

    if (((++g_perfCheckpointCalls) & 0xFFFFFu) == 0u)
    {
        perfProbeLine(m_checkpointYields, m_eeCycle, currentVSyncTick());
    }
    if (g_watchOn)
    {
        watchPoll(*this, m_rdram);
    }
    // TEMP(transformers) qualification: PS2X_VU1_CKPT_DRAIN_MIN=N drains the VU1 worker output at a checkpoint only once N events
    // are queued (default 1 = round 4: every checkpoint), so the worker's PATH1 packets merge into bigger batches; the
    // PPE/event/fence/packet drains are unchanged (host-only ordering, guest state does not depend on drain timing).
    if (m_runtime.memory().m_vif1Instant && m_runtime.memory().m_vif1Instant->queuedEvents() >= g_vu1CkptDrainMin) m_runtime.memory().m_vif1Instant->drain();
    if (m_runtime.memory().m_vif1Stream) {
        if (m_runtime.memory().m_vif1Stream->chargeCheckpoint(cycles,[this](uint32_t n){accountCycles(n);}))
            m_checkpointPending.store(true,std::memory_order_release);
    } else if (ps2EeoptEnabled() &&
               m_subsystemCycleDebt + std::max<uint64_t>(1u, cycles) < 1024u) {
        // TEMP(transformers): accountCycles also returns below 1024; deadline/slice
        // checks still run below. No subsystem callback can change running here.
        shortAccounting = true;
        const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
        m_eeCycle += elapsed;
        if (running) running->activeContext().cop0_count = static_cast<uint32_t>(m_eeCycle);
        m_subsystemCycleDebt += elapsed;
    } else accountCycles(cycles);

    bool yield = false;
    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        yield = true;
    }
    else
    {
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
        {
            m_checkpointPending.store(true, std::memory_order_release);
            yield = true;
        }
        else if (m_eeCycle < m_sliceEndCycle)
        {
            return false;
        }
        else if (const GuestThread *current = shortAccounting ? running : cachedCurrentThread(*this, m_currentThreadId);
                 current != nullptr && hasReadyAtOrAbovePriority(current->currentPriority))
        {
            m_rescheduleRequested = true;
            m_timeSliceExpired = true;
            yield = true;
        }
        else
        {
            renewTimeSlice();
            return false;
        }
    }

    ++m_checkpointYields;
    if (g_eetrimStats && g_eetrimStatsState.spinEpisodeStartTsc != 0u)
    {
        EetrimStats &st = g_eetrimStatsState;
        st.spinTicks += __rdtsc() - st.spinEpisodeStartTsc;
        st.spinEpisodeStartTsc = 0u;
    }
    return yield;
}

void EeScheduler::stallVif1Cycles(uint64_t cycles)
{
    // Dependency stalls still advance every existing event/timer in guest order.
    processPendingEvents();
    while (cycles) {
        const uint64_t next=m_nextDeadlineCycle.load(std::memory_order_acquire);
        uint64_t step=std::min<uint64_t>(cycles,0xFFFFFFFFu);
        if (next && next>m_eeCycle) step=std::min(step,next-m_eeCycle);
        const uint64_t timer=m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
        if (timer && timer!=UINT64_MAX) step=std::min(step,timer);
        accountCycles(static_cast<uint32_t>(step)); cycles-=step;
        processPendingEvents();
    }
    m_checkpointPending.store(true,std::memory_order_release);
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    // COP0 Count ticks at the EE clock; generated MFC0 $9 reads it straight from the live context,
    // so keep it current or busy-wait delays (Count - start < N) never finish.
    if (GuestThread *running = cachedCurrentThread(*this, m_currentThreadId))
    {
        running->activeContext().cop0_count = static_cast<uint32_t>(m_eeCycle);
    }
    // Timers and the IOP are advanced in batches: tight guest loops (e.g. busy-waiting on COP0
    // Count for frame pacing) hit this every 32 cycles, and a full timer + IOP scheduling pass per
    // call made such waits ~15x slower than real time. 1024 EE cycles = 128 IOP cycles (~3.5us).
    constexpr uint64_t kSubsystemBatchCycles = 1024u;
    m_subsystemCycleDebt += elapsed;
    if (m_subsystemCycleDebt < kSubsystemBatchCycles)
    {
        return;
    }
    const uint64_t batch = m_subsystemCycleDebt;
    m_subsystemCycleDebt = 0u;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(batch);
    m_runtime.advanceIopEeCycles(batch);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    noteThreadTableChanged();
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    noteThreadTableChanged();
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    refreshCurrentThread();
    m_nextAttentionCycle = 0u;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
        noteThreadTableChanged();
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        refreshCurrentThread();
        m_nextAttentionCycle = 0u;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        refreshCurrentThread();
        m_nextAttentionCycle = 0u;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
        refreshCurrentThread();
        m_nextAttentionCycle = 0u;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    // GENERIC (ported from SSX3 SSX-0032, crossover integration update): the EE kernel does not reject max_count <= 0
    // (Play!'s HLE kernel stores it unchecked; SSX3 creates its job-worker semas from a zeroed ee_sema_t, max=0
    // init=0). Rejecting it returned -1, so WaitSema(-1) never blocked and the game busy-looped. Only a negative init
    // count (or init > max when max > 0) is an error; max <= 0 is treated as "no limit" in signalSemaphore.
    if (initCount < 0 || (maxCount > 0 && initCount > maxCount))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->maxCount > 0 && object->count >= object->maxCount) // max <= 0 = no limit (SSX-0032 port)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

std::vector<GuestInvocation> EeScheduler::takePendingInvocations(GuestInvocationKind kind)
{
    assertExecutor();
    std::vector<GuestInvocation> taken;
    for (auto it = m_pendingInvocations.begin(); it != m_pendingInvocations.end();)
    {
        if (it->kind == kind)
        {
            taken.push_back(std::move(*it));
            it = m_pendingInvocations.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return taken;
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    static constexpr uint32_t kInvocationStackSize = 0x4000u;

    // Invocation (interrupt / SIF command / callback) stacks are carved top-down from the end of RAM,
    // but games commonly place the main thread stack there too (SetupThread(stack=0x1FE0000,
    // size=0x20000) in Transformers). Handlers then ran on top of the main thread's live frames and
    // clobbered saved $ra values (crash: main thread returned to pc 0x25 during the first level load).
    // Never hand out a stack range that overlaps a guest thread stack or the runtime's RPC/TLS pools.
    const auto overlapsReserved = [this](uint32_t top) -> bool
    {
        const uint32_t hi = top + 0x10u;
        const uint32_t lo = hi - kInvocationStackSize;
        // TEMP(transformers) qualification: PS2X_KERNEL_HEAP=1 moves the pools to 0x80000 (ps2_kernel_heap.h); off = 0x01F00000.
        const uint32_t kRuntimePoolsLo = ps2_kheap::poolsBase();                  // kRpcPacketPoolBase (Syscalls/Helpers/State.h)
        const uint32_t kRuntimePoolsHi = ps2_kheap::poolsBase() + ps2_kheap::kPoolsBytes; // end of kBootModePool
        if (lo < kRuntimePoolsHi && kRuntimePoolsLo < hi)
        {
            return true;
        }
        for (const auto &[id, thread] : m_threads)
        {
            (void)id;
            if (thread.stack == 0u || thread.stackSize == 0u)
            {
                continue;
            }
            const uint64_t tLo = thread.stack;
            const uint64_t tHi = static_cast<uint64_t>(thread.stack) + thread.stackSize;
            if (lo < tHi && tLo < hi)
            {
                return true;
            }
        }
        return false;
    };

    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        if (!overlapsReserved(existing->second))
        {
            return existing->second;
        }
        m_invocationStackTops.erase(existing);
    }

    uint32_t top = 0u;
    uint32_t skipped = 0u;
    while (true)
    {
        const uint32_t candidate = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
        if (candidate == 0u)
        {
            break;
        }
        if (!overlapsReserved(candidate))
        {
            top = candidate;
            break;
        }
        ++skipped;
    }
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    std::cerr << "[ee-invoke-stack] thread=" << owner->id << " depth=" << depth << " top=0x" << std::hex << top
              << std::dec << " skippedOverlapping=" << skipped << std::endl; // TEMP(transformers)
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    std::cerr << "[irq] add " << (dmac ? "DMAC" : "INTC") << " cause=" << cause << " handler=0x" << std::hex << handler
              << " arg=0x" << argument << std::dec << " id=" << id << std::endl; // TEMP
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    std::cerr << "[irq] " << (enabled ? "enable " : "disable ") << (dmac ? "DMAC" : "INTC") << " cause=" << cause << std::endl; // TEMP
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    const uint32_t dbgIndex = (dmac ? 32u : 0u) + (cause & 31u);
    ps2dbg::irqRaised[dbgIndex].fetch_add(1, std::memory_order_relaxed);
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        ps2dbg::irqMasked[dbgIndex].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        if (!dmac && cause == 2u) ps2WarpLoadTrace(1, "irq-queue", m_rdram, &invocation.context, &m_runtime, handler.handler, true); // TEMP(transformers)
        queueInvocation(std::move(invocation));
        ps2dbg::irqDelivered[dbgIndex].fetch_add(1, std::memory_order_relaxed);
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    noteThreadTableChanged();
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    refreshCurrentThread();
    m_nextAttentionCycle = 0u;
    if (Vif1Stream::enabled()) {
        auto &ctx=item.activeContext();
        ctx.vu0_vpu_stat=(ctx.vu0_vpu_stat & ~0x700u)|(m_runtime.cpu().vu0_vpu_stat & 0x700u);
    }
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    refreshCurrentThread();
    m_nextAttentionCycle = 0u;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    refreshCurrentThread();
    m_nextAttentionCycle = 0u;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    if (m_runtime.memory().m_vif1Instant) m_runtime.memory().m_vif1Instant->drain();
    else if (!Vif1Stream::enabled()) m_runtime.vu1().synchronize();
    else {
        m_runtime.memory().pollVif1Stream(m_eeCycle);
        m_runtime.drainCompletedDmacHandlers(m_rdram);
    }
    // eetrim fast path (PS2X_EE_PPE_FAST=0 disables): runs after every guest function return and at the top of every
    // dispatcher loop. When no deadline is due by cycle, no event is flagged (postEvent/requestStop set
    // m_checkpointPending under the event lock) and no EE timer interrupt is pending, the slow path below takes three
    // mutex round trips to conclude exactly that; skip them and do only its unconditional parts.
    if (g_ppeFast && m_pendingEeTimerInterrupts == 0u && !m_checkpointPending.load(std::memory_order_acquire))
    {
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle == 0u || m_eeCycle < nextEventCycle)
        {
            ++g_eetrimStatsState.ppeFast; // TEMP(transformers) eetrim stats
            if (!(m_runtime.memory().m_vif1Stream && m_runtime.memory().m_vif1Stream->pending()))
                ps2Vif1ChainResumeTick(m_runtime.memory());
            applyPendingPreemption();
            return;
        }
    }
    processDueDeadlines();
    if (!(m_runtime.memory().m_vif1Stream && m_runtime.memory().m_vif1Stream->pending()))
        ps2Vif1ChainResumeTick(m_runtime.memory()); // continue a VIF1 chain the game was still writing when it kicked it
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        bool waitedForSlot = false; // TEMP(transformers) qualification: stats only
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            // TEMP(transformers) qualification: PS2X_REALTIME=0 (scripted default) = legacy wait (else-branch below).
            // PS2X_REALTIME=1 waits precisely: condition-variable sleep to kRealtimeSpinMargin before the slot, then a
            // yield spin (the old single wait_until woke up to ~2 ms late on Windows).
            if (g_realtime && now < pacingDeadline)
            {
                waitedForSlot = true;
                bool interrupted = false;
                for (;;)
                {
                    const auto t = std::chrono::steady_clock::now();
                    if (t >= pacingDeadline)
                    {
                        break;
                    }
                    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                    {
                        interrupted = true;
                        break;
                    }
                    if (pacingDeadline - t > kRealtimeSpinMargin)
                    {
                        ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitCap); // TEMP(transformers) qualification
                        const bool woke = m_eventCv.wait_until(lock, pacingDeadline - kRealtimeSpinMargin, [this]()
                                             { return !m_events.empty() ||
                                                      m_stopRequested.load(std::memory_order_acquire); });
                        ps2_perfscope::noteTimedWait( // TEMP(transformers) qualification: oversleep of the cap's sleep
                            std::chrono::duration_cast<std::chrono::nanoseconds>(pacingDeadline - kRealtimeSpinMargin - t).count(),
                            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count(),
                            !woke);
                    }
                    else
                    {
                        // Busy-pause, not yield(): SwitchToThread hands the core to any ready thread and on a loaded
                        // host the EE thread then lost whole scheduler quanta (slot errors of 100+ ms in a test run
                        // next to a -j8 build). At most kRealtimeSpinMargin per field.
                        ps2_perfscope::Scope perfSpin(ps2_perfscope::EeWaitSpin); // TEMP(transformers) qualification
                        lock.unlock();
                        for (int spin = 0; spin < 256; ++spin)
                        {
                            _mm_pause();
                        }
                        lock.lock();
                    }
                }
                if (g_realtimeStats)
                {
                    ++g_realtimeStatsState.waits;
                    g_realtimeStatsState.waitMs +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
                }
                if (interrupted)
                {
                    updateNextDeadline();
                    return;
                }
            }
            else if (!g_realtime && now < pacingDeadline)
            {
                // v4 integration (release fix): cap OFF = the LEGACY pacing (the pre-release fix code, chained host deadlines),
                // not "never wait". The lane's unpaced mode was not deterministic on a quiet fast host: the guest ran
                // ahead of the host-side async work (MPEG decoder, IOP/CD) and saw extra vsyncs (test machine v4s4_mvp: vs=5512
                // cyc=27093128089 instead of 5366/26375494435, +20 vsyncs already in the first boot movie). All reference
                // counters were recorded with this legacy pacing.
                {
                    ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitLegacy); // TEMP(transformers) qualification
                    const bool woke = m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                         { return !m_events.empty() ||
                                                  m_stopRequested.load(std::memory_order_acquire); });
                    ps2_perfscope::noteTimedWait( // TEMP(transformers) qualification
                        std::chrono::duration_cast<std::chrono::nanoseconds>(pacingDeadline - now).count(),
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - now).count(),
                        !woke);
                }
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // TEMP(transformers) qualification: the next real-time slot is this slot + one real field, unless the
                // guest is more than PS2X_REALTIME_CATCHUP_MS late - then the schedule is pulled up to (now - window)
                // (the slow stretch is dropped; at most one window is ever repaid by running ahead). Legacy chain when the cap is off
                // (= the pre-release fix pacing that scripted runs use).
                // v4 merge with release fix: the field is the guest's own PAL-aware field (realtimeField / vblankPeriodHost).
                std::chrono::steady_clock::time_point nextSlot =
                    scheduled.hostDeadline + std::chrono::duration_cast<std::chrono::steady_clock::duration>(vblankPeriodHost());
                if (g_realtime)
                {
                    const auto field = std::chrono::duration_cast<std::chrono::steady_clock::duration>(realtimeField());
                    const auto fireNow = std::chrono::steady_clock::now();
                    const auto late = fireNow - scheduled.hostDeadline;
                    if (late > g_realtimeCatchUp)
                    {
                        // Keep the schedule exactly PS2X_REALTIME_CATCHUP_MS behind now (not at now): a light field that
                        // follows a heavy one then finds its slot already past instead of sleeping ~one field (that
                        // cost heavy scenes ~10-15 ms per 2-field game frame in bin_realtime_2: rt2_cine_on waited
                        // 4.3 s per 600 vblanks at 2.4 vs/s). The bounded debt allows at most one window of catch-up.
                        nextSlot = fireNow - std::chrono::duration_cast<std::chrono::steady_clock::duration>(g_realtimeCatchUp) + field;
                        if (g_realtimeStats)
                        {
                            ++g_realtimeStatsState.rebases;
                        }
                    }
                    else
                    {
                        nextSlot = scheduled.hostDeadline + field;
                        if (g_realtimeStats && late > std::chrono::steady_clock::duration::zero() && !waitedForSlot)
                        {
                            ++g_realtimeStatsState.catchUps;
                        }
                    }
                    if (g_realtimeStats && waitedForSlot)
                    {
                        g_realtimeStatsState.slotErrorUs.push_back(
                            static_cast<float>(std::chrono::duration<double, std::micro>(late).count()));
                    }
                }
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + vblankPeriodCycles(),
                              nextSlot,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
                if (g_realtimeStats)
                {
                    realtimeStatsVBlank(m_vsyncTick + 1u);
                }
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    if (Vif1Instant::enabled()) m_runtime.memory().m_vif1Instant->drain();
    else if (!Vif1Stream::enabled()) m_runtime.vu1().synchronize();
    else m_runtime.memory().pollVif1Stream(m_eeCycle);
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        // GAME(transformers): PC menu pause (qualification, PC_OPTIONS_DESIGN.md 6.4). While the PC menu is open the EE
        // parks here, at a guest frame boundary, so guest time (cycles, vsyncs, IOP, SPU2) stands still. On resume every
        // absolute host deadline moves by the paused time so the guest does not burst to catch up. When the menu was
        // never opened this is one relaxed atomic load: no guest-visible change.
        if (ps2x::ui::pauseRequested())
        {
            const auto paused = ps2x::ui::waitWhilePaused([this]() { return m_stopRequested.load(std::memory_order_acquire); });
            if (paused > std::chrono::steady_clock::duration::zero())
            {
                std::lock_guard lock(m_eventMutex);
                for (ScheduledEvent &item : m_deadlines)
                    item.hostDeadline += paused;
            }
        }
        ps2x::ui::g_pcAspect.store(ps2x::ui::g_pcAspectPending.load(std::memory_order_relaxed),std::memory_order_relaxed);
        ++m_vsyncTick;
        ps2_vu1new::vsync(m_vsyncTick); // TEMP(transformers)
        ps2_perfscope::onVSync(m_vsyncTick); // TEMP(transformers) qualification: host-only (vsync interval, EE core, clock)
        ps2WarpLoadTrace(0, "vblank", m_rdram, currentThread() ? &currentThread()->activeContext() : nullptr, &m_runtime, static_cast<uint32_t>(m_pendingInvocations.size())); // TEMP(transformers)

        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        m_runtime.memory().publishInstantField(m_vsyncTick);
        // TEMP(transformers): PORT_LIST rank 3. Latch the host presentation frame here, on the EE executor at guest
        // VBlankStart: processPendingEvents() only runs between guest slices (never inside a synchronous VIF1/D1
        // kick), so DISPFB and the VRAM snapshot are of one guest instant. It runs before the INTC2 /
        // sceGsSyncVCallback handlers queued below, i.e. it captures the buffer displayed during the field that just
        // ended (a flip made by those handlers shows from the next latch). PS2X_PRESENT_LATCH=0 = legacy render-thread
        // latch. Hook point ported from Scotho/socom-unzipped third_party/ps2recomp/ps2xRuntime/src/lib/Kernel/
        // EeScheduler.cpp:2201-2243 (GuestFrameBoundary() at VBlankStart on the EE executor) (GPL-3.0).
        ps2PresentLatchOnVBlank(m_runtime.gs());
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        ps2_test::onVSync(m_vsyncTick, m_eeCycle); // TEMP(transformers): PS2X_SCRIPT steps, stall / stop-at-vs exits
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    case EeEventType::CdCallback:
    {
        // GENERIC (ported from SSX3 SSX-0027/0028, crossover integration update): sceCdCallback handlers run from the CD
        // completion interrupt on hardware, so queue them Interrupt-kind: they may preempt a running thread, and an
        // iSignalSema they do wakes the waiter when the handler returns. a0 = the libcdvd function number.
        const uint32_t func = event.id;
        const uint32_t callback = static_cast<uint32_t>(event.value & 0xFFFFFFFFu);
        const uint32_t gp = static_cast<uint32_t>(event.value >> 32);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = callback;
        SET_GPR_U32(&invocation.context, 4, func);
        SET_GPR_U32(&invocation.context, 28, gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::scheduleCdCallback(uint32_t func, uint32_t callback, uint32_t gp)
{
    assertExecutor();
    EeEvent event{};
    event.type = EeEventType::CdCallback;
    event.id = func;
    event.value = static_cast<uint64_t>(callback) | (static_cast<uint64_t>(gp) << 32);
    // Short modelled completion delay (1 ms) so the game can set its in-flight flags before the handler runs.
    const uint64_t deadline = m_eeCycle + microsecondsToEeCycles(1000u);
    scheduleEvent(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1), event);
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    if (Vif1Stream::enabled() && m_runtime.memory().vif1Boundary()!=UINT64_MAX) {
        m_runtime.memory().fenceVu1();
        m_checkpointPending.store(true,std::memory_order_release);
        return;
    }
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitIdle); // TEMP(transformers) qualification
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    bool fromTimer = false; // TEMP(transformers) qualification
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
            fromTimer = true; // TEMP(transformers) qualification
        }
    }
    // TEMP(transformers) qualification: cap on = wake kRealtimeSpinMargin before a scheduled event's slot and let
    // processDueDeadlines() spin the rest precisely (EE timer deadlines keep the old plain wait; guest cycle accounting is
    // the same either way: the timeout path always advances to deadlineCycle). Cap off = the legacy plain wait until the
    // host deadline (v4 integration release fix: the lane's "no host wait" made scripted counters host-timing dependent).
    if (g_realtime && !fromTimer && hostDeadline != std::chrono::steady_clock::time_point::max())
    {
        hostDeadline -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(kRealtimeSpinMargin);
    }

    const auto perfWaitStart = std::chrono::steady_clock::now(); // TEMP(transformers) qualification
    bool signaled = false;
    {
        ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitIdle); // TEMP(transformers) qualification (the wait only)
        signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                        { return !m_events.empty() ||
                                                 m_stopRequested.load(std::memory_order_acquire); });
    }
    if (hostDeadline != std::chrono::steady_clock::time_point::max()) // TEMP(transformers) qualification
        ps2_perfscope::noteTimedWait(
            std::chrono::duration_cast<std::chrono::nanoseconds>(hostDeadline - perfWaitStart).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - perfWaitStart).count(),
            !signaled);
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    // TEMP(transformers): executor-only deadline mutation invalidates its own cache.
    m_nextAttentionCycle = 0u;
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}

// TEMP(transformers): runtime table installation is allowed before the executor starts.
void EeScheduler::assertFunctionMutationThread() const {
    assert(m_executorThread == std::thread::id{} || m_executorThread == std::this_thread::get_id());
}
GuestThread *EeScheduler::cachedCurrentThreadMember() {
    if (m_runningThreadId != m_currentThreadId || m_runningThreadGeneration != m_threadTableGeneration)
        refreshCurrentThread();
    return m_runningThread;
}
void EeScheduler::refreshCurrentThread() {
    // TEMP(transformers): a switch may install an earlier slice horizon.
    m_nextAttentionCycle = 0u;
    m_runningThread = currentThread();
    m_runningThreadId = m_currentThreadId;
    m_runningThreadGeneration = m_threadTableGeneration;
}
void EeScheduler::refreshCheckpointAttention() {
    // RC1b advances timers only at accountCycles' 1024-cycle boundary. That
    // service boundary is a conservative next-timer-fire and IOP deadline;
    // do not import upstream lazy timers or change the IOP's batch sizes.
    const uint64_t service = m_eeCycle + (1024u - std::min<uint64_t>(m_subsystemCycleDebt, 1024u));
    const uint64_t deadline = m_nextDeadlineCycle.load(std::memory_order_acquire);
    const bool drain = m_runtime.memory().m_vif1Instant &&
        m_runtime.memory().m_vif1Instant->queuedEvents() >= g_vu1CkptDrainMin;
    m_nextAttentionCycle = std::min(service, m_sliceEndCycle);
    if (deadline) m_nextAttentionCycle = std::min(m_nextAttentionCycle, deadline);
    // TEMP(transformers): each charge is at least one cycle, so this conservative
    // horizon guarantees that the diagnostic call itself takes the full path.
    m_nextAttentionCycle = std::min(m_nextAttentionCycle, m_eeCycle +
        (0x100000u - (ps2_eeopt::checkpointCalls & 0xFFFFFu)));
    if (drain || m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire)) m_nextAttentionCycle = 0u;
}
