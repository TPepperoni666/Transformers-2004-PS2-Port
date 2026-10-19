// TEMP(transformers): qualification - scheduling hints for the EE thread (see ps2_eeqos.h).
#include "ps2_eeqos.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2_eeqos
{
#if defined(_WIN32)
    namespace
    {
        // Older SDK headers may lack these (values from the Windows 11 SDK, processthreadsapi.h).
        constexpr ULONG kPowerThrottlingVersion = 1;            // PROCESS/THREAD_POWER_THROTTLING_CURRENT_VERSION
        constexpr ULONG kExecutionSpeed = 0x1;                  // *_POWER_THROTTLING_EXECUTION_SPEED
        constexpr ULONG kIgnoreTimerResolution = 0x4;           // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        struct PowerThrottlingState
        {
            ULONG Version, ControlMask, StateMask;
        };

        // GetProcAddress keeps the exe loadable on systems/Wine builds without these exports.
        template <class F>
        F kernelProc(const char *name)
        {
            HMODULE k = GetModuleHandleW(L"kernel32.dll");
            return k ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(k, name))) : nullptr;
        }

        // PS2X_POWER_OPTOUT: 0 = off, 1 = execution speed + timer resolution (default), 2 = execution speed only,
        // 3 = timer resolution only (2/3 = attribution experiments).
        int powerMode()
        {
            const char *v = std::getenv("PS2X_POWER_OPTOUT");
            if (!v || !*v)
                return 1;
            const int m = std::atoi(v);
            return (m >= 0 && m <= 3) ? m : 1;
        }
    }

    void tuneProcess()
    {
        const int mode = powerMode();
        if (mode == 0)
        {
            std::fprintf(stderr, "[eeqos] process power-throttling opt-out skipped (PS2X_POWER_OPTOUT=0)\n");
            return;
        }
        const bool wantSpeed = mode != 3, wantTimer = mode != 2;
        using SetProcInfo = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        auto setProcessInformation = kernelProc<SetProcInfo>("SetProcessInformation");
        BOOL okSpeed = FALSE, okTimer = FALSE;
        if (setProcessInformation)
        {
            constexpr int kProcessPowerThrottling = 4; // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
            const ULONG mask = (wantSpeed ? kExecutionSpeed : 0u) | (wantTimer ? kIgnoreTimerResolution : 0u);
            PowerThrottlingState s{kPowerThrottlingVersion, mask, 0};
            const BOOL ok = setProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof(s));
            okSpeed = ok && wantSpeed;
            okTimer = ok && wantTimer;
            if (!ok && wantSpeed && wantTimer) // pre-22H2 Windows rejects the timer bit: retry with execution speed only
            {
                s.ControlMask = kExecutionSpeed;
                okSpeed = setProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof(s));
            }
        }
        std::fprintf(stderr, "[eeqos] process power throttling: execution-speed opt-out %s, timer-resolution opt-out %s "
                             "(PS2X_POWER_OPTOUT=0 disables, 2 = speed only, 3 = timer only)\n",
                     okSpeed ? "on" : (wantSpeed ? "unavailable" : "off"), okTimer ? "on" : (wantTimer ? "unavailable" : "off"));
    }

    void tuneEeThread()
    {
        HANDLE self = GetCurrentThread();
        const unsigned cpus = std::thread::hardware_concurrency();
        int mode = cpus >= 6u ? 1 : 0;
        if (const char *v = std::getenv("PS2X_EE_PRIORITY"); v && *v)
            mode = std::atoi(v);
        BOOL okPrio = TRUE;
        if (mode == 1)
            okPrio = SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL);
        else if (mode >= 2)
            okPrio = SetThreadPriority(self, THREAD_PRIORITY_HIGHEST);

        BOOL okQos = FALSE;
        if (const int pm = powerMode(); pm == 1 || pm == 2) // thread-level EcoQoS opt-out goes with the speed bit
        {
            using SetThreadInfo = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
            if (auto setThreadInformation = kernelProc<SetThreadInfo>("SetThreadInformation"))
            {
                constexpr int kThreadPowerThrottling = 3; // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
                PowerThrottlingState s{kPowerThrottlingVersion, kExecutionSpeed, 0};
                okQos = setThreadInformation(self, kThreadPowerThrottling, &s, sizeof(s));
            }
        }

        unsigned long long mask = 0;
        BOOL okAff = TRUE;
        if (const char *v = std::getenv("PS2X_EE_AFFINITY"); v && *v)
        {
            mask = std::strtoull(v, nullptr, 16);
            if (mask)
                okAff = SetThreadAffinityMask(self, static_cast<DWORD_PTR>(mask)) != 0;
        }
        std::fprintf(stderr, "[eeqos] EE thread: priority %s%s (now %d), EcoQoS opt-out %s, affinity %s0x%llx "
                             "(PS2X_EE_PRIORITY=0|1|2, PS2X_EE_AFFINITY=<hex>; %u logical CPUs)\n",
                     mode == 0 ? "unchanged" : mode == 1 ? "above normal" : "highest", okPrio ? "" : " FAILED",
                     GetThreadPriority(self), okQos ? "on" : "unavailable", mask ? (okAff ? "" : "FAILED ") : "default ",
                     mask, cpus);
    }

    // v0.7.0 rebase: the VU1 worker thread (PS2X_VU1_THREAD=3) is the second hot thread - the EE waits for it at every
    // VU1 fence, so it gets the same hints as the EE thread (priority + thread EcoQoS opt-out). PS2X_VU1W_PRIORITY=0|1|2
    // overrides the priority (default = the EE thread's mode); the EcoQoS bit follows PS2X_POWER_OPTOUT as for the EE.
    void tuneVu1Worker()
    {
        HANDLE self = GetCurrentThread();
        const unsigned cpus = std::thread::hardware_concurrency();
        int mode = cpus >= 6u ? 1 : 0;
        if (const char *v = std::getenv("PS2X_EE_PRIORITY"); v && *v)
            mode = std::atoi(v);
        if (const char *v = std::getenv("PS2X_VU1W_PRIORITY"); v && *v)
            mode = std::atoi(v);
        BOOL okPrio = TRUE;
        if (mode == 1)
            okPrio = SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL);
        else if (mode >= 2)
            okPrio = SetThreadPriority(self, THREAD_PRIORITY_HIGHEST);
        BOOL okQos = FALSE;
        if (const int pm = powerMode(); pm == 1 || pm == 2)
        {
            using SetThreadInfo = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
            if (auto setThreadInformation = kernelProc<SetThreadInfo>("SetThreadInformation"))
            {
                constexpr int kThreadPowerThrottling = 3; // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
                PowerThrottlingState s{kPowerThrottlingVersion, kExecutionSpeed, 0};
                okQos = setThreadInformation(self, kThreadPowerThrottling, &s, sizeof(s));
            }
        }
        std::fprintf(stderr, "[eeqos] VU1 worker thread: priority %s%s (now %d), EcoQoS opt-out %s (PS2X_VU1W_PRIORITY=0|1|2)\n",
                     mode == 0 ? "unchanged" : mode == 1 ? "above normal" : "highest", okPrio ? "" : " FAILED",
                     GetThreadPriority(self), okQos ? "on" : "unavailable");
    }
#else
    void tuneProcess() {}
    void tuneEeThread() {}
    void tuneVu1Worker() {}
#endif
}
