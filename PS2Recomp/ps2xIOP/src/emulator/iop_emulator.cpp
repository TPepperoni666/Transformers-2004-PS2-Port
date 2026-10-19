// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <sstream>
#include <utility>
#include <vector>


// TEMP(transformers): counts only; no clocks or profiling. Producer/EE thread only.
extern "C" { extern const bool iopIdleStatsEnabled = [] { const char *p=std::getenv("PS2X_IOP_STATS"); return p && std::strcmp(p,"1")==0; }(); }
namespace iopIdle_stats {
const bool &enabled = iopIdleStatsEnabled;
const bool horizon = [] { const char *p=std::getenv("PS2X_IOP_HORIZON"); return p && std::strcmp(p,"1")==0; }();
uint64_t vs=0, advance=0, calls=0, ready=0, dma=0, callback=0, timer=0, sample=0, idle=0, walks=0, skipped=0;
}
extern "C" void iopIdleStatsAdvance(uint64_t vs) noexcept {
    using namespace iopIdle_stats;
    if (!enabled) return;
    if (vs != iopIdle_stats::vs) {
        std::fprintf(stderr,"[iopIdle:stats] vs=%llu advance=%llu calls=%llu ready=%llu dma_due=%llu callback_due=%llu timer_due=%llu sample_due=%llu idle=%llu timer_walks=%llu skipped=%llu\n",
          (unsigned long long)iopIdle_stats::vs,(unsigned long long)advance,(unsigned long long)calls,(unsigned long long)ready,(unsigned long long)dma,(unsigned long long)callback,(unsigned long long)timer,(unsigned long long)sample,(unsigned long long)idle,(unsigned long long)walks,(unsigned long long)skipped);
        iopIdle_stats::vs=vs;advance=calls=ready=dma=callback=timer=sample=idle=walks=skipped=0;
    }
    ++advance;
}
extern "C" void iopIdleStatsTimerWalk() noexcept { if (iopIdle_stats::enabled) ++iopIdle_stats::walks; }

// TEMP(transformers): flush final bucket at orderly process shutdown.
namespace { struct StatsFinal { ~StatsFinal() { if (iopIdleStatsEnabled) iopIdleStatsAdvance(iopIdle_stats::vs+1); } } statsFinal; }

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr uint32_t kLoaderStackSize = 0x4000u;
        constexpr uint32_t kLoaderPriority = 0x20u;         // modload runs module starts at a middling priority
        constexpr uint64_t kLoaderCycleCap = 36864000u;     // 1s of IOP time (36.864MHz)
        constexpr uint64_t kLoaderSlice = 36864u;           // 1ms
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch + ps2recomp-memcard.patch (GPL-3.0)
        constexpr uint64_t kMaxOutsideWaitCycles = 36864000u; // outside-thread WaitSema/WaitEventFlag cap: ~1 s of IOP time
        constexpr uint64_t kSifDmaCompletionCycles = 2000u;   // sceSifSetDmaIntr: func(data) ~54 us after the copy

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        // fmvaudio (2026-10-09): EE->IOP SetDma "landing buffer" write-after-write hazard.
        // The SIRENS3 movie/music streamer receives every 8 KB EE chunk in ONE IOP landing buffer; the EE notifies the
        // IOP (RPC cmd) after its DMA completed and the IOP stream thread copies the landing buffer into its ring on its
        // next 5 ms poll. Our guest EE time between "notify" and the next chunk's SetDma is far shorter than on hardware
        // (HLE MPEG decode / VU1 work cost no EE cycles), so the next SetDma overwrites the landing buffer before the IOP
        // copied it: one chunk is lost and the next is copied twice (audible 43 ms skip + 43 ms repeat, every few
        // chunks, in every movie). Fix: for a landing buffer the IOP has been seen to memcpy out in full, an EE SetDma
        // that would overwrite a not-yet-consumed chunk first saves that chunk; the IOP's next memcpy from the landing
        // buffer is served from the saved chunks in order. Guest control flow and timing are unchanged (same copies,
        // same notifications); only the copied bytes are the ones the EE actually sent. PS2X_SIF_LANDING_FIFO=0 disables.
        class SifLandingFifo
        {
        public:
            static bool enabled()
            {
                static const bool s_on = [] { const char *v = std::getenv("PS2X_SIF_LANDING_FIFO"); return !(v && v[0] == '0'); }();
                return s_on;
            }
            // EE SetDma about to write [dest, dest+size): returns true if the current contents must be saved first.
            bool beforeEeWrite(uint32_t dest, uint32_t size) const
            {
                const auto it = m_slots.find(dest);
                return it != m_slots.end() && it->second.size == size && it->second.learned && it->second.pending;
            }
            void save(uint32_t dest, std::vector<uint8_t> bytes)
            {
                Slot &slot = m_slots[dest];
                if (slot.saved.size() >= kMaxDepth)
                {
                    slot.saved.erase(slot.saved.begin()); // never grows without bound; oldest unconsumed chunk is lost
                    ++m_overflow;
                }
                slot.saved.push_back(std::move(bytes));
                ++m_saves;
            }
            void afterEeWrite(uint32_t dest, uint32_t size)
            {
                Slot &slot = m_slots[dest];
                if (slot.size != size)
                {
                    slot = Slot{};
                    slot.size = size;
                }
                slot.pending = true;
            }
            // IOP memcpy(dst, src, n). Returns the saved chunk to deliver instead of RAM, if any.
            std::optional<std::vector<uint8_t>> onIopCopy(uint32_t src, uint32_t n)
            {
                const auto it = m_slots.find(src);
                if (it == m_slots.end() || it->second.size != n)
                    return std::nullopt;
                Slot &slot = it->second;
                slot.learned = true;
                if (!slot.saved.empty())
                {
                    std::vector<uint8_t> out = std::move(slot.saved.front());
                    slot.saved.erase(slot.saved.begin());
                    ++m_served;
                    return out; // RAM still holds a newer, unconsumed chunk: stays pending
                }
                slot.pending = false;
                return std::nullopt;
            }
            uint64_t saves() const noexcept { return m_saves; }
            uint64_t served() const noexcept { return m_served; }
            uint64_t overflow() const noexcept { return m_overflow; }

        private:
            static constexpr size_t kMaxDepth = 8u;
            static constexpr uint32_t kMinSize = 0x800u;
            struct Slot
            {
                uint32_t size = 0u;
                bool learned = false; // the IOP copies this exact buffer out in full (a landing buffer)
                bool pending = false; // RAM holds an EE chunk the IOP has not copied out yet
                std::vector<std::vector<uint8_t>> saved;
            };
            std::map<uint32_t, Slot> m_slots;
            uint64_t m_saves = 0u, m_served = 0u, m_overflow = 0u;

        public:
            static bool candidateSize(uint32_t size) noexcept { return size >= kMinSize && size <= 0x40000u; }
        };

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            uint32_t textSize = 0;
            uint32_t dataSize = 0;
            uint32_t bssSize = 0;
            uint16_t version = 0;
            std::string iopmodName; // module name from .iopmod (ReferModuleStatus.name)
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch (GPL-3.0)
            kernel.setOutsideThreadWait([this](const std::function<bool()> &ready) { return waitOutsideThread(ready); });
        }

        void reset()
        {
            totalCycles = 0; // Reset the audio clock before resetting its state.
            memory.spu2().setClock(&totalCycles); // trace time stamps (iop_spu2.h)
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingAdmaMask = 0;
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            waitingOutsideThread = false;
            settlingThreads = false;
            waitedInstructions = 0u;
            sifCommandDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            // StopTrans disables ADMAS and/or clears CHCR. Cancel the old completion before rearming.
            for (int core = 0; core < 2; ++core) {
                if ((pendingAdmaMask & (1u << core)) &&
                    (!memory.spu2().admaEnabled(core) ||
                     !(memory.read32(core ? 0x1F801508u : 0x1F8010C8u) & 0x01000000u))) {
                    pendingDmaInterrupts.erase(core ? 40 : 36);
                    pendingAdmaMask &= ~(1u << core);
                    memory.spu2().cancelAdma(core);
                }
            }
            // Keep StopTrans/ADMA cancellation above even when no new DMA starts.
            if (!memory.hasDmaStart())
                return;
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                const int core = dma->irq == 40 ? 1 : 0;
                if (memory.spu2().admaEnabled(core)) pendingAdmaMask |= 1u << core;
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        bool traceModuleStart = false; // TEMP(transformers): trace imports during IRX _start
        uint32_t traceLines = 0u;
        static bool quietIo() // TEMP(transformers) qualification: PS2X_QUIET_IO=1 drops the per-import trace (CD/SIF) lines
        {
            static const bool s_quiet = []() { const char *e = std::getenv("PS2X_QUIET_IO"); return e && *e && *e != '0'; }();
            return s_quiet;
        }
        static bool traceWorthy(const IopImportCall &c) // TEMP: CD/SIF/printf only
        {
            // TEMP(transformers) fmvaudio: PS2X_IOP_TRACE_LIBS=thsemap,thbase,... adds libraries to the import trace.
            static const std::string s_extra = []() {
                const char *v = std::getenv("PS2X_IOP_TRACE_LIBS");
                return v ? std::string(",") + v + "," : std::string();
            }();
            if (!s_extra.empty())
            {
                std::string key = "," + std::string(c.library) + ",";
                for (char &ch : key)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (s_extra.find(key) != std::string::npos)
                    return true;
            }
            return iequals(c.library, "cdvdman") || iequals(c.library, "sifcmd") || iequals(c.library, "sifman") ||
                   (iequals(c.library, "sysmem") && c.ordinal == 14u);
        }
        static uint32_t traceMaxLines() // TEMP(transformers) fmvaudio: PS2X_IOP_TRACE_MAX (default 30000)
        {
            static const uint32_t s_max = []() {
                const char *v = std::getenv("PS2X_IOP_TRACE_MAX");
                return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 30000u;
            }();
            return s_max;
        }

        // PS2X_SD_TRACE=1: log every libsd call SIRENS3 (or any IRX) makes into the real LIBSD.IRX, with arguments.
        // Ordinal -> name for LIBSD 1.05 (verified against the export table of the game's LIBSD.IRX and the
        // wrapper functions in SIRENS3; see private qualification evidence).  Behaviour-neutral: it only logs.
        void traceLibSd(uint16_t ordinal, const CpuState &cpu)
        {
            static const bool enabled = std::getenv("PS2X_SD_TRACE") != nullptr;
            if (!enabled)
                return;
            static uint32_t lines = 0u;
            if (++lines > 400000u)
                return;
            static const char *const names[] = {
                "?0", "?1", "?2", "?3", "sceSdInit", "sceSdSetParam", "sceSdGetParam", "sceSdSetSwitch",
                "sceSdGetSwitch", "sceSdSetAddr", "sceSdGetAddr", "sceSdSetCoreAttr", "sceSdGetCoreAttr",
                "sceSdNote2Pitch", "sceSdPitch2Note", "sceSdProcBatch", "sceSdProcBatchEx", "sceSdVoiceTrans",
                "sceSdBlockTrans", "sceSdVoiceTransStatus", "sceSdBlockTransStatus", "sceSdSetTransCallback",
                "sceSdSetIRQCallback", "sceSdSetEffectAttr", "sceSdGetEffectAttr", "sceSdClearEffectWorkArea",
                "sceSdSetTransIntrHandler", "sceSdSetSpu2IntrHandler", "sceSdGetTransIntrHandlerArgument",
                "sceSdGetSpu2IntrHandlerArgument", "sceSdStopTrans", "sceSdCleanEffectWorkArea", "libsd:32",
                "libsd:33"};
            const char *name = ordinal < sizeof(names) / sizeof(names[0]) ? names[ordinal] : "libsd:?";
            std::ostringstream out;
            out << "[sd] t=" << totalCycles << ' ' << name << std::hex << "(0x" << cpu.gpr[4] << ",0x" << cpu.gpr[5]
                << ",0x" << cpu.gpr[6] << ",0x" << cpu.gpr[7] << ") ra=0x" << cpu.gpr[31];
            log(LogLevel::Info, out.str());
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };
            const auto missing = [&](bool handled) -> ImportDisposition
            {
                if (handled)
                    return ImportDisposition::Handled;
                // HLE libraries used to return Missing silently with v0 left stale; make it loud (once per import).
                static std::set<uint32_t> reported;
                const uint32_t key = (static_cast<uint32_t>(std::hash<std::string>{}(call.library)) << 8u) ^ call.ordinal;
                if (reported.insert(key).second)
                {
                    std::ostringstream warn;
                    warn << "[IOP] unimplemented HLE import " << call.library << ':' << call.ordinal << " ra=0x" << std::hex << cpu.gpr[31];
                    log(LogLevel::Warning, warn.str());
                }
                setV0(0);
                return ImportDisposition::Missing;
            };
            if (traceModuleStart && !quietIo() && traceWorthy(call) && ++traceLines < traceMaxLines())
            {
                std::ostringstream out;
                out << "[IOP trace] " << call.library << ':' << call.ordinal << " ra=0x" << std::hex << cpu.gpr[31]
                    << " a0=0x" << a0 << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7] << " sp16=0x" << memory.read32(cpu.gpr[29] + 16u) << " sp24=0x" << memory.read32(cpu.gpr[29] + 24u) << " sp28=0x" << memory.read32(cpu.gpr[29] + 28u);
                log(LogLevel::Info, out.str());
            }

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return missing(kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles));
            }
            if (iequals(call.library, "thsemap"))
            {
                return missing(kernel.dispatchSemaphoreImport(call.ordinal, cpu));
            }
            if (iequals(call.library, "thevent"))
            {
                return missing(kernel.dispatchEventImport(call.ordinal, cpu));
            }
            if (iequals(call.library, "sifcmd"))
            {
                const bool handled = rpc.dispatchSifCmdImport(call.ordinal, cpu);
                if (const auto completion = rpc.takeCompletionCallback())
                {
                    // sceSifSendCmdIntr: the "DMA done" callback must run before the EE can react to the
                    // command (on hardware the data DMA completes before the packet is seen). Delaying it
                    // let the EE send its next request first, and TwoFish then replied from a freed buffer.
                    pendingGuestCallbacks.emplace(
                        totalCycles,
                        ScheduledGuestCallback{completion->function, completion->gp, completion->data});
                }
                return missing(handled);
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && (call.ordinal == 17u || call.ordinal == 23u))
            {
                constexpr uint32_t kKeUnknownModule = 0xFFFFFE70u; // -400
                if (call.ordinal == 23u) // SearchModuleByAddress(const void *addr) -> modid
                {
                    const uint32_t physical = IopMemory::physicalAddress(a0);
                    for (const auto &[id, module] : modules)
                    {
                        if (physical >= module.base && physical < module.base + module.size)
                        {
                            setV0(static_cast<uint32_t>(id));
                            return ImportDisposition::Handled;
                        }
                    }
                    setV0(kKeUnknownModule);
                    return ImportDisposition::Handled;
                }
                // ReferModuleStatus(int modid, ModuleStatus *status); ModuleStatus is 96 bytes:
                // char name[56]; u16 version, flags; int id; u32 entry, gp, text_addr, text_size, data_size, bss_size; u32 resv[2]
                const auto found = modules.find(static_cast<int>(a0));
                const uint32_t out = cpu.gpr[5];
                if (found == modules.end() || out == 0u)
                {
                    setV0(kKeUnknownModule);
                    return ImportDisposition::Handled;
                }
                const Module &module = found->second;
                for (uint32_t i = 0; i < 96u; i += 4u)
                    memory.write32(out + i, 0u);
                const std::string &name = module.iopmodName.empty() ? module.name : module.iopmodName;
                for (size_t i = 0; i < name.size() && i < 55u; ++i)
                    memory.write8(out + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
                memory.write16(out + 56u, module.version);
                memory.write16(out + 58u, 0u);
                memory.write32(out + 60u, static_cast<uint32_t>(module.id));
                memory.write32(out + 64u, module.entry);
                memory.write32(out + 68u, module.gp);
                memory.write32(out + 72u, module.base); // text_addr = image load address
                memory.write32(out + 76u, module.textSize);
                memory.write32(out + 80u, module.dataSize);
                memory.write32(out + 84u, module.bssSize);
                setV0(0u);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                const bool handled = rpc.dispatchSifManImport(call.ordinal, cpu);
                // Ported from KIexster/god-of-war-recomp patches/ps2recomp-memcard.patch (GPL-3.0)
                // sceSifSetDmaIntr: run the caller's completion callback func(data) as the DMA-end interrupt would,
                // a little after the (already finished) copy so the caller can first record that a DMA is in flight.
                if (const auto completion = rpc.takeDmaCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kSifDmaCompletionCycles,
                        ScheduledGuestCallback{completion->function, completion->gp, completion->data});
                }
                return missing(handled);
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                if (call.ordinal == 12u && SifLandingFifo::enabled() && SifLandingFifo::candidateSize(cpu.gpr[6])) // memcpy
                {
                    if (auto saved = sifLanding.onIopCopy(physicalAddress(cpu.gpr[5]), cpu.gpr[6]))
                    {
                        (void)memory.writeRam(cpu.gpr[4], saved->data(), saved->size());
                        if ((sifLanding.served() & 63u) == 1u) // TEMP(transformers) fmvaudio stats
                        {
                            std::ostringstream out;
                            out << "[sif-landing] served=" << sifLanding.served() << " saved=" << sifLanding.saves()
                                << " overflow=" << sifLanding.overflow() << " landing=0x" << std::hex << physicalAddress(cpu.gpr[5]);
                            log(LogLevel::Info, out.str());
                        }
                        setV0(cpu.gpr[4]);
                        return ImportDisposition::Handled;
                    }
                }
                return missing(sysclib.dispatchImport(call.ordinal, cpu));
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "libsd"))
                traceLibSd(call.ordinal, cpu);

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            // No HLE file descriptors exist yet. Preserve guest ioman exports above,
            // but fail unsupported file operations instead of inventing fd 0/success.
            if (iequals(call.library, "ioman") && call.ordinal >= 4u && call.ordinal <= 8u)
            {
                setV0(static_cast<uint32_t>(call.ordinal == 4u ? -19 : -9)); // -ENODEV / -EBADF
                return ImportDisposition::Handled;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // Import decoding starts with the same jr ra test; avoid the out-of-line lookup for other opcodes.
            const auto import = memory.fetch32(cpu.pc) == 0x03E00008u ? imports.decode(cpu.pc) : std::nullopt;
            if (import)
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                if (traceModuleStart && !quietIo() && traceWorthy(*import) && traceLines < traceMaxLines())
                {
                    std::ostringstream out;
                    out << "[IOP trace]   -> v0=0x" << std::hex << cpu.gpr[2] << " cycle=" << std::dec << totalCycles;
                    log(LogLevel::Info, out.str());
                }
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            // Instructions other threads executed while this call waited outside a thread (waitOutsideThread) are
            // not this call's work: do not let a long legitimate wait use up its budget and abandon it mid-function.
            const uint64_t waitedStart = waitedInstructions;
            const auto executed = [&]() -> uint64_t
            {
                const uint64_t total = totalInstructions - start;
                const uint64_t waited = waitedInstructions - waitedStart;
                return waited < total ? total - waited : 0u;
            };
            while (!cpu.stopped && !cpu.yielded && executed() < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                {
                    memory.spu2().advanceTo(totalCycles);
                    servicePendingDmaInterrupts();
                }
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(executed());
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            // TEMP(transformers): an unfinished RPC was previously reported as a completed call.
            // Record every abandoned context, even after the ordinary call trace reaches its cap.
            if (!cpu.stopped)
            {
                std::ostringstream out;
                out << "[IOP call:unfinished] entry=0x" << std::hex << address
                    << " a0=0x" << a0 << " a1=0x" << a1 << " a2=0x" << a2
                    << " pc=0x" << cpu.pc << " ra=0x" << cpu.gpr[31]
                    << " sp=0x" << cpu.gpr[29] << " t9=0x" << cpu.gpr[25]
                    << " s3=0x" << cpu.gpr[19] << " budget=" << std::dec << budget
                    << " yielded=" << cpu.yielded;
                log(LogLevel::Warning, out.str());
            }
            // TEMP(transformers): this fired on every IOP callback (~200k lines per run, slowing tests); keep the first
            // 2000 unless PS2X_IOP_TRACE=1.
            static const bool s_iopTraceAll = std::getenv("PS2X_IOP_TRACE") != nullptr;
            static uint32_t s_callFunctionTraceLines = 0u;
            if (traceModuleStart && depth == 0u && (s_iopTraceAll || ++s_callFunctionTraceLines <= 2000u))
            {
                std::ostringstream out;
                out << "[IOP trace] callFunction 0x" << std::hex << address << " ended pc=0x" << cpu.pc
                    << " ra=0x" << cpu.gpr[31] << " stopped=" << cpu.stopped << " yielded=" << cpu.yielded;
                log(LogLevel::Info, out.str());
            }
            return cpu.gpr[2];
        }

        bool inInterruptContext() const override
        {
            return servicingDmaInterrupts || servicingGuestCallbacks || timerHandlerDepth > 0u;
        }

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-iop-sound.patch (GPL-3.0), with extra guards.
        // IopKernel calls this for WaitSema/WaitEventFlag when there is no current thread, i.e. an RPC server function
        // the EE called directly. On hardware that function runs on the RPC thread, which would block while the other
        // threads, interrupts and callbacks run, so do exactly that until `ready()` holds or ~1 s of IOP time passes.
        // Refused (the wait fails at once, v0 = KE_SEMA_ZERO / KE_EVF_COND) in interrupt context - DMA/timer handlers,
        // scheduled callbacks, EE->IOP SIF command handlers (hardware: KE_ILLEGAL_CONTEXT; and the interrupt servicing
        // flags would stop the very interrupt that could satisfy the wait) - and when nested in another such wait.
        // PS2X_IOP_OUTSIDE_WAIT_CYCLES=<IOP cycles> changes the cap; 0 disables the wait (old fail-fast behaviour,
        // but with v0 set instead of stale).
        bool waitOutsideThread(const std::function<bool()> &ready)
        {
            if (ready())
                return true;
            static const uint64_t s_cap = []() -> uint64_t
            {
                const char *e = std::getenv("PS2X_IOP_OUTSIDE_WAIT_CYCLES");
                return e ? static_cast<uint64_t>(std::strtoull(e, nullptr, 0)) : kMaxOutsideWaitCycles;
            }();
            if (s_cap == 0u || waitingOutsideThread || settlingThreads || inInterruptContext() || sifCommandDepth > 0u)
                return false;

            waitingOutsideThread = true;
            const uint64_t startCycles = totalCycles;
            const uint64_t startInstructions = totalInstructions;
            while (!ready() && totalCycles - startCycles < s_cap)
            {
                const uint64_t before = totalCycles;
                runCycles(kDefaultSlice);
                if (totalCycles == before)
                    break; // runCycles swallowed an exception without advancing: do not spin forever
            }
            waitingOutsideThread = false;

            const bool satisfied = ready();
            if (satisfied)
            {
                // Only a successful wait is not charged to the waiting call's instruction budget: a function that
                // keeps retrying a wait that times out still runs out of budget instead of hanging the EE.
                waitedInstructions += totalInstructions - startInstructions;
            }
            // TEMP(transformers): first 64 satisfied waits and first 64 timeouts, to see how often RPC code really waits.
            static uint32_t s_satisfiedLogs = 0u;
            static uint32_t s_timeoutLogs = 0u;
            uint32_t &logCount = satisfied ? s_satisfiedLogs : s_timeoutLogs;
            if (logCount < 64u)
            {
                ++logCount;
                std::ostringstream out;
                out << "[IOP] wait outside a thread " << (satisfied ? "satisfied" : "TIMED OUT") << " after " << std::dec
                    << (totalCycles - startCycles) << " IOP cycles (pc=0x" << std::hex
                    << (activeCpu ? activeCpu->pc : 0u) << " ra=0x" << (activeCpu ? activeCpu->gpr[31] : 0u) << ')';
                log(satisfied ? LogLevel::Info : LogLevel::Warning, out.str());
            }
            return satisfied;
        }

        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-runtime.patch (GPL-3.0) (runReadyThreads), narrowed
        // to threads that outrank the RPC thread (see IopRpcBridge::handleRpc). Only from the EE side, never while IOP
        // code is on the host stack, never nested.
        void runHigherPriorityThreads(uint32_t priority, uint64_t maxCycles) override
        {
            if (activeCpu != nullptr || settlingThreads || waitingOutsideThread)
                return;
            settlingThreads = true;
            const uint64_t start = totalCycles;
            while (kernel.hasReadyThreadAbove(priority) && totalCycles - start < maxCycles)
            {
                const uint64_t before = totalCycles;
                runCycles(kDefaultSlice); // small slices: a lower-priority thread gets at most the rest of one slice
                if (totalCycles == before)
                    break; // runCycles swallowed an exception without advancing: do not spin forever
            }
            settlingThreads = false;
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                memory.spu2().advanceTo(totalCycles);
                if (it->first == 36 || it->first == 40)
                {
                    memory.completeSpuDma(it->first == 40 ? 1 : 0);
                    pendingAdmaMask &= ~(1u << (it->first == 40 ? 1 : 0));
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    memory.spu2().advanceTo(totalCycles);
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    ++timerHandlerDepth;
                    timrman.serviceDue(totalCycles, *this); // (wrap in try/catch if serviceDue can throw)
                    --timerHandlerDepth;
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        memory.spu2().advanceTo(totalCycles);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        memory.spu2().advanceTo(totalCycles);
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    next->runCycles += runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                    memory.spu2().advanceTo(totalCycles);
                }
                if (iopIdle_stats::horizon) updateIdleFastPath();
            }
            catch (...)
            {
                idleUntil = 0;
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        void updateIdleFastPath() noexcept {
            idleUntil = 0;
            if (servicingDmaInterrupts || servicingGuestCallbacks || timerHandlerDepth || callDepth ||
                pendingAdmaMask || waitingOutsideThread || settlingThreads || sifCommandDepth || kernel.hasReadyThread()) return;
            uint64_t until = kernel.nextWakeCycle(UINT64_MAX);
            for (const auto &[irq, c] : pendingDmaInterrupts) until = std::min(until,c);
            if (!pendingGuestCallbacks.empty()) until = std::min(until,pendingGuestCallbacks.begin()->first);
            until = timrman.nextEventCycle(until);
            idleUntil = std::min(until,memory.spu2().nextSampleCycle());
            idleKernelVersion = kernel.version();
        }
        void recordStats(uint64_t target) noexcept {
            using namespace iopIdle_stats;
            if (!enabled) return;
            ++calls;
            // TEMP(transformers): exact prediction of the nonzero-cycle skip path, only with stats enabled.
            if (horizon && target > totalCycles && target < idleUntil && kernel.version()==idleKernelVersion) ++skipped;
            bool r=kernel.hasReadyThread(),d=false,c=!pendingGuestCallbacks.empty() && pendingGuestCallbacks.begin()->first<=target;
            for (const auto &[irq,cycle] : pendingDmaInterrupts) d |= cycle<=target;
            bool t=timrman.nextEventCycle(UINT64_MAX)<=target, a=memory.spu2().nextSampleCycle()<=target;
            ready+=r;dma+=d;callback+=c;timer+=t;sample+=a;
            idle+=!(r||d||c||t||a||kernel.nextWakeCycle(UINT64_MAX)<=target||pendingAdmaMask||callDepth||timerHandlerDepth||servicingDmaInterrupts||servicingGuestCallbacks);
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;
            module.textSize = loaded.textSize;
            module.dataSize = loaded.dataSize;
            module.bssSize = loaded.bssSize;
            module.version = loaded.version;
            module.iopmodName = loaded.name;
            // Register before _start runs: SIRENS3 calls SearchModuleByAddress/ReferModuleStatus on itself from
            // inside _start / its first RPC, before the load returns (module.resident is fixed up afterwards).
            modules[module.id] = module;
            log(LogLevel::Info, "[IOP] module id=" + std::to_string(module.id) + " iopmod name='" + module.iopmodName + "'");

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            traceModuleStart = true;
            const uint64_t traceInstrBefore = totalInstructions;
            uint32_t startResult = 0u;
            bool startFinished = true;
            const int loaderId = activeCpu == nullptr
                                     ? kernel.createLoaderThread(module.entry, argumentSize, args, module.gp,
                                                                 kLoaderStackSize, kLoaderPriority)
                                     : -1;
            if (loaderId > 0)
            {
                // Pump the IOP until _start returns, as the EE's load RPC would wait for it on hardware.
                // Sleeping on CD callbacks / EE replies works because this is a real thread.
                const uint64_t deadline = totalCycles + kLoaderCycleCap;
                const IopThread *loader = kernel.findThread(loaderId);
                while (loader && loader->state != IopThreadState::Dormant &&
                       loader->state != IopThreadState::Dead && totalCycles < deadline)
                {
                    runCycles(kLoaderSlice);
                    loader = kernel.findThread(loaderId);
                }
                startFinished = loader && loader->state == IopThreadState::Dormant;
                if (startFinished)
                {
                    startResult = loader->cpu.gpr[2];
                    kernel.deleteThread(loaderId);
                }
                else
                {
                    // Leave it running in the background and treat the module as resident.
                    log(LogLevel::Warning, "[IOP] module _start did not return within 1s of IOP time; continuing in background");
                }
            }
            else
            {
                // Nested load from IOP code (or no memory for a thread): run _start inline as before.
                startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            }
            traceModuleStart = true; // TEMP: keep tracing after _start
            {
                std::ostringstream out;
                out << "[IOP trace] _start ran " << (totalInstructions - traceInstrBefore) << " instructions, v0=0x"
                    << std::hex << startResult << (loaderId > 0 ? " (loader thread)" : " (inline)");
                log(LogLevel::Info, out.str());
            }
            if (args && startFinished)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            // Real callers pass an IOP address (image already DMA'd to IOP RAM); keep the EE read as a fallback.
            if (!IopModuleLoader::readElfFromIop(memory, guestAddress, image) &&
                !IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        SifLandingFifo sifLanding; // fmvaudio: EE->IOP landing-buffer hazard (see SifLandingFifo)
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint32_t pendingAdmaMask = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        uint64_t idleUntil = 0, idleKernelVersion = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        uint32_t timerHandlerDepth = 0u; // >0 while timrman.serviceDue runs hard-timer handlers
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        bool waitingOutsideThread = false; // waitOutsideThread is pumping the IOP for an RPC server function
        bool settlingThreads = false;      // runHigherPriorityThreads is pumping the IOP after an RPC
        uint64_t waitedInstructions = 0u;  // instructions run by other threads during satisfied outside-thread waits
        uint32_t sifCommandDepth = 0u;     // >0 while an EE -> IOP SIF command handler runs (interrupt context on hw)
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->idleUntil = 0;
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        m_impl->idleUntil = 0;
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        const uint64_t target=m_impl->totalCycles+iopCycles;
        if (iopIdleStatsEnabled) m_impl->recordStats(target); // TEMP(transformers): includes zero-cycle calls.
        if (!iopCycles) return;
        if (iopIdle_stats::horizon && target >= m_impl->totalCycles && target < m_impl->idleUntil && m_impl->kernel.version()==m_impl->idleKernelVersion) {
            m_impl->totalCycles=target;
            // TEMP(transformers): skipped accounting is in the single guarded stats block.
            return;
        }
        m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        m_impl->idleUntil = 0;
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    bool IopEmulator::deliverSifCommand(uint32_t cid, const void *packet, uint32_t packetSize,
                                        uint32_t extraDestination, const void *extra, uint32_t extraSize)
    {
        m_impl->idleUntil = 0;
        // SIF command handlers run in the SIF interrupt on hardware: they must not block (waitOutsideThread refuses).
        struct DepthGuard
        {
            uint32_t &depth;
            explicit DepthGuard(uint32_t &d) : depth(d) { ++depth; }
            ~DepthGuard() { --depth; }
        } guard(m_impl->sifCommandDepth);
        return m_impl->rpc.deliverCommand(cid, packet, packetSize, extraDestination, extra, extraSize, *m_impl);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->idleUntil = 0;
        m_impl->rpc.onSifTransfer(transfer);
        if (transfer.kind == SifTransferKind::SetDma && SifLandingFifo::enabled() &&
            SifLandingFifo::candidateSize(transfer.size))
        {
            const uint32_t dest = physicalAddress(transfer.destinationAddress);
            SifLandingFifo &fifo = m_impl->sifLanding;
            if (transfer.phase == SifTransferPhase::BeforeCopy)
            {
                if (fifo.beforeEeWrite(dest, transfer.size))
                {
                    std::vector<uint8_t> bytes(transfer.size);
                    if (m_impl->memory.readRam(dest, bytes.data(), bytes.size()))
                        fifo.save(dest, std::move(bytes));
                }
            }
            else
            {
                fifo.afterEeWrite(dest, transfer.size);
            }
        }
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.allocate(size, alignment);
    }

    uint32_t IopEmulator::allocateMemoryLast(uint32_t size, uint32_t alignment)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.allocateLast(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    void IopEmulator::finishAudioCapture() { m_impl->memory.spu2().finishAudioCapture(); } // TEMP(transformers)

    size_t IopEmulator::drainSamples(int16_t* stereo, size_t frames) noexcept
    {
        return m_impl->memory.spu2().drainSamples(stereo, frames);
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
