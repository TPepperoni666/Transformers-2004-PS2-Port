// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
#pragma once

// SPU2 register file, sound RAM, cycle-clocked dry voice/ADMA mixer, bounded trace.
// No host output: drainSamples() supplies stereo 48 kHz frames to one consumer.
//
// Register offsets are relative to 0x1F900000; core 1 mirrors core 0 at +0x400.

#include "iop_spu2_mixer.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    class Spu2Device
    {
    public:
        static constexpr uint32_t Base = 0x1F900000u;
        static constexpr uint32_t WindowBytes = 0x800u; // 2 cores x 0x400
        static constexpr uint32_t RamBytes = 2u * 1024u * 1024u;
        static constexpr uint32_t CoreStride = 0x400u;

        // Core-relative register offsets (see plan.md section 3).
        static constexpr uint32_t RegAttr = 0x19Au;
        static constexpr uint32_t RegTsaHi = 0x1A8u;
        static constexpr uint32_t RegTsaLo = 0x1AAu;
        static constexpr uint32_t RegFifo = 0x1ACu;
        static constexpr uint32_t RegStatx = 0x344u;

        // Trace record types.
        enum TraceType : uint8_t
        {
            TraceWrite16 = 1,
            TraceRead16 = 2,
            TraceDmaToSpu = 3,
            TraceDmaFromSpu = 4,
            TracePioWrite = 5,
        };

        Spu2Device();
        ~Spu2Device();
        Spu2Device(const Spu2Device &) = delete;
        Spu2Device &operator=(const Spu2Device &) = delete;

        void reset();
        void advanceTo(uint64_t cycle) { m_mixer.advanceTo(cycle); }
        [[nodiscard]] uint64_t nextSampleCycle() const noexcept { return m_mixer.nextSampleCycle(); }
        // TEMP(transformers): call only on producer thread, or after it has joined.
        void finishAudioCapture() { m_mixer.closeWav(); closeTrace(); m_envChecked=true; }
        size_t drainSamples(int16_t* stereo, size_t frames) noexcept { return m_mixer.drainSamples(stereo, frames); }
        bool admaEnabled(int core) const { return m_mixer.admaEnabled(core); }
        void cancelAdma(int core) { m_mixer.cancelAdma(core); }
        uint64_t dmaCompletionDelay(int core, uint64_t fallback) const {
            const uint64_t delay = m_mixer.dmaDelay(core, fallback);
            return admaEnabled(core) && delay != fallback ? delay - (m_clock ? *m_clock % 768 : 0) : fallback;
        }
        void completeDma(int core) {
            m_regs[(core * CoreStride + RegStatx) / 2] =
                static_cast<uint16_t>((m_regs[(core * CoreStride + RegStatx) / 2] & ~0x400u) | 0x80u);
        }

        // IOP cycle counter used to time-stamp trace records (optional; 0 when unset).
        void setClock(const uint64_t *cycles) noexcept { m_clock = cycles; }

        [[nodiscard]] static bool contains(uint32_t phys) noexcept
        {
            return phys >= Base && phys < Base + WindowBytes;
        }

        // `offset` is relative to Base; must be even for the 16-bit forms.
        [[nodiscard]] uint16_t read16(uint32_t offset);
        void write16(uint32_t offset, uint16_t value);
        [[nodiscard]] uint8_t read8(uint32_t offset);
        void write8(uint32_t offset, uint8_t value);
        [[nodiscard]] uint32_t read32(uint32_t offset);
        void write32(uint32_t offset, uint32_t value);

        // SPU DMA start (CHCR start bit seen).  `core` 0 = DMA4, 1 = DMA7.  `toSpu` = CHCR bit 0 (RAM -> SPU).
        // `iopRam`/`iopRamSize` expose IOP RAM for the payload copy; for SPU->RAM the callback below
        // receives the bytes read from sound RAM.  Returns the number of bytes moved.
        uint32_t onDma(int core, bool toSpu, uint32_t madr, uint32_t bytes,
                       const uint8_t *iopRam, size_t iopRamSize,
                       std::vector<uint8_t> *readBack);

        [[nodiscard]] const std::vector<uint8_t> &soundRam() const noexcept { return m_ram; }
        [[nodiscard]] uint64_t registerWrites() const noexcept { return m_writeCount; }
        [[nodiscard]] uint64_t dmaBytes() const noexcept { return m_dmaBytes; }

        // Explicit trace control (tests); the env var is honoured lazily on the first record otherwise.
        bool openTrace(const std::string &path);
        void closeTrace();
        [[nodiscard]] bool tracing() const noexcept { return m_trace != nullptr; }

    private:
        struct Closer
        {
            void operator()(std::FILE *f) const noexcept
            {
                if (f)
                    std::fclose(f);
            }
        };

        [[nodiscard]] uint32_t tsaBytes(int core) const noexcept;
        void setTsaBytes(int core, uint32_t bytes) noexcept;
        void ensureRam();
        void maybeOpenTraceFromEnv();
        void record(TraceType type, uint32_t offset, uint32_t value);
        void recordPayload(TraceType type, int core, uint32_t spuByteAddr, uint32_t iopAddr,
                           const uint8_t *data, uint32_t bytes);

        std::vector<uint16_t> m_regs;
        std::vector<uint8_t> m_ram;
        Spu2Mixer m_mixer;
        const uint64_t *m_clock = nullptr;
        std::unique_ptr<std::FILE, Closer> m_trace;
        bool m_envChecked = false;
        bool m_traceReads = false;
        uint64_t m_writeCount = 0;
        uint64_t m_dmaBytes = 0;
        uint64_t m_traceRecords = 0;
        uint64_t m_traceBytes = 0;
        uint64_t m_traceLimit = 512ull * 1024 * 1024;
        bool m_traceCapped = false;
        bool traceRoom(uint64_t bytes);
    };
}
