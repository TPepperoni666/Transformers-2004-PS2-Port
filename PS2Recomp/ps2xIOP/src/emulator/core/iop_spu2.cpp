#include "iop_spu2.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr char kTraceMagic[8] = {'S', 'P', 'U', '2', 'T', 'R', 'C', '1'};
        constexpr uint32_t kTraceVersion = 1u;

        // 16-byte fixed record, little endian:
        //   u64 cycle; u8 type; u8 core; u16 offset (byte offset inside the 0x800 window);
        //   u32 value (write16/read16: the halfword; DMA/PIO: payload byte count)
        // DMA / PIO payload records are followed by a 16-byte extension
        //   { u32 spuByteAddr; u32 iopAddr; u32 reserved0; u32 reserved1 }
        // and then `value` payload bytes padded with zeroes to a multiple of 16.
        struct TraceRecord
        {
            uint64_t cycle;
            uint8_t type;
            uint8_t core;
            uint16_t offset;
            uint32_t value;
        };
        static_assert(sizeof(TraceRecord) == 16, "trace record must be 16 bytes");

        struct TraceExtension
        {
            uint32_t spuByteAddr;
            uint32_t iopAddr;
            uint32_t reserved0;
            uint32_t reserved1;
        };
        static_assert(sizeof(TraceExtension) == 16, "trace extension must be 16 bytes");
    }

    Spu2Device::Spu2Device()
        : m_regs(WindowBytes / 2u, uint16_t{0}), m_mixer(m_regs, m_ram)
    {
    }

    Spu2Device::~Spu2Device()
    {
        closeTrace();
    }

    void Spu2Device::reset()
    {
        std::fill(m_regs.begin(), m_regs.end(), uint16_t{0});
        if (!m_ram.empty())
            std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        m_mixer.reset(m_clock ? *m_clock : 0);
        m_writeCount = 0;
        m_dmaBytes = 0;
        // A reset (new IOP boot) starts a new trace segment; keep the file open so a single run is one file.
        if (m_trace && traceRoom(16))
        {
            TraceRecord marker{m_clock ? *m_clock : 0u, 0u, 0u, 0u, 0u}; // type 0 = reset marker
            std::fwrite(&marker, sizeof(marker), 1, m_trace.get());
        }
    }

    void Spu2Device::ensureRam()
    {
        if (m_ram.empty())
            m_ram.assign(RamBytes, uint8_t{0});
    }

    bool Spu2Device::openTrace(const std::string &path)
    {
        closeTrace();
        m_envChecked = true;
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
            return false;
        m_trace.reset(file);
        m_traceBytes = 16;
        m_traceCapped = false;
        // TEMP(transformers): prevent unattended diagnostic traces filling the disk.
        // Default 512 MiB. PS2X_SPU2_TRACE_MAX_MB may raise/lower it; zero explicitly means unlimited.
        if (const char* cap = std::getenv("PS2X_SPU2_TRACE_MAX_MB")) {
            char* end = nullptr;
            const uint64_t mb = std::strtoull(cap, &end, 10);
            if (end != cap && *end == 0 && mb <= (UINT64_MAX / (1024 * 1024)))
                m_traceLimit = mb * 1024 * 1024;
        }
        std::fwrite(kTraceMagic, sizeof(kTraceMagic), 1, file);
        const uint32_t header[2] = {kTraceVersion, 0u};
        std::fwrite(header, sizeof(header), 1, file);
        return true;
    }

    void Spu2Device::closeTrace()
    {
        if (m_trace)
        {
            std::fflush(m_trace.get());
            m_trace.reset();
        }
    }

    void Spu2Device::maybeOpenTraceFromEnv()
    {
        if (m_envChecked)
            return;
        m_envChecked = true;
        const char *path = std::getenv("PS2X_SPU2_TRACE");
        if (path && *path)
            (void)openTrace(path);
        const char *reads = std::getenv("PS2X_SPU2_TRACE_READS");
        m_traceReads = reads && *reads && *reads != '0';
    }

    bool Spu2Device::traceRoom(uint64_t bytes)
    {
        if (m_traceCapped) return false;
        if (m_traceLimit && (m_traceBytes > m_traceLimit || bytes > m_traceLimit - m_traceBytes)) {
            m_traceCapped = true;
            std::fflush(m_trace.get());
            std::fprintf(stderr, "[SPU2] trace capped at %llu bytes (complete records only)\n",
                         static_cast<unsigned long long>(m_traceBytes));
            return false;
        }
        m_traceBytes += bytes;
        return true;
    }

    void Spu2Device::record(TraceType type, uint32_t offset, uint32_t value)
    {
        maybeOpenTraceFromEnv();
        if (!m_trace || !traceRoom(16))
            return;
        TraceRecord rec{m_clock ? *m_clock : 0u,
                        static_cast<uint8_t>(type),
                        static_cast<uint8_t>((offset >= CoreStride) ? 1u : 0u),
                        static_cast<uint16_t>(offset),
                        value};
        std::fwrite(&rec, sizeof(rec), 1, m_trace.get());
        if ((++m_traceRecords & 0xFFFu) == 0u)
            std::fflush(m_trace.get());
    }

    void Spu2Device::recordPayload(TraceType type, int core, uint32_t spuByteAddr, uint32_t iopAddr,
                                   const uint8_t *data, uint32_t bytes)
    {
        maybeOpenTraceFromEnv();
        if (!m_trace || !traceRoom(32ull + ((static_cast<uint64_t>(bytes) + 15) & ~15ull)))
            return;
        TraceRecord rec{m_clock ? *m_clock : 0u,
                        static_cast<uint8_t>(type),
                        static_cast<uint8_t>(core),
                        0u,
                        bytes};
        TraceExtension ext{spuByteAddr, iopAddr, 0u, 0u};
        std::FILE *f = m_trace.get();
        std::fwrite(&rec, sizeof(rec), 1, f);
        std::fwrite(&ext, sizeof(ext), 1, f);
        if (bytes != 0u && data)
            std::fwrite(data, 1, bytes, f);
        const uint32_t pad = (16u - (bytes & 15u)) & 15u;
        if (pad)
        {
            static const uint8_t zeros[16] = {};
            std::fwrite(zeros, 1, pad, f);
        }
        std::fflush(f); // payload records are rare and large; keep the file consistent if the run is killed
    }

    uint32_t Spu2Device::tsaBytes(int core) const noexcept
    {
        const uint32_t base = static_cast<uint32_t>(core) * CoreStride;
        const uint32_t hi = m_regs[(base + RegTsaHi) / 2u] & 0xFu;
        const uint32_t lo = m_regs[(base + RegTsaLo) / 2u];
        return ((hi << 16u) | lo) * 2u;
    }

    void Spu2Device::setTsaBytes(int core, uint32_t bytes) noexcept
    {
        const uint32_t base = static_cast<uint32_t>(core) * CoreStride;
        const uint32_t halfwords = (bytes / 2u) & 0xFFFFFu;
        m_regs[(base + RegTsaHi) / 2u] = static_cast<uint16_t>(halfwords >> 16u);
        m_regs[(base + RegTsaLo) / 2u] = static_cast<uint16_t>(halfwords & 0xFFFFu);
    }

    uint16_t Spu2Device::read16(uint32_t offset)
    {
        advanceTo(m_clock ? *m_clock : 0);
        offset &= (WindowBytes - 1u) & ~1u;
        const uint16_t value = m_regs[offset / 2u];
        if (m_traceReads || !m_envChecked)
        {
            maybeOpenTraceFromEnv();
            if (m_traceReads)
                record(TraceRead16, offset, value);
        }
        return value;
    }

    void Spu2Device::write16(uint32_t offset, uint16_t value)
    {
        advanceTo(m_clock ? *m_clock : 0);
        offset &= (WindowBytes - 1u) & ~1u;
        // ENDX writes clear the addressed half independently of the value.
        m_regs[offset / 2u] = ((offset % CoreStride == 0x340u) || (offset % CoreStride == 0x342u)) ? 0 : value;
        m_mixer.write(offset, value);
        ++m_writeCount;
        record(TraceWrite16, offset, value);

        // PIO data port: the halfword goes to sound RAM at TSA, TSA advances (hardware FIFO behaviour).
        if ((offset % CoreStride) == RegFifo)
        {
            const int core = static_cast<int>(offset / CoreStride);
            ensureRam();
            const uint32_t at = tsaBytes(core);
            if (at + 2u <= RamBytes)
                std::memcpy(m_ram.data() + at, &value, sizeof(value));
            setTsaBytes(core, (at + 2u) % RamBytes);
        }
    }

    uint8_t Spu2Device::read8(uint32_t offset)
    {
        const uint16_t half = read16(offset & ~1u);
        return static_cast<uint8_t>(half >> ((offset & 1u) * 8u));
    }

    void Spu2Device::write8(uint32_t offset, uint8_t value)
    {
        const uint32_t aligned = offset & ~1u;
        uint16_t half = m_regs[(aligned & (WindowBytes - 1u)) / 2u];
        const uint32_t shift = (offset & 1u) * 8u;
        half = static_cast<uint16_t>((half & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift));
        write16(aligned, half);
    }

    uint32_t Spu2Device::read32(uint32_t offset)
    {
        return static_cast<uint32_t>(read16(offset)) | (static_cast<uint32_t>(read16(offset + 2u)) << 16u);
    }

    void Spu2Device::write32(uint32_t offset, uint32_t value)
    {
        write16(offset, static_cast<uint16_t>(value));
        write16(offset + 2u, static_cast<uint16_t>(value >> 16u));
    }

    uint32_t Spu2Device::onDma(int core, bool toSpu, uint32_t madr, uint32_t bytes,
                               const uint8_t *iopRam, size_t iopRamSize,
                               std::vector<uint8_t> *readBack)
    {
        advanceTo(m_clock ? *m_clock : 0);
        core &= 1;
        const uint32_t base = static_cast<uint32_t>(core) * CoreStride;
        const uint32_t iopAddr = madr & 0x1FFFFFu;
        if (iopAddr > iopRamSize || bytes > iopRamSize - iopAddr)
            bytes = iopAddr > iopRamSize ? 0u : static_cast<uint32_t>(iopRamSize - iopAddr);

        ensureRam();
        const uint32_t spuAddr = tsaBytes(core);
        uint32_t moved = 0u;

        if (toSpu && admaEnabled(core))
        {
            const uint8_t* src = iopRam + iopAddr;
            recordPayload(TraceDmaToSpu, core, spuAddr, iopAddr, src, bytes);
            m_mixer.startAdma(core, src, bytes);
            m_dmaBytes += bytes;
            return bytes;
        }
        if (toSpu)
        {
            // Wraps at the end of sound RAM like the hardware address counter.
            const uint8_t *src = iopRam + iopAddr;
            uint32_t at = spuAddr;
            uint32_t left = bytes;
            while (left != 0u)
            {
                const uint32_t chunk = std::min(left, RamBytes - at);
                std::memcpy(m_ram.data() + at, src + moved, chunk);
                moved += chunk;
                left -= chunk;
                at = (at + chunk) % RamBytes;
            }
            recordPayload(TraceDmaToSpu, core, spuAddr, iopAddr, src, bytes);
            setTsaBytes(core, at);
        }
        else
        {
            if (readBack)
            {
                readBack->assign(bytes, uint8_t{0});
                uint32_t at = spuAddr;
                uint32_t left = bytes;
                while (left != 0u)
                {
                    const uint32_t chunk = std::min(left, RamBytes - at);
                    std::memcpy(readBack->data() + moved, m_ram.data() + at, chunk);
                    moved += chunk;
                    left -= chunk;
                    at = (at + chunk) % RamBytes;
                }
                setTsaBytes(core, at);
            }
            recordPayload(TraceDmaFromSpu, core, spuAddr, iopAddr, nullptr, 0u);
        }
        m_dmaBytes += moved;

        // Same side effect as the previous generic implementation: "transfer ready" (bit 7) in STATX.
        m_regs[(base + RegStatx) / 2u] |= 0x80u;
        return moved;
    }
}
