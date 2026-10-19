#include "../ps2_vu1new.h" // TEMP(transformers)
// TEMP(transformers): unattended-test harness, see TestScript.h. Design: private qualification evidence section 3.4; usage and
// the .tfs script grammar: private qualification evidence
//
// Determinism contract: everything that changes guest-visible state (only the pad buttons) is decided on the EE thread
// from guest counters (port-0 pad reads, vsyncs, IOP sceCdRead requests, sceMpegCreate/Delete). Wall-clock time is used
// for one thing only: ending a run (script budget, host stall, exit watchdog). So the same build + script + fresh card
// always produce the same "[script]" timeline (compare two runs with private qualification evidence).
#include "TestScript.h"
#include "../ps2_iop_transport.h" // TEMP(transformers): deterministic PCM endpoint

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "../debug_counters.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern void (*g_ps2xCdReadHook)(uint32_t lsn, uint32_t sectors); // TEMP(transformers): ps2xIOP iop_cdvd.cpp (sceCdRead)

namespace ps2_stubs { void finishStoreImageByteTrace(); } // TEMP(transformers)

namespace ps2_test
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr const char *kResultNames[] = {"PASS", "RUNTIME_ERROR", "STALL", "FAIL",
                                                "TIMEOUT", "CRASH", "SCRIPT_ERROR", "ABORTED"};

        struct ButtonName
        {
            const char *name;
            uint16_t mask;
        };

        // Active-low PS2 pad bits, same layout as Kernel/Stubs/Pad.cpp kPadBtn*.
        constexpr ButtonName kButtons[] = {
            {"select", 1u << 0}, {"l3", 1u << 1}, {"r3", 1u << 2}, {"start", 1u << 3},
            {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
            {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11},
            {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};

        enum class Kind
        {
            Cd,          // an IOP sceCdRead touched the anchor's LSN range since the statement began
            Seen,        // ... at any time since boot
            Pads,        // n port-0 pad reads since the statement began
            Vs,          // n vsyncs since the statement began
            AtVs,        // absolute guest vsync >= n
            MovieCreate, // sceMpegCreate since the statement began
            MovieDelete, // sceMpegDelete since the statement began
            Watchdog,    // 3 s-watchdog detector state == (n != 0)
        };

        struct Atom
        {
            Kind kind = Kind::Vs;
            int anchor = -1;
            uint64_t n = 0;
        };

        struct Anchor // inclusive LSN range; lastSerial = cdSerial of the last read touching it (0 = never)
        {
            uint32_t lo = 0;
            uint32_t hi = 0;
            std::string label;
            uint64_t lastSerial = 0;
        };

        enum class Op
        {
            Wait,
            Press,
            PressUntil,
            Buttons,
            Stick, // TEMP(transformers) qualification: held analog stick values
            Mark,
            Dump,
            Pass,
            Fail,
        };

        struct Stmt
        {
            Op op = Op::Wait;
            int line = 0;
            std::string label; // label written on this line (empty if none)
            std::string text;  // statement text without label / comment (logs)
            std::string arg;   // mark / dump / pass / fail text
            std::vector<Atom> cond;
            uint16_t mask = 0;
            uint8_t stick[4] = {128, 128, 128, 128}; // 'stick' lx ly rx ry (0..255, 128 = centre)
            int hold = 3;
            int gap = 6;
            int everyReads = 30;
            int maxPresses = 5;
            uint64_t timeout = 3000; // 0 = no timeout
            bool timeoutInPads = false;
            bool elseContinue = false;
        };

        struct IsoFile
        {
            uint32_t lsn = 0;
            uint32_t sectors = 0;
            std::string path;
            bool seen = false;
            uint64_t firstVs = 0;
            uint64_t lastVs = 0;
        };

        enum class PressPhase
        {
            Idle,
            Down,
            Up,
        };

        struct Engine
        {
            bool active = false;
            bool ownsPad = false;
            PS2Runtime *rt = nullptr;
            std::string scriptPath;

            // Program (immutable once init() returns).
            std::vector<Stmt> prog;
            std::vector<Anchor> anchors;
            std::vector<IsoFile> iso; // sorted by lsn, zero-sector entries dropped
            int defHold = 3;
            int defGap = 6;
            uint64_t defTimeout = 3000;
            bool defTimeoutInPads = false;

            // Step machine.
            size_t ip = 0;
            bool started = false;
            std::string curLabel;
            uint64_t sVs = 0, sPads = 0, sSerial = 0, sMovC = 0, sMovD = 0; // counters when the statement began
            uint64_t lastPressPads = 0;
            int presses = 0;

            // Press machine.
            PressPhase phase = PressPhase::Idle;
            uint16_t pressMask = 0;
            uint16_t heldMask = 0; // 'buttons'
            uint8_t heldStick[4] = {128, 128, 128, 128}; // 'stick' lx ly rx ry
            bool stickOn = false;
            uint32_t lastStickLog = 0x80808080u;
            int phaseLeft = 0;
            int pressGap = 0;

            // Guest-time state (EE thread).
            uint64_t vs = 0, cyc = 0, pads = 0, firstPadVs = 0, lastPadVs = 0, lastCdVs = 0, cdSerial = 0;
            uint64_t movC = 0, movD = 0;
            uint16_t lastButtons = 0xFFFFu;
            std::deque<uint64_t> padVs; // vsync of each pad read inside the watchdog window
            bool wdOn = false;

            // Options.
            uint64_t stopAtVs = 0;
            uint64_t stallVs = 600;      // 0 = off
            uint64_t hardStallVs = 3600; // 0 = off
            uint64_t wdWindow = 600;
            uint64_t wdReads = 30; // 0 = off
            bool wdFail = false;
            double budgetS = 900.0; // 0 = off (always off without a script)
            double hostStallS = 60.0;
            bool dumpSteps = true;
            int stepNo = 0;
            Clock::time_point t0{};
            std::string dumpDir = ".";
            std::string resultPath;

            // Cross-thread.
            std::mutex mx; // guards everything above: EE-thread hooks vs. the exit watchdog thread and main.cpp
            std::atomic<uint64_t> vsPub{0};
            std::atomic<int> verdict{-1};
            std::mutex dumpMx;
            std::deque<std::string> dumpQueue; // guarded by dumpMx; drained by the render thread
            std::atomic<int> dumpPending{0};
        };

        Engine E;

        // ---- small helpers -------------------------------------------------------------------------------------

        unsigned long long ull(uint64_t v)
        {
            return static_cast<unsigned long long>(v);
        }

        const char *resultName(int code)
        {
            return (code >= 0 && code < static_cast<int>(sizeof(kResultNames) / sizeof(kResultNames[0])))
                       ? kResultNames[code]
                       : "UNKNOWN";
        }

        std::string vstrf(const char *f, va_list ap)
        {
            char buf[1024];
            va_list copy;
            va_copy(copy, ap);
            const int n = std::vsnprintf(buf, sizeof(buf), f, copy);
            va_end(copy);
            if (n < 0)
                return std::string();
            if (static_cast<size_t>(n) < sizeof(buf))
                return std::string(buf, static_cast<size_t>(n));
            std::string out(static_cast<size_t>(n) + 1u, '\0');
            std::vsnprintf(out.data(), out.size(), f, ap);
            out.resize(static_cast<size_t>(n));
            return out;
        }

        std::string strf(const char *f, ...)
        {
            va_list ap;
            va_start(ap, f);
            std::string s = vstrf(f, ap);
            va_end(ap);
            return s;
        }

        // One fwrite per line keeps lines whole while other threads log too (stderr -> boot_<run>.err.log).
        void logLine(const std::string &line, bool alsoStdout = false)
        {
            const std::string s = line + "\n";
            std::fwrite(s.data(), 1, s.size(), stderr);
            std::fflush(stderr);
            if (alsoStdout)
            {
                std::fwrite(s.data(), 1, s.size(), stdout);
                std::fflush(stdout);
            }
        }

        void slog(const char *f, ...)
        {
            va_list ap;
            va_start(ap, f);
            const std::string s = vstrf(f, ap);
            va_end(ap);
            logLine(s);
        }

        const char *envValue(const char *name)
        {
            const char *v = std::getenv(name);
            return (v && *v) ? v : nullptr;
        }

        double wall()
        {
            return std::chrono::duration<double>(Clock::now() - E.t0).count();
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string trim(const std::string &s)
        {
            const size_t b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return std::string();
            const size_t e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1u);
        }

        std::string buttonNames(uint16_t mask)
        {
            if (mask == 0u)
                return "none";
            std::string out;
            for (const ButtonName &b : kButtons)
            {
                if ((mask & b.mask) != 0u)
                {
                    if (!out.empty())
                        out += '+';
                    out += b.name;
                }
            }
            return out;
        }

        // Deterministic fingerprint: EE-thread guest counters only (prims are counted on the GS worker, so not here).
        std::string fingerprint()
        {
            return strf("vs=%llu pads=%llu cyc=%llu cd=%llu i5=%llu mscal=%llu", ull(E.vs), ull(E.pads), ull(E.cyc),
                        ull(E.cdSerial), ull(ps2dbg::irqDelivered[5].load(std::memory_order_relaxed)),
                        ull(ps2dbg::vu1Mscal.load(std::memory_order_relaxed)));
        }

        std::string fileTag(const std::string &text)
        {
            std::string out;
            for (char c : text)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                out += (std::isalnum(uc) || c == '-' || c == '_' || c == '.') ? c : '_';
                if (out.size() >= 48u)
                    break;
            }
            return out.empty() ? std::string("step") : out;
        }

        std::string jsonEscape(const std::string &s)
        {
            std::string out;
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else if (static_cast<unsigned char>(c) < 0x20u)
                {
                    out += strf("\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        // ---- verdict ---------------------------------------------------------------------------------------------

        // Caller holds E.mx. The PNG is written by the render thread (ps2_runtime.cpp UploadFrame -> takeDumpRequest).
        void requestDump(const std::string &tag, bool force)
        {
            if (!force && !E.dumpSteps)
                return;
            const std::string path = strf("%s/step_%02d_%s_vs%llu.png", E.dumpDir.c_str(), ++E.stepNo,
                                          fileTag(tag).c_str(), ull(E.vs));
            {
                std::lock_guard<std::mutex> lock(E.dumpMx);
                E.dumpQueue.push_back(path);
            }
            E.dumpPending.fetch_add(1, std::memory_order_release);
            slog("[script] dump %s", path.c_str());
        }

        void writeResultJson(int code, const std::string &reason)
        {
            if (E.resultPath.empty())
                return;
            std::ofstream out(E.resultPath, std::ios::out | std::ios::trunc);
            if (!out)
            {
                slog("[script] WARN cannot write result file %s", E.resultPath.c_str());
                return;
            }
            const int line = E.ip < E.prog.size() ? E.prog[E.ip].line : 0;
            out << strf("{\"result\":\"%s\",\"code\":%d,\"step\":%llu,\"label\":\"%s\",\"line\":%d,\"vs\":%llu,"
                        "\"pads\":%llu,\"cd\":%llu,\"wall\":%.1f,\"reason\":\"%s\"}",
                        resultName(code), code, ull(E.ip), jsonEscape(E.curLabel).c_str(), line, ull(E.vs), ull(E.pads),
                        ull(E.cdSerial), wall(), jsonEscape(reason).c_str())
                << "\n";
        }

        // First verdict wins. Caller holds E.mx (any thread). The exit watchdog thread then stops the runtime.
        void finish(int code, const std::string &reason)
        {
            int expected = -1;
            if (!E.verdict.compare_exchange_strong(expected, code))
                return;
            // TEMP(transformers): PASS is reached on the EE thread at the exact guest verdict.
            // Close capture now, before the wall-clock screenshot grace period adds audio.
            if (code == kExitPass)
            {
                PS2IopTransport::finishAudioCapture(E.rt);
                ps2_stubs::finishStoreImageByteTrace(); // TEMP(transformers)
            }
            logLine(strf("[script] RESULT %s code=%d step=%llu '%s' %s t=%.1fs reason=\"%s\"", resultName(code), code,
                         ull(E.ip), E.curLabel.c_str(), fingerprint().c_str(), wall(), reason.c_str()),
                    true);
            logLine("[script] state" + ps2dbg::summary()); // irq/I5, vu1, prims, pad counters
            if (E.rt)
            {
                const EeKernelSnapshot snap = E.rt->eeScheduler().snapshot(); // copies under its own mutex
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    slog("[script] thread T%d st=%d wait=%d:%d pc=0x%x ra=0x%x pri=%d inv=%u", t.id,
                         static_cast<int>(t.status), static_cast<int>(t.waitReason), t.waitId, t.pc, t.ra,
                         t.currentPriority, t.invocationDepth);
                }
            }
            {
                std::vector<const IsoFile *> seen;
                for (const IsoFile &f : E.iso)
                {
                    if (f.seen)
                        seen.push_back(&f);
                }
                std::sort(seen.begin(), seen.end(), [](const IsoFile *a, const IsoFile *b)
                          { return a->lastVs > b->lastVs; });
                std::string files;
                for (size_t i = 0; i < seen.size() && i < 8u; ++i)
                    files += strf(" %s@%llu", seen[i]->path.c_str(), ull(seen[i]->lastVs));
                slog("[script] files(last read)%s", files.empty() ? " none (no ISO map loaded?)" : files.c_str());
            }
            slog("[script] pads in last %llu vs: %llu%s", ull(E.wdWindow), ull(E.padVs.size()),
                 E.wdOn ? " (3 s watchdog mode)" : "");
            writeResultJson(code, reason);
            requestDump(std::string("RESULT_") + resultName(code), true);
        }

        // ---- step machine (EE thread, E.mx held) -----------------------------------------------------------------

        bool eval(const Stmt &s)
        {
            for (const Atom &a : s.cond)
            {
                switch (a.kind)
                {
                case Kind::Cd:
                    if (E.anchors[static_cast<size_t>(a.anchor)].lastSerial > E.sSerial)
                        return true;
                    break;
                case Kind::Seen:
                    if (E.anchors[static_cast<size_t>(a.anchor)].lastSerial != 0u)
                        return true;
                    break;
                case Kind::Pads:
                    if (E.pads - E.sPads >= a.n)
                        return true;
                    break;
                case Kind::Vs:
                    if (E.vs - E.sVs >= a.n)
                        return true;
                    break;
                case Kind::AtVs:
                    if (E.vs >= a.n)
                        return true;
                    break;
                case Kind::MovieCreate:
                    if (E.movC > E.sMovC)
                        return true;
                    break;
                case Kind::MovieDelete:
                    if (E.movD > E.sMovD)
                        return true;
                    break;
                case Kind::Watchdog:
                    if (E.wdOn == (a.n != 0u))
                        return true;
                    break;
                }
            }
            return false;
        }

        bool timedOut(const Stmt &s)
        {
            if (s.timeout == 0u)
                return false;
            return s.timeoutInPads ? (E.pads - E.sPads >= s.timeout) : (E.vs - E.sVs >= s.timeout);
        }

        void begin(const Stmt &s)
        {
            E.started = true;
            E.sVs = E.vs;
            E.sPads = E.pads;
            E.sSerial = E.cdSerial;
            E.sMovC = E.movC;
            E.sMovD = E.movD;
            E.presses = 0;
            if (!s.label.empty())
                E.curLabel = s.label;
            slog("[script] step %llu '%s' line=%d %s t=%.1fs :: %s", ull(E.ip), E.curLabel.c_str(), s.line,
                 fingerprint().c_str(), wall(), s.text.c_str());
        }

        void startPress(const Stmt &s)
        {
            E.phase = PressPhase::Down;
            E.pressMask = s.mask;
            E.phaseLeft = std::max(1, s.hold);
            E.pressGap = std::max(1, s.gap);
            E.lastPressPads = E.pads;
            ++E.presses;
            slog("[script] press %s #%d pads=%llu vs=%llu", buttonNames(s.mask).c_str(), E.presses, ull(E.pads),
                 ull(E.vs));
        }

        void advance()
        {
            ++E.ip;
            E.started = false;
        }

        void elseOrFail(const Stmt &s, const char *what)
        {
            if (s.elseContinue)
            {
                slog("[script] WARN %s (else continue) line=%d %s", what, s.line, fingerprint().c_str());
                advance();
                return;
            }
            finish(kExitFail, strf("%s at line %d: %s", what, s.line, s.text.c_str()));
        }

        // Runs statements until one blocks. Called at every pad read and every vsync, but only while no press is in
        // flight, so a 'press until' never presses again once its condition has become true.
        void step()
        {
            while (E.verdict.load() < 0 && E.phase == PressPhase::Idle)
            {
                if (E.ip >= E.prog.size())
                {
                    finish(kExitPass, "end of script");
                    return;
                }
                const Stmt &s = E.prog[E.ip];
                if (!E.started)
                    begin(s);
                switch (s.op)
                {
                case Op::Wait:
                    if (eval(s))
                    {
                        advance();
                        continue;
                    }
                    if (timedOut(s))
                    {
                        elseOrFail(s, "wait timed out");
                        continue;
                    }
                    return;
                case Op::Press:
                    if (E.presses == 0)
                    {
                        startPress(s);
                        return;
                    }
                    advance(); // down + up have completed
                    continue;
                case Op::PressUntil:
                    if (eval(s))
                    {
                        advance();
                        continue;
                    }
                    if (timedOut(s))
                    {
                        elseOrFail(s, "press-until timed out");
                        continue;
                    }
                    if (E.presses < s.maxPresses &&
                        (E.presses == 0 || E.pads - E.lastPressPads >= static_cast<uint64_t>(s.everyReads)))
                    {
                        startPress(s);
                    }
                    return;
                case Op::Buttons:
                    E.heldMask = s.mask;
                    advance();
                    continue;
                case Op::Stick:
                    std::memcpy(E.heldStick, s.stick, sizeof(E.heldStick));
                    E.stickOn = !(s.stick[0] == 128 && s.stick[1] == 128 && s.stick[2] == 128 && s.stick[3] == 128);
                    advance();
                    continue;
                case Op::Mark:
                    slog("[script] mark %s %s", s.arg.c_str(), fingerprint().c_str());
                    requestDump(s.arg, false);
                    advance();
                    continue;
                case Op::Dump:
                    requestDump(s.arg, true);
                    advance();
                    continue;
                case Op::Pass:
                    finish(kExitPass, s.arg);
                    return;
                case Op::Fail:
                    finish(kExitFail, s.arg);
                    return;
                }
                return;
            }
        }

        // ---- parser ----------------------------------------------------------------------------------------------

        std::vector<std::string> tokenize(const std::string &s)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
                const size_t b = i;
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
                if (i > b)
                    out.push_back(s.substr(b, i - b));
            }
            return out;
        }

        // ISO paths compare case-insensitively: upper case, '/' separators, leading '/', no ";1" version suffix.
        std::string normPath(std::string p)
        {
            for (char &c : p)
            {
                if (c == '\\')
                    c = '/';
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            const size_t semi = p.find(';');
            if (semi != std::string::npos)
                p.resize(semi);
            if (p.empty() || p[0] != '/')
                p.insert(p.begin(), '/');
            return p;
        }

        bool isLabelName(const std::string &s)
        {
            if (s.empty())
                return false;
            for (char c : s)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
                    return false;
            }
            return true;
        }

        // Plain decimal, or hex with a 0x prefix; the whole token must be consumed.
        bool parseNumber(const std::string &tok, uint64_t &out)
        {
            if (tok.empty() || !std::isdigit(static_cast<unsigned char>(tok[0])))
                return false;
            const bool hex = tok.size() > 2u && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X');
            char *end = nullptr;
            out = std::strtoull(tok.c_str(), &end, hex ? 16 : 10);
            return end && *end == '\0';
        }

        // "LO-HI" or "LO" (LO == HI), decimal or 0x hex.
        bool parseRange(const std::string &tok, uint64_t &lo, uint64_t &hi)
        {
            const size_t dash = tok.find('-');
            if (dash == std::string::npos)
            {
                if (!parseNumber(tok, lo))
                    return false;
                hi = lo;
                return true;
            }
            return parseNumber(tok.substr(0, dash), lo) && parseNumber(tok.substr(dash + 1u), hi) && lo <= hi;
        }

        // "3000vs" -> 3000 + "vs"; "40" -> 40 + "".
        bool splitCount(const std::string &tok, uint64_t &value, std::string &unit)
        {
            size_t i = 0;
            while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i])))
                ++i;
            if (i == 0u || i > 18u)
                return false;
            value = std::strtoull(tok.substr(0, i).c_str(), nullptr, 10);
            unit = lower(tok.substr(i));
            return true;
        }

        struct Parser
        {
            const std::vector<std::string> &tok;
            size_t i = 0;

            bool more() const { return i < tok.size(); }
            std::string peek() const { return more() ? lower(tok[i]) : std::string(); }
            std::string next() { return more() ? tok[i++] : std::string(); }
            std::string rest()
            {
                std::string out;
                while (more())
                {
                    if (!out.empty())
                        out += ' ';
                    out += tok[i++];
                }
                return out;
            }
        };

        // T := n 'vs' | n 'pads' (one token "3000vs" or two tokens "3000 vs"); a bare n means vsyncs.
        bool parseSpan(Parser &p, uint64_t &value, bool &inPads, std::string &err)
        {
            const std::string tok = p.next();
            std::string unit;
            if (!splitCount(tok, value, unit))
            {
                err = "expected a count like 3000vs or 40pads, got '" + tok + "'";
                return false;
            }
            if (unit.empty() && (p.peek() == "vs" || p.peek() == "pads"))
                unit = lower(p.next());
            if (unit.empty() || unit == "vs")
            {
                inPads = false;
            }
            else if (unit == "pads")
            {
                inPads = true;
            }
            else
            {
                err = "unknown unit in '" + tok + "' (use vs or pads)";
                return false;
            }
            return true;
        }

        bool parseVsCount(Parser &p, uint64_t &value, std::string &err, const char *what)
        {
            bool inPads = false;
            if (!parseSpan(p, value, inPads, err))
                return false;
            if (inPads)
            {
                err = std::string(what) + " is counted in vsyncs, not pads";
                return false;
            }
            return true;
        }

        bool parseReadCount(Parser &p, uint64_t &value, std::string &err, const char *what)
        {
            const std::string tok = p.next();
            std::string unit;
            if (!splitCount(tok, value, unit) || (!unit.empty() && unit != "pads"))
            {
                err = std::string(what) + " needs a number (of pad reads), got '" + tok + "'";
                return false;
            }
            return true;
        }

        bool hasKey(const std::string &lowTok, const char *key)
        {
            return lowTok.rfind(std::string(key) + "=", 0) == 0;
        }

        // "hold=3" style option.
        bool parseIntOption(const std::string &lowTok, const char *key, int minValue, int &out, std::string &err)
        {
            uint64_t n = 0;
            if (!parseNumber(lowTok.substr(std::strlen(key) + 1u), n) || n < static_cast<uint64_t>(minValue) ||
                n > 100000u)
            {
                err = strf("bad value in '%s' (%s needs a number >= %d)", lowTok.c_str(), key, minValue);
                return false;
            }
            out = static_cast<int>(n);
            return true;
        }

        bool parseButtons(const std::string &tok, uint16_t &mask, std::string &err)
        {
            mask = 0;
            const std::string s = lower(tok);
            size_t pos = 0;
            while (pos <= s.size())
            {
                size_t end = s.find('+', pos);
                if (end == std::string::npos)
                    end = s.size();
                const std::string name = s.substr(pos, end - pos);
                uint16_t m = 0;
                for (const ButtonName &b : kButtons)
                {
                    if (name == b.name)
                        m = b.mask;
                }
                if (m == 0u)
                {
                    err = "unknown button '" + name +
                          "' (start select up down left right cross circle square triangle l1 r1 l2 r2 l3 r3)";
                    return false;
                }
                mask = static_cast<uint16_t>(mask | m);
                pos = end + 1u;
            }
            return true;
        }

        const IsoFile *findIso(const std::string &path)
        {
            const std::string want = normPath(path);
            for (const IsoFile &f : E.iso)
            {
                if (f.path == want)
                    return &f;
            }
            return nullptr;
        }

        int addAnchor(uint32_t lo, uint32_t hi, const std::string &label)
        {
            for (size_t i = 0; i < E.anchors.size(); ++i)
            {
                if (E.anchors[i].lo == lo && E.anchors[i].hi == hi)
                    return static_cast<int>(i);
            }
            Anchor a;
            a.lo = lo;
            a.hi = hi;
            a.label = label;
            E.anchors.push_back(a);
            return static_cast<int>(E.anchors.size() - 1u);
        }

        bool parseAtom(Parser &p, Atom &a, std::string &err)
        {
            if (!p.more())
            {
                err = "missing condition";
                return false;
            }
            const std::string kw = lower(p.next());
            if (kw == "cd" || kw == "seen")
            {
                if (!p.more())
                {
                    err = kw + " needs an ISO path";
                    return false;
                }
                std::string path = p.next();
                std::string range;
                const size_t at = path.find('@');
                if (at != std::string::npos)
                {
                    range = path.substr(at + 1u);
                    path.resize(at);
                }
                else if (p.peek().size() > 1u && p.peek()[0] == '@')
                {
                    range = p.next().substr(1u);
                }
                const IsoFile *f = findIso(path);
                if (!f)
                {
                    err = "path '" + path + "' is not in PS2X_ISOMAP" +
                          (E.iso.empty() ? std::string(" (no map loaded: set PS2X_ISOMAP)") : std::string());
                    return false;
                }
                uint32_t lo = f->lsn;
                uint32_t hi = f->lsn + f->sectors - 1u;
                std::string label = f->path;
                if (!range.empty())
                {
                    uint64_t rlo = 0, rhi = 0;
                    if (!parseRange(range, rlo, rhi) || rhi >= f->sectors)
                    {
                        err = strf("bad sector range '@%s' for %s (%u sectors: use @LO-HI with LO <= HI < %u)",
                                   range.c_str(), f->path.c_str(), f->sectors, f->sectors);
                        return false;
                    }
                    lo = f->lsn + static_cast<uint32_t>(rlo);
                    hi = f->lsn + static_cast<uint32_t>(rhi);
                    label += "@" + range;
                }
                a.kind = (kw == "cd") ? Kind::Cd : Kind::Seen;
                a.anchor = addAnchor(lo, hi, label);
                return true;
            }
            if (kw == "lsn")
            {
                const std::string tok = p.next();
                uint64_t lo = 0, hi = 0;
                if (!parseRange(tok, lo, hi) || hi > 0xFFFFFFFFull)
                {
                    err = "lsn needs LO-HI (decimal or 0x hex, inclusive), got '" + tok + "'";
                    return false;
                }
                a.kind = Kind::Cd;
                a.anchor = addAnchor(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), "lsn " + tok);
                return true;
            }
            if (kw == "pads" || kw == "vs" || kw == "atvs")
            {
                const std::string tok = p.next();
                std::string unit;
                if (!splitCount(tok, a.n, unit) || !unit.empty())
                {
                    err = kw + " needs a plain number, got '" + tok + "'";
                    return false;
                }
                a.kind = (kw == "pads") ? Kind::Pads : ((kw == "vs") ? Kind::Vs : Kind::AtVs);
                return true;
            }
            if (kw == "movie")
            {
                const std::string what = lower(p.next());
                if (what == "create")
                    a.kind = Kind::MovieCreate;
                else if (what == "delete")
                    a.kind = Kind::MovieDelete;
                else
                {
                    err = "movie needs create|delete, got '" + what + "'";
                    return false;
                }
                return true;
            }
            if (kw == "watchdog")
            {
                const std::string what = lower(p.next());
                if (what != "on" && what != "off")
                {
                    err = "watchdog condition needs on|off, got '" + what + "'";
                    return false;
                }
                a.kind = Kind::Watchdog;
                a.n = (what == "on") ? 1u : 0u;
                return true;
            }
            err = "unknown condition '" + kw + "' (cd seen lsn pads vs atvs movie watchdog)";
            return false;
        }

        bool parseCond(Parser &p, std::vector<Atom> &cond, std::string &err)
        {
            for (;;)
            {
                Atom a;
                if (!parseAtom(p, a, err))
                    return false;
                cond.push_back(a);
                if (p.peek() != "or")
                    return true;
                p.next();
            }
        }

        // Trailing statement options: hold= gap= until (press), timeout else (wait / press until), every max (press until).
        bool parseStmtOptions(Parser &p, Stmt &s, bool isPress, std::string &err)
        {
            bool everySet = false;
            while (p.more())
            {
                const std::string low = p.peek();
                if (isPress && hasKey(low, "hold"))
                {
                    if (!parseIntOption(lower(p.next()), "hold", 1, s.hold, err))
                        return false;
                }
                else if (isPress && hasKey(low, "gap"))
                {
                    if (!parseIntOption(lower(p.next()), "gap", 0, s.gap, err))
                        return false;
                }
                else if (isPress && low == "until" && s.op == Op::Press)
                {
                    p.next();
                    s.op = Op::PressUntil;
                    if (!parseCond(p, s.cond, err))
                        return false;
                }
                else if (low == "timeout" && s.op != Op::Press)
                {
                    p.next();
                    if (!parseSpan(p, s.timeout, s.timeoutInPads, err))
                        return false;
                }
                else if (low == "else" && s.op != Op::Press)
                {
                    p.next();
                    const std::string what = lower(p.next());
                    if (what == "fail")
                        s.elseContinue = false;
                    else if (what == "continue")
                        s.elseContinue = true;
                    else
                    {
                        err = "else needs fail|continue, got '" + what + "'";
                        return false;
                    }
                }
                else if (low == "every" && s.op == Op::PressUntil)
                {
                    p.next();
                    uint64_t n = 0;
                    if (!parseReadCount(p, n, err, "every"))
                        return false;
                    if (n == 0u || n > 100000u)
                    {
                        err = "every needs 1..100000 pad reads";
                        return false;
                    }
                    s.everyReads = static_cast<int>(n);
                    everySet = true;
                }
                else if (low == "max" && s.op == Op::PressUntil)
                {
                    p.next();
                    uint64_t n = 0;
                    if (!parseReadCount(p, n, err, "max") || n == 0u || n > 100000u)
                    {
                        if (err.empty())
                            err = "max needs 1..100000 presses";
                        return false;
                    }
                    s.maxPresses = static_cast<int>(n);
                }
                else
                {
                    err = "unexpected '" + p.next() + "'";
                    if (s.op == Op::Press)
                        err += " (a one-shot press only takes hold=/gap=; add 'until COND' for every/max/timeout/else)";
                    return false;
                }
            }
            if (s.op == Op::PressUntil && !everySet)
                s.everyReads = std::max(s.hold + s.gap, 30);
            return true;
        }

        enum class LineKind
        {
            Option,
            Statement,
        };

        bool parseLine(const std::vector<std::string> &tok, int lineNo, const std::string &label, LineKind &kind,
                       std::string &err)
        {
            Parser p{tok};
            const std::string kw = lower(p.next());
            kind = LineKind::Option;
            if (kw == "budget")
            {
                if (p.peek() == "off")
                {
                    p.next();
                    E.budgetS = 0.0;
                }
                else
                {
                    const std::string t = p.next();
                    uint64_t n = 0;
                    std::string unit;
                    if (!splitCount(t, n, unit))
                    {
                        err = "budget needs seconds, e.g. 'budget 900s', got '" + t + "'";
                        return false;
                    }
                    if (unit.empty() && p.peek() == "s")
                        unit = lower(p.next());
                    if (!unit.empty() && unit != "s")
                    {
                        err = "budget is in seconds ('900s'), got '" + t + "'";
                        return false;
                    }
                    E.budgetS = static_cast<double>(n);
                }
            }
            else if (kw == "stall")
            {
                if (p.peek() == "off")
                {
                    p.next();
                    E.stallVs = 0;
                }
                else
                {
                    if (!parseVsCount(p, E.stallVs, err, "stall"))
                        return false;
                    E.hardStallVs = std::max<uint64_t>(3600u, E.stallVs);
                    if (p.peek() == "hard")
                    {
                        p.next();
                        if (!parseVsCount(p, E.hardStallVs, err, "stall hard"))
                            return false;
                    }
                }
            }
            else if (kw == "watchdog")
            {
                const std::string mode = lower(p.next());
                if (mode == "off")
                {
                    E.wdReads = 0;
                }
                else if (mode == "warn" || mode == "fail")
                {
                    E.wdFail = (mode == "fail");
                    if (!parseReadCount(p, E.wdReads, err, "watchdog reads") ||
                        !parseVsCount(p, E.wdWindow, err, "watchdog window"))
                        return false;
                    if (E.wdWindow == 0u)
                    {
                        err = "watchdog window must be > 0 vsyncs";
                        return false;
                    }
                }
                else
                {
                    err = "watchdog option needs 'warn R N', 'fail R N' or 'off', got '" + mode + "'";
                    return false;
                }
            }
            else if (kw == "defaults")
            {
                if (!p.more())
                {
                    err = "defaults needs hold= gap= timeout=";
                    return false;
                }
                while (p.more())
                {
                    const std::string t = lower(p.next());
                    if (hasKey(t, "hold"))
                    {
                        if (!parseIntOption(t, "hold", 1, E.defHold, err))
                            return false;
                    }
                    else if (hasKey(t, "gap"))
                    {
                        if (!parseIntOption(t, "gap", 0, E.defGap, err))
                            return false;
                    }
                    else if (hasKey(t, "timeout"))
                    {
                        const std::vector<std::string> one{t.substr(8)};
                        Parser q{one};
                        if (!parseSpan(q, E.defTimeout, E.defTimeoutInPads, err))
                            return false;
                    }
                    else
                    {
                        err = "unknown default '" + t + "' (hold= gap= timeout=)";
                        return false;
                    }
                }
            }
            else if (kw == "dumpsteps")
            {
                const std::string v = lower(p.next());
                if (v == "on")
                    E.dumpSteps = true;
                else if (v == "off")
                    E.dumpSteps = false;
                else
                {
                    err = "dumpsteps needs on|off, got '" + v + "'";
                    return false;
                }
            }
            else
            {
                kind = LineKind::Statement;
                Stmt s;
                s.line = lineNo;
                s.label = label;
                s.hold = E.defHold;
                s.gap = E.defGap;
                s.timeout = E.defTimeout;
                s.timeoutInPads = E.defTimeoutInPads;
                {
                    Parser all{tok};
                    s.text = all.rest();
                }
                if (kw == "wait")
                {
                    s.op = Op::Wait;
                    if (!parseCond(p, s.cond, err) || !parseStmtOptions(p, s, false, err))
                        return false;
                }
                else if (kw == "press")
                {
                    s.op = Op::Press;
                    if (!parseButtons(p.next(), s.mask, err) || !parseStmtOptions(p, s, true, err))
                        return false;
                }
                else if (kw == "buttons")
                {
                    s.op = Op::Buttons;
                    const std::string t = p.next();
                    if (lower(t) == "none")
                        s.mask = 0;
                    else if (!parseButtons(t, s.mask, err))
                        return false;
                }
                else if (kw == "stick") // TEMP(transformers) qualification: stick none | stick <lx> <ly> [<rx> <ry>]
                {
                    s.op = Op::Stick;
                    const std::string t = lower(p.next());
                    if (t != "none")
                    {
                        std::string vals[4] = {t, p.next(), "", ""};
                        if (p.more())
                        {
                            vals[2] = p.next();
                            vals[3] = p.next();
                        }
                        for (int k = 0; k < 4; ++k)
                        {
                            if (vals[k].empty())
                            {
                                if (k < 2)
                                {
                                    err = "stick needs 'none' or <lx> <ly> [<rx> <ry>] (0..255, 128 = centre)";
                                    return false;
                                }
                                continue;
                            }
                            char *endp = nullptr;
                            const unsigned long v = std::strtoul(vals[k].c_str(), &endp, 10);
                            if (!endp || *endp != '\0' || v > 255u)
                            {
                                err = "bad stick value '" + vals[k] + "' (0..255, 128 = centre)";
                                return false;
                            }
                            s.stick[k] = static_cast<uint8_t>(v);
                        }
                    }
                }
                else if (kw == "mark" || kw == "dump")
                {
                    s.op = (kw == "mark") ? Op::Mark : Op::Dump;
                    s.arg = p.rest();
                    if (s.arg.empty())
                    {
                        err = kw + " needs a text";
                        return false;
                    }
                }
                else if (kw == "pass" || kw == "fail")
                {
                    s.op = (kw == "pass") ? Op::Pass : Op::Fail;
                    s.arg = p.rest();
                    if (s.arg.empty())
                        s.arg = kw;
                }
                else
                {
                    err = "unknown keyword '" + kw + "' (wait press buttons stick mark dump pass fail budget stall watchdog "
                                                     "defaults dumpsteps)";
                    return false;
                }
                if (p.more())
                {
                    err = "unexpected '" + p.next() + "'";
                    return false;
                }
                E.prog.push_back(std::move(s));
                return true;
            }
            if (p.more())
            {
                err = "unexpected '" + p.next() + "'";
                return false;
            }
            return true;
        }

        bool parseScript(const char *file, std::string &err)
        {
            std::ifstream in(file);
            if (!in)
            {
                err = strf("cannot open script '%s'", file);
                return false;
            }
            std::string raw;
            std::string pendingLabel;
            int lineNo = 0;
            while (std::getline(in, raw))
            {
                ++lineNo;
                if (lineNo == 1 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
                    raw.erase(0, 3); // UTF-8 BOM
                const size_t hash = raw.find('#');
                std::vector<std::string> tok = tokenize(hash == std::string::npos ? raw : raw.substr(0, hash));
                if (tok.empty())
                    continue;
                std::string label = pendingLabel;
                if (tok[0].size() > 1u && tok[0].back() == ':' && isLabelName(tok[0].substr(0, tok[0].size() - 1u)))
                {
                    label = tok[0].substr(0, tok[0].size() - 1u);
                    tok.erase(tok.begin());
                    if (tok.empty())
                    {
                        pendingLabel = label; // label alone on its line: applies to the next statement
                        continue;
                    }
                }
                LineKind kind = LineKind::Statement;
                std::string lineErr;
                if (!parseLine(tok, lineNo, label, kind, lineErr))
                {
                    err = strf("line %d: %s  [%s]", lineNo, lineErr.c_str(), trim(raw).c_str());
                    return false;
                }
                pendingLabel = (kind == LineKind::Option) ? label : std::string();
            }
            if (E.prog.empty())
            {
                err = "script has no statements";
                return false;
            }
            return true;
        }

        void loadIsoMap(const char *path)
        {
            if (!path)
                return;
            std::ifstream in(path);
            if (!in)
            {
                slog("[script] WARN cannot open PS2X_ISOMAP %s", path);
                return;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                const size_t t1 = line.find('\t');
                const size_t t2 = (t1 == std::string::npos) ? std::string::npos : line.find('\t', t1 + 1u);
                if (t2 == std::string::npos)
                    continue;
                uint64_t lsn = 0, sectors = 0;
                if (!parseNumber(trim(line.substr(0, t1)), lsn) ||
                    !parseNumber(trim(line.substr(t1 + 1u, t2 - t1 - 1u)), sectors) || sectors == 0u)
                    continue; // zero-sector files can never be read
                IsoFile f;
                f.lsn = static_cast<uint32_t>(lsn);
                f.sectors = static_cast<uint32_t>(sectors);
                f.path = normPath(trim(line.substr(t2 + 1u)));
                E.iso.push_back(f);
            }
            std::sort(E.iso.begin(), E.iso.end(), [](const IsoFile &a, const IsoFile &b)
                      { return a.lsn < b.lsn; });
        }

        std::string describeCond(const std::vector<Atom> &cond)
        {
            std::string out;
            for (const Atom &a : cond)
            {
                if (!out.empty())
                    out += " or ";
                switch (a.kind)
                {
                case Kind::Cd:
                case Kind::Seen:
                {
                    const Anchor &an = E.anchors[static_cast<size_t>(a.anchor)];
                    out += strf("%s %s[lsn 0x%x-0x%x]", a.kind == Kind::Cd ? "cd" : "seen", an.label.c_str(), an.lo,
                                an.hi);
                    break;
                }
                case Kind::Pads:
                    out += strf("pads %llu", ull(a.n));
                    break;
                case Kind::Vs:
                    out += strf("vs %llu", ull(a.n));
                    break;
                case Kind::AtVs:
                    out += strf("atvs %llu", ull(a.n));
                    break;
                case Kind::MovieCreate:
                    out += "movie create";
                    break;
                case Kind::MovieDelete:
                    out += "movie delete";
                    break;
                case Kind::Watchdog:
                    out += (a.n != 0u) ? "watchdog on" : "watchdog off";
                    break;
                }
            }
            return out;
        }

        std::string describeTimeout(const Stmt &s)
        {
            if (s.timeout == 0u)
                return std::string("timeout none");
            return strf("timeout %llu%s else %s", ull(s.timeout), s.timeoutInPads ? "pads" : "vs",
                        s.elseContinue ? "continue" : "fail");
        }

        std::string describe(const Stmt &s)
        {
            switch (s.op)
            {
            case Op::Wait:
                return "wait " + describeCond(s.cond) + " " + describeTimeout(s);
            case Op::Press:
                return strf("press %s hold=%d gap=%d", buttonNames(s.mask).c_str(), s.hold, s.gap);
            case Op::PressUntil:
                return strf("press %s hold=%d gap=%d until %s every %d max %d %s", buttonNames(s.mask).c_str(), s.hold,
                            s.gap, describeCond(s.cond).c_str(), s.everyReads, s.maxPresses, describeTimeout(s).c_str());
            case Op::Buttons:
                return "buttons " + buttonNames(s.mask);
            case Op::Stick:
                return strf("stick %u %u %u %u", s.stick[0], s.stick[1], s.stick[2], s.stick[3]);
            case Op::Mark:
                return "mark " + s.arg;
            case Op::Dump:
                return "dump " + s.arg;
            case Op::Pass:
                return "pass " + s.arg;
            case Op::Fail:
                return "fail " + s.arg;
            }
            return std::string();
        }

        std::string optionSummary()
        {
            return strf("budget=%.0fs stall=%llu/%llu watchdog=%s %llu/%llu dumpsteps=%s stopAtVs=%llu hostStall=%.0fs",
                        E.budgetS, ull(E.stallVs), ull(E.hardStallVs),
                        E.wdReads == 0u ? "off" : (E.wdFail ? "fail" : "warn"), ull(E.wdReads), ull(E.wdWindow),
                        E.dumpSteps ? "on" : "off", ull(E.stopAtVs), E.hostStallS);
        }

        void printProgram()
        {
            logLine(strf("[script] program %s: %llu statements, %llu anchors, iso=%llu %s", E.scriptPath.c_str(),
                         ull(E.prog.size()), ull(E.anchors.size()), ull(E.iso.size()), optionSummary().c_str()),
                    true);
            std::string label;
            for (size_t i = 0; i < E.prog.size(); ++i)
            {
                const Stmt &s = E.prog[i];
                if (!s.label.empty())
                    label = s.label;
                logLine(strf("[script] prog %llu line=%d '%s' %s", ull(i), s.line, label.c_str(), describe(s).c_str()),
                        true);
            }
        }

        // ---- exit watchdog thread: the only place where wall-clock time matters ------------------------------------

        void watchdogThread()
        {
            uint64_t seenVs = ~0ull;
            Clock::time_point changed = Clock::now();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const int v = E.verdict.load();
                if (v >= 0)
                {
                    // Give the render thread up to 3 s to write the queued step / verdict screenshots.
                    for (int i = 0; i < 12 && E.dumpPending.load(std::memory_order_acquire) > 0; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    E.rt->requestStop(); // normal path: run() joins the game thread, main.cpp exits with exitCode()
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    logLine(strf("[script] exit watchdog: run() did not return within 20 s, forcing exit %d", v));
                    std::fflush(nullptr);
                    ps2_vu1new::shutdown(); // TEMP(transformers): this exit bypasses static destructors
                    std::_Exit(v);
                }
                const uint64_t cur = E.vsPub.load(std::memory_order_relaxed);
                if (cur != seenVs)
                {
                    seenVs = cur;
                    changed = Clock::now();
                }
                else if (cur != 0u && E.hostStallS > 0.0 &&
                         std::chrono::duration<double>(Clock::now() - changed).count() > E.hostStallS)
                {
                    std::lock_guard<std::mutex> lock(E.mx);
                    finish(kExitStall, strf("host stall: guest vsync stuck at %llu for %.0f s", ull(cur), E.hostStallS));
                    continue;
                }
                if (E.budgetS > 0.0 && wall() > E.budgetS)
                {
                    std::lock_guard<std::mutex> lock(E.mx);
                    finish(kExitTimeout, strf("wall budget %.0f s exceeded", E.budgetS));
                }
            }
        }
    }

    void init(PS2Runtime &rt)
    {
        if(ps2_vu1new::enabled())g_ps2xCdReadHook = &onCdRead; // TEMP(transformers): also observe ordinary play
        const char *script = envValue("PS2X_SCRIPT");
        const char *stop = envValue("PS2X_STOP_AT_VS");
        const char *stall = envValue("PS2X_STALL_VS");
        const bool parseOnly = envValue("PS2X_SCRIPT_PARSE_ONLY") != nullptr;
        if (!script && !stop && !stall && !parseOnly)
            return; // inactive: every hook is a no-op (play.bat, plain runs)

        E.active = true;
        E.rt = &rt;
        E.t0 = Clock::now();
        if (stop)
            E.stopAtVs = std::strtoull(stop, nullptr, 10);
        if (stall)
        {
            E.stallVs = std::strtoull(stall, nullptr, 10); // 0 = off
            E.hardStallVs = std::max<uint64_t>(3600u, E.stallVs);
        }
        if (const char *h = envValue("PS2X_HOST_STALL_S"))
            E.hostStallS = std::atof(h); // 0 = off
        if (const char *d = envValue("PS2X_DUMP_DIR"))
            E.dumpDir = d;
        if (const char *r = envValue("PS2X_SCRIPT_RESULT"))
            E.resultPath = r;
        loadIsoMap(envValue("PS2X_ISOMAP"));

        if (script)
        {
            E.ownsPad = true;
            E.scriptPath = script;
            std::string err;
            if (!parseScript(script, err)) // a script's own budget/stall/watchdog lines override the env defaults
            {
                E.prog.clear(); // the result JSON reports step 0 / line 0; the line is in the reason
                E.verdict.store(kExitScriptError);
                logLine("[script] SCRIPT_ERROR " + err, true);
                logLine(strf("[script] RESULT SCRIPT_ERROR code=%d reason=\"%s\"", static_cast<int>(kExitScriptError),
                             err.c_str()),
                        true);
                writeResultJson(kExitScriptError, err);
                std::fflush(nullptr);
                std::_Exit(kExitScriptError);
            }
        }
        else
        {
            E.budgetS = 0.0; // no script: run_test -Seconds is the only wall-clock limit
            if (parseOnly)
            {
                logLine("[script] SCRIPT_ERROR PS2X_SCRIPT_PARSE_ONLY needs PS2X_SCRIPT", true);
                std::fflush(nullptr);
                std::_Exit(kExitScriptError);
            }
        }
        if (parseOnly)
        {
            printProgram();
            std::fflush(nullptr);
            std::_Exit(kExitPass);
        }

        g_ps2xCdReadHook = &onCdRead;
        std::thread(watchdogThread).detach();
        // No host paths in this line: script_compare.py diffs it between runs.
        slog("[script] start script=%s steps=%llu iso=%llu anchors=%llu ownsPad=%d %s",
             E.scriptPath.empty() ? "-" : E.scriptPath.c_str(), ull(E.prog.size()), ull(E.iso.size()),
             ull(E.anchors.size()), E.ownsPad ? 1 : 0, optionSummary().c_str());
    }

    bool ownsPad()
    {
        return E.ownsPad;
    }

    void onPadRead(uint16_t &buttonsActiveLow)
    {
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        ++E.pads;
        E.lastPadVs = E.vs;
        E.padVs.push_back(E.vs);
        if (E.firstPadVs == 0u)
            E.firstPadVs = std::max<uint64_t>(E.vs, 1u);
        if (E.ownsPad && E.verdict.load() < 0)
        {
            step(); // may start a press on this very read
            uint16_t mask = E.heldMask;
            if (E.phase == PressPhase::Down)
            {
                mask = static_cast<uint16_t>(mask | E.pressMask);
                if (--E.phaseLeft <= 0)
                {
                    E.phase = PressPhase::Up;
                    E.phaseLeft = E.pressGap;
                }
            }
            else if (E.phase == PressPhase::Up && --E.phaseLeft <= 0)
            {
                E.phase = PressPhase::Idle;
            }
            buttonsActiveLow = static_cast<uint16_t>(buttonsActiveLow & ~mask);
        }
        if (buttonsActiveLow != E.lastButtons) // input audit trail: only changes are logged
        {
            slog("[script] pad pads=%llu vs=%llu btn=0x%04x %s", ull(E.pads), ull(E.vs),
                 static_cast<unsigned>(buttonsActiveLow),
                 buttonNames(static_cast<uint16_t>(~buttonsActiveLow)).c_str());
            E.lastButtons = buttonsActiveLow;
        }
    }

    // TEMP(transformers) qualification: the script's held 'stick' values replace the (centred) scripted sticks. Called
    // right after onPadRead() for the same read, so a 'stick' statement takes effect on the read that reached it.
    void onPadSticks(uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry)
    {
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        if (!E.ownsPad || E.verdict.load() >= 0)
            return;
        if (!E.stickOn)
        {
            if (E.lastStickLog != 0x80808080u)
            {
                slog("[script] stick pads=%llu vs=%llu centred", ull(E.pads), ull(E.vs));
                E.lastStickLog = 0x80808080u;
            }
            return;
        }
        lx = E.heldStick[0];
        ly = E.heldStick[1];
        rx = E.heldStick[2];
        ry = E.heldStick[3];
        const uint32_t packed = uint32_t(lx) << 24 | uint32_t(ly) << 16 | uint32_t(rx) << 8 | uint32_t(ry);
        if (packed != E.lastStickLog) // input audit trail: only changes are logged
        {
            slog("[script] stick pads=%llu vs=%llu lx=%u ly=%u rx=%u ry=%u", ull(E.pads), ull(E.vs), lx, ly, rx, ry);
            E.lastStickLog = packed;
        }
    }

    void onVSync(uint64_t vs, uint64_t eeCycle)
    {
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        E.vs = vs;
        E.cyc = eeCycle;
        E.vsPub.store(vs, std::memory_order_relaxed);
        while (!E.padVs.empty() && E.padVs.front() + E.wdWindow <= vs)
            E.padVs.pop_front();
        if (E.verdict.load() >= 0)
            return;
        if (E.stopAtVs != 0u && vs >= E.stopAtVs)
        {
            finish(kExitPass, strf("stop-at-vs %llu", ull(E.stopAtVs)));
            return;
        }
        // Stall rule (PLAN 1.4): no pad read AND no CD read for stallVs, or no pad read at all for hardStallVs (an audio
        // stream can keep reading the CD after the EE hung). Armed after the first pad read. The 3 s-watchdog frame
        // loop (1 pad read per ~180 vsyncs) still responds to input, so it is only a warning below.
        if (E.stallVs != 0u && E.pads != 0u)
        {
            const uint64_t noPad = vs - E.lastPadVs;
            const uint64_t noCd = vs - E.lastCdVs;
            if (E.hardStallVs != 0u && noPad >= E.hardStallVs)
            {
                finish(kExitStall, strf("no pad read for %llu vs", ull(noPad)));
                return;
            }
            if (noPad >= E.stallVs && noCd >= E.stallVs)
            {
                finish(kExitStall, strf("no pad read for %llu vs and no CD read for %llu vs", ull(noPad), ull(noCd)));
                return;
            }
        }
        if (E.wdReads != 0u && E.pads != 0u && vs >= E.firstPadVs + E.wdWindow)
        {
            const bool wd = E.padVs.size() < E.wdReads;
            if (wd != E.wdOn)
            {
                E.wdOn = wd;
                if (wd)
                    slog("[script] WARN frame loop on 3 s watchdog vs=%llu pads=%llu reads/%llu vs=%llu", ull(vs),
                         ull(E.pads), ull(E.wdWindow), ull(E.padVs.size()));
                else
                    slog("[script] watchdog mode ended vs=%llu pads=%llu reads/%llu vs=%llu", ull(vs), ull(E.pads),
                         ull(E.wdWindow), ull(E.padVs.size()));
                if (wd && E.wdFail)
                {
                    finish(kExitFail, "frame loop on 3 s watchdog");
                    return;
                }
            }
        }
        if (E.ownsPad)
            step();
    }

    void onCdRead(uint32_t lsn, uint32_t sectors)
    {
        ps2_vu1new::cdRead(lsn); // TEMP(transformers)
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        ++E.cdSerial;
        E.lastCdVs = E.vs;
        const uint64_t last64 = static_cast<uint64_t>(lsn) + (sectors != 0u ? sectors - 1u : 0u);
        const uint32_t last = static_cast<uint32_t>(std::min<uint64_t>(last64, 0xFFFFFFFFull));
        for (Anchor &a : E.anchors)
        {
            if (lsn <= a.hi && last >= a.lo)
                a.lastSerial = E.cdSerial;
        }
        if (E.iso.empty())
            return;
        // Every file overlapping [lsn, last], starting at the last file that begins at or before lsn.
        auto it = std::upper_bound(E.iso.begin(), E.iso.end(), lsn, [](uint32_t l, const IsoFile &f)
                                   { return l < f.lsn; });
        if (it != E.iso.begin())
            --it;
        for (; it != E.iso.end() && it->lsn <= last; ++it)
        {
            if (static_cast<uint64_t>(lsn) >= static_cast<uint64_t>(it->lsn) + it->sectors)
                continue;
            if (!it->seen)
            {
                it->seen = true;
                it->firstVs = E.vs;
                slog("[script] file %s first vs=%llu pads=%llu", it->path.c_str(), ull(E.vs), ull(E.pads));
            }
            it->lastVs = E.vs;
        }
    }

    void onMpeg(int kind)
    {
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        if (kind == 0)
            ++E.movC;
        else
            ++E.movD;
        slog("[script] movie %s vs=%llu pads=%llu", kind == 0 ? "create" : "delete", ull(E.vs), ull(E.pads));
    }

    void onGameThreadError(const char *what)
    {
        if (!E.active)
            return;
        std::lock_guard<std::mutex> lock(E.mx);
        finish(kExitCrash, std::string("game thread exception: ") + (what ? what : "unknown"));
    }

    bool takeDumpRequest(char *path, size_t cap)
    {
        if (E.dumpPending.load(std::memory_order_acquire) <= 0)
            return false;
        std::lock_guard<std::mutex> lock(E.dumpMx);
        if (E.dumpQueue.empty())
            return false;
        std::snprintf(path, cap, "%s", E.dumpQueue.front().c_str());
        E.dumpQueue.pop_front();
        E.dumpPending.fetch_sub(1, std::memory_order_release);
        return true;
    }

    int exitCode()
    {
        if (!E.active)
            return 0;
        std::lock_guard<std::mutex> lock(E.mx);
        if (E.verdict.load() < 0)
            finish(kExitAborted, "run ended without a verdict (window closed or game thread stopped)");
        return E.verdict.load();
    }
}
