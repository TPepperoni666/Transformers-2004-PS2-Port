// Third-party adaptation notice: GPL-3.0; unwind tracking release fix adapted from PS2Recomp PR #270 7c74436acd4c and related forks credited in CREDITS.md.
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#include "ps2_present_trace.h"
extern "C" { extern void (*ps2xPresentTraceHook)(int,int); } // TEMP(transformers) // TEMP(transformers)
extern "C" const bool iopIdleStatsEnabled; // TEMP(transformers): cached process flag.
// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
extern "C" void iopIdleStatsAdvance(unsigned long long) noexcept;
#include "ps2_drain.h"
#include "runtime/ps2_eeopt.h"
// checkpoint fast path adapted from brad-richardson/PS2Recomp (GPL-3.0); budget design after snuri00/ps2-web-recomp (MIT)
#include "runtime/ps2_settings.h"
#include "runtime/ps2_pc_live.h"
#include "ps2_eeqos.h" // TEMP(transformers)
#include "ps2_runtime.h"
#include "ps2_vu1new.h" // TEMP(transformers)
#include "ps2_intc_pending.h"
#include "ps2_profiler.h" // TEMP(transformers)
#include "debug_counters.h" // TEMP(transformers)
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_ee_hle.h"
#include "ps2_remap_getter.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "Kernel/TestScript.h" // TEMP(transformers): PS2X_SCRIPT test harness
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2_iop_transport.h"
#include "ps2_host_audio.h"
#include "ps2_perfscope.h"
#include "gs/pdiet_pixels.h" // TEMP(transformers): perfscope
#include "Kernel/ps2_kernel_heap.h" // TEMP(transformers) lane hang1: PS2X_KERNEL_HEAP
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>

static uint32_t ps2Vu1CycleBudget(); // defined near PS2Runtime::Store32
void ps2PadSampleHostInput();       // ps2_pad.cpp, TEMP(transformers): latch host input between game pad polls
void ps2PadStopHostRumble();        // ps2_pad.cpp, TEMP(transformers): playtest rumble off at exit
#include "rlgl.h" // TEMP(transformers): own-framebuffer capture flush
#include "ps2_playtest.h"           // TEMP(transformers): F12 problem marks, F11 fps overlay
#include "ps2_crash_reporter.h"     // crash report + minidump (generic, ported from SSX3)
#include "ps2_sprint_probe.h"       // TEMP(transformers): qualification - PS2X_SPRINT_LOG=1 robot walk/run telemetry
#include <sstream>

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
static void ps2WidescreenUpdateWindow();
static float ps2WidescreenAspect(); // PS2X_WIDESCREEN, defined before TfWidescreen
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    // TEMP(transformers) qualification: PS2X_HEAPSCOPE=1 -> "[heapscope]" lines on stderr every PS2X_HEAPSCOPE_VS guest vsyncs
    // (default 600). Reads the game's own TDG heaps (SLUS_206.68 globals: heap table 0x6D1FD0, heap count 0x6CE4AC, main
    // heap pointer 0x6CE4C4; heap +8 = free bytes, heap +4 = root of the free-block tree whose +0x10/+0x14/+0x18 give the
    // largest free block, see 0x104700) and the runtime's HLE malloc arena (malloc/free/_malloc_r are runtime stubs).
    // Read-only, render thread, racy reads of aligned words: guest timeline untouched.
    uint32_t heapscopeIntervalVs()
    {
        static const uint32_t interval = []
        {
            const char *on = std::getenv("PS2X_HEAPSCOPE");
            if (!on || !on[0] || on[0] == '0')
            {
                return 0u;
            }
            const char *n = std::getenv("PS2X_HEAPSCOPE_VS");
            const uint32_t v = n ? static_cast<uint32_t>(std::strtoul(n, nullptr, 10)) : 600u;
            return v ? v : 600u;
        }();
        return interval;
    }

    uint32_t heapscopeRead32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0u;
        addr &= 0x1FFFFFFFu;
        if (rdram && addr <= PS2_RAM_SIZE - 4u)
        {
            std::memcpy(&v, rdram + addr, sizeof(v));
        }
        return v;
    }

    bool heapscopeTdgHeap(const uint8_t *rdram, uint32_t heap, uint32_t &freeBytes, uint32_t &largest)
    {
        freeBytes = 0u;
        largest = 0u;
        if (!rdram || heap == 0u || (heap & 0x1FFFFFFFu) >= PS2_RAM_SIZE - 0x20u || (heap >> 28) == 0x7u) // 0x70xxxxxx = scratchpad heap
        {
            return false;
        }
        freeBytes = heapscopeRead32(rdram, heap + 8u);
        const uint32_t root = heapscopeRead32(rdram, heap + 4u);
        if (root != 0u && (root & 0x1FFFFFFFu) < PS2_RAM_SIZE - 0x20u)
        {
            largest = std::max(heapscopeRead32(rdram, root + 0x10u),
                               std::max(heapscopeRead32(rdram, root + 0x14u), heapscopeRead32(rdram, root + 0x18u)));
        }
        return true;
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// TEMP(transformers): PS2X_SCRIPT step / verdict screenshots queued by Kernel/TestScript.cpp (render thread).
// Presentation upscale + sharpen (ps2_present_upscale.cpp): PS2X_UPSCALE=fsr1|cas|off, PS2X_SHARPNESS=0..1.
void ps2PresentUpscaleDraw(const Texture2D &tex, Rectangle src, Rectangle dst);
void ps2PresentUpscaleNoteDump(const char *path);                  // TEMP(transformers): PS2X_UPSCALE_CAPTURE
void ps2PresentUpscaleCapture(const Texture2D &tex, Rectangle src); // TEMP(transformers): PS2X_UPSCALE_CAPTURE
void ps2PresentUpscaleShutdown();
void ps2ResolutionPresent(const Texture2D&,Rectangle,Rectangle);
void ps2ResolutionPresentShutdown();
float ps2PresentDisplayAspect(); // PS2X_PRESENT_ASPECT: 4:3 by default, 0 = square pixels
float ps2PresentFrameAspect(float frameWidth); // this frame's display aspect: PS2X_WIDESCREEN (FMV frames 4:3) else ps2PresentDisplayAspect

static void ServiceTestScriptDump(const std::vector<uint8_t> &buffer, uint32_t height)
{
    char path[512];
    if (height == 0u || !ps2_test::takeDumpRequest(path, sizeof(path)))
    {
        return;
    }
    Image img{};
    img.data = const_cast<uint8_t *>(buffer.data());
    img.width = static_cast<int>(FB_WIDTH);
    img.height = static_cast<int>(std::min<uint32_t>(height, FB_HEIGHT));
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    ExportImage(img, path);
    ps2PresentUpscaleNoteDump(path); // TEMP(transformers): PS2X_UPSCALE_CAPTURE
}

// TEMP(transformers): PORT_LIST rank 3 - presentation latch on the EE executor at guest VBlankStart
// (gs/gs_frontend.cpp; hook in Kernel/EeScheduler.cpp processEvent(VBlankStart)). PS2X_PRESENT_LATCH=0 = legacy.
int ps2PresentLatchMode();
bool ps2PresentLatchConsume(GS &gs, uint64_t &ioSequence, uint64_t &outVsyncTick);
// TEMP(transformers) qualification I1a: PS2X_FIELD60 field-split presentation (gs/gs_pgs_backend.cpp, gs/gs_frontend.cpp).
bool ps2Field60Enabled();
void ps2Field60LastPresented(uint64_t &seq, uint64_t &tick, int &kind);
bool ps2PresentField60Poll(GS &gs);

// TEMP(transformers): sharp presentation copy; native marks/script dumps retain their original raster.
bool ps2PgsSharpTake(std::vector<uint8_t>&,uint32_t&,uint32_t&);
bool ps2PgsSetSkipNative(bool);
// TEMP(transformers): no native consumer can run in this process configuration. Script dump requests are
// enabled by PS2X_SCRIPT at startup; dump envs are immutable for the life of UploadFrame's static settings.
static bool pdietCanSkipNative() {
    static const bool noReaders=[] {
        for(const char *key:{"PS2X_SCRIPT","PS2X_DUMP_FRAMES","PS2X_FIELD60_DUMP"}) {
            const char *v=std::getenv(key);if(v && *v)return false;
        }return true;
    }();return pdiet::lazy() && noReaders;
}
float ps2PgsNativeFrameWidth(float);
static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static uint32_t s_outputWidth=0,s_outputHeight=0;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);
    static const bool s_eeLatch = ps2PresentLatchMode() != 0; // TEMP(transformers): PORT_LIST rank 3
    static uint64_t s_eeLatchSequence = 0u;
    static const bool s_field60 = s_eeLatch && ps2Field60Enabled(); // TEMP(transformers) qualification I1a
    uint64_t f60Seq = 0u;
    int f60Kind = -1;

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    // presentTick = guest vsync the presented frame belongs to. EE latch: the tick at which the EE latched it
    // (frame dumps / VRAM dumps report that); legacy: the tick seen when this thread sampled the live state.
    uint64_t presentTick = currentTick;
    bool needsLatch = false;
    if (s_field60)
    {
        // TEMP(transformers) qualification I1a: poll the paced field queue every host frame (the newer field is due half a
        // tick after the older one, between guest VBlankStart publications).
        needsLatch = ps2PresentField60Poll(rt->gs());
        if (needsLatch)
            ps2Field60LastPresented(f60Seq, presentTick, f60Kind);
    }
    else if (s_eeLatch)
    {
        // Converts the snapshot the EE took at its last VBlankStart, if a new one was published; never samples live.
        needsLatch = ps2PresentLatchConsume(rt->gs(), s_eeLatchSequence, presentTick);
    }
    else
    {
        needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
        if (needsLatch)
        {
            rt->gs().latchHostPresentationFrame();
            s_lastPresentationTick = currentTick;
            s_hasLatchedInitialFrame = true;
        }
    }
    if (!needsLatch)
    {
        if (s_hasUploadedFrame)
        {
            outWidth = s_outputWidth ? s_outputWidth : FB_WIDTH;
            outHeight = s_outputHeight ? s_outputHeight : DEFAULT_DISPLAY_HEIGHT;
            ServiceTestScriptDump(s_uploadBuffer, s_lastHeight); // TEMP(transformers): last uploaded frame
        }
        // else: EE latch, no guest VBlankStart latched yet -> keep the blank start-up texture (no magenta flash).
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    // TEMP(transformers): scoped skip only around this one copy; external GS callers remain eager.
    struct NativeSkipGuard {bool old;NativeSkipGuard():old(ps2PgsSetSkipNative(pdietCanSkipNative())){} ~NativeSkipGuard(){ps2PgsSetSkipNative(old);}};
    bool copiedNative=false;
    { NativeSkipGuard guard;
      copiedNative=rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource);
    }
    if (!copiedNative)
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << presentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    if (displayFbp != s_lastDisplayFbp)
        ps2_playtest::noteFlip(); // TEMP(transformers): game-frame counter for the F11 overlay / [playtest] stats
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    const bool dietSkippedNative=pdietCanSkipNative() && s_scratch.empty(); // TEMP(transformers)
    if(!pdiet::rn()) std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    uint32_t dietCopiedRows=0; // TEMP(transformers)
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
            if(pdiet::rn()) std::memset(s_uploadBuffer.data()+dstOffset+copyRowBytes,0,dstRowBytes-copyRowBytes);
            ++dietCopiedRows;
        }
    }

    if(pdiet::rn() && !dietSkippedNative) std::memset(s_uploadBuffer.data()+size_t(dietCopiedRows)*FB_WIDTH*4,0,s_uploadBuffer.size()-size_t(dietCopiedRows)*FB_WIDTH*4); // TEMP(transformers)

    // PS2X_DUMP_FRAMES=N: every Nth presented frame -> frame_XXXXX.png in the working directory, plus pixel stats.
    {
        static const int s_dumpEvery = []()
        {
            const char *env = std::getenv("PS2X_DUMP_FRAMES");
            return env ? std::max(0, std::atoi(env)) : 0;
        }();
        static uint32_t s_presented = 0u;
        ps2dbg::presentedFrames.fetch_add(1u, std::memory_order_relaxed); // TEMP(transformers)
        ps2dbg::vsyncTick.store(currentTick, std::memory_order_relaxed);   // TEMP(transformers)
        { // TEMP(transformers): PS2X_VRAM_DUMP_VS=<vsync>[,<vsync>...] writes the 4 MB GS VRAM to vram_vs<tick>.bin once per value
            static std::vector<uint64_t> s_vramDumpAt = []()
            {
                std::vector<uint64_t> out;
                for (const char *p = std::getenv("PS2X_VRAM_DUMP_VS"); p && *p;)
                {
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    out.push_back(v);
                    p = (*end == ',') ? end + 1 : end;
                }
                return out;
            }();
            for (auto it = s_vramDumpAt.begin(); it != s_vramDumpAt.end(); ++it)
            {
                if (presentTick < *it)
                    continue;
                // EE latch (PORT_LIST rank 3): the snapshot already holds the VRAM this frame was converted from
                // (taken at guest VBlankStart presentTick); legacy: sample live VRAM now.
                if (!s_eeLatch)
                    rt->gs().refreshDisplaySnapshot();
                uint32_t vramSize = 0u;
                if (const uint8_t *vram = rt->gs().lockDisplaySnapshot(vramSize))
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "vram_vs%llu.bin", static_cast<unsigned long long>(presentTick));
                    std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(vram), vramSize);
                    std::cout << "[vram:dump] " << name << " size=" << vramSize << std::endl;
                }
                rt->gs().unlockDisplaySnapshot();
                s_vramDumpAt.erase(it);
                break;
            }
        }
        if (s_dumpEvery > 0 && (s_presented++ % static_cast<uint32_t>(s_dumpEvery)) == 0u && width != 0u && height != 0u)
        {
            const uint32_t w = std::min<uint32_t>(width, FB_WIDTH);
            const uint32_t h = std::min<uint32_t>(height, FB_HEIGHT);
            uint64_t sum[4] = {0, 0, 0, 0};
            uint64_t nonBlack = 0;
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t *row = s_uploadBuffer.data() + static_cast<size_t>(y) * FB_WIDTH * 4u;
                for (uint32_t x = 0; x < w; ++x)
                {
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[x * 4u + c];
                    nonBlack += (row[x * 4u] | row[x * 4u + 1] | row[x * 4u + 2]) != 0u;
                }
            }
            const double n = static_cast<double>(w) * h;
            std::cout << "[frame:dump] n=" << s_presented - 1u << " fbp=" << sourceFbp << " " << w << "x" << h
                      << " avgRGBA=" << sum[0] / n << "," << sum[1] / n << "," << sum[2] / n << "," << sum[3] / n
                      << " nonBlack=" << nonBlack << " vs=" << presentTick << std::endl;
            Image img{};
            img.data = s_uploadBuffer.data();
            img.width = static_cast<int>(FB_WIDTH);
            img.height = static_cast<int>(h);
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            // TEMP(transformers): PS2X_DUMP_DIR=<dir> writes frame dumps there (lets several test runs share disc/).
            static const std::string s_dumpDir = []()
            {
                const char *d = std::getenv("PS2X_DUMP_DIR");
                return std::string(d && *d ? d : "");
            }();
            char name[512];
            if (s_dumpDir.empty())
                std::snprintf(name, sizeof(name), "frame_%05u.png", s_presented - 1u);
            else
                std::snprintf(name, sizeof(name), "%s/frame_%05u.png", s_dumpDir.c_str(), s_presented - 1u);
            ExportImage(img, name);
        }
    }
    if (f60Kind >= 0 && width != 0u && height != 0u)
    {
        // TEMP(transformers) qualification I1a: PS2X_FIELD60_DUMP=<from>:<to> (guest vsync of the scanout) dumps every
        // presented field-60 frame as f60_<seq>_vs<tick>_<A|B|P|a|b>.png (A/B = older/newer field of the scanout split,
        // P = progressive, a/b = pre-composite fields) into PS2X_DUMP_DIR.
        static const std::pair<uint64_t, uint64_t> s_f60Dump = []()
        {
            const char *e = std::getenv("PS2X_FIELD60_DUMP");
            if (!e || !*e)
                return std::pair<uint64_t, uint64_t>{1, 0};
            char *n = nullptr;
            const uint64_t lo = std::strtoull(e, &n, 10);
            return std::pair<uint64_t, uint64_t>{lo, (n && *n == ':') ? std::strtoull(n + 1, nullptr, 10) : lo};
        }();
        if (presentTick >= s_f60Dump.first && presentTick <= s_f60Dump.second)
        {
            static const char kKind[] = "ABPab";
            const char *d = std::getenv("PS2X_DUMP_DIR");
            char name[512];
            std::snprintf(name, sizeof(name), "%s%sf60_%06llu_vs%05llu_%c.png", d ? d : "", (d && *d) ? "/" : "",
                          static_cast<unsigned long long>(f60Seq), static_cast<unsigned long long>(presentTick),
                          kKind[std::min(f60Kind, 4)]);
            Image img{};
            img.data = s_uploadBuffer.data();
            img.width = static_cast<int>(FB_WIDTH);
            img.height = static_cast<int>(std::min<uint32_t>(height, FB_HEIGHT));
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            ExportImage(img, name);
            // TEMP(transformers) qualification: with PS2X_UPSCALE_CAPTURE=<W>x<H> also render this field frame through the
            // presenter's upscale passes (f60_..._<W>x<H>_{nearest,bilinear,fsr1,cas}.png): the on-screen FSR1/RCAS pass
            // runs on every presented field frame (frameTex is updated per field, ps2PresentUpscaleDraw every host frame).
            ps2PresentUpscaleNoteDump(name);
        }
    }
    ServiceTestScriptDump(s_uploadBuffer, width != 0u ? height : 0u); // TEMP(transformers): PS2X_SCRIPT step dumps

    // TEMP(transformers): replace the host texture atomically after preparing native dump consumers.
    static std::vector<uint8_t> pooledSharp; // TEMP(transformers): second sharp buffer, render-thread owned
    std::vector<uint8_t> localSharp;
    auto &sharpPixels=pdiet::rn()?pooledSharp:localSharp;uint32_t sharpW=0,sharpH=0;
    const bool sharp=ps2PgsSharpTake(sharpPixels,sharpW,sharpH);
    const uint32_t textureW=sharp?sharpW:FB_WIDTH,textureH=sharp?sharpH:FB_HEIGHT;
    ps2PresentTrace::Scope traceTexture(ps2PresentTrace::TextureUpload); // TEMP(transformers)
    ps2_perfscope::Scope dietUpload(ps2_perfscope::RnTexUpload); // TEMP(transformers)
    if(tex.width!=int(textureW)||tex.height!=int(textureH)) {
        Image image{};image.data=sharp?sharpPixels.data():s_uploadBuffer.data();
        image.width=int(textureW);image.height=int(textureH);image.mipmaps=1;image.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D replacement=LoadTextureFromImage(image);
        if(replacement.id) { const auto old=tex;tex=replacement;UnloadTexture(old); }
        else {outWidth=s_outputWidth;outHeight=s_outputHeight;return;}
    } else UpdateTexture(tex,sharp?sharpPixels.data():s_uploadBuffer.data());
    outWidth = sharp?sharpW:width;
    outHeight = sharp?sharpH:height;
    s_outputWidth=outWidth;s_outputHeight=outHeight;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    ps2_ee_hle::RuntimeClaim hleClaim(*this);
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
    if (ps2_kheap::enabled()) // TEMP(transformers) qualification: invocation stacks below the ELF (ps2_kernel_heap.h)
    {
        m_asyncCallbackStackFloor = ps2_kheap::kLowInvokeStacksFloor;
        m_asyncCallbackStackTop = ps2_kheap::kLowInvokeStacksTop;
    }
    hleClaim.commit();
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        if(m_memory.m_vif1Instant)m_memory.m_vif1Instant->abandon();
        if (m_memory.m_vif1Stream && !m_memory.m_vif1Stream->ownsCurrentThread())
            m_memory.m_vif1Stream->abandon(); // EE has stopped: join before releasing memory; no off-owner GS publication.
        if(!m_memory.m_vif1Instant)m_memory.fenceVu1();
        m_vu1.setDependencyFence({});
        if (!m_memory.m_vif1Stream && !m_memory.m_vif1Instant) m_vu1.synchronize();
        m_memory.setVu1Fence({});
        m_gs.setVu1Fence({});
        m_gs.setVu1Poll({});
        m_gs.setVu1Ordered({});
        requestStop();
        ps2_ee_hle::uninstall(*this);
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
    ps2_ee_hle::releaseRuntime(*this);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    ps2_perfscope::Scope perf(ps2_perfscope::EeIopRpc); // TEMP(transformers) qualification
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    ps2_perfscope::Scope perf(ps2_perfscope::EeIopRpc); // TEMP(transformers) qualification
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    if (iopIdleStatsEnabled) iopIdleStatsAdvance(ps2dbg::vsyncTick.load(std::memory_order_relaxed)); // TEMP(transformers)
#if defined(PS2X_IOP_BATCH_PROFILING) // TEMP(transformers): observer builds only.
    if (ps2_perfscope::g_enabled) { // TEMP(transformers): skip sampling/TLS work with stats off.
        ps2_perfscope::SampledScope<4> perf(ps2_perfscope::EeIop);
        m_iopSubsystem->runEeCycles(eeCycles);
    } else {
        m_iopSubsystem->runEeCycles(eeCycles);
    }
#else
    m_iopSubsystem->runEeCycles(eeCycles);
#endif
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}


// TEMP(transformers): PS2X_VU1DUMP=<from>[-<to>[/<step>]][:<count>] dumps VU1 code/data/state before and after the first
// <count> (default 8) VU1 calls (MSCAL and MSCNT, in order) of every <step>-th presented frame in [from, to], as
// vu1dump_f<frame>_<seq>_{m,c}_{pre,post}.bin. Header {u32 startPC (MSCNT: 0x80000000|resume pc), top, itop,
// sizeof(VU1State)} + 16K micro memory + 16K data memory + VU1State. Replay with vu1_replay/replay.exe.
namespace
{
    std::string vu1DumpNameForCall(bool mscnt)
    {
        struct Spec
        {
            long long from = -1, to = -1, step = 1;
            int count = 8;
            int skip = 0; // TEMP(transformers): capture a small later invocation window without dumping its predecessors.
            bool vsync = false; // TEMP(transformers): allow hub captures on the scripted vsync timeline.
        };
        static const Spec spec = []()
        {
            Spec out;
            const char *env = std::getenv("PS2X_VU1DUMP_VS"); // TEMP(transformers): same grammar, guest vsync instead of presented frame.
            out.vsync = env != nullptr;
            if (const char *skip = std::getenv("PS2X_VU1DUMP_SKIP"))
                out.skip = std::max(0, std::atoi(skip));
            if (!env)
                env = std::getenv("PS2X_VU1DUMP");
            if (!env)
                return out;
            char *end = nullptr;
            out.from = std::strtoll(env, &end, 10);
            out.to = out.from;
            if (end && *end == '-')
                out.to = std::strtoll(end + 1, &end, 10);
            if (end && *end == '/')
                out.step = std::max(1ll, std::strtoll(end + 1, &end, 10));
            if (end && *end == ':')
                out.count = std::atoi(end + 1);
            return out;
        }();
        if (spec.from < 0)
            return {};
        const long long frame = static_cast<long long>(spec.vsync ? ps2dbg::vsyncTick.load() : ps2dbg::presentedFrames.load());
        if (frame < spec.from || frame > spec.to || ((frame - spec.from) % spec.step) != 0)
            return {};
        static long long s_frame = -1;
        static int s_seq = 0;
        if (frame != s_frame)
        {
            s_frame = frame;
            s_seq = 0;
        }
        const int callSeq = s_seq++; // TEMP(transformers): names retain the actual eligible-call sequence.
        if (callSeq < spec.skip || callSeq - spec.skip >= spec.count)
            return {};
        char name[96];
        std::snprintf(name, sizeof(name), "vu1dump_%c%05lld_%03d_%c_", spec.vsync ? 'v' : 'f', frame, callSeq, mscnt ? 'c' : 'm');
        return name;
    }

    void writeVu1Dump(const std::string &name, uint32_t word0, uint32_t top, uint32_t itop, PS2Memory &memory, VU1Interpreter &vu1)
    {
        std::ofstream out(name, std::ios::binary);
        const uint32_t header[4] = {word0, top, itop, static_cast<uint32_t>(sizeof(VU1State))};
        out.write(reinterpret_cast<const char *>(header), sizeof(header));
        out.write(reinterpret_cast<const char *>(memory.getVU1Code()), PS2_VU1_CODE_SIZE);
        out.write(reinterpret_cast<const char *>(memory.getVU1Data()), PS2_VU1_DATA_SIZE);
        out.write(reinterpret_cast<const char *>(&vu1.state()), sizeof(VU1State));
    }

    // GAME(transformers) STOPGAP: the Amazon level's VU1 tessellator draws every strip twice (field 1 with matrix M1 at
    // VU1 q038-q03B, field 2 with M2 at q03C-q03F), but M2 is never written in our runtime (still 0xFFE0E0E0 fill) -> all
    // field-2 vertices XYZ=0 and every other display line dark. Until the real writer is found, copy M1 into an unwritten M2
    // (private qualification evidence: replay with M2:=M1 gives a valid identical field-2 copy). PS2X_FIELD2_STOPGAP=0 disables.
    void ps2Field2MatrixStopgap(PS2Memory &memory)
    {
        static const bool s_on = []()
        {
            const char *e = std::getenv("PS2X_FIELD2_STOPGAP");
            if (e && e[0] == '0')
                return false;
            return PS2Runtime::getIoPaths().elfPath.string().find("SLUS_206.68") != std::string::npos;
        }();
        if (!s_on)
            return;
        uint8_t *data = memory.getVU1Data();
        uint32_t m2[16];
        std::memcpy(m2, data + 0x3Cu * 16u, sizeof(m2));
        for (uint32_t w : m2)
            if (w != 0xFFE0E0E0u)
                return;
        std::memcpy(data + 0x3Cu * 16u, data + 0x38u * 16u, 64u);
        if (memory.vif1StreamExecuting() || memory.instantWorker()) return;
        static uint32_t s_logs = 0u; // TEMP(transformers): M2 is copied from the matrix that scratchpad word 0x7000067C points to
        if (s_logs++ < 4u)
        {
            uint32_t ptr = 0u, spr630[4] = {};
            try
            {
                ptr = memory.read32(0x7000067Cu);
                for (int i = 0; i < 4; ++i)
                    spr630[i] = memory.read32(0x70000630u + 4u * i);
            }
            catch (...) {}
            std::cerr << "[field2:stopgap] M2 unwritten; SPR[0x67C]=0x" << std::hex << ptr << " SPR[0x630..]=" << spr630[0] << ","
                      << spr630[1] << "," << spr630[2] << "," << spr630[3] << std::dec << " vs="
                      << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << std::endl;
        }
    }

    // TEMP(transformers): with PS2X_VU1WATCH=<lo>-<hi>@<vsFrom>-<vsTo> also log every MSCAL/MSCNT in the vsync window
    // (start pc, TOP/ITOP, the microprogram jump table at VU1 q004-q008).
    void vu1WatchLogCall(const char *kind, uint32_t pc, uint32_t top, uint32_t itop, PS2Memory &memory)
    {
        static const std::pair<long long, long long> s_win = []()
        {
            const char *e = std::getenv("PS2X_VU1WATCH");
            if (!e)
                return std::pair<long long, long long>{1, 0};
            const char *at = std::strchr(e, '@');
            if (!at)
                return std::pair<long long, long long>{0, 1ll << 62};
            char *n = nullptr;
            const long long from = std::strtoll(at + 1, &n, 10);
            const long long to = (n && *n == '-') ? std::strtoll(n + 1, nullptr, 10) : (1ll << 62);
            return std::pair<long long, long long>{from, to};
        }();
        const long long vs = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        if (vs < s_win.first || vs > s_win.second)
            return;
        static uint32_t s_logs = 0u;
        if (s_logs++ >= 4000u)
            return;
        uint32_t jt[20] = {};
        std::memcpy(jt, memory.getVU1Data() + 4 * 16, sizeof(jt));
        std::cerr << "[vu1watch:call] vs=" << vs << " " << kind << " pc=0x" << std::hex << pc << " top=0x" << top << " itop=0x"
                  << itop << " q004-8=";
        for (int i = 0; i < 20; ++i)
            std::cerr << (i ? (i % 4 ? "," : "|") : "") << jt[i];
        std::cerr << std::dec << std::endl;
    }

    // TEMP(transformers): PS2X_VU1CUT_DUMP=1 snapshots VU1 before every MSCAL/MSCNT and, for the first 3 calls that hit
    // the cycle budget (a microprogram that never ends), writes vu1cut_<n>_vs<vs>_pre.bin (replayable start state) and
    // _post.bin (state at the cut) to PS2X_DUMP_DIR, same format as the PS2X_VU1DUMP files.
    struct Vu1CutSnapshot
    {
        uint32_t header[4] = {};
        std::vector<uint8_t> code, data;
        VU1State state{};
    };

    bool vu1CutDumpOn()
    {
        static const bool s_on = std::getenv("PS2X_VU1CUT_DUMP") != nullptr;
        return s_on;
    }

    void vu1CutSnapshot(Vu1CutSnapshot &s, uint32_t word0, uint32_t top, uint32_t itop, PS2Memory &memory, VU1Interpreter &vu1)
    {
        s.header[0] = word0;
        s.header[1] = top;
        s.header[2] = itop;
        s.header[3] = static_cast<uint32_t>(sizeof(VU1State));
        s.code.assign(memory.getVU1Code(), memory.getVU1Code() + PS2_VU1_CODE_SIZE);
        s.data.assign(memory.getVU1Data(), memory.getVU1Data() + PS2_VU1_DATA_SIZE);
        s.state = vu1.state();
    }

    void vu1CutCheck(const Vu1CutSnapshot &pre, uint64_t cyclesBefore, uint32_t budget, uint32_t word0, uint32_t top,
                     uint32_t itop, PS2Memory &memory, VU1Interpreter &vu1)
    {
        if (vu1.state().cycles - cyclesBefore < budget)
            return;
        static int s_n = 0;
        if (s_n >= 3)
            return;
        ++s_n;
        const char *dir = std::getenv("PS2X_DUMP_DIR");
        const std::string base = std::string(dir && *dir ? dir : ".") + "/vu1cut_" + std::to_string(s_n) + "_vs" +
                                 std::to_string(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        {
            std::ofstream out(base + "_pre.bin", std::ios::binary);
            out.write(reinterpret_cast<const char *>(pre.header), sizeof(pre.header));
            out.write(reinterpret_cast<const char *>(pre.code.data()), static_cast<std::streamsize>(pre.code.size()));
            out.write(reinterpret_cast<const char *>(pre.data.data()), static_cast<std::streamsize>(pre.data.size()));
            out.write(reinterpret_cast<const char *>(&pre.state), sizeof(VU1State));
        }
        writeVu1Dump(base + "_post.bin", word0, top, itop, memory, vu1);
        std::cerr << "[vu1:cut] dumped " << base << "_pre/_post.bin word0=0x" << std::hex << word0 << " top=0x" << top
                  << " itop=0x" << itop << " endPc=0x" << vu1.state().pc << std::dec << std::endl;
    }
}

// TEMP(transformers) qualification (release fix): PS2X_VU1_COALESCE (=1 on, =0 off; unset = on with PS2X_LOAD_SKIP=worker...,
// off otherwise) - EE replay of worker PATH1 output.
// Runs of consecutive, contiguous, same-context, complete vertex-only PATH1 packets (immediate, not DIRECTHL) are submitted
// as one GIF transfer at the end of the run (next non-matching packet or the end of the replay event, afterReplay). Every
// tag of such a packet is PACKED or REGLIST without A+D (0xE), TEX0_1/2 (6/7) descriptors or IMAGE, and the tags consume
// the packet exactly, so: parallel-gs sees the same GIF stream (path state carries across gif_transfer calls), the
// frontend's EE-state / FIELD60 composite scans have nothing to find in it, and RawGIF's mid-tag reset never applies.
// EE thread only (Vif1Instant::drain is EE-only and never nests).
namespace
{
struct Vu1CoalesceRun
{
    const uint8_t *start = nullptr;
    uint32_t size = 0;
    uint32_t packets = 0;
    gsTimeline::Context ctx{};
};
thread_local Vu1CoalesceRun t_vu1Coalesce;
std::atomic<uint64_t> g_vu1CoalescePackets{0}, g_vu1CoalesceRuns{0};
struct Vu1CoalesceReport
{
    ~Vu1CoalesceReport()
    {
        if (const uint64_t runs = g_vu1CoalesceRuns.load())
            std::fprintf(stderr, "[warpin] coalesce runs=%llu packets=%llu\n", (unsigned long long)runs,
                         (unsigned long long)g_vu1CoalescePackets.load());
    }
} g_vu1CoalesceReport;
bool ps2Vu1CoalesceEnabled()
{
    static const bool on = []
    {
        // Default: on only together with the worker-paced loading vortex (PS2X_LOAD_SKIP=worker...). With a fixed
        // LOAD_SKIP=N the cheaper replay lets the EE outrun the VU1 worker during the vortex and the queued vortex calls
        // lengthen the load-end wait (test machine: 0.89 -> 2.07 s), so N-mode keeps the v0.7.0 replay unless =1 asks for it.
        const char *e = std::getenv("PS2X_VU1_COALESCE");
        const char *ls = std::getenv("PS2X_LOAD_SKIP");
        const bool v = (e && *e) ? (e[0] != '0') : (ls && std::strncmp(ls, "worker", 6) == 0);
        std::cerr << "[warpin] PS2X_VU1_COALESCE=" << (v ? 1 : 0) << " (EE replay of worker PATH1 vertex packets in runs)" << std::endl;
        return v;
    }();
    return on;
}
bool ps2Vu1VertexOnlyPacket(const uint8_t *p, uint32_t n)
{
    if (!p || n < 16u || (n & 15u))
        return false;
    uint32_t off = 0;
    while (off < n)
    {
        if (off + 16u > n)
            return false;
        uint64_t lo, hi;
        std::memcpy(&lo, p + off, 8);
        std::memcpy(&hi, p + off + 8, 8);
        off += 16u;
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        if (!nreg)
            nreg = 16u;
        if (flg >= 2u) // IMAGE / disabled
            return false;
        for (uint32_t r = 0; r < nreg; ++r)
        {
            const uint32_t d = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
            if (d == 0xEu || d == 6u || d == 7u)
                return false;
        }
        const uint64_t qw = flg == 0u ? uint64_t(nloop) * nreg : (uint64_t(nloop) * nreg + 1u) / 2u;
        if (qw * 16u > n - off)
            return false;
        off += static_cast<uint32_t>(qw * 16u);
    }
    return off == n;
}
bool sameGsContext(const gsTimeline::Context &a, const gsTimeline::Context &b)
{
    return a.tick == b.tick && a.field == b.field && a.mxcsr == b.mxcsr && a.valid == b.valid;
}
} // namespace

static void ps2Vu1CoalesceFlush(PS2Memory &mem)
{
    Vu1CoalesceRun &r = t_vu1Coalesce;
    if (!r.start)
        return;
    const uint8_t *start = r.start;
    const uint32_t size = r.size, packets = r.packets;
    const gsTimeline::Context ctx = r.ctx;
    r.start = nullptr;
    r.size = r.packets = 0;
    g_vu1CoalescePackets.fetch_add(packets, std::memory_order_relaxed);
    g_vu1CoalesceRuns.fetch_add(1, std::memory_order_relaxed);
    gsTimeline::Scope scope(ctx);
    mem.submitGifPacket(GifPathId::Path1, start, size, true, false);
}

static bool ps2Vu1CoalescePacket(PS2Memory &mem, const uint8_t *p, uint32_t n, uint32_t path, bool immediate, bool hl)
{
    if (!ps2Vu1CoalesceEnabled())
        return false;
    Vu1CoalesceRun &r = t_vu1Coalesce;
    if (path != 1u || !immediate || hl || !ps2Vu1VertexOnlyPacket(p, n))
    {
        ps2Vu1CoalesceFlush(mem); // keep order: the pending run goes first
        return false;
    }
    const gsTimeline::Context ctx = gsTimeline::current;
    if (r.start && p == r.start + r.size && r.size + n <= (256u << 10) && sameGsContext(ctx, r.ctx))
    {
        r.size += n;
        ++r.packets;
        return true;
    }
    ps2Vu1CoalesceFlush(mem);
    r.start = p;
    r.size = n;
    r.packets = 1;
    r.ctx = ctx;
    return true;
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1Fence([this] { m_vu1.synchronize(); });
    m_vu1.setDependencyFence([this] { if (m_memory.m_vif1Stream) m_memory.m_vif1Stream->fence(); });
    if(Vif1Instant::enabled()) {
        m_memory.m_vif1Instant=std::make_unique<Vif1Instant>();
        m_memory.m_vif1EeField.store(m_memory.gs().csr.load()&0x2000ull);
        auto &instant=*m_memory.m_vif1Instant;
        instant.oracleField=m_memory.m_vif1EeField.load();
        instant.captureContext=[this] {
            return gsTimeline::Context{m_memory.gs().vsyncTick.load(),m_memory.m_vif1EeField.load(),_mm_getcsr(),true};
        };
        instant.afterReplay=[this] {
            ps2Vu1CoalesceFlush(m_memory); // TEMP(transformers) qualification: a pending PATH1 run belongs to this event
            auto &csr=m_memory.gs().csr;auto old=csr.load();
            while(!csr.compare_exchange_weak(old,(old&~0x2000ull)|m_memory.m_vif1EeField.load())){}
        };
        instant.packetReplay=[this](const uint8_t *p,uint32_t n,uint32_t path,bool immediate,bool hl) {
            if(ps2Vu1CoalescePacket(m_memory,p,n,path,immediate,hl))return; // TEMP(transformers) qualification
            m_memory.submitGifPacket(static_cast<GifPathId>(path),p,n,immediate,hl);
        };
        m_memory.m_vif1CaptureControls=[this] {auto *c=m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;return c?c->vu0_fbrst:m_cpuContext.vu0_fbrst;};
        m_vu1.setDependencyFence([this]{Vu1FenceReason r_(kVu1FenceMem);m_memory.m_vif1Instant->fence();}); // qualification: reason
        m_memory.m_vif1Instant->publishStatus=[this](uint32_t status){
            auto *c=m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            if(c)c->vu0_vpu_stat=(c->vu0_vpu_stat&~0x700u)|status;
            m_cpuContext.vu0_vpu_stat=(m_cpuContext.vu0_vpu_stat&~0x700u)|status;
        };
    }
    m_memory.configureVif1Stream(
        [this] { return m_eeScheduler ? m_eeScheduler->vu1GuestCycle() : 0; },
        [this](uint64_t n) {
            if (m_eeScheduler) m_eeScheduler->stallVif1Cycles(n);
        },
        [this] { auto *c=m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            return c ? c->vu0_fbrst : m_cpuContext.vu0_fbrst; },
        [this](uint32_t status) {
            auto *c=m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            if (c) c->vu0_vpu_stat=(c->vu0_vpu_stat & ~0x700u)|status;
            m_cpuContext.vu0_vpu_stat=(m_cpuContext.vu0_vpu_stat & ~0x700u)|status;
        });
    if (m_memory.m_vif1Stream) m_gs.setVu1Poll([this] {
        auto &stream=*m_memory.m_vif1Stream;
        if (stream.ownsCurrentThread()) stream.poll(stream.now ? stream.now() : 0);
    });
    if(m_memory.m_vif1Instant) {
        m_gs.setVu1Poll([this]{m_memory.m_vif1Instant->drain();});
        m_gs.setVu1Ordered([this](std::function<void()> event) {
            auto &q=*m_memory.m_vif1Instant;if(q.replaying())return false;
            q.enqueue([&q,event=std::move(event)]() mutable{q.emit(std::move(event));});return true;
        });
    }
    m_gs.setVu1Fence([this] {
        if(m_memory.m_vif1Instant){Vu1FenceReason r_(g_vu1FenceReason==kVu1FenceOther?kVu1FenceGs:g_vu1FenceReason);m_memory.m_vif1Instant->fence();} // qualification: reason
        else if (m_memory.m_vif1Stream && m_memory.m_vif1Stream->ownsCurrentThread()) m_memory.fenceVu1();
        else m_vu1.synchronizeIfOwner();
    });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     if(m_memory.instantWorker()) {
                                         auto &q=*m_memory.m_vif1Instant;
                                         m_vu1.state().dBitEnabled=(q.controls&0x400u)!=0;
                                         m_vu1.state().tBitEnabled=(q.controls&0x800u)!=0;
                                         ps2Field2MatrixStopgap(m_memory);
                                         m_vu1.execute(m_memory.getVU1Code(),PS2_VU1_CODE_SIZE,m_memory.getVU1Data(),PS2_VU1_DATA_SIZE,m_gs,&m_memory,startPC,top,itop,ps2Vu1CycleBudget());
                                         q.stopBits=(m_vu1.state().stoppedByD?0x200u:0u)|(m_vu1.state().stoppedByT?0x400u:0u);
                                         return;
                                     }

                                     if (m_memory.vif1StreamExecuting()) {
                                         auto &stream=*m_memory.m_vif1Stream;
                                         m_vu1.state().dBitEnabled=(stream.controls & (1u<<10))!=0;
                                         m_vu1.state().tBitEnabled=(stream.controls & (1u<<11))!=0;
                                         ps2Field2MatrixStopgap(m_memory);
                                         const uint64_t before=m_vu1.state().cycles;
                                         m_vu1.execute(m_memory.getVU1Code(),PS2_VU1_CODE_SIZE,m_memory.getVU1Data(),PS2_VU1_DATA_SIZE,m_gs,&m_memory,startPC,top,itop,ps2Vu1CycleBudget());
                                         stream.vuCycles+=m_vu1.state().cycles-before;
                                         stream.stopBits=(m_vu1.state().stoppedByD?0x200u:0u)|(m_vu1.state().stoppedByT?0x400u:0u);
                                         return;
                                     }

                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     const std::string dumpName = vu1DumpNameForCall(false);
                                     const bool dumpThis = !dumpName.empty();
                                     auto dumpVu1 = [&](const char *tag)
                                     {
                                         writeVu1Dump(dumpName + tag + ".bin", startPC, top, itop, m_memory, m_vu1);
                                     };
                                     if (dumpThis)
                                         dumpVu1("pre");
                                     vu1WatchLogCall("MSCAL", startPC, top, itop, m_memory); // TEMP(transformers)
                                     ps2Field2MatrixStopgap(m_memory);                         // GAME(transformers) stopgap
                                     static Vu1CutSnapshot s_cutPre; // TEMP(transformers)
                                     const uint64_t cutCyclesBefore = m_vu1.state().cycles;
                                     if (vu1CutDumpOn())
                                         vu1CutSnapshot(s_cutPre, startPC, top, itop, m_memory, m_vu1);
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, ps2Vu1CycleBudget());
                                     if (vu1CutDumpOn())
                                         vu1CutCheck(s_cutPre, cutCyclesBefore, ps2Vu1CycleBudget(), startPC, top, itop, m_memory, m_vu1);
                                     if (dumpThis)
                                     {
                                         dumpVu1("post");
                                         std::cout << "[vu1:dump] " << dumpName << " MSCAL startPC=0x" << std::hex << startPC << " top=0x" << top
                                                   << " itop=0x" << itop << std::dec << std::endl;
                                     }
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         ((!Vu1Worker::enabled() || (cpuContext->vu0_fbrst & (1u << 10))) && m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         ((!Vu1Worker::enabled() || (cpuContext->vu0_fbrst & (1u << 11))) && m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     if(m_memory.instantWorker()) {
                                         auto &q=*m_memory.m_vif1Instant;
                                         m_vu1.state().dBitEnabled=(q.controls&0x400u)!=0;
                                         m_vu1.state().tBitEnabled=(q.controls&0x800u)!=0;
                                         ps2Field2MatrixStopgap(m_memory);
                                         m_vu1.resume(m_memory.getVU1Code(),PS2_VU1_CODE_SIZE,m_memory.getVU1Data(),PS2_VU1_DATA_SIZE,m_gs,&m_memory,top,itop,ps2Vu1CycleBudget());
                                         q.stopBits=(m_vu1.state().stoppedByD?0x200u:0u)|(m_vu1.state().stoppedByT?0x400u:0u);
                                         return;
                                     }

                                     if (m_memory.vif1StreamExecuting()) {
                                         auto &stream=*m_memory.m_vif1Stream;
                                         m_vu1.state().dBitEnabled=(stream.controls & (1u<<10))!=0;
                                         m_vu1.state().tBitEnabled=(stream.controls & (1u<<11))!=0;
                                         ps2Field2MatrixStopgap(m_memory);
                                         const uint64_t before=m_vu1.state().cycles;
                                         m_vu1.resume(m_memory.getVU1Code(),PS2_VU1_CODE_SIZE,m_memory.getVU1Data(),PS2_VU1_DATA_SIZE,m_gs,&m_memory,top,itop,ps2Vu1CycleBudget());
                                         stream.vuCycles+=m_vu1.state().cycles-before;
                                         stream.stopBits=(m_vu1.state().stoppedByD?0x200u:0u)|(m_vu1.state().stoppedByT?0x400u:0u);
                                         return;
                                     }

                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     const std::string cntDumpName = vu1DumpNameForCall(true);
                                     const bool cntDumpThis = !cntDumpName.empty();
                                     const uint32_t resumePc = 0x80000000u | m_vu1.state().pc; // header word 0 for MSCNT dumps (replay: resume())
                                     auto cntDumpVu1 = [&](const char *tag)
                                     {
                                         writeVu1Dump(cntDumpName + tag + ".bin", resumePc, top, itop, m_memory, m_vu1);
                                     };
                                     if (cntDumpThis)
                                         cntDumpVu1("pre");
                                     vu1WatchLogCall("MSCNT", resumePc & 0x7FFFFFFFu, top, itop, m_memory); // TEMP(transformers)
                                     ps2Field2MatrixStopgap(m_memory);                                         // GAME(transformers) stopgap
                                     static Vu1CutSnapshot s_cntCutPre; // TEMP(transformers)
                                     const uint64_t cntCutCyclesBefore = m_vu1.state().cycles;
                                     if (vu1CutDumpOn())
                                         vu1CutSnapshot(s_cntCutPre, resumePc, top, itop, m_memory, m_vu1);
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, ps2Vu1CycleBudget());
                                     if (vu1CutDumpOn())
                                         vu1CutCheck(s_cntCutPre, cntCutCyclesBefore, ps2Vu1CycleBudget(), resumePc, top, itop, m_memory, m_vu1);
                                     if (cntDumpThis)
                                     {
                                         cntDumpVu1("post");
                                     }
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         ((!Vu1Worker::enabled() || (cpuContext->vu0_fbrst & (1u << 10))) && m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         ((!Vu1Worker::enabled() || (cpuContext->vu0_fbrst & (1u << 11))) && m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    // Crash report + minidump into PS2X_PLAYTEST_DIR / PS2X_DUMP_DIR on any fatal exception (no dialogs).
    InstallCrashReporter();
    RegisterThreadForCrashReporter("main/render");
    // TEMP(transformers) v0.6.0 integration: release label. PS2X_VERSION (set by play_env.txt, e.g. "Transformers PC v0.6.0")
    // is printed as the first runtime log line and used as the game window title. Unset = old title, no line.
    if (const char *ver = std::getenv("PS2X_VERSION"); ver && *ver)
    {
        std::fprintf(stderr, "[version] %s\n", ver);
        std::fflush(stderr);
        title = ver;
    }
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }
        { // TEMP(transformers): guest state + RDRAM for crash reports (atomics only: called from the crash helper thread)
            static PS2Runtime *s_crashRuntime = nullptr;
            s_crashRuntime = this;
            SetCrashReporterGuestMemory(m_memory.getRDRAM(), PS2_RAM_SIZE);
            SetCrashReporterGuestInfo([](char *buf, size_t size)
                                      {
                PS2Runtime *rt = s_crashRuntime;
                if (!rt)
                    return;
                std::snprintf(buf, size, "ee pc=0x%08x ra=0x%08x vs=%llu pads=%llu",
                              rt->m_debugPc.load(std::memory_order_relaxed), rt->m_debugRa.load(std::memory_order_relaxed),
                              static_cast<unsigned long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)),
                              static_cast<unsigned long long>(ps2dbg::padReads.load(std::memory_order_relaxed))); });
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // PS2X_HIDDEN_WINDOW=1: create the window without taking focus (unattended test runs must not steal maintainer's
        // keyboard). Do NOT minimise: a minimised GL window throttles buffer swaps and stalls presentation pacing.
        const char *unfocusedEnv = std::getenv("PS2X_HIDDEN_WINDOW");
        const bool unfocused = unfocusedEnv && *unfocusedEnv && *unfocusedEnv != '0';
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | (unfocused ? FLAG_WINDOW_UNFOCUSED : 0u));
        // Open the window at the presentation display aspect (4:3 -> 597x448; PS2X_WIDESCREEN -> that aspect).
        const float wsAspect = ps2WidescreenAspect();
        const float dispAspect = (wsAspect > 0.0f) ? wsAspect : ps2PresentDisplayAspect();
        const int winW = (dispAspect > 0.0f) ? static_cast<int>(HOST_WINDOW_HEIGHT * dispAspect + 0.5f) : HOST_WINDOW_WIDTH;
        InitWindow(winW, HOST_WINDOW_HEIGHT, title);
        ps2WidescreenUpdateWindow();
        if (ps2HostAudioEnabled())
            InitAudioDevice();
        // SPU2 is the sole playback source; keep legacy VAG/RPC HLE playback muted.
        m_audioBackend.setAudioReady(false);
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

namespace // GAME(transformers): native replacement for 0x1B0DD0, see loadELF
{
// 0x1B0DD0 = copy a Mat4 (a1 -> a0) forcing the w column to (0,0,0,1): lqc2 vf4-vf7; vsub.w vfN,vfN,vfN (rows 0-2);
// vmove.w vf7,vf0; sqc2; return a0. On the PS2 VU every bit pattern is a finite float, so x - x == 0; the recompiled SSE
// code keeps NaN when the junk w qualification exponent 255 (stale VU0 lanes / stack, e.g. 0xFFE0E0E0), which poisoned the
// camera's field-2 matrix (EC0 -> SPR 0xF00 -> M2: every other line dark in the level; private qualification evidence).
// Bit-exact with PS2 semantics for all 11 callers. PS2X_WCLEAN_FIX=0 disables.
void tfMat4CopyAffine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t dst = GPR_U32(ctx, 4);
    const uint32_t src = GPR_U32(ctx, 5);
    __m128 rows[4];
    for (int i = 0; i < 4; ++i) // all loads first (src may equal dst)
        rows[i] = _mm_castsi128_ps(READ128(src + 16u * static_cast<uint32_t>(i)));
    for (int i = 0; i < 4; ++i)
    {
        rows[i] = _mm_insert_ps(rows[i], _mm_set_ss(i == 3 ? 1.0f : 0.0f), 0x30); // lane 3 (w) = 0 / 1.0
        ctx->vu0_vf[4 + i] = rows[i];                                             // guest-visible VU0 side effects
    }
    for (int i = 0; i < 4; ++i)
        WRITE128(dst + 16u * static_cast<uint32_t>(i), _mm_castps_si128(rows[i]));
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 4)); // daddu v0, a0, zero (delay slot)
    ctx->pc = GPR_U32(ctx, 31);           // jr ra
}
// GAME(transformers): frame-wait check experiment (PS2X_TF_NOFRAMEWAIT=1, default OFF; see private qualification evidence).
// sub_0025D990 is polled by the main-loop wait (0x26EEC0: jal 0x25D990; beqz v0 -> poll again), so it returns 1 when
// the wait is OVER (flag -0x414C(gp)==0, or flag -0x57C4(gp)!=0) and 0 while it must keep waiting. To skip the wait it has
// to return 1 (Gemini's note said "return 0", which would spin forever).
void tfFrameWaitSkip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    SET_GPR_U64(ctx, 2, 1);
    ctx->pc = GPR_U32(ctx, 31); // jr ra
}


// 0x25DE90 = the game's own frame-sync wait (this is the real 30 fps limiter, found while testing 0x25D990): when byte
// 0x6F8539 != 0 it spins until the vsync counter (gp-0x4220) >= lastFrameCount (gp-0x40D8) + 1 or + 2 (+ a field-parity match);
// when 0x6F8539 == 0 it only runs the field-toggle tail and returns the counter. PS2X_TF_NOFRAMEWAIT&2 forces that tail path.
static PS2Runtime::RecompiledFunction g_tfFrameSyncOrig = nullptr;
static bool g_tfFrameSyncSkip = false;
static bool g_tfFrameSync60 = false; // PS2X_TF_NOFRAMEWAIT&4: force gp-0x40E0 = 0 so the wait is "next vsync" (60 fps cap) instead of the 2-vsync/field-parity path
static bool g_tfFrameSyncLog = false;
// TEMP(transformers): PS2X_TF_FRAMELOG=1 logs one line per call (= per game frame) with the guest vsync counter, pad reads, VU1 MSCALs.
void tfFrameSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool entry = (ctx->pc == 0x0025DE90u);
    static uint32_t s_lastCnt = 0xFFFFFFFFu;
    static uint32_t s_calls = 0;
    const bool newFrame = entry && g_tfFrameSyncLog && (++s_calls, READ32(GPR_U32(ctx, 28) - 0x4220u) != s_lastCnt);
    if (newFrame)
    {
        const uint32_t gp = GPR_U32(ctx, 28);
        s_lastCnt = READ32(gp - 0x4220u);
        std::cerr << "[tfframe] calls=" << s_calls << " vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed)
                  << " cnt=" << READ32(gp - 0x4220u) << " base=" << READ32(gp - 0x40D8u)
                  << " pads=" << ps2dbg::padReads.load(std::memory_order_relaxed)
                  << " mscal=" << ps2dbg::vu1Mscal.load(std::memory_order_relaxed)
                  << " en=" << static_cast<unsigned>(READ8(0x6F8539u)) << " d0=" << static_cast<unsigned>(READ8(gp - 0x40D0u))
                  << " dc=" << static_cast<unsigned>(READ8(gp - 0x40DCu)) << " e0=" << static_cast<unsigned>(READ8(gp - 0x40E0u))
                  << " d4=" << static_cast<unsigned>(READ8(gp - 0x40D4u)) << std::endl;
        s_calls = 0;
    }
    if (entry && g_tfFrameSync60)
        WRITE8(GPR_U32(ctx, 28) - 0x40E0u, 0);
    if (g_tfFrameSyncSkip)
    {
        const uint8_t saved = READ8(0x6F8539u);
        WRITE8(0x6F8539u, 0);
        g_tfFrameSyncOrig(rdram, ctx, runtime);
        WRITE8(0x6F8539u, saved);
        return;
    }
    g_tfFrameSyncOrig(rdram, ctx, runtime);
}
}

// PS2X_WIDESCREEN=<display aspect> (e.g. 1.7778, 2.3333, or "16:9" / "21:9"); 0 when unset or <= 1.4 (4:3). Generic:
// the OSD reports TV type 16:9 (Kernel/Syscalls/Helpers/Runtime.h) and the presenter stretches the frame to this aspect.
// GAME(transformers): saved menu intent and window aspect cross EE/render threads atomically.
static std::atomic<bool> g_tfMenuWide{false};
static std::atomic<float> g_tfWindowAspect{16.0f / 9.0f};
static bool ps2WidescreenOverride()
{
    const float pc = ps2x::ui::g_pcAspect.load(std::memory_order_relaxed);
    if (pc == -2.0f) return false;
    if (pc >= 0.0f) return true;
    static const bool present = []() {
        const char *e = std::getenv("PS2X_WIDESCREEN");
        return e && *e;
    }();
    return present; // An explicit 0 also overrides the saved ON setting; environment is read once at launch.
}
static float ps2WidescreenAspect()
{
    const float pc = ps2x::ui::g_pcAspect.load(std::memory_order_relaxed);
    if (pc == -2.0f) return g_tfMenuWide.load(std::memory_order_acquire) ? g_tfWindowAspect.load(std::memory_order_acquire) : 0.0f;
    if (pc >= 0.0f) return pc > 1.4f ? pc : 0.0f;
    static const float aspect = []() -> float {
        const char *ws = std::getenv("PS2X_WIDESCREEN");
        if (!ws || !*ws)
            return 0.0f;
        char *end = nullptr;
        double a = std::strtod(ws, &end);
        if (end && *end == ':')
        {
            const double d = std::strtod(end + 1, nullptr);
            a = (d > 0.0) ? a / d : 0.0;
        }
        return (a > 1.4 && a < 8.0) ? static_cast<float>(a) : 0.0f;
    }();
    return ps2WidescreenOverride() ? aspect :
        (g_tfMenuWide.load(std::memory_order_acquire) ? g_tfWindowAspect.load(std::memory_order_acquire) : 0.0f);
}

// Render thread only: use the current monitor when ON is first applied, then follow client/fullscreen size.
static void ps2WidescreenUpdateWindow()
{
    const bool on = !ps2WidescreenOverride() && g_tfMenuWide.load(std::memory_order_acquire);
    static bool previous = false;
    if (on && !previous && !IsWindowFullscreen())
    {
        const int monitor = GetCurrentMonitor();
        const int width = GetMonitorWidth(monitor), height = GetMonitorHeight(monitor);
        SetWindowSize(width, height);
        if (width > 0 && height > 0)
            g_tfWindowAspect.store(static_cast<float>(width) / height, std::memory_order_release);
        previous = on;
        return; // Client dimensions can lag the resize until the next window-event poll.
    }
    previous = on;
    if (!on)
    {
        const int monitor = GetCurrentMonitor();
        const int width = GetMonitorWidth(monitor), height = GetMonitorHeight(monitor);
        if (width > 0 && height > 0)
            g_tfWindowAspect.store(static_cast<float>(width) / height, std::memory_order_release);
        return;
    }
    if (GetScreenWidth() > 0 && GetScreenHeight() > 0)
        g_tfWindowAspect.store(static_cast<float>(GetScreenWidth()) / GetScreenHeight(), std::memory_order_release);
}

extern std::atomic<uint64_t> g_ps2LastPath1Vsync; // ps2_memory.cpp: presented-frame vsync of the last PATH1 (VU1) GIF packet
extern std::atomic<uint64_t> g_ps2LastPath1EeVsync; // ps2_memory.cpp: the same in guest EE time
extern std::atomic<uint64_t> g_ps2MpegLastPictureVsync; // Kernel/Stubs/MPEG.cpp: last sceMpegGetPicture
// A full-screen FMV frame: no VU1 geometry was drawn in the last 8 vsyncs (a movie inside a 3D scene, e.g. the globe
// preview, keeps the scene wide) and either the displayed frame is 640 wide (the game shows movies in a 640x448 display
// mode, its 3D/menus at 512x448) or an MPEG picture was decoded in the last 12 vsyncs. Such frames were not widened,
// so the presenter shows them at 4:3 (pillarboxed); the HUD squeeze only runs in frames with VU1 geometry anyway.
// VU1 geometry drawn in the last 8 vsyncs (a 3D scene, rendered Hor+). EE thread (HUD hooks): guest EE time on both
// sides, so the decision - which changes the guest's 2D vertex data - is the same on every run. (The qualification version used
// the presented-frame vsync, which lags the EE by a host-timing-dependent amount: ws2 run ws2f_169 drifted by 8 guest
// cycles in the hub and diverged later; the known '+8 cycles at 21:9' had the same cause.)
static bool ps2WidescreenVu1Frame(PS2Runtime *runtime)
{
    const uint64_t now = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
    const uint64_t lastVu1 = g_ps2LastPath1EeVsync.load(std::memory_order_relaxed);
    return lastVu1 != 0u && now - lastVu1 < 8u;
}
// frameWidth = width of the displayed PS2 frame (512 for the game's 3D/menus, 640 for full-screen movies, including the
// level intro movies fed through the IPU DMA path, which never call sceMpegGetPicture - ws2 lane, run ws2_219a step 9).
static bool ps2WidescreenFmvFrame(float frameWidth)
{
    const uint64_t now = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
    const uint64_t lastVu1 = g_ps2LastPath1Vsync.load(std::memory_order_relaxed);
    const uint64_t lastPic = g_ps2MpegLastPictureVsync.load(std::memory_order_relaxed);
    const bool vu1 = lastVu1 != 0u && now - lastVu1 < 8u;
    // lastPic is EE time (scheduler vsync), now/lastVu1 are the presented frame's vsync, which lags the EE by the GS
    // pipeline (up to ~200 vsyncs seen in run ws2_219b) -> a picture stamped after 'now' counts as recent.
    const bool mpeg = lastPic != 0u && (lastPic >= now || now - lastPic < 12u);
    const bool wide640 = frameWidth >= 576.0f;
    static const int rule = []() { const char *e = std::getenv("PS2X_WS2_FMVRULE"); return e ? std::atoi(e) : 0; }(); // TEMP(transformers): ws2 A/B
    const bool fmv = rule == 1 ? (wide640 || (mpeg && !vu1)) : (rule == 2 ? (mpeg && !vu1) : (!vu1 && (wide640 || mpeg)));
    { // TEMP(transformers): PS2X_WS2_FMVLOG=1 - one line per change of the inputs' booleans (render thread)
        static const bool log = []() { const char *e = std::getenv("PS2X_WS2_FMVLOG"); return e && e[0] == '1'; }();
        static int lastKey = -1;
        const int key = (vu1 ? 1 : 0) | (mpeg ? 2 : 0) | (wide640 ? 4 : 0) | (fmv ? 8 : 0);
        if (log && key != lastKey)
        {
            lastKey = key;
            std::cout << "[ws2:fmv] vs=" << now << " lastVu1=" << lastVu1 << " lastPic=" << lastPic << " w=" << frameWidth
                      << " vu1=" << vu1 << " mpeg=" << mpeg << " fmv=" << fmv << std::endl;
        }
    }
    return fmv;
}

#if defined(PS2X_WITH_PGS)
int ps2PgsPresentedScene(); // GAME(transformers): metadata carried with the actual published PGS image
#endif

namespace // GAME(transformers): widescreen support, see loadELF and private qualification evidence
{
// The game has a native anamorphic 16:9 mode: Options+4 "widescreenEnabled" (Options = *(gp-0x3EAC)), defaulted by
// sub_0032E0F0 from sceScfGetAspect() == 2, mirrored into the flag byte gp-0x40EC (0x6CF004), which drives the pixel
// aspect (sub_0025E180: *1.3333), the camera FOV (fn_002CAB30: *1.2667, i.e. Hor+) and HUD/screen-space layout
// (fn_0034D210, sub_00318DF0). But that mode is not what we want: the flag shrinks r (pixel aspect) so the projection
// becomes Vert- (hub/menu cameras keep hfov 90 -> cropped top/bottom), only the gameplay camera widens its FOV (by an
// approximate 1.2667) and the hub camera moves back, while the HUD and captions are still drawn for 4:3 and stretch
// (runtime evidence: private qualification evidence). So by default (Hor+ mode) the game stays in its 4:3 mode and we
//  (1) preserve Options+4 for the menu and card; clear only the rendering mirror gp-0x40EC after its writers
//      (including the separate 0x2780A0 restore entry) -> camera logic identical to 4:3. A saved ON enables
//      the window's aspect automatically; PS2X_WIDESCREEN (including explicit 0) takes precedence;
//  (2) use Hor+ above 16:9 and Vert+ below 16:9 (16:9 horizontal FOV is the minimum); the
//      display aspect): the perspective builder sub_0020CA00(Mat4 *m, near f12, far f13, r = H/W*pixelAspect f14,
//      hfov f15) gets tan(hfov/2)*kt and r/kr with kt = max(aspect,16/9)/(4/3), kr = displayK / (flag ? 1.3333 : 1).
//      m00 = 1/(kt*tan), m11 = displayK/(kt*tan*r43); gp-0x46E8 = kt*tan. sub_0020C530 then
//      stores r into gp-0x46D8, which is divided by kr afterwards (perspective path only: gp-0x46D4 == 0).
//      sub_00221E70 builds the VU1 projection P1/P2 (SPR 0x6A0/0x760) and the clip matrix (SPR 0x6E0) from gp-0x46E8
//      (tan_h) and gp-0x46D8 (tan_v = tan_h * r), so geometry, clipping and frustum culling widen consistently
//      (logged: P1xx 256.96 -> 192.69 = x0.75 at 16:9, P1yy unchanged).
// PS2X_WIDESCREEN_MODE=game instead forces the game's own mode on (flag 1, no projection hooks).
struct TfWidescreen
{
    static constexpr uint32_t kGp = 0x006D30F0u;
    static constexpr uint32_t kWideFlag = kGp - 0x40ECu;   // byte, 0x6CF004
    static constexpr uint32_t kOptionsPtr = kGp - 0x3EACu; // word, Options* (byte +4 = widescreenEnabled)
    static constexpr uint32_t kTanH = kGp - 0x46E8u;       // float tan(hfov/2)
    static constexpr uint32_t kAspectR = kGp - 0x46D8u;    // float r = H/W * pixel aspect (tan_v = tan_h * r)
    static constexpr uint32_t kOrtho = kGp - 0x46D4u;      // byte, 1 = orthographic camera
    static constexpr float kGameWideFactor = 1.33333337f;  // pixel-aspect factor of the game's mode (float at 0x6CB1FC)
    static inline float kt = 1.0f;                         // horizontal tangent / HUD width factor
    static inline float displayK = 1.0f;                   // actual aspect / (4/3); r divisor
    static inline float hudY = 1.0f;                       // actual aspect / horizontal reference aspect
    static inline float legacyViewportKr = 1.0f; // TEMP(transformers): OFF-only old entry/post-return behavior
    static inline bool force = false;
    static inline bool nativeMode = false;
    static inline uint8_t flagValue = 0; // forced value of widescreenEnabled (0 in Hor+ mode, 1 in game mode)
    static inline int logBudget = 0; // TEMP(transformers): PS2X_WS_LOG=<n> lines
    static inline uint64_t perspCalls = 0;
    // TEMP(transformers): default-off observer; OFF switch is for the controlled viewport A/B only.
    static inline bool yieldLog = false, yieldFix = true;
    static inline uint64_t viewportStarts = 0, viewportYields = 0, viewportResumes = 0,
        viewportReturns = 0, viewportFixes = 0, buildCalls = 0, buildRaw = 0, buildCorrect = 0,
        buildOther = 0, hudYields = 0, hudRestores = 0;
    static inline float observedRawR = 0.0f, observedKr = 1.0f;
    // TEMP(transformers): all six direct gp-46D8 stores audited from SLUS_206.68.
    // Observe at the first dispatch after each store, before its consumer can run.
    static inline uint32_t aspectWriter = 0, observedPerspectiveRa = 0;
    static inline uint64_t cameraRestoreCalls = 0, buildOtherCut = 0, buildOtherUnexpected = 0;
    static void observeAspectDispatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                      uint32_t source, uint32_t target, bool isCall)
    {
        if (!yieldLog) return;
        if (isCall && target == 0x0020B2B0u) ++cameraRestoreCalls;
        switch (source) {
        case 0x0020B338u: aspectWriter=0x0020B328u; break;
        case 0x0020C8A8u: aspectWriter=0x0020C898u; break;
        case 0x0020C8ECu: if (READ8(kOrtho)==0u) aspectWriter=0x0020C858u; break;
        case 0x002BA6D4u: aspectWriter=0x002BA6B8u; break;
        case 0x002BAD94u: aspectWriter=0x002BAD6Cu; break;
        case 0x002BB394u: aspectWriter=0x002BB36Cu; break;
        default: break;
        }
    }
    static void yieldObserve(PS2Runtime *runtime)
    {
        if (!yieldLog) return;
        static uint64_t last = 0;
        const auto vs = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        if (vs < last + 600u) return;
        last = vs;
        std::cerr << "[ws:yield] vs=" << vs << " fix=" << yieldFix
            << " viewport=" << viewportStarts << " yields=" << viewportYields
            << " resumes=" << viewportResumes << " returns=" << viewportReturns
            << " divisions=" << viewportFixes << " perspective=" << perspCalls
            << " buildP=" << buildCalls << " raw=" << buildRaw << " corrected=" << buildCorrect
            << " other=" << buildOther << " other_cut=" << buildOtherCut
            << " other_unexpected=" << buildOtherUnexpected << " writer=0x" << std::hex << aspectWriter
            << " persp_ra=0x" << observedPerspectiveRa << std::dec
            << " sub20B2B0=" << cameraRestoreCalls << " hud_yields=" << hudYields
            << " hud_restores=" << hudRestores << std::endl;
    }
    static inline PS2Runtime::RecompiledFunction origPerspective = nullptr; // 0x20CA00
    static inline PS2Runtime::RecompiledFunction origViewport = nullptr;    // 0x20C530
    static inline PS2Runtime::RecompiledFunction origFlagWriter[4] = {};    // 0x277FB0, 0x278010, 0x278090, 0x2780E0

    static inline PS2Runtime::RecompiledFunction origOptionGetter = nullptr;
    static void optionGetter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t index = GPR_U32(ctx, 4), ra = GPR_U32(ctx, 31);
        origOptionGetter(rdram, ctx, runtime);
        // GAME(transformers): these three non-UI consumers read Options+4 directly, not the render mirror.
        // Preserve their original 4:3 layout/camera input underneath exact Hor+. UI and save reads stay real.
        if (force && index == 4u &&
            (ra == 0x00513E2Cu || ra == 0x0059C944u || ra == 0x003BCD44u))
            SET_GPR_ZE32(ctx, 2, flagValue);
    }

    static float rdF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a)
    {
        const uint32_t w = READ32(a);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }
    static void wrF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, float f)
    {
        uint32_t w;
        std::memcpy(&w, &f, 4);
        WRITE32(a, w);
    }

    static void updateAspect()
    {
        const float aspect = ps2WidescreenAspect();
        // GAME(transformers): below 16:9 keep its horizontal view and reveal more vertically.
        // Use the actual aspect for r, so the shared projection/clip/culling globals agree.
        displayK = !nativeMode && aspect > 0.0f ? aspect / (4.0f / 3.0f) : 1.0f;
        kt = !nativeMode && aspect > 0.0f ? std::max(aspect, 16.0f / 9.0f) / (4.0f / 3.0f) : 1.0f;
        hudY = displayK / kt;
    }


    // GAME(transformers): render builders use the same tangents as perspective(), without extra guest calls.
    static inline bool cullOn = false; // opt-in until the eco CD checkpoint shift is explained
    // TEMP(transformers): bits 0..12 follow the brief; 13/14 independently enable pregame/special-scene C.
    static inline uint32_t cullSites = 0x6DFFu; // audited render scopes; birdies and actor fallback remain off
    static inline bool cullLog = false;
    static inline PS2Runtime::RecompiledFunction origCull[4] = {};
    struct CullArm { R5900Context *owner; float kh, kr; unsigned site; uint32_t consumeRa; };
    static std::unordered_map<uint32_t, CullArm> &cullArms()
    {
        static std::unordered_map<uint32_t, CullArm> arms; // EE dispatcher only
        return arms;
    }
    // TEMP(transformers): counters/histogram are observational, no guest cycles or writes.
    static inline uint64_t cullCalls[15] = {}, cullFixes[15] = {}, cullMisses[15] = {};
    static std::unordered_map<uint64_t, uint64_t> &cullUnknown()
    {
        static std::unordered_map<uint64_t, uint64_t> hist;
        return hist;
    }
    static float cullKr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return displayK / (READ8(kWideFlag) ? kGameWideFactor : 1.0f);
    }
    static bool cullCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        if (!cam || cam >= 0x02000000u - 0x64u) return false;
        const uint32_t cd = READ32(cam + 0x60u);
        if (!cd || cd >= 0x02000000u - 0x18u || READ32(cd + 0x14u) == 1u) return false;
        const float fov = rdF(rdram, ctx, runtime, cd + 8u) * 0.01745329f;
        return fov > 0.0f && fov < 3.1f;
    }
    static uint32_t cullDisplayCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t display)
    {
        if (!display || display >= 0x02000000u - 8u) return 0u;
        const uint32_t view = READ32(display + 4u);
        return view && view < 0x02000000u - 8u ? READ32(view + 4u) : 0u;
    }
    static bool cullEnabled(unsigned site, float kr)
    {
        return cullOn && !nativeMode && site < 15u && (cullSites & (1u << site)) && kt != 1.0f;
    }
    static void cullPair(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, float factor)
    {
        const float c = rdF(rdram, ctx, runtime, a), s = rdF(rdram, ctx, runtime, a + 4u) * factor;
        const float n = 1.0f / std::sqrt(c*c + s*s);
        wrF(rdram, ctx, runtime, a, c*n);
        wrF(rdram, ctx, runtime, a + 4u, s*n);
    }
    static void cullPlaneFix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t p, float kr)
    {
        cullPair(rdram, ctx, runtime, p + 8u, kt);
        if (kt != kr) cullPair(rdram, ctx, runtime, p + 0x10u, kt / kr);
        // Guard band and pixel-size/LOD factors deliberately retain their camera values.
    }
    static void cullObserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!cullLog) return;
        static uint64_t last = 0;
        const uint64_t vs = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        if (vs < last + 600u) return;
        last = vs;
        std::cerr << "[ws:cull] vs=" << vs << " kt=" << kt << " kr=" << cullKr(rdram, ctx, runtime)
                  << " sites=0x" << std::hex << cullSites << std::dec;
        for (unsigned i=0; i<15u; ++i)
            std::cerr << " s" << i << '=' << cullCalls[i] << '/' << cullFixes[i] << '/' << cullMisses[i];
        const uint32_t peak = READ32(kGp - 0x40B0u), capacity = READ32(kGp - 0x40ACu);
        std::cerr << " dma_bytes=" << peak << '/' << capacity << " dma_words_capacity=" << READ32(kGp - 0x41C0u)*4u;
        for (const auto &h : cullUnknown()) std::cerr << " unknown=0x" << std::hex << h.first << std::dec << ':' << h.second;
        std::cerr << std::endl;
    }
    static unsigned cullBSite(uint32_t ra)
    {
        switch (ra) {
        case 0x42E434u: return 3; case 0x4296FCu: return 4; case 0x270620u: return 5;
        case 0x273F6Cu: return 6; case 0x4368CCu: return 7; case 0x564E54u: return 8;
        case 0x5BAFB0u: return 9; default: return 15;
        }
    }
    static unsigned cullCSite(uint32_t ra)
    {
        switch (ra) {
        case 0x2C4F8Cu: return 10; case 0x2BAA98u: return 11;
        case 0x33339Cu: return 13; case 0x409BECu: return 14; default: return 15;
        }
    }
    template <int I> static void cullOuter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateAspect();
        constexpr uint32_t entry = I == 0 ? 0x1F4B00u : (I == 1 ? 0x20AB70u : 0x2B89C0u);
        const bool start = ctx->pc == entry;
        // Continuations have a live guest frame: original ra was saved before the first possible yield.
        const uint32_t ra = start ? GPR_U32(ctx,31) : READ32(GPR_U32(ctx,29) + (I == 2 ? 0x60u : 0x40u));
        // GPR_U32's register-index zero check does not parenthesise its argument: keep ternaries OUTSIDE the macro.
        const uint32_t object = start ? (I == 1 ? GPR_U32(ctx,5) : GPR_U32(ctx,4)) :
            (I == 2 ? GPR_U32(ctx,19) : GPR_U32(ctx,16));
        unsigned site = I == 0 ? (ra == 0x2191ACu ? 0u : (ra == 0x42A870u ? 1u : 15u)) :
                        (I == 1 ? cullBSite(ra) : cullCSite(ra));
        uint32_t cam = I == 0 ? (start ? GPR_U32(ctx,5) : GPR_U32(ctx,18)) :
            (I == 1 && !start ? GPR_U32(ctx,18) :
             cullDisplayCamera(rdram,ctx,runtime,start ? (I == 1 ? GPR_U32(ctx,4) : GPR_U32(ctx,5)) : GPR_U32(ctx,18)));
        const float kr = cullKr(rdram,ctx,runtime);
        const bool enabled = cullEnabled(site,kr) && cullCamera(rdram,ctx,runtime,cam);
        const bool scenePlanes = I == 2 && cullEnabled(2,kr) &&
            (site == 10u || cullEnabled(site,kr)) && cullCamera(rdram,ctx,runtime,cam);
        // TEMP(transformers): bounded eligibility evidence for the real display argument layouts.
        if (cullLog && start && I == 1 && site < 15u) {
            static unsigned samples = 0;
            if (samples++ < 12u) {
                const uint32_t d = GPR_U32(ctx,4), v = READ32(d+4u), rawcam = v ? READ32(v+4u) : 0u;
                std::cerr << "[ws:cull:B] site=" << site << " ra=0x" << std::hex << ra << " display=0x" << d
                    << " view=0x" << v << " rawcam=0x" << rawcam << " cam=0x" << cam << " F=0x" << object << std::dec
                    << " enabled=" << enabled << std::endl;
            }
        }
        if (start) {
            if (site < 15u) ++cullCalls[site];
            else if (cullLog && !(I == 0 && ra == 0x2B8A24u)) ++cullUnknown()[(uint64_t(I)<<32)|ra];
            if (I != 0) {
                const uint32_t f = object + (I == 2 ? 0x210u : 0u);
                cullArms().erase(f); // Every fresh outer entry clears a possibly reused stack address, even OFF/gameplay.
                if (enabled) cullArms().emplace(f,CullArm{ctx,kt,kr,site,I == 1 ? 0x20ACBCu : 0x2B8C08u});
            }
        } else if (I != 0 && !enabled) {
            // GAME(transformers): independently dispatched excluded continuations also invalidate reused addresses.
            // A permitted continuation preserves its existing arm; never re-arm after compute has consumed it.
            cullArms().erase(object + (I == 2 ? 0x210u : 0u));
        }
        origCull[I](rdram,ctx,runtime);
        if (ctx->pc != ra) {
            if (site < 15u) ++cullMisses[site];
            if (I == 2 && scenePlanes) ++cullMisses[2];
        } else {
            if (I == 0 && enabled) { cullPlaneFix(rdram,ctx,runtime,object,kr); ++cullFixes[site]; }
            if (scenePlanes) {
                cullPlaneFix(rdram,ctx,runtime,object+0x1D8u,kr); ++cullFixes[2];
            }
        }
        if (I == 2 && start && site < 15u) ++cullCalls[2];
        cullObserve(rdram,ctx,runtime);
    }
    static void cullCompute(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateAspect();
        if (ctx->pc == 0x1B2940u) {
            const uint32_t f = GPR_U32(ctx,4), ra = GPR_U32(ctx,31);
            auto &arms = cullArms(); const auto it = arms.find(f);
            if (it != arms.end()) {
                const CullArm a = it->second;
                arms.erase(it); // Consume before entering guest code, including a guest checkpoint yield.
                if (a.owner == ctx && a.consumeRa == ra && cullOn) {
                    wrF(rdram,ctx,runtime,f+0xA0u,rdF(rdram,ctx,runtime,f+0xA0u)*a.kh);
                    wrF(rdram,ctx,runtime,f+0xA4u,rdF(rdram,ctx,runtime,f+0xA4u)/a.kr);
                    ++cullFixes[a.site];
                    // Retain widened inputs: reflected water/sky recomputations and cell copies inherit them once.
                }
            }
        }
        origCull[3](rdram,ctx,runtime);
    }
    static void installCull(PS2Runtime &rt)
    {
        // Capture the resolved startup setting; menu edits are restart-required.
        cullOn = ps2x::settings::get("display.culling") == "wide";
        ps2x::settings::declareWired("ws2-culling");
        // TEMP(transformers): diagnostics and site bisection.
        const char *sites = std::getenv("PS2X_WIDESCREEN_CULL_SITES");
        if (sites) cullSites = static_cast<uint32_t>(std::strtoul(sites,nullptr,16));
        const char *log = std::getenv("PS2X_WS_CULL_LOG"); cullLog = log && log[0] == '1';
        if (!(force || kt != 1.0f)) return;
        constexpr uint32_t entries[4] = {0x1F4B00u,0x20AB70u,0x2B89C0u,0x1B2940u};
        constexpr uint32_t ends[4] = {0x1F4C90u,0x20ACF0u,0x2B8F90u,0x1B2B50u};
        constexpr PS2Runtime::RecompiledFunction hooks[4] = {&cullOuter<0>,&cullOuter<1>,&cullOuter<2>,&cullCompute};
        for (unsigned i=0;i<4;++i) {
            if (!rt.hasFunction(entries[i])) continue;
            origCull[i] = rt.lookupFunction(entries[i]);
            for (uint32_t pc=entries[i];pc<ends[i];pc+=4u)
                if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origCull[i]) rt.replaceFunction(pc,hooks[i]);
        }
    }


    static void perspective(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateAspect();
        float perspectiveKr = 1.0f;
        const uint32_t ra = GPR_U32(ctx, 31);
        const float nearZ = ctx->f[12];
        const float farZ = ctx->f[13];
        const float r = ctx->f[14];
        const float fov = ctx->f[15];
        if (kt != 1.0f && fov > 0.0f && fov < 3.1f && r > 0.0f)
        {
            const float kr = displayK / (READ8(kWideFlag) ? kGameWideFactor : 1.0f);
            ctx->f[15] = 2.0f * std::atan(kt * std::tan(0.5f * fov));
            ctx->f[14] = r / kr;
            perspectiveKr = kr;
            // GAME(transformers): 20C838 copies saved f21 into f14. 20CA00 saves/restores
            // f21 across every yield; 20C858 therefore stores the corrected ratio itself.
            if (yieldFix && ra == 0x0020C848u) {
                ctx->f[21] = r / kr;
                if (yieldLog) ++viewportFixes;
            }
        }
        // TEMP(transformers): classify the next projection consumer without any guest writes.
        if (!yieldFix) legacyViewportKr = perspectiveKr;
        if (yieldLog) { observedRawR = r; observedKr = perspectiveKr; observedPerspectiveRa = ra; }
        origPerspective(rdram, ctx, runtime);
        ++perspCalls;
        static uint32_t lastKey[3] = {};
        uint32_t key[3] = {ra, 0u, 0u};
        std::memcpy(&key[1], &r, 4);
        std::memcpy(&key[2], &fov, 4);
        const bool changed = std::memcmp(key, lastKey, sizeof(key)) != 0;
        std::memcpy(lastKey, key, sizeof(key));
        if (logBudget > 0 && (changed || (perspCalls % 4096u) == 0u)) // TEMP(transformers): log on change
        {
            --logBudget;
            std::cerr << "[ws:persp] n=" << perspCalls << " ra=0x" << std::hex << ra << std::dec << " near=" << nearZ
                      << " far=" << farZ << " r=" << r << " hfov_deg=" << fov * 57.29578f << " kt=" << kt
                      << " kr=" << perspectiveKr
                      << " tanH=" << rdF(rdram, ctx, runtime, kTanH) << " wide=" << static_cast<int>(READ8(kWideFlag))
                      << std::endl;
        }
    }

    static inline PS2Runtime::RecompiledFunction origBuildP = nullptr; // TEMP(transformers): 0x221E70 log wrapper
    static void buildP(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // TEMP(transformers): inspect at entry, BEFORE the guest consumes r.
        if (yieldLog) {
            ++buildCalls;
            const float r = rdF(rdram,ctx,runtime,kAspectR);
            if (READ8(kOrtho) == 0u && observedKr != 1.0f && observedRawR > 0.0f) {
                if (std::abs(r-observedRawR) < 0.000001f) ++buildRaw;
                else if (std::abs(r-observedRawR/observedKr) < 0.000001f) ++buildCorrect;
                else {
                    ++buildOther;
                    if (observedPerspectiveRa==0x0020B5D4u) ++buildOtherCut;
                    else ++buildOtherUnexpected;
                }
            }
            yieldObserve(runtime);
        }
        origBuildP(rdram, ctx, runtime);
        static float last[4] = {};
        const float cur[4] = {rdF(rdram, ctx, runtime, 0x700006A0u), rdF(rdram, ctx, runtime, 0x700006B4u),
                              rdF(rdram, ctx, runtime, 0x700006E0u), rdF(rdram, ctx, runtime, 0x700006F4u)};
        if (logBudget > 0 && std::memcmp(cur, last, sizeof(cur)) != 0)
        {
            --logBudget;
            std::cerr << "[ws:P] P1xx=" << cur[0] << " P1yy=" << cur[1]
                      << " CLIPxx=" << cur[2] << " CLIPyy=" << cur[3] << " tanH=" << rdF(rdram, ctx, runtime, kTanH)
                      << " r=" << rdF(rdram, ctx, runtime, kAspectR) << " sx=" << rdF(rdram, ctx, runtime, kGp - 0x42E8u)
                      << " sy=" << rdF(rdram, ctx, runtime, kGp - 0x42E4u) << " ortho=" << static_cast<int>(READ8(kOrtho))
                      << " wide=" << static_cast<int>(READ8(kWideFlag)) << std::endl;
        }
        std::memcpy(last, cur, sizeof(cur));
    }

    // (3) HUD / 2D at 4:3 proportions: the renderer's 2D primitive builders (renderer vtable 0x6B4450 slots 0x70 =
    //     fn_00257610 (2 vertices a1,a2), 0x74 = fn_00257160 (triangle a1,a2,a3), 0x78 = fn_002578F0 (rect a1,a2),
    //     0x7C = fn_00256280 (a2 = vertex array, stride 0x18, a3 = count); vertex x/y floats at +0/+4) map virtual
    //     2D coordinates to GS XY with x = gp-0x4204 + x * gp-0x420C (set by sub_0025FBB0 from the display mode;
    //     sub_0025FD60 is the scalar getter; the 0x7C type-10 emitter sub_00257070 adds gp-0x41FC to pixel x). Before
    //     one of them runs, the primitive's vertex x values are squeezed by 1/kt around the screen centre (restored
    //     right after), so HUD elements, captions and menu text keep their 4:3 shape inside the centre 4:3 area.
    //     Effect passes are recognised by their geometry (see hud<>) and left alone. Only in frames with VU1
    //     geometry (3D scenes: hub, globe, levels); boot/legal screens, FMVs and pure-2D screens keep the original
    //     2D (at 16:10 squeezing the pre-movie boot screens stalled the boot sequence: run ws_1610a/b/c). The
    //     2D transform globals themselves are never modified (sub_00206A50 / sub_00264DB0 / sub_00265370 use the
    //     offset as the screen origin for full-screen passes). PS2X_WIDESCREEN_HUD=0 disables.
    static constexpr uint32_t kHudScaleX = kGp - 0x420Cu;
    static constexpr uint32_t kHudScaleY = kGp - 0x4208u;
    static constexpr uint32_t kHalfWidthRef = kGp - 0x42D8u;  // 2048 - width/2
    static constexpr uint32_t kHalfHeightRef = kGp - 0x42D4u; // 2048 - height/2
    static inline bool hudOn = false;
    static inline bool hudCentreOnly = false;
    // GAME(transformers): fn_00313F30 draws the holographic menu's 2D surface.
    // Its bars and glyphs become one 3D panel; the perspective hook handles its aspect.
    // Applying screen HUD anchors here moves short left-column labels off their bars.
    static inline unsigned menuSurfaceDepth = 0;
    static inline PS2Runtime::RecompiledFunction origMenuSurface = nullptr;
    static inline PS2Runtime::RecompiledFunction origFontDraw = nullptr;
    // BEGIN ws3 tracker extraction
    class PanelTracker
    {
        struct Frame
        {
            const void *owner, *context;
            uint32_t entrySp, size, raOffset, expectedRa;
        };
        std::vector<Frame> frames;
    public:
        void leave(const void *owner, const void *context, uint32_t sp)
        {
            frames.erase(std::remove_if(frames.begin(), frames.end(), [&](const Frame &f) {
                return f.owner == owner && f.context == context && sp >= f.entrySp;
            }), frames.end());
        }
        void enter(const void *owner, const void *context, uint32_t sp,
                   uint32_t size, uint32_t raOffset, uint32_t expectedRa)
        {
            leave(owner, context, sp);
            if (sp >= 0x02000000u || sp < size || size == 0u || raOffset + 4u > size) return;
            frames.push_back({owner, context, sp, size, raOffset, expectedRa});
        }
        template <typename Read>
        bool active(const void *owner, const void *context, uint32_t sp, Read read) const
        {
            for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            {
                const Frame &f = *it;
                if (f.owner == owner && f.context == context && sp < f.entrySp &&
                    f.entrySp - sp <= 0x4000u && read(f.entrySp - f.size + f.raOffset) == f.expectedRa)
                    return true;
            }
            return false;
        }
    };
    // END ws3 tracker extraction
    static PanelTracker &panelTracker()
    {
        static PanelTracker tracker; // EE thread only, no worker access.
        return tracker;
    }
    static PanelTracker &surfaceTracker()
    {
        static PanelTracker tracker; // EE dispatcher only.
        return tracker;
    }
    static void menuSurface(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        struct Scope
        {
            Scope() { ++menuSurfaceDepth; }
            ~Scope() { --menuSurfaceDepth; }
        } scope;
        if (yieldFix && ctx->pc == 0x00313F30u)
            surfaceTracker().enter(runtime,ctx,GPR_U32(ctx,29),0x610u,0xA0u,GPR_U32(ctx,31));
        origMenuSurface(rdram, ctx, runtime);
        if (yieldFix) surfaceTracker().leave(runtime,ctx,GPR_U32(ctx,29));
    }
    static void fontDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // GAME(transformers): gate names are drawn into the scrolling sign surface by fn_004F0B50.
        // Only these two text calls belong to the sign; the same function also draws ordinary UI statistics.
        const uint32_t parent = ctx->pc == 0x00233330u ? GPR_U32(ctx, 31) : READ32(GPR_U32(ctx, 29) + 0x50u);
        const bool surface = parent == 0x004F0DA4u || parent == 0x004F0DFCu || parent == 0x004EDFFCu || parent == 0x004EE03Cu;
        struct Scope
        {
            bool active;
            explicit Scope(bool on) : active(on) { if (active) ++menuSurfaceDepth; }
            ~Scope() { if (active) --menuSurfaceDepth; }
        } scope(surface);
        if (yieldFix && surface && ctx->pc == 0x00233330u)
            surfaceTracker().enter(runtime,ctx,GPR_U32(ctx,29),0x80u,0x50u,parent);
        origFontDraw(rdram, ctx, runtime);
        if (yieldFix) surfaceTracker().leave(runtime,ctx,GPR_U32(ctx,29));
    }
    // GAME(transformers): information pages are a single screen-space layout.
    // Their nine-slice panels have x=0 edges and tall 32-pixel strips; treating
    // those as post-processing while anchoring the interior tears the panel.
    // Use one centred transform for frames, fills and text in the whole page.
    static inline unsigned screenPanelDepth = 0;
    struct PanelSpec { uint32_t start, end, size, raOffset; };
    static constexpr PanelSpec panelSpecs[8] = {
        {0x005A9F10u, 0x005AA5E0u, 0x170u, 0x50u},
        {0x005A91F0u, 0x005A9A70u, 0x190u, 0x60u},
        {0x005AA7A0u, 0x005AACF0u, 0xD0u, 0x40u},
        {0x005AB410u, 0x005ABDB0u, 0x190u, 0x50u},
        {0x005AC4A0u, 0x005AC7A0u, 0x150u, 0x70u},
        {0x005AC7A0u, 0x005ACBC0u, 0x1D0u, 0x10u},
        {0x005ACBC0u, 0x005ADA00u, 0x300u, 0xA0u},
        // GAME(transformers): composite pickup Render, including inherited bars
        // and its later disabled-label overlay. Same ws3 layout for every piece.
        {0x00310220u, 0x00310570u, 0x100u, 0x50u}
    };
    // GAME(transformers): a host call can return while the guest is yielded in
    // a child painter. Track the live guest stack frame as well as host depth.
    // Saved RA validation rejects reused stack storage and other contexts.
    static inline PS2Runtime::RecompiledFunction origScreenPanel[8] = {};
    template <int I>
    static void screenPanel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PanelSpec &spec = panelSpecs[I];
        if (ctx->pc == spec.start)
            panelTracker().enter(runtime, ctx, GPR_U32(ctx, 29), spec.size, spec.raOffset, GPR_U32(ctx, 31));
        struct Scope
        {
            Scope() { ++screenPanelDepth; }
            ~Scope() { --screenPanelDepth; }
        } scope;
        origScreenPanel[I](rdram, ctx, runtime);
        panelTracker().leave(runtime, ctx, GPR_U32(ctx, 29));
    }
    // GAME(transformers): health wedges, armor arc and emblem form one widget.
    // Keep this tracker separate from the existing panel/surface trackers.
    static PanelTracker &widgetTracker() { static PanelTracker tracker; return tracker; }
    static inline bool widgetLayout = true;
    static inline unsigned gaugeDepth = 0;
    static inline PS2Runtime::RecompiledFunction origGauge = nullptr;
    // TEMP(transformers): normalized health:armor override, confined to host Render slices.
    struct GaugeCall {
        PS2Runtime *owner; R5900Context *context;
        uint32_t sp, ra, component;
    };
    static std::vector<GaugeCall> &gaugeCalls() { static std::vector<GaugeCall> calls; return calls; }
    static const std::pair<float,float> &gaugeTest()
    {
        static const std::pair<float,float> test=[] {
            float h=-1.0f,a=-1.0f; char tail=0;
            const char *e=std::getenv("PS2X_TEST_GAUGE");
            if (!e || std::sscanf(e,"%f:%f%c",&h,&a,&tail)!=2 ||
                !(h>=0.0f && h<=1.0f && a>=0.0f && a<=1.0f)) return std::make_pair(-1.0f,-1.0f);
            return std::make_pair(h,a);
        }();
        return test;
    }
    static inline PS2Runtime::RecompiledFunction origGaugeMesh=nullptr;
    static void gaugeMesh(uint8_t *rdram,R5900Context *ctx,PS2Runtime *runtime)
    {
        // TEMP(transformers): permit independent damaged fill plus armor in the test sweep.
        // The mesh copies f13 into saved f20 before any child call/yield.
        if (ctx->pc==0x00438050u && gaugeTest().first>=0.0f &&
            (GPR_U32(ctx,31)==0x00302EF4u || GPR_U32(ctx,31)==0x00302F10u) &&
            (gaugeDepth || widgetTracker().active(runtime,ctx,GPR_U32(ctx,29),
                [&](uint32_t addr){return READ32(addr);}))) ctx->f[13]=gaugeTest().first;
        origGaugeMesh(rdram,ctx,runtime);
    }
    static void gaugeRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == 0x00302B90u;
        const uint32_t sp = GPR_U32(ctx,29);
        const uint32_t returnSp = entry ? sp : sp+0x120u;
        const uint32_t ra = entry ? GPR_U32(ctx,31) : READ32(sp+0x60u);
        auto &calls = gaugeCalls();
        if (entry) {
            widgetTracker().enter(runtime,ctx,sp,0x120u,0x60u,ra);
            calls.erase(std::remove_if(calls.begin(),calls.end(),[&](const GaugeCall &c) {
                return c.owner==runtime && c.context==ctx && sp>=c.sp;
            }),calls.end());
            calls.push_back({runtime,ctx,sp,ra,GPR_U32(ctx,4)});
        }
        uint32_t component=0;
        for (auto it=calls.rbegin();it!=calls.rend();++it)
            if (it->owner==runtime && it->context==ctx && it->sp==returnSp && it->ra==ra) {
                component=it->component; break;
            }
        const auto &test=gaugeTest();
        const bool overrideOn=test.first>=0.0f && component && component<0x02000000u-0x24u;
        uint32_t savedHealth=0,savedMax=0;
        if (overrideOn) {
            savedHealth=READ32(component+0x18u); savedMax=READ32(component+0x20u);
            // Render computes fill=health/max, armor=(health-max)*gp[-75E8].
            // Test-only mesh argument override below makes the fill independent.
            const float scale=rdF(rdram,ctx,runtime,kGp-0x75E8u);
            const float base=rdF(rdram,ctx,runtime,component+0x20u);
            const float health=test.second>0.0f && scale>0.0f ? base+test.second/scale : base*test.first;
            wrF(rdram,ctx,runtime,component+0x18u,health);
            // Armor implies full health in the game's model. Keep the actual base maximum.
        }
        struct Scope { Scope(){++gaugeDepth;} ~Scope(){--gaugeDepth;} } scope;
        origGauge(rdram,ctx,runtime);
        // Restore on every host yield too: scheduler/game logic never sees test fields.
        if (overrideOn) { WRITE32(component+0x18u,savedHealth); WRITE32(component+0x20u,savedMax); }
        if (ctx->pc==ra && GPR_U32(ctx,29)==returnSp) {
            widgetTracker().leave(runtime,ctx,returnSp);
            calls.erase(std::remove_if(calls.begin(),calls.end(),[&](const GaugeCall &c) {
                return c.owner==runtime && c.context==ctx && c.sp==returnSp && c.ra==ra;
            }),calls.end());
        }
    }

    // GAME(transformers): bosses share taBossHealthComponent, not the Autobot Render.
    // Choose the anchor from BOTH complete, immutable meshes, never a clipped fill.
    struct BossCall {
        PS2Runtime *owner; R5900Context *context;
        uint32_t sp, ra; int ax, ay;
    };
    static std::vector<BossCall> &bossCalls() { static std::vector<BossCall> v; return v; }
    static inline const BossCall *bossSlice = nullptr;
    static inline PS2Runtime::RecompiledFunction origBoss = nullptr;
    static const BossCall *activeBoss(uint8_t *rdram,R5900Context *ctx,PS2Runtime *runtime,uint32_t sp)
    {
        if (bossSlice && bossSlice->owner==runtime && bossSlice->context==ctx) return bossSlice;
        for (auto it=bossCalls().rbegin();it!=bossCalls().rend();++it)
            if (it->owner==runtime && it->context==ctx && sp<=it->sp-0x90u &&
                READ32(it->sp-0x90u+0x40u)==it->ra) return &*it;
        return nullptr;
    }
    static void bossRender(uint8_t *rdram,R5900Context *ctx,PS2Runtime *runtime)
    {
        updateAspect();
        if (!hudOn || kt==1.0f) { origBoss(rdram,ctx,runtime); return; }
        const bool entry=ctx->pc==0x00300E90u;
        const uint32_t sp=GPR_U32(ctx,29),retSp=entry?sp:sp+0x90u;
        const uint32_t ra=entry?GPR_U32(ctx,31):READ32(sp+0x40u);
        auto &calls=bossCalls();
        if (entry) {
            calls.erase(std::remove_if(calls.begin(),calls.end(),[&](const BossCall &c) {
                return c.owner==runtime && c.context==ctx && sp>=c.sp;
            }),calls.end());
            float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f;
            const uint32_t component=GPR_U32(ctx,4);
            for (uint32_t off:{0x38u,0x40u}) {
                const uint32_t count=READ32(component+off),mesh=READ32(component+off+4u);
                if (count!=5u || !mesh || mesh>=0x02000000u-10u*0x18u) continue;
                for (uint32_t k=0;k<count*2u;++k) {
                    const float x=rdF(rdram,ctx,runtime,mesh+k*0x18u);
                    const float y=rdF(rdram,ctx,runtime,mesh+k*0x18u+4u);
                    if (!(x>-4096.0f && x<8192.0f && y>-4096.0f && y<8192.0f)) continue;
                    x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);
                }
            }
            const float pxX=rdF(rdram,ctx,runtime,kHudScaleX)/16.0f;
            const float pxY=rdF(rdram,ctx,runtime,kHudScaleY)/16.0f;
            const float hw=2048.0f-rdF(rdram,ctx,runtime,kHalfWidthRef);
            const float hh=2048.0f-rdF(rdram,ctx,runtime,kHalfHeightRef);
            const float cx=(x0+x1)*0.5f*pxX,cy=(y0+y1)*0.5f*pxY;
            const int ax=x1>=x0?(cx<0.5f*hw?-1:(cx>1.5f*hw?1:0)):0;
            const int ay=y1>=y0?(cy<0.5f*hh?-1:(cy>1.5f*hh?1:0)):0;
            calls.push_back({runtime,ctx,sp,ra,ax,ay});
        }
        BossCall slice{}; bool found=false;
        for (auto it=calls.rbegin();it!=calls.rend();++it)
            if (it->owner==runtime && it->context==ctx && it->sp==retSp && it->ra==ra) {
                slice=*it;found=true;break;
            }
        const BossCall *previous=bossSlice;bossSlice=found?&slice:nullptr;
        origBoss(rdram,ctx,runtime);
        bossSlice=previous;
        if (ctx->pc==ra && GPR_U32(ctx,29)==retSp)
            calls.erase(std::remove_if(calls.begin(),calls.end(),[&](const BossCall &c) {
                return c.owner==runtime && c.context==ctx && c.sp==retSp && c.ra==ra;
            }),calls.end());
    }

    // TEMP(transformers): per-call geometry observer; also works with widget layout OFF.
    static void hudMap(uint32_t widget,uint32_t ra,bool skipped,int ax,int ay,
                       float xmin,float xmax,float ymin,float ymax,
                       float oxmin,float oxmax,float oymin,float oymax,float error)
    {
        static const std::pair<long long,long long> window=[] {
            const char *e=std::getenv("PS2X_WS_HUDMAP");
            if (!e) return std::make_pair(-1ll,-1ll);
            const char *c=std::strchr(e,':');
            return std::make_pair(std::atoll(e),c?std::atoll(c+1):std::atoll(e));
        }();
        const auto vs=static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        if (window.first<0 || vs<window.first || vs>window.second) return;
        // TEMP(transformers): format once; one stderr write avoids per-character pipe backpressure.
        std::ostringstream record;
        record << "[ws:hudmap] vs=" << vs << " widget=0x" << std::hex << widget
                  << " ra=0x" << ra << std::dec << " input=" << xmin << "," << xmax << "," << ymin << "," << ymax
                  << " skipped=" << skipped << " ax=" << ax << " ay=" << ay
                  << " output=" << oxmin << "," << oxmax << "," << oymin << "," << oymax
                  << " error=" << error << std::endl;
        const std::string text=record.str();
        std::fwrite(text.data(),1,text.size(),stderr);
    }
    struct HudGroup
    {
        uint64_t win = ~0ull;
        float cminx = 1e30f, cmaxx = -1e30f, pminx = 0.0f, pmaxx = 0.0f;
        float cminy = 1e30f, cmaxy = -1e30f, pminy = 0.0f, pmaxy = 0.0f;
        bool phave = false;
    };
    static std::unordered_map<uint64_t, HudGroup> &hudGroups()
    {
        static std::unordered_map<uint64_t, HudGroup> groups; // EE thread only
        return groups;
    }
    static inline PS2Runtime::RecompiledFunction origHud[4] = {};
    // GAME(transformers): transformed vertices belong to a LIVE guest builder, not a host call.
    struct HudVertex { uint32_t addr; float x,y,nx,ny; };
    struct HudPending {
        PS2Runtime *owner; R5900Context *ctx; unsigned builder; uint32_t sp,ra;
        bool yChanged; std::vector<HudVertex> vertices;
    };
    static std::vector<HudPending> &hudPending() { static std::vector<HudPending> v; return v; }
    template<int I> static void hudFinish(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         uint32_t sp, uint32_t ra)
    {
        if (ctx->pc != ra || GPR_U32(ctx,29) != sp) { if (yieldLog) ++hudYields; return; }
        auto &pending = hudPending();
        for (size_t i=0; i<pending.size(); ++i) {
            const auto &p = pending[i];
            if (p.owner != runtime || p.ctx != ctx || p.builder != I || p.sp != sp || p.ra != ra) continue;
            for (const auto &v : p.vertices) {
                const float now = rdF(rdram,ctx,runtime,v.addr);
                if (std::memcmp(&now,&v.nx,4)==0) wrF(rdram,ctx,runtime,v.addr,v.x);
                if (p.yChanged) {
                    const float nowY = rdF(rdram,ctx,runtime,v.addr+4u);
                    if (std::memcmp(&nowY,&v.ny,4)==0) wrF(rdram,ctx,runtime,v.addr+4u,v.y);
                }
            }
            pending.erase(pending.begin()+i);
            if (yieldLog) ++hudRestores;
            return;
        }
    }
    template <int I>
    static void hud(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t entries[4] = {0x257610u,0x257160u,0x2578F0u,0x256280u};
        constexpr uint32_t sizes[4] = {0x90u,0xC0u,0x10u,0x300u};
        constexpr uint32_t raOffsets[4] = {0x50u,0x60u,0u,0x50u};
        if (ctx->pc != entries[I]) {
            // Every continuation retains transformed data; the leaf rectangle keeps ra live.
            const uint32_t sp = GPR_U32(ctx,29)+sizes[I];
            const uint32_t ra = I == 2 ? GPR_U32(ctx,31) : READ32(GPR_U32(ctx,29)+raOffsets[I]);
            origHud[I](rdram,ctx,runtime);
            if (yieldFix) hudFinish<I>(rdram,ctx,runtime,sp,ra);
            return;
        }
        updateAspect();
        const uint32_t entrySp = GPR_U32(ctx,29), entryRa = GPR_U32(ctx,31);
        if (yieldFix) surfaceTracker().leave(runtime,ctx,entrySp);
        const bool inSurface = yieldFix && surfaceTracker().active(runtime,ctx,entrySp,
            [&](uint32_t addr) { return READ32(addr); });
        // GAME(transformers): preserve the original narrow/off early return.
        if (!hudOn || kt == 1.0f)
        {
            origHud[I](rdram, ctx, runtime);
            return;
        }
        panelTracker().leave(runtime, ctx, GPR_U32(ctx, 29));
        const bool inPanel = screenPanelDepth != 0u || panelTracker().active(runtime, ctx, GPR_U32(ctx, 29),
            [&](uint32_t addr) { return READ32(addr); });
        // GAME(transformers): a tagged screen-space composite can call the shared
        // menu painter directly. It is not the holographic offscreen surface.
        // Its inherited bars and later disabled text must use the same transform.
        if ((!inPanel && (menuSurfaceDepth != 0u || inSurface)) || !ps2WidescreenVu1Frame(runtime))
        {
            origHud[I](rdram, ctx, runtime);
            return;
        }
        widgetTracker().leave(runtime,ctx,entrySp);
        const bool gaugeActive = gaugeDepth != 0u || widgetTracker().active(runtime,ctx,entrySp,
            [&](uint32_t addr) { return READ32(addr); });
        const BossCall *boss=activeBoss(rdram,ctx,runtime,entrySp);
        const bool bossActive=boss!=nullptr;
        static const bool bossLayout=[] { const char *e=std::getenv("PS2X_WS_BOSS2");return !(e && e[0]=='0'); }(); // TEMP(transformers): OLD control
        const bool inGauge = widgetLayout && (gaugeActive || (bossLayout && bossActive));
        const uint32_t widgetTag=bossActive?0x300E90u:(gaugeActive?0x302B90u:0u);
        uint32_t addrs[512];
        uint32_t n = 0;
        auto add = [&](uint32_t v) {
            for (uint32_t k = 0; k < n; ++k)
                if (addrs[k] == v)
                    return;
            if (n < 512u)
                addrs[n++] = v;
        };
        if (I == 0 || I == 2)
        {
            add(GPR_U32(ctx, 5));
            add(GPR_U32(ctx, 6));
        }
        else if (I == 1)
        {
            add(GPR_U32(ctx, 5));
            add(GPR_U32(ctx, 6));
            add(GPR_U32(ctx, 7));
        }
        else
        {
            const uint32_t cnt = std::min<uint32_t>(GPR_U32(ctx, 7), 512u);
            for (uint32_t k = 0; k < cnt; ++k)
                addrs[n++] = GPR_U32(ctx, 6) + k * 0x18u;
        }
        float xs[512], ys[512];
        float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
        bool sane = true;
        for (uint32_t k = 0; k < n; ++k)
        {
            if ((addrs[k] & 3u) != 0u || (addrs[k] & 0x1FFFFFFFu) >= 0x02000000u)
            {
                sane = false; // not a RAM vertex (unknown argument layout for this primitive type)
                break;
            }
            xs[k] = rdF(rdram, ctx, runtime, addrs[k]);
            const float y = ys[k] = rdF(rdram, ctx, runtime, addrs[k] + 4u);
            if (!(xs[k] > -4096.0f && xs[k] < 8192.0f && y > -4096.0f && y < 8192.0f))
            {
                sane = false; // not screen coordinates: leave the call alone
                break;
            }
            xmin = std::min(xmin, xs[k]);
            xmax = std::max(xmax, xs[k]);
            ymin = std::min(ymin, y);
            ymax = std::max(ymax, y);
        }
        const float sx = rdF(rdram, ctx, runtime, kHudScaleX);
        const float sy = rdF(rdram, ctx, runtime, kHudScaleY);
        const float halfW = 2048.0f - rdF(rdram, ctx, runtime, kHalfWidthRef);
        const float halfH = 2048.0f - rdF(rdram, ctx, runtime, kHalfHeightRef);
        const float pxX = sx * (1.0f / 16.0f), pxY = sy * (1.0f / 16.0f);
        const float spanPx = (xmax - xmin) * pxX;
        const float spanPy = (ymax - ymin) * pxY;
        // Leave alone: full-width prims (fades, full-screen copies); anything touching the left or top edge or lying
        // below the screen; tall thin strips. The post-processing passes draw strips/tiles that start at 0 in small
        // offscreen targets or cover double-height buffers (runtime log PS2X_WS_HUDLOG: callers 0x264984 8-px strips
        // over y 0-896, 0x205FE8 16/21-px x 128 strips, 0x206400 128x64 tiles, 0x2033E8 lines); squeezing those
        // breaks the effect. HUD elements sit inside the screen (hexagons x31-55 y32-80, frames x48-176 y36-100,
        // gauge x28-113 y356-418, bar fill x356-457 y42-69 in the 512x448 space).
        // GAME(transformers): the gauge is one widget, including wedges crossing the screen edges.
        // Reject only invalid geometry or a bbox entirely outside the screen while its tag is active.
        const bool invalid = !sane || n == 0u || !(xmax >= xmin) ||
                             halfW <= 0.0f || halfH <= 0.0f || pxX <= 0.0f || pxY <= 0.0f;
        const bool skip = inGauge
            ? invalid || xmax * pxX < 0.0f || xmin * pxX > 2.0f * halfW ||
                         ymax * pxY < 0.0f || ymin * pxY > 2.0f * halfH
            : !sane || n == 0u || !(xmax >= xmin) || halfW <= 0.0f || pxX <= 0.0f || spanPx >= 1.8f * halfW ||
              ymax * pxY > 2.0f * halfH + 2.0f ||
              (!inPanel && (xmin <= 0.5f || ymin <= 0.5f ||
               (spanPy >= 96.0f && spanPx <= 32.0f)));
        { // TEMP(transformers): PS2X_WS_HUDLOG=<vsFrom>:<vsTo> - one line per 2D builder call
            static long long s_from = -2, s_to = -1;
            if (s_from == -2)
            {
                s_from = -1;
                if (const char *e = std::getenv("PS2X_WS_HUDLOG"))
                {
                    s_from = std::atoll(e);
                    const char *c = std::strchr(e, ':');
                    s_to = c ? std::atoll(c + 1) : s_from;
                }
            }
            const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
            if (s_from >= 0 && vsNow >= s_from && vsNow <= s_to)
            {
                float lymin = 1e30f, lymax = -1e30f;
                auto accy = [&](uint32_t v) { const float y = rdF(rdram, ctx, runtime, v + 4u); lymin = std::min(lymin, y); lymax = std::max(lymax, y); };
                if (I == 0 || I == 2) { accy(GPR_U32(ctx, 5)); accy(GPR_U32(ctx, 6)); }
                else if (I == 1) { accy(GPR_U32(ctx, 5)); accy(GPR_U32(ctx, 6)); accy(GPR_U32(ctx, 7)); }
                else { const uint32_t n = std::min<uint32_t>(GPR_U32(ctx, 7), 512u); for (uint32_t i = 0; i < n; ++i) accy(GPR_U32(ctx, 6) + i * 0x18u); }
                const uint32_t sp = GPR_U32(ctx, 29);
                // TEMP(transformers): format once; one stderr write avoids per-character pipe backpressure.
                std::ostringstream record;
                record << "[ws:hud] vs=" << vsNow << " slot=" << I << " ra=0x" << std::hex << GPR_U32(ctx, 31) << " ra2=0x"
                          << READ32(sp + 0u) << std::dec << " x=" << xmin << ".." << xmax << " y=" << lymin << ".." << lymax
                          << " spanPx=" << spanPx << " n=" << (I == 3 ? GPR_U32(ctx, 7) : 0u) << " a1=" << GPR_U32(ctx, 5) << std::endl;
                const std::string text=record.str();
                std::fwrite(text.data(),1,text.size(),stderr);
            }
        }
        if (skip)
        {
            hudMap(widgetTag,entryRa,true,0,0,xmin,xmax,ymin,ymax,xmin,xmax,ymin,ymax,0.0f);
            origHud[I](rdram, ctx, runtime);
            return;
        }
        // Squeeze the primitive's own vertices by 1/kt (x units: pixels * 16 / sx; the type-10 strip emitter
        // sub_00257070 uses raw pixels + gp-0x41FC, same units when sx == 16) and restore them afterwards; the
        // caller's vertex data (often persistent HUD meshes) is left exactly as it was.
        // Anchor per widget, not per primitive: primitives are grouped by (caller ra, 64-px row band, screen half);
        // a group whose bbox (accumulated over the previous 8-vsync window) is centred in the left quarter keeps its
        // distance to the LEFT screen edge (x/kt), right quarter -> RIGHT edge, otherwise the group is squeezed
        // around the centre. Corner widgets (frames x48-176 / x338-466, fills, hexagons, gauge x17-135) move to the
        // corners as a whole; a centred caption's glyphs fall into centre groups even when single glyphs sit in an
        // outer quarter (per-primitive anchoring split "Choose an Autobot" in run ws_169l). PS2X_WIDESCREEN_HUD=
        // center squeezes everything around the centre (HUD inside the 4:3 area).
        const float vw = 2.0f * halfW / pxX; // virtual screen width
        int anchor = 0, anchorY = 0;
        if (inGauge) { anchor=bossActive?boss->ax:-1; anchorY=bossActive?boss->ay:1; }
        else if (!hudCentreOnly && !inPanel)
        {
            const float cxPx = 0.5f * (xmin + xmax) * pxX;
            const float cyPx = 0.5f * (ymin + ymax) * pxY;
            const uint64_t key = (static_cast<uint64_t>(GPR_U32(ctx, 31)) << 16) |
                                 (static_cast<uint64_t>(std::max(0, std::min(255, static_cast<int>(cyPx / 64.0f)))) << 1) |
                                 (cxPx < halfW ? 0u : 1u);
            const uint64_t win = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) / 8u; // guest EE time
            HudGroup &g = hudGroups()[key];
            if (g.win != win)
            {
                if (g.cmaxx >= g.cminx)
                {
                    g.pminx = g.cminx;
                    g.pmaxx = g.cmaxx;
                    g.pminy = g.cminy;
                    g.pmaxy = g.cmaxy;
                    g.phave = true;
                }
                g.cminx = 1e30f;
                g.cmaxx = -1e30f;
                g.cminy = 1e30f;
                g.cmaxy = -1e30f;
                g.win = win;
            }
            g.cminx = std::min(g.cminx, xmin * pxX);
            g.cmaxx = std::max(g.cmaxx, xmax * pxX);
            g.cminy = std::min(g.cminy, ymin * pxY);
            g.cmaxy = std::max(g.cmaxy, ymax * pxY);
            const float gc = g.phave ? 0.5f * (g.pminx + g.pmaxx) : 0.5f * (g.cminx + g.cmaxx);
            anchor = gc < 0.5f * halfW ? -1 : (gc > 1.5f * halfW ? 1 : 0);
            const float gy = g.phave ? 0.5f * (g.pminy + g.pmaxy) : 0.5f * (g.cminy + g.cmaxy);
            anchorY = gy < 0.5f * halfH ? -1 : (gy > 1.5f * halfH ? 1 : 0);
        }
        const float vc = 0.5f * vw;
        const float vh = 2.0f * halfH / pxY;
        float nxs[512], nys[512];
        for (uint32_t k = 0; k < n; ++k)
        {
            const float x = xs[k];
            nxs[k] = anchor < 0 ? x / kt : (anchor > 0 ? vw - (vw - x) / kt : vc + (x - vc) / kt);
            wrF(rdram, ctx, runtime, addrs[k], nxs[k]);
            // GAME(transformers): retain widget size at the 16:9 horizontal scale, anchored vertically.
            const float y = ys[k];
            nys[k] = anchorY < 0 ? y * hudY : (anchorY > 0 ? vh - (vh - y) * hudY : 0.5f * vh + (y - 0.5f * vh) * hudY);
            if (hudY != 1.0f) wrF(rdram, ctx, runtime, addrs[k] + 4u, nys[k]);
        }
        static const bool wantMap=std::getenv("PS2X_WS_HUDMAP")!=nullptr;
        if (wantMap) { // TEMP(transformers): reread actual builder input after writes.
            float oxmin=1e30f,oxmax=-1e30f,oymin=1e30f,oymax=-1e30f,mapError=0.0f;
            for (uint32_t k=0;k<n;++k) {
                const float ox=rdF(rdram,ctx,runtime,addrs[k]),oy=rdF(rdram,ctx,runtime,addrs[k]+4u);
                oxmin=std::min(oxmin,ox); oxmax=std::max(oxmax,ox);
                oymin=std::min(oymin,oy); oymax=std::max(oymax,oy);
                if (gaugeActive || bossActive) {
                    const int ax=bossActive?boss->ax:-1,ay=bossActive?boss->ay:1;
                    const float ex=ax<0?xs[k]/kt:(ax>0?vw-(vw-xs[k])/kt:vc+(xs[k]-vc)/kt);
                    const float ey=ay<0?ys[k]*hudY:(ay>0?vh-(vh-ys[k])*hudY:0.5f*vh+(ys[k]-0.5f*vh)*hudY);
                    mapError=std::max(mapError,std::max(std::fabs(ox-ex),std::fabs(oy-ey)));
                }
            }
            hudMap(widgetTag,entryRa,false,anchor,anchorY,
                xmin,xmax,ymin,ymax,oxmin,oxmax,oymin,oymax,mapError);
        }
        if (yieldFix) {
            HudPending pending{runtime,ctx,I,entrySp,entryRa,hudY != 1.0f,{}};
            for (uint32_t k=0; k<n; ++k)
                pending.vertices.push_back({addrs[k],xs[k],ys[k],nxs[k],nys[k]});
            hudPending().push_back(std::move(pending));
        }
        origHud[I](rdram, ctx, runtime);
        if (yieldFix) { hudFinish<I>(rdram,ctx,runtime,entrySp,entryRa); return; }
        if (yieldLog && (ctx->pc != entryRa || GPR_U32(ctx,29) != entrySp)) ++hudYields;
        for (uint32_t k = 0; k < n; ++k) // restore, unless the builder itself rewrote that word meanwhile
        {
            const float now = rdF(rdram, ctx, runtime, addrs[k]);
            if (std::memcmp(&now, &nxs[k], 4) == 0)
                wrF(rdram, ctx, runtime, addrs[k], xs[k]);
            if (hudY != 1.0f)
            {
                const float nowY = rdF(rdram, ctx, runtime, addrs[k] + 4u);
                if (std::memcmp(&nowY, &nys[k], 4) == 0)
                    wrF(rdram, ctx, runtime, addrs[k] + 4u, ys[k]);
            }
        }
    }

    static void viewport(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool start = ctx->pc == 0x0020C530u;
        const uint32_t sp = GPR_U32(ctx,29);
        // GAME(transformers): 20C534 saves ra at sp+60 in a 250-byte frame;
        // all 16 continuation aliases run before the 20C8CC epilogue restores it.
        const uint32_t ra = start ? GPR_U32(ctx,31) : READ32(sp+0x60u);
        const uint32_t returnSp = start ? sp : sp+0x250u;
        if (yieldLog) { if (start) ++viewportStarts; else ++viewportResumes; }
        origViewport(rdram, ctx, runtime);
        const bool returned = ctx->pc == ra && GPR_U32(ctx,29) == returnSp;
        if (yieldLog) {
            if (returned) {
                ++viewportReturns;
                // TEMP(transformers): generated JR returns bypass dispatchGuestBranch unless
                // STRICT_RETURN_DIAGNOSTICS is enabled. No consumer runs between this store
                // and the viewport epilogue, so observe it here before the caller resumes.
                aspectWriter = READ8(kOrtho) == 0u ? 0x0020C858u : 0x0020C898u;
            } else ++viewportYields;
        }
        // TEMP(transformers): the 17-entry wrapper is observational in the default path.
        // FIX=0 preserves the old entry-only divisor loss for controlled comparisons.
        if (!yieldFix && returned && start && legacyViewportKr != 1.0f && READ8(kOrtho) == 0u) {
            wrF(rdram, ctx, runtime, kAspectR, rdF(rdram, ctx, runtime, kAspectR) / legacyViewportKr);
            if (yieldLog) ++viewportFixes;
        }
        yieldObserve(runtime);
    }

    template <int I>
    static void flagWriter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        origFlagWriter[I](rdram, ctx, runtime);
        if (!force)
            return;
        const uint32_t opts = READ32(kOptionsPtr);
        if (opts != 0u && opts < 0x02000000u - 5u)
        {
            const bool on = READ8(opts + 4u) != 0;
            const bool before = g_tfMenuWide.exchange(on, std::memory_order_acq_rel);
            // TEMP(transformers): menu/save transition evidence, never change the saved options byte.
            if (before != on)
                std::cerr << "[ws:menu] option=" << on << " writer=" << I
                          << " aspect=" << ps2WidescreenAspect() << std::endl;
            WRITE8(kWideFlag, flagValue);
            updateAspect();
        }
    }

    static void install(PS2Runtime &rt)
    {
        const float aspect = ps2WidescreenAspect();
        const char *yieldEnv = std::getenv("PS2X_WS_YIELD_LOG"); // TEMP(transformers)
        yieldLog = yieldEnv && yieldEnv[0] == '1';
        const char *fixEnv = std::getenv("PS2X_WS_YIELD_FIX"); // TEMP(transformers): controlled OFF build
        yieldFix = !(fixEnv && fixEnv[0] == '0');
        const char *logEnv = std::getenv("PS2X_WS_LOG"); // TEMP(transformers)
        logBudget = logEnv ? std::atoi(logEnv) : 0;
        const char *forceEnv = std::getenv("PS2X_WIDESCREEN_FORCE");
        force = !(forceEnv && forceEnv[0] == '0');
        const char *modeEnv = std::getenv("PS2X_WIDESCREEN_MODE");
        const bool gameMode = ps2WidescreenOverride() && modeEnv && std::strcmp(modeEnv, "game") == 0;
        displayK = (aspect > 0.0f && !gameMode) ? aspect / (4.0f / 3.0f) : 1.0f;
        kt = (aspect > 0.0f && !gameMode) ? std::max(aspect, 16.0f / 9.0f) / (4.0f / 3.0f) : 1.0f;
        hudY = displayK / kt;
        flagValue = gameMode ? 1u : 0u;
        nativeMode = gameMode;
        if (rt.hasFunction(0x0020CA00u) && (force || kt != 1.0f || logBudget > 0))
        {
            origPerspective = rt.lookupFunction(0x0020CA00u);
            rt.replaceFunction(0x0020CA00u, &perspective);
        }
        if ((logBudget > 0 || yieldLog) && rt.hasFunction(0x00221E70u)) // TEMP(transformers)
        {
            origBuildP = rt.lookupFunction(0x00221E70u);
            rt.replaceFunction(0x00221E70u, &buildP);
        }
        if ((force || kt != 1.0f) && rt.hasFunction(0x0020C530u))
        {
            origViewport = rt.lookupFunction(0x0020C530u);
            for (uint32_t pc=0x0020C530u; pc<0x0020C900u; pc+=4u)
                if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origViewport)
                    rt.replaceFunction(pc,&viewport);
        }
        installCull(rt); // GAME(transformers): experimental, default OFF
        const char *hudEnv = std::getenv("PS2X_WIDESCREEN_HUD");
        hudOn = !gameMode && !(hudEnv && hudEnv[0] == '0');
        hudCentreOnly = hudEnv && std::strcmp(hudEnv, "center") == 0;
        if (hudOn)
        {
            // GAME(transformers): cover separately dispatched yield/resume aliases as well as entry.
            const char *surfaceEnv = std::getenv("PS2X_WS_MENU_SURFACE");
            if (!(surfaceEnv && surfaceEnv[0] == '0') && rt.hasFunction(0x00313F30u))
            {
                origMenuSurface = rt.lookupFunction(0x00313F30u);
                for (uint32_t pc = 0x00313F30u; pc < 0x003153A0u; pc += 4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc) == origMenuSurface)
                        rt.replaceFunction(pc, &menuSurface);
            }
            if (!(surfaceEnv && surfaceEnv[0] == '0') && rt.hasFunction(0x00233330u))
            {
                origFontDraw = rt.lookupFunction(0x00233330u);
                for (uint32_t pc = 0x00233330u; pc < 0x00233600u; pc += 4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc) == origFontDraw)
                        rt.replaceFunction(pc, &fontDraw);
            }
            // GAME(transformers): Autobot, Mini-Con and equipment help/info page renderers.
            // Cover separately dispatched guest continuation aliases, as for the
            // holographic menu surface. No changes to guest panel data or timing.
            const char *panelEnv = std::getenv("PS2X_WS_PANEL_LAYOUT");
            if (!(panelEnv && panelEnv[0] == '0'))
            {
                static constexpr PS2Runtime::RecompiledFunction hooks[8] = {
                    &screenPanel<0>, &screenPanel<1>, &screenPanel<2>, &screenPanel<3>,
                    &screenPanel<4>, &screenPanel<5>, &screenPanel<6>, &screenPanel<7>};
                for (int i = 0; i < 8; ++i)
                {
                    const PanelSpec &spec = panelSpecs[i];
                    if (!rt.hasFunction(spec.start)) continue;
                    origScreenPanel[i] = rt.lookupFunction(spec.start);
                    for (uint32_t pc = spec.start; pc < spec.end; pc += 4u)
                        if (rt.hasFunction(pc) && rt.lookupFunction(pc) == origScreenPanel[i])
                            rt.replaceFunction(pc, hooks[i]);
                }
            }
            const char *widgetEnv=std::getenv("PS2X_WS_WIDGET_LAYOUT");
            widgetLayout=!(widgetEnv && widgetEnv[0]=='0');
            if (rt.hasFunction(0x00302B90u)) {
                origGauge=rt.lookupFunction(0x00302B90u);
                for (uint32_t pc=0x00302B90u;pc<0x00303254u;pc+=4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origGauge)
                        rt.replaceFunction(pc,&gaugeRender);
            }
            if (rt.hasFunction(0x00300E90u)) {
                origBoss=rt.lookupFunction(0x00300E90u);
                for (uint32_t pc=0x00300E90u;pc<0x00301300u;pc+=4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origBoss)
                        rt.replaceFunction(pc,&bossRender);
            }
            if (gaugeTest().first>=0.0f && rt.hasFunction(0x00438050u)) {
                origGaugeMesh=rt.lookupFunction(0x00438050u);
                for (uint32_t pc=0x00438050u;pc<0x00438D00u;pc+=4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origGaugeMesh)
                        rt.replaceFunction(pc,&gaugeMesh);
            }
            static constexpr uint32_t builders[4] = {0x00257610u, 0x00257160u, 0x002578F0u, 0x00256280u};
            static constexpr PS2Runtime::RecompiledFunction hooks[4] = {&hud<0>, &hud<1>, &hud<2>, &hud<3>};
            for (int i = 0; i < 4; ++i)
            {
                if (!rt.hasFunction(builders[i]))
                    continue;
                origHud[i] = rt.lookupFunction(builders[i]);
                constexpr uint32_t ends[4] = {0x2578F0u,0x257610u,0x257E70u,0x256AC0u};
                for (uint32_t pc=builders[i]; pc<ends[i]; pc+=4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc)==origHud[i])
                        rt.replaceFunction(pc,hooks[i]);
            }
        }
        if (force)
        {
            if (rt.hasFunction(0x00278080u))
            {
                origOptionGetter = rt.lookupFunction(0x00278080u);
                rt.replaceFunction(0x00278080u, &optionGetter);
            }
            static constexpr uint32_t writers[4] = {0x00277FB0u, 0x00278010u, 0x00278090u, 0x002780E0u};
            static constexpr PS2Runtime::RecompiledFunction hooks[4] = {&flagWriter<0>, &flagWriter<1>, &flagWriter<2>,
                                                                         &flagWriter<3>};
            static constexpr uint32_t ends[4] = {0x00278010u, 0x00278080u, 0x002780E0u, 0x00278220u};
            for (int i = 0; i < 4; ++i)
            {
                if (!rt.hasFunction(writers[i]))
                    continue;
                origFlagWriter[i] = rt.lookupFunction(writers[i]);
                // GAME(transformers): also wrap independently dispatched entries and yield continuations.
                for (uint32_t pc = writers[i]; pc < ends[i]; pc += 4u)
                    if (rt.hasFunction(pc) && rt.lookupFunction(pc) == origFlagWriter[i])
                        rt.replaceFunction(pc, hooks[i]);
            }
        }
        std::cerr << "[transformers] widescreen: aspect=" << aspect << " Hor+ kt=" << kt << " force=" << force
                  << " hud=" << hudOn << " (OSD TV type " << (aspect > 0.0f ? "16:9" : "4:3") << ")" << std::endl;
    }
};
} // namespace

// Display aspect for the current frame: PS2X_WIDESCREEN, except that in Transformers Hor+ mode full-screen FMV frames
// (ps2WidescreenFmvFrame) are shown at 4:3, pillarboxed.
static float ps2WidescreenPresentAspect(float frameWidth)
{
    const float aspect = ps2WidescreenAspect();
    if (aspect <= 0.0f)
        return aspect;
    static const bool pillar = []() { const char *e = std::getenv("PS2X_WIDESCREEN_PILLARBOX"); return !(e && e[0] == '0'); }();
    const int pcPillar = ps2x::ui::g_pcPillarbox.load(std::memory_order_relaxed);
    if (pcPillar == 0 || (pcPillar < 0 && !pillar))
        return aspect;
    const uint64_t now = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
    const uint64_t last = g_ps2LastPath1Vsync.load(std::memory_order_relaxed);
    const bool flat = last == 0u || (now >= last && now - last >= 8u);
    int presentedScene = -1;
#if defined(PS2X_WITH_PGS)
    presentedScene = ps2PgsPresentedScene();
#endif
    // GAME(transformers): a held image retains its aspect even if EE/worker activity advances or stops.
    // The legacy/CPU path keeps its existing classifier. PGS carries stream-ordered classification with each image.
    const float result = presentedScene >= 0 ? (presentedScene ? aspect : 4.0f / 3.0f) :
                         ((flat || ps2WidescreenFmvFrame(frameWidth)) ? 4.0f / 3.0f : aspect);
    // TEMP(transformers): wsedge1 includes the flat short-circuit, absent from WS2_FMVLOG.
    static const bool log = []() { const char *e = std::getenv("PS2X_WSEDGE_LOG"); return e && *e == '1'; }();
    static float previous = -1.0f;
    if (log && previous != result)
    {
        std::cout << "[wsedge:aspect] vs=" << now << " path1=" << last << " width=" << frameWidth
                  << " flat=" << flat << " aspect=" << result << std::endl;
        previous = result;
    }
    return result;
}

// Display aspect the presenter fits the current frame into (window dstRect and the PS2X_UPSCALE_CAPTURE placement):
// PS2X_WIDESCREEN when set (full-screen FMV frames 4:3, see above), otherwise the default presentation aspect
// (PS2X_PRESENT_ASPECT, 4:3; 0 = square pixels). The upscale pass (PS2X_UPSCALE) does the non-uniform x/y scale either way.
float ps2PresentFrameAspect(float frameWidth)
{
    // TEMP(transformers): FMV detection uses PS2 raster width, never the supersampled texture width.
    frameWidth=ps2PgsNativeFrameWidth(frameWidth);
    const float wsAspect = ps2WidescreenPresentAspect(frameWidth);
    return (wsAspect > 0.0f) ? wsAspect : ps2PresentDisplayAspect();
}

namespace // TEMP(transformers): see loadELF
{
struct VuAwareMem
{
    static inline PS2Runtime::RecompiledFunction origMemcpy = nullptr;
    static inline PS2Runtime::RecompiledFunction origMemset = nullptr;
    static bool touchesVu(uint32_t addr, uint32_t size)
    {
        const uint32_t phys = (addr >= 0x80000000u) ? (addr & 0x1FFFFFFFu) : addr;
        const uint64_t end = static_cast<uint64_t>(phys) + size;
        return phys < 0x11010000u && end > 0x11000000u;
    }
    static void memcpyFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = getRegU32(ctx, 4);
        const uint32_t src = getRegU32(ctx, 5);
        const uint32_t size = getRegU32(ctx, 6);
        if (size != 0u && size < 0x01000000u && (touchesVu(dst, size) || touchesVu(src, size)))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, dst + i, runtime->Load8(rdram, ctx, src + i));
            }
            ctx->r[2] = ctx->r[4];
            ctx->pc = getRegU32(ctx, 31); // behave like the guest function's jr $ra
            return;
        }
        origMemcpy(rdram, ctx, runtime);
    }
    static void memsetFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = getRegU32(ctx, 4);
        const uint8_t value = static_cast<uint8_t>(getRegU32(ctx, 5));
        const uint32_t size = getRegU32(ctx, 6);
        if (size != 0u && size < 0x01000000u && touchesVu(dst, size))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, dst + i, value);
            }
            ctx->r[2] = ctx->r[4];
            ctx->pc = getRegU32(ctx, 31); // behave like the guest function's jr $ra
            return;
        }
        origMemset(rdram, ctx, runtime);
    }
};
}

#include "ps2_carstick_detector.h"
#include "ps2_menustick_detector.h"
bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);
    // GAME(transformers): option B changes only the dead statistic observation.
    const char *stat=std::getenv("PS2X_VU1_DEAD_STAT");
    m_memory.m_vif1StatisticShortcut=elfPath.find("SLUS_206.68")!=std::string::npos && !(stat&&stat[0]=='0');

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
        if (ps2_kheap::enabled()) // TEMP(transformers) qualification: invocation stacks below the ELF (ps2_kernel_heap.h)
        {
            m_asyncCallbackStackFloor = ps2_kheap::kLowInvokeStacksFloor;
            m_asyncCallbackStackTop = ps2_kheap::kLowInvokeStacksTop;
        }
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    // GAME(transformers): identity-gated, default-off EE leaf kernels.
    ps2_ee_hle::install(*this, module.name.c_str(), m_cpuContext.pc, elfCrc32, elfCrc32Valid);

    // GAME(transformers): release fix, preserve original getter calls and allocations.
    if (module.name == "SLUS_206.68" && m_cpuContext.pc == 0x00100008u)
        ps2_remap::installGetter(*this);
    if (module.name == "SLUS_206.68") { ps2_carstick::install(*this); ps2_menustick::enable(); }

    // TEMP(transformers): workaround for Ps2IsPhysicalSpecialAddress (include/runtime/ps2_address.h) only treating
    // [0x11004000, 0x1100C000) as VU memory. VU0 code (0x11000000) and VU1 data (0x1100C000) fall through to the
    // fast RAM path and alias main RAM 0x1000000/0x100C000 (& PS2_RAM_MASK). Transformers saves/restores VU1
    // data memory with memcpy/memset (fn 0x26ED80 etc.), which overwrote live heap objects at 0x100C000-0x1010000
    // and crashed the first level load. Until the header is fixed (full rebuild), route the game's memcpy (0x109110)
    // and memset (0x110340) through the slow Load/Store path whenever a range touches VU memory.
    // Superseded by the ps2_address.h fix (2026-10-07); kept behind PS2X_VU_MEMCPY_WRAP=1 as a fallback.
    if (std::getenv("PS2X_VU_MEMCPY_WRAP") && elfPath.find("SLUS_206.68") != std::string::npos && m_cpuContext.pc == 0x00100008u)
    {
        if (hasFunction(0x00109110u) && hasFunction(0x00110340u))
        {
            VuAwareMem::origMemcpy = lookupFunction(0x00109110u);
            VuAwareMem::origMemset = lookupFunction(0x00110340u);
            if (VuAwareMem::origMemcpy && VuAwareMem::origMemset)
            {
                replaceFunction(0x00109110u, &VuAwareMem::memcpyFn);
                replaceFunction(0x00110340u, &VuAwareMem::memsetFn);
                std::cerr << "[transformers] memcpy/memset wrapped for VU memory ranges" << std::endl;
            }
        }
    }

    // GAME(transformers): PS2-exact replacement for the Mat4 copy helper 0x1B0DD0 (see tfMat4CopyAffine). PS2X_WCLEAN_FIX=0 disables.
    {
        const char *wclean = std::getenv("PS2X_WCLEAN_FIX");
        if (!(wclean && wclean[0] == '0') && elfPath.find("SLUS_206.68") != std::string::npos && hasFunction(0x001B0DD0u))
        {
            replaceFunction(0x001B0DD0u, &tfMat4CopyAffine);
            std::cerr << "[transformers] 0x1B0DD0 replaced (PS2-exact w-column clear)" << std::endl;
        }
    }

    if (ps2Field60Enabled() && elfPath.find("SLUS_206.68") != std::string::npos)
    {
        // GAME(transformers) TEMP(transformers) qualification I1a: PS2X_FIELD60 needs the game's 30 Hz mode (gp-0x576C = 1:
        // field 2 = t - 0.5; in 60 Hz mode gp-0x5A00 = 1.0 and both fields are the same pose). The load-balancing
        // governor sub_0025B670 drops to 60 Hz after gp-0x5764 (= 2, .sdata word, read only at 0x25bcdc, never written)
        // consecutive fast frames: raise the threshold so it never does (high_fps.md 2.3/5.1). PS2X_FIELD60_PIN30=0 skips.
        const char *pin = std::getenv("PS2X_FIELD60_PIN30");
        if (!(pin && pin[0] == '0'))
        {
            constexpr uint32_t kFastFrameThreshold = 0x6D30F0u - 0x5764u; // gp-0x5764 = 0x6CD98C
            uint8_t *p = m_memory.getRDRAM() + kFastFrameThreshold;
            uint32_t old = 0;
            std::memcpy(&old, p, 4);
            const uint32_t pinned = 0x7FFFFFFFu;
            std::memcpy(p, &pinned, 4);
            std::cerr << "[field60] 30 Hz mode pinned: gp-0x5764 (0x" << std::hex << kFastFrameThreshold << ") " << std::dec
                      << old << " -> " << pinned << std::endl;
        }
    }

    // GAME(transformers): PS2X_TF_NOFRAMEWAIT=1 makes the main-loop frame-wait check (0x25D990) always report "done" (experiment, default OFF).
    {
        const char *nfw = std::getenv("PS2X_TF_NOFRAMEWAIT");
        const int nfwMode = nfw ? std::atoi(nfw) : 0; // bit 0: GS-flip pending check 0x25D990; bit 1: vsync frame limiter 0x25DE90, bit 2: 60 fps cap (e0=0)
        if (nfwMode != 0 && elfPath.find("SLUS_206.68") != std::string::npos)
        {
            if ((nfwMode & 1) && hasFunction(0x0025D990u))
            {
                replaceFunction(0x0025D990u, &tfFrameWaitSkip);
                std::cerr << "[transformers] 0x25D990 flip-pending check replaced (PS2X_TF_NOFRAMEWAIT&1: wait skipped)" << std::endl;
            }
        }
        const char *flog = std::getenv("PS2X_TF_FRAMELOG");
        g_tfFrameSyncLog = flog && flog[0] == '1';
        g_tfFrameSyncSkip = (nfwMode & 2) != 0;
        g_tfFrameSync60 = (nfwMode & 4) != 0;
        if ((g_tfFrameSyncSkip || g_tfFrameSync60 || g_tfFrameSyncLog) && elfPath.find("SLUS_206.68") != std::string::npos && hasFunction(0x0025DE90u))
        {
            g_tfFrameSyncOrig = lookupFunction(0x0025DE90u);
            replaceFunction(0x0025DE90u, &tfFrameSync);
            std::cerr << "[transformers] 0x25DE90 frame-sync wrapped (skip=" << g_tfFrameSyncSkip << " 60=" << g_tfFrameSync60 << " log=" << g_tfFrameSyncLog << ")" << std::endl;
        }
    }
    // GAME(transformers): widescreen (PS2X_WIDESCREEN=<aspect>; see TfWidescreen). No-op unless the env var is set.
    if (elfPath.find("SLUS_206.68") != std::string::npos)
        TfWidescreen::install(*this);
    // TEMP(transformers): qualification - PS2X_SPRINT_LOG=1 logs the robot's walk/run pick (ps2_sprint_probe.h). No-op unset.
    if (elfPath.find("SLUS_206.68") != std::string::npos)
        TfSprintProbe::install(*this);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }
    // PS2X_MC_ROOT=<dir> puts memory card 0 elsewhere (e.g. a fresh card per unattended test run).
    if (const char *envMc = std::getenv("PS2X_MC_ROOT"); envMc && *envMc)
    {
        paths.mcRoot = std::filesystem::path(envMc);
        std::error_code ec;
        std::filesystem::create_directories(paths.mcRoot, ec);
    }

    // Raw sector reads (sceCdRead from IOP modules) need the disc image, not the extracted tree.
    // PS2X_CD_IMAGE wins; otherwise use a lone .iso next to the ELF.
    if (paths.cdImage.empty())
    {
        if (const char *envImage = std::getenv("PS2X_CD_IMAGE"); envImage && *envImage)
        {
            paths.cdImage = std::filesystem::path(envImage);
        }
        else if (!paths.elfDirectory.empty())
        {
            std::error_code ec;
            std::filesystem::path found;
            int isoCount = 0;
            for (const auto &entry : std::filesystem::directory_iterator(paths.elfDirectory, ec))
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (entry.is_regular_file(ec) && ext == ".iso")
                {
                    found = entry.path();
                    ++isoCount;
                }
            }
            if (isoCount == 1)
            {
                paths.cdImage = found;
            }
        }
    }

    setIoPaths(paths);
}

namespace
{
    // TEMP(transformers) runtime fast path: EE executor owns table mutation and cache use.
    // Global generation also invalidates entries if another runtime installs hooks.
    uint64_t g_eeoptDispatchGeneration = 1u;
    struct EeoptDispatchEntry {
        const PS2Runtime *owner = nullptr;
        uint32_t pc = 0u;
        uint64_t generation = 0u;
        PS2Runtime::RecompiledFunction fn = nullptr;
    };
    EeoptDispatchEntry g_eeoptDispatchCache[64];

    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    // TEMP(transformers): startup installs precede run(); live mutation is executor-only.
    if (m_eeScheduler) m_eeScheduler->assertFunctionMutationThread();
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    ++g_eeoptDispatchGeneration; // TEMP(transformers): registerFunction delegates here.
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
        { // TEMP(transformers): dump the objects involved + recent DMA/IOP writes that touched them
            extern void ps2DbgDumpEeWritesOverlapping(uint32_t lo, uint32_t hi, const char *why);
            uint32_t spWord0 = 0u;
            readGuestU32At(sp & 0x1FFFFFFFu, spWord0);
            const uint32_t objs[3] = {a0 & 0x1FFFFFFFu, s0 & 0x1FFFFFFFu, spWord0 & 0x1FFFFFFFu};
            const char *names[3] = {"a0", "s0", "*(sp+0)"};
            for (int i = 0; i < 3; ++i)
            {
                const uint32_t base = objs[i] & ~0xFu;
                std::cerr << "[missing:obj] " << names[i] << "=0x" << std::hex << objs[i];
                for (uint32_t off = 0u; off < 0x60u; off += 4u)
                {
                    uint32_t w = 0u;
                    if (!readGuestU32At(base + off, w))
                    {
                        break;
                    }
                    if ((off & 0xFu) == 0u)
                    {
                        std::cerr << "\n  " << (base + off) << ":";
                    }
                    std::cerr << " " << w;
                }
                std::cerr << std::dec << std::endl;
                if (objs[i] >= 0x00100000u && objs[i] < PS2_RAM_SIZE)
                {
                    ps2DbgDumpEeWritesOverlapping(base, base + 0x60u, names[i]);
                }
            }
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

// TEMP(transformers) eetrim: the three env-gated dispatch probes below (PS2X_JALR_WATCH / PS2X_DRONE_WATCH /
// PS2X_CINEMA_WATCH) each paid a function-local-static guard check (+ vsync loads) on every guest call/jump; one plain
// bool read at static-initialisation time now skips all three when none is set.
static const bool g_dispatchProbesOn = std::getenv("PS2X_JALR_WATCH") || std::getenv("PS2X_DRONE_WATCH") ||
                                       std::getenv("PS2X_CINEMA_WATCH");

// qualification / CROSSOVER3 H3: guest unwind tracking, re-implemented from upstream PR #270 (7c74436acd4c; the same fix is
// in SOCOM ec4b9fbc365e, MunchkinClubber cfb42206a572, JustBeanie d3f326046c38, noahbaxter 6170df4e41fc).
// Guest code gives control back to the EE scheduler by returning up the host stack. dispatchGuestBranch() used to decide
// "the callee returned normally" from ctx->pc alone (pc == callee entry -> implicit return; pc == fallthrough -> return),
// which an unwind can imitate. Unwinds are now counted and compared across the callee call:
//   - checkpoint yields: the scheduler's checkpointYieldCount() (every checkpointDue() that returned true, i.e. the
//     generated back-edges via eeCheckpointDue() and the dispatch charge below) - our earlier calleeYielded counter;
//   - non-yield unwinds, counted here: a non-call branch handed back to the dispatcher, and a callee that came back
//     somewhere other than its fallthrough (non-local return / tail jump).
// When the count moved, the caller returns false with ctx->pc untouched, so every frame unwinds and the scheduler resumes
// at the parked pc. Kept out of ee_scheduler.h (runner PCH). EE executor thread only. PS2X_GUEST_UNWIND=0 = old logic.
static uint64_t s_guestNonYieldUnwinds = 0u;
static const bool g_guestUnwindFix = []
{
    const char *e = std::getenv("PS2X_GUEST_UNWIND");
    return !(e && e[0] == '0');
}();
// TEMP(transformers) qualification: how often the unwind rule decides differently from the old pc-only rule.
static uint64_t s_guestUnwindDiffers = 0u;
static void noteGuestUnwindDiffers(const char *why, uint32_t sourcePc, uint32_t targetPc, uint32_t entryPc,
                                   uint32_t fallthroughPc, uint32_t pc)
{
    const uint64_t n = ++s_guestUnwindDiffers;
    if (n <= 20u || (n & (n - 1u)) == 0u)
        std::cerr << "[unwind] #" << n << " " << why << " src=0x" << std::hex << sourcePc << " target=0x" << targetPc
                  << " entry=0x" << entryPc << " fallthrough=0x" << fallthroughPc << " pc=0x" << pc << std::dec
                  << " vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << std::endl;
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    TfWidescreen::observeAspectDispatch(rdram, ctx, this, sourcePc, targetPc, isCall); // TEMP(transformers)
    if (g_dispatchProbesOn)
    {
    { // TEMP(transformers): PS2X_JALR_WATCH=<hex source pc> logs the targets taken by that indirect call site.
        static const uint32_t s_jalrWatch = []() -> uint32_t
        {
            const char *e = std::getenv("PS2X_JALR_WATCH");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
        }();
        if (s_jalrWatch != 0u && sourcePc == s_jalrWatch)
        {
            static uint32_t s_logs = 0u;
            if (s_logs++ < 400u)
            {
                const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
                float f12 = 0.0f;
                std::memcpy(&f12, &ctx->f[12], sizeof(f12));
                std::cerr << "[jalrwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " src=0x" << std::hex
                          << sourcePc << " target=0x" << targetPc << " a0=0x" << a0 << " f12=" << std::dec << f12 << " obj=";
                for (uint32_t off = 0x28u; off <= 0x40u; off += 4u)
                    std::cerr << (off > 0x28u ? "," : "") << std::hex << m_memory.read32(a0 + off);
                const uint32_t s3 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[19])); // script args array
                std::cerr << " s3=0x" << s3 << " args=";
                for (uint32_t i = 0; i < 8u; ++i)
                    std::cerr << (i ? "," : "") << m_memory.read32(s3 + i * 4u);
                std::cerr << std::dec << std::endl;
            }
        }
    }
    { // TEMP(transformers): PS2X_DRONE_WATCH=<from>-<to vsync> traces the Amazon sentry-drone searchlight cone (blue cast,
      // private qualification evidence): drone update fn_00447D70 state/effects/position, its distance call, cone start/stop.
        static const std::pair<long long, long long> s_droneVs = []()
        {
            const char *e = std::getenv("PS2X_DRONE_WATCH");
            if (!e)
                return std::pair<long long, long long>{-1, -1};
            char *n = nullptr;
            const long long lo = std::strtoll(e, &n, 10);
            return std::pair<long long, long long>{lo, (n && *n == '-') ? std::strtoll(n + 1, nullptr, 10) : lo};
        }();
        const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        if (s_droneVs.first >= 0 && vsNow >= s_droneVs.first && vsNow <= s_droneVs.second &&
            (targetPc == 0x447D70u || sourcePc == 0x447EC8u || targetPc == 0x449210u || targetPc == 0x448D50u ||
             targetPc == 0x448C60u ||
             ((targetPc == 0x362520u || targetPc == 0x362550u || targetPc == 0x362560u || targetPc == 0x348AD0u ||
               targetPc == 0x26C060u) &&
              sourcePc >= 0x447D70u && sourcePc < 0x448C50u)))
        {
            static uint32_t s_lines = 0u;
            if (s_lines < 6000u)
            {
                ++s_lines;
                auto rd = [this](uint32_t a) -> uint32_t { return a ? m_memory.read32(a) : 0u; };
                auto rf = [&rd](uint32_t a) -> float { const uint32_t v = rd(a); float f; std::memcpy(&f, &v, 4); return f; };
                const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
                const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
                const uint32_t a2 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
                const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                std::cerr << "[dronewatch] vs=" << vsNow << " src=0x" << std::hex << sourcePc << " tgt=0x" << targetPc
                          << " a0=0x" << a0 << " a1=0x" << a1 << " a2=0x" << a2 << " ra=0x" << ra << std::dec;
                if (targetPc == 0x447D70u)
                {
                    float dt = 0.0f;
                    std::memcpy(&dt, &ctx->f[12], sizeof(dt));
                    std::cerr << " dt=" << dt << " state=" << rd(a0 + 0x50u) << " s48=" << rd(a0 + 0x48u) << " b54=0x"
                              << std::hex << rd(a0 + 0x54u) << std::dec << " f58=" << rf(a0 + 0x58u) << " w5c=" << rd(a0 + 0x5Cu)
                              << " t60=" << rf(a0 + 0x60u) << " w64=" << rd(a0 + 0x64u) << " pow=" << rf(a0 + 0x78u) << ","
                              << rf(a0 + 0x8Cu) << "," << rf(a0 + 0xA0u);
                    for (uint32_t off : {0x68u, 0x7Cu, 0x90u, 0xA4u, 0xB4u, 0xE0u})
                    {
                        const uint32_t eff = rd(a0 + off);
                        std::cerr << " e" << std::hex << off << "=0x" << eff << std::dec;
                        if (eff >= 0x100000u && eff < 0x2000000u)
                            std::cerr << "(s38=" << rd(eff + 0x38u) << " vis=0x" << std::hex << rd(eff + 0x30u) << std::dec << ")";
                    }
                    const uint32_t sc = rd(a0 + 0x9Cu);
                    if (sc >= 0x100000u && sc < 0x2000000u)
                        std::cerr << " scale=" << rf(sc) << "," << rf(sc + 4u) << "," << rf(sc + 8u);
                    uint32_t node = rd(a0 + 0x24u);
                    node = (node >= 0x100000u && node < 0x2000000u) ? rd(node + 0xCu) : 0u;
                    node = (node >= 0x100000u && node < 0x2000000u) ? rd(node + 0x14u) : 0u;
                    if (node >= 0x100000u && node < 0x2000000u)
                        std::cerr << " pos=" << rf(node + 0x30u) << "," << rf(node + 0x34u) << "," << rf(node + 0x38u);
                    const uint32_t hp = rd(a0 + 0x40u);
                    if (hp >= 0x100000u && hp < 0x2000000u)
                        std::cerr << " hp=" << rf(hp + 4u) << " hpflags=0x" << std::hex << rd(hp + 0x14u) << std::dec;
                }
                else if (sourcePc == 0x447EC8u)
                {
                    std::cerr << " target=" << rf(a0) << "," << rf(a0 + 4u) << "," << rf(a0 + 8u) << " self=" << rf(a1) << ","
                              << rf(a1 + 4u) << "," << rf(a1 + 8u);
                }
                else if (targetPc == 0x449210u || targetPc == 0x448D50u || targetPc == 0x448C60u)
                {   // gate init / StartWarpIn / Activate: progress gate status (2 = activated -> init state 8)
                    const uint32_t level = rd(0x716EF0u);
                    const uint32_t gate = rd(a0 + 0x48u);
                    std::cerr << " level=" << level << " gate=" << gate << " gatestatus="
                              << rd(0x7170E0u + level * 76u + gate * 4u + 0x38u) << " state=" << rd(a0 + 0x50u)
                              << " b54=0x" << std::hex << rd(a0 + 0x54u) << std::dec;
                }
                else if (targetPc == 0x26C060u)
                {
                    std::cerr << " pos=" << rf(a1) << "," << rf(a1 + 4u) << "," << rf(a1 + 8u) << "," << rf(a1 + 12u);
                }
                std::cerr << std::endl;
            }
        }
    }

    { // TEMP(transformers): PS2X_CINEMA_WATCH=<from>-<to vsync> traces the level-start cinema playlist (the Amazon intro /
      // AutobotWarpIn cinematics never play; private qualification evidence): manager gp-0x3568, playlist build/add, PlayNext,
      // the AutoPlay thread-name strcmp, thread activation and script command execution.
        static const std::pair<long long, long long> s_cinVs = []()
        {
            const char *e = std::getenv("PS2X_CINEMA_WATCH");
            if (!e)
                return std::pair<long long, long long>{-1, -1};
            char *n = nullptr;
            const long long lo = std::strtoll(e, &n, 10);
            return std::pair<long long, long long>{lo, (n && *n == '-') ? std::strtoll(n + 1, nullptr, 10) : lo};
        }();
        const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        if (s_cinVs.first >= 0 && vsNow >= s_cinVs.first && vsNow <= s_cinVs.second &&
            (targetPc == 0x515F50u || targetPc == 0x5155C0u || targetPc == 0x5143F0u || targetPc == 0x515D10u ||
             targetPc == 0x516C40u || targetPc == 0x50D7C0u || targetPc == 0x50FC00u || targetPc == 0x514B40u ||
             (targetPc == 0x1134A8u && sourcePc >= 0x514000u && sourcePc < 0x517000u)))
        {
            static uint32_t s_cinLines = 0u;
            if (s_cinLines < 3000u)
            {
                ++s_cinLines;
                auto rd = [this](uint32_t a) -> uint32_t { return (a >= 0x1000u && a < 0x2000000u) ? m_memory.read32(a) : 0u; };
                auto str = [this](uint32_t a) -> std::string
                {
                    std::string s;
                    if (a < 0x1000u || a >= 0x2000000u)
                        return s;
                    for (uint32_t i = 0; i < 80u; ++i)
                    {
                        const char c = static_cast<char>(m_memory.read8(a + i));
                        if (!c)
                            break;
                        s.push_back((c >= 32 && c < 127) ? c : '?');
                    }
                    return s;
                };
                auto kstr = [&](uint32_t obj) -> std::string // game string object: inline at +1 unless bit0 -> ptr at +8
                { return (rd(obj) & 1u) ? str(rd(obj + 8u)) : str(obj + 1u); };
                const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
                const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
                const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                const uint32_t mgr = rd(0x6D30F0u - 0x3568u);
                std::cerr << "[cinewatch] vs=" << vsNow << " src=0x" << std::hex << sourcePc << " tgt=0x" << targetPc
                          << " a0=0x" << a0 << " a1=0x" << a1 << " ra=0x" << ra << " mgr=0x" << mgr;
                if (mgr)
                    std::cerr << " m34=" << rd(mgr + 0x34u) << " m38=0x" << rd(mgr + 0x38u) << " m3c=" << rd(mgr + 0x3Cu)
                              << " m50=" << rd(mgr + 0x50u) << " m64=" << rd(mgr + 0x64u) << " m80=" << rd(mgr + 0x80u);
                std::cerr << std::dec;
                if (targetPc == 0x1134A8u)
                    std::cerr << " s0='" << str(a0) << "' s1='" << str(a1) << "'";
                else if (targetPc == 0x515D10u)
                    std::cerr << " obj0='" << kstr(a0) << "' w=" << std::hex << rd(a0) << "," << rd(a0 + 4u) << ","
                              << rd(a0 + 8u) << "," << rd(a0 + 12u) << std::dec;
                std::cerr << std::endl;
            }
        }
    }
    } // g_dispatchProbesOn

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        ++s_guestNonYieldUnwinds; // qualification / H3: a non-call branch handed back unwinds every caller
        return false;
    }

    // TEMP(transformers) runtime fast path: one validated table lookup for calls.
    RecompiledFunction targetFn = nullptr;
    if (ps2EeoptEnabled()) {
        auto &entry = g_eeoptDispatchCache[(targetPc >> 2) & 63u];
        if (kind == GuestBranchKind::DirectCall && entry.owner == this &&
            entry.pc == targetPc && entry.generation == g_eeoptDispatchGeneration) {
            targetFn = entry.fn;
        } else {
            uint32_t slot = 0u;
            if (generatedFunctionTableSlot(targetPc, slot)) targetFn = g_ps2RecompiledFunctionTable[slot];
            if (kind == GuestBranchKind::DirectCall)
                entry = EeoptDispatchEntry{this, targetPc, g_eeoptDispatchGeneration, targetFn};
        }
    }
    if (ps2EeoptEnabled() ? targetFn == nullptr : !hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    if (ps2EeoptEnabled()) pushDispatchPc(targetPc); // same successful-call history as lookupFunction
    else targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t yieldsBefore = m_eeScheduler ? m_eeScheduler->checkpointYieldCount() : 0u;
    const uint64_t nonYieldUnwindsBefore = s_guestNonYieldUnwinds;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // pc == entry normally means the callee never redirected pc, but a loop whose back-edge targets
    // the function's first instruction leaves the same pc when it unwinds for a checkpoint. In that
    // case keep unwinding so the dispatcher resumes inside the loop, not at the return address.
    const bool calleeYielded = m_eeScheduler && m_eeScheduler->checkpointYieldCount() != yieldsBefore;
    if (g_guestUnwindFix) // qualification / H3 (see s_guestNonYieldUnwinds)
    {
        const bool calleeUnwound = calleeYielded || s_guestNonYieldUnwinds != nonYieldUnwindsBefore;
        if (calleeUnwound)
        {
            // Old rule: pc == entry && !yielded -> return; otherwise return iff pc == fallthrough.
            const bool oldReturned = (ctx->pc == entryPc && !calleeYielded) || ctx->pc == fallthroughPc;
            if (oldReturned)
                noteGuestUnwindDiffers(calleeYielded ? "yield-parked-at-fallthrough" : "handoff-at-entry-or-fallthrough",
                                       sourcePc, targetPc, entryPc, fallthroughPc, ctx->pc);
            return false;
        }
        if (ctx->pc == entryPc)
        {
            ctx->pc = fallthroughPc;
        }
        if (ctx->pc != fallthroughPc)
        {
            ++s_guestNonYieldUnwinds; // non-local return / tail jump: the scheduler continues at ctx->pc
            return false;
        }
        return true;
    }
    if (ctx->pc == entryPc && !calleeYielded)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    // qualification / CROSSOVER3 H5 (JustBeanie 2a208f921661; MunchkinClubber 116f03505ea5 lifts the cap to 1<<20 instead):
    // run the micro program to its E bit (or D/T halt) like VU1's run-to-end, in 64K-cycle slices with a 256-slice
    // guard (16.7M cycles), instead of a fixed 4096-cycle budget that cut longer routines off and copied their partial
    // state back to the EE. PS2X_VU0_MICRO_BUDGET=<n> restores a single fixed budget of n cycles (4096 = the old cap).
    static const uint32_t s_fixedBudget = []() -> uint32_t
    {
        const char *e = std::getenv("PS2X_VU0_MICRO_BUDGET");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    constexpr uint32_t kSliceCycles = 65536u;
    constexpr int kMaxSlices = 256;
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, s_fixedBudget != 0u ? s_fixedBudget : kSliceCycles);
    if (s_fixedBudget == 0u)
    {
        int slices = 1;
        for (; m_vu0.programPending() && slices < kMaxSlices; ++slices)
        {
            m_vu0.resume(vu0Code, PS2_VU0_CODE_SIZE,
                         vu0Data, PS2_VU0_DATA_SIZE,
                         m_gs, &m_memory, 0u, ctx->vu0_itop, kSliceCycles);
        }
        if (m_vu0.programPending())
        {
            static uint32_t s_warned = 0u;
            if (s_warned++ < 8u)
                std::cerr << "[VU0 micro] program at 0x" << std::hex << startPC << " still running at pc=0x"
                          << m_vu0.state().pc << std::dec << " after " << kMaxSlices << " x " << kSliceCycles
                          << " cycles; abandoning (possible infinite loop)" << std::endl;
        }
        else if (m_vu0.state().cycles > 4096u)
        {
            static uint32_t s_long = 0u; // TEMP(transformers) qualification: programs the old 4096-cycle cap cut off
            if (s_long++ < 8u)
                std::cerr << "[VU0 micro] program at 0x" << std::hex << startPC << std::dec << " ran "
                          << m_vu0.state().cycles << " cycles in " << slices << " slice(s) (the old 4096 cap cut it)"
                          << std::endl;
        }
    }
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

// TEMP(transformers): counters are EE-thread-local; logging only on guest vsync changes.
namespace ps2drain {
void flush(uint64_t vs) {
    static thread_local uint64_t last=~uint64_t(0);
    if(last==vs) return;
    if(last!=~uint64_t(0)) std::cerr << "[drain] vs=" << last << " stores=" << counts.stores
        << " drains=" << counts.drains << " dmac=" << counts.dmac << " intc=" << counts.intc
        << " mutexes=" << counts.mutexes << " syncs=" << counts.syncs << '\n';
    last=vs; counts=Counts{};
}
void store() { flush(ps2dbg::vsyncTick.load(std::memory_order_relaxed)); ++counts.stores; }
}
void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    if (ps2drain::stats) { ps2drain::flush(ps2dbg::vsyncTick.load(std::memory_order_relaxed)); ++ps2drain::counts.drains; ++ps2drain::depth; }
    if (!m_memory.m_vif1Stream && !m_memory.m_vif1Instant) {
        if (ps2drain::stats) ++ps2drain::counts.syncs;
        m_vu1.synchronize();
    }
    auto dmac = m_memory.consumeCompletedDmacCauses();
    if (ps2drain::stats && !dmac.empty()) ++ps2drain::counts.dmac;
    for (uint32_t cause : dmac)
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    auto intc = ps2intc::consume();
    if (ps2drain::stats && !intc.empty()) ++ps2drain::counts.intc;
    for (uint32_t cause : intc)
        m_eeScheduler->dispatchIrq(false, cause);
    if (ps2drain::stats) --ps2drain::depth;
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    // TEMP(transformers) qualification: PS2X_KERNEL_HEAP=1 lets the heap run up to the end of RAM (SetupHeap passes the stack base).
    const uint32_t hardLimit = ps2_kheap::enabled() ? PS2_RAM_SIZE : std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (ps2_kheap::enabled()) // TEMP(transformers) qualification: explicit limits up to the end of RAM; 0 = the old default
    {
        if (guestLimit == 0u)
        {
            return hardLimit;
        }
        return std::min(guestLimit, PS2_RAM_SIZE);
    }
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

// TEMP(transformers): PS2X_HUB_MATERIAL_WATCH=<first>-<last> logs the two material-copy setters
// and the single-light uploader. Read-only snapshots; no guest register or memory changes.
static void ps2WatchHubMaterial(PS2Memory &mem, const R5900Context *ctx, bool upload)
{
    struct Window { long long from = -1, to = -1; };
    static const Window w = []()
    {
        Window s;
        if (const char *e = std::getenv("PS2X_HUB_MATERIAL_WATCH"))
        {
            char *end = nullptr;
            s.from = std::strtoll(e, &end, 10);
            s.to = end && *end == '-' ? std::strtoll(end + 1, nullptr, 10) : s.from;
        }
        return s;
    }();
    const auto vs = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
    if (!ctx || w.from < 0 || vs < w.from || vs > w.to)
        return;
    static uint32_t logs = 0;
    if (logs++ >= 512u)
        return;
    try
    {
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint32_t material = mem.read32(0x70000E8Cu);
        const uint32_t source = upload ? material : a1 + (ctx->pc == 0x25AE50u ? 0x10u : 0xA4u);
        uint32_t words[4], previous[4];
        for (unsigned i = 0; i < 4; ++i)
        {
            words[i] = mem.read32(source + i * 4u);
            previous[i] = mem.read32(material + i * 4u);
        }
        std::cerr << "[hubmaterial:" << (upload ? "upload" : "setter") << "] vs=" << vs
                  << " pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31)
                  << " a1=0x" << a1 << " source=0x" << source << " material=0x" << material
                  << " ARGB=" << words[0] << ',' << words[1] << ',' << words[2] << ',' << words[3]
                  << " oldARGB=" << previous[0] << ',' << previous[1] << ',' << previous[2] << ',' << previous[3];
        if (upload)
            std::cerr << " rgbaPayload=0x" << (getRegU32(ctx, 4) + 0x20u)
                      << " savedCaller=0x" << mem.read32(getRegU32(ctx, 29) + 0x30u);
        std::cerr << std::dec << '\n';
    }
    catch (...) { /* TEMP(transformers): diagnostic failures must not change guest exception semantics. */ }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (vaddr == 0x70000E8Cu && ctx && ctx->pc == 0x23BFF8u) // TEMP(transformers)
        ps2WatchHubMaterial(m_memory, ctx, true);
    try
    {
        uint32_t field=0;
        if(vaddr==0x12001000u && ctx && m_memory.readInstantField(vaddr,ctx->pc,field))return field;
        const auto value=m_memory.read32(vaddr);
        if(ctx && vaddr==0x12001000u && (ctx->pc==0x25DF70u || ctx->pc==0x26E9A8u))
            gsTimeline::traceCsr(ctx->pc,m_memory.gs().vsyncTick.load(),value);
        return value;
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

// TEMP(transformers): PS2X_PKTPTR_WATCH=1 logs every store that moves Transformers' DMA packet write pointer (kept at
// scratchpad 0x70000D30) forward by more than 32 KB, with the guest pc/ra - the globe/Options frames reserve ~194 KB
// that never gets written in our runtime.
// Cycle budget per VU1 MSCAL/MSCNT (a safety net against runaway microprograms; real hardware has none). 65536 cut off
// Transformers' level microprograms mid-recursion (they need up to ~2.1M cycles; a cut program is then resumed by the next
// MSCNT on the next batch's data). Default 8M.
// PS2X_VU1_BUDGET=<cycles> overrides.
static uint32_t ps2Vu1CycleBudget()
{
    static const uint32_t s_budget = []() -> uint32_t
    {
        const char *e = std::getenv("PS2X_VU1_BUDGET");
        const unsigned long v = e ? std::strtoul(e, nullptr, 0) : 0ul;
        return v ? static_cast<uint32_t>(v) : (1u << 23); // level programs legitimately need ~2.1M cycles
    }();
    return s_budget;
}

// GAME(transformers) TEMP(qualification): loading-screen frame skip, PS2X_LOAD_SKIP=<N>|auto[:<share>].
// The warp-gate / hub / level-exit loading screen is drawn by the VSync callback 0x535140 -> fn_0026E990 while the main
// thread loads synchronously. Every vsync the callback flips the display field, writes its parameter qword into VU1 data
// memory and starts the VU1 vortex microprogram with CTC2 CMSAR1 at 0x26EB54 (~4M VU1 cycles = ~150-170 ms of host time in
// the interpreter) - >80% of a load's host time (private qualification evidence). VU1 runs synchronously here and charges no EE
// cycles, and nothing on the EE reads the vortex's VU1 state or output, so not running it changes only the picture: the
// callback itself still runs unchanged (frame counter, display flip, VU1 parameter write) and the guest timeline (EE cycles,
// vsyncs, CD reads, MSCAL counters) is the same as with the knob off. A skipped vsync would show a cleared frame and the
// first field of a pair an incomplete one, so those scanouts are not published (GS::rawGuestVsync tags the vsync,
// GSPgsBackend::RawVsync skips the copy) and the window keeps the last complete vortex frame; needs PS2X_GS_BACKEND=PGS.
// The vortex always runs on 2 consecutive callbacks (both interlace fields) so a shown frame is never half old/half new.
//   <N> (2..64): run 2 of every 2N callbacks (deterministic picture).
//   auto[:<share>]: host-paced - after each pair, skip until the host time since that pair ended is at least
//     pairCost * (1 - share) / share, i.e. the vortex takes at most <share> of the host time (default 0.25). A pair that
//     costs under 4 ms (compiled/native vortex) never causes a skip. The picture then depends on host speed; the guest
//     still does not.
//   worker[:<K>[:<N>]] (TEMP(transformers) qualification, release fix; needs the VU1 worker thread PS2X_VU1_THREAD=3):
//     worker-paced - a pair (both fields) is due every 2N callbacks (N default 1 = every vsync; worker:1:4 = never more
//     often than LOAD_SKIP=4) and a due pair waits while more than <K> (default 1) vortex VU1 calls are still
//     queued/running on the VU1 worker, so the vortex is drawn as often as this machine's worker can draw it and the
//     queue never builds up (v0.7.0 with N=4: 7-9 queued vortex calls that the first sun-occlusion readback after the load
//     had to wait for: 0.3-1.1 s). Without the worker it behaves like N=4. Like auto, the picture depends on host speed;
//     the guest does not (same skip mechanics as N: only the VU1 start is skipped).
// Unset/0/1 = off (default).
namespace
{
struct LoadSkipConfig
{
    uint32_t n = 0u;     // fixed pairs mode
    double share = 0.0;  // auto mode when > 0
    int worker = -1;     // TEMP(transformers) qualification: worker-paced mode when >= 0 (max queued vortex calls at a pair start)
    uint32_t workerPeriod = 1u; // TEMP(transformers) qualification: a pair is due every 2*workerPeriod callbacks
};
// TEMP(transformers) qualification: loading-vortex VU1 calls enqueued on the VU1 worker and not finished yet.
std::atomic<int> g_loadSkipVortexPending{0};
LoadSkipConfig ps2LoadSkipConfig()
{
    static const LoadSkipConfig s_cfg = []() -> LoadSkipConfig
    {
        LoadSkipConfig c{};
        const char *e = std::getenv("PS2X_LOAD_SKIP");
        if (!e || !*e)
            return c;
        const char *backendAuto = std::getenv("PS2X_GS_BACKEND");
        if (std::strncmp(e, "auto", 4) == 0 && (!backendAuto || std::strcmp(backendAuto, "PGS") != 0))
        {
            std::cerr << "[loadskip] PS2X_LOAD_SKIP ignored: needs PS2X_GS_BACKEND=PGS" << std::endl;
            return c;
        }
        if (std::strncmp(e, "worker", 6) == 0) // TEMP(transformers) qualification
        {
            const char *thr = std::getenv("PS2X_VU1_THREAD");
            if (!thr || std::strcmp(thr, "3") != 0)
            {
                c.n = 4u;
                std::cerr << "[loadskip] PS2X_LOAD_SKIP=worker needs PS2X_VU1_THREAD=3 - using 4 (2 of every 8 vsyncs)" << std::endl;
                return c;
            }
            c.worker = 1;
            if (e[6] == ':')
            {
                c.worker = std::max(0, std::min(8, std::atoi(e + 7)));
                if (const char *colon = std::strchr(e + 7, ':'))
                    c.workerPeriod = static_cast<uint32_t>(std::max(1, std::min(64, std::atoi(colon + 1))));
            }
            std::cerr << "[loadskip] PS2X_LOAD_SKIP=worker:" << c.worker << ":" << c.workerPeriod
                      << ": loading-screen vortex pair due every " << (2u * c.workerPeriod) << " vsyncs, started when <= "
                      << c.worker << " vortex VU1 calls are queued on the worker" << std::endl;
            return c;
        }
        if (std::strncmp(e, "auto", 4) == 0)
        {
            c.share = 0.25;
            if (e[4] == ':')
            {
                const double v = std::strtod(e + 5, nullptr);
                if (v > 0.0 && v <= 1.0)
                    c.share = v;
            }
            std::cerr << "[loadskip] PS2X_LOAD_SKIP=auto: loading-screen vortex limited to " << c.share
                      << " of host time (pairs of fields)" << std::endl;
            return c;
        }
        const char *backend = std::getenv("PS2X_GS_BACKEND");
        if (!backend || std::strcmp(backend, "PGS") != 0)
        {
            // The loading callback clears the frame every vsync; only the parallel-gs path can hold the last frame
            // (GS::rawGuestVsync), so on the CPU GS the skipped vsyncs would flash black.
            std::cerr << "[loadskip] PS2X_LOAD_SKIP ignored: needs PS2X_GS_BACKEND=PGS" << std::endl;
            return c;
        }
        const unsigned long v = std::strtoul(e, nullptr, 10);
        c.n = static_cast<uint32_t>(std::min<unsigned long>(v, 64ul));
        if (c.n >= 2u)
            std::cerr << "[loadskip] PS2X_LOAD_SKIP=" << c.n << ": loading-screen vortex runs on 2 of every " << (2u * c.n)
                      << " vsyncs" << std::endl;
        else
            c.n = 0u;
        return c;
    }();
    return s_cfg;
}
// EE thread only (the vsync callback); no locking.
struct LoadSkipState
{
    uint64_t calls = 0u, skipped = 0u;
    bool secondOfPair = false;
    std::chrono::steady_clock::time_point pairStart{}, lastPairEnd{}, renderStart{};
    double lastPairSeconds = 0.0;
    bool havePair = false;
    uint32_t sincePairStart = 1u << 20; // TEMP(transformers) qualification: callbacks since the last worker-paced pair started
} g_loadSkip;
} // namespace

// true = do not run the vortex this time. holdNextScanout = the next PCRTC scanout would not show a complete pair.
static bool ps2LoadScreenSkipVu1Begin(bool &holdNextScanout)
{
    holdNextScanout = false;
    const LoadSkipConfig cfg = ps2LoadSkipConfig();
    if (cfg.n == 0u && cfg.share <= 0.0 && cfg.worker < 0)
        return false;
    LoadSkipState &st = g_loadSkip;
    const uint64_t k = st.calls++;
    bool skip = false;
    if (cfg.worker >= 0) // TEMP(transformers) qualification: worker-paced pairs (second field always follows a drawn first)
    {
        if (st.secondOfPair)
        {
            st.secondOfPair = false;
            holdNextScanout = false;
        }
        else
        {
            const bool due = st.sincePairStart + 1u >= 2u * cfg.workerPeriod;
            skip = !due || g_loadSkipVortexPending.load(std::memory_order_acquire) > cfg.worker;
            st.secondOfPair = !skip;
            holdNextScanout = true;
        }
        st.sincePairStart = (!skip && st.secondOfPair) ? 0u : st.sincePairStart + 1u; // first field of a new pair = 0
        if (skip)
            ++st.skipped;
        if ((st.calls & 1023u) == 0u)
            std::cerr << "[loadskip] vortex calls=" << st.calls << " skipped=" << st.skipped << " (worker-paced)" << std::endl;
        return skip;
    }
    if (cfg.n != 0u)
        skip = ((k / 2u) % cfg.n) != 0u;
    else if (!st.secondOfPair && st.havePair && st.lastPairSeconds >= 0.004)
    {
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - st.lastPairEnd).count();
        // A long gap (> 2 s) means a new loading screen started: render at once.
        skip = waited < st.lastPairSeconds * (1.0 - cfg.share) / cfg.share && waited < 2.0;
    }
    if (skip)
        ++st.skipped;
    else
        st.renderStart = std::chrono::steady_clock::now();
    // First field of a pair: (k even) in fixed mode, not the pair's second call in auto mode.
    holdNextScanout = skip || (cfg.n != 0u ? (k % 2u) == 0u : !st.secondOfPair);
    if ((st.calls & 1023u) == 0u)
        std::cerr << "[loadskip] vortex calls=" << st.calls << " skipped=" << st.skipped
                  << " lastPairMs=" << static_cast<int>(st.lastPairSeconds * 1000.0) << std::endl;
    return skip;
}

static void ps2LoadScreenSkipVu1End()
{
    const LoadSkipConfig cfg = ps2LoadSkipConfig();
    if (cfg.share <= 0.0)
        return;
    LoadSkipState &st = g_loadSkip;
    const auto now = std::chrono::steady_clock::now();
    const double cost = std::chrono::duration<double>(now - st.renderStart).count();
    if (!st.secondOfPair)
    {
        st.pairStart = st.renderStart;
        st.lastPairSeconds = cost; // first field; completed below
        st.secondOfPair = true;
        return;
    }
    st.lastPairSeconds += cost;
    st.lastPairEnd = now;
    st.havePair = true;
    st.secondOfPair = false;
}

// COP2 CMSAR1 starts VU1 directly; it is not a VIF MSCAL and must not toggle DBF.
// Use the existing synchronous VU execution model, preserving VIF TOP/ITOP.
void ps2StartVu1Cmsar1(PS2Runtime *runtime, R5900Context *ctx, uint32_t value)
{
    ctx->vu0_cmsar1 = value;
    const bool loadVortex = ctx->pc == 0x26EB54u; // GAME(transformers) TEMP(qualification): fn_0026E990 loading vortex
    extern std::atomic<bool> g_ps2LoadSkipHoldScanout; // gs_frontend.cpp: keep showing the last complete vortex frame
    bool holdNextScanout = false;
    if (loadVortex && ps2LoadScreenSkipVu1Begin(holdNextScanout))
    {
        ctx->vu0_vpu_stat &= ~0x0600u; // as after a normal vortex run (ends on its E bit, not stopped by D/T)
        g_ps2LoadSkipHoldScanout.store(true, std::memory_order_relaxed);
        return;
    }
    auto &mem = runtime->memory();
    auto &vu = runtime->vu1();
    if(mem.m_vif1Instant) {
        const auto ctl=ctx->vu0_fbrst,top=mem.vif1_regs.top,itop=mem.vif1_regs.itop;
        if(loadVortex)g_loadSkipVortexPending.fetch_add(1,std::memory_order_acq_rel); // TEMP(transformers) qualification
        mem.m_vif1Instant->enqueue([runtime,&mem,&vu,value,top,itop,ctl,loadVortex] {
            struct PendingDone{bool on;~PendingDone(){if(on)g_loadSkipVortexPending.fetch_sub(1,std::memory_order_acq_rel);}} done{loadVortex}; // TEMP(transformers) qualification
            vu.state().dBitEnabled=(ctl&0x400u)!=0;vu.state().tBitEnabled=(ctl&0x800u)!=0;
            vu.execute(mem.getVU1Code(),PS2_VU1_CODE_SIZE,mem.getVU1Data(),PS2_VU1_DATA_SIZE,runtime->gs(),&mem,(value&0x7FFu)*8u,top,itop,ps2Vu1CycleBudget());
            mem.m_vif1Instant->stopBits=(vu.state().stoppedByD?0x200u:0u)|(vu.state().stoppedByT?0x400u:0u);
        },false,ctl);
        if(ctl&0xC00u)mem.m_vif1Instant->fence();
        else if(mem.m_vif1Instant->publishStatus)mem.m_vif1Instant->publishStatus(0);
        if (loadVortex) // v5 merge (release fix x release fix): keep the lane-loads pair accounting / scanout hold in mode 3
        {
            ps2LoadScreenSkipVu1End();
            if (holdNextScanout)
                g_ps2LoadSkipHoldScanout.store(true, std::memory_order_relaxed);
        }
        return;
    }
    vu.state().dBitEnabled = (ctx->vu0_fbrst & (1u << 10)) != 0u;
    vu.state().tBitEnabled = (ctx->vu0_fbrst & (1u << 11)) != 0u;
    const uint64_t primsBefore = ps2dbg::primsDrawn.load(std::memory_order_relaxed); // TEMP(transformers)
    vu.execute(mem.getVU1Code(), PS2_VU1_CODE_SIZE,
               mem.getVU1Data(), PS2_VU1_DATA_SIZE,
               runtime->gs(), &mem, (value & 0x7ffu) * 8u,
               mem.vif1_regs.top, mem.vif1_regs.itop, ps2Vu1CycleBudget());
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) |
                       ((!Vu1Worker::enabled() || (ctx->vu0_fbrst & (1u << 10))) && vu.state().stoppedByD ? 0x0200u : 0u) |
                       ((!Vu1Worker::enabled() || (ctx->vu0_fbrst & (1u << 11))) && vu.state().stoppedByT ? 0x0400u : 0u);
    if (loadVortex)
    {
        ps2LoadScreenSkipVu1End(); // TEMP(qualification): pair cost for PS2X_LOAD_SKIP=auto
        if (holdNextScanout)
            g_ps2LoadSkipHoldScanout.store(true, std::memory_order_relaxed);
    }
    { // TEMP(transformers): warp-gate loading screen diagnostics (private qualification evidence)
        static std::atomic<uint32_t> s_calls{0};
        const uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 12 || (n % 60) == 0)
        {
            const uint32_t *code = reinterpret_cast<const uint32_t *>(mem.getVU1Code());
            const uint32_t *data = reinterpret_cast<const uint32_t *>(mem.getVU1Data());
            std::cerr << "[vu1:cmsar1] n=" << n << " vs=" << ps2dbg::vsyncTick.load() << " value=0x" << std::hex << value
                      << " pc0=0x" << ((value & 0x7ffu) * 8u) << " code[0..3]=" << code[0] << "," << code[1] << "," << code[2]
                      << "," << code[3] << " data[0x3FF0/4..]=" << data[0xFFC] << "," << data[0xFFD] << "," << data[0xFFE]
                      << "," << data[0xFFF] << " top=0x" << mem.vif1_regs.top << " itop=0x" << mem.vif1_regs.itop
                      << std::dec << " stopD=" << vu.state().stoppedByD << " stopT=" << vu.state().stoppedByT
                      << " prims+=" << (ps2dbg::primsDrawn.load(std::memory_order_relaxed) - primsBefore) << std::endl;
        }
    }
}

// TEMP(transformers): PS2X_SPRWATCH=1 logs guest stores into scratchpad 0x70000630-0x7000067F (the matrix block whose
// pointer at 0x7000067C feeds the level's field-2 matrix M2) with guest pc/ra - something clobbers it with 0xFFE0E0xx.
static void ps2WatchSprMatrix(const R5900Context *ctx, uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t hi)
{
    static const long long s_from = []() { const char *e = std::getenv("PS2X_SPRWATCH"); return e ? std::atoll(e) : -1ll; }();
    // PS2X_SPRWATCH=<from vsync>: the set-view inputs SPR 0x760-0x79F and 0xF00-0xF3F (M2 = 0x760 x 0xF00, copied to 0x630)
    const bool inA = vaddr + size > 0x70000760u && vaddr < 0x700007A0u;
    const bool inB = vaddr + size > 0x70000F00u && vaddr < 0x70000F40u;
    if (s_from < 0 || !ctx || !(inA || inB) || static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)) < s_from)
        return;
    auto hasColourWord = [](uint64_t v)
    {
        return ((v & 0xFFFFFF00ull) == 0xFFE0E000ull) || (((v >> 32) & 0xFFFFFF00ull) == 0xFFE0E000ull);
    };
    if (!hasColourWord(lo) && !(size == 16u && hasColourWord(hi)))
        return; // only the 0xFFE0E0xx colour words that end up in M2
    static uint32_t s_logs = 0u;
    if (s_logs++ >= 400u)
        return;
    std::cerr << "[sprwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " addr=0x" << std::hex << vaddr
              << " size=" << std::dec << size << " pc=0x" << std::hex << ctx->pc << " ra=0x"
              << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) << " val=" << lo;
    if (size == 16u)
        std::cerr << ":" << hi;
    std::cerr << std::dec << std::endl;
}

static void ps2WatchPacketPointer(PS2Memory &mem, const R5900Context *ctx, uint32_t vaddr, uint32_t newValue)
{
    static const bool s_on = std::getenv("PS2X_PKTPTR_WATCH") != nullptr;
    if (!s_on || vaddr != 0x70000D30u || !ctx)
        return;
    static uint32_t s_logs = 0u;
    const uint32_t oldValue = mem.read32(0x70000D30u);
    const uint32_t oldPhys = oldValue & 0x01FFFFFFu;
    const uint32_t newPhys = newValue & 0x01FFFFFFu;
    if (newPhys > oldPhys + 0x8000u && s_logs++ < 200u)
        std::cerr << "[pktptr] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " pc=0x" << std::hex << ctx->pc
                  << " ra=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) << " old=0x" << oldValue << " new=0x"
                  << newValue << " jump=0x" << (newPhys - oldPhys) << std::dec << std::endl;
}

// TEMP(transformers): PS2X_EE_WORDWATCH (see ps2_runtime.h) - the hook is active only when the variable is set.
static const struct EeWordWatchSpec
{
    uint32_t words[4]{};
    unsigned count = 0;
    EeWordWatchSpec()
    {
        const char *e = std::getenv("PS2X_EE_WORDWATCH");
        if (!e)
            return;
        char *n = nullptr;
        for (unsigned i = 0; i < 4u; ++i)
        {
            words[i] = static_cast<uint32_t>(std::strtoul(i == 0 ? e : n + 1, &n, 16));
            ++count;
            if (!n || *n != ',')
                break;
        }
    }
} g_eeWordWatchSpec;
bool g_ps2EeWordWatchOn = g_eeWordWatchSpec.count != 0;
// TEMP(transformers): startup-only environment, matching watcher presence semantics (including "0").
static const bool g_ps2StoreWatchOn = std::getenv("PS2X_EE_COLORWATCH") != nullptr ||
    std::getenv("PS2X_PKTPTR_WATCH") != nullptr || std::getenv("PS2X_SPRWATCH") != nullptr;

void ps2EeWordWatchHit(uint32_t guestAddr, uint32_t size, uint64_t valueLo, uint64_t valueHi, const R5900Context *ctx)
{
    const uint32_t lanes[4] = {static_cast<uint32_t>(valueLo), static_cast<uint32_t>(valueLo >> 32),
                               static_cast<uint32_t>(valueHi), static_cast<uint32_t>(valueHi >> 32)};
    const unsigned laneCount = size >= 16u ? 4u : size >= 8u ? 2u : 1u;
    { // TEMP(transformers): PS2X_EE_ADDRWATCH=<hex addr> logs every store (any width) that covers that guest address.
        static const uint32_t s_addrWatch = []() -> uint32_t
        {
            const char *e = std::getenv("PS2X_EE_ADDRWATCH");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
        }();
        const uint32_t phys = guestAddr & 0x1FFFFFFFu;
        const uint32_t span = size < 1u ? 1u : size;
        if (s_addrWatch != 0u && phys <= s_addrWatch && s_addrWatch < phys + span)
        {
            static uint32_t s_addrLogs = 0u;
            if (s_addrLogs++ < 400u)
                std::cerr << "[eeaddrwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " size=" << size
                          << " addr=0x" << std::hex << guestAddr << " pc=0x" << (ctx ? ctx->pc : 0u) << " ra=0x"
                          << (ctx ? static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) : 0u) << " v=" << lanes[0] << ","
                          << lanes[1] << "," << lanes[2] << "," << lanes[3] << std::dec << std::endl;
        }
    }
    bool hit = false;
    for (unsigned l = 0; l < laneCount && !hit; ++l)
        for (unsigned i = 0; i < g_eeWordWatchSpec.count; ++i)
            if (lanes[l] == g_eeWordWatchSpec.words[i] && (size >= 4u))
            {
                hit = true;
                break;
            }
    if (!hit)
        return;
    static const std::pair<long long, long long> s_vsWindow = []() // PS2X_EE_WORDWATCH_VS=<from>-<to>
    {
        const char *e = std::getenv("PS2X_EE_WORDWATCH_VS");
        if (!e)
            return std::pair<long long, long long>{0, 1ll << 62};
        char *n = nullptr;
        const long long lo = std::strtoll(e, &n, 10);
        return std::pair<long long, long long>{lo, (n && *n == '-') ? std::strtoll(n + 1, nullptr, 10) : lo};
    }();
    const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
    if (vsNow < s_vsWindow.first || vsNow > s_vsWindow.second)
        return;
    static uint32_t s_logs = 0u;
    if (s_logs++ >= 2000u)
        return;
    const uint32_t pc = ctx ? ctx->pc : 0u;
    const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) : 0u;
    std::cerr << "[eewordwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " size=" << size << " addr=0x"
              << std::hex << guestAddr << " pc=0x" << pc << " ra=0x" << ra << " v=" << lanes[0] << "," << lanes[1] << ","
              << lanes[2] << "," << lanes[3] << std::dec << std::endl;
}

// TEMP(transformers): PS2X_EE_COLORWATCH=1 logs EE stores into VU1 data memory (slow Store path, the game writes
// VU1 memory directly) that carry the blue-cast colour lanes (5,0,64,100) / (100,64,0,5) in any width: pc/ra/args.
static void ps2WatchEeColor(const R5900Context *ctx, uint32_t vaddr, uint32_t size, uint64_t lo, uint64_t hi)
{
    static const bool s_on = std::getenv("PS2X_EE_COLORWATCH") != nullptr;
    if (!s_on || !ctx)
        return;
    const uint32_t phys = vaddr & 0x1FFFFFFFu;
    if (phys < 0x1100C000u || phys >= 0x11010000u)
        return;
    bool hit = false;
    if (size == 4u)
        hit = (lo == 100u) || (lo == 5u);
    else if (size == 8u)
        hit = lo == ((100ull << 32) | 64ull) || lo == ((5ull << 32) | 0ull) || lo == ((64ull << 32) | 100ull) ||
              lo == ((5ull << 32) | 0ull) || lo == ((0ull << 32) | 5ull) || lo == ((100ull << 32) | 5ull);
    else
        hit = (lo == ((0ull << 32) | 5ull) && hi == ((100ull << 32) | 64ull)) ||
              (lo == ((64ull << 32) | 100ull) && hi == ((5ull << 32) | 0ull));
    if (!hit)
        return;
    static uint32_t s_logs = 0u;
    if (s_logs++ >= 80u)
        return;
    auto gpr = [&](int r) { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[r])); };
    std::cerr << "[eecolorwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " size=" << size << " addr=0x"
              << std::hex << vaddr << " pc=0x" << ctx->pc << " ra=0x" << gpr(31) << " a0=0x" << gpr(4) << " a1=0x" << gpr(5)
              << " a2=0x" << gpr(6) << " a3=0x" << gpr(7) << " t0=0x" << gpr(8) << " v0=0x" << gpr(2) << " v1=0x" << gpr(3)
              << " lo=0x" << lo << " hi=0x" << hi << std::dec << std::endl;
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    if (vaddr == 0x70000460u && ctx && (ctx->pc == 0x25AE50u || ctx->pc == 0x2592A4u)) // TEMP(transformers)
        ps2WatchHubMaterial(m_memory, ctx, false);
    if (g_ps2StoreWatchOn) ps2WatchEeColor(ctx, vaddr, 4u, value, 0u); // TEMP(transformers)
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    if (g_ps2StoreWatchOn) ps2WatchPacketPointer(m_memory, ctx, vaddr, value); // TEMP(transformers)
    if (g_ps2StoreWatchOn) ps2WatchSprMatrix(ctx, vaddr, 4u, value, 0u);        // TEMP(transformers)
    try
    {
        m_memory.write32(vaddr, value);
        if (ps2drain::stats) ps2drain::store();
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    if (g_ps2StoreWatchOn) ps2WatchEeColor(ctx, vaddr, 8u, value, 0u); // TEMP(transformers)
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    if (g_ps2StoreWatchOn) ps2WatchPacketPointer(m_memory, ctx, vaddr, static_cast<uint32_t>(value)); // TEMP(transformers)
    if (g_ps2StoreWatchOn) ps2WatchSprMatrix(ctx, vaddr, 8u, value, 0u);                                // TEMP(transformers)
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    if (g_ps2StoreWatchOn) ps2WatchEeColor(ctx, vaddr, 16u, _parts[0], _parts[1]); // TEMP(transformers)
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    if (g_ps2StoreWatchOn) ps2WatchPacketPointer(m_memory, ctx, vaddr, static_cast<uint32_t>(_parts[0])); // TEMP(transformers)
    if (g_ps2StoreWatchOn) ps2WatchSprMatrix(ctx, vaddr, 16u, _parts[0], _parts[1]);                        // TEMP(transformers)
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    ps2_eeqos::tuneProcess(); // TEMP(transformers)
    ps2_profile::Session profileSession; // TEMP(transformers): stops after gameThread joins.
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    // Main-thread stream lifetime ends before the IOP subsystem/device can be destroyed.
    PS2HostAudio hostAudio([this](int16_t *samples, size_t frames) {
        return PS2IopTransport::drainAudio(this, samples, frames);
    });
    std::atomic<bool> gameThreadFinished{false};
    // TEMP(transformers) qualification: render (this) thread + reporter; audio stats until hostAudio goes away (below).
    ps2_perfscope::registerThread(ps2_perfscope::LaneRender);
    ps2_perfscope::setAudioStatsProvider([](void *user, ps2_perfscope::AudioStats &out)
                                         { return static_cast<const PS2HostAudio *>(user)->perfStats(out); },
                                         &hostAudio);
    ps2_perfscope::start();

    ps2_test::init(*this); // TEMP(transformers): PS2X_SCRIPT / PS2X_STOP_AT_VS / PS2X_STALL_VS harness (Kernel/TestScript.cpp)

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        RegisterThreadForCrashReporter("EE/game");
        CrashReporterSelfTestIfRequested(); // TEMP(transformers): PS2X_CRASH_TEST
        ps2_profile::registerThread("EE/game"); // TEMP(transformers)
        ps2_perfscope::registerThread(ps2_perfscope::LaneEE); // TEMP(transformers) qualification
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
            ps2_test::onGameThreadError(e.what()); // TEMP(transformers): PS2X_SCRIPT verdict CRASH (5)
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
            ps2_test::onGameThreadError("unknown exception"); // TEMP(transformers): PS2X_SCRIPT verdict CRASH (5)
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        // TEMP(transformers): host iteration continues even when no FIELD60 selection is due.
        uint64_t traceSeq=0,traceTick=0; int traceKind=0;
        if(ps2PresentTrace::enabled()) ps2Field60LastPresented(traceSeq,traceTick,traceKind);
        ps2PresentTrace::Scope traceHost(ps2PresentTrace::HostFrame,traceTick,traceSeq);
        {
            ps2_perfscope::Scope perf(ps2_perfscope::RnAudioPump); // TEMP(transformers) lane perfscope
            hostAudio.pump();
        }
        if (const uint32_t hsInterval = heapscopeIntervalVs()) // TEMP(transformers) qualification: PS2X_HEAPSCOPE
        {
            static uint64_t s_hsNextVs = 0u;
            static uint32_t s_hsMinFree = 0xFFFFFFFFu, s_hsMinLargest = 0xFFFFFFFFu, s_hsAllMinFree = 0xFFFFFFFFu;
            static bool s_hsHaltReported = false;
            const uint8_t *hsRam = m_memory.getRDRAM();
            const uint64_t hsVs = m_eeScheduler->currentVSyncTick();
            const uint32_t hsMain = heapscopeRead32(hsRam, 0x006CE4C4u);
            uint32_t hsFree = 0u, hsLargest = 0u;
            const bool hsValid = heapscopeTdgHeap(hsRam, hsMain, hsFree, hsLargest);
            if (hsValid)
            {
                s_hsMinFree = std::min(s_hsMinFree, hsFree);
                s_hsMinLargest = std::min(s_hsMinLargest, hsLargest);
                s_hsAllMinFree = std::min(s_hsAllMinFree, hsFree);
            }
            const bool hsHalted = (m_debugPc.load(std::memory_order_relaxed) == 0x00256220u); // game's fatal Message Dialog spin
            if (hsVs >= s_hsNextVs || (hsHalted && !s_hsHaltReported))
            {
                std::ostringstream hs;
                hs << "[heapscope] vs=" << hsVs << " pads=" << ps2dbg::padReads.load(std::memory_order_relaxed);
                if (hsValid)
                {
                    hs << " tdg main=0x" << std::hex << hsMain << std::dec << " free=" << hsFree << " largest=" << hsLargest
                       << " minFree=" << s_hsMinFree << " minLargest=" << s_hsMinLargest << " allMinFree=" << s_hsAllMinFree;
                }
                else
                {
                    hs << " tdg main=none";
                }
                const uint32_t hsCount = std::min<uint32_t>(heapscopeRead32(hsRam, 0x006CE4ACu), 8u);
                hs << " heaps=" << hsCount << " [";
                for (uint32_t i = 0; i < hsCount; ++i)
                {
                    const uint32_t h = heapscopeRead32(hsRam, 0x006D1FD0u + 4u * i);
                    uint32_t f = 0u, l = 0u;
                    hs << (i ? " " : "") << "0x" << std::hex << h << std::dec;
                    if (heapscopeTdgHeap(hsRam, h, f, l))
                    {
                        hs << ":" << f << "/" << l;
                    }
                }
                hs << "]";
                {
                    std::lock_guard<std::mutex> hsLock(m_guestHeapMutex);
                    uint64_t hleFree = 0u, hleUsed = 0u;
                    uint32_t hleLargest = 0u, hleUsedBlocks = 0u;
                    for (const GuestHeapBlock &b : m_guestHeapBlocks)
                    {
                        if (b.free)
                        {
                            hleFree += b.size;
                            hleLargest = std::max(hleLargest, b.size);
                        }
                        else
                        {
                            hleUsed += b.size;
                            ++hleUsedBlocks;
                        }
                    }
                    hs << " | hle base=0x" << std::hex << m_guestHeapBase << " limit=0x" << m_guestHeapLimit << std::dec
                       << " free=" << hleFree << " largest=" << hleLargest << " used=" << hleUsed << " usedBlocks=" << hleUsedBlocks;
                }
                if (hsHalted && !s_hsHaltReported)
                {
                    hs << " | HALTED at 0x256220 (game's fatal Message Dialog loop, e.g. OUT OF MEMORY)";
                    s_hsHaltReported = true;
                }
                std::cerr << hs.str() << std::endl;
                s_hsMinFree = 0xFFFFFFFFu;
                s_hsMinLargest = 0xFFFFFFFFu;
                s_hsNextVs = hsVs + hsInterval;
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
                { // TEMP(transformers): thread wait states + IRQ/GS/semaphore/pad counters
                    std::ostringstream threads;
                    for (const auto &t : eeSnapshot.threads)
                    {
                        threads << " T" << t.id << "{st=" << static_cast<int>(t.status)
                                << " wait=" << static_cast<int>(t.waitReason) << ":" << t.waitId
                                << " pc=0x" << std::hex << t.pc << " ra=0x" << t.ra << std::dec
                                << " pri=" << t.currentPriority << " inv=" << t.invocationDepth << "}";
                    }
                    std::cout << "[run:dbg] tick=" << tick << " vsync=" << m_eeScheduler->currentVSyncTick()
                              << threads.str() << ps2dbg::summary()
                              << " csr=0x" << std::hex << gs.csr.load() << " imrReg=0x" << gs.imr << std::dec << std::endl;
                }

            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        {
            ps2_perfscope::Scope perf(ps2_perfscope::RnUpload); // TEMP(transformers) lane perfscope
            ps2PresentTrace::Scope traceStage(ps2PresentTrace::Upload,traceTick,traceSeq); // TEMP(transformers)
            UploadFrame(frameTex, this, presentWidth, presentHeight);
        }
        {
            ps2_perfscope::Scope perf(ps2_perfscope::RnUpscale); // TEMP(transformers) lane perfscope
            ps2PresentTrace::Scope traceStage(ps2PresentTrace::Capture,traceTick,traceSeq); // TEMP(transformers)
            ps2WidescreenUpdateWindow();
            ps2PresentUpscaleCapture(frameTex, Rectangle{0.0f, 0.0f, static_cast<float>(std::max<uint32_t>(1u, presentWidth)),
                                                         static_cast<float>(std::max<uint32_t>(1u, presentHeight))}); // TEMP(transformers)
        }

        if(ps2PresentTrace::enabled()) ps2Field60LastPresented(traceSeq,traceTick,traceKind); // TEMP(transformers)
        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // Present at the PS2's display aspect (4:3 by default, PS2X_PRESENT_ASPECT), not at square pixels: fit inside
        // the window, letter/pillarbox the rest. The upscale pass handles the non-uniform x/y scale.
        // PS2X_WIDESCREEN=<aspect>: the guest renders Hor+ into the same framebuffer -> present at that aspect
        // (full-screen FMVs, which were not widened, stay 4:3 pillarboxed; PS2X_WIDESCREEN_PILLARBOX=0 stretches).
        const float dispAspect = ps2PresentFrameAspect(srcWidth);
        const float srcAspectW = (dispAspect > 0.0f) ? srcHeight * dispAspect : srcWidth;
        const float scale = std::min(screenWidth / srcAspectW, screenHeight / srcHeight);
        const float dstWidth = srcAspectW * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        {
            ps2_perfscope::Scope perf(ps2_perfscope::RnUpscale); // TEMP(transformers) lane perfscope
            ps2PresentTrace::Scope traceStage(ps2PresentTrace::Draw,traceTick,traceSeq); // TEMP(transformers)
            ps2ResolutionPresent(frameTex, srcRect, dstRect); // PS2X_UPSCALE (off = the old DrawTexturePro stretch)
        }
        { // TEMP(transformers) lane perfscope: UI block
            ps2PresentTrace::Scope traceStage(ps2PresentTrace::Ui,traceTick,traceSeq); // TEMP(transformers)
        ps2_perfscope::Scope perfUi(ps2_perfscope::RnUi); // TEMP(transformers) lane perfscope: debug UI + playtest overlays
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        // TEMP(transformers): playtest F12 mark (+ screenshot of the presented frame) / F11 overlay.
        ps2_playtest::frame(frameTex, presentWidth, presentHeight, m_eeScheduler->currentVSyncTick(),
                            ps2dbg::padReads.load(std::memory_order_relaxed),
                            [](void *user) -> std::string
                            {
                                PS2Runtime *rt = static_cast<PS2Runtime *>(user);
                                std::ostringstream os;
                                os << "ee pc=0x" << std::hex << rt->m_debugPc.load(std::memory_order_relaxed)
                                   << " ra=0x" << rt->m_debugRa.load(std::memory_order_relaxed) << std::dec
                                   << " cur=T" << rt->m_eeScheduler->currentThreadId() << " threads:";
                                const auto snap = rt->m_eeScheduler->snapshot();
                                for (const auto &t : snap.threads)
                                    os << " T" << t.id << "{st=" << static_cast<int>(t.status)
                                       << " wait=" << static_cast<int>(t.waitReason) << ":" << t.waitId
                                       << " pc=0x" << std::hex << t.pc << " ra=0x" << t.ra << std::dec << "}";
                                return os.str();
                            },
                            this);
        } // TEMP(transformers) lane perfscope: end of the UI block
        // TEMP(transformers): deterministic final-framebuffer evidence for hires1 (never desktop capture).
        {
            ps2PresentTrace::Scope traceCapture(ps2PresentTrace::HiresCapture,traceTick,traceSeq); // TEMP(transformers)
            static const bool captureWindow=[](){if(std::getenv("PS2X_HIRES_CAPTURE_DIR"))SetWindowSize(1600,1200);return true;}();
            (void)captureWindow;
            static std::vector<uint64_t> captureVs=[](){std::vector<uint64_t> v;
                if(const char* e=std::getenv("PS2X_HIRES_CAPTURE_VS")){std::istringstream in(e);std::string x;
                    while(std::getline(in,x,','))v.push_back(std::strtoull(x.c_str(),nullptr,10));}return v;}();
            const auto vs=m_eeScheduler->currentVSyncTick();
            if(!captureVs.empty() && vs>=captureVs.front() && presentWidth>0) {
                rlDrawRenderBatchActive();Image image=LoadImageFromScreen();
                const char* dir=std::getenv("PS2X_HIRES_CAPTURE_DIR");char path[1024];
                std::snprintf(path,sizeof(path),"%s/present_vs%llu_%ux%u.png",dir?dir:".",(unsigned long long)vs,presentWidth,presentHeight);
                ExportImage(image,path);UnloadImage(image);captureVs.erase(captureVs.begin());
                std::fprintf(stderr,"[hires1:capture] vs=%llu scanout=%ux%u final=%dx%d path=%s\n",(unsigned long long)vs,presentWidth,presentHeight,GetScreenWidth(),GetScreenHeight(),path);
            }
        }
        { // TEMP(transformers) lane perfscope: time the swap (+ raylib frame wait + input poll)
            ps2_perfscope::Scope perfSwap(ps2_perfscope::RnSwap);
            { ps2PresentTrace::Scope traceStage(ps2PresentTrace::EndDrawing,traceTick,traceSeq);
              if(ps2PresentTrace::enabled()) { ps2PresentTrace::rayTick=traceTick; ps2PresentTrace::raySeq=traceSeq;
                  ps2xPresentTraceHook=ps2PresentTrace::rayEvent; }
              EndDrawing(); } // TEMP(transformers)
            // TEMP(transformers): swap-return clock for the exact FIELD60 selection, not GetPicture time.
            static const bool audioSwapTrace=std::getenv("PS2X_AUDIO_VIDEO_TRACE")!=nullptr;
            static uint64_t audioLastSwapSeq=0;
            if(audioSwapTrace) {
                uint64_t seq=0,tick=0;int kind=0;ps2Field60LastPresented(seq,tick,kind);
                if(seq && seq!=audioLastSwapSeq) {
                    std::fprintf(stderr,"[aud2:frame-end] steady_us=%lld seq=%llu tick=%llu\n",
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                        (unsigned long long)seq,(unsigned long long)tick);audioLastSwapSeq=seq;
                }
            }
        }
        {
            ps2_perfscope::Scope perf(ps2_perfscope::RnPad); // TEMP(transformers) lane perfscope
            ps2PresentTrace::Scope traceStage(ps2PresentTrace::Pad,traceTick,traceSeq); // TEMP(transformers)
            ps2PadSampleHostInput(); // TEMP(transformers): EndDrawing polled input; latch presses for the next game pad read
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    ps2PadStopHostRumble(); // TEMP(transformers)
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    PS2IopTransport::finishAudioCapture(this); // TEMP(transformers): joined producer; flush before _Exit
    ps2_perfscope::stop();                                // TEMP(transformers) qualification: final TOTAL line
    ps2_perfscope::setAudioStatsProvider(nullptr, nullptr); // before hostAudio is destroyed

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    ps2ResolutionPresentShutdown();
    ps2PresentUpscaleShutdown();
    UnloadTexture(frameTex);
    CloseWindow();

    ps2_vu1new::shutdown(); // TEMP(transformers): main uses _Exit, bypassing static destructors
    RUNTIME_LOG("[run] exiting loop");
}
