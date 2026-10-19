#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
        // True while a guest interrupt handler / scheduled completion callback runs (intrman QueryIntrContext).
        [[nodiscard]] virtual bool inInterruptContext() const { return false; }
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-runtime.patch (GPL-3.0) (runReadyThreads), narrowed:
        // run Ready IOP threads whose priority is higher (numerically lower) than `priority` for at most `maxCycles`
        // IOP cycles. Called after an RPC server function returns and before its reply is copied: on hardware a thread
        // the server woke that outranks the RPC thread preempts it before sifcmd sends the reply.
        virtual void runHigherPriorityThreads(uint32_t priority, uint64_t maxCycles)
        {
            (void)priority;
            (void)maxCycles;
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

        // EE -> IOP sceSifSendCmd: copy the extra payload and packet into IOP RAM and run the
        // handler the IOP registered for cid (as the SIF interrupt would). False if none is registered.
        [[nodiscard]] bool deliverCommand(uint32_t cid, const void *packet, uint32_t packetSize,
                                          uint32_t extraDestination, const void *extra, uint32_t extraSize,
                                          IopGuestExecutor &executor);

        // Completion callback requested by the last sceSifSendCmdIntr; the emulator schedules it.
        struct PendingCompletion
        {
            uint32_t function = 0;
            uint32_t data = 0;
            uint32_t gp = 0;
        };
        [[nodiscard]] std::optional<PendingCompletion> takeCompletionCallback()
        {
            std::optional<PendingCompletion> taken = m_pendingCompletion;
            m_pendingCompletion.reset();
            return taken;
        }

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-memcard.patch (GPL-3.0)
        // Completion callback func(data) requested by the last sifman sceSifSetDmaIntr; the emulator schedules it.
        [[nodiscard]] std::optional<PendingCompletion> takeDmaCompletionCallback()
        {
            std::optional<PendingCompletion> taken = m_pendingDmaCompletion;
            m_pendingDmaCompletion.reset();
            return taken;
        }

    private:
        struct CmdHandler
        {
            uint32_t function = 0;
            uint32_t data = 0;
            uint32_t gp = 0;
        };
        std::unordered_map<uint32_t, CmdHandler> m_cmdHandlers;
        std::optional<PendingCompletion> m_pendingCompletion;
        std::optional<PendingCompletion> m_pendingDmaCompletion;
        // sceSifSetRpcQueue(qd, thread_id): the thread that serves the queue (its priority bounds the post-RPC settle).
        std::unordered_map<uint32_t, int> m_queueThreads;

        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
