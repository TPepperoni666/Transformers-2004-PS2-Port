// Third-party adaptation notice: GPL-3.0; ordered VBlank snapshot adapted from Scotho/socom-unzipped and Sinan-Karakaya PS2Recomp PR #254 (5d5dfe3).
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_gs_timeline.h"
#include "../debug_counters.h" // TEMP(transformers)
#include "../ps2_perfscope.h"  // TEMP(transformers): qualification
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_pgs_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "ps2_log.h"
#include "ps2_glyph_upload.h"
#include <map>
#include "runtime/ps2_memory.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

// GAME(transformers): keep glyph state out of PCH headers. A runtime owns one GS;
// map entries are reused/reset with GS::reset (also handles multiple offline GS instances).
static ps2_glyphs::UploadState &glyphState(const GS *gs)
{
    static std::mutex mutex;
    static std::map<const GS *, ps2_glyphs::UploadState> states;
    thread_local const GS *cachedOwner = nullptr;
    thread_local ps2_glyphs::UploadState *cachedState = nullptr;
    if (cachedOwner == gs) return *cachedState;
    std::lock_guard<std::mutex> lock(mutex);
    cachedOwner = gs; cachedState = &states[gs];
    return *cachedState;
}
static thread_local const GS *glyphSubmissionOwner = nullptr;
struct GlyphGuard
{
    bool &flag;
    const GS *previous;
    GlyphGuard(bool &v, const GS *owner) : flag(v), previous(glyphSubmissionOwner)
    { flag=true; glyphSubmissionOwner=owner; }
    ~GlyphGuard() { flag=false; glyphSubmissionOwner=previous; }
};

static void emitGlyphUpload(GS *gs, ps2_glyphs::UploadState &glyph,
    const ps2_glyphs::Upload &u, const std::vector<uint8_t> &payload)
{
    const auto packet = ps2_glyphs::uploadPacket(u, payload);
    gs->processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
    if (u.blt != glyph.regs.blt || u.pos != glyph.regs.pos || u.reg != glyph.regs.reg || glyph.regs.dir != 0) {
        const auto restore = ps2_glyphs::restorePacket(glyph.regs);
        gs->processGIFPacket(restore.data(), static_cast<uint32_t>(restore.size()));
    }
    // TEMP(transformers): opt-in upload diagnostics; no guest timing/counters.
    if (const char *log = std::getenv("PS2X_GLYPHS_LOG"); log && *log == '1')
        std::fprintf(stderr, "[glyphs] upload dbp=%llu bytes=%zu style=%d\n",
            static_cast<unsigned long long>((u.blt >> 32) & 0x3fff), payload.size(),
            static_cast<int>(ps2_glyphs::selected(ps2_glyphs::snapshot().style)));
}

namespace
{
    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}


// TEMP(transformers): M1 parallel-gs without PS2X_GS_SHADOW. The raw packet already went to the backend; the EE only
// needs the register writes with EE-visible side effects: SIGNAL/FINISH/LABEL (CSR, SIGLBLID), the transfer registers
// (local->host bookkeeping for consumeLocalToHostBytes) and the libgs HLE privileged aliases 0x59-0x5f. Skipping the
// full decode (vertex queue, draw batches, debug history) removes the per-packet frontend cost; the loading screens
// send ~200k tiny packets per vsync (PGS_M1.md). GIF-tag walk mirrors processGIFPacket (PACKED A+D, REGLIST, IMAGE).
template <typename WriteFn>
static void scanRawGifForEeState(const uint8_t *data, uint32_t sizeBytes, WriteFn &&write)
{
    auto interesting = [](uint32_t addr) {
        return (addr >= GS_REG_BITBLTBUF && addr <= GS_REG_TRXDIR) || (addr >= 0x59u && addr <= 0x5Cu) || addr == 0x5Fu ||
               addr == GS_REG_SIGNAL || addr == GS_REG_FINISH || addr == GS_REG_LABEL;
    };
    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        const uint64_t tagLo = loadLE64(data + offset);
        const uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    if (((tagHi >> (r * 4)) & 0xF) == 0x0E)
                    {
                        const uint32_t addr = static_cast<uint32_t>(loadLE64(data + offset + 8) & 0xFF);
                        if (interesting(addr))
                            write(static_cast<uint8_t>(addr), loadLE64(data + offset));
                    }
                    offset += 16;
                }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            // REGLIST descriptors are 0x0-0xF: none of them is EE-visible.
            const uint64_t words = uint64_t(nloop) * nreg;
            offset += static_cast<uint32_t>(std::min<uint64_t>((words + (words & 1)) * 8ull, sizeBytes - offset));
        }
        else
        {
            const uint32_t imageBytes = std::min<uint32_t>(nloop * 16u, sizeBytes - offset);
            offset += imageBytes;
        }
    }
}

// TEMP(transformers): raw backend without PS2X_GS_SHADOW -> frontend only scans for EE-visible registers.
static inline bool rawOnly(const std::unique_ptr<GSRasterBackend> &backend, bool rawTap)
{
    return rawTap && !backend->WantsDecodedRaster();
}

GS::GS()
    : m_backend(MakeGSBackend(CreateSelectedGSBackend())) // TEMP(transformers): PS2X_GS_BACKEND=CPU|PGS
{
    m_rawTap = m_backend->UsesRawGIF();
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    fenceVu1();
    if(m_raw.Active()) m_raw.Fail("VRAM reinitialization during capture"); // TEMP(transformers)
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if(m_raw.Enabled()) gsraw::privHook=[this](const void*owner,uint32_t a,uint64_t v,uint32_t w){if(owner==m_privRegs)rawPrivWrite(a,v,w);}; // TEMP(transformers)
    if (!m_backend)
        m_backend = MakeGSBackend(CreateSelectedGSBackend());
    m_rawTap = m_backend->UsesRawGIF();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    glyphState(this) = ps2_glyphs::UploadState{};
    if(m_raw.Enabled()) rawEvent(gsraw::Reset,nullptr,0); // TEMP(transformers)
    m_rawShadow.fill(0);m_rawShadow[GS_REG_PRMODECONT]=1;m_rawShadow[GS_REG_TRXDIR]=3;
    for(int i=0;i<2;i++){m_rawShadow[GS_REG_FRAME_1+i]=10ull<<16;m_rawShadow[GS_REG_SCISSOR_1+i]=(639ull<<16)|(447ull<<48);}
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    observeVu1();
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    observeVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? gsTimeline::tick(m_privRegs->vsyncTick.load(std::memory_order_acquire)) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    observeVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    observeVu1();
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = gsTimeline::tick(m_privRegs->vsyncTick.load(std::memory_order_acquire));
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

// ---------------------------------------------------------------------------------------------------------------------
// TEMP(transformers): PORT_LIST rank 3 - latch the presentation frame on the EE executor at guest VBlankStart.
//
// Legacy (PS2X_PRESENT_LATCH=0): UploadFrame (render thread) calls latchHostPresentationFrame() whenever the guest vsync
// tick changed and samples DISPFB + VRAM live, i.e. possibly while the EE thread is hundreds of ms inside a synchronous
// VIF1/D1 kick. Transformers aliases its Z buffer with the display buffers, so such a sample shows Z32 as colour
// (striped globe / Mini-Con / warp-gate frames; private qualification evidence F1, F2, F2b).
//
// Default: EeScheduler::processEvent(VBlankStart) calls ps2PresentLatchOnVBlank() on the EE executor, between guest
// slices (never inside a kick). It builds the GSPresentationRequest from the priv regs of that guest instant and
// snapshots GS local memory at the same position of the GS stream:
//   - GS worker active (PS2X_GS_THREAD=1): the snapshot is an ordered worker command (GSThreadedBackend::EnqueueOrdered),
//     so it sees exactly the draws enqueued before the vsync and the EE does not wait for the worker;
//   - no worker, or PS2X_PRESENT_LATCH=2: Flush + Sync(Presentation) + SnapshotVram inline (one 4 MiB memcpy).
// The render thread (UploadFrame -> ps2PresentLatchConsume) converts that snapshot to RGBA with a private GSCpuBackend
// (the same Present code as before, only later and on that thread) and stores it as the latched host frame.
// One snapshot is outstanding at a time (mailbox): the EE skips a VBlankStart while the host has not taken the previous
// snapshot yet. Direct callers outside these two scopes (unit tests) keep the old synchronous live latch.
//
// Hook point ported from Scotho/socom-unzipped third_party/ps2recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp
// 2201-2243 (GuestFrameBoundary() on the EE executor at VBlankStart) (GPL-3.0); ordered snapshot ported from
// ran-j/PS2Recomp PR #254 (Sinan-Karakaya/PS2Recomp@5d5dfe3) gs_threaded_backend.cpp Op::Prepare (GPL-3.0).
// The state is file-scope instead of GS members because gs_frontend.h is in the runner PCH (keeps this a fast build);
// move it into GS on the next full rebuild.
namespace
{
    enum class PresentLatchScope : uint8_t
    {
        None = 0,    // direct callers (unit tests, legacy UploadFrame): synchronous live latch
        Capture = 1, // EE executor at guest VBlankStart: request + VRAM snapshot, no conversion
        Consume = 2, // render thread: convert the snapshot captured by the EE
    };
    thread_local PresentLatchScope t_presentLatchScope = PresentLatchScope::None;
    thread_local bool t_rawTookFrame = false; // TEMP(transformers) qualification I1a: set when the raw-tap Present() returned a frame

    struct PresentLatchScopeGuard
    {
        explicit PresentLatchScopeGuard(PresentLatchScope scope) { t_presentLatchScope = scope; }
        ~PresentLatchScopeGuard() { t_presentLatchScope = PresentLatchScope::None; }
        PresentLatchScopeGuard(const PresentLatchScopeGuard &) = delete;
        PresentLatchScopeGuard &operator=(const PresentLatchScopeGuard &) = delete;
    };

    constexpr size_t kPresentLatchMinVramBytes = 4u * 1024u * 1024u; // GSCpuBackend::Initialize minimum

    struct EePresentLatchState
    {
        std::mutex mutex;                // guards request, vram, spare, ready
        GSPresentationRequest request{};
        std::vector<uint8_t> vram;       // GS local memory at the guest vsync (backend hand-off format)
        std::vector<uint8_t> spare;      // recycled buffer for the next snapshot
        bool ready = false;
        std::atomic<bool> busy{false};   // a snapshot is in flight or not yet taken by the host
        std::atomic<uint64_t> published{0};
        std::atomic<uint64_t> publishedTick{0};
    };

    EePresentLatchState &eePresentLatch()
    {
        // Leaked on purpose: a queued GS worker command may still publish while statics are torn down.
        static EePresentLatchState *s_state = new EePresentLatchState();
        return *s_state;
    }

    std::vector<uint8_t> takeEePresentLatchBuffer()
    {
        EePresentLatchState &latch = eePresentLatch();
        std::vector<uint8_t> buffer;
        std::lock_guard<std::mutex> lock(latch.mutex);
        buffer.swap(latch.spare);
        return buffer;
    }

    // EE executor (inline snapshot) or GS worker (queued snapshot).
    void publishEePresentLatch(const GSPresentationRequest &request, std::vector<uint8_t> &&vram)
    {
        EePresentLatchState &latch = eePresentLatch();
        {
            std::lock_guard<std::mutex> lock(latch.mutex);
            latch.request = request;
            latch.vram = std::move(vram);
            latch.ready = true;
        }
        latch.publishedTick.store(request.vsyncTick, std::memory_order_relaxed);
        latch.published.fetch_add(1u, std::memory_order_release);
    }
}

// PS2X_PRESENT_LATCH: unset/1 = EE latch at VBlankStart (snapshot queued on the GS worker when PS2X_GS_THREAD=1);
// 2 = EE latch with a blocking snapshot even with the worker (A/B); 0 = legacy render-thread live latch.
int ps2PresentLatchMode()
{
    static const int s_mode = []()
    {
        const char *value = std::getenv("PS2X_PRESENT_LATCH");
        int mode = 1;
        if (value && value[0] == '0')
            mode = 0;
        else if (value && value[0] == '2')
            mode = 2;
        std::cout << "[present:latch] mode=" << mode
                  << (mode == 0 ? " (legacy: render thread samples live DISPFB/VRAM)"
                                : (mode == 2 ? " (EE VBlankStart, blocking snapshot)" : " (EE VBlankStart)"))
                  << std::endl;
        return mode;
    }();
    return s_mode;
}

// EE executor, EeScheduler::processEvent(VBlankStart), after the vsync tick / CSR.FIELD update.
void ps2PresentLatchOnVBlank(GS &gs)
{
    gs.recordGuestVsync(); // TEMP(transformers): every vsync, even with latch disabled/busy.
    gs.rawGuestVsync(); // TEMP(transformers): ordered boundary, after FIELD update.
    if (ps2PresentLatchMode() == 0)
        return;
    EePresentLatchState &latch = eePresentLatch();
    if (latch.busy.exchange(true, std::memory_order_acq_rel))
        return; // previous snapshot still queued on the GS worker or not yet taken by the host
    PresentLatchScopeGuard scope(PresentLatchScope::Capture);
    gs.latchHostPresentationFrame();
}

// Render thread (UploadFrame). Returns true and converts the newest EE snapshot into the latched host frame when one
// was published since `ioSequence`; `outVsyncTick` = guest vsync tick of that snapshot.
bool ps2PresentLatchConsume(GS &gs, uint64_t &ioSequence, uint64_t &outVsyncTick)
{
    EePresentLatchState &latch = eePresentLatch();
    const uint64_t sequence = latch.published.load(std::memory_order_acquire);
    if (sequence == ioSequence)
        return false;
    ioSequence = sequence;
    outVsyncTick = latch.publishedTick.load(std::memory_order_relaxed);
    PresentLatchScopeGuard scope(PresentLatchScope::Consume);
    gs.latchHostPresentationFrame();
    return true;
}

// TEMP(transformers) qualification I1a: PS2X_FIELD60 (PGS raw tap). The render thread polls the backend's paced field
// queue every host frame (not only when a guest VBlankStart was published: the newer field is due half a tick after the
// older one, between guest vsync publications). True when a new frame was latched.
bool ps2PresentField60Poll(GS &gs)
{
    PresentLatchScopeGuard scope(PresentLatchScope::Consume);
    t_rawTookFrame = false;
    gs.latchHostPresentationFrame();
    return t_rawTookFrame;
}

void GS::latchHostPresentationFrame()
{
    observeVu1();
    PresentationFrame frame{};
    if (m_rawTap)
    {
        // TEMP(transformers): M1 parallel-gs. GS::rawGuestVsync (called first in ps2PresentLatchOnVBlank) already queued
        // the PCRTC scanout at the exact stream position; the backend publishes finished scanouts to a mailbox.
        // Capture (EE): only signal the render thread. Consume / legacy (render thread): take the newest scanout.
        if (t_presentLatchScope == PresentLatchScope::Capture)
        {
            EePresentLatchState &latch = eePresentLatch();
            latch.publishedTick.store(m_privRegs ? gsTimeline::tick(m_privRegs->vsyncTick.load()) : 0, std::memory_order_relaxed);
            latch.published.fetch_add(1, std::memory_order_release);
            latch.busy.store(false, std::memory_order_release);
            return;
        }
        // TEMP(transformers): keep descriptor and lazy backing atomic to native-copy callers.
        // Preserve the existing backend-lifetime -> presentation lock order used by setBackend().
        std::unique_lock<std::mutex> presentationLock(m_presentationMutex,std::defer_lock);
        {
            std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
            presentationLock.lock();
            if (m_backend)
                frame = m_backend->Present({}); // mailbox only, never blocks on the GS worker
        }
        if (!frame)
            return; // nothing new: keep the frame converted last time
        t_rawTookFrame = true; // TEMP(transformers) lane interp I1a
        m_hostPresentationWidth = frame.width;
        m_hostPresentationHeight = frame.height;
        m_hostPresentationDisplayFbp = frame.displayFbp;
        m_hostPresentationSourceFbp = frame.sourceFbp;
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = true;
        return;
    }
    const PresentLatchScope latchScope = t_presentLatchScope; // TEMP(transformers): PORT_LIST rank 3, see above
    if (latchScope == PresentLatchScope::Capture)
    {
        EePresentLatchState &latch = eePresentLatch();
        GSPresentationRequest request{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
            if (!m_backend || !m_privRegs)
            {
                latch.busy.store(false, std::memory_order_release);
                return;
            }
            request = buildPresentationRequestUnlocked();
        }
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (!m_backend)
        {
            latch.busy.store(false, std::memory_order_release);
            return;
        }
        if (ps2PresentLatchMode() == 1)
        {
            if (auto *threaded = dynamic_cast<GSThreadedBackend *>(m_backend.get()))
            {
                // Must not touch GS members: runs on the worker, possibly while GS swaps/destroys the backend.
                threaded->EnqueueOrdered([request](GSRasterBackend &backend)
                                         {
                    backend.Flush();
                    backend.Sync(GSSyncReason::Presentation);
                    std::vector<uint8_t> vram = takeEePresentLatchBuffer();
                    backend.SnapshotVram(vram);
                    publishEePresentLatch(request, std::move(vram)); });
                return;
            }
        }
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Presentation);
        std::vector<uint8_t> vram = takeEePresentLatchBuffer();
        m_backend->SnapshotVram(vram);
        publishEePresentLatch(request, std::move(vram));
        return;
    }

    if (latchScope == PresentLatchScope::Consume)
    {
        EePresentLatchState &latch = eePresentLatch();
        GSPresentationRequest request{};
        std::vector<uint8_t> vram;
        {
            std::lock_guard<std::mutex> lock(latch.mutex);
            if (!latch.ready)
                return; // nothing new: keep the frame converted last time
            request = latch.request;
            vram.swap(latch.vram);
            latch.ready = false;
        }
        latch.busy.store(false, std::memory_order_release); // the EE may capture its next VBlankStart meanwhile
        if (vram.size() >= kPresentLatchMinVramBytes)
        {
            thread_local GSCpuBackend s_converter;
            s_converter.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
            frame = s_converter.Present(request);
            s_converter.Initialize(nullptr, 0u);
        }
        // PS2X_VRAM_DUMP_VS (UploadFrame) dumps m_displaySnapshot: keep the VRAM image of this latched frame there,
        // and recycle the previous image as the next snapshot buffer.
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            m_displaySnapshot.swap(vram);
        }
        {
            std::lock_guard<std::mutex> lock(latch.mutex);
            if (latch.spare.empty())
                latch.spare.swap(vram);
        }
    }
    else
    {
        GSPresentationRequest request{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
            if (!m_backend || !m_privRegs)
            {
                std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
                m_hostPresentationFrame.clear();
                m_hasHostPresentationFrame = false;
                m_hostPresentationWidth = m_hostPresentationHeight = 0u;
                return;
            }
            request = buildPresentationRequestUnlocked();
        }

        {
            std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
            if (m_backend)
            {
                m_backend->Flush();
                m_backend->Sync(GSSyncReason::Presentation);
                frame = m_backend->Present(request);
            }
        }
    }

    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

// TEMP(transformers): lazy descriptor reader; all public copies preserve packed native RGBA bytes.
bool ps2PgsLazyNativeCopy(std::vector<uint8_t>&,uint32_t,uint32_t);
bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    if(m_hostPresentationFrame.size()==1u) // TEMP(transformers): lazy sharp native descriptor
        return ps2PgsLazyNativeCopy(outPixels,outWidth,outHeight);
    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    // GAME(transformers): nested glyph submissions inherit the outer ordered GS entry.
    if (glyphSubmissionOwner != this) fenceVu1();
    ps2_perfscope::Scope perf(ps2_perfscope::EeGif); // TEMP(transformers) qualification
    std::unique_lock<std::recursive_mutex> lock(m_stateMutex, std::try_to_lock);
    if (!lock.owns_lock()) // TEMP(transformers) qualification: contended -> time the wait for the GS state mutex
    {
        ps2_perfscope::Scope perfLock(ps2_perfscope::EeWaitGsLock);
        lock.lock();
    }
    if (!data || sizeBytes < 16 || !m_backend)
        return;

    auto &glyph = glyphState(this);
    if (!glyph.injecting && ps2_glyphs::UploadState::enabled())
    {
        GlyphGuard guard(glyph.injecting, this);
        auto emit = [&](const ps2_glyphs::Upload &u, const std::vector<uint8_t> &payload) {
            emitGlyphUpload(this, glyph, u, payload);
        };
        glyph.refresh(emit);
        if (glyph.packet(data, sizeBytes,
            [&](const uint8_t *p, uint32_t n) { processGIFPacket(p, n); }, emit)) return;
    }

    if (m_rawTap) m_backend->RawGIF(gsraw::path, data, sizeBytes); // TEMP(transformers): M1 raw tap, before any decode
    if (rawOnly(m_backend, m_rawTap))
    {
        scanRawGifForEeState(data, sizeBytes, [this](uint8_t reg, uint64_t value) { writeRegisterUnlocked(reg, value); });
        return;
    }
    if(m_raw.Enabled()) rawGif(data,sizeBytes,gsraw::path); // TEMP(transformers): includes native image fast path.

    if (tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg >= GIF_FMT_IMAGE) // TEMP(transformers): FLG=3 follows IMAGE (GoW 353ac83c)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    // GAME(transformers): nested glyph submissions inherit the outer ordered GS entry.
    if (glyphSubmissionOwner != this) fenceVu1();
    ps2_perfscope::Scope perf(ps2_perfscope::EeGif); // TEMP(transformers) qualification
    std::unique_lock<std::recursive_mutex> lock(m_stateMutex, std::try_to_lock);
    if (!lock.owns_lock()) // TEMP(transformers) qualification: contended -> time the wait for the GS state mutex
    {
        ps2_perfscope::Scope perfLock(ps2_perfscope::EeWaitGsLock);
        lock.lock();
    }
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    auto &glyph = glyphState(this);
    if (!glyph.injecting && ps2_glyphs::UploadState::enabled())
    {
        GlyphGuard guard(glyph.injecting, this);
        auto emit = [&](const ps2_glyphs::Upload &u, const std::vector<uint8_t> &payload) {
            emitGlyphUpload(this, glyph, u, payload);
        };
        glyph.refresh(emit);
        if (glyph.packet(data, sizeBytes,
            [&](const uint8_t *p, uint32_t n) { processNativePackedGIFPacket(p, n); }, emit)) return true;
    }

    if (m_rawTap) m_backend->RawGIF(3, data, sizeBytes); // TEMP(transformers): M1 raw tap (validated PATH3 packet)
    if (rawOnly(m_backend, m_rawTap))
    {
        scanRawGifForEeState(data, sizeBytes, [this](uint8_t reg, uint64_t value) { writeRegisterUnlocked(reg, value); });
        ++m_nativePackedGIFPacketCount;
        return true;
    }
    if(m_raw.Enabled()) rawGif(data,sizeBytes,3); // TEMP(transformers): accepted native PATH3 packet only.
    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    // GAME(transformers): nested glyph submissions inherit the outer ordered GS entry.
    if (glyphSubmissionOwner != this) fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    auto &glyph = glyphState(this);
    if (!glyph.injecting && data && sizeBytes && m_backend && ps2_glyphs::UploadState::enabled())
    {
        GlyphGuard guard(glyph.injecting, this);
        auto emit = [&](const ps2_glyphs::Upload &u, const std::vector<uint8_t> &payload) {
            emitGlyphUpload(this, glyph, u, payload);
        };
        glyph.refresh(emit);
        glyph.reg(0x50, bitbltbuf); glyph.reg(0x51, trxpos);
        glyph.reg(0x52, trxreg); glyph.reg(0x53, trxdir);
        uploadImageNative(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
        glyph.image(data, sizeBytes, emit);
        return;
    }
    if (m_rawTap && data && sizeBytes && m_backend)
    {
        // TEMP(transformers): M1 raw tap. Direct uploads carry no GIF tags: rebuild the PATH3 packet (A+D setup + IMAGE).
        const uint32_t qwords = (sizeBytes + 15u) / 16u;
        std::vector<uint8_t> packet(16u + 4u * 16u + 16u + size_t(qwords) * 16u, 0u);
        auto put64 = [&](size_t off, uint64_t v) { std::memcpy(packet.data() + off, &v, 8); };
        put64(0, 4ull | (1ull << 15) | (0ull << 58) | (1ull << 60)); // NLOOP=4 EOP=1 PACKED NREG=1
        put64(8, 0xEull);                                            // A+D
        const uint64_t setup[4][2] = {{bitbltbuf, GS_REG_BITBLTBUF}, {trxpos, GS_REG_TRXPOS}, {trxreg, GS_REG_TRXREG}, {trxdir, GS_REG_TRXDIR}};
        for (int i = 0; i < 4; ++i) { put64(16u + i * 16u, setup[i][0]); put64(24u + i * 16u, setup[i][1]); }
        put64(80, uint64_t(qwords & 0x7FFFu) | (1ull << 15) | (2ull << 58)); // IMAGE, EOP
        std::memcpy(packet.data() + 96u, data, sizeBytes);
        m_backend->RawGIF(3, packet.data(), static_cast<uint32_t>(packet.size()));
        if (rawOnly(m_backend, m_rawTap))
        {
            ++m_nativeImageUploadCount;
            return;
        }
    }
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-gs-packed-depth.patch (GPL-3.0): XYZ2 carries Z32;
        // a float loses the low bits above 2^24 and rounds 0xFFFFFFFF up to 2^32.
        vtx.z = static_cast<double>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        // Ported from god-of-war-recomp ps2recomp-gs-packed-depth.patch (GPL-3.0): XYZ3 keeps the same Z32 depth.
        vtx.z = static_cast<double>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    // GAME(transformers): nested glyph submissions inherit the outer ordered GS entry.
    if (glyphSubmissionOwner != this) fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if(m_raw.Enabled()) { gsraw::AD ad{regAddr,value};rawEvent(gsraw::DirectAD,&ad,sizeof(ad)); } // TEMP(transformers)
    if (m_rawTap && regAddr < 128) m_backend->RawAD(regAddr, value); // TEMP(transformers): M1 raw tap (HLE A+D writes)
    writeRegisterUnlocked(regAddr, value);
    auto &glyph = glyphState(this);
    if (!glyph.injecting && ps2_glyphs::UploadState::enabled())
    {
        GlyphGuard guard(glyph.injecting, this);
        if (regAddr == GS_REG_HWREG) {
            uint8_t bytes[8]; std::memcpy(bytes, &value, 8);
            glyph.image(bytes, 8, [&](const ps2_glyphs::Upload &u, const std::vector<uint8_t> &payload) {
                emitGlyphUpload(this, glyph, u, payload);
            });
        } else glyph.reg(regAddr, value);
    }
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    if(m_raw.Enabled() && regAddr<128) m_rawShadow[regAddr]=value; // TEMP(transformers): includes TEX2/MIPTBP even when unused by CPU.
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_backend) m_backend->Sync(GSSyncReason::Finish);
        ps2dbg::gsSignalWrites.fetch_add(1, std::memory_order_relaxed);
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        ps2dbg::gsFinishWrites.fetch_add(1, std::memory_order_relaxed);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_backend) m_backend->Sync(GSSyncReason::Finish);
        ps2dbg::gsLabelWrites.fetch_add(1, std::memory_order_relaxed);
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if(m_raw.Enabled()) rawPrivWrite(0x12000000u+0x70u,value); // TEMP(transformers)
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if(m_raw.Enabled()) rawPrivWrite(0x12000000u+0x80u,value); // TEMP(transformers)
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if(m_raw.Enabled()) rawPrivWrite(0x12000000u+0x90u,value); // TEMP(transformers)
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if(m_raw.Enabled()) rawPrivWrite(0x12000000u+0xa0u,value); // TEMP(transformers)
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if(m_raw.Enabled()) rawPrivWrite(0x12000000u+0xe0u,value); // TEMP(transformers)
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend)
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        updatePreferredDisplaySourceForDraw(batch);
        m_backend->Submit(batch);
        recordDrawDebugEventUnlocked(needed);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_backend)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if(m_raw.Enabled()) { uint32_t p[2]={contextIndex!=0,rgba};rawEvent(gsraw::Clear,p,sizeof(p)); } // TEMP(transformers)
    return m_backend && m_backend->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if(m_raw.Enabled()) { uint32_t p[2]={m_prim.ctxt?1u:0u,rgba};rawEvent(gsraw::Clear,p,sizeof(p)); } // TEMP(transformers)
    return m_backend && m_backend->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    const uint32_t n=m_backend ? m_backend->ConsumeLocalToHostBytes(dst,maxBytes):0u;
    if(m_raw.Enabled()) { std::vector<uint8_t> p(sizeof(gsraw::ReadbackPayload)+n);gsraw::ReadbackPayload h{maxBytes,n};std::memcpy(p.data(),&h,sizeof(h));if(n)std::memcpy(p.data()+sizeof(h),dst,n);rawEvent(gsraw::Readback,p.data(),uint32_t(p.size())); } // TEMP(transformers)
    return n;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if(m_raw.Active()) m_raw.Fail("backend replacement during capture"); // TEMP(transformers)
    if (!backend)
        backend = std::make_unique<GSCpuBackend>();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = MakeGSBackend(std::move(backend));
    m_rawTap = m_backend->UsesRawGIF();
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    fenceVu1();
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if(m_raw.Enabled()) { GSRecPixel p{psm,base,bw,x,y,value};rawEvent(gsraw::Pixel,&p,sizeof(p)); } // TEMP(transformers)
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}

// TEMP(transformers): no private/frontend register serialization is necessary at this
// decoded backend boundary: Submit and Present contain complete effective state.
void GS::recordGuestVsync()
{
    static const bool enabled = std::getenv("PS2X_GS_RECORD") != nullptr;
    if (!enabled) return;
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    std::lock_guard<std::mutex> lifetimeLock(m_backendLifetimeMutex);
    if (auto *backend = dynamic_cast<GSThreadedBackend *>(m_backend.get()))
        backend->RecordVsync(buildPresentationRequestUnlocked());
}

// TEMP(transformers): raw-stream recorder and private CPU resume.
GS::~GS() { if(m_raw.Enabled() || m_raw.Done()) gsraw::privHook={}; }
uint64_t GS::rawDecodedCount() const { if(auto*b=dynamic_cast<GSThreadedBackend*>(m_backend.get()))return b->DecodedRecordCount();return 0; }
void GS::rawEvent(uint32_t op,const void*p,uint32_t n) { m_raw.EventRecord(op,p,n,m_privRegs?gsTimeline::tick(m_privRegs->vsyncTick.load()):0,rawDecodedCount()); }
void GS::rawGif(const uint8_t*data,uint32_t n,uint32_t path) {
 if(!m_raw.Active())return;
 std::vector<uint8_t> bytes(8ull+n);std::memcpy(bytes.data(),&path,4);std::memcpy(bytes.data()+4,&n,4);std::memcpy(bytes.data()+8,data,n);rawEvent(gsraw::Gif,bytes.data(),uint32_t(bytes.size()));
}
void GS::rawPrivWrite(uint32_t address,uint64_t value,uint32_t width) {
    fenceVu1();
 if(!m_raw.Enabled())return;std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
 gsraw::PrivPayload p{address,width,value};rawEvent(gsraw::Priv,&p,sizeof(p));
}
std::array<uint8_t,8192> GS::rawPrivBlock() const {
 std::array<uint8_t,8192> out{};if(!m_privRegs)return out;
 auto put=[&](size_t o,uint64_t v){std::memcpy(out.data()+o,&v,8);};
 put(0x0000,m_privRegs->pmode);put(0x0010,m_privRegs->smode1);put(0x0020,m_privRegs->smode2);
 put(0x0030,m_privRegs->srfsh);put(0x0040,m_privRegs->synch1);put(0x0050,m_privRegs->synch2);put(0x0060,m_privRegs->syncv);
 put(0x0070,m_privRegs->dispfb1);put(0x0080,m_privRegs->display1);put(0x0090,m_privRegs->dispfb2);put(0x00a0,m_privRegs->display2);
 put(0x00b0,m_privRegs->extbuf);put(0x00c0,m_privRegs->extdata);put(0x00d0,m_privRegs->extwrite);put(0x00e0,m_privRegs->bgcolor);
 put(0x1000,gsTimeline::csr(m_privRegs->csr.load()));put(0x1010,m_privRegs->imr);put(0x1040,m_privRegs->busdir);put(0x1080,m_privRegs->siglblid);return out;
}
void GS::rawPrivRestore(const std::array<uint8_t,8192>& in) {
    fenceVu1();
 if(!m_privRegs)throw std::runtime_error("GSRAW privileged owner missing");
 auto get=[&](size_t o){uint64_t v;std::memcpy(&v,in.data()+o,8);return v;};
 m_privRegs->pmode=get(0);m_privRegs->smode1=get(0x10);m_privRegs->smode2=get(0x20);m_privRegs->srfsh=get(0x30);
 m_privRegs->synch1=get(0x40);m_privRegs->synch2=get(0x50);m_privRegs->syncv=get(0x60);
 m_privRegs->dispfb1=get(0x70);m_privRegs->display1=get(0x80);m_privRegs->dispfb2=get(0x90);m_privRegs->display2=get(0xa0);
 m_privRegs->extbuf=get(0xb0);m_privRegs->extdata=get(0xc0);m_privRegs->extwrite=get(0xd0);m_privRegs->bgcolor=get(0xe0);
 m_privRegs->csr.store(get(0x1000));m_privRegs->imr=get(0x1010);m_privRegs->busdir=get(0x1040);m_privRegs->siglblid=get(0x1080);
}
std::vector<gsraw::AD> GS::rawADState() const {
 auto regs=m_rawShadow;auto bits=[](float f){uint32_t v;std::memcpy(&v,&f,4);return v;};
 regs[GS_REG_RGBAQ]=uint64_t(m_curR)|(uint64_t(m_curG)<<8)|(uint64_t(m_curB)<<16)|(uint64_t(m_curA)<<24)|(uint64_t(bits(m_curQ))<<32);
 regs[GS_REG_ST]=uint64_t(bits(m_curS))|(uint64_t(bits(m_curT))<<32);regs[GS_REG_UV]=uint64_t(m_curU)|(uint64_t(m_curV)<<16);regs[GS_REG_FOG]=uint64_t(m_curFog)<<56;
 for(int c=0;c<2;c++){auto&t=m_ctx[c].tex0;regs[GS_REG_TEX0_1+c]=uint64_t(t.tbp0)|(uint64_t(t.tbw)<<14)|(uint64_t(t.psm)<<20)|(uint64_t(t.tw)<<26)|(uint64_t(t.th)<<30)|(uint64_t(t.tcc)<<34)|(uint64_t(t.tfx)<<35)|(uint64_t(t.cbp)<<37)|(uint64_t(t.cpsm)<<51)|(uint64_t(t.csm)<<55)|(uint64_t(t.csa)<<56)|(uint64_t(t.cld)<<61);}
 const uint8_t addresses[]={0x00,0x1a,0x1b,0x1c,0x22,0x3b,0x3d,0x3f,0x44,0x45,0x46,0x49,0x06,0x07,0x08,0x09,0x14,0x15,0x16,0x17,0x18,0x19,0x34,0x35,0x36,0x37,0x40,0x41,0x42,0x43,0x47,0x48,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,0x50,0x51,0x52,0x53,0x01,0x02,0x03,0x0a};
 std::vector<gsraw::AD> out;for(auto a:addresses)out.push_back({a,regs[a]});return out;
}
std::vector<uint8_t> GS::rawFrontendState() const {
 std::vector<uint8_t> out;auto add=[&](const auto&v){const auto*p=reinterpret_cast<const uint8_t*>(&v);out.insert(out.end(),p,p+sizeof(v));};
 add(m_ctx);add(m_prim);add(m_primRegister);add(m_prmodeRegister);
 add(m_curR);add(m_curG);add(m_curB);add(m_curA);add(m_curQ);add(m_curS);add(m_curT);add(m_curU);add(m_curV);add(m_curFog);
 add(m_fogR);add(m_fogG);add(m_fogB);add(m_prmodecont);add(m_pabe);add(m_scanmsk);add(m_dimx);add(m_dthe);add(m_colclamp);add(m_texa);add(m_texclut);
 add(m_bitbltbuf);add(m_trxpos);add(m_trxreg);add(m_trxdir);add(m_vtxQueue);add(m_vtxCount);add(m_vtxIndex);
 add(m_preferredDisplaySourceFrame);add(m_preferredDisplayDestFbp);add(m_hasPreferredDisplaySource);return out;
}
void GS::rawRestore(const std::vector<gsraw::AD>&ad,const std::vector<uint8_t>&in,const std::vector<uint8_t>&cpu) {
    fenceVu1();
 std::lock_guard<std::recursive_mutex> lock(m_stateMutex);size_t pos=0;
 auto get=[&](auto&v){if(pos>in.size()||sizeof(v)>in.size()-pos)throw std::runtime_error("short frontend state");std::memcpy(&v,in.data()+pos,sizeof(v));pos+=sizeof(v);};
 get(m_ctx);get(m_prim);get(m_primRegister);get(m_prmodeRegister);
 get(m_curR);get(m_curG);get(m_curB);get(m_curA);get(m_curQ);get(m_curS);get(m_curT);get(m_curU);get(m_curV);get(m_curFog);
 get(m_fogR);get(m_fogG);get(m_fogB);get(m_prmodecont);get(m_pabe);get(m_scanmsk);get(m_dimx);get(m_dthe);get(m_colclamp);get(m_texa);get(m_texclut);
 get(m_bitbltbuf);get(m_trxpos);get(m_trxreg);get(m_trxdir);get(m_vtxQueue);get(m_vtxCount);get(m_vtxIndex);
 get(m_preferredDisplaySourceFrame);get(m_preferredDisplayDestFbp);get(m_hasPreferredDisplaySource);
 if(pos!=in.size()||m_vtxCount<0||m_vtxCount>6||m_vtxIndex<0)throw std::runtime_error("invalid frontend state/layout");
 for(auto&p:ad){if(p.reg>=128)throw std::runtime_error("invalid AD address");m_rawShadow[p.reg]=p.value;}
 auto*b=dynamic_cast<GSCpuBackend*>(m_backend.get());if(!b)throw std::runtime_error("replay must use serial CPU backend");b->ReplayState(cpu);
}
// TEMP(transformers) qualification: set (EE thread) by the PS2X_LOAD_SKIP loading-screen skip in ps2StartVu1Cmsar1 for every
// vsync whose next PCRTC scanout would not show a complete vortex frame (vortex skipped -> the game's double-buffer swap
// cleared the field, or only the first field of a pair drawn). That vsync still runs normally on parallel-gs (so its
// field history stays right) but is tagged in an unused privileged-block word (offset 0x0FF8) and the
// backend does not copy/publish its scanout, so the window keeps the last complete vortex frame. Presentation only.
std::atomic<bool> g_ps2LoadSkipHoldScanout{false};

void GS::rawGuestVsync(uint64_t capturedTick,uint64_t capturedField) {
    // TEMP(transformers) warpflick1: carry the EE's hold decision with this ordered boundary.
    // Bit 63 is private to this call; strip it before constructing any GS register/record.
    // Consuming the global flag on the worker lets a later EE field suppress an earlier
    // scanout and leaves the actual skipped/half-drawn field eligible for presentation.
    constexpr uint64_t holdBit = 1ull << 63;
    if(capturedTick==UINT64_MAX) {
        capturedTick=m_privRegs?gsTimeline::tick(m_privRegs->vsyncTick.load()):0;
        capturedField=m_privRegs?gsTimeline::csr(m_privRegs->csr.load())&(1ull<<13):0;
        if (g_ps2LoadSkipHoldScanout.exchange(false, std::memory_order_relaxed))
            capturedField |= holdBit;
        if(m_vu1Ordered && m_vu1Ordered([this,capturedTick,capturedField]{rawGuestVsync(capturedTick,capturedField);}))return;
    }
    const bool holdScanout = (capturedField & holdBit) != 0;
    capturedField &= ~holdBit;
    auto privBlock=[&] {
        auto out=rawPrivBlock();uint64_t csr=0;std::memcpy(&csr,out.data()+0x1000,8);
        csr=(csr&~(1ull<<13))|capturedField;std::memcpy(out.data()+0x1000,&csr,8);return out;
    };
    observeVu1();
 if (m_rawTap) { // M1 parallel-gs: ordered vsync (scanout) with this guest instant's privileged block, after FIELD update
  std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
  std::array<uint8_t, 8192> priv = privBlock();
  // TEMP(transformers) qualification I1a: guest vsync tick for the backend (reserved offset 0x0F00, cleared there).
  // v5 merge (release fix x release fix): the tick captured at VBlankStart (== vsyncTick in mode 0; the admission tick in mode 3).
  const uint64_t vtick = capturedTick;
  std::memcpy(priv.data() + 0x0F00, &vtick, sizeof(vtick));
  if (holdScanout) // TEMP(transformers) warpflick1: decision captured at admission, not worker execution
  {
   const uint64_t tag = 0x504C4F4144484C44ull; // "DLHDAOLP" - GSPgsBackend::RawVsync checks and clears it
   std::memcpy(priv.data() + 0x0FF8, &tag, sizeof(tag)); // upper half of the unused privileged slot 0x12000FF0
  }
  m_backend->RawVsync(priv);
 }
 if(!m_raw.Enabled())return;std::lock_guard<std::recursive_mutex> lock(m_stateMutex);std::lock_guard<std::mutex> lifetimeLock(m_backendLifetimeMutex);
 const uint64_t tick=capturedTick;
 auto snapshot=[&](std::vector<uint8_t>&cpu,std::vector<uint8_t>&portable,std::vector<uint8_t>&vram){
  if(auto*b=dynamic_cast<GSThreadedBackend*>(m_backend.get()))b->RawSnapshot(cpu,portable,vram);
  else if(auto*b=dynamic_cast<GSCpuBackend*>(m_backend.get())){cpu=b->RecordState();portable=b->RawPortableState();b->SnapshotVram(vram);}
  else throw std::runtime_error("GSRAW CPU backend required");
  auto add=[&](const auto&v){auto*p=reinterpret_cast<const uint8_t*>(&v);portable.insert(portable.end(),p,p+sizeof(v));};
  add(uint32_t(m_vtxCount));add(uint32_t(m_vtxIndex));
  for(const auto&v:m_vtxQueue){std::array<uint8_t,40> b{};auto put=[&](size_t o,const auto&n){std::memcpy(b.data()+o,&n,sizeof(n));};
   put(0,v.x);put(4,v.y);put(8,v.z);put(16,v.r);put(17,v.g);put(18,v.b);put(19,v.a);put(20,v.q);put(24,v.s);put(28,v.t);put(32,v.u);put(34,v.v);put(36,v.fog);
   portable.insert(portable.end(),b.begin(),b.end());}

 };
 if(!m_raw.Active() && tick>=m_raw.Start() && !m_raw.Done()){
  std::vector<uint8_t> cpu,portable,vram;snapshot(cpu,portable,vram);gsraw::Header h;h.startTick=tick;h.decodedStart=rawDecodedCount();GSRecHeader identity;std::memcpy(h.sourceId,identity.sourceId,64);
  m_raw.Begin(h,rawADState(),privBlock(),portable,rawFrontendState(),cpu,vram);return;
 }
 if(m_raw.Active()){
  gsraw::VsyncPayload v{((m_privRegs?gsTimeline::csr(m_privRegs->csr.load()):0)&~(1ull<<13))|capturedField};m_raw.EventRecord(gsraw::Vsync,&v,sizeof(v),tick,rawDecodedCount());
  if(m_raw.Boundary()){std::vector<uint8_t>cpu,portable,vram;snapshot(cpu,portable,vram);m_raw.Finish(vram,tick,rawDecodedCount());}
 }
}
