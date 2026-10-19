#pragma once
// TEMP(transformers): qualification - pollable sprint telemetry. PS2X_SPRINT_LOG=1 wraps sub_00476700, Transformers' robot
// target-speed pick (called every game frame by the Walk / Run states, a1 = the transformer):
//   |move| = |actor+0x190| = left stick after the game's 0.2 per-axis dead zone, ControllerDeadZone + cubic response and
//            camera yaw (sub_0047DBA0); the pick (f0) = WalkSpeed (desc +0x120) if |move| > 0, MaxRunSpeed (desc +0x124)
//            if |move| > 0.99 (float gp-0x7904), times the actor's speed scale (+0x1fc); actor+0x44 = current speed.
// Logs every change of the picked class (idle / walk / RUN) with those numbers. Read-only (never writes guest state);
// not installed at all unless PS2X_SPRINT_LOG=1. Included only by ps2_runtime.cpp, after ps2_runtime.h and
// ps2_runtime_macros.h (READ32 / GPR_U32). No PCH change.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace TfSprintProbe
{
    inline PS2Runtime::RecompiledFunction orig = nullptr;
    inline int lastClass = -1;
    inline uint32_t lastActor = 0;
    inline unsigned long long calls = 0, runPicks = 0, walkPicks = 0;

    inline float rdF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a)
    {
        const uint32_t w = READ32(a);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }

    inline void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint32_t sp = GPR_U32(ctx, 29);
        const uint32_t actor = GPR_U32(ctx, 5);
        orig(rdram, ctx, runtime);
        if (ctx->pc != ra || GPR_U32(ctx, 29) != sp || actor < 0x100000u || actor >= 0x2000000u)
            return; // a guest yield/exception mid-call is not a completed return
        const float target = ctx->f[0];
        const float mx = rdF(rdram, ctx, runtime, actor + 0x190u), my = rdF(rdram, ctx, runtime, actor + 0x194u),
                    mz = rdF(rdram, ctx, runtime, actor + 0x198u);
        const float move = std::sqrt(mx * mx + my * my + mz * mz);
        const float speed = rdF(rdram, ctx, runtime, actor + 0x44u);
        const float scale = rdF(rdram, ctx, runtime, actor + 0x1fcu);
        const float stickX = rdF(rdram, ctx, runtime, actor + 0x180u), stickY = rdF(rdram, ctx, runtime, actor + 0x184u);
        const int cls = move > 0.99f ? 2 : (move > 0.0f ? 1 : 0);
        ++calls;
        runPicks += cls == 2 ? 1u : 0u;
        walkPicks += cls == 1 ? 1u : 0u;
        if (cls != lastClass || actor != lastActor)
        {
            lastClass = cls;
            lastActor = actor;
            std::fprintf(stderr,
                         "[sprint] pick %s |move|=%.5f stick=(%+.4f,%+.4f) target=%.3f speed=%.3f scale=%.2f actor=%08x "
                         "(calls=%llu run=%llu walk=%llu)\n",
                         cls == 2 ? "RUN " : (cls == 1 ? "walk" : "idle"), move, stickX, stickY, target, speed, scale, actor,
                         calls, runPicks, walkPicks);
        }
    }

    inline void install(PS2Runtime &rt)
    {
        const char *e = std::getenv("PS2X_SPRINT_LOG");
        if (!(e && e[0] == '1') || !rt.hasFunction(0x00476700u))
            return;
        orig = rt.lookupFunction(0x00476700u);
        if (orig && orig != &hook)
        {
            rt.replaceFunction(0x00476700u, &hook);
            std::fprintf(stderr, "[sprint] PS2X_SPRINT_LOG=1: sub_00476700 (robot target speed) wrapped, read-only\n");
        }
    }
}
