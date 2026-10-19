// Third-party adaptation notice: GPL-3.0; RPC budget/settling adaptation references KIexster/god-of-war-recomp.
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_cmdHandlers.clear();
        m_pendingDmaCompletion.reset();
        m_queueThreads.clear();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
    }

    bool IopRpcBridge::deliverCommand(uint32_t cid, const void *packet, uint32_t packetSize,
                                      uint32_t extraDestination, const void *extra, uint32_t extraSize,
                                      IopGuestExecutor &executor)
    {
        if (extra && extraSize != 0u && extraDestination != 0u)
        {
            (void)m_memory.writeRam(extraDestination, extra, extraSize);
        }

        const auto handler = m_cmdHandlers.find(cid);
        if (handler == m_cmdHandlers.end() || handler->second.function == 0u || !packet || packetSize == 0u)
            return false;

        const uint32_t packetAddress = m_memory.allocate(packetSize, 16u);
        if (packetAddress == 0u)
            return false;
        const bool written = m_memory.writeRam(packetAddress, packet, packetSize);
        if (written)
        {
            (void)executor.executeGuestFunction(handler->second.function, packetAddress, handler->second.data,
                                                0u, 0u, handler->second.gp);
        }
        (void)m_memory.freeAllocation(packetAddress);
        return written;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        case 32: // sceSifSetDmaIntr(dmat, count, func, data): as SetDma, then func(data) when the DMA completes
                 // (Ported from KIexster/god-of-war-recomp patches/ps2recomp-memcard.patch (GPL-3.0))
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-memcard.patch (GPL-3.0)
            // The copy above is already done; the emulator runs func(data) a little later (DMA-end interrupt).
            if (ordinal == 32u && cpu.gpr[6] != 0u)
                m_pendingDmaCompletion = PendingCompletion{cpu.gpr[6], cpu.gpr[7], cpu.gpr[28]};

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 14: // InitRpc
        case 15:
        case 16:
            setV0(0);
            return true;
        case 10: // sceSifAddCmdHandler(cid, handler, data): target of EE -> IOP sceSifSendCmd
            m_cmdHandlers[cpu.gpr[4]] = CmdHandler{cpu.gpr[5], cpu.gpr[6], cpu.gpr[28]};
            setV0(0);
            return true;
        case 11: // sceSifRemoveCmdHandler(cid)
            m_cmdHandlers.erase(cpu.gpr[4]);
            setV0(0);
            return true;
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        case 28: // sceSifSendCmdIntr: as SendCmd, plus completion callback(func, data) at sp+24/sp+28
        case 29: // isceSifSendCmdIntr
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            // No ownership check: callers usually build the packet on the stack and leave the
            // 16-byte header (size/dest/cid) for sifcmd to fill, so those bytes were never written.
            // readRam below still rejects addresses outside IOP RAM.
            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize)
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                // Bounds only: hardware DMAs whatever is in IOP RAM; an "owned" requirement made
                // replies fail whenever part of the buffer was never written (e.g. a short final read).
                const uint32_t sourcePhys = IopMemory::physicalAddress(extraSource);
                const bool sourceInRam = sourcePhys < IopMemory::RamSize && extraSize <= IopMemory::RamSize - sourcePhys;
                if (extraSource == 0u || extraDestination == 0u || !sourceInRam ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + sourcePhys, extraSize))
                {
                    std::fprintf(stderr, "[sifcmd send] FAILED cid=%u src=0x%x dst=0x%x size=0x%x inRam=%d\n",
                                 commandId, extraSource, extraDestination, extraSize, sourceInRam ? 1 : 0); // TEMP
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            if (ordinal == 28u || ordinal == 29u)
            {
                const uint32_t completion = m_memory.read32(stackPointer + 24u);
                if (completion != 0u)
                    m_pendingCompletion = PendingCompletion{completion, m_memory.read32(stackPointer + 28u), cpu.gpr[28]};
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue(qd, thread_id)
            // Remember which thread serves the queue: handleRpc uses its priority for the post-RPC settle.
            m_queueThreads[cpu.gpr[4]] = static_cast<int>(cpu.gpr[5]);
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23: // sceSifGetOtherData(rd, eeSource, iopDestination, size, mode)
        {
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch (GPL-3.0)
            // The IOP pulls a block of EE RAM into IOP RAM (used to return 0 without copying anything).
            // Transformers: imported by MCSERV only (sceMc is HLE on the EE side, so normally never reached).
            const uint32_t receiveData = cpu.gpr[4];
            const uint32_t source = cpu.gpr[5];
            const uint32_t destination = cpu.gpr[6];
            const int32_t size = static_cast<int32_t>(cpu.gpr[7]);
            if (size > 0)
            {
                if (static_cast<uint32_t>(size) > IopMemory::RamSize)
                {
                    setV0(static_cast<uint32_t>(-1));
                    return true;
                }
                std::vector<uint8_t> buffer(static_cast<size_t>(size));
                if (!m_host.readGuest(source, buffer.data(), buffer.size()) ||
                    !m_memory.writeRam(destination, buffer.data(), buffer.size()))
                {
                    setV0(static_cast<uint32_t>(-1));
                    return true;
                }
            }
            // SifRpcReceiveData_t: 16-byte SifRpcHeader_t, then src, dest, size (as sifcmd records them).
            if (receiveData != 0u)
            {
                const uint32_t fields[3] = {source, destination, static_cast<uint32_t>(size)};
                (void)m_memory.writeRam(receiveData + 0x10u, fields, sizeof(fields));
            }
            setV0(0);
            return true;
        }
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        // Copy, not a reference: the server function may now let other IOP threads run (outside-thread WaitSema /
        // WaitEventFlag, post-RPC settle) and those may register or remove RPC servers before we read it back.
        const RpcServer server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        // TEMP(transformers): opt-in Sirens RPC payload capture; includes batched bank commands.
        static const bool traceSirens = std::getenv("PS2X_SIRENS_RPC_TRACE") != nullptr;
        if (traceSirens && request.sid == 0x00514E95u)
        {
            std::ostringstream out;
            out << "[Sirens RPC] fn=" << request.function << " size=" << request.send.size
                << " entry=0x" << std::hex << server.function << " payload=";
            const uint32_t size = std::min<uint32_t>(request.send.size, 512u);
            for (uint32_t i = 0u; i < size; ++i)
            {
                const uint8_t c = m_memory.read8(server.buffer + i);
                if (c >= 32u && c < 127u) out << static_cast<char>(c);
                else out << "<" << static_cast<unsigned>(c) << ">";
            }
            m_host.log(LogLevel::Info, out.str());
        }
        // RPC server functions run directly on the EE thread. The generic 2M-instruction call budget cut SIRENS3's
        // tone-library Load (Transformers, sid 0x514E95 fn 0x19) off mid SOUNDE.ZIP read while it held its mutex.
        // GoW raised the global budget 2M -> 400M (patches/ps2recomp-runtime.patch); we raise it for RPC server calls
        // only, to 50M by default (the value play.bat and the rpcA/kick6/kick7/lvl7 runs used: zero
        // "[IOP call:unfinished]", zero "Tone not found"). PS2X_IOP_RPC_BUDGET=<instructions> still overrides it
        // (PS2X_IOP_RPC_BUDGET=2000000 restores the old behaviour). Instructions other IOP threads run while the server
        // waits outside a thread are not charged to this budget (see IopEmulator::Impl::runCpu).
        constexpr uint32_t kDefaultRpcBudget = 50'000'000u;
        static const uint32_t s_rpcBudget = []() -> uint32_t
        {
            const char *e = std::getenv("PS2X_IOP_RPC_BUDGET");
            const uint32_t value = e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
            return value != 0u ? value : kDefaultRpcBudget;
        }();
        uint32_t returnPointer = executor.executeGuestFunctionWithBudget(server.function, request.function,
                                                                         server.buffer, request.send.size, 0u,
                                                                         server.gp, s_rpcBudget);

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-runtime.patch (GPL-3.0) (runReadyThreads after each
        // RPC), narrowed to what hardware guarantees: before sifcmd sends the reply, threads the server function woke
        // run only if they outrank the RPC thread (GoW: smpd's reader thread took the next request's destination
        // otherwise). Lower-priority threads keep waiting for the normal EE-cycle lockstep, as before. Skipped when the
        // serving thread is unknown (no sceSifSetRpcQueue seen). PS2X_IOP_RPC_SETTLE=0 disables it.
        static const bool s_rpcSettle = []
        {
            const char *e = std::getenv("PS2X_IOP_RPC_SETTLE");
            return !(e && e[0] == '0');
        }();
        if (s_rpcSettle)
        {
            constexpr uint64_t kRpcSettleCycles = 4'000'000u; // GoW's cap: ~0.1 s of IOP time
            const auto queueThread = m_queueThreads.find(server.queue);
            const IopThread *rpcThread = queueThread != m_queueThreads.end() ? m_kernel.findThread(queueThread->second)
                                                                              : nullptr;
            if (rpcThread != nullptr)
                executor.runHigherPriorityThreads(rpcThread->priority, kRpcSettleCycles);
        }

        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
