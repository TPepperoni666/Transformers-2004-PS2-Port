// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
        uint64_t runCycles = 0; // IOP cycles executed by this thread (ReferThreadRunStatus.runClocks)
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }
        [[nodiscard]] uint64_t version() const noexcept { return m_version; }
        [[nodiscard]] bool hasReadyThread() const noexcept {
            for (const auto &[id, t] : m_threads)
                if (t.state == IopThreadState::Ready || t.state == IopThreadState::Running) return true;
            return false;
        }

        // Module _start runs on its own thread (as modload does on hardware) so it can
        // GetThreadId/SleepThread/WaitSema while waiting on CD reads or the EE.
        [[nodiscard]] int createLoaderThread(uint32_t entry, uint32_t a0, uint32_t a1, uint32_t gp,
                                             uint32_t stackSize, uint32_t priority);
        [[nodiscard]] const IopThread *findThread(int id) const;
        void deleteThread(int id);

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch (GPL-3.0)
        // WaitSema/WaitEventFlag with no current thread (an RPC server function called from the EE) cannot block a
        // thread. The emulator installs this hook: it runs the other IOP threads (as the real IOP would while the RPC
        // thread is blocked) until `ready()` holds or a time cap expires, and returns whether it holds. Without a hook
        // (or when it refuses, e.g. in interrupt context) the wait fails instead of faking success.
        using OutsideThreadWait = std::function<bool(const std::function<bool()> &)>;
        void setOutsideThreadWait(OutsideThreadWait wait) { m_outsideThreadWait = std::move(wait); }

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-runtime.patch (GPL-3.0) (hasReadyThread), narrowed:
        // true if a Ready thread has a higher priority (numerically lower) than `priority`. Used after an RPC so a
        // thread the server function woke runs before the reply only if it would preempt the RPC thread on hardware.
        [[nodiscard]] bool hasReadyThreadAbove(uint32_t priority) const noexcept
        {
            for (const auto &[id, thread] : m_threads)
            {
                if (thread.state == IopThreadState::Ready && thread.priority < priority)
                    return true;
            }
            return false;
        }

    private:
        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);
        [[nodiscard]] bool waitOutsideThread(const std::function<bool()> &ready);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        // Cache only a no-ready result, until a mutator or the earliest Delay wake-up.
        uint64_t m_version = 1;
        uint64_t m_idleVersion = 0;
        uint64_t m_idleUntil = 0;
        uint64_t m_idleFrom = 0; // also handles a caller querying an earlier cycle
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        IopThread *m_currentThread = nullptr;
        OutsideThreadWait m_outsideThreadWait; // not cleared by reset(): installed once by the emulator
    };
}
