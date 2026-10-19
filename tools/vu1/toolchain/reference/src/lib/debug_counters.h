#pragma once
// Transformers port: lightweight cross-module counters for diagnosing the menu stall.
// Not part of upstream PS2Recomp; strip before any PR.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace ps2dbg
{
    inline std::array<std::atomic<uint64_t>, 64> irqRaised{};    // [dmac*32 + cause] dispatchIrq calls
    inline std::array<std::atomic<uint64_t>, 64> irqMasked{};    // raised but cause disabled
    inline std::array<std::atomic<uint64_t>, 64> irqDelivered{}; // handler invocations queued
    inline std::atomic<uint64_t> gsSignalWrites{0}, gsFinishWrites{0}, gsLabelWrites{0}, gsImrWrites{0};
    inline std::atomic<uint64_t> gsLastImr{0};
    inline std::array<std::atomic<uint64_t>, 256> semWaits{}, semSignals{}, semPolls{};
    inline std::atomic<uint64_t> padReads{0}, padLastButtons{0xFFFF};
    inline std::atomic<uint64_t> presentedFrames{0}, vsyncTick{0};
    inline std::atomic<uint64_t> vu1Mscal{0}, vu1Mscnt{0}, primsDrawn{0}, primsTextured{0}, lastPrimFbp{0}, lastPrimZbp{0};
    // PS2X_GSCAPTURE=<frame>[,<frame>...]: log every primitive + dump every distinct texture drawn during those presented frames.
    inline bool gsCaptureActive()
    {
        static const std::vector<uint64_t> frames = []()
        {
            std::vector<uint64_t> out;
            const char *env = std::getenv("PS2X_GSCAPTURE");
            for (const char *p = env; p && *p;)
            {
                char *end = nullptr;
                const unsigned long long v = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                out.push_back(v);
                p = (*end == ',') ? end + 1 : end;
            }
            return out;
        }();
        // PS2X_GSCAPTURE_VS=<guest vsync>:<count>: capture the first <count> primitives drawn at/after that vsync
        // (presented-frame numbers drift between runs and don't map 1:1 to game frames on heavy screens).
        static const std::pair<uint64_t, uint64_t> vsSpec = []()
        {
            const char *env = std::getenv("PS2X_GSCAPTURE_VS");
            if (!env)
                return std::pair<uint64_t, uint64_t>{~0ull, 0ull};
            char *end = nullptr;
            const unsigned long long vs = std::strtoull(env, &end, 10);
            const unsigned long long cnt = (end && *end == ':') ? std::strtoull(end + 1, nullptr, 10) : 20000ull;
            return std::pair<uint64_t, uint64_t>{vs, cnt};
        }();
        static uint64_t vsCaptured = 0;
        if (vsSpec.second && vsyncTick.load(std::memory_order_relaxed) >= vsSpec.first && vsCaptured < vsSpec.second)
        {
            ++vsCaptured;
            return true;
        }
        if (frames.empty())
            return false;
        const uint64_t now = presentedFrames.load(std::memory_order_relaxed);
        for (uint64_t f : frames)
            if (f == now)
                return true;
        return false;
    }

    inline std::string summary()
    {
        std::ostringstream out;
        out << " irq[";
        for (int i = 0; i < 64; ++i)
        {
            const uint64_t r = irqRaised[i].load(), d = irqDelivered[i].load(), m = irqMasked[i].load();
            if (r || d)
                out << (i >= 32 ? "D" : "I") << (i % 32) << ":" << r << "/" << d << "/m" << m << " ";
        }
        out << "] gs[sig=" << gsSignalWrites.load() << " fin=" << gsFinishWrites.load()
            << " lbl=" << gsLabelWrites.load() << " imrW=" << gsImrWrites.load() << " imr=0x" << std::hex
            << gsLastImr.load() << std::dec << "] sem[";
        for (int i = 0; i < 256; ++i)
        {
            const uint64_t w = semWaits[i].load(), s = semSignals[i].load(), p = semPolls[i].load();
            if (w || s || p)
                out << i << ":w" << w << "/s" << s << "/p" << p << " ";
        }
        out << "] vu1[mscal=" << vu1Mscal.load() << " mscnt=" << vu1Mscnt.load() << "] prims[" << primsDrawn.load()
            << " tex=" << primsTextured.load() << " lastFbp=" << lastPrimFbp.load() << " lastZbp=" << lastPrimZbp.load();
        out << "] pad[reads=" << padReads.load() << " btn=0x" << std::hex << padLastButtons.load() << std::dec << "]";
        return out.str();
    }
}
