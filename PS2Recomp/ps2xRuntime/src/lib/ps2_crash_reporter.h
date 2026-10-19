#pragma once
// Generic crash reporter (ported from the SSX3 workspace, Gemini + Claude fixes; minidump added for the Transformers
// playtest). On a fatal exception (unhandled SEH exception, stack overflow, abort(), invalid CRT parameter, pure call)
// it writes, from a fresh helper thread:
//   - stderr + <dir>/crash_<ts>.txt: code, faulting thread (+ label), module-relative RVAs of the faulting stack
//     (unwound from the exception context; resolve with the exe's .map/.pdb), registers, guest (EE) state, and a
//     recursion pattern for stack overflows;
//   - <dir>/crash_<ts>.dmp (MiniDumpWriteDump; PS2X_MINIDUMP_FULL=1 = full memory) and <dir>/crash_<ts>_rdram.bin.
// <dir> = PS2X_PLAYTEST_DIR, else PS2X_DUMP_DIR, else the working directory. No dialogs: the process then exits with
// the exception code (abort: 3). No-op on non-Windows builds.
#include <cstddef>

void InstallCrashReporter();
void RegisterThreadForCrashReporter();                  // 64 KB stack guarantee so a stack overflow can be reported
void RegisterThreadForCrashReporter(const char *label); // + a name shown in the report (static string)

// Optional hooks: a text describing the guest state (called on the helper thread; must not lock), and the guest
// RAM to save next to the dump.
using CrashGuestInfoFn = void (*)(char *buffer, size_t size);
void SetCrashReporterGuestInfo(CrashGuestInfoFn fn);
void SetCrashReporterGuestMemory(const void *base, size_t size);

// TEMP(transformers): PS2X_CRASH_TEST=av|stack|abort|bigframe on the calling thread (verifies the reporter, the
// minidump and the /STACK reserve; bigframe = touch a 4 MB stack frame and continue). No-op when unset.
void CrashReporterSelfTestIfRequested();
