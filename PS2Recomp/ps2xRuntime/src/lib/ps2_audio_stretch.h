#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// qualification (2026-10-09): host-side time-stretch for when the guest runs below real time.
//
// The guest SPU2 produces 48 kHz in GUEST time. When the game runs at speed s < 1 it produces only s seconds of audio
// per wall second, so the host jitter queue ran dry, went silent and re-primed (0.3-0.7 s gaps, ~(1-s) of the time
// silent; playtest #2 REVIEW item 2). Like PCSX2's SPU2 "TimeStretch" sync mode (SoundTouch), this stage measures the
// guest production rate against the device consumption rate and, while it is clearly below real time, stretches the
// audio to that rate with WSOLA (waveform-similarity overlap-add: tempo changes, pitch does not).
//
// - Full speed: bypass. Drained frames go to the jitter queue untouched (bit-identical to PS2X_AUDIO_STRETCH=0).
// - Full-speed DRC owns normal drift. Excess must exceed 4% and 120ms queue pressure for 500ms;
//   deep pressure is target+160ms. Short deficit entry needs 200ms persistence, rejecting 30fps bursts.
// - Recovery uses a 0.975..1.04 long-rate band, 0.94..1.08 short band, 100ms dwell and a wider fill band.
//   Excess correction also exits once total pressure is <=target+45ms with a healthy queue and short rate;
//   a persistently fast movie clock does not have to return to unity. Rate buckets close across wall-clock wakes.
//   Exit only if the source tail fits the queue's headroom; the tail is crossfaded once back to bypass.
// - Tempo is bounded to 0.5..1.25. A 60ms wall-time drought, or exhausted source reserve,
//   releases the retained source once at unity; the queue fades over 4ms and re-primes on fresh PCM.
//   Planned source-starvation fades and unexpected device underruns have separate counters.
// Host-only: runs on the audio device callback after the guest SPU2 ring; never visible to the guest.
// Self-contained (no SoundTouch). Only the device callback thread touches this object except stats().
class PS2AudioStretch {
public:
    static constexpr size_t Rate = 48000;
    static constexpr size_t Seq = Rate * 40 / 1000;    // WSOLA sequence (output per step = Seq - Ovl = 30 ms)
    static constexpr size_t Seek = Rate * 15 / 1000;   // search window for the best splice point
    static constexpr size_t Ovl = Rate * 10 / 1000;    // crossfade length
    static constexpr size_t Need = Seq + Seek;         // input needed for one step
    static constexpr size_t BinFrames = Rate * 50 / 1000, Bins = 20, MinBins = 10, ShortBins = 5;
                                                       // 1 s speed window (0.5 s minimum), 0.25 s short window
    static constexpr size_t InCap = Rate * 2;          // input FIFO bound (frames); overflow forces a flush to bypass
    static constexpr size_t KeepOnEntry = Rate * 20 / 1000; // queue audio left in place when taking back on entry
    static constexpr double EnterRate = 0.975, ExitRate = 0.985, MinTempo = 0.5, MaxTempo = 1.25;
    static constexpr double GainBelow = 0.6, GainAbove = 0.3; // fill correction gains (queue below / above target)
    static constexpr double EnterFill = 0.6, DropRate = 0.9;  // entry gates (see above)

    struct Stats { double rate, tempo; bool active; uint64_t stretchedFrames, enters, flushes, callbackFrames, correctionFrames, fastFrames; };

    // One device callback: `pcm`/`n` = guest frames just drained, `consumed` = frames the device is about to take.
    // Q = the jitter queue (PS2AudioJitter): queued(), target(), append(), takeBack(), holdRate().
    template <class Q>
    void feed(const int16_t* pcm, size_t n, size_t consumed, Q& q, int64_t nowUs = steadyUs()) {
        auto sink = [&q](const int16_t* p, size_t k) { q.append(p, k); };
        const size_t fill = q.queued(), target = q.target();
        meter(n, consumed, nowUs);
        // TEMP(transformers): device subcallbacks share a wall clock, never count empty 512-frame chunks as drought.
        if (n || m_lastInputUs < 0) m_lastInputUs = nowUs;
        const int64_t droughtUs = std::max<int64_t>(0, nowUs - m_lastInputUs);
        if (n) m_stalled=false;
        // TEMP(transformers): an exhausted bypass reserve also fades deliberately; empty subcallbacks
        // with a healthy queue cannot trigger this path, regardless of device period size.
        if (m_guard && !m_active && !n && m_bins>=2 && fill<consumed+2) m_stalled=true;
        m_callbacks.fetch_add(consumed, std::memory_order_relaxed);
        if (m_active) m_correction.fetch_add(consumed, std::memory_order_relaxed);
        if (m_pubTempo.load(std::memory_order_relaxed) > 1.25) m_fast.fetch_add(consumed, std::memory_order_relaxed);
        // A stall drains the remaining source once at unity, then the jitter queue's 4ms fade handles silence.
        // No grains are synthesized from an old voice/music tail. Fresh arrivals use the normal re-prime blend.
        q.guestStall(m_guard && (droughtUs >= 60000 || m_stalled));
        if (m_guard && (droughtUs >= 60000 || m_stalled)) {
            if (m_active) { splice(sink); q.holdRate(false); }
            sink(pcm, n); return;
        }
        const double rate = m_bins >= MinBins && m_sumCons ? double(m_sumProd) / double(m_sumCons) : 1.0;
        m_pubRate.store(rate, std::memory_order_relaxed);
        m_deficitFrames = shortRate() < 0.94 ? m_deficitFrames + consumed : 0; // TEMP(transformers): reject frame-burst meter noise
        if (!m_active) {
            const bool slow = rate < EnterRate && double(fill) < EnterFill * double(target);
            const bool drop = rate < 0.96 && shortRate() < DropRate && fill < target && m_deficitFrames >= Rate / 5;
            // TEMP(transformers): pitch-preserving catch-up for excess production.
            const bool excess = m_control && rate > 1.04 && fill > target + Rate * 120 / 1000;
            m_excessFrames = excess ? m_excessFrames + consumed : 0;
            const bool deep = m_control && fill > target + Rate * 160 / 1000;
            // TEMP(transformers): enter before the queue loses the WSOLA priming window.
            // The old 500ms warm-up and 60% threshold could leave <Need+KeepOnEntry.
            const bool reserve = m_recovery && m_bins >= 2 && m_deficitFrames >= Rate / 5 &&
                fill + n < Need + KeepOnEntry + consumed * 2 && shortRate() < 0.94;
            if ((m_bins >= MinBins && (slow || drop || (excess && m_excessFrames >= Rate / 2) || deep)) || reserve) {
                m_active = true; m_exiting = false; m_primed = false; m_skipFract = 0.0;
                m_inHead = 0; m_inCount = 0; m_recoveredFrames = 0; m_totalConsumed=0; m_haveWindow=false;
                m_enters.fetch_add(1, std::memory_order_relaxed);
                push(m_back.data(), q.takeBack(m_back.data(), Need, KeepOnEntry));
                q.holdRate(true);
            } else {
                m_pubActive.store(false, std::memory_order_relaxed);
                m_pubTempo.store(1.0, std::memory_order_relaxed);
                sink(pcm, n);
                return;
            }
        }
        push(pcm, n);
        m_pubActive.store(true, std::memory_order_relaxed);
        const bool backlog = m_inCount + q.queued() > target + Rate * 180 / 1000;
        // TEMP(transformers): hysteresis leaves ordinary production jitter to the DRC.
        if (shortRate() > 0.94 && shortRate() < 1.08 && rate > 0.975 && rate < 1.04 && q.queued() >= target / 2)
            m_recoveredFrames += consumed;
        else m_recoveredFrames = 0;
        // TEMP(transformers): excess correction ends on pressure, not on the source clock returning to unity.
        // A movie clock can remain 5% fast throughout a film. Use separated queue watermarks so the DRC
        // owns the intervening callbacks instead of holding WSOLA active for the entire movie.
        const bool pressureRecovered = rate >= 0.975 && shortRate() >= 0.975 &&
            q.queued() >= target * 3 / 4 && q.queued() + m_inCount <= target + Rate * 45 / 1000;
        m_exiting = pressureRecovered || (rate > 0.975 && !backlog && (!m_control || (rate < 1.04 && q.queued() <= target + Rate * 100 / 1000))
            && (!m_recovery || m_recoveredFrames >= Rate / 10));
        if ((m_exiting && m_inCount >= Ovl + Seek) || m_inCount > InCap - Rate / 2) {
            if (!m_exiting) m_flushes.fetch_add(1, std::memory_order_relaxed);
            splice(sink);
            q.holdRate(false);
            return;
        }
        const double fast = shortRate(), ff = std::max(MinTempo, fast >= rate ? fast : 0.5 * (fast + rate));
        // TEMP(transformers): during drought emit a shorter valid sequence instead of stranding 20-40ms.
        while (m_inCount >= (m_recovery ? (m_guard ? Ovl * 2 : Seq) : Need)) {
            const size_t level = q.queued();
            // TEMP(transformers): the 15ms search margin is optional; do not go silent with a full sequence ready.
            if (m_inCount < Need && (!m_recovery || level >= target / 2)) break;
            if (target && level >= target + target / 2) break;   // enough queued: keep the rest for later callbacks
            const double ahead = double(level) + double(m_inCount > Need ? m_inCount - Need : 0) / ff;
            const double corr = target ? std::clamp((ahead - double(target)) / double(target), -1.0, 1.5) : 0.0;
            // TEMP(transformers): never turn source drought into a 10x stretched voice/music tail.
            const double tempo = std::clamp(ff * (1.0 + (corr < 0.0 ? GainBelow : GainAbove) * corr), MinTempo, MaxTempo);
            m_pubTempo.store(tempo, std::memory_order_relaxed);
            // TEMP(transformers): no stationary/backward grain starts; shrink the sequence when future input is absent.
            size_t minimumOffset=0;
            if(m_progress && m_guard && m_recovery && m_haveWindow) {
                const size_t minimumStart=m_lastWindow+std::max<size_t>(1,m_lastAdvance/2);
                minimumOffset=minimumStart>m_totalConsumed ? minimumStart-m_totalConsumed : 0;
                if(m_inCount < minimumOffset+Ovl*2)break;
            }
            step(tempo, sink, m_guard && m_recovery ? std::min(m_progress ? size_t(Rate*30/1000) : Seq, m_inCount-minimumOffset) : Seq, minimumOffset);
        }
        // TEMP(transformers): source reserve can run out before the wall deadline on a sudden guest slowdown.
        // Release the kept tail once at unity and deliberately fade instead of waiting for a device underrun.
        if(m_guard && q.queued()<consumed+2 && m_inCount<Ovl*2) {
            splice(sink);q.holdRate(false);q.guestStall(true);m_stalled=true;
        }
    }

    // PS2X_AUDIO_STRETCH=0: only measure the guest speed (for the stats line); the audio is not touched.
    void observe(size_t n, size_t consumed) noexcept {
        meter(n, consumed, steadyUs());
        m_pubRate.store(m_bins >= MinBins && m_sumCons ? double(m_sumProd) / double(m_sumCons) : 1.0, std::memory_order_relaxed);
    }

    size_t diagnosticInputFrames() const noexcept { return m_inCount; } // TEMP(transformers)
    Stats stats() const noexcept {
        return {m_pubRate.load(std::memory_order_relaxed), m_pubTempo.load(std::memory_order_relaxed),
            m_pubActive.load(std::memory_order_relaxed), m_stretched.load(std::memory_order_relaxed),
            m_enters.load(std::memory_order_relaxed), m_flushes.load(std::memory_order_relaxed), m_callbacks.load(std::memory_order_relaxed),
            m_correction.load(std::memory_order_relaxed), m_fast.load(std::memory_order_relaxed)};
    }

private:
    static int64_t steadyUs() noexcept { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    double shortRate() const noexcept {
        uint64_t p = 0, c = 0;
        for (size_t k = 1; k <= ShortBins && k <= m_bins; ++k) {
            const size_t i = (m_binPos + Bins - k) % Bins; p += m_prod[i]; c += m_cons[i];
        }
        return c ? double(p) / double(c) : 0.0;
    }
    void meter(size_t n, size_t consumed, int64_t nowUs) noexcept {
        // TEMP(transformers): complete buckets before the next wake's arrivals. A device wake's
        // entire burst stays together; no first-chunk producer / last-chunk consumer phase bias.
        if (m_meterUs < 0) m_meterUs = nowUs;
        if (nowUs - m_meterUs >= 50000 && m_binCons) {
            if (m_bins == Bins) { m_sumProd -= m_prod[m_binPos]; m_sumCons -= m_cons[m_binPos]; } else ++m_bins;
            m_prod[m_binPos] = m_binProd; m_cons[m_binPos] = m_binCons;
            m_sumProd += m_binProd; m_sumCons += m_binCons;
            m_binPos = (m_binPos + 1) % Bins; m_binProd = 0; m_binCons = 0;
            m_meterUs += ((nowUs - m_meterUs) / 50000) * 50000;
        }
        m_binProd += n; m_binCons += consumed;
    }
    float& at(size_t i, size_t c) noexcept { return m_in[((m_inHead + i) % InCap) * 2 + c]; }
    void push(const int16_t* pcm, size_t n) noexcept {
        if (n > InCap - m_inCount) { pcm += (n - (InCap - m_inCount)) * 2; n = InCap - m_inCount; } // cannot happen (flush first)
        for (size_t i = 0; i < n; ++i) {
            const size_t t = (m_inHead + m_inCount + i) % InCap;
            m_in[t * 2] = pcm[i * 2]; m_in[t * 2 + 1] = pcm[i * 2 + 1];
        }
        m_inCount += n;
    }
    void consume(size_t n) noexcept { n = std::min(n, m_inCount); m_totalConsumed += n; m_inHead = (m_inHead + n) % InCap; m_inCount -= n; }
    template <class Sink> void emit(float l, float r, Sink& sink) {
        m_out[m_outN * 2] = int16_t(std::clamp<long>(std::lround(l), -32768, 32767));
        m_out[m_outN * 2 + 1] = int16_t(std::clamp<long>(std::lround(r), -32768, 32767));
        if (++m_outN == OutChunk) flushOut(sink);
    }
    template <class Sink> void flushOut(Sink& sink) {
        if (m_outN) { sink(m_out.data(), m_outN); m_stretched.fetch_add(m_outN, std::memory_order_relaxed); m_outN = 0; }
    }
    // Best splice offset in [0, limit): normalised cross-correlation of the kept tail (m_mid) with the input, on L+R.
    size_t seekBest(size_t limit, size_t base = 0) noexcept {
        if (limit <= 1) return base;
        for (size_t i = 0; i < Ovl + limit && i < m_inCount; ++i) m_mono[i] = at(base+i, 0) + at(base+i, 1);
        auto score = [&](size_t p) {
            double num = 0.0, den = 1e-3;
            for (size_t i = 0; i < Ovl; ++i) { const double x = m_mono[p + i]; num += m_midMono[i] * x; den += x * x; }
            return num / std::sqrt(den);
        };
        size_t best = 0; double bestScore = -1e300;
        for (size_t p = 0; p < limit; p += 4) { const double s = score(p); if (s > bestScore) { bestScore = s; best = p; } }
        const size_t lo = best >= 3 ? best - 3 : 0, hi = std::min(limit - 1, best + 3);
        for (size_t p = lo; p <= hi; ++p) { const double s = score(p); if (s > bestScore) { bestScore = s; best = p; } }
        return base+best;
    }
    void keepMid(size_t from) noexcept {
        for (size_t i = 0; i < Ovl; ++i) {
            m_mid[i * 2] = at(from + i, 0); m_mid[i * 2 + 1] = at(from + i, 1);
            m_midMono[i] = m_mid[i * 2] + m_mid[i * 2 + 1];
        }
    }
    template <class Sink> void crossfade(size_t offset, Sink& sink) {
        for (size_t i = 0; i < Ovl; ++i) {
            const float w = float(i) / float(Ovl);
            emit(m_mid[i * 2] * (1.0f - w) + at(offset + i, 0) * w, m_mid[i * 2 + 1] * (1.0f - w) + at(offset + i, 1) * w, sink);
        }
    }
    // One WSOLA step (SoundTouch TDStretch layout): crossfade the kept tail into the best-matching input position, copy
    // the sequence body, keep its tail for the next splice, advance the input by tempo x (Seq - Ovl).
    template <class Sink> void step(double tempo, Sink& sink, size_t sequence, size_t minimumOffset) {
        size_t offset = 0;
        if (!m_primed) { keepMid(0); m_primed = true; } // first step continues the input exactly
        else offset = seekBest(std::min(Seek > minimumOffset ? Seek-minimumOffset : size_t(1),m_inCount - sequence - minimumOffset + 1),minimumOffset); // TEMP(transformers): bounded emergency search
        m_lastWindow=m_totalConsumed+offset; m_haveWindow=true;
        m_lastTail=m_lastWindow+sequence-Ovl; // TEMP(transformers): first retained, not-yet-emitted source frame
        crossfade(offset, sink);
        for (size_t i = offset + Ovl; i < offset + sequence - Ovl; ++i) emit(at(i, 0), at(i, 1), sink);
        keepMid(offset + sequence - Ovl);
        m_skipFract += tempo * double(sequence - Ovl);
        const size_t skip = size_t(m_skipFract);
        m_skipFract -= double(skip);
        m_lastAdvance=std::min(skip,m_inCount);
        consume(skip);
        flushOut(sink);
    }
    // Leave stretch mode: splice the kept tail onto the raw input once more, then pass the rest straight through.
    template <class Sink> void splice(Sink& sink) {
        if (m_primed) {
            // TEMP(transformers): recovery must not search behind the last grain start.
            const size_t base=m_progress && m_haveWindow && m_lastTail>m_totalConsumed ? m_lastTail-m_totalConsumed : 0;
            const size_t limit = m_inCount >= base+Ovl ? std::min(Seek, m_inCount - base - Ovl + 1) : 0;
            if (limit) {
                const size_t offset = seekBest(limit,base);
                crossfade(offset, sink);
                consume(offset + Ovl);
            } else {
                for (size_t i = 0; i < Ovl; ++i) emit(m_mid[i * 2], m_mid[i * 2 + 1], sink);
                if(m_progress)consume(std::min(m_inCount,base+Ovl)); // TEMP(transformers): do not replay an already-emitted body
            }
        }
        while (m_inCount) { emit(at(0, 0), at(0, 1), sink); consume(1); }
        flushOut(sink);
        m_active = false; m_exiting = false; m_primed = false;
        m_pubActive.store(false, std::memory_order_relaxed);
        m_pubTempo.store(1.0, std::memory_order_relaxed);
    }

    static constexpr size_t OutChunk = 1024;
    std::vector<float> m_in = std::vector<float>(InCap * 2);
    size_t m_inHead = 0, m_inCount = 0, m_recoveredFrames = 0, m_excessFrames = 0, m_deficitFrames = 0;
    std::array<float, Ovl * 2> m_mid{};
    std::array<double, Ovl> m_midMono{};
    std::array<float, Ovl + Seek> m_mono{};
    std::array<int16_t, OutChunk * 2> m_out{};
    std::array<int16_t, Need * 2> m_back{};
    size_t m_outN = 0;
    size_t m_totalConsumed=0,m_lastWindow=0,m_lastAdvance=0,m_lastTail=0;
    bool m_haveWindow=false;
    bool m_stalled=false; // TEMP(transformers): hold the fade policy until fresh PCM arrives
    std::array<size_t, Bins> m_prod{}, m_cons{};
    size_t m_binPos = 0, m_bins = 0, m_binProd = 0, m_binCons = 0;
    uint64_t m_sumProd = 0, m_sumCons = 0;
    double m_skipFract = 0.0;
    // TEMP(transformers): same-binary A/B control.
    const bool m_control = [] { const char* v = std::getenv("PS2X_AUDIO_QUEUE_CONTROL"); return !v || std::strcmp(v, "0") != 0; }();
    const bool m_recovery = [] { const char* v = std::getenv("PS2X_AUDIO_RECOVERY"); return !v || std::strcmp(v, "0") != 0; }(); // TEMP(transformers)
    const bool m_guard = [] { const char* v = std::getenv("PS2X_AUDIO_STARVE_GUARD"); return !v || std::strcmp(v, "0") != 0; }(); // TEMP(transformers): same-binary final3 control
    const bool m_progress = [] { const char* v=std::getenv("PS2X_AUDIO_GRAIN_PROGRESS"); return !v || std::strcmp(v,"0")!=0; }(); // TEMP(transformers)
    bool m_active = false, m_exiting = false, m_primed = false;
    std::atomic<double> m_pubRate{1.0}, m_pubTempo{1.0};
    std::atomic<bool> m_pubActive{false};
    int64_t m_lastInputUs = -1;
    int64_t m_meterUs = -1; // TEMP(transformers)
    std::atomic<uint64_t> m_callbacks{0}, m_correction{0}, m_fast{0};
    std::atomic<uint64_t> m_stretched{0}, m_enters{0}, m_flushes{0};
};

