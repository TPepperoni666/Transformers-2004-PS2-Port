extern "C" const bool iopIdleStatsEnabled; // TEMP(transformers): cached process flag.
// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
extern "C" void iopIdleStatsTimerWalk() noexcept;
#include "ps2_drain.h"
#include "runtime/ps2_eeopt.h"
// checkpoint fast path adapted from brad-richardson/PS2Recomp (GPL-3.0); budget design after snuri00/ps2-web-recomp (MIT)
#include "ps2_vu1new.h" // TEMP(transformers)
#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_raw_record.h"
#include "ps2_log.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream> // TEMP(transformers): bounded DMA provenance capture
#include "debug_counters.h" // TEMP(transformers)

// release fix (qualification): CHCR.TTE puts each DMAtag's upper 8 bytes in front of its payload, so offsets in the
// concatenated VIF1 chain buffer are not the hardware quadword phase of the data. The hardware UNPACK V2/V3 w-lane
// rules depend on that phase, so the chain walker records where each tag's 8 bytes start (helpers in
// ps2_vif1_interpreter.cpp, keyed by the chain buffer's data pointer, which survives the move into PendingTransfer).
namespace ps2x_vifunpack
{
    void registerChainTte(const uint8_t *key, std::vector<uint32_t> &&offsets);
    void beginChain(const uint8_t *key);
    void endChain();
    std::vector<uint32_t> takeChainTte(const uint8_t *key); // TEMP(transformers) qualification
}

// PATH3 mask-window state (see PS2Memory::flushMaskedPath3Packets). Set by the VIF1 MSKPATH3 unmask; file-scope so
// the PCH header ps2_memory.h does not change. (GENERIC, ported from SSX3 SSX-0042.)
bool g_path3ReleaseOnePacket = false;
size_t g_path3FrontOffset = 0u;

void ps2ResetVif1DirectContinuation(); // ps2_vif1_interpreter.cpp
void ps2Vif1ChainResumeTick(PS2Memory &mem); // below; also called from Kernel/EeScheduler.cpp

namespace
{
    // VIF1 source-chain suspension. Some games kick the VIF1 chain before they have finished writing it and keep
    // appending tags while the DMAC (throttled by VIF/VU1/GS stalls on hardware) is still busy with the first part
    // (Transformers' Options/globe screens: tag 178 is still the previous frame's GIF data at kick time). Walking the
    // whole chain at the CHCR write then runs into stale memory, never reaches the final END/i-bit and the game's frame
    // wait falls back to its 3 s watchdog. When a tag looks unwritten (reserved bits set, address outside RAM/SPR) the
    // walk stops there with STR still set; ps2Vif1ChainResumeTick() (EE scheduler events) and DMA register reads retry
    // from that tag once it looks valid. PS2X_VIF1_CHAIN_SUSPEND=0 disables this; after kVif1SuspendTimeoutVsyncs the
    // walk is forced through as before.
    struct Vif1ChainSuspend
    {
        bool active = false;
        bool resuming = false;
        bool force = false;
        uint32_t chcr = 0u;
        uint32_t tagAddr = 0u;
        uint64_t sinceVsync = 0u;
        uint32_t retries = 0u;
    };
    Vif1ChainSuspend g_vif1Suspend;
    constexpr uint64_t kVif1SuspendTimeoutVsyncs = 120u;

    bool vif1ChainSuspendEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *e = std::getenv("PS2X_VIF1_CHAIN_SUSPEND");
            return !(e && e[0] == '0');
        }();
        return s_enabled;
    }

    // A source-chain DMAtag that cannot be what the game meant: a data/next address outside main RAM (and its uncached
    // mirrors) or the scratchpad. (Reserved bits 16..25 are NOT checked: the DMAC ignores them and Transformers' normal
    // frame chains carry data there.)
    bool dmaTagLooksUnwritten(uint64_t tag)
    {
        const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
        if (id == 1u || id == 6u || id == 7u) // cnt/ret/end: address unused
            return false;
        const uint32_t addr = static_cast<uint32_t>(tag >> 32);
        if (addr & 0x80000000u) // SPR
            return (addr & 0x7FFFFFFFu) >= 0x4000u;
        return (addr & 0x0FFFFFFFu) >= PS2_RAM_SIZE;
    }

    // TEMP(transformers): PS2X_VIF1_KICKLOG=<guest vsync> logs every VIF1 DMA kick (tags walked, why the walk stopped).
    bool vif1KickLogOnMem()
    {
        static const long long s_from = []()
        {
            const char *e = std::getenv("PS2X_VIF1_KICKLOG");
            return e ? std::atoll(e) : -1ll;
        }();
        return s_from >= 0 && static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)) >= s_from;
    }

    // GAME(transformers) NIGHT_B: EE stores to GIF_FIFO (0x10006000) -> PATH3. See PS2Memory::write128.
    bool gifFifoEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *e = std::getenv("PS2X_GIF_FIFO");
            return !(e && e[0] == '0');
        }();
        return s_enabled;
    }

    struct GifFifoState
    {
        std::vector<uint8_t> data;  // collected qwords (whole GIFtag packets)
        uint64_t tagNeedQw = 0u;     // qwords still owed to the current GIFtag (0 = next qword is a GIFtag)
        bool sawEop = false;         // the current GIFtag has EOP
        uint32_t packets = 0u;       // TEMP(transformers): log the first few
    };
    GifFifoState g_gifFifo; // EE thread only (guest stores)

    void gifFifoPush(PS2Memory &mem, const uint8_t *qw)
    {
        GifFifoState &f = g_gifFifo;
        f.data.insert(f.data.end(), qw, qw + 16);
        if (f.tagNeedQw == 0u)
        {
            uint64_t tag = 0u;
            std::memcpy(&tag, qw, sizeof(tag));
            const uint64_t nloop = tag & 0x7FFFull;
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 0x3u);
            uint64_t nreg = (tag >> 60) & 0xFull;
            if (nreg == 0u)
                nreg = 16u;
            f.tagNeedQw = flg == 0u ? nloop * nreg : (flg == 1u ? (nloop * nreg + 1u) / 2u : nloop);
            f.sawEop = ((tag >> 15) & 1ull) != 0ull;
        }
        else
        {
            --f.tagNeedQw;
        }
        if (f.tagNeedQw != 0u || (!f.sawEop && f.data.size() < (1u << 20)))
            return;
        if (f.packets < 8u) // TEMP(transformers)
        {
            ++f.packets;
            std::cerr << "[gif:fifo] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed)
                      << " packet qw=" << (f.data.size() / 16u) << " -> PATH3" << std::endl;
        }
        mem.submitGifPacket(GifPathId::Path3, f.data.data(), static_cast<uint32_t>(f.data.size()));
        f.data.clear();
        f.sawEop = false;
    }

    // TEMP(transformers) runtime fast path: positive, bounded RAM/SPR classification.
    // Accept physical low RAM and documented mirrors; reject TLB, VU, GS, MMIO and region edges.
    struct EeoptMemoryRange { uint32_t offset; bool scratch; };
    inline bool eeoptMemoryRange(uint32_t address, uint32_t bytes, EeoptMemoryRange &range) {
        const uint32_t spr = (address & 0x7fffffffu) - 0x70000000u;
        if (spr < PS2_SCRATCHPAD_SIZE && bytes <= PS2_SCRATCHPAD_SIZE - spr) {
            range = {spr, true}; return true;
        }
        uint32_t physical;
        if (address < PS2_RAM_SIZE) physical = address;
        else if (address - 0x20000000u < 0x20000000u) physical = address & PS2_RAM_MASK;
        else if (address - 0x80000000u < 0x40000000u) physical = address & 0x1fffffffu;
        else return false;
        if (physical >= PS2_RAM_SIZE || bytes > PS2_RAM_SIZE - physical) return false;
        range = {physical, false}; return true;
    }

    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    // TEMP(transformers): privileged GS registers (CSR/IMR/DISPFB/PMODE/...) live in
    // gs_regs and are updated on the EE thread; none of them depends on the GS
    // worker's VRAM progress (FINISH/SIGNAL/LABEL already fence in GS::writeRegister,
    // presentation fences inside latchHostPresentationFrame). Draining the worker on
    // every access serialised EE and GS once per vsync (game vsync handler 0x25df60
    // reads CSR and writes PMODE/DISPFB1/DISPLAY1). PS2X_GS_PRIV_SYNC=1 restores it.
    inline void gsPrivRegFence(const uint8_t *vram)
    {
        static const bool s_enabled = [] {
            const char *v = std::getenv("PS2X_GS_PRIV_SYNC");
            return v && v[0] == '1';
        }();
        if (s_enabled)
            GSThreadedBackend::SyncVram(vram);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;
    // GENERIC (ported from SSX3 SSX-0023, crossover integration update): CSR bits 14-15 are the GS FIFO status
    // (01 = empty). Our GS consumes GIF data synchronously or through its own queue, so the FIFO always reads empty
    // on EE reads (PCSX2 resets CSR.FIFO to EMPTY too). Games that poll it (SSX3 WaitGS 0x375800) hung forever.
    constexpr uint64_t kGsCsrFifoEmpty = 0x4000ull;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (m_vif1Instant) m_vif1Instant->abandon();
    if (m_vif1Stream) m_vif1Stream->abandon();
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        GSThreadedBackend::SyncVram(m_gsVRAM);
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    fenceVu1();
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        GSThreadedBackend::SyncVram(m_gsVRAM);
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
        m_completedDmacPending.store(false, std::memory_order_release);
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    g_path3FrontOffset = 0u;
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    ps2ResetVif1DirectContinuation();
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        m_gifStatSlot = nullptr;

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));
        m_vif1Scan={};m_vif1Actual={};m_vif1LastStatistic.store(0);

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    // Ported from KIexster/god-of-war-recomp 061be7f (Romeo), GPL-3.0: reuse the GIF_STAT value address.
    if (!m_gifStatSlot)
    {
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        if (gifStatIt != m_ioRegisters.end())
            m_gifStatSlot = &gifStatIt->second;
    }
    if (m_gifStatSlot)
        *m_gifStatSlot &= ~kGifFqcMask;

    if (iopIdleStatsEnabled) iopIdleStatsTimerWalk(); // TEMP(transformers)
    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    if (physAddr >= PS2_VU1_CODE_BASE && physAddr < PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE) {Vu1FenceReason r_(kVu1FenceMem);fenceVu1();} // qualification: reason
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (ps2IsScratchpadAddress(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && true && eeoptMemoryRange(address, 1u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        uint8_t value; std::memcpy(&value, base + ordinary.offset, 1); return value;
    }
    uint8_t predicted{};if((address + 1u > 0x12000000u) && (address < 0x12001000u || (address&~7u)==0x12001010u) && readInstantGsSetting(address,&predicted,sizeof(predicted)))return predicted;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // VU1 fence reason
    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 1u) == 0u && eeoptMemoryRange(address, 2u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        uint16_t value; std::memcpy(&value, base + ordinary.offset, 2); return value;
    }
    uint16_t predicted{};if((address + 2u > 0x12000000u) && (address < 0x12001000u || (address&~7u)==0x12001010u) && readInstantGsSetting(address,&predicted,sizeof(predicted)))return predicted;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // VU1 fence reason
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 3u) == 0u && eeoptMemoryRange(address, 4u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        uint32_t value; std::memcpy(&value, base + ordinary.offset, 4); return value;
    }
    uint32_t predicted{};if((address + 4u > 0x12000000u) && (address < 0x12001000u || (address&~7u)==0x12001010u) && readInstantGsSetting(address,&predicted,sizeof(predicted)))return predicted;

    // GAME(transformers): dead statistic -> dead store 0x6CF068; optional relaxed observation.
    if(m_vif1Instant && m_vif1StatisticShortcut && ((address & 0x1FFFFFFFu) == 0x1100C0B0u || address >= 0xC0000000u) && translateAddress(address)==0x1100C0B0u)
        return m_vif1LastStatistic.load(std::memory_order_acquire);
    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        gsPrivRegFence(m_gsVRAM);
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load() | kGsCsrFifoEmpty;
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 7u) == 0u && eeoptMemoryRange(address, 8u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        uint64_t value; std::memcpy(&value, base + ordinary.offset, 8); return value;
    }
    uint64_t predicted{};if((address + 8u > 0x12000000u) && (address < 0x12001000u || (address&~7u)==0x12001010u) && readInstantGsSetting(address,&predicted,sizeof(predicted)))return predicted;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // VU1 fence reason
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        gsPrivRegFence(m_gsVRAM);
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load() | kGsCsrFifoEmpty;
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && true && eeoptMemoryRange(address & ~15u, 16u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        __m128i value; std::memcpy(&value, base + ordinary.offset, 16); return value;
    }
    __m128i predicted{};if((address + 16u > 0x12000000u) && (address < 0x12001000u || (address&~7u)==0x12001010u) && readInstantGsSetting(address,&predicted,sizeof(predicted)))return predicted;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // VU1 fence reason
    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-ee-fixes.patch (GPL-3.0), extended to PS2Memory:
    // LQ/LQC2 ignore address bits 3:0 on the R5900 (no address error), so read the containing quadword instead of
    // throwing (the throw became SignalException(ADDRESS_ERROR) + a zero result in PS2Runtime::Load128).
    address &= ~15u;

    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && true && eeoptMemoryRange(address, 1u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        std::memcpy(base + ordinary.offset, &value, 1);
        return;
    }
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        if(address<0x12001000u || address==0x12001010u) { const auto *p=reinterpret_cast<const uint8_t *>(&value);for(size_t i=0;i<sizeof(value);++i)m_vif1GsVisible[address+uint32_t(i)]=p[i]; }
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value] {m_vif1Instant->emit([this,address,value] {write8(address,value);});});return;
    }

    if(queueVu1Write(address,&value,sizeof(value)))return;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    if(isGsPrivReg(address)) gsraw::PrivWrite(&gs_regs,address,value,1); // TEMP(transformers)
    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code) {
                markVU1CodeModified();
                ps2_vu1new::directStore(physAddr, 1); // TEMP(transformers)
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 1u) == 0u && eeoptMemoryRange(address, 2u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        std::memcpy(base + ordinary.offset, &value, 2);
        return;
    }
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        if(address<0x12001000u || address==0x12001010u) { const auto *p=reinterpret_cast<const uint8_t *>(&value);for(size_t i=0;i<sizeof(value);++i)m_vif1GsVisible[address+uint32_t(i)]=p[i]; }
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value] {m_vif1Instant->emit([this,address,value] {write16(address,value);});});return;
    }

    if(queueVu1Write(address,&value,sizeof(value)))return;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    if(isGsPrivReg(address)) gsraw::PrivWrite(&gs_regs,address,value,2); // TEMP(transformers)
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code) {
                markVU1CodeModified();
                ps2_vu1new::directStore(physAddr, 2); // TEMP(transformers)
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 3u) == 0u && eeoptMemoryRange(address, 4u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        if (!ordinary.scratch) markModified(address, 4);
        std::memcpy(base + ordinary.offset, &value, 4);
        return;
    }
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        if(address<0x12001000u || address==0x12001010u) { const auto *p=reinterpret_cast<const uint8_t *>(&value);for(size_t i=0;i<sizeof(value);++i)m_vif1GsVisible[address+uint32_t(i)]=p[i]; }
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value] {m_vif1Instant->emit([this,address,value] {write32(address,value);});});return;
    }

    if(queueVu1Write(address,&value,sizeof(value)))return;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    if(isGsPrivReg(address)) gsraw::PrivWrite(&gs_regs,address,value,4); // TEMP(transformers)
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        gsPrivRegFence(m_gsVRAM);
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
        }
        return;
    }

    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code) {
                markVU1CodeModified();
                ps2_vu1new::directStore(physAddr, 4); // TEMP(transformers)
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && (address & 7u) == 0u && eeoptMemoryRange(address, 8u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        if (!ordinary.scratch) markModified(address, 8);
        std::memcpy(base + ordinary.offset, &value, 8);
        return;
    }
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        if(address<0x12001000u || address==0x12001010u) { const auto *p=reinterpret_cast<const uint8_t *>(&value);for(size_t i=0;i<sizeof(value);++i)m_vif1GsVisible[address+uint32_t(i)]=p[i]; }
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value] {m_vif1Instant->emit([this,address,value] {write64(address,value);});});return;
    }

    if(queueVu1Write(address,&value,sizeof(value)))return;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    if(isGsPrivReg(address)) gsraw::PrivWrite(&gs_regs,address,value,8); // TEMP(transformers)
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        gsPrivRegFence(m_gsVRAM);
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code) {
                markVU1CodeModified();
                ps2_vu1new::directStore(physAddr, 8); // TEMP(transformers)
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    // TEMP(transformers) runtime fast path: rejected accesses retain the complete original path below.
    EeoptMemoryRange ordinary;
    if (ps2EeoptEnabled() && true && eeoptMemoryRange(address & ~15u, 16u, ordinary)) {
        uint8_t *base = ordinary.scratch ? m_scratchpad : m_rdram;
        if (!ordinary.scratch) markModified(address & ~15u, 16);
        std::memcpy(base + ordinary.offset, &value, 16);
        return;
    }
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        if(address<0x12001000u || address==0x12001010u) { const auto *p=reinterpret_cast<const uint8_t *>(&value);for(size_t i=0;i<sizeof(value);++i)m_vif1GsVisible[address+uint32_t(i)]=p[i]; }
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value] {m_vif1Instant->emit([this,address,value] {write128(address,value);});});return;
    }

    if(queueVu1Write(address&~15u,&value,sizeof(value)))return;

    if (isGsPrivReg(address) && (!m_vif1Instant || (address&~7u)==0x12001000u || (address&~7u)==0x12001080u)) {Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-ee-fixes.patch (GPL-3.0), extended to PS2Memory:
    // SQ/SQC2 ignore address bits 3:0 on the R5900, so write the containing quadword instead of throwing (the throw
    // became SignalException(ADDRESS_ERROR) and a silently dropped store in PS2Runtime::Store128).
    address &= ~15u;

    if (isGsPrivReg(address)) gsPrivRegFence(m_gsVRAM);
    const bool scratch = ps2IsScratchpadAddress(address);
    uint32_t physAddr = scratch ? ps2ScratchpadOffset(address) : translateAddress(address);

    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr >= 0x10006000u && physAddr < 0x10008000u && vif1KickLogOnMem()) // TEMP(transformers): GIF/IPU FIFOs
    {
        static uint32_t s_fifoWrites = 0u;
        if (s_fifoWrites++ < 200u)
            std::cerr << "[fifo:write128] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " addr=0x" << std::hex
                      << physAddr << std::dec << std::endl;
    }
    if (!scratch && physAddr >= 0x10006000u && physAddr < 0x10007000u && gifFifoEnabled()) // GIF_FIFO (EE -> PATH3)
    {
        // GAME(transformers) NIGHT_B: direct EE stores to the GIF FIFO were dropped (fell through to the IO-register
        // path). Transformers' loading-screen vsync handler 0x26F1F0 (inlined sceGsPutDrawEnv) pushes
        // giftag[field] + draw env (FRAME_1 FBP 0/70, XYOFFSET half offset, SCISSOR, TEST, ZBUF) + clear through it
        // every field, as do 0x26E800 / 0x26EEC0; without it the warp vortex (VU1, PATH1) drew with the hub's leftover
        // FRAME_1 (FBP 112, FBW 8) and the displayed buffers FBP 0/70 stayed black (also black under parallel-gs).
        // Qwords are collected until a GIFtag with EOP has all its data, then submitted as one PATH3 packet.
        // PS2X_GIF_FIFO=0 restores the old drop for an A/B.
        alignas(16) uint8_t qw[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(qw), value);
        gifFifoPush(*this, qw);
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code) {
                markVU1CodeModified();
                ps2_vu1new::directStore(physAddr, 16); // TEMP(transformers)
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if(m_vif1Instant && isGsPrivReg(address) && !m_vif1Instant->replaying()) {
        for(unsigned i=0;i<4;++i)m_vif1GsVisible[address+i]=static_cast<uint8_t>(value>>(i*8));
        predictInstantCsrWrite(address,&value,sizeof(value));
        m_vif1Instant->enqueue([this,address,value]{m_vif1Instant->emit([this,address,value]{writeIORegister(address,value);});});
        return true;
    }
    if (!m_vif1Instant && (!m_vif1Stream ||
        (address>=0x10003C00u && address<0x10003D00u) ||
        (address>=0x10009000u && address<0x10009100u) ||
        (address>=0x10003000u && address<0x10003100u) ||
        (address>=0x1000A000u && address<0x1000A100u) || address==0x1000E000u)) fenceVu1();
    else if(m_vif1Stream)m_vif1Stream->poll(m_vif1Stream->now ? m_vif1Stream->now() : 0);

    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        gsPrivRegFence(m_gsVRAM);
        gsraw::PrivWrite(&gs_regs,address,value,4); // TEMP(transformers)
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (vif1KickLogOnMem()) // TEMP(transformers): who drives the IPU directly?
        {
            static uint32_t s_ipuWrites = 0u;
            if (s_ipuWrites++ < 400u)
                std::cerr << "[ipu:write] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " reg=0x" << std::hex
                          << address << " val=0x" << value << std::dec << std::endl;
        }
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    if (m_vif1Stream && address==0x10009000u && (value & 0x100u)) {
        constexpr uint32_t offsets[]={0,0x10,0x20,0x30,0x40,0x50,0x80};
        for (size_t i=0;i<7;++i) m_vif1DmaVisible[i]=m_ioRegisters[0x10009000u+offsets[i]];
        m_vif1DmaVisible[0]=value;
    }
    if(m_vif1Instant && address>=0x10003C00u && address<0x10003E00u) {
        const uint32_t off=address-0x10003C00u;
        m_vif1Instant->enqueue([this,off,value] {
            auto &r=m_vif1Actual.regs;
            switch(off) {
            case 0x10: if(value&1u){m_vif1Actual={};m_vif1Instant->emit([this]{bool was=m_path3Masked;m_path3Masked=false;if(was)flushMaskedPath3Packets();});}
                       if(value&8u)r.stat &= ~0x3F00u;break;
            case 0x30:r.mark=value&0xFFFFu;r.stat&=~(1u<<6);break;
            case 0x40:r.cycle=value&0xFFFFu;break;
            case 0x50:r.mode=value&3u;break;
            case 0x60:r.num=value&255u;break;
            case 0x70:r.mask=value;break;
            case 0x80:r.code=value;break;
            case 0x90:r.itops=value&1023u;break;
            case 0xA0:r.base=value&1023u;break;
            case 0xB0:r.ofst=value&1023u;break;
            case 0xC0:r.tops=value&1023u;break;
            case 0xD0:r.itop=value&1023u;break;
            case 0xE0:r.top=value&1023u;break;
            default:break; // matches the shipping MMIO write switch
            }
        });
        if(address==0x10003C10u && (value&1u)){m_vif1Scan={};m_vif1PredictedPath3Masked=false;}
    }
    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                if(!m_vif1Instant)ps2ResetVif1DirectContinuation();
                if(!m_vif1Instant)m_path3Masked = false;
                if (wasPath3Masked && !m_vif1Instant)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            if (vif1KickLogOnMem() && channelBase != 0x10009000u) // TEMP(transformers): every non-VIF1 DMA kick
            {
                static uint32_t s_kicks = 0u;
                if (s_kicks++ < 4000u)
                    std::cerr << "[dma:kick] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " ch=0x" << std::hex
                              << channelBase << " chcr=0x" << value << " madr=0x" << madr << " qwc=0x" << qwc << " tadr=0x"
                              << m_ioRegisters[channelBase + 0x30] << " sadr=0x" << m_ioRegisters[channelBase + 0x80]
                              << " dctrl=0x" << m_ioRegisters[0x1000E000u] << std::dec << std::endl;
            }

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            // GAME(transformers): toIPU (DMA channel 4, normal mode). The game's own movie feeder (0x275280) DMAs raw
            // MPEG-2 elementary-stream chunks for .m2v level movies straight into the IPU FIFO instead of using
            // sceMpegDemuxPss. Hand the bytes to the MPEG HLE decoder and complete the channel.
            if (channelBase == 0x1000B400u)
            {
                extern void ps2IpuDmaFeedFromGuest(uint8_t * rdram, uint32_t madr, uint32_t qwc); // Kernel/Stubs/MPEG.cpp
                m_ioRegisters[channelBase] = value;
                const uint32_t ipuMadr = madr & 0x0FFFFFF0u;
                const uint32_t ipuQwc = qwc & 0xFFFFu;
                if (((value >> 2) & 3u) == 0u && ipuMadr + ipuQwc * 16u <= PS2_RAM_SIZE)
                    ps2IpuDmaFeedFromGuest(m_rdram, ipuMadr, ipuQwc);
                m_ioRegisters[channelBase + 0x10u] = (madr + ipuQwc * 16u);
                m_ioRegisters[channelBase + 0x20u] = 0u;
                completeDmacChannel(channelBase, 4u);
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                const bool kickLog = channelBase == 0x10009000u && vif1KickLogOnMem(); // TEMP(transformers)
                if (kickLog)
                    std::cerr << "[vif1:kick] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " chcr=0x"
                              << std::hex << chcr << " mode=" << mode << " madr=0x" << madr << " qwc=0x" << qwc
                              << " tadr=0x" << m_ioRegisters[channelBase + 0x30] << std::dec << std::endl;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                    m_ioRegisters[channelBase + 0x10] = madr + qwc * 16u; // MADR advances past the data
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // Real hardware has no tag limit; this only guards against runaway/looping chains. 4096 was too low
                    // for heavy 3D frames (Transformers' Mini-Con screen): the walker stopped before the final 'end' tag,
                    // so the end-of-frame VIF i-bit never fired and the game fell back to its 3 s watchdog.
                    const int kMaxChainTags = 262144;
                    std::vector<uint8_t> chainBuf;
                    // eetrim: the chain is concatenated with repeated inserts, which regrew (realloc + copy) the
                    // buffer ~20 times per kick (~45 MB of copies per 1M-checkpoint window in the hub). Reserve the
                    // largest chain seen so far on this thread (+25%) up front; contents are unchanged.
                    static thread_local size_t t_chainBufReserve = 0u;
                    chainBuf.reserve(t_chainBufReserve);
                    // TEMP(transformers): preserve concatenation offsets BEFORE processVIF1Data can stall.
                    // PS2X_VIF1_SOURCE_DUMP=<kick-vsync-first>-<last>; at most eight chains per process.
                    struct SourcePiece { size_t offset; uint32_t bytes, guest, physical, tag; bool scratch, tagData; uint64_t tagLo, tagHi; };
                    std::vector<SourcePiece> sourcePieces;
                    std::vector<uint32_t> tteOffsets; // release fix: chain offsets of the TTE tag bytes (VIF1 only)
                    const uint64_t sourceVs = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
                    static const std::pair<long long, long long> sourceWindow = []()
                    {
                        const char *e = std::getenv("PS2X_VIF1_SOURCE_DUMP");
                        if (!e) return std::pair<long long, long long>{-1, -1};
                        char *end = nullptr;
                        const auto lo = std::strtoll(e, &end, 10);
                        return std::pair<long long, long long>{lo, end && *end == '-' ? std::strtoll(end + 1, nullptr, 10) : lo};
                    }();
                    static unsigned sourceDumpCount = 0;
                    const bool sourceOn = channelBase == 0x10009000u && sourceWindow.first >= 0 &&
                        sourceVs >= static_cast<uint64_t>(sourceWindow.first) &&
                        sourceVs <= static_cast<uint64_t>(sourceWindow.second) && sourceDumpCount < 8u;
                    uint32_t sourceTag = 0;
                    uint64_t sourceTagLo = 0, sourceTagHi = 0;


                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            if (sourceOn) // TEMP(transformers): canonical guest address plus exact backing offset.
                                sourcePieces.push_back({chainBuf.size(), chunk, scratch ? 0x70000000u + src : src,
                                                        src, sourceTag, scratch, false, sourceTagLo, sourceTagHi});
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        if (sourceOn) // TEMP(transformers)
                            sourcePieces.push_back({chainBuf.size(), 8u, localTagAddr + 8u, tagPhys + 8u,
                                                    localTagAddr, tagScratch, true, sourceTagLo, sourceTagHi});
                        if (channelBase == 0x10009000u) // release fix
                            tteOffsets.push_back(static_cast<uint32_t>(chainBuf.size()));
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    uint32_t madrOut = m_ioRegisters[channelBase + 0x10]; // MADR after the last tag's data (hardware behaviour)
                    std::string kickTags;              // TEMP(transformers): PS2X_VIF1_KICKLOG tag trace
                    std::string kickLastTags[4];       // TEMP(transformers)
                    const char *kickEnd = "cap";       // TEMP(transformers)
                    bool vif1SuspendHere = false;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        sourceTag = currentTagAddr; // TEMP(transformers)
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        kickEnd = "badtagaddr"; // TEMP(transformers)
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        if (sourceOn) // TEMP(transformers): retain raw source/SPR flag and TTE codes.
                        {
                            sourceTagLo = tag;
                            std::memcpy(&sourceTagHi, tp + 8, 8);
                        }
                        if (channelBase == 0x10009000u && vif1ChainSuspendEnabled() && !g_vif1Suspend.force &&
                            dmaTagLooksUnwritten(tag))
                        {
                            vif1SuspendHere = true; // tagAddr still points at this tag: resume re-reads it
                            kickEnd = "suspend";
                            break;
                        }
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;
                        kickEnd = "cap"; // TEMP(transformers)
                        if (kickLog && tagsProcessed <= 64)
                        {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), " %x:%x@%x%s%s", id, tagQwc, static_cast<uint32_t>(tag >> 32),
                                          irq ? "!" : "", tagInSPR ? "s" : "");
                            kickTags += buf;
                        }
                        if (kickLog) // TEMP(transformers): ring of the last 4 raw tags for the chain-end report
                        {
                            uint64_t hi = 0;
                            std::memcpy(&hi, tp + 8, sizeof(hi));
                            char buf[96];
                            std::snprintf(buf, sizeof(buf), "  #%d @%08x lo=%016llx hi=%016llx\n", tagsProcessed, currentTagAddr,
                                          static_cast<unsigned long long>(tag), static_cast<unsigned long long>(hi));
                            kickLastTags[(tagsProcessed - 1) & 3] = buf;
                        }

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (id <= 7u)
                            madrOut = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                        if (transferTagData)
                            appendVifTagData(currentTagAddr);

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                        {
                            endChain = true;
                            kickEnd = "irq+tie"; // TEMP(transformers)
                        }
                        else if (endChain)
                            kickEnd = id == 0u ? "refe" : id == 7u ? "end" : id == 6u ? "ret-empty" : "badid"; // TEMP(transformers)
                        if (endChain)
                            break;
                    }
                    if (sourceOn && !chainBuf.empty()) // TEMP(transformers): binary stream + all source pieces, no per-tag stderr spam.
                    {
                        char prefix[96];
                        std::snprintf(prefix, sizeof(prefix), "vif1_source_%03u", sourceDumpCount++);
                        std::ofstream stream(std::string(prefix) + ".bin", std::ios::binary);
                        stream.write(reinterpret_cast<const char *>(chainBuf.data()), static_cast<std::streamsize>(chainBuf.size()));
                        std::ofstream map(std::string(prefix) + ".map");
                        map << "vs=" << sourceVs << " bytes=" << chainBuf.size() << " end=" << kickEnd << '\n';
                        map << "offset bytes guest physical tag scratch tagData tagLo tagHi\n";
                        for (const auto &s : sourcePieces)
                            map << std::dec << s.offset << ' ' << s.bytes << ' ' << std::hex << s.guest << ' '
                                << s.physical << ' ' << s.tag << ' ' << s.scratch << ' ' << s.tagData
                                << ' ' << s.tagLo << ' ' << s.tagHi << '\n';
                        std::cerr << "[vif1:source] " << prefix << " vs=" << sourceVs << " bytes=" << chainBuf.size()
                                  << " pieces=" << sourcePieces.size() << " written=" << (stream.good() && map.good()) << '\n';
                    }
                    if (kickLog) // TEMP(transformers)
                    {
                        std::cerr << "[vif1:chain] tags=" << tagsProcessed << " bytes=" << chainBuf.size() << " end=" << kickEnd
                                  << " tte=" << transferTagData << " tie=" << tieEnabled << " nextTadr=0x" << std::hex << tagAddr
                                  << " dctrl=0x" << m_ioRegisters[0x1000E000u] << " dstadr=0x" << m_ioRegisters[0x1000E040u]
                                  << " madrOut=0x" << madrOut << " sprD30=0x"
                                  << [&]() { uint32_t v = 0; std::memcpy(&v, m_scratchpad + 0xD30u, 4); return v; }()
                                  << std::dec << " :" << kickTags << (tagsProcessed > 64 ? " ..." : "") << std::endl;
                        if (std::strcmp(kickEnd, "end") != 0)
                        {
                            std::cerr << "[vif1:chain-tail]\n";
                            for (int i = std::max(0, tagsProcessed - 4); i < tagsProcessed; ++i)
                                std::cerr << kickLastTags[i & 3];
                            std::cerr << std::flush;
                        }
                    }

                    if (channelBase == 0x10009000u)
                    {
                        if (vif1SuspendHere)
                        {
                            if (!g_vif1Suspend.active || !g_vif1Suspend.resuming)
                            {
                                g_vif1Suspend.sinceVsync = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
                                g_vif1Suspend.retries = 0u;
                            }
                            g_vif1Suspend.active = true;
                            g_vif1Suspend.chcr = (value & ~0x30u) | ((asp & 0x3u) << 4) | 0x100u; // keep ASP for the resume
                            g_vif1Suspend.tagAddr = tagAddr;
                        }
                        else
                        {
                            g_vif1Suspend.active = false;
                        }
                        g_vif1Suspend.force = false;
                    }

                    { // TEMP(transformers): report new maximum chain lengths
                        static int s_maxTags = 0;
                        if (tagsProcessed > s_maxTags && tagsProcessed > 1024)
                        {
                            s_maxTags = tagsProcessed;
                            std::cerr << "[dma:chain] new max tags=" << tagsProcessed << " channel=0x" << std::hex << channelBase
                                      << std::dec << std::endl;
                        }
                    }
                    if (tagsProcessed >= kMaxChainTags)
                    {
                        static uint32_t s_chainCapWarnings = 0u;
                        if (s_chainCapWarnings++ < 16u)
                            std::cerr << "[dma:chain] WARNING: tag cap " << kMaxChainTags << " hit on channel 0x" << std::hex
                                      << channelBase << " at tag 0x" << tagAddr << std::dec << std::endl;
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    // Real DMAC leaves MADR just past the last transferred qword (e.g. after an 'end' tag:
                    // endTag + 0x10 + QWC*16). Games read it back (Transformers' VIF1 handler checks a marker there).
                    m_ioRegisters[channelBase + 0x10] = madrOut;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        t_chainBufReserve = std::max(t_chainBufReserve,
                                                     std::min<size_t>(chainBuf.size() + chainBuf.size() / 4u, 64u << 20));
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            // release fix (also when empty, so a reused buffer address never picks up a stale entry)
                            ps2x_vifunpack::registerChainTte(pt.chainData.data(), std::move(tteOffsets));
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    if (m_vif1Stream && channelBase==0x10009000u && m_pendingVif1Transfers.empty() && !g_vif1Suspend.active)
                        m_pendingVif1Transfers.emplace_back(); // Empty DMA still has a timed completion/IRQ.
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    { // TEMP(transformers): report each SPR DMA channel/mode the first time it is used
        static bool s_seen[2][4] = {};
        bool &seen = s_seen[channelBase == kSprToChannel ? 1 : 0][mode];
        if (!seen)
        {
            seen = true;
            std::cerr << "[spr:dma] first " << (channelBase == kSprToChannel ? "toSPR" : "fromSPR") << " mode=" << mode
                      << " chcr=0x" << std::hex << chcr << " madr=0x" << m_ioRegisters[channelBase + 0x10u] << " qwc=0x"
                      << m_ioRegisters[channelBase + 0x20u] << " tadr=0x" << m_ioRegisters[channelBase + 0x30u]
                      << " sadr=0x" << m_ioRegisters[channelBase + 0x80u] << " sqwc=0x" << m_ioRegisters[0x1000E030u]
                      << std::dec << " vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << std::endl;
        }
    }
    if (mode == 1u || mode == 2u)
    {
        // Chain and interleave modes (only normal mode existed before; anything else was silently dropped, so packets
        // a game assembles in scratchpad and scatters into its main-RAM DMA list never arrived - Transformers'
        // Options/globe screens then kicked VIF1 chains with holes of stale data and lost the end-of-frame i-bit).
        const bool fromSpr = channelBase == kSprFromChannel;
        uint32_t sadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;
        uint32_t madr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
        const bool tie = (chcr & (1u << 7)) != 0u;
        uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;

        // Copies qwc qwords between main RAM (ramAddr) and scratchpad (sadr, wrapping at 16 KB).
        auto copyQw = [&](uint32_t ramAddr, uint32_t qwCount, bool toRam) -> bool
        {
            if (qwCount == 0u)
                return true;
            const bool ramIsSpr = isScratchpad(ramAddr) || (ramAddr & 0x80000000u) != 0u;
            uint32_t ramOff = 0u;
            try
            {
                ramOff = ramIsSpr ? (ramAddr & 0x3FF0u) : translateAddress(ramAddr & 0x7FFFFFFFu);
            }
            catch (const std::exception &)
            {
                return false;
            }
            uint8_t *ramBase = ramIsSpr ? m_scratchpad : m_rdram;
            const uint32_t ramSize = ramIsSpr ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            for (uint32_t i = 0; i < qwCount; ++i)
            {
                const uint32_t ro = (ramOff + i * 16u) % ramSize;
                uint8_t *ram = ramBase + ro;
                uint8_t *spr = m_scratchpad + sadr;
                if (toRam)
                {
                    std::memcpy(ram, spr, 16u);
                    if (!ramIsSpr)
                        markModified(ro, 16u);
                }
                else
                {
                    std::memcpy(spr, ram, 16u);
                }
                sadr = (sadr + 16u) & 0x3FF0u;
            }
            return true;
        };

        if (mode == 2u)
        {
            // Interleave: TQWC qwords transferred, SQWC qwords skipped in main RAM, until QWC is exhausted.
            const uint32_t dsqwc = m_ioRegisters[0x1000E030u];
            const uint32_t sqwc = dsqwc & 0xFFu;
            const uint32_t tqwc = std::max<uint32_t>(1u, (dsqwc >> 16) & 0xFFu);
            uint32_t remaining = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
            while (remaining != 0u)
            {
                const uint32_t n = std::min(remaining, tqwc);
                if (!copyQw(madr, n, fromSpr))
                    break;
                remaining -= n;
                madr += (n + (remaining != 0u ? sqwc : 0u)) * 16u;
            }
        }
        else if (fromSpr)
        {
            // fromSPR destination chain: DMAtags come from the scratchpad stream (cnts=0, cnt=1, end=7); each tag's
            // ADDR is the main-RAM destination of the QWC qwords that follow it.
            uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
            if (qwc != 0u) // resume a partially transferred block first
                copyQw(madr, qwc, true), madr += qwc * 16u;
            for (int n = 0; n < 65536; ++n)
            {
                uint64_t tag = 0;
                std::memcpy(&tag, m_scratchpad + sadr, sizeof(tag));
                sadr = (sadr + 16u) & 0x3FF0u;
                qwc = static_cast<uint32_t>(tag & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
                const bool irq = ((tag >> 31) & 1ull) != 0ull;
                lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                madr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
                if (!copyQw(madr, qwc, true))
                    break;
                madr += qwc * 16u;
                if (id == 0u)
                    m_ioRegisters[0x1000E060u] = madr; // cnts: stall address (D_STADR)
                if (id == 7u || (irq && tie))
                    break;
            }
        }
        else
        {
            // toSPR source chain: DMAtags in main RAM (TADR), data into scratchpad at SADR.
            uint32_t tadr = m_ioRegisters[channelBase + 0x30u];
            uint32_t asr0 = m_ioRegisters[channelBase + 0x40u];
            uint32_t asr1 = m_ioRegisters[channelBase + 0x50u];
            uint32_t asp = (chcr >> 4) & 0x3u;
            for (int n = 0; n < 65536; ++n)
            {
                uint32_t tagOff = 0u;
                try
                {
                    tagOff = translateAddress(tadr & 0x7FFFFFFFu);
                }
                catch (const std::exception &)
                {
                    break;
                }
                if (tagOff + 16u > PS2_RAM_SIZE)
                    break;
                uint64_t tag = 0;
                std::memcpy(&tag, m_rdram + tagOff, sizeof(tag));
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
                const bool irq = ((tag >> 31) & 1ull) != 0ull;
                const uint32_t addr = static_cast<uint32_t>(tag >> 32);
                lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                uint32_t data = tadr + 16u;
                bool end = false;
                switch (id)
                {
                case 0: data = addr; end = true; break;                          // refe
                case 1: tadr = data + qwc * 16u; break;                           // cnt
                case 2: tadr = addr; break;                                       // next
                case 3: case 4: data = addr; tadr += 16u; break;                  // ref/refs
                case 5:                                                           // call
                    if (asp == 0u) asr0 = data + qwc * 16u; else asr1 = data + qwc * 16u;
                    asp = std::min<uint32_t>(asp + 1u, 2u);
                    tadr = addr;
                    break;
                case 6:                                                           // ret
                    if (asp == 2u) { tadr = asr1; asp = 1u; }
                    else if (asp == 1u) { tadr = asr0; asp = 0u; }
                    else end = true;
                    break;
                default: end = true; break;                                       // end
                }
                if (!copyQw(data, qwc, false))
                    break;
                madr = data + qwc * 16u;
                if (end || (irq && tie))
                    break;
            }
            m_ioRegisters[channelBase + 0x30u] = tadr;
            m_ioRegisters[channelBase + 0x40u] = asr0;
            m_ioRegisters[channelBase + 0x50u] = asr1;
            chcr = (chcr & ~0x30u) | ((asp & 0x3u) << 4);
        }

        m_ioRegisters[channelBase + 0x10u] = madr;
        m_ioRegisters[channelBase + 0x20u] = 0u;
        m_ioRegisters[channelBase + 0x80u] = sadr;
        m_ioRegisters[channelBase] = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
        completeDmacChannel(channelBase, fromSpr ? 8u : 9u);
        return true;
    }
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
            { extern void ps2DbgRecordEeWrite(uint32_t, uint32_t, const char *); ps2DbgRecordEeWrite(mainOffset + copied, chunk, "spr-from"); } // TEMP(transformers)
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    if ((m_vif1Stream || m_vif1Instant) && hadVif1) {
        if(!m_vif1Instant)fenceVu1();
        std::vector<uint8_t> input;
        std::vector<uint32_t> segments;
        std::vector<std::vector<uint32_t>> tte; // TEMP(transformers) qualification: release fix TTE offsets per segment
        auto append=[&](const uint8_t *p,uint32_t n) {
            if (n>64u*1024u*1024u-input.size()) throw std::runtime_error("VIF DMA input exceeded 64 MiB");
            input.insert(input.end(),p,p+n); segments.push_back(n);
        };
        for (const auto &p:m_pendingVif1Transfers) {
            if (!p.chainData.empty()) {
                append(p.chainData.data(),static_cast<uint32_t>(p.chainData.size()));
                tte.resize(segments.size());tte.back()=ps2x_vifunpack::takeChainTte(p.chainData.data());
            }
            else if (p.qwc) {
                uint32_t src=translateAddress(p.srcAddr);
                uint64_t left=uint64_t(p.qwc)*16u;
                const uint32_t limit=p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                const uint8_t *base=p.fromScratchpad ? m_scratchpad : m_rdram;
                while (left) {
                    if (src>=limit) src=0;
                    uint32_t n=static_cast<uint32_t>(std::min<uint64_t>(left,limit-src));
                    append(base+src,n); src+=n; left-=n;
                }
            }
        }
        if(m_vif1Instant){tte.resize(segments.size());submitVif1Instant(std::move(input),std::move(segments),true,std::move(tte));}
        else {
        m_vif1Visible=vif1_regs;
        const auto err=m_ioRegisters.find(0x10003C20u);
        m_vif1Stream->err=err==m_ioRegisters.end() ? 0u : err->second;
        m_vif1Stream->submit(std::move(input),[this,segments=std::move(segments)](const uint8_t *p,uint32_t) {
            for (uint32_t n:segments) { processVIF1Data(p,n);p+=n; }
        });
        }
    } else for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            ps2x_vifunpack::beginChain(p.chainData.data()); // release fix: hardware quadword phase of this chain
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
            ps2x_vifunpack::endChain();
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    if (m_gifArbiter)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1 && !g_vif1Suspend.active) // a suspended chain completes when its walk reaches the end
    {
        auto complete=[this] {
            completeDmacChannel(VIF1_CHANNEL,1u);
            m_ioRegisters[VIF1_CHANNEL + 0x20]=0;
        };
        if (m_vif1Stream && m_vif1Stream->pending()) {
            constexpr uint32_t offsets[]={0,0x10,0x20,0x30,0x40,0x50,0x80};
            std::array<uint32_t,7> finalRegs{};
            for (size_t i=0;i<7;++i) {
                finalRegs[i]=m_ioRegisters[VIF1_CHANNEL+offsets[i]];
                m_ioRegisters[VIF1_CHANNEL+offsets[i]]=m_vif1DmaVisible[i];
            }
            m_vif1Stream->dmaComplete=[this,finalRegs,complete] {
                constexpr uint32_t offsets[]={0,0x10,0x20,0x30,0x40,0x50,0x80};
                for (size_t i=0;i<7;++i) m_ioRegisters[VIF1_CHANNEL+offsets[i]]=finalRegs[i];
                complete();
            };
        }
        else {
            raiseDStatChannel(1u); queueCompletedDmacCause(1u);
            m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
            m_ioRegisters[VIF1_CHANNEL + 0x20]=0;
        }
    }
}

// Retry a suspended VIF1 source chain (see Vif1ChainSuspend). Cheap when nothing is suspended or the tag at TADR still
// looks unwritten; called from the EE scheduler's event processing and from DMA status reads.
void ps2Vif1ChainResumeTick(PS2Memory &mem)
{
    if (!g_vif1Suspend.active || g_vif1Suspend.resuming)
        return;
    const uint64_t vs = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
    const bool timedOut = vs > g_vif1Suspend.sinceVsync + kVif1SuspendTimeoutVsyncs;
    const uint32_t tadr = g_vif1Suspend.tagAddr;
    const bool tagInSpr = (tadr & 0x80000000u) != 0u || (tadr >= 0x70000000u && tadr < 0x70004000u);
    if (!timedOut && !tagInSpr)
    {
        const uint32_t phys = tadr & 0x01FFFFF0u;
        uint64_t tag = 0;
        std::memcpy(&tag, mem.getRDRAM() + phys, sizeof(tag));
        if (dmaTagLooksUnwritten(tag))
            return;
    }
    ++g_vif1Suspend.retries;
    if (vif1KickLogOnMem() || timedOut || g_vif1Suspend.retries <= 4u) // TEMP(transformers)
        std::cerr << "[vif1:resume] vs=" << vs << " tadr=0x" << std::hex << tadr << std::dec << " after "
                  << (vs - g_vif1Suspend.sinceVsync) << " vsyncs" << (timedOut ? " (timeout: forcing walk)" : "")
                  << std::endl;
    g_vif1Suspend.force = timedOut;
    g_vif1Suspend.resuming = true;
    mem.writeIORegister(0x10009000u, g_vif1Suspend.chcr);
    g_vif1Suspend.resuming = false;
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
    if (ps2drain::fast) m_completedDmacPending.store(true, std::memory_order_release);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    if (ps2drain::fast && !m_completedDmacPending.load(std::memory_order_acquire)) return {};
    if (ps2drain::stats && ps2drain::depth) ++ps2drain::counts.mutexes;
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    if (ps2drain::fast) m_completedDmacPending.store(false, std::memory_order_release);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if(!m_vif1Instant)fenceVu1();
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    // GENERIC (ported from SSX3 SSX-0042, crossover integration update): an MSKPATH3 unmask window releases ONE GS
    // packet (GIFtags up to and including the one with EOP), like hardware/PCSX2 where a re-mask takes effect at the
    // next packet end and our VIF1 re-masks instantly. Games interleave PATH3 texture uploads with PATH1/2 draws this
    // way (SSX3's movie player toggles MSKPATH3 96 times per frame; releasing the whole queue at the first unmask made
    // every draw sample the last-uploaded columns). Other callers (unmasked submit, VIF1 reset) drain everything.
    // PS2X_PATH3_MASK_PACKET=0 (read in ps2_vif1_interpreter.cpp) restores the drain-all behaviour.
    if (g_path3ReleaseOnePacket)
    {
        g_path3ReleaseOnePacket = false;
        bool done = false;
        while (!done && !m_path3MaskedFifo.empty())
        {
            const std::vector<uint8_t> &front = m_path3MaskedFifo.front();
            size_t offset = g_path3FrontOffset;
            const size_t start = offset;
            while (offset + 16u <= front.size())
            {
                uint64_t tag = 0u;
                std::memcpy(&tag, front.data() + offset, sizeof(tag));
                const uint64_t nloop = tag & 0x7FFFu;
                const bool eop = ((tag >> 15) & 1u) != 0u;
                const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
                uint64_t nreg = (tag >> 60) & 0xFu;
                if (nreg == 0u)
                    nreg = 16u;
                uint64_t dataBytes = 0u;
                if (flg == 0u)
                    dataBytes = nloop * nreg * 16u;
                else if (flg == 1u)
                    dataBytes = ((nloop * nreg * 8u) + 15u) & ~uint64_t(15u);
                else
                    dataBytes = nloop * 16u;
                offset = std::min<size_t>(front.size(), offset + 16u + static_cast<size_t>(dataBytes));
                if (eop)
                {
                    done = true;
                    break;
                }
            }
            if (offset > start)
                emit(front.data() + start, static_cast<uint32_t>(offset - start));
            if (offset + 16u > front.size())
            {
                m_path3MaskedFifo.erase(m_path3MaskedFifo.begin());
                g_path3FrontOffset = 0u;
            }
            else
            {
                g_path3FrontOffset = offset;
            }
        }
        if (m_gifArbiter && drainImmediately)
            m_gifArbiter->drain();
        return;
    }

    bool first = true;
    for (const auto &packet : m_path3MaskedFifo)
    {
        const size_t skip = first ? std::min<size_t>(g_path3FrontOffset, packet.size()) : 0u;
        first = false;
        if (packet.size() >= skip + 16u)
            emit(packet.data() + skip, static_cast<uint32_t>(packet.size() - skip));
    }
    m_path3MaskedFifo.clear();
    g_path3FrontOffset = 0u;

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

std::atomic<uint64_t> g_ps2LastPath1Vsync{0}; // presented-frame vsync (ps2dbg) of the last PATH1 (VU1 XGKICK) GIF packet
std::atomic<uint64_t> g_ps2LastPath1EeVsync{0}; // the same in guest EE time (gs().vsyncTick): deterministic, for guest-side hooks

namespace // TEMP(transformers): ws lane, PS2X_WS_GIFLOG=<vsFrom>:<vsTo> - one line per GIF packet (path, source, prims, bbox)
{
void wsGifLog(int path, const uint8_t *data, uint32_t sizeBytes, const uint8_t *rdram)
{
    static long long s_from = -2, s_to = -1;
    if (s_from == -2)
    {
        s_from = -1;
        if (const char *e = std::getenv("PS2X_WS_GIFLOG"))
        {
            s_from = std::atoll(e);
            const char *c = std::strchr(e, ':');
            s_to = c ? std::atoll(c + 1) : s_from + 2;
        }
    }
    if (s_from < 0)
        return;
    const long long vs = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
    if (vs < s_from || vs > s_to)
        return;
    uint32_t prim = 0xFFFFFFFFu, tbp = 0xFFFFFFFFu, ztst = 0xFFFFFFFFu, fbp = 0xFFFFFFFFu;
    int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30), nxyz = 0;
    auto reg = [&](uint32_t r, uint64_t v) {
        switch (r)
        {
        case 0x00: prim = static_cast<uint32_t>(v & 0x7FFu); break;
        case 0x06: case 0x07: tbp = static_cast<uint32_t>(v & 0x3FFFu); break;
        case 0x47: case 0x48: ztst = static_cast<uint32_t>((v >> 16) & 7u) | (static_cast<uint32_t>((v >> 16) & 1u) << 3); break;
        case 0x4C: case 0x4D: fbp = static_cast<uint32_t>(v & 0x1FFu); break;
        case 0x04: case 0x05: case 0x0C: case 0x0D:
        {
            const int x = static_cast<int>(v & 0xFFFFu), y = static_cast<int>((v >> 16) & 0xFFFFu);
            minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); ++nxyz;
            break;
        }
        default: break;
        }
    };
    uint32_t off = 0;
    while (off + 16 <= sizeBytes)
    {
        uint64_t lo, hi;
        std::memcpy(&lo, data + off, 8); std::memcpy(&hi, data + off + 8, 8);
        off += 16;
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu); if (!nreg) nreg = 16;
        if ((lo >> 46) & 1u) prim = static_cast<uint32_t>((lo >> 47) & 0x7FFu);
        if (flg == 0)
        {
            for (uint32_t l = 0; l < nloop; ++l)
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (off + 16 > sizeBytes) goto done;
                    uint64_t a, b; std::memcpy(&a, data + off, 8); std::memcpy(&b, data + off + 8, 8); off += 16;
                    const uint32_t rd = static_cast<uint32_t>((hi >> (r * 4)) & 0xFu);
                    if (rd == 0xE) reg(static_cast<uint32_t>(b & 0xFFu), a);
                    else if (rd == 0x5 || rd == 0x4) reg(rd, (a & 0xFFFFu) | ((a >> 32) & 0xFFFFu) << 16);
                    else if (rd == 0x0) reg(0, a);
                    else if (rd == 0x6 || rd == 0x7) reg(rd, a);
                }
        }
        else if (flg == 1)
        {
            for (uint32_t l = 0; l < nloop; ++l)
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (off + 8 > sizeBytes) goto done;
                    uint64_t a; std::memcpy(&a, data + off, 8); off += 8;
                    reg(static_cast<uint32_t>((hi >> (r * 4)) & 0xFu), a);
                }
            if ((nloop * nreg) & 1u) off += 8;
        }
        else
            off += nloop * 16u;
    }
done:
    const long long src = (rdram && data >= rdram && data < rdram + 32u * 1024u * 1024u) ? static_cast<long long>(data - rdram) : -1;
    std::fprintf(stderr, "[ws:gif] vs=%lld path=%d size=%u src=%s0x%llx prim=0x%x tbp=0x%x ztst=0x%x fbp=0x%x nxyz=%d bbox=%d,%d-%d,%d\n",
                 vs, path, sizeBytes, src < 0 ? "-" : "", src < 0 ? 0ull : static_cast<unsigned long long>(src), prim, tbp, ztst,
                 fbp, nxyz, nxyz ? minx / 16 : 0, nxyz ? miny / 16 : 0, nxyz ? maxx / 16 : 0, nxyz ? maxy / 16 : 0);
    if (path == 2 && nxyz > 0 && nxyz <= 12 && sizeBytes <= 448u && (prim == 0x5Bu || prim == 0x5Cu || prim == 0x54u))
    {
        for (uint32_t q = 0; q < sizeBytes / 16u && q < 12u; ++q)
        {
            uint32_t w[4];
            std::memcpy(w, data + q * 16u, 16);
            std::fprintf(stderr, "[ws:gifq] %u: %08x %08x %08x %08x\n", q, w[0], w[1], w[2], w[3]);
        }
    }
}
} // namespace

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if(m_vif1Instant && !m_vif1Instant->replaying()) {
        if(!data || sizeBytes<16)return;
        if(instantWorker() || !m_vif1Instant->directReady()) {
            m_vif1Instant->packet(data,sizeBytes,static_cast<uint32_t>(pathId),drainImmediately,path2DirectHl);
            return;
        }
        ++m_vif1Instant->directPackets; // TEMP(transformers)
    }
    if (vif1StreamExecuting()) {
        std::vector<uint8_t> copy(data,data+sizeBytes);
        m_vif1Stream->emit([this,pathId,copy=std::move(copy),drainImmediately,path2DirectHl] {
            submitGifPacket(pathId,copy.data(),static_cast<uint32_t>(copy.size()),drainImmediately,path2DirectHl);
        },sizeBytes);
        return;
    }
    if(!m_vif1Instant)fenceVu1();
    if (!data || sizeBytes < 16)
        return;
    wsGifLog(static_cast<int>(pathId), data, sizeBytes, m_rdram); // TEMP(transformers)
    if (pathId == GifPathId::Path1) // widescreen presenter: a frame with VU1 geometry is rendered Hor+ (ps2_runtime.cpp)
    {
        g_ps2LastPath1Vsync.store(ps2dbg::vsyncTick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_ps2LastPath1EeVsync.store(gs_regs.vsyncTick.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback) { gsraw::PathScope path(static_cast<uint32_t>(pathId));m_gifPacketCallback(data,sizeBytes); } // TEMP(transformers)

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (m_vif1Stream) fenceVu1();
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_vif1Stream) fenceVu1();
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        submitGifPacket(GifPathId::Path3,data,sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    if(m_vif1Instant)return false; // ordered PATH3 fallback

    if (m_vif1Stream) fenceVu1();
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    if(gs.rawRecordingEnabled()) // TEMP(transformers): same original GIF tags/data, no DMA tags.
    {
        std::vector<uint8_t> packet(setupPayload,setupPayload+80u);
        packet.insert(packet.end(),imageGifTag,imageGifTag+16u);packet.insert(packet.end(),imageData,imageData+imageBytes);
        gsraw::PathScope path(3);gs.processGIFPacket(packet.data(),static_cast<uint32_t>(packet.size()));
    }
    else gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    if(m_vif1Instant)return false; // ordered PATH3 fallback

    if (m_vif1Stream) fenceVu1();
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (m_vif1Stream) {
        const bool vifRead=address>=0x10003C00u && address<0x10003D00u;
        const bool dmaRead=address>=0x10009000u && address<0x10009100u;
        if (vifRead || dmaRead) {
            if (vifRead && address!=0x10003C00u) {Vu1FenceReason r_(kVu1FenceMem);fenceVu1();} // qualification: reason; non-status VIF registers are true state dependencies
            m_vif1Stream->poll(m_vif1Stream->now ? m_vif1Stream->now() : 0);
            if (m_vif1Stream->pending()) {
                if (address==0x10009000u) return m_ioRegisters[address] | 0x100u;
                if (vifRead) {
                    const uint32_t off=address-0x10003C00u;
                    switch (off) {
                    case 0: return (m_vif1Visible.stat & ~(3u|0x1F000000u|(1u<<23))) |
                        (m_ioRegisters[address] & (1u<<23)) | 2u | (1u<<24);
                    case 0x30: return m_vif1Visible.mark;
                    case 0x60: return m_vif1Visible.num;
                    case 0x80: return m_vif1Visible.code;
                    case 0x90: return m_vif1Visible.itops;
                    case 0xA0: return m_vif1Visible.base;
                    case 0xB0: return m_vif1Visible.ofst;
                    case 0xC0: return m_vif1Visible.tops;
                    case 0xD0: return m_vif1Visible.itop;
                    case 0xE0: return m_vif1Visible.top;
                    default: return m_ioRegisters[address];
                    }
                }
                return m_ioRegisters[address];
            }
        } else if ((address>=0x10003000u && address<0x10003100u) ||
                   (address>=0x1000A000u && address<0x1000A100u)) fenceVu1();
        else m_vif1Stream->poll(m_vif1Stream->now ? m_vif1Stream->now() : 0);
    } else if (!m_vif1Instant) fenceVu1();
    else m_vif1Instant->drain();
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        if(m_vif1Instant && ((address&~7u)==0x12001000u || (address&~7u)==0x12001080u)){Vu1FenceReason r_(kVu1FenceCsr);fenceVu1();} // qualification: reason
        gsPrivRegFence(m_gsVRAM);
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>(((gs_regs.csr.load() | kGsCsrFifoEmpty) >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    // VIF0/VIF1 status & state registers reflect the interpreter's live state (INT from the i-bit, MARK,
    // CODE, ...). Transfers complete synchronously, so VPS (bits 0-1) and FQC (bits 24-28) read as idle.
    if ((address >= 0x10003800u && address < 0x10003900u) || (address >= 0x10003C00u && address < 0x10003D00u))
    {
        const bool isVif1 = address >= 0x10003C00u;
        const VIFRegisters &vif = isVif1 ? vif1_regs : vif0_regs;
        switch (address - (isVif1 ? 0x10003C00u : 0x10003800u))
        {
        case 0x00:
        {
            const auto guest = m_ioRegisters.find(address);
            const uint32_t guestFdr = (guest != m_ioRegisters.end()) ? (guest->second & (1u << 23)) : 0u;
            return (vif.stat & ~(0x3u | 0x1F000000u | (1u << 23))) | guestFdr;
        }
        case 0x30: return vif.mark;
        case 0x60: return vif.num;
        case 0x80: return vif.code;
        case 0x90: return vif.itops;
        case 0xA0: return vif.base;
        case 0xB0: return vif.ofst;
        case 0xC0: return vif.tops;
        case 0xD0: return vif.itop;
        case 0xE0: return vif.top;
        default: break;
        }
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               ((m_vif1Instant ? m_vif1PredictedPath3Masked : m_path3Masked) ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if (address == 0x10009000u && g_vif1Suspend.active)
            {
                ps2Vif1ChainResumeTick(*this);
                if (g_vif1Suspend.active)
                    return m_ioRegisters[address] | 0x100u; // still busy: the rest of the chain is not written yet
            }
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}

bool PS2Memory::queueVu1Write(uint32_t address,const void *data,uint32_t n)
{
    if(ps2IsScratchpadAddress(address))return false;
    if(!m_vif1Instant || instantWorker())return false;
    const uint32_t phys=translateAddress(address);
    if(phys<PS2_VU1_CODE_BASE || uint64_t(phys)+n>uint64_t(PS2_VU1_DATA_BASE)+PS2_VU1_DATA_SIZE)return false;
    if(n<16u && (address&(n-1u)))throw std::runtime_error("Unaligned VU1 memory write");
    if(phys<PS2_VU1_DATA_BASE && uint64_t(phys)+n>PS2_VU1_DATA_BASE)return false;
    std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),static_cast<const uint8_t *>(data)+n);
    m_vif1Instant->enqueue([this,phys,bytes=std::move(bytes)] {
        const bool code=phys<PS2_VU1_DATA_BASE;
        auto *dst=code ? m_vu1Code : m_vu1Data;
        std::memcpy(dst+phys-(code?PS2_VU1_CODE_BASE:PS2_VU1_DATA_BASE),bytes.data(),bytes.size());
        if(code) { markVU1CodeModified(); ps2_vu1new::directStore(phys, uint32_t(bytes.size())); } // TEMP(transformers)
    });return true;
}

bool PS2Memory::readInstantGsSetting(uint32_t address,void *dst,uint32_t n) const
{
    if(!m_vif1Instant || (address>=0x12001000u && (address&~7u)!=0x12001010u) || m_vif1Instant->replaying())return false;
    auto *p=static_cast<uint8_t *>(dst);
    bool any=false;for(uint32_t i=0;i<n;++i)any=any||m_vif1GsVisible.count(address+i);
    if(!any)return false;
    for(uint32_t i=0;i<n;++i) {
        const auto a=address+i;const auto *reg=gsRegPtr(const_cast<GSRegisters&>(gs_regs),a);
        const auto it=m_vif1GsVisible.find(a);
        p[i]=!reg ? 0 : it!=m_vif1GsVisible.end() ? it->second : uint8_t(*reg>>((a&7u)*8u));
    }
    return true;
}

void PS2Memory::predictInstantCsrWrite(uint32_t address,const void *value,uint32_t size) {
    // Match CURRENT mode 0: write8/write16/write128 never reach the CSR helpers,
    // since GS privileged addresses are outside isIoRegister's EE IO range.
    // Only aligned write32/write64 (and direct writeIORegister) update CSR here.
    if((size!=4 && size!=8) || (address&(size-1u)))return;
    if(address>0x12001001u || uint64_t(address)+size<=0x12001001u)return;
    const auto field=uint64_t(static_cast<const uint8_t*>(value)[0x12001001u-address]&0x20u)<<8;
    m_vif1EeField.store(field);
    auto *q=m_vif1Instant.get();q->enqueue([q,field]{q->oracleField=field;});
}
void PS2Memory::publishInstantField(uint64_t tick) {
    if(!m_vif1Instant)return;
    const auto field=(tick&1u)?0x2000ull:0ull;m_vif1EeField.store(field);
    auto *q=m_vif1Instant.get();
    q->enqueue([q,tick]{q->oracleField=(tick&1u)?0x2000ull:0ull;});
}
bool PS2Memory::readInstantField(uint32_t address,uint32_t pc,uint32_t &value) {
    if(!m_vif1Instant || address!=0x12001000u || !m_rdram)return false;
    // GAME(transformers): consumed-bit specialization, guarded by both LW and ANDI.
    // Every other CSR access (including all widths / SIGNAL / FINISH) keeps the exact fence.
    uint32_t lw=0,mask=0,middle=0;
    if(pc==0x25DF70u) {std::memcpy(&lw,m_rdram+pc,4);std::memcpy(&mask,m_rdram+pc+8,4);std::memcpy(&middle,m_rdram+pc+4,4);if(lw!=0x8c231000u || middle!=0x8f82bde0u || mask!=0x30632000u)return false;}
    else if(pc==0x26E9A8u) {std::memcpy(&lw,m_rdram+pc,4);std::memcpy(&mask,m_rdram+pc+4,4);if(lw!=0x8c221000u || mask!=0x30422000u)return false;}
    else return false;
    value=static_cast<uint32_t>(m_vif1EeField.load());
    auto *q=m_vif1Instant.get();const auto expected=value;const auto tick=gs_regs.vsyncTick.load();
    // TEMP(transformers): every consumed FIELD read has a worker-side mode-0 oracle.
    q->enqueue([q,expected,pc,tick]{
        q->csrOracleReads.fetch_add(1);
        if(q->oracleField!=expected) {q->csrOracleFailures.fetch_add(1);throw std::runtime_error("VU1 CSR FIELD oracle mismatch");}
        gsTimeline::traceCsr(pc,tick,expected);
    });
    return true;
}

// TEMP(transformers): INTC publication state is file-local, with the original FIFO mutex.
namespace {
std::mutex drainIntcMutex;
std::vector<uint32_t> drainIntcCauses;
std::atomic<bool> drainIntcPending{false};
bool drainEnv(const char *name) { const char *v=std::getenv(name); return v && std::atoi(v)==1; }
}
namespace ps2drain {
const bool fast=[] { const char *p=std::getenv("PS2X_DRAIN_FAST"); return !p || std::atoi(p)==1; }(); // TEMP(transformers): qualified shipping default.
const bool stats=drainEnv("PS2X_DRAIN_STATS");
thread_local Counts counts;
thread_local unsigned depth=0;
}
namespace ps2intc {
void queue(uint32_t cause) {
    std::lock_guard<std::mutex> lock(drainIntcMutex);
    drainIntcCauses.push_back(cause);
    if (ps2drain::fast) drainIntcPending.store(true, std::memory_order_release);
}
std::vector<uint32_t> consume() {
    if (ps2drain::fast && !drainIntcPending.load(std::memory_order_acquire)) return {};
    if (ps2drain::stats && ps2drain::depth) ++ps2drain::counts.mutexes;
    std::lock_guard<std::mutex> lock(drainIntcMutex);
    std::vector<uint32_t> out;
    out.swap(drainIntcCauses);
    if (ps2drain::fast) drainIntcPending.store(false, std::memory_order_release);
    return out;
}
}
