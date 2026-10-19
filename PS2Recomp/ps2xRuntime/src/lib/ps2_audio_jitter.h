#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Only the device callback owns this queue. The upstream SPU2 SPSC ring remains
// the sole producer/consumer boundary; no producer index is ever overwritten.
//
// fmvaudio (2026-10-09): the guest SPU2 is clocked by guest time, which is never exactly host real time (it runs
// ~0.5% fast at a nominal 60 vsync/s, bursts above 100% after stalls, and arrives in per-frame bursts). The old queue
// dropped every frame above target+25 ms with a 4 ms blend, i.e. a small skip on nearly every device callback during
// movies (audible crackle). Now a dynamic-rate-control resampler (proportional, up to +-2%) absorbs drift and burstiness,
// and frames are only dropped on a real overflow (catch-up bursts), far above the target.
// audiostretch (2026-10-09): silenceFrames counts output frames rendered while waiting to re-prime (start-up and
// underruns; the 4 ms fades are not counted). queued()/target()/takeBack()/holdRate() serve the time-stretch stage
// (ps2_audio_stretch.h); with PS2X_AUDIO_STRETCH=0, or while it is idle, none of them changes the queue's behaviour.
class PS2AudioJitter {
public:
    static constexpr size_t Rate = 48000, Capacity = Rate * 600 / 1000;
    static constexpr size_t Fade = Rate * 4 / 1000;
    static constexpr size_t MinTarget = Rate * 100 / 1000, MaxTarget = Rate * 300 / 1000;
    static constexpr size_t Headroom = Rate * 200 / 1000;   // hard-drop threshold above the target
    static constexpr double MaxAdjust = 0.02;               // DRC range +-2% (proportional: 0.5% drift -> 0.5%)
    static constexpr double FullScale = Rate * 80 / 1000.0;  // fill error (frames) that reaches MaxAdjust
    static constexpr size_t HoldResume = Rate * 50 / 1000;  // re-prime level while time-stretch is active
    struct Stats { uint64_t underruns, droppedFrames; size_t queuedFrames, targetFrames; double ratio; uint64_t silenceFrames, stallFades; };
    void append(const int16_t* pcm, size_t frames) noexcept {
        const size_t limit = std::min(Capacity, m_target + Headroom);
        const size_t discard = m_count + frames > limit ? m_count + frames - limit : 0;
        if (discard) {
            const size_t old = std::min(discard, m_count);
            m_head = (m_head + old) % Capacity; m_count -= old;
            pcm += (discard - old) * 2; frames -= discard - old;
            m_dropped.fetch_add(discard, std::memory_order_relaxed);
            // TEMP(transformers): preserve overflow pressure.
            m_avg = double(m_control ? m_count + frames : std::min(m_count + frames, m_target));
            beginBlend();
        }
        for (size_t i = 0; i < frames; ++i) {
            const size_t tail = (m_head + m_count) % Capacity;
            m_pcm[tail*2] = pcm[i*2]; m_pcm[tail*2+1] = pcm[i*2+1]; ++m_count;
        }
    }
    void render(int16_t* out, size_t frames) noexcept {
        // Smoothed fill level (time constant ~1 s at typical 5-20 ms callbacks) drives the resampling ratio.
        if (!m_waiting) {
            m_avg += (double(m_count) - m_avg) * std::min(1.0, double(frames) / double(Rate));
            const double err = std::clamp((m_avg - double(m_target)) / FullScale, -1.0, 1.0);
            m_step = m_hold ? 1.0 : 1.0 + MaxAdjust * err;
        }
        for (size_t i = 0; i < frames; ++i) {
            if (m_waiting && m_count >= (m_hold ? std::min(m_target, HoldResume) : m_target)) {
                m_waiting = false; m_frac = 0.0; m_avg = double(m_count); beginBlend();
            }
            if (!m_waiting && m_count < 2) {
                m_waiting = true;
                if (!m_guestStall) m_underruns.fetch_add(1, std::memory_order_relaxed);
                else m_stallFades.fetch_add(1, std::memory_order_relaxed); // TEMP(transformers): explicit planned source-starvation fades
                // TEMP(transformers): a host stall must not permanently ratchet gameplay latency to 300ms.
                if (!m_guestStall) m_target = std::min(m_recovery ? Rate * 125 / 1000 : MaxTarget, m_target + Rate * 25 / 1000);
                m_stable = 0; m_step = 1.0; beginBlend();
            }
            int32_t next[2] = {0, 0};
            if (m_waiting) ++m_silence;
            if (!m_waiting) {
                const size_t h1 = (m_head + 1) % Capacity;
                for (size_t c = 0; c < 2; ++c) {
                    const double a = m_pcm[m_head*2+c], b = m_pcm[h1*2+c];
                    next[c] = int32_t(std::lround(a + (b - a) * m_frac));
                }
                m_frac += m_step;
                while (m_frac >= 1.0 && m_count > 1) {
                    m_frac -= 1.0; m_head = (m_head + 1) % Capacity; --m_count;
                }
                if (++m_stable >= Rate * 30) {
                    m_target = std::max(MinTarget, m_target - Rate * 5 / 1000);
                    m_stable = 0;
                }
            }
            for (size_t c = 0; c < 2; ++c) {
                // Blend from the last output on recovery/drop; on starvation this
                // becomes a 4 ms ramp to zero, including across callback boundaries.
                if (m_blend) next[c] = int32_t((int64_t(m_anchor[c]) * int64_t(m_blend) +
                    int64_t(next[c]) * int64_t(Fade - m_blend)) / int64_t(Fade));
                next[c] = std::clamp(next[c], -32768, 32767);
                out[i*2+c] = int16_t(next[c]); m_last[c] = next[c];
            }
            if (m_blend) --m_blend;
        }
        m_queued.store(m_count, std::memory_order_relaxed);
        m_publishedTarget.store(m_target, std::memory_order_relaxed);
        m_publishedStep.store(m_step, std::memory_order_relaxed);
        m_publishedSilence.store(m_silence, std::memory_order_relaxed);
    }
    // Device-callback thread only (the owner of the queue).
    size_t queued() const noexcept { return m_count; }
    size_t target() const noexcept { return m_target; }
    // Remove up to maxFrames of the newest, not yet played frames (leaving at least `keep`) into dst; returns the count.
    size_t takeBack(int16_t* dst, size_t maxFrames, size_t keep) noexcept {
        if (m_waiting || m_count <= keep + 2) return 0;
        const size_t n = std::min(maxFrames, m_count - std::max<size_t>(keep, 2));
        for (size_t i = 0; i < n; ++i) {
            const size_t at = (m_head + m_count - n + i) % Capacity;
            dst[i*2] = m_pcm[at*2]; dst[i*2+1] = m_pcm[at*2+1];
        }
        m_count -= n;
        return n;
    }
    // While the time-stretch stage is active it regulates the fill level: hold the DRC ratio at exactly 1.0 (no pitch
    // shift), and after an underrun resume at 50 ms instead of the full target (the tempo loop refills the rest).
    void guestStall(bool stall) noexcept { m_guestStall=stall; } // TEMP(transformers): expected silence is not a device underrun
    void holdRate(bool hold) noexcept { m_hold = hold; }
    Stats stats() const noexcept {
        return {m_underruns.load(std::memory_order_relaxed), m_dropped.load(std::memory_order_relaxed),
            m_queued.load(std::memory_order_relaxed), m_publishedTarget.load(std::memory_order_relaxed),
            m_publishedStep.load(std::memory_order_relaxed), m_publishedSilence.load(std::memory_order_relaxed), m_stallFades.load(std::memory_order_relaxed)};
    }
private:
    void beginBlend() noexcept { m_anchor = m_last; m_blend = Fade; }
    std::array<int16_t, Capacity*2> m_pcm{};
    size_t m_head = 0, m_count = 0, m_target = Rate * 125 / 1000;
    size_t m_blend = 0, m_stable = 0;
    uint64_t m_silence = 0;
    double m_frac = 0.0, m_step = 1.0, m_avg = 0.0;
    // TEMP(transformers): same-binary A/B control.
    const bool m_control = [] { const char* v = std::getenv("PS2X_AUDIO_QUEUE_CONTROL"); return !v || std::strcmp(v, "0") != 0; }();
    const bool m_recovery = [] { const char* v = std::getenv("PS2X_AUDIO_RECOVERY"); return !v || std::strcmp(v, "0") != 0; }(); // TEMP(transformers)
    bool m_waiting = true, m_hold = false, m_guestStall = false;
    std::array<int32_t, 2> m_anchor{}, m_last{};
    std::atomic<uint64_t> m_underruns{0}, m_dropped{0}, m_publishedSilence{0};
    std::atomic<uint64_t> m_stallFades{0}; // TEMP(transformers)
    std::atomic<size_t> m_queued{0}, m_publishedTarget{Rate * 125 / 1000};
    std::atomic<double> m_publishedStep{1.0};
};
