// TEMP(transformers): opt-in host thread placement extracted from eff15, default OFF.
#include "ps2_core_place.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace ps2_core_place {
namespace {
const int g_place=[] { const char* p=std::getenv("PS2X_CORE_PLACE"); return p?std::atoi(p):0; }();
        // ---------------------------------------------------------------- core placement
        struct Topo
        {
            bool valid = false, hetero = false;
            int n = 0;
            uint64_t fast = 0, slow = 0, eeCore = 0, vuCore = 0;
            std::string how, detail;
        };

        std::string maskList(uint64_t m)
        {
            std::string s;
            for (int i = 0; i < 64; ++i)
            {
                if (!(m >> i & 1u))
                    continue;
                int j = i;
                while (j + 1 < 64 && (m >> (j + 1) & 1u))
                    ++j;
                if (!s.empty())
                    s += ',';
                s += std::to_string(i);
                if (j > i)
                    s += '-' + std::to_string(j);
                i = j;
            }
            return s.empty() ? std::string("-") : s;
        }

#if defined(_WIN32)
        bool readSysLong(const char *fmt, int cpu, long long &out)
        {
            char path[160];
            std::snprintf(path, sizeof(path), fmt, cpu);
            std::FILE *f = std::fopen(path, "rb");
            if (!f)
                return false;
            char buf[64] = {};
            const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            if (!got)
                return false;
            out = std::atoll(buf);
            return true;
        }
        bool readSysStr(const char *fmt, int cpu, std::string &out)
        {
            char path[160];
            std::snprintf(path, sizeof(path), fmt, cpu);
            std::FILE *f = std::fopen(path, "rb");
            if (!f)
                return false;
            char buf[128] = {};
            const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            out.assign(buf, got);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                out.pop_back();
            return got != 0;
        }

        struct Core
        {
            uint64_t mask = 0;
            long long rank = 0; // higher = faster (Linux: cpuinfo_max_freq, ties by highest_perf; Windows: EfficiencyClass)
            int first = 64;
        };

        // Fill `cores` from Linux sysfs through Wine's Z: drive (read-only). False when not readable.
        bool coresFromSysfs(int n, std::vector<Core> &cores, std::string &how)
        {
            std::vector<long long> maxf(n, 0), hp(n, 0);
            std::vector<std::string> sib(n);
            int okF = 0, okH = 0;
            for (int i = 0; i < n; ++i)
            {
                okF += readSysLong("Z:\\sys\\devices\\system\\cpu\\cpu%d\\cpufreq\\cpuinfo_max_freq", i, maxf[i]) ? 1 : 0;
                okH += readSysLong("Z:\\sys\\devices\\system\\cpu\\cpu%d\\acpi_cppc\\highest_perf", i, hp[i]) ? 1 : 0;
                if (!readSysStr("Z:\\sys\\devices\\system\\cpu\\cpu%d\\topology\\thread_siblings_list", i, sib[i]))
                    sib[i] = std::to_string(i);
            }
            if (okF != n && okH != n)
                return false;
            how = okF == n ? "sysfs cpuinfo_max_freq" : "sysfs acpi_cppc highest_perf";
            for (int i = 0; i < n; ++i)
            {
                const long long r = (okF == n ? maxf[i] : hp[i]) * 1000 + (okH == n ? hp[i] % 1000 : 0);
                auto it = std::find_if(cores.begin(), cores.end(), [&](const Core &c) {
                    for (int k = 0; k < n; ++k)
                        if ((c.mask >> k & 1u) && sib[k] == sib[i])
                            return true;
                    return false;
                });
                if (it == cores.end())
                {
                    cores.push_back(Core{});
                    it = cores.end() - 1;
                }
                it->mask |= 1ull << i;
                it->rank = std::max(it->rank, r);
                it->first = std::min(it->first, i);
            }
            return true;
        }

        bool coresFromWindows(std::vector<Core> &cores, std::string &how)
        {
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            if (!len)
                return false;
            std::vector<uint8_t> buf(len);
            auto *base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data());
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len))
                return false;
            for (DWORD off = 0; off < len;)
            {
                auto *e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data() + off);
                if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 && e->Processor.GroupMask[0].Group == 0)
                {
                    Core c;
                    c.mask = static_cast<uint64_t>(e->Processor.GroupMask[0].Mask);
                    c.rank = e->Processor.EfficiencyClass;
                    for (int i = 0; i < 64; ++i)
                        if (c.mask >> i & 1u)
                        {
                            c.first = i;
                            break;
                        }
                    cores.push_back(c);
                }
                off += e->Size;
                if (!e->Size)
                    break;
            }
            how = "GetLogicalProcessorInformationEx EfficiencyClass";
            return !cores.empty();
        }
#endif

        Topo buildTopo()
        {
            Topo t;
#if defined(_WIN32)
            t.n = static_cast<int>(std::min<DWORD>(64u, GetActiveProcessorCount(0)));
            std::vector<Core> cores;
            const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
            const bool wine = nt && GetProcAddress(nt, "wine_get_version");
            bool ok = wine && coresFromSysfs(t.n, cores, t.how);
            if (!ok)
            {
                cores.clear();
                ok = coresFromWindows(cores, t.how);
            }
            if (!ok || cores.empty())
                return t;
            t.valid = true;
            long long lo = cores[0].rank, hi = cores[0].rank;
            for (const Core &c : cores)
            {
                lo = std::min(lo, c.rank);
                hi = std::max(hi, c.rank);
            }
            // sysfs ranks are kHz*1000: "different class" = max clock >= 15 % apart. EfficiencyClass: any difference.
            const bool sysfs = t.how.rfind("sysfs", 0) == 0;
            t.hetero = sysfs ? (lo > 0 && hi * 100 >= lo * 115) : (hi != lo);
            const long long cut = sysfs ? (lo + hi) / 2 : hi;
            std::vector<Core> fastCores;
            for (const Core &c : cores)
            {
                if (c.rank >= cut)
                {
                    t.fast |= c.mask;
                    fastCores.push_back(c);
                }
                else
                    t.slow |= c.mask;
            }
            // Mode 2: EE on the best fast core (ties: avoid the core holding CPU 0 - it takes most interrupts), VU1 worker
            // on the next one.
            std::sort(fastCores.begin(), fastCores.end(), [](const Core &a, const Core &b) {
                if (a.rank != b.rank)
                    return a.rank > b.rank;
                const bool a0 = a.mask & 1u, b0 = b.mask & 1u;
                if (a0 != b0)
                    return !a0;
                return a.first < b.first;
            });
            if (!fastCores.empty())
                t.eeCore = fastCores[0].mask;
            t.vuCore = fastCores.size() > 1 ? fastCores[1].mask : t.eeCore;
            char d[256];
            std::snprintf(d, sizeof(d), "%d logical, %zu cores, fast=%s slow=%s eeCore=%s vuCore=%s", t.n, cores.size(),
                          maskList(t.fast).c_str(), maskList(t.slow).c_str(), maskList(t.eeCore).c_str(),
                          maskList(t.vuCore).c_str());
            t.detail = d;
#endif
            return t;
        }

        const Topo &topo()
        {
            static const Topo t = buildTopo();
            return t;
        }

        const char *kLaneName[] = {"EE", "GS", "render", "audio", "VU1worker"};
    }

    void onThreadStart(int lane)
    {
        static thread_local bool done = false;
        if (done)
            return;
        done = true;
        if (lane < 0 || lane > 4)
            return;
        if (!g_place)
            return;
        const Topo &t = topo();
        static std::once_flag once;
        std::call_once(once, [&] {
            std::fprintf(stderr, "[eff15] PS2X_CORE_PLACE=%d: %s (%s)%s\n", g_place, t.valid ? t.detail.c_str() : "topology unknown",
                         t.how.c_str(), (t.valid && !t.hetero) ? " - one core class, placement skipped" : "");
        });
        if (!t.valid || !t.hetero)
            return;
        const bool hot = lane == 0 || lane == 4;
        uint64_t mask = 0;
        switch (g_place)
        {
        case 1: mask = hot ? t.fast : t.slow; break;
        case 2: mask = lane == 0 ? t.eeCore : lane == 4 ? t.vuCore : t.slow; break;
        case 3: mask = t.slow; break;
        case 4: mask = hot ? t.fast : 0; break;
        case 5: mask = lane == 0 ? t.fast : t.slow; break;
        default: break;
        }
        if (!mask)
            return;
#if defined(_WIN32)
        const DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
        std::fprintf(stderr, "[eff15] thread %s (tid %lu) -> cpus %s %s (was 0x%llx)\n", kLaneName[lane], GetCurrentThreadId(),
                     maskList(mask).c_str(), prev ? "ok" : "FAILED", static_cast<unsigned long long>(prev));
#endif
    }

}
