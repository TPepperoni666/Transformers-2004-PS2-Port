// TEMP(transformers): unattended-test harness (design: private qualification evidence section 3.4, usage: private qualification evidence).
// Event-driven pad scripts (PS2X_SCRIPT=<file.tfs>), stall / stop-at-vsync exits and PASS/FAIL exit codes.
// NOT part of the runner PCH: never include this from ps2_runtime.h, ps2_runtime_macros.h or anything they include
// (a PCH change forces a full ~35-minute rebuild). Hook sites include this header or declare the one function they use.
#pragma once

#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace ps2_test
{
    // Process exit codes (run_test.ps1 maps them to result names).
    enum ExitCode : int
    {
        kExitPass = 0,         // script reached 'pass' / its end, or PS2X_STOP_AT_VS reached
        kExitRuntimeError = 1, // main.cpp init/exception path (not produced here)
        kExitStall = 2,        // no pad read (and no CD read) for the stall window, or guest vsync frozen on the host
        kExitFail = 3,         // 'fail', a timed-out wait / press-until, or 'watchdog fail'
        kExitTimeout = 4,      // the script's wall-clock 'budget' ran out
        kExitCrash = 5,        // exception escaped the game thread
        kExitScriptError = 6,  // parse error (exits before the game starts)
        kExitAborted = 7,      // the run ended without a verdict (window closed, game thread returned)
    };

    // The harness is active when PS2X_SCRIPT, PS2X_STOP_AT_VS or PS2X_STALL_VS is set; otherwise every hook is a no-op.
    void init(PS2Runtime &rt);                     // H4: PS2Runtime::run(), before the game thread starts
    bool ownsPad();                                 // PS2X_SCRIPT active: host input and PS2X_AUTOPRESS are ignored
    void onPadRead(uint16_t &buttonsActiveLow);     // H2: Pad.cpp, port 0 slot 0, after host input / autopress (EE thread)
    void onPadSticks(uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry); // H2b (qualification): script 'stick' values, same read
    void onVSync(uint64_t vs, uint64_t eeCycle);    // H1: EeScheduler VBlankStart (EE thread)
    void onCdRead(uint32_t lsn, uint32_t sectors);  // H3: IOP sceCdRead via g_ps2xCdReadHook (EE thread)
    void onMpeg(int kind);                          // H7: MPEG.cpp, 0 = sceMpegCreate, 1 = sceMpegDelete (EE thread)
    void onGameThreadError(const char *what);       // ps2_runtime.cpp game-thread catch blocks -> CRASH (5)
    bool takeDumpRequest(char *path, size_t cap);   // H5: render thread; pops one queued screenshot path
    int exitCode();                                 // H6: main.cpp; 0 when inactive, else the verdict (7 if none yet)
}
