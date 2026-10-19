#pragma once

#include "ps2_runtime.h"
#include "ps2_iop_host.h"
#include "ps2_perfscope.h" // TEMP(transformers): qualification
#include "ps2x/iop/iop_subsystem.h"

#include <utility>

class PS2IopTransport
{
public:
    static ps2_ee_hle::State& eeHle(PS2Runtime& runtime) { return runtime.m_iopHost->eeHle; }

    [[nodiscard]] static ps2x::iop::RpcAbi selectRpcAbi(
        const PS2Runtime *runtime,
        const ps2x::iop::RpcAbiRequest &request)
    {
        return runtime
                   ? runtime->selectIopRpcAbi(request)
                   : ps2x::iop::RpcAbi::RuntimeDefault;
    }

    [[nodiscard]] static ps2x::iop::RpcResult handleRpc(
        PS2Runtime *runtime,
        uint8_t *rdram,
        R5900Context *context,
        ps2x::iop::RpcRequest request)
    {
        return runtime
                   ? runtime->handleIopRpc(rdram, context, std::move(request))
                   : ps2x::iop::RpcResult{};
    }

    [[nodiscard]] static bool canBindRpc(const PS2Runtime *runtime, uint32_t sid)
    {
        return !runtime || runtime->canBindIopRpc(sid);
    }

    static void notifyTransfer(
        PS2Runtime *runtime,
        uint8_t *rdram,
        const ps2x::iop::SifTransfer &transfer)
    {
        if (runtime)
        {
            runtime->notifyIopSifTransfer(rdram, transfer);
        }
    }

    [[nodiscard]] static bool deliverSifCommand(
        PS2Runtime *runtime,
        uint8_t *rdram,
        uint32_t cid,
        const void *packet,
        uint32_t packetSize,
        uint32_t extraDestination,
        const void *extra,
        uint32_t extraSize)
    {
        if (!runtime || !runtime->m_iopSubsystem || !runtime->m_iopHost)
        {
            return false;
        }
        ps2_perfscope::Scope perf(ps2_perfscope::EeIopRpc); // TEMP(transformers) qualification
        auto scope = runtime->m_iopHost->enterCall(nullptr, rdram);
        return runtime->m_iopSubsystem->deliverSifCommand(cid, packet, packetSize, extraDestination, extra, extraSize);
    }

    // sceSifAllocSysMemory(mode, ...): mode 1 (ALLOC_LAST) allocates from the top of IOP RAM.
    [[nodiscard]] static uint32_t allocateIopMemory(PS2Runtime *runtime, uint32_t mode, uint32_t size, uint32_t alignment)
    {
        if (!runtime || !runtime->m_iopSubsystem)
        {
            return 0u;
        }
        return mode == 1u ? runtime->m_iopSubsystem->allocateMemoryLast(size, alignment)
                          : runtime->m_iopSubsystem->allocateMemory(size, alignment);
    }

    // Audio callback is the sole consumer while enabled; otherwise render drains/discards.
    // The subsystem remains alive throughout run(), including callback teardown.
    static size_t drainAudio(PS2Runtime *runtime, int16_t *stereo, size_t frames) noexcept
    {
        return runtime && runtime->m_iopSubsystem
                   ? runtime->m_iopSubsystem->drainSamples(stereo, frames)
                   : 0u;
    }

    // TEMP(transformers): successful script verdict runs on the EE/IOP producer thread.
    // After run() joins that thread this is also safe for incomplete capture shutdown.
    static void finishAudioCapture(PS2Runtime *runtime)
    {
        if (runtime && runtime->m_iopSubsystem) runtime->m_iopSubsystem->finishAudioCapture();
    }

    static void reset(PS2Runtime *runtime)
    {
        if (runtime)
        {
            runtime->resetIop();
        }
    }
};
