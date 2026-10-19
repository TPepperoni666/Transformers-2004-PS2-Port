#pragma once
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
// TEMP(transformers): host-only film presentation compensation. No guest clock or scheduling changes.
// An audio-bearing PSS picture is delayed by the current host audio pipeline plus the measured guest PCM lead.
// Elementary M2V previews have no audio timeline and never opt in.
namespace ps2AudioPresentClock {
inline std::atomic<uint64_t> movieTick{0};
inline std::atomic<bool> movieAudio{false};
inline std::atomic<uint32_t> pipelineUs{0};
inline std::atomic<uint32_t> deviceUs{0}; // TEMP(transformers): miniaudio reported period budget
inline bool enabled() {
    static const bool value=[] {const char* p=std::getenv("PS2X_AUDIO_AVSYNC");return p && std::strcmp(p,"1")==0;}();
    return value;
}
inline uint32_t guestLeadUs() {
    static const uint32_t value=[] {const char* p=std::getenv("PS2X_AUDIO_AVSYNC_LEAD_MS");
        if (!p) return 40000u;
        char* end=nullptr; const long ms=std::strtol(p,&end,10);
        return uint32_t(end==p || *end ? 40 : std::clamp(ms,0l,200l))*1000u;}();
    return value;
}
inline uint32_t delayUs() { return uint32_t(std::min<uint64_t>(500000, uint64_t(pipelineUs.load(std::memory_order_relaxed)) + deviceUs.load(std::memory_order_relaxed) + guestLeadUs())); }
}
