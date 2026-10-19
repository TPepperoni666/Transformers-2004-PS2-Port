#include "runtime/ps2_settings.h"
#pragma once
#include "ps2_host_backend.h"
#include "ps2_audio_jitter.h"
#include "ps2_audio_stretch.h"
#include "ps2_audio_present_clock.h" // TEMP(transformers)
#include "ps2_perfscope.h" // TEMP(transformers): qualification (callback time, jitter stats)
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <thread>

// PS2X_AUDIO_VOLUME=0..1 (default 1.0): host output gain only (after the guest-time SPU2 mix; never seen by the guest).
// Unattended host-path tests use 0 or a tiny value so nothing is audible.
inline float ps2HostAudioVolume()
{
    const char *value = std::getenv("PS2X_AUDIO_VOLUME");
    if (!value || !*value) return 1.0f;
    const float v = std::strtof(value, nullptr);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline bool ps2HostAudioEnabled()
{
    const char *value = std::getenv("PS2X_AUDIO");
    return !value || std::strcmp(value, "0") != 0;
}

// PS2X_AUDIO_STRETCH=1 (default) / 0: time-stretch the host output when the guest runs below real time
// (ps2_audio_stretch.h, qualification). 0 = the plain jitter queue (silent re-prime gaps when slow).
inline bool ps2HostAudioStretchEnabled()
{
    const char *value = std::getenv("PS2X_AUDIO_STRETCH");
    return !value || !*value || std::strcmp(value, "0") != 0;
}

// TEMP(transformers): PS2X_AUDIO_WAV=<path> dumps exactly what the host hands the audio device (after the jitter queue
// and time-stretch, before the device volume, so PS2X_AUDIO_VOLUME=0 runs still record full scale). 48 kHz s16 stereo.
// PS2X_AUDIO_WAV_MAX_S caps the length (default 1200 s). The device callback only copies into a lock-free ring; the
// render thread (pump) writes the file.
class PS2HostWavDump
{
public:
    static std::unique_ptr<PS2HostWavDump> fromEnv(const char* key = "PS2X_AUDIO_WAV")
    {
        const char *path = std::getenv(key);
        if (!path || !*path) return nullptr;
        std::FILE *f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "[audio] cannot open PS2X_AUDIO_WAV %s\n", path); return nullptr; }
        const char *maxS = std::getenv("PS2X_AUDIO_WAV_MAX_S");
        const double seconds = maxS && *maxS ? std::strtod(maxS, nullptr) : 1200.0;
        std::unique_ptr<PS2HostWavDump> dump(new PS2HostWavDump(f, uint64_t(seconds > 0.0 ? seconds * 48000.0 : 0.0)));
        std::fprintf(stderr, "[audio] host output WAV -> %s\n", path);
        return dump;
    }
    ~PS2HostWavDump()
    {
        write();
        std::fseek(m_file, 0, SEEK_SET);
        header(uint32_t(m_written * 4));
        std::fclose(m_file);
        if (m_lost.load())
            std::fprintf(stderr, "[audio] WAV dump lost %llu frames (render thread too slow)\n",
                static_cast<unsigned long long>(m_lost.load()));
    }
    // Device callback thread (single producer).
    void push(const int16_t *pcm, size_t frames) noexcept
    {
        const size_t w = m_w.load(std::memory_order_relaxed), r = m_r.load(std::memory_order_acquire);
        const size_t room = Cap - (w - r);
        if (frames > room) { m_lost.fetch_add(frames - room, std::memory_order_relaxed); frames = room; }
        for (size_t i = 0; i < frames; ++i) {
            m_ring[((w + i) % Cap) * 2] = pcm[i * 2];
            m_ring[((w + i) % Cap) * 2 + 1] = pcm[i * 2 + 1];
        }
        m_w.store(w + frames, std::memory_order_release);
    }
    // Render thread (single consumer).
    void write()
    {
        const size_t w = m_w.load(std::memory_order_acquire);
        size_t r = m_r.load(std::memory_order_relaxed);
        while (r != w) {
            const size_t at = r % Cap, n = std::min(w - r, Cap - at);
            const size_t keep = m_max && m_written + n > m_max ? size_t(m_max - std::min(m_max, m_written)) : n;
            if (keep) std::fwrite(&m_ring[at * 2], 4, keep, m_file);
            m_written += keep;
            r += n;
        }
        m_r.store(r, std::memory_order_release);
    }
private:
    static constexpr size_t Cap = 48000 * 4;
    PS2HostWavDump(std::FILE *f, uint64_t maxFrames) : m_file(f), m_max(maxFrames), m_ring(Cap * 2) { header(0); }
    void header(uint32_t data)
    {
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, m_file); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, m_file); };
        std::fwrite("RIFF", 1, 4, m_file); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, m_file);
        u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
        std::fwrite("data", 1, 4, m_file); u32(data);
    }
    std::FILE *m_file;
    uint64_t m_max, m_written = 0;
    std::vector<int16_t> m_ring;
    std::atomic<size_t> m_w{0}, m_r{0};
    std::atomic<uint64_t> m_lost{0};
};

// TEMP(transformers): bounded SPSC telemetry. Callback never does file IO or logging.
class PS2AudioTrace {
public:
    struct Event { int64_t us; uint64_t output,source; size_t drained,frames,input; PS2AudioJitter::Stats q; };
    static std::unique_ptr<PS2AudioTrace> fromEnv() {
        const char* path=std::getenv("PS2X_AUDIO_TRACE");
        if (!path || !*path) return nullptr;
        auto f=std::fopen(path,"w"); if (!f) return nullptr;
        return std::unique_ptr<PS2AudioTrace>(new PS2AudioTrace(f));
    }
    void push(const Event& e) noexcept {
        size_t w=m_w.load(std::memory_order_relaxed),r=m_r.load(std::memory_order_acquire);
        if(w-r==Cap) {m_lost.fetch_add(1);return;}
        m_events[w%Cap]=e;m_w.store(w+1,std::memory_order_release);
    }
    void write() {
        size_t r=m_r.load(std::memory_order_relaxed),w=m_w.load(std::memory_order_acquire);
        while(r<w) {const auto& e=m_events[r%Cap];
            std::fprintf(m_file,"%lld,%llu,%llu,%zu,%zu,%zu,%zu,%zu,%llu,%llu,%llu\n",
                (long long)e.us,(unsigned long long)e.output,(unsigned long long)e.source,e.drained,e.frames,e.input,
                e.q.queuedFrames,e.q.targetFrames,(unsigned long long)e.q.underruns,(unsigned long long)e.q.droppedFrames,(unsigned long long)e.q.silenceFrames);++r;}
        m_r.store(r,std::memory_order_release);std::fflush(m_file);
    }
    ~PS2AudioTrace() {write();std::fclose(m_file);if(m_lost.load())std::fprintf(stderr,"[aud2:trace] lost=%llu\n",(unsigned long long)m_lost.load());}
private:
    static constexpr size_t Cap=8192;
    explicit PS2AudioTrace(FILE* f):m_file(f) {std::fprintf(f,"steady_us,output_frame,source_frame,drained,callback_frames,stretch_input,queued,target,underruns,dropped,silence\n");}
    FILE* m_file;std::array<Event,Cap> m_events{};std::atomic<size_t> m_w{0},m_r{0},m_lost{0};
};

extern "C" unsigned int ps2AudioDeviceLatencyUs(void); // TEMP(transformers): source-built raylib bridge

class PS2HostAudio
{
public:
    using Drain = std::function<size_t(int16_t*, size_t)>;
    explicit PS2HostAudio(Drain drain) : m_drain(std::move(drain))
    {
#if !defined(PLATFORM_VITA)
        // TEMP(transformers): silent sink uses the same consumer without opening a device.
        const char* nullSink = std::getenv("PS2X_AUDIO_NULL");
        if (!ps2HostAudioEnabled() && nullSink && std::strcmp(nullSink, "1") == 0) {
            m_stretchOn = ps2HostAudioStretchEnabled();
            m_wav = PS2HostWavDump::fromEnv();
            m_inputWav = PS2HostWavDump::fromEnv("PS2X_AUDIO_INPUT_WAV");
            m_trace = PS2AudioTrace::fromEnv();
            const char* stats = std::getenv("PS2X_AUDIO_STATS");
            m_stats = stats && std::strcmp(stats, "1") == 0;
            m_null = true;
            m_nullThread = std::thread([this] {
                // TEMP(transformers): match the real miniaudio consumer's default priority.
                extern int ps2AudioNullSetPriority();
                const int priority=ps2AudioNullSetPriority();
                std::fprintf(stderr,"[aud2:null] thread_priority=%d (miniaudio default=highest)\n",priority);
                std::array<int16_t, 480 * 2> output{};
                auto deadline = std::chrono::steady_clock::now();
                while (!m_nullStop.load(std::memory_order_acquire)) {
                    deadline += std::chrono::milliseconds(10);
                    process(output.data(), 480);
                    std::this_thread::sleep_until(deadline);
                }
            });
            std::fprintf(stderr, "[audio] silent null sink: 480 frames/10ms stretch=%d\n", int(m_stretchOn));
            return;
        }
        if (!ps2HostAudioEnabled() || !IsAudioDeviceReady()) return;
        SetAudioStreamBufferSizeDefault(480); // Small stream allocation; callback frame counts are device/converter driven.
        m_stream = LoadAudioStream(48000, 16, 2);
        ps2AudioPresentClock::deviceUs.store(ps2AudioDeviceLatencyUs(),std::memory_order_relaxed);
        SetAudioStreamBufferSizeDefault(0);
        if (!m_stream.buffer) return;
        m_stretchOn = ps2HostAudioStretchEnabled();
        m_wav = PS2HostWavDump::fromEnv();
            m_inputWav = PS2HostWavDump::fromEnv("PS2X_AUDIO_INPUT_WAV");
            m_trace = PS2AudioTrace::fromEnv();
        // raylib's callback has no userdata. This runtime owns one SPU2 stream.
        PS2HostAudio* expected = nullptr;
        if (!s_owner.compare_exchange_strong(expected, this)) {
            UnloadAudioStream(m_stream); m_stream = {}; m_wav.reset(); m_inputWav.reset(); m_trace.reset(); return;
        }
        SetAudioStreamCallback(m_stream, callback);
        SetAudioStreamVolume(m_stream, 1.0f);
        PlayAudioStream(m_stream);
#endif
        // TEMP(transformers): host-only diagnostics, no IOP or guest counters.
        const char* stats = std::getenv("PS2X_AUDIO_STATS");
        m_stats = stats && std::strcmp(stats, "1") == 0;
#if !defined(PLATFORM_VITA)
        if (m_stats && m_stream.buffer)
            std::fprintf(stderr, "[audio] time-stretch %s (PS2X_AUDIO_STRETCH)\n", m_stretchOn ? "on" : "off");
#endif
    }
    ~PS2HostAudio()
    {
#if !defined(PLATFORM_VITA)
        if (m_null) {
            m_nullStop.store(true, std::memory_order_release);
            m_nullThread.join();
            if (m_stats) logStats("[audio] final null");
            m_wav.reset(); m_inputWav.reset(); m_trace.reset();
        }
        if (m_stream.buffer) {
            StopAudioStream(m_stream);
            // Detach under raylib's audio mutex; any in-flight callback completes
            // before this returns. Destroy the queue only after unloading the stream.
            SetAudioStreamCallback(m_stream, nullptr);
            UnloadAudioStream(m_stream);
            s_owner.store(nullptr, std::memory_order_release);
            if (m_stats) logStats("[audio] final");
            m_wav.reset(); m_inputWav.reset(); m_trace.reset(); // flushes and closes the WAV after the callback is gone
        }
#endif
    }
    PS2HostAudio(const PS2HostAudio&) = delete;
    PS2HostAudio& operator=(const PS2HostAudio&) = delete;
    // TEMP(transformers): qualification - jitter-buffer stats (atomics; any thread). False when no device stream.
    bool perfStats(ps2_perfscope::AudioStats &out) const
    {
#if !defined(PLATFORM_VITA)
        if (!m_stream.buffer && !m_null) return false;
        const auto s = m_jitter.stats();
        out.underruns = s.underruns;
        out.droppedFrames = s.droppedFrames;
        out.queuedMs = s.queuedFrames / 48.0;
        out.targetMs = s.targetFrames / 48.0;
        out.drcPercent = (s.ratio - 1.0) * 100.0;
        return true;
#else
        (void)out;
        return false;
#endif
    }
    void pump()
    {
#if !defined(PLATFORM_VITA)
        if (m_stream.buffer) {
            const float gain = ps2x::settings::getBool("audio.mute_on_focus_loss") && !IsWindowFocused() ? 0.0f : float(ps2x::settings::getFloat("audio.master_volume"));
            if (gain != m_pcGain) { SetAudioStreamVolume(m_stream,gain); m_pcGain=gain; }
        }
#endif
#if !defined(PLATFORM_VITA)
        if (!m_stream.buffer && !m_null)
#endif
            m_drain(m_scratch.data(), RingFrames); // Silent/no-device runs retain old discard path.
        // TEMP(transformers): log on render thread, never printf on device callback.
        const auto now = std::chrono::steady_clock::now();
#if !defined(PLATFORM_VITA)
        if (m_wav) m_wav->write();
        if (m_inputWav) m_inputWav->write();
        if (m_trace) m_trace->write();
#endif
        if (m_stats && now - m_lastLog >= std::chrono::seconds(10)) {
            logStats("[audio]");
            m_lastLog = now;
        }
    }
private:
    // TEMP(transformers): cumulative counters. silence_ms = output time spent silent while re-priming (start-up and
    // underruns); speed = guest audio production / real time over the last 2 s; stretch_ms = output produced by the
    // time-stretch stage; enters = times it engaged; flushes = emergency exits on a > 1.5 s input backlog.
    void logStats(const char *tag)
    {
        const auto s = m_jitter.stats();
        const auto t = m_stretch.stats();
        std::fprintf(stderr, "%s underruns=%llu dropped_samples=%llu latency_ms=%.1f target_ms=%.1f drc=%+.2f%% silence_ms=%.0f"
            " speed=%.3f stretch=%s tempo=%.3f stretch_ms=%.0f enters=%llu flushes=%llu correction_pct=%.2f over125_pct=%.2f device_ms=%.1f stall_fades=%llu\n", tag,
            static_cast<unsigned long long>(s.underruns),
            static_cast<unsigned long long>(s.droppedFrames * 2),
            s.queuedFrames / 48.0, s.targetFrames / 48.0, (s.ratio - 1.0) * 100.0, s.silenceFrames / 48.0,
            t.rate, !m_stretchOn ? "off" : (t.active ? "active" : "idle"), t.tempo, t.stretchedFrames / 48.0,
            static_cast<unsigned long long>(t.enters), static_cast<unsigned long long>(t.flushes),
            t.callbackFrames ? 100.0*t.correctionFrames/t.callbackFrames : 0.0,
            t.callbackFrames ? 100.0*t.fastFrames/t.callbackFrames : 0.0,
            ps2AudioPresentClock::deviceUs.load(std::memory_order_relaxed)/1000.0, static_cast<unsigned long long>(s.stallFades));
    }
    static constexpr size_t RingFrames = 32768;
#if !defined(PLATFORM_VITA)
    static void callback(void* buffer, unsigned int frames)
    {
        ps2_perfscope::registerThread(ps2_perfscope::LaneAudio); // TEMP(transformers) qualification: once per device thread
        ps2_perfscope::Scope perfCb(ps2_perfscope::AuCallback);  // TEMP(transformers) qualification
        auto* self = s_owner.load(std::memory_order_acquire);
        if (!self) { std::memset(buffer, 0, size_t(frames)*4); return; }
        self->process(static_cast<int16_t*>(buffer), frames);
    }
    // TEMP(transformers): sole consumer path shared by real and null sinks.
    void process(int16_t* buffer, size_t frames)
    {
        // One bounded snapshot of the existing lock-free SPU2 ring. The render
        // thread never drains while this callback is installed.
        const size_t n = m_drain(m_scratch.data(), RingFrames);
        if (m_inputWav) m_inputWav->push(m_scratch.data(), n); // TEMP(transformers): source samples
        if (m_stretchOn)
            m_stretch.feed(m_scratch.data(), n, frames, m_jitter);
        else {
            m_jitter.append(m_scratch.data(), n);
            m_stretch.observe(n, frames);
        }
        m_jitter.render(buffer, frames);
        // TEMP(transformers): queued output + WSOLA source backlog (including its kept overlap).
        const auto stretch=m_stretch.stats();
        const double pending=m_jitter.queued() + (stretch.active ?
            double(m_stretch.diagnosticInputFrames()) / std::max(0.1,stretch.tempo) + PS2AudioStretch::Ovl : 0.0);
        ps2AudioPresentClock::pipelineUs.store(uint32_t(std::min(400000.0,pending * 1000000.0 / 48000.0)),std::memory_order_relaxed);
        if (m_wav) m_wav->push(buffer, frames);
        // TEMP(transformers): clock denotes callback START sample, captured after render; null/hardware handoff only.
        if (m_trace) m_trace->push({std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
            m_observerFrames,m_sourceFrames,n,frames,m_stretch.diagnosticInputFrames(),m_jitter.stats()});
        m_observerFrames += frames; m_sourceFrames += n;
    }
    inline static std::atomic<PS2HostAudio*> s_owner{nullptr};
    float m_pcGain = -1.0f;
    AudioStream m_stream{};
    // TEMP(transformers): null consumer is joined before queue destruction.
    bool m_null = false;
    std::atomic<bool> m_nullStop{false};
    std::thread m_nullThread;
    std::unique_ptr<PS2HostWavDump> m_wav, m_inputWav;
    std::unique_ptr<PS2AudioTrace> m_trace;
    uint64_t m_observerFrames=0, m_sourceFrames=0; // TEMP(transformers)
#endif
    Drain m_drain;
    PS2AudioJitter m_jitter;
    PS2AudioStretch m_stretch;
    bool m_stretchOn = false;
    std::array<int16_t, RingFrames*2> m_scratch{};
    bool m_stats = false;
    std::chrono::steady_clock::time_point m_lastLog = std::chrono::steady_clock::now();
};
