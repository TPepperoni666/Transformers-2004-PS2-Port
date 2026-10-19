// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ps2x::iop::detail {
// Emulation thread owns synthesis. One other thread may drain stereo frames.
// Linear interpolation is intentional for this first implementation (no Gaussian table).
class Spu2Mixer {
public:
    Spu2Mixer(std::vector<uint16_t>& regs, std::vector<uint8_t>& ram);
    ~Spu2Mixer();
    void reset(uint64_t cycle);
    void advanceTo(uint64_t cycle);
    [[nodiscard]] uint64_t nextSampleCycle() const noexcept { return m_sampleCycle + 768u; }
    // TEMP(transformers): producer-thread capture endpoint; synthesis remains unchanged.
    void closeWav();
    void write(uint32_t offset, uint16_t value);
    void startAdma(int core, const uint8_t* data, uint32_t bytes);
    bool admaEnabled(int core) const;
    void cancelAdma(int core);
    uint64_t dmaDelay(int core, uint64_t fallback) const;
    size_t drainSamples(int16_t* stereo, size_t frames) noexcept;
    uint64_t droppedFrames() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    static void decode(const uint8_t* block, int32_t& h1, int32_t& h2, int16_t* out);
private:
    struct Voice {
        std::array<int16_t,28> samples{};
        uint32_t address=0, loop=0, fraction=0, index=28, counter=0;
        int32_t h1=0,h2=0,env=0;
        uint8_t phase=0,flags=0;
        bool manualLoop=false;
    };
    struct Pcm { std::vector<uint8_t> data; size_t frame=0; };
    uint16_t r(uint32_t o) const { return m_regs[o/2]; }
    uint32_t address(uint32_t o) const;
    void putAddress(uint32_t o,uint32_t bytes);
    void keyOn(int core,int voice);
    void loadBlock(int core,int voice);
    void envelope(int core,int voice);
    void tick(int16_t* out);
    void emit(const int16_t* sample);
    static int32_t volume(uint16_t value);
    std::vector<uint16_t>& m_regs;
    std::vector<uint8_t>& m_ram;
    std::array<std::array<Voice,24>,2> m_voices{};
    std::array<Pcm,2> m_pcm{};
    uint64_t m_sampleCycle=0;
    static constexpr uint64_t RingFrames=32768;
    std::array<int16_t,RingFrames*2> m_ring{};
    std::atomic<uint64_t> m_read{0},m_write{0},m_dropped{0};
    std::FILE* m_wav=nullptr;
    uint64_t m_wavFrames=0;
    bool m_wavChecked=false;
};
}
