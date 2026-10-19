// TEMP(transformers): host sampling profiler, enabled only by PS2X_PROFILE.
#include "ps2_profiler.h"
#if defined(_WIN32) && defined(_M_X64)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ps2_profile {
namespace {
struct Target {
    DWORD id;
    HANDLE handle;
    std::string label;
    std::map<DWORD64, uint64_t> counts;
    uint64_t samples = 0, failures = 0;
};
struct Raw { size_t target; DWORD64 rip; };
struct State {
    std::string path;
    unsigned period = 60;
    HANDLE wake = nullptr;
    HANDLE timer = nullptr;
    std::thread sampler;
    std::mutex labelsMutex;
    std::map<DWORD, std::string> labels;
    std::vector<Target> targets;
};
State* state = nullptr; // Set/cleared by owner only, after all registered workers join.

void discover(State& s, DWORD samplerId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 e{};
    e.dwSize = sizeof(e);
    if (Thread32First(snapshot, &e)) do {
        if (e.th32OwnerProcessID != GetCurrentProcessId() || e.th32ThreadID == samplerId) continue;
        auto found = std::find_if(s.targets.begin(), s.targets.end(), [&](const Target& t) {
            return t.id == e.th32ThreadID && WaitForSingleObject(t.handle, 0) == WAIT_TIMEOUT;
        });
        if (found != s.targets.end()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | SYNCHRONIZE, FALSE, e.th32ThreadID);
        if (h) s.targets.push_back({e.th32ThreadID, h, "runtime/worker"});
    } while (Thread32Next(snapshot, &e));
    CloseHandle(snapshot);
    // Never hold this mutex while a target is suspended.
    std::lock_guard<std::mutex> lock(s.labelsMutex);
    for (auto& t : s.targets) {
        auto label = s.labels.find(t.id);
        if (label != s.labels.end()) t.label = label->second;
    }
}

std::string hexAddress(DWORD64 rip) {
    std::ostringstream out;
    out << "0x" << std::hex << rip;
    return out.str();
}
struct Resolved { std::string function, module; };
Resolved resolve(DWORD64 rip, bool symbols) {
    Resolved r{hexAddress(rip), "unknown"};
    if (!symbols) return r;
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(GetCurrentProcess(), rip, &module)) r.module = module.ModuleName;
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol))
        r.function = r.module + "!" + std::string(symbol->Name, symbol->NameLen);
    else if (module.BaseOfImage) // no PDB: print the module-relative offset (resolve with the linker /MAP file)
        r.function = r.module + "+" + hexAddress(rip - module.BaseOfImage);
    else r.function = r.module + "!" + hexAddress(rip);
    return r;
}
void ranking(std::ostream& out, const std::map<std::string, uint64_t>& counts, uint64_t total, size_t limit) {
    std::vector<std::pair<std::string, uint64_t>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    size_t n = 0;
    for (const auto& row : rows) {
        if (n++ == limit) break;
        out << std::setw(10) << row.second << " " << std::fixed << std::setprecision(2)
            << std::setw(7) << (total ? 100.0 * row.second / total : 0.0) << "%  " << row.first << '\n';
    }
}
void report(State& s, bool final) {
    // All targets are running. DbgHelp is used exclusively on the sampler thread.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string directory(exe);
    const auto slash = directory.find_last_of("\\/");
    if (slash != std::string::npos) directory.resize(slash);
    const bool symbols = SymInitialize(GetCurrentProcess(), directory.c_str(), TRUE) != FALSE;
    // TEMP(transformers): main image bounds do not depend on DbgHelp/PDB availability.
    const auto imageBase = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
    const DWORD64 imageEnd = imageBase + nt->OptionalHeader.SizeOfImage;
    std::map<DWORD64, Resolved> cache;
    const std::string temporary = s.path + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (out) {
        out << "PS2X host profiler: cumulative " << (final ? "final" : "periodic") << " report\n"
            << "Requested interval 1 ms; self RIP samples only (no inclusive stacks).\n"
            << "Wall-time occupancy includes waits; percentages are within each thread.\n"
            << "Sampling pauses during symbol resolution/reporting; PDB symbols=" << symbols << "\n";
        // TEMP(transformers): bind RVA reports to the executable that produced them.
        out << "Main executable PE timestamp=" << std::hex << nt->FileHeader.TimeDateStamp
            << " image_size=" << nt->OptionalHeader.SizeOfImage << std::dec << '\n';
        for (const auto& t : s.targets) {
            std::map<std::string, uint64_t> functions, modules;
            for (const auto& entry : t.counts) {
                auto found = cache.find(entry.first);
                if (found == cache.end()) found = cache.emplace(entry.first, resolve(entry.first, symbols)).first;
                // TEMP(transformers): preserve main-exe identification if DbgHelp initialization fails.
                if (entry.first >= imageBase && entry.first < imageEnd && found->second.module == "unknown")
                    found->second = {"ps2EntryRunner+" + hexAddress(entry.first - imageBase), "ps2EntryRunner"};
                functions[found->second.function] += entry.second;
                modules[found->second.module] += entry.second;
            }
            out << "\nThread " << t.id << " [" << t.label << "] samples=" << t.samples
                << " capture_failures=" << t.failures << "\nTop 60 functions (self):\n";
            ranking(out, functions, t.samples, 60);
            out << "Modules (self):\n";
            ranking(out, modules, t.samples, modules.size());
            // TEMP(transformers): complete, unaggregated main-exe histogram, same snapshot as totals.
            out << "Main executable RVAs (hex_rva count):\n";
            for (const auto& entry : t.counts)
                if (entry.first >= imageBase && entry.first < imageEnd)
                    out << std::hex << (entry.first - imageBase) << std::dec << ' ' << entry.second << '\n';
            out << "End RVAs\n";
        }
        out.flush();
        const bool good = bool(out);
        out.close();
        // Keep the last complete report intact if interrupted while writing the next one.
        if (good && !MoveFileExA(temporary.c_str(), s.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            std::fprintf(stderr, "[profile] report replace failed: %lu\n", GetLastError());
    } else std::fprintf(stderr, "[profile] cannot write %s\n", temporary.c_str());
    if (symbols) SymCleanup(GetCurrentProcess());
}
void sample(State& s) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const DWORD ownId = GetCurrentThreadId();
    // Fixed raw-RIP batch. No allocation, locks, logging, or symbol calls under suspension.
    Raw raw[8192];
    size_t used = 0;
    auto drain = [&] {
        for (size_t i = 0; i < used; ++i) {
            auto& t = s.targets[raw[i].target];
            ++t.counts[raw[i].rip];
            ++t.samples;
        }
        used = 0;
    };
    using Clock = std::chrono::steady_clock;
    auto nextDiscovery = Clock::now();
    auto nextReport = Clock::now() + std::chrono::seconds(s.period);
    for (;;) {
        const auto now = Clock::now();
        if (now >= nextDiscovery) {
            discover(s, ownId);
            nextDiscovery = now + std::chrono::seconds(1);
        }
        for (size_t i = 0; i < s.targets.size(); ++i) {
            auto& t = s.targets[i];
            if (WaitForSingleObject(t.handle, 0) != WAIT_TIMEOUT) continue;
            if (used == 8192) drain();
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(t.handle) == DWORD(-1)) { ++t.failures; continue; }
            const BOOL captured = GetThreadContext(t.handle, &context);
            const DWORD resumed = ResumeThread(t.handle); // Always balance our suspend, including capture failure.
            if (captured && resumed != DWORD(-1)) raw[used++] = {i, context.Rip};
            else ++t.failures;
        }
        if (Clock::now() >= nextReport) {
            drain();
            report(s, false);
            nextReport = Clock::now() + std::chrono::seconds(s.period);
        }
        LARGE_INTEGER due{};
        due.QuadPart = -10000; // 1 ms, relative 100 ns units.
        if (!SetWaitableTimer(s.timer, &due, 0, nullptr, nullptr, FALSE)) break;
        HANDLE waits[] = {s.wake, s.timer};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
    }
    drain();
    report(s, true);
}
}
void start() {
    if (state) return;
    const char* path = std::getenv("PS2X_PROFILE");
    if (!path || !*path) return;
    auto* s = new State;
    s->path = path;
    if (const char* period = std::getenv("PS2X_PROFILE_PERIOD")) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(period, &end, 10);
        if (end != period && !*end && value >= 1 && value <= 86400) s->period = unsigned(value);
    }
    s->wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s->wake) { delete s; return; }
    // High-resolution waitable timers avoid Sleep(1)'s usual ~15.6 ms rounding.
    s->timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!s->timer) s->timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!s->timer) { CloseHandle(s->wake); delete s; return; }
    s->labels[GetCurrentThreadId()] = "main/render";
    state = s;
    try { s->sampler = std::thread([s] { try { sample(*s); }
        catch (...) { std::fprintf(stderr, "[profile] sampler stopped after internal failure\n"); } }); }
    catch (...) { state = nullptr; CloseHandle(s->timer); CloseHandle(s->wake); delete s; }
}
void registerThread(const char* label) {
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->labelsMutex);
    state->labels[GetCurrentThreadId()] = label;
}
void stop() {
    if (!state) return;
    State* s = state;
    SetEvent(s->wake);
    s->sampler.join();
    for (auto& t : s->targets) CloseHandle(t.handle);
    CloseHandle(s->timer);
    CloseHandle(s->wake);
    state = nullptr;
    delete s;
}
}
#else
namespace ps2_profile {
void start() {}
void registerThread(const char*) {}
void stop() {}
}
#endif
