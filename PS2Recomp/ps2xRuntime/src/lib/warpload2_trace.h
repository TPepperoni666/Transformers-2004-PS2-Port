// TEMP(transformers): warpload2, remove after the test machine diagnostic pass.
#pragma once
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

inline void ps2WarpLoadTrace(unsigned stage, const char *name, uint8_t *ram,
                            R5900Context *ctx, PS2Runtime *runtime,
                            uint32_t extra = 0, bool force = false)
{
    const auto vs = runtime->eeScheduler().currentVSyncTick();
    if (vs < 4300 || vs > 4800) return;
    // TEMP(transformers) qualification: PS2X_QUIET_IO=1 also drops this trace (~6,600 lines per scripted warp load; with the
    // vortex skipped it was ~19% of the load's host time). Default unchanged.
    static const bool s_quiet = []() { const char *e = std::getenv("PS2X_QUIET_IO"); return e && *e && *e != '0'; }();
    if (s_quiet) return;
    static uint64_t last[32] = {};
    if (!force && last[stage] == vs) return;
    last[stage] = vs;
    const auto read = [ram](uint32_t addr) {
        uint32_t value = 0;
        // Raw RAM only: diagnostics must not trigger MMIO or a scheduler yield.
        const uint32_t physical = addr & 0x1fffffffu;
        if (ram && physical >= 0x10000u && physical <= 0x02000000u - 4)
            std::memcpy(&value, ram + physical, 4);
        return value;
    };
    const uint32_t gp = ctx ? getRegU32(ctx, 28) : 0;
    const uint32_t state = read(0x6cf140);
    std::fprintf(stderr, "[warp2:%s] vs=%llu pc=%08x ra=%08x gp=%08x slot=%08x gpSlot=%08x state=%08x code=%08x/%x data=%08x/%x saved=%08x/%08x frame=%u progress=%x flags=%08x gates=%08x/%08x extra=%08x vpu=%08x\n",
        name, static_cast<unsigned long long>(vs), ctx ? ctx->pc : 0,
        ctx ? getRegU32(ctx, 31) : 0, gp, read(0x6cefd0), gp ? read(gp - 0x4120) : 0,
        state, read(state), read(state + 4), read(state + 8), read(state + 12),
        read(state + 16), read(state + 20), read(state + 24), read(state + 28),
        read(state + 32), read(0x6ce334), read(0x6cfcac), extra,
        ctx ? ctx->vu0_vpu_stat : 0);
}
