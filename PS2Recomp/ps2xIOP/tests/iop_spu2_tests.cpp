// SPU2 skeleton tests (audio runtime): 16-bit register semantics, DMA payload -> sound RAM, trace file.
#include "emulator/core/iop_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using ps2x::iop::detail::IopMemory;
using ps2x::iop::detail::Spu2Device;

namespace
{
    bool expect(bool condition, const char *message)
    {
        if (!condition)
            std::cerr << "FAILED: " << message << '\n';
        return condition;
    }

    constexpr uint32_t kSpu = Spu2Device::Base;
    constexpr uint32_t kDma4Madr = 0x1F8010C0u;
    constexpr uint32_t kDma4Bcr = 0x1F8010C4u;
    constexpr uint32_t kDma4Chcr = 0x1F8010C8u;
    constexpr uint32_t kDma7Madr = 0x1F801500u;
    constexpr uint32_t kDma7Bcr = 0x1F801504u;
    constexpr uint32_t kDma7Chcr = 0x1F801508u;

    bool testRegisterSemantics()
    {
        IopMemory mem;
        // A halfword store must update exactly that register and be a single device write.
        mem.write16(kSpu + 0x1A0u, 0x00FFu); // core 0 KON low
        mem.write16(kSpu + 0x1A2u, 0x0001u); // core 0 KON high
        mem.write16(kSpu + 0x5A0u, 0x1234u); // core 1 KON low
        const uint64_t writes = mem.spu2().registerWrites();
        bool ok = expect(writes == 3u, "each sh must be exactly one SPU2 register write");
        ok &= expect(mem.read16(kSpu + 0x1A0u) == 0x00FFu, "core0 KON low readback");
        ok &= expect(mem.read16(kSpu + 0x1A2u) == 0x0001u, "core0 KON high readback");
        ok &= expect(mem.read16(kSpu + 0x5A0u) == 0x1234u, "core1 KON low readback");
        ok &= expect(mem.read32(kSpu + 0x1A0u) == 0x000100FFu, "32-bit read = two halfwords");
        mem.write32(kSpu + 0x760u, 0x3FFF2000u); // MVOLL/MVOLR
        ok &= expect(mem.read16(kSpu + 0x760u) == 0x2000u && mem.read16(kSpu + 0x762u) == 0x3FFFu, "32-bit write split");
        mem.write8(kSpu + 0x19Bu, 0xC0u);
        ok &= expect((mem.read16(kSpu + 0x19Au) & 0xFF00u) == 0xC000u, "byte write merges into halfword");
        // The rest of the 0x1F9xxxxx window keeps the old generic behaviour.
        mem.write32(0x1F900800u, 0xCAFEBABEu);
        ok &= expect(mem.read32(0x1F900800u) == 0xCAFEBABEu, "outside SPU2 window: generic map");
        return ok;
    }

    bool testDmaToSoundRam(int core)
    {
        IopMemory mem;
        std::vector<uint8_t> payload(0x80);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<uint8_t>(i * 3u + 1u);
        const uint32_t src = 0x00040000u;
        (void)mem.writeRam(src, payload.data(), payload.size());

        const uint32_t base = static_cast<uint32_t>(core) * Spu2Device::CoreStride;
        const uint32_t tsaByte = 0x5000u;
        mem.write16(kSpu + base + 0x1A8u, static_cast<uint16_t>((tsaByte / 2u) >> 16u));
        mem.write16(kSpu + base + 0x1AAu, static_cast<uint16_t>((tsaByte / 2u) & 0xFFFFu));
        const uint32_t madr = core ? kDma7Madr : kDma4Madr;
        const uint32_t bcr = core ? kDma7Bcr : kDma4Bcr;
        const uint32_t chcr = core ? kDma7Chcr : kDma4Chcr;
        mem.write32(madr, src);
        mem.write32(bcr, (2u << 16) | 0x10u); // 2 blocks x 16 words = 128 bytes
        mem.write32(chcr, 0x01000201u);

        bool ok = expect(mem.spu2().dmaBytes() == 0x80u, "DMA moved 128 bytes");
        const auto &ram = mem.spu2().soundRam();
        ok &= expect(ram.size() == Spu2Device::RamBytes, "sound RAM allocated");
        ok &= expect(ram.size() > tsaByte + payload.size() &&
                         std::memcmp(ram.data() + tsaByte, payload.data(), payload.size()) == 0,
                     "payload landed at TSA*2 in sound RAM");
        const uint32_t newTsa = (static_cast<uint32_t>(mem.read16(kSpu + base + 0x1A8u)) << 16 |
                                 mem.read16(kSpu + base + 0x1AAu)) * 2u;
        ok &= expect(newTsa == tsaByte + 0x80u, "TSA advanced by the transfer size");
        ok &= expect((mem.read16(kSpu + base + 0x344u) & 0x80u) != 0u, "STATX bit 7 set after DMA (old behaviour)");
        ok &= expect(mem.takeDmaStart().has_value(), "completion interrupt still scheduled");

        // SPU -> RAM read back.
        const uint32_t dst = 0x00050000u;
        mem.write16(kSpu + base + 0x1A8u, static_cast<uint16_t>((tsaByte / 2u) >> 16u));
        mem.write16(kSpu + base + 0x1AAu, static_cast<uint16_t>((tsaByte / 2u) & 0xFFFFu));
        mem.write32(madr, dst);
        mem.write32(bcr, (2u << 16) | 0x10u);
        mem.write32(chcr, 0x01000200u);
        std::vector<uint8_t> back(payload.size());
        (void)mem.readRam(dst, back.data(), back.size());
        ok &= expect(back == payload, "SPU->RAM DMA returned the uploaded bytes");
        return ok;
    }

    bool testTrace()
    {
        const std::string path = "spu2_test_trace.bin";
        {
            IopMemory mem;
            uint64_t clock = 1234u;
            mem.spu2().setClock(&clock);
            if (!expect(mem.spu2().openTrace(path), "open trace"))
                return false;
            mem.write16(kSpu + 0x760u, 0x3FFFu);
            clock = 2000u;
            std::vector<uint8_t> payload(32, 0xA5u);
            (void)mem.writeRam(0x1000u, payload.data(), payload.size());
            mem.write32(kDma4Madr, 0x1000u);
            mem.write32(kDma4Bcr, (1u << 16) | 8u);
            mem.write32(kDma4Chcr, 0x01000201u);
            mem.spu2().closeTrace();
        }
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!expect(f != nullptr, "trace file exists"))
            return false;
        std::vector<uint8_t> data(1 << 16);
        const size_t n = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        // header 16 + write rec 16 + dma rec 16 + ext 16 + 32 payload
        bool ok = expect(n == 16u + 16u + 16u + 16u + 32u, "trace size");
        ok &= expect(std::memcmp(data.data(), "SPU2TRC1", 8) == 0, "trace magic");
        uint64_t cycle = 0;
        std::memcpy(&cycle, data.data() + 16, 8);
        ok &= expect(cycle == 1234u && data[24] == Spu2Device::TraceWrite16, "write record stamp/type");
        std::memcpy(&cycle, data.data() + 32, 8);
        ok &= expect(cycle == 2000u && data[40] == Spu2Device::TraceDmaToSpu, "dma record stamp/type");
        if (!std::getenv("PS2X_SPU2_TEST_KEEP_TRACE")) // keep for the python tools' format cross-check
            std::remove(path.c_str());
        return ok;
    }
}

int main()
{
    if (!testRegisterSemantics() || !testDmaToSoundRam(0) || !testDmaToSoundRam(1) || !testTrace())
        return 1;
    std::cout << "ps2xIOP SPU2 skeleton tests passed\n";
    return 0;
}
