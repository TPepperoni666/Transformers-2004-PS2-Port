#pragma once
// TEMP(transformers): qualification - Windows scheduling hints for the game (EE) thread. Host-only; guest timing and the
// test counters do not depend on them.
//  - process: opt out of power throttling (EcoQoS) for execution speed AND for timer resolution
//    (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION: Windows 11 otherwise may ignore the 1 ms timer request of a
//    process whose window is minimised/occluded, and sleeps fall back to 15.6 ms).            PS2X_POWER_OPTOUT=0 = off
//    (2 = execution speed only, 3 = timer resolution only: attribution experiments). Measured on the laptop with the
//    game window not in the foreground: Windows throttles the process without it (~-25% fps, worse 1%/0.1% lows).
//  - EE thread: THREAD_PRIORITY_ABOVE_NORMAL (PS2X_EE_PRIORITY=0 leave, 1 above normal = default with >= 6 logical
//    CPUs, 2 highest), thread-level EcoQoS opt-out, optional hard affinity PS2X_EE_AFFINITY=<hex mask> (experiments only).
//  - VU1 worker thread (v0.7.0, PS2X_VU1_THREAD=3): same priority as the EE thread + thread EcoQoS opt-out
//    (PS2X_VU1W_PRIORITY=0|1|2 overrides the priority; default follows PS2X_EE_PRIORITY).
// One "[eeqos]" line per thread reports what was applied.
namespace ps2_eeqos
{
    void tuneProcess();   // PS2Runtime::run, main thread
    void tuneEeThread();  // first thing on the game (EE) thread
    void tuneVu1Worker(); // first thing on the VU1 worker thread (ps2Vu1PerfWorkerStart, ps2_perfscope.cpp)
}
