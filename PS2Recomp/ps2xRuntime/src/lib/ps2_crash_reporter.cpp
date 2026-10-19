// Generic crash reporter, see ps2_crash_reporter.h. Ported from the SSX3 workspace
// (shared crash-reporting implementation; fixes:
// proper 64-bit formatting, module-relative RVAs, include/extern placement) and extended for the Transformers
// playtest: work happens on a fresh helper thread (the faulting thread may have no stack left), the stack is unwound
// from the exception CONTEXT (not the handler's own stack), a minidump + guest RDRAM are written, and the process
// then exits without a WER dialog.
#include "ps2_crash_reporter.h"

#if defined(_WIN32) && !defined(PLATFORM_VITA)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

void ps2HostRumbleSet(float lowFrequency, float highFrequency); // ps2_host_rumble.cpp

namespace
{
    constexpr int kMaxFrames = 64;
    constexpr size_t kReportSize = 48 * 1024;

    struct ThreadLabel
    {
        std::atomic<DWORD> tid{0};
        const char *label = nullptr;
    };
    ThreadLabel g_threadLabels[64];
    std::atomic<int> g_threadLabelCount{0};

    std::atomic<CrashGuestInfoFn> g_guestInfo{nullptr};
    std::atomic<const void *> g_guestMem{nullptr};
    std::atomic<size_t> g_guestMemSize{0};

    std::atomic<int> g_crashState{0}; // 0 idle, 1 reporting, 2 done
    PVOID g_veh = nullptr;
    bool g_installed = false;

    struct CrashJob
    {
        const char *what = nullptr;
        DWORD code = 0;
        DWORD threadId = 0;
        EXCEPTION_RECORD record{};
        bool hasRecord = false;
        CONTEXT context{};
        EXCEPTION_POINTERS pointers{};
        EXCEPTION_POINTERS *originalPointers = nullptr;
    };
    CrashJob g_job;
    char g_report[kReportSize];
    size_t g_reportLen = 0;
    char g_stamp[32];
    char g_dir[MAX_PATH];

    void appendf(const char *fmt, ...)
    {
        if (g_reportLen >= kReportSize - 1)
            return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(g_report + g_reportLen, kReportSize - g_reportLen, fmt, ap);
        va_end(ap);
        if (n > 0)
            g_reportLen = std::min(kReportSize - 1, g_reportLen + static_cast<size_t>(n));
    }

    void writeStderr(const char *text, size_t len)
    {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, text, static_cast<DWORD>(len), &written, nullptr);
        }
    }

    const char *codeName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case 0xE06D7363u: return "C++ EXCEPTION (unhandled)";
        case 0xC0000409u: return "STACK_BUFFER_OVERRUN / fail-fast";
        case 0xC0000417u: return "INVALID_CRUNTIME_PARAMETER";
        case 0xC0000374u: return "HEAP_CORRUPTION";
        default: return "?";
        }
    }

    const char *threadLabel(DWORD tid)
    {
        const int n = std::min(g_threadLabelCount.load(), 64);
        for (int i = 0; i < n; ++i)
            if (g_threadLabels[i].tid.load() == tid)
                return g_threadLabels[i].label ? g_threadLabels[i].label : "";
        return "";
    }

    // "module.dll+0xRVA" for any address (module-relative RVAs resolve against that module's .map/.pdb).
    void describeAddress(DWORD64 addr, char *out, size_t size)
    {
        HMODULE mod = nullptr;
        if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(addr), &mod) && mod)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char *base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            std::snprintf(out, size, "%s+0x%llx", base,
                          static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(mod)));
        }
        else
            std::snprintf(out, size, "?+0x%016llx", static_cast<unsigned long long>(addr));
    }

    // Unwinds the faulting thread's stack from its CONTEXT with the x64 unwind tables (no dbghelp needed).
    // POD locals only (SEH __try below).
    int walkStack(const CONTEXT *start, DWORD64 *frames, int maxFrames)
    {
        CONTEXT ctx = *start;
        int count = 0;
        __try
        {
            while (count < maxFrames && ctx.Rip)
            {
                frames[count++] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    // Leaf function without unwind data: return address is at [rsp].
                    ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
                else
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    void recursionAnalysis(const DWORD64 *frames, int count)
    {
        int bestRepeats = 0, bestLen = 0, bestStart = 0;
        for (int len = 1; len <= count / 2; ++len)
            for (int start = 0; start + len * 2 <= count; ++start)
            {
                int repeats = 1;
                while (start + (repeats + 1) * len <= count)
                {
                    bool match = true;
                    for (int k = 0; k < len && match; ++k)
                        match = frames[start + k] == frames[start + repeats * len + k];
                    if (!match)
                        break;
                    ++repeats;
                }
                if (repeats > 1 && repeats * len > bestRepeats * bestLen)
                {
                    bestRepeats = repeats;
                    bestLen = len;
                    bestStart = start;
                }
            }
        if (bestRepeats > 1)
        {
            appendf("Recursion: a %d-frame pattern repeats %d times (within the first %d frames):\n", bestLen, bestRepeats, count);
            for (int k = 0; k < bestLen; ++k)
            {
                char where[MAX_PATH + 32];
                describeAddress(frames[bestStart + k], where, sizeof(where));
                appendf("    %s\n", where);
            }
        }
        else
            appendf("Recursion: no repeating pattern in the first %d frames (one huge frame? see the first frames)\n", count);
    }

    void buildPath(char *out, size_t size, const char *suffix)
    {
        std::snprintf(out, size, "%s\\crash_%s%s", g_dir, g_stamp, suffix);
    }

    void writeFile(const char *path, const void *data, size_t len)
    {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return;
        const char *p = static_cast<const char *>(data);
        while (len > 0)
        {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 16u << 20)), written = 0;
            if (!WriteFile(f, p, chunk, &written, nullptr) || written == 0)
                break;
            p += written;
            len -= written;
        }
        CloseHandle(f);
    }

    void appendFile(const char *path, const char *data, size_t len)
    {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(f, data, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(f);
    }

    DWORD WINAPI crashWorker(LPVOID)
    {
        CrashJob &job = g_job;
        DWORD64 frames[kMaxFrames];
        const int count = walkStack(&job.context, frames, kMaxFrames);

        g_reportLen = 0;
        appendf("\n=== FATAL: %s - code 0x%08lX (%s) at %s ===\n", job.what ? job.what : "exception",
                static_cast<unsigned long>(job.code), codeName(job.code), g_stamp);
        char where[MAX_PATH + 32];
        describeAddress(job.context.Rip, where, sizeof(where));
        appendf("Fault at %s (0x%016llx), thread %lu '%s'\n", where, static_cast<unsigned long long>(job.context.Rip),
                static_cast<unsigned long>(job.threadId), threadLabel(job.threadId));
        if (job.hasRecord && job.code == EXCEPTION_ACCESS_VIOLATION && job.record.NumberParameters >= 2)
        {
            const ULONG_PTR kind = job.record.ExceptionInformation[0];
            appendf("Access violation: %s address 0x%016llx\n", kind == 0 ? "READ" : kind == 1 ? "WRITE" : kind == 8 ? "EXECUTE (DEP)" : "?",
                    static_cast<unsigned long long>(job.record.ExceptionInformation[1]));
        }
        if (CrashGuestInfoFn fn = g_guestInfo.load())
        {
            char guest[1024] = {};
            fn(guest, sizeof(guest));
            appendf("Guest: %s\n", guest);
        }
        const CONTEXT &c = job.context;
        appendf("Registers: rip=%016llx rsp=%016llx rbp=%016llx\n"
                "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
                "  rsi=%016llx rdi=%016llx r8 =%016llx r9 =%016llx\n"
                "  r10=%016llx r11=%016llx r12=%016llx r13=%016llx\n"
                "  r14=%016llx r15=%016llx\n",
                (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rbp,
                (unsigned long long)c.Rax, (unsigned long long)c.Rbx, (unsigned long long)c.Rcx, (unsigned long long)c.Rdx,
                (unsigned long long)c.Rsi, (unsigned long long)c.Rdi, (unsigned long long)c.R8, (unsigned long long)c.R9,
                (unsigned long long)c.R10, (unsigned long long)c.R11, (unsigned long long)c.R12, (unsigned long long)c.R13,
                (unsigned long long)c.R14, (unsigned long long)c.R15);
        appendf("Stack (%d frames, unwound from the fault; resolve RVAs with ps2EntryRunner.map/.pdb):\n", count);
        for (int i = 0; i < count; ++i)
        {
            describeAddress(frames[i], where, sizeof(where));
            appendf("  [%02d] %s\n", i, where);
        }
        if (job.code == EXCEPTION_STACK_OVERFLOW)
            recursionAnalysis(frames, count);

        char path[MAX_PATH + 64];
        buildPath(path, sizeof(path), ".dmp");
        appendf("Minidump: %s\n", path);
        writeStderr(g_report, g_reportLen);
        char txt[MAX_PATH + 64];
        buildPath(txt, sizeof(txt), ".txt");
        writeFile(txt, g_report, g_reportLen);

        // Minidump (dbghelp, linked by ps2_runtime).
        HANDLE dump = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE)
        {
            const char *full = std::getenv("PS2X_MINIDUMP_FULL");
            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                (full && *full == '1') ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                          MiniDumpWithUnloadedModules)
                                       : (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                          MiniDumpWithUnloadedModules | MiniDumpWithHandleData |
                                          MiniDumpWithProcessThreadData));
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = job.threadId;
            mei.ExceptionPointers = &job.pointers;
            mei.ClientPointers = FALSE;
            const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type, &mei, nullptr, nullptr);
            CloseHandle(dump);
            char msg[MAX_PATH + 96];
            const int n = std::snprintf(msg, sizeof(msg), "Minidump %s (error %lu)\n", ok ? "written" : "FAILED",
                                        ok ? 0ul : static_cast<unsigned long>(GetLastError()));
            writeStderr(msg, n);
            appendFile(txt, msg, n);
        }

        // Guest RAM (32 MB) next to the dump: lets the guest state be inspected with the usual offline tools.
        if (const void *mem = g_guestMem.load())
        {
            char rd[MAX_PATH + 64];
            buildPath(rd, sizeof(rd), "_rdram.bin");
            writeFile(rd, mem, g_guestMemSize.load());
        }

        // Best-effort symbol names (needs ps2EntryRunner.pdb next to the exe; the .map always works offline).
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE); // may already be initialised by the profiler: fine
        g_reportLen = 0;
        appendf("Symbols:\n");
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
        for (int i = 0; i < count && i < 24; ++i)
        {
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
            std::memset(symBuf, 0, sizeof(symBuf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), frames[i], &disp, sym))
                appendf("  [%02d] %s+0x%llx\n", i, sym->Name, static_cast<unsigned long long>(disp));
            else
                appendf("  [%02d] ?\n", i);
        }
        appendf("=== end of crash report ===\n");
        writeStderr(g_report, g_reportLen);
        appendFile(txt, g_report, g_reportLen);
        ps2HostRumbleSet(0.0f, 0.0f);
        return 0;
    }

    // Runs the report once (other threads crashing meanwhile wait for it). Returns when the report is complete.
    void handleFatal(const char *what, DWORD code, const EXCEPTION_RECORD *record, const CONTEXT *context)
    {
        int expected = 0;
        if (!g_crashState.compare_exchange_strong(expected, 1))
        {
            for (int i = 0; i < 1200 && g_crashState.load() != 2; ++i)
                Sleep(100);
            return;
        }
        g_job.what = what;
        g_job.code = code;
        g_job.threadId = GetCurrentThreadId();
        g_job.hasRecord = record != nullptr;
        if (record)
            g_job.record = *record;
        else
        {
            g_job.record = EXCEPTION_RECORD{};
            g_job.record.ExceptionCode = code;
        }
        g_job.context = *context;
        g_job.record.ExceptionAddress = reinterpret_cast<PVOID>(g_job.context.Rip);
        g_job.pointers.ExceptionRecord = &g_job.record;
        g_job.pointers.ContextRecord = &g_job.context;

        SYSTEMTIME st;
        GetLocalTime(&st);
        std::snprintf(g_stamp, sizeof(g_stamp), "%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);

        HANDLE worker = CreateThread(nullptr, 1u << 20, crashWorker, nullptr, 0, nullptr);
        if (worker)
        {
            WaitForSingleObject(worker, 120000);
            CloseHandle(worker);
        }
        else
        {
            static const char msg[] = "\n=== FATAL: crash reporter could not start its helper thread ===\n";
            writeStderr(msg, sizeof(msg) - 1);
        }
        _fflush_nolock(stdout); // never take the stdio lock here: the crashed thread may hold it
        _fflush_nolock(stderr);
        g_crashState.store(2);
    }

    LONG WINAPI vectoredHandler(PEXCEPTION_POINTERS ep)
    {
        // First-chance: only a stack overflow is handled here (the unhandled-exception filter may not get enough
        // stack). Everything else waits for the unhandled filter, so exceptions that something handles never report.
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
            handleFatal("stack overflow", EXCEPTION_STACK_OVERFLOW, ep->ExceptionRecord, ep->ContextRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI unhandledFilter(PEXCEPTION_POINTERS ep)
    {
        if (ep && ep->ExceptionRecord && ep->ContextRecord)
            handleFatal("unhandled exception", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord, ep->ContextRecord);
        return EXCEPTION_EXECUTE_HANDLER; // terminate with the exception code, no WER dialog
    }

    void reportHere(const char *what, DWORD code)
    {
        CONTEXT ctx{};
        RtlCaptureContext(&ctx);
        handleFatal(what, code, nullptr, &ctx);
    }

    void onAbortSignal(int)
    {
        reportHere("abort() (std::terminate / assert / CRT abort)", 3);
    }

    void onInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
    {
        reportHere("invalid CRT parameter", 0xC0000417u);
        TerminateProcess(GetCurrentProcess(), 0xC0000417u);
    }

    void onPureCall()
    {
        reportHere("pure virtual call", 0xC0000025u);
        TerminateProcess(GetCurrentProcess(), 0xC0000025u);
    }

    void resolveDir()
    {
        const char *dir = nullptr;
        for (const char *var : {"PS2X_PLAYTEST_DIR", "PS2X_DUMP_DIR"})
            if (const char *v = std::getenv(var); v && *v)
            {
                dir = v;
                break;
            }
        if (dir)
        {
            std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
            CreateDirectoryA(g_dir, nullptr);
        }
        else if (!GetCurrentDirectoryA(sizeof(g_dir), g_dir))
            std::snprintf(g_dir, sizeof(g_dir), ".");
        size_t n = std::strlen(g_dir);
        while (n > 0 && (g_dir[n - 1] == '\\' || g_dir[n - 1] == '/'))
            g_dir[--n] = '\0';
    }
}

void InstallCrashReporter()
{
    if (g_installed)
        return;
    g_installed = true;
    resolveDir();
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    g_veh = AddVectoredExceptionHandler(1, vectoredHandler);
    SetUnhandledExceptionFilter(unhandledFilter);
    std::signal(SIGABRT, onAbortSignal);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    char msg[MAX_PATH + 64];
    const int n = std::snprintf(msg, sizeof(msg), "[crash] reporter installed, dumps -> %s\n", g_dir);
    writeStderr(msg, n);
}

void RegisterThreadForCrashReporter()
{
    RegisterThreadForCrashReporter(nullptr);
}

void RegisterThreadForCrashReporter(const char *label)
{
    ULONG size = 64 * 1024;
    SetThreadStackGuarantee(&size);
    const int i = g_threadLabelCount.fetch_add(1);
    if (i < 64)
    {
        g_threadLabels[i].label = label;
        g_threadLabels[i].tid.store(GetCurrentThreadId());
    }
}

namespace
{
    __declspec(noinline) int crashTestRecurse(volatile int depth)
    {
        volatile char pad[4096];
        pad[0] = static_cast<char>(depth);
        return crashTestRecurse(depth + 1) + pad[0];
    }

    __declspec(noinline) int crashTestBigFrame()
    {
        volatile char big[4u << 20]; // 4 MB: overflows a 1 MB reserve, fits the 16 MB /STACK reserve
        for (size_t i = 0; i < sizeof(big); i += 4096)
            big[i] = static_cast<char>(i);
        return big[12345];
    }
}

void CrashReporterSelfTestIfRequested()
{
    const char *t = std::getenv("PS2X_CRASH_TEST");
    if (!t || !*t)
        return;
    char msg[128];
    const int n = std::snprintf(msg, sizeof(msg), "[crash] PS2X_CRASH_TEST=%s on thread %lu\n", t,
                                static_cast<unsigned long>(GetCurrentThreadId()));
    writeStderr(msg, n);
    if (std::strcmp(t, "av") == 0)
        *reinterpret_cast<volatile int *>(static_cast<uintptr_t>(0x10)) = 1;
    else if (std::strcmp(t, "stack") == 0)
        crashTestRecurse(0);
    else if (std::strcmp(t, "abort") == 0)
        std::abort();
    else if (std::strcmp(t, "bigframe") == 0)
    {
        const int v = crashTestBigFrame();
        const int m = std::snprintf(msg, sizeof(msg), "[crash] bigframe OK (4 MB stack frame survived, %d)\n", v);
        writeStderr(msg, m);
    }
}

void SetCrashReporterGuestInfo(CrashGuestInfoFn fn)
{
    g_guestInfo.store(fn);
}

void SetCrashReporterGuestMemory(const void *base, size_t size)
{
    g_guestMemSize.store(size);
    g_guestMem.store(base);
}

#else
void CrashReporterSelfTestIfRequested() {}
void InstallCrashReporter() {}
void RegisterThreadForCrashReporter() {}
void RegisterThreadForCrashReporter(const char *) {}
void SetCrashReporterGuestInfo(CrashGuestInfoFn) {}
void SetCrashReporterGuestMemory(const void *, size_t) {}
#endif
