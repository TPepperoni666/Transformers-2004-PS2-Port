#include "ps2_vu1new.h" // TEMP(transformers)
// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "debug_counters.h" // TEMP(transformers)
#include "ps2_intc_pending.h"
#include "ps2_perfscope.h" // TEMP(transformers): qualification
extern bool g_path3ReleaseOnePacket; // ps2_memory.cpp (GENERIC, SSX-0042 port)
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm> // release fix
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono> // TEMP(transformers): vif1fast1 profiler wall-time calibration
#include <sstream> // TEMP(transformers): emit a profiler snapshot in one write

// release fix (qualification): hardware UNPACK V2/V3 lanes.
// The VIF writes all four lanes of every UNPACK destination quadword; it never keeps the old VU memory value of a lane
// the format carries no data for. Reference = PCSX2: pcsx2/Vif_Unpack.cpp (UNPACK_V2 writes v0 v1 v0 v1, "The PS2
// console actually writes v1v0v1v0 for all V2 unpacks"; V3 uses UNPACK_V4, so w = the next source element, "confirmed
// real hardware behavior") and its default recompiled path pcsx2/x86/Vif_UnpackSSE.cpp (xUPK_V2_* / xUPK_V3_*, which
// additionally force w = 0 in some source-alignment cases, "zero last word - tested on ps2"):
//   V2-32/16/8  x=v0 y=v1 z=v0 w=v1        recompiler: V2-32 w=0 when the data starts on an odd word
//   V3-32/16/8  x=v0 y=v1 z=v2 w=next      recompiler: w=0 per recW0() below (depends on the quadword phase)
// The value is a normal data qualification: MASK selects data/ROW/COL/protect, MODE 1/2 add ROW (2 updates ROW).
// PS2X_VIF_UNPACK_HW=1 (default) PCSX2 recompiler rules, =2 PCSX2 C-interpreter rules (w never forced to 0),
// =0 old behaviour (missing lanes keep the old VU memory value, plus ROW in offset/difference mode).
namespace ps2x_vifunpack
{
    int g_modeOverride = -1; // unit tests only

    void setHwModeForTest(int mode)
    {
        g_modeOverride = mode;
    }

    int hwMode()
    {
        if (g_modeOverride >= 0)
            return g_modeOverride;
        static const int s_mode = []()
        {
            const char *e = std::getenv("PS2X_VIF_UNPACK_HW");
            if (!e || !*e)
                return 1;
            const int v = std::atoi(e);
            return (v < 0 || v > 2) ? 1 : v;
        }();
        return s_mode;
    }

    // CHCR.TTE chains: offsets of the 8 tag bytes inside the concatenated buffer (registered by ps2_memory.cpp).
    std::mutex s_tteMutex;
    std::unordered_map<const uint8_t *, std::vector<uint32_t>> s_tte;
    thread_local std::vector<uint32_t> t_curTte;

    void registerChainTte(const uint8_t *key, std::vector<uint32_t> &&offsets)
    {
        std::lock_guard<std::mutex> lock(s_tteMutex);
        if (s_tte.size() > 4096u) // chains dropped without being processed must not pile up
            s_tte.clear();
        s_tte[key] = std::move(offsets);
    }

    void beginChain(const uint8_t *key)
    {
        t_curTte.clear();
        std::lock_guard<std::mutex> lock(s_tteMutex);
        auto it = s_tte.find(key);
        if (it != s_tte.end())
        {
            t_curTte.swap(it->second);
            s_tte.erase(it);
        }
    }

    void endChain()
    {
        t_curTte.clear();
    }

    // TEMP(transformers) qualification: PS2X_VU1_THREAD=3 copies the chain buffers into one job and parses them twice (EE
    // pre-scan + worker), so the offsets travel with the job instead of living in the walking thread's t_curTte.
    std::vector<uint32_t> takeChainTte(const uint8_t *key)
    {
        std::vector<uint32_t> out;
        std::lock_guard<std::mutex> lock(s_tteMutex);
        auto it = s_tte.find(key);
        if (it != s_tte.end())
        {
            out.swap(it->second);
            s_tte.erase(it);
        }
        return out;
    }

    std::vector<uint32_t> currentChainTte()
    {
        return t_curTte;
    }

    void setChainTte(const std::vector<uint32_t> &offsets)
    {
        t_curTte = offsets;
    }

    // Word (0..3) of the hardware quadword that buffer byte offset `pos` belongs to. A TTE tag's 8 bytes are the upper
    // half of the tag quadword and its payload starts on a quadword, so every byte of tag + payload has phase (pos-s+8).
    uint32_t hwWordPhase(uint32_t pos)
    {
        if (!t_curTte.empty())
        {
            auto it = std::upper_bound(t_curTte.begin(), t_curTte.end(), pos);
            if (it != t_curTte.begin())
                return ((pos - *(it - 1) + 8u) >> 2) & 3u;
        }
        return (pos >> 2) & 3u;
    }

    // PCSX2 x86/Vif_UnpackSSE.cpp + Vif_Dynarec.cpp: does the recompiled unpack force w = 0 for this vector?
    // k = index of the source vector inside the UNPACK (data cycles only), o = word phase of the UNPACK data start.
    // start_aligned (Vif_Unpack.cpp vifUnpackSetup) = o, or 4 when o == 0; the block key keeps only its low bit for
    // every format except V3-16 (dVifUnpack: key1 &= 0xFFFF01FF unless upkType == 9).
    bool recW0(uint32_t vn, uint32_t vl, uint32_t k, uint32_t o)
    {
        const uint32_t startAligned = (o == 0u) ? 4u : o;
        if (vn == 1u) // xUPK_V2_32: if (IsAligned) zero w; V2-16/V2-8 never
            return vl == 0u && (startAligned & 1u) != 0u;
        if (vn != 2u)
            return false;
        if (vl == 0u) // xUPK_V3_32: UnpkLoopIteration (post-incremented, & 1) != IsAligned
            return (k & 1u) != (startAligned & 1u);
        if (vl == 2u) // xUPK_V3_8: UnpkLoopIteration (pre-incremented, never wrapped) != IsAligned
            return (k + 1u) != (startAligned & 1u);
        if (vl == 1u) // xUPK_V3_16: it = pre-incremented count; zero when it is even and the phase formula hits 0
        {
            const uint32_t it = k + 1u;
            return (it & 1u) == 0u && (((it / 4u) + 1u + (4u - startAligned)) & 3u) == 0u;
        }
        return false;
    }

    // Fills the lanes a V2/V3 format has no data for (decompressed[0..vn] already decoded from srcVec).
    void fillMissingLanes(uint32_t *decompressed, uint32_t vn, uint32_t vl, bool zeroExtend, const uint8_t *srcVec,
                          const uint8_t *bufEnd, uint32_t k, uint32_t o, int mode)
    {
        if (vn == 1u)
        {
            decompressed[2] = decompressed[0];
            decompressed[3] = decompressed[1];
        }
        else
        {
            const uint32_t elemBytes = 4u >> vl; // 4 / 2 / 1
            const uint8_t *next = srcVec + 3u * elemBytes;
            uint32_t w = 0u;
            if (next + elemBytes <= bufEnd) // past the buffer: nothing to read, use 0
            {
                if (vl == 0u)
                    std::memcpy(&w, next, 4u);
                else if (vl == 1u)
                {
                    uint16_t raw = 0u;
                    std::memcpy(&raw, next, 2u);
                    w = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                }
                else
                    w = zeroExtend ? next[0] : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(next[0])));
            }
            decompressed[3] = w;
        }
        if (mode == 1 && recW0(vn, vl, k, o))
            decompressed[3] = 0u;
    }


    // TEMP(transformers): existing diagnostic parsed once; fast kernels must not bypass it.
    struct ColorWatch { bool on=false;uint32_t w[4]{}; };
    __forceinline const ColorWatch &colorWatch()
    {
    static const ColorWatch value = []() -> ColorWatch
    {
        ColorWatch c{};
        const char *e = std::getenv("PS2X_VIF_COLORWATCH");
        if (!e)
            return c;
        char *n = nullptr;
        for (int i = 0; i < 4; ++i)
        {
            c.w[i] = static_cast<uint32_t>(std::strtoul(i == 0 ? e : n + 1, &n, 16));
            if (i < 3 && (!n || *n != ','))
                return c;
        }
        c.on = true;
        return c;
    }();
        return value;
    }
    bool censusOn();

// Adapted from KIexster/god-of-war-recomp (Romeo), GPL-3.0: f2f45251
// ps2recomp-vif1-unpack-formats.patch, 50b57551 ps2recomp-vif1-ee-timers-fast.patch
// (VIF hunk), a279888d ps2recomp-vif1-unpack-mask-sse.patch;
// V2-32/V3 lanes changed to release fix rules. No God of War W-preserve behavior.
int g_fastOverride = -1;
thread_local uint32_t g_parsePosTest=0; // TEMP(transformers): only recorded while the test override is active
uint32_t parsePosForTest() { return g_parsePosTest; }
void setFastModeForTest(int mode) { g_fastOverride=mode; }
bool fastOn()
{
    if (g_fastOverride>=0) return g_fastOverride!=0;
    static const bool on=[]() { const char *e=std::getenv("PS2X_VIF_UNPACK_FAST"); return !e || !*e || *e!='0'; }();
    return on;
}
struct FastUnpackState
{
    __m128i specs[4][4], cols[4], row;
    bool protect[4]{}, zeroW[16]{}, mask, add, difference, special8;
};
template<unsigned VN,unsigned VL,bool USN>
__forceinline __m128i fastDecode(const uint8_t *src,const uint8_t *end)
{
    if constexpr (VL==3)
    {
        uint16_t p; std::memcpy(&p,src,2);
        return _mm_setr_epi32((p&31u)<<3,(p&0x3e0u)>>2,(p&0x7c00u)>>7,(p&0x8000u)>>8);
    }
    else
    {
        constexpr unsigned elem=4u>>VL, bytes=(VN+1u)*elem;
        __m128i raw;
        if (size_t(end-src)>=16u) raw=_mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
        else
        {
            alignas(16) uint8_t tail[16]{};
            std::memcpy(tail,src,bytes);
            if constexpr (VN==2)
                if (size_t(end-src)>=bytes+elem) std::memcpy(tail+bytes,src+bytes,elem);
            raw=_mm_load_si128(reinterpret_cast<const __m128i *>(tail));
        }
        __m128i v;
        if constexpr (VL==0) v=raw;
        else if constexpr (VL==1) v=USN ? _mm_cvtepu16_epi32(raw) : _mm_cvtepi16_epi32(raw);
        else v=USN ? _mm_cvtepu8_epi32(raw) : _mm_cvtepi8_epi32(raw);
        if constexpr (VN==0) return _mm_shuffle_epi32(v,_MM_SHUFFLE(0,0,0,0));
        else if constexpr (VN==1) return _mm_shuffle_epi32(v,_MM_SHUFFLE(1,0,1,0));
        else return v;
    }
}
template<unsigned VN,unsigned VL,bool USN>
__forceinline void fastFormat(uint8_t *vu,const uint8_t *src,const uint8_t *end,
                             uint32_t dest,uint32_t count,uint32_t cl,uint32_t wl,FastUnpackState &s)
{
    constexpr unsigned stride=VL==3 ? 2u : (VN+1u)*(4u>>VL);
    uint32_t cycle=0;
    for (uint32_t k=0;k<count;++k,src+=stride)
    {
        __m128i v=fastDecode<VN,VL,USN>(src,end);
        if constexpr ((VN==1 && VL==0) || VN==2)
        {
            const bool zero=s.special8 ? (k ? true : s.zeroW[0]) : s.zeroW[k&15u];
            if (zero) v=_mm_blend_epi16(v,_mm_setzero_si128(),0xc0);
        }
        const __m128i oldRow=s.row;
        if (s.add) v=_mm_add_epi32(v,oldRow);
        uint8_t *dst=vu+dest*16u;
        if (s.mask)
        {
            const uint32_t cyc=cycle<3u ? cycle : 3u;
            const __m128i mem=s.protect[cyc] ? _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst)) : _mm_setzero_si128();
            const __m128i out=_mm_or_si128(
                _mm_or_si128(_mm_and_si128(v,s.specs[cyc][0]),_mm_and_si128(oldRow,s.specs[cyc][1])),
                _mm_or_si128(_mm_and_si128(s.cols[cyc],s.specs[cyc][2]),_mm_and_si128(mem,s.specs[cyc][3])));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst),out);
            if (s.difference) s.row=_mm_blendv_epi8(oldRow,v,s.specs[cyc][0]);
        }
        else
        {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst),v);
            if (s.difference) s.row=v;
        }
        ++dest;
        if (++cycle==wl) { cycle=0;dest+=cl-wl; }
        dest&=0x3ffu;
    }
}
static bool vif1UnpackFast(uint8_t *vu,const uint8_t *data,uint32_t size,uint32_t pos,
                          uint32_t totalBytes,VIFRegisters &regs,uint32_t vn,uint32_t vl,
                          uint32_t count,uint32_t addr,uint32_t cl,uint32_t wl,
                          bool usn,bool mask,bool scan,uint32_t phase)
{
    if (!fastOn() || scan || !vu || cl<wl || (vl==3 && vn!=3) ||
        totalBytes==0 || pos>size || totalBytes>size-pos || censusOn() || colorWatch().on) return false;
    const int hw=hwMode();
    if(hw==0) return false;
    const uint32_t mode=regs.mode&3u;
    const uint8_t *src=data+pos,*end=data+size;
    if (vn==3 && vl==0 && !mask && mode!=1 && mode!=2 && cl==wl)
    {
        const uint32_t first=std::min(count,1024u-addr);
        std::memcpy(vu+addr*16u,src,first*16u);
        if (first<count) std::memcpy(vu,src+first*16u,(count-first)*16u);
        return true;
    }
    FastUnpackState s{};
    s.row=_mm_loadu_si128(reinterpret_cast<const __m128i *>(regs.row));
    s.mask=mask;s.add=vl!=3 && (mode==1 || mode==2);s.difference=s.add && mode==2;
    s.special8=hw==1 && vn==2 && vl==2;
    if (hw==1)
    {
        if (vn==1 && vl==0)
            std::fill(std::begin(s.zeroW),std::end(s.zeroW),recW0(vn,vl,0,phase));
        else if (vn==2 && vl==0)
        {
            const bool even=recW0(vn,vl,0,phase),odd=!even;
            for(uint32_t k=0;k<16;++k) s.zeroW[k]=(k&1u) ? odd : even;
        }
        else if(vn==2 && vl==1)
            for(uint32_t k=0;k<16;++k) s.zeroW[k]=recW0(vn,vl,k,phase);
        else if(vn==2 && vl==2) s.zeroW[0]=recW0(vn,vl,0,phase);
    }
    if (mask)
    {
        const uint32_t maskValue=regs.mask;
        for (uint32_t cyc=0;cyc<4;++cyc)
        {
            alignas(16) int32_t bits[4][4]{};
            for (uint32_t lane=0;lane<4;++lane)
            {
                const uint32_t spec=(maskValue>>((cyc*4u+lane)*2u))&3u;
                bits[spec][lane]=-1;s.protect[cyc]|=spec==3;
            }
            for (uint32_t spec=0;spec<4;++spec) s.specs[cyc][spec]=_mm_load_si128(reinterpret_cast<const __m128i *>(bits[spec]));
            s.cols[cyc]=_mm_set1_epi32(static_cast<int32_t>(regs.col[cyc]));
        }
    }
#define VIF_FAST_FORMAT(N,L) case ((N)*4+(L)): if(usn) fastFormat<N,L,true>(vu,src,end,addr,count,cl,wl,s); else fastFormat<N,L,false>(vu,src,end,addr,count,cl,wl,s); break
    switch (vn*4+vl)
    {
        VIF_FAST_FORMAT(0,0);VIF_FAST_FORMAT(0,1);VIF_FAST_FORMAT(0,2);
        VIF_FAST_FORMAT(1,0);VIF_FAST_FORMAT(1,1);VIF_FAST_FORMAT(1,2);
        VIF_FAST_FORMAT(2,0);VIF_FAST_FORMAT(2,1);VIF_FAST_FORMAT(2,2);
        VIF_FAST_FORMAT(3,0);VIF_FAST_FORMAT(3,1);VIF_FAST_FORMAT(3,2);VIF_FAST_FORMAT(3,3);
        default:return false;
    }
#undef VIF_FAST_FORMAT
    if(s.difference) _mm_storeu_si128(reinterpret_cast<__m128i *>(regs.row),s.row);
    return true;
}

    // TEMP(transformers): pollable VIF1 body profiler; per-thread cells exclude EE pre-scan.
    // Ticks are constant-rate TSC ticks, NOT core cycles. Pseudorandom sampling per class
    // prevents repeated short/long command sequences aliasing a fixed 1-in-16 stride.
    bool unpackProfOn()
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_VIF1_UNPACK_PROF");
            return e && *e && *e != '0'; }();
        return on;
    }
    struct ProfCell { uint64_t cmds=0, vecs=0, ticks=0, sampledVecs=0, samples=0; uint32_t rng=0x9e3779b9u; };
    struct UnpackProfiler
    {
        ProfCell cells[768]{};
        ProfCell direct{}, mpg{};
        uint64_t cmds=0, vecs=0, lastCmds=0, lastVecs=0, lastVs=0;
        uint64_t lastUnpack=0, lastDirect=0, lastMpg=0;
        uint64_t originTsc=__rdtsc();
        std::chrono::steady_clock::time_point origin=std::chrono::steady_clock::now(), last=origin;
        void print(bool table)
        {
            const auto now=std::chrono::steady_clock::now();
            const double dt=std::chrono::duration<double>(now-last).count();
            if (!table && dt<5.0) return;
            std::ostringstream report; // TEMP(transformers): local formatting avoids shared cerr hex/decimal races
            uint64_t ticks=0, sampledVecs=0;
            for (const auto &c:cells) { ticks+=c.ticks; sampledVecs+=c.sampledVecs; }
            const uint64_t vs=ps2dbg::vsyncTick.load(std::memory_order_relaxed);
            const double hz=double(__rdtsc()-originTsc)/std::chrono::duration<double>(now-origin).count();
            const double dvs=double(vs-lastVs);
            if (dt>=5.0) {
                report << "[vif1unpack] vs=" << vs << " lane="
                    << (ps2_perfscope::t_state ? unsigned(ps2_perfscope::t_state->lane):255u)
                    << " cmds/vs=" << (dvs ? double(cmds-lastCmds)/dvs:0)
                    << " vecs/vs=" << (dvs ? double(vecs-lastVecs)/dvs:0)
                    << " unpack_ms_per_s=" << double(ticks-lastUnpack)*16000.0/hz/dt
                    << " direct_ms_per_s=" << double(direct.ticks-lastDirect)*16000.0/hz/dt
                    << " mpg_ms_per_s=" << double(mpg.ticks-lastMpg)*16000.0/hz/dt
                    << " cyc_per_vec=" << (sampledVecs ? double(ticks)/sampledVecs:0)
                    << " tsc_hz=" << hz << " cmds=" << cmds << " vecs=" << vecs
                    << " unpack_ticks=" << ticks*16 << std::endl;
                last=now; lastCmds=cmds; lastVecs=vecs; lastVs=vs;
                lastUnpack=ticks; lastDirect=direct.ticks; lastMpg=mpg.ticks;
            }
            if (table) {
                report << "[vif1unpack:classes] vs=" << vs << " cmds=" << cmds << std::endl;
                for (unsigned id=0;id<768;++id) {
                    const auto &c=cells[id]; if (!c.cmds) continue;
                    unsigned x=id; const unsigned usn=x%2; x/=2;
                    const unsigned cycle=x%3; x/=3; const unsigned mode=x%4; x/=4;
                    const unsigned mask=x%2; x/=2; const unsigned vl=x%4; x/=4;
                    report << "[vif1unpack:class] vn=" << x << " vl=" << vl
                        << " mask=" << mask << " mode=" << mode << " fill=" << (cycle==1)
                        << " skip=" << (cycle==2) << " usn=" << usn << " cmds=" << c.cmds
                        << " vecs=" << c.vecs << " samples=" << c.samples
                        << " sampled_vecs=" << c.sampledVecs << " ticks=" << c.ticks
                        << " cyc_per_vec=" << (c.sampledVecs ? double(c.ticks)/c.sampledVecs:0)
                        << std::endl;
                }
            }
            std::cerr << report.str(); // TEMP(transformers): one write, outside the sampled body
        }
        template<class F> void body(ProfCell &cell, uint32_t count, F &&fn)
        {
            // TEMP(transformers): every-16th-command aliases periodic short/long UNPACKs.
            // Deterministic xorshift sampling keeps the expected inclusion probability 1/16.
            ++cell.cmds;
            uint32_t x=cell.rng; x^=x<<13; x^=x>>17; x^=x<<5; cell.rng=x;
            const bool sample=(x & 15u)==0;
            cell.vecs+=count;
            if (sample) {
                const uint64_t start=__rdtsc(); fn(); const uint64_t end=__rdtsc();
                cell.ticks+=end-start; cell.sampledVecs+=count; ++cell.samples;
            } else fn();
        }
        template<class F> void unpack(unsigned id, uint32_t count, F &&fn)
        {
            body(cells[id],count,std::forward<F>(fn)); ++cmds; vecs+=count;
            // Poll the wall clock only every 1024 commands, outside the timed body.
            if ((cmds & 1023u)==0) print((cmds & 0x3ffffu)==0);
        }
    };
    thread_local UnpackProfiler unpackProfiler;

    // TEMP(transformers): PS2X_VIF_UNPACK_CENSUS=1 - which V2/V3 UNPACK variants the game uses (VIF1), cumulative table
    // every 2^16 commands: format, mask, mode, fill (CL<WL), data word phase, usn; commands, vectors, w forced to 0.
    struct CensusCell
    {
        uint64_t cmds = 0, vecs = 0, w0 = 0;
    };
    bool censusOn()
    {
        static const bool on = []()
        {
            const char *e = std::getenv("PS2X_VIF_UNPACK_CENSUS");
            return e && *e && *e != '0';
        }();
        return on;
    }
    CensusCell s_census[2][3][2][4][2][4][2];
    uint64_t s_censusCmds = 0;
    void censusPrint()
    {
        static const char *fmt[2][3] = {{"V2-32", "V2-16", "V2-8"}, {"V3-32", "V3-16", "V3-8"}};
        std::cerr << "[vifunpack:census] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " cmds=" << s_censusCmds
                  << " hwmode=" << hwMode() << std::endl;
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 3; ++b)
                for (int m = 0; m < 2; ++m)
                    for (int md = 0; md < 4; ++md)
                        for (int f = 0; f < 2; ++f)
                            for (int o = 0; o < 4; ++o)
                                for (int u = 0; u < 2; ++u)
                                {
                                    const CensusCell &c = s_census[a][b][m][md][f][o][u];
                                    if (c.cmds)
                                        std::cerr << "[vifunpack:census]   " << fmt[a][b] << " m=" << m << " mode=" << md
                                                  << " fill=" << f << " phase=" << o << " usn=" << u << " cmds=" << c.cmds
                                                  << " vecs=" << c.vecs << " w0=" << c.w0 << std::endl;
                                }
    }
}

namespace
{
    // TEMP(transformers): PS2X_VIF1_KICKLOG=<guest vsync> logs every VIF1 buffer from that vsync on: i-bits seen,
    // where processing stopped, the last VIFcodes. Buffers without an i-bit are dumped (first 8) to PS2X_DUMP_DIR.
    bool vif1KickLogOn()
    {
        static const long long s_from = []()
        {
            const char *e = std::getenv("PS2X_VIF1_KICKLOG");
            return e ? std::atoll(e) : -1ll;
        }();
        return s_from >= 0 && static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed)) >= s_from;
    }

    struct Vif1BufferTrace
    {
        const uint8_t *data;
        uint32_t size;
        const uint32_t *pos;
        bool on;
        uint32_t ibits = 0u;
        uint32_t codes = 0u;
        uint32_t last[8] = {};

        void code(uint32_t cmd)
        {
            if (!on)
                return;
            last[codes++ & 7u] = cmd;
            if (cmd & 0x80000000u)
                ++ibits;
        }

        ~Vif1BufferTrace()
        {
            if (!on)
                return;
            static uint32_t s_seq = 0u;
            static uint32_t s_dumps = 0u;
            const uint32_t seq = s_seq++;
            const uint64_t vs = ps2dbg::vsyncTick.load(std::memory_order_relaxed);
            std::cerr << "[vif1:buf] #" << seq << " vs=" << vs << " size=" << size << " endPos=" << *pos
                      << " codes=" << codes << " ibits=" << ibits << " last=" << std::hex;
            for (uint32_t i = 0u; i < 8u && i < codes; ++i)
                std::cerr << (i ? "," : "") << last[(codes - 1u - i) & 7u];
            std::cerr << std::dec << std::endl;
            if (ibits == 0u && s_dumps < 8u)
            {
                ++s_dumps;
                const char *dir = std::getenv("PS2X_DUMP_DIR");
                const std::string path = std::string(dir && *dir ? dir : ".") + "/vif1_noibit_" + std::to_string(seq) +
                                         "_vs" + std::to_string(vs) + ".bin";
                std::ofstream f(path, std::ios::binary);
                f.write(reinterpret_cast<const char *>(data), size);
            }
        }
    };
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    // DIRECT/DIRECTHL payload qwords still owed by a DIRECT that was cut off by the end of a
    // processVIF1Data() buffer: only that continuation arrives as raw data. A GIF IMAGE that just
    // spans several DIRECTs continues inside the next DIRECT's payload, after its VIFcodes.
    uint32_t s_vif1DirectRemainQwc = 0u;
    bool s_vif1DirectRemainHl = false;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

// Called on PS2Memory reset and VIF1_FBRST.RST (declared at the call sites; ps2_memory.h is in the PCH).
void ps2ResetVif1DirectContinuation()
{
    s_vif1DirectRemainQwc = 0u;
    s_vif1DirectRemainHl = false;
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
        {
            if (!m_vif1Instant) fenceVu1(); // preceding PATH1 effects precede the VIF i-bit interrupt
            vif0_regs.stat |= (1u << 11);
            // VIFcode i-bit raises INTC 4 (VIF0) unless VIF0_ERR.MII masks it.
            const auto err = m_ioRegisters.find(0x10003820u);
            if (err == m_ioRegisters.end() || (err->second & 1u) == 0u)
                ps2intc::queue(4u);
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            if (!m_vif1Instant) fenceVu1();
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            if (!m_vif1Instant) fenceVu1();
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    // release fix: hardware writes all four lanes (S-32 broadcasts x; V2/V3 as in the VIF1 path).
                    if (vn < 3u && ps2x_vifunpack::hwMode() != 0)
                    {
                        if (vn == 0u)
                            lanes[1] = lanes[2] = lanes[3] = lanes[0];
                        else
                            ps2x_vifunpack::fillMissingLanes(lanes, vn, 0u, false, srcVec, data + sizeBytes, srcIndex - 1u,
                                                             (pos >> 2) & 3u, ps2x_vifunpack::hwMode());
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)

{

    if (sizeBytes == 0u)

        return;

    if (m_vif1Stream && !vif1StreamExecuting()) {
        fenceVu1();
        m_vif1Visible=vif1_regs;
        const auto err=m_ioRegisters.find(0x10003C20u);
        m_vif1Stream->err=err==m_ioRegisters.end() ? 0u : err->second;
        m_vif1Stream->submit(std::vector<uint8_t>(data,data+sizeBytes),
            [this](const uint8_t *p,uint32_t n) { processVIF1Data(p,n); });
        return;
    }

    if (m_vif1Instant && !instantWorker() && m_vif1Scanning!=this) {
        // TEMP(transformers) qualification: carry the caller's release fix TTE tag offsets (if any) into the scan and the worker
        submitVif1Instant(std::vector<uint8_t>(data,data+sizeBytes),{sizeBytes},false,{ps2x_vifunpack::currentChainTte()});return;
    }
    const bool scan=m_vif1Scanning==this;
    auto &parserState=scan ? m_vif1Scan : m_vif1Actual;
    auto &vif1_regs=m_vif1Instant ? parserState.regs : this->vif1_regs;
    auto &s_vif1DirectRemainQwc=m_vif1Instant ? parserState.directRemain : ::s_vif1DirectRemainQwc;
    auto &s_vif1DirectRemainHl=m_vif1Instant ? parserState.directHl : ::s_vif1DirectRemainHl;
    auto &m_vif1PendingPath2ImageQwc=m_vif1Instant ? parserState.imageRemain : this->m_vif1PendingPath2ImageQwc;
    auto &m_vif1PendingPath2DirectHl=m_vif1Instant ? parserState.imageHl : this->m_vif1PendingPath2DirectHl;
    ps2_perfscope::Scope perf(ps2_perfscope::EeVif1); // TEMP(transformers) qualification (VU1/GIF inside are their own buckets; qualification: EE pre-scan in mode 3, worker thread charges its own lane)

    uint32_t pos = 0;
    Vif1BufferTrace trace{data, sizeBytes, &pos, !scan && sizeBytes > 16u && vif1KickLogOn()}; // TEMP(transformers)

    // Hands one DIRECT/DIRECTHL payload to PATH2. GIF packets are parsed standalone, so the
    // leading qwords that finish an IMAGE left open by an earlier payload get a synthetic IMAGE
    // tag; whatever follows them is a fresh GIF stream.
    auto submitDirectPayload = [&](const uint8_t *payload, uint32_t qwc, bool directHl)
    {
        if(scan)return;
        while (m_vif1PendingPath2ImageQwc != 0u && qwc != 0u)
        {
            const uint32_t chunkQw = std::min<uint32_t>(std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwc), 0x7FFFu);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, payload, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);

            payload += chunkQw * 16u;
            qwc -= chunkQw;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
        }
        if (qwc == 0u)
            return;

        submitGifPacket(GifPathId::Path2, payload, qwc * 16u, true, directHl);

        const uint32_t pendingImageQw = pendingGifImageQwc(payload, qwc * 16u);
        if (pendingImageQw != 0u)
        {
            m_vif1PendingPath2ImageQwc = pendingImageQw;
            m_vif1PendingPath2DirectHl = directHl;
        }
    };

    while (pos + 4 <= sizeBytes)
    {
        if (s_vif1DirectRemainQwc != 0u)
        {
            // Rest of a DIRECT payload split across buffers: raw qwords, no VIFcodes in front.
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(s_vif1DirectRemainQwc, availableQw);
            // TEMP(transformers): include continuation chunks in DIRECT body timing.
            auto directBody = [&]() { submitDirectPayload(data + pos, chunkQw, s_vif1DirectRemainHl); };
            if (!scan && ps2x_vifunpack::unpackProfOn())
                ps2x_vifunpack::unpackProfiler.body(ps2x_vifunpack::unpackProfiler.direct,0,directBody);
            else directBody();
            pos += chunkQw * 16u;
            s_vif1DirectRemainQwc -= chunkQw;
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;
        trace.code(cmd); // TEMP(transformers)

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
        {
            if (!m_vif1Instant) fenceVu1(); // preceding PATH1 effects precede the VIF i-bit interrupt
            vif1_regs.stat |= (1u << 11); // INT
            // VIFcode i-bit raises INTC 5 (VIF1) unless VIF1_ERR.MII masks it. Games use this as an
            // end-of-frame signal (Transformers waits on it every frame, else a 3 s watchdog fires).
            if (instantWorker()) {
                if(m_vif1FallbackExecuting==this && !m_vif1FallbackIrqMasked)m_vif1Instant->emit([]{ps2intc::queue(5u);});
            }
            else if(scan) {const auto err=m_ioRegisters.find(0x10003C20u);if(err==m_ioRegisters.end()||!(err->second&1u))++m_vif1ScanIrqs;}
            else if (vif1StreamExecuting()) {
                if (!(m_vif1Stream->err & 1u)) m_vif1Stream->emit([] { ps2intc::queue(5u); });
            } else {
                const auto err = m_ioRegisters.find(0x10003C20u);
                if (err == m_ioRegisters.end() || (err->second & 1u) == 0u) ps2intc::queue(5u);
            }
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            if (!m_vif1Instant) fenceVu1();
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool masked=(imm & 0x8000u)!=0u;
            // v5 merge (release fix x release fix): every path applies the mask the same way, incl. the crossover's
            // one-GS-packet release per unmask window (PS2X_PATH3_MASK_PACKET=0 reverts to drain-all).
            auto applyMask=[this](bool m) {
                const bool wasMasked = m_path3Masked;
                m_path3Masked = m;
                if (wasMasked && !m_path3Masked)
                {
                    // one GS packet per unmask window (GENERIC, see ps2_memory.cpp); PS2X_PATH3_MASK_PACKET=0 reverts
                    static const bool s_onePacket = []() { const char *e = std::getenv("PS2X_PATH3_MASK_PACKET"); return !(e && e[0] == '0'); }();
                    g_path3ReleaseOnePacket = s_onePacket;
                    flushMaskedPath3Packets();
                    g_path3ReleaseOnePacket = false;
                }
            };
            if(m_vif1Instant)parserState.path3Masked=masked;
            if (scan) {m_vif1PredictedPath3Masked=masked;}
            else if (instantWorker()) {
                m_vif1Instant->emit([applyMask,masked] { applyMask(masked); });
            }
            else if (vif1StreamExecuting()) {
                m_vif1Stream->emit([applyMask,masked] { applyMask(masked); });
            } else {
                applyMask(masked);
            }
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            if (!m_vif1Instant) fenceVu1();
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if(scan)++m_vif1ScanMscal;
            else if (instantWorker()) {if(m_vif1FallbackExecuting==this)m_vif1Instant->emit([]{ps2dbg::vu1Mscal.fetch_add(1u,std::memory_order_relaxed);});}
            else if (vif1StreamExecuting()) m_vif1Stream->emit([] { ps2dbg::vu1Mscal.fetch_add(1u, std::memory_order_relaxed); });
            else ps2dbg::vu1Mscal.fetch_add(1u, std::memory_order_relaxed); // TEMP(transformers)
            if (!scan && m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if(scan)++m_vif1ScanMscnt;
            else if (instantWorker()) {if(m_vif1FallbackExecuting==this)m_vif1Instant->emit([]{ps2dbg::vu1Mscnt.fetch_add(1u,std::memory_order_relaxed);});}
            else if (vif1StreamExecuting()) m_vif1Stream->emit([] { ps2dbg::vu1Mscnt.fetch_add(1u, std::memory_order_relaxed); });
            else ps2dbg::vu1Mscnt.fetch_add(1u, std::memory_order_relaxed); // TEMP(transformers)
            if (!scan && m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            if (!m_vif1Instant) fenceVu1();
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            auto mpgBody = [&]() {
            if (!scan && m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    if (pos + mpgBytes <= sizeBytes) ps2_vu1new::mpg(data + pos, instructionCount, destAddr); // TEMP(transformers)
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            };
            if (!scan && ps2x_vifunpack::unpackProfOn())
                ps2x_vifunpack::unpackProfiler.body(ps2x_vifunpack::unpackProfiler.mpg,0,mpgBody);
            else mpgBody();
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            const bool directHl = (opcode == VIF_DIRECTHL);
            const uint32_t sentQw = truncated ? availableQw : qwCount;

            auto directBody = [&]() { if (sentQw > 0) submitDirectPayload(data + pos, sentQw, directHl); };
            if (!scan && ps2x_vifunpack::unpackProfOn())
                ps2x_vifunpack::unpackProfiler.body(ps2x_vifunpack::unpackProfiler.direct,0,directBody);
            else directBody();

            pos += sentQw * 16;
            if (truncated)
            {
                s_vif1DirectRemainQwc = qwCount - sentQw;
                s_vif1DirectRemainHl = directHl;
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            if(scan && (vif1_regs.mode & 3u)==2u && (vn==1u || vn==2u))
                throw Vif1ScanDependency{};
            const bool zeroExtend = (imm & 0x4000u) != 0u;

            { // TEMP(transformers): PS2X_VU1WATCH=<qlo>-<qhi>@<vsFrom>-<vsTo> (hex qword addrs) logs UNPACKs into that range
                struct WatchSpec { uint32_t lo, hi; long long vsFrom, vsTo; };
                static const WatchSpec s_watch = []() -> WatchSpec
                {
                    const char *e = std::getenv("PS2X_VU1WATCH");
                    if (!e)
                        return WatchSpec{1u, 0u, 0, -1};
                    char *n = nullptr;
                    WatchSpec w{};
                    w.lo = static_cast<uint32_t>(std::strtoul(e, &n, 16));
                    w.hi = (n && *n == '-') ? static_cast<uint32_t>(std::strtoul(n + 1, &n, 16)) : w.lo;
                    w.vsFrom = (n && *n == '@') ? std::strtoll(n + 1, &n, 10) : 0;
                    w.vsTo = (n && *n == '-') ? std::strtoll(n + 1, &n, 10) : (1ll << 62);
                    return w;
                }();
                const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
                const uint32_t span = (cl >= wl) ? ((writeVectorCount + wl - 1u) / wl) * cl : writeVectorCount;
                if (s_watch.lo <= s_watch.hi && vsNow >= s_watch.vsFrom && vsNow <= s_watch.vsTo &&
                    vuAddr <= s_watch.hi && vuAddr + span > s_watch.lo)
                {
                    static uint32_t s_logs = 0u;
                    if (s_logs++ < 3000u)
                    {
                        std::cerr << "[vu1watch:unpack] vs=" << vsNow << " cmd=0x" << std::hex << cmd << " vn=" << unsigned(vn)
                                  << " vl=" << unsigned(vl) << " num=" << std::dec << writeVectorCount << " addr=0x" << std::hex
                                  << vuAddr << " span=" << std::dec << span << " usn=" << zeroExtend << " flg="
                                  << ((imm & 0x8000u) != 0u) << " m=" << maskEnable << " cl=" << cl << " wl=" << wl << " mode="
                                  << vif1_regs.mode << " mask=0x" << std::hex << vif1_regs.mask << " row=" << vif1_regs.row[0]
                                  << "," << vif1_regs.row[1] << "," << vif1_regs.row[2] << "," << vif1_regs.row[3] << " col="
                                  << vif1_regs.col[0] << "," << vif1_regs.col[1] << "," << vif1_regs.col[2] << ","
                                  << vif1_regs.col[3] << std::dec << " bytes=" << totalBytes << " pos=" << pos << "/" << sizeBytes
                                  << ((pos + totalBytes > sizeBytes) ? " TRUNCATED(dropped)" : "") << " src=" << std::hex;
                        // TEMP(transformers): log actual payload only, optionally up to a complete constant block.
                        static const uint32_t watchWords = []()
                        {
                            const char *e = std::getenv("PS2X_VU1WATCH_WORDS");
                            return e ? std::min(64u, static_cast<uint32_t>(std::strtoul(e, nullptr, 10))) : 8u;
                        }();
                        for (uint32_t i = 0; i < std::min(watchWords, totalBytes / 4u) && pos + i * 4u + 4u <= sizeBytes; ++i)
                        {
                            uint32_t wv = 0;
                            std::memcpy(&wv, data + pos + i * 4u, 4);
                            std::cerr << (i ? "," : "") << wv;
                        }
                        std::cerr << std::dec << std::endl;
                    }
                }
            }

            if (!m_vif1Instant) fenceVu1(); // UNPACK reads/overwrites memory exclusively owned by a pending VU job
            if(scan && (vif1_regs.mode & 3u)!=2u) {
                pos+=totalBytes;if(pos>sizeBytes)break;continue;
            }
            // release fix: hardware qualification and the hardware quadword word phase of this UNPACK's data.
            const int hwUnpack = (vn == 1u || vn == 2u) && vl != 3u ? ps2x_vifunpack::hwMode() : 0;
            const uint32_t hwPhase = hwUnpack ? ps2x_vifunpack::hwWordPhase(pos) : 0u;
            ps2x_vifunpack::CensusCell *census = nullptr; // TEMP(transformers) PS2X_VIF_UNPACK_CENSUS
            if ((vn == 1u || vn == 2u) && vl != 3u && ps2x_vifunpack::censusOn())
            {
                census = &ps2x_vifunpack::s_census[vn - 1u][vl][maskEnable ? 1 : 0][vif1_regs.mode & 3u][cl < wl ? 1 : 0]
                                                  [ps2x_vifunpack::hwWordPhase(pos)][zeroExtend ? 1 : 0];
                ++census->cmds;
                if ((++ps2x_vifunpack::s_censusCmds & 0xFFFFu) == 0u)
                    ps2x_vifunpack::censusPrint();
            }

            // TEMP(transformers): same body boundary will enclose the optional fast kernel.
            auto unpackBody = [&]() {
                if (ps2x_vifunpack::vif1UnpackFast(m_vu1Data,data,sizeBytes,pos,totalBytes,vif1_regs,
                    vn,vl,writeVectorCount,vuAddr,cl,wl,zeroExtend,maskEnable,scan,hwPhase)) return;
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    if(!scan)std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    uint32_t srcVecIndex = 0u; // release fix: index of this source vector inside the UNPACK
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        srcVecIndex = srcIndex;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5 (RGBA5551): the VIF expands each 5-bit channel to 8 bits (<<3) and the 1-bit alpha to 0x80.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x1Fu) << 3;
                        decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                        decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                        decompressed[3] = ((packed >> 15) & 0x01u) << 7;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // release fix: the lanes a V2/V3 format has no data for are written too (PS2X_VIF_UNPACK_HW).
                    if (hwUnpack && decoded && handledFormat)
                    {
                        ps2x_vifunpack::fillMissingLanes(decompressed, vn, vl, zeroExtend, srcVec, data + sizeBytes,
                                                         srcVecIndex, hwPhase, hwUnpack);
                    }
                    if (census && decoded) // TEMP(transformers)
                    {
                        ++census->vecs;
                        if (ps2x_vifunpack::recW0(vn, vl, srcVecIndex, ps2x_vifunpack::hwWordPhase(pos)))
                            ++census->w0;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        if(!scan)std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    // Ported from KIexster/god-of-war-recomp 8841ef0 (Romeo), GPL-3.0: whole-qword UNPACK.
                    // Keep the common store and color-watch below, including preserved V2/V3 lanes.
                    if (!maskEnable && decoded && handledFormat && (!canAdd || (mode != 1u && mode != 2u)))
                        std::memcpy(lanes, decompressed, sizeof(lanes));
                    else
                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    if(!scan)std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));

                    { // TEMP(transformers): PS2X_VIF_COLORWATCH=<w0>,<w1>,<w2>,<w3> (hex) logs every UNPACK write that lands
                      // exactly those four words: command, destination, chain offset of the source vector, raw bytes.
                        const auto &s_cw=ps2x_vifunpack::colorWatch();
                        if (s_cw.on && lanes[0] == s_cw.w[0] && lanes[1] == s_cw.w[1] && lanes[2] == s_cw.w[2] && lanes[3] == s_cw.w[3])
                        {
                            static uint32_t s_logs = 0u;
                            if (s_logs++ < 200u)
                            {
                                std::cerr << "[vifcolorwatch] vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << " cmd=0x"
                                          << std::hex << cmd << " vn=" << unsigned(vn) << " vl=" << unsigned(vl) << " dest=0x" << destVec
                                          << " addr=0x" << vuAddr << " num=" << std::dec << writeVectorCount << " idx=" << writeIndex
                                          << " mode=" << mode << " m=" << maskEnable << " mask=0x" << std::hex << vif1_regs.mask
                                          << " usn=" << zeroExtend << " srcOff=" << std::dec
                                          << (srcVec ? static_cast<long long>(srcVec - data) : -1ll) << "/" << sizeBytes << " raw=";
                                for (uint32_t i = 0; srcVec && i < bytesPerVector && i < 16u; ++i)
                                    std::cerr << std::hex << std::setw(2) << std::setfill('0') << unsigned(srcVec[i]);
                                std::cerr << std::dec << std::setfill(' ') << std::endl;
                            }
                        }
                    }
                }
            }

            };
            if (!scan && ps2x_vifunpack::unpackProfOn() && m_vu1Data && pos + totalBytes <= sizeBytes) {
                const unsigned cls=((((vn*4u+vl)*2u+unsigned(maskEnable))*4u+(vif1_regs.mode&3u))*3u
                    +(cl<wl ? 1u:cl>wl ? 2u:0u))*2u+unsigned(zeroExtend);
                ps2x_vifunpack::unpackProfiler.unpack(cls,writeVectorCount,unpackBody);
            } else unpackBody();

            // GAME(transformers) STOPGAP: the level's per-object VU1 block (56-qword V4-32 UNPACK, EE method 0x2688F0)
            // carries M1 at block+3..+6 and the field-2 matrix M2 at block+7..+10; M2 is copied by the game from a
            // scratchpad matrix (SPR 0x70000630) that something clobbers in our runtime (0xFFE0E0xx colour words, NaN as
            // floats) -> every field-2 vertex XYZ=0 and every other display line dark. Replace a NaN/Inf M2 with M1 until
            // the clobber is found (PS2X_SPRWATCH). PS2X_FIELD2_STOPGAP=0 disables.
            if (!scan && m_vu1Data && vn == 3u && vl == 0u && writeVectorCount == 56u && cl == wl && !maskEnable)
            {
                static const bool s_field2Stopgap = []()
                {
                    const char *e = std::getenv("PS2X_FIELD2_STOPGAP");
                    return !(e && e[0] == '0');
                }();
                if (s_field2Stopgap)
                {
                    uint8_t *m1 = m_vu1Data + ((vuAddr + 3u) & 0x3FFu) * 16u;
                    uint8_t *m2 = m_vu1Data + ((vuAddr + 7u) & 0x3FFu) * 16u;
                    bool bad = false;
                    for (int i = 0; i < 16; ++i)
                    {
                        uint32_t w = 0;
                        std::memcpy(&w, m2 + i * 4, 4);
                        // REVIEW(port-gow batch 1): with the FPU/VU0 clamp (ps2_runtime_macros.h) a poisoned M2 comes out
                        // as +-FLT_MAX (exponent 254) instead of NaN, so also treat |x| >= 2^127 as bad (no real
                        // view/projection matrix has such entries).
                        bad |= (w & 0x7F800000u) >= 0x7F000000u;
                    }
                    if (bad && ((vuAddr + 10u) & 0x3FFu) > ((vuAddr + 3u) & 0x3FFu))
                    {
                        std::memmove(m2, m1, 64u);
                        static uint32_t s_logs = 0u;
                        if (s_logs++ < 3u)
                            std::cerr << "[field2:stopgap] replaced NaN M2 at VU1 q0x" << std::hex << ((vuAddr + 7u) & 0x3FFu)
                                      << std::dec << " with M1" << std::endl;
                    }
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
    if(ps2x_vifunpack::g_fastOverride>=0) ps2x_vifunpack::g_parsePosTest=pos; // TEMP(transformers)
}

void PS2Memory::submitVif1Instant(std::vector<uint8_t> bytes,std::vector<uint32_t> segments,bool dma,std::vector<std::vector<uint32_t>> tte)
{
    // TEMP(transformers) qualification: every parse of segment i (pre-scan, fallback, worker) sees that segment's release fix TTE offsets,
    // exactly like the mode-0 walker's beginChain/processVIF1Data/endChain.
    tte.resize(segments.size());
    auto parseAll=[this](const std::vector<uint8_t> &in,const std::vector<uint32_t> &seg,const std::vector<std::vector<uint32_t>> &offs) {
        const uint8_t *p=in.data();
        try {for(size_t i=0;i<seg.size();++i){ps2x_vifunpack::setChainTte(offs[i]);processVIF1Data(p,seg[i]);p+=seg[i];}}
        catch(...){ps2x_vifunpack::endChain();throw;}
        ps2x_vifunpack::endChain();
    };
    const uint32_t ctl=m_vif1CaptureControls ? m_vif1CaptureControls() : 0;
    const auto mcBefore=ps2dbg::vu1Mscal.load(std::memory_order_relaxed),mnBefore=ps2dbg::vu1Mscnt.load(std::memory_order_relaxed);
    const auto initial=m_vif1Scan;
    m_vif1ScanMscal=m_vif1ScanMscnt=m_vif1ScanIrqs=0;
    m_vif1Scan.regs=vif1_regs;
    auto previous=m_vif1Scanning;m_vif1Scanning=this;
    try {parseAll(bytes,segments,tte);}
    catch(const Vif1ScanDependency&) {
        // STMOD2 V2/V3 ROW inherits VU memory. It is a true dependency, absent from
        // the normal game streams: execute this whole job exactly, at +0 guest cycles.
        m_vif1Scanning=previous;m_vif1Scan=initial;m_vif1PredictedPath3Masked=initial.path3Masked;
        const auto err=m_ioRegisters.find(0x10003C20u);
        const bool masked=err!=m_ioRegisters.end()&&(err->second&1u);
        m_vif1Instant->enqueue([this,parseAll,bytes=std::move(bytes),segments=std::move(segments),tte=std::move(tte),masked] {
            m_vif1FallbackExecuting=this;
            m_vif1FallbackIrqMasked=masked;
            try {parseAll(bytes,segments,tte);}
            catch(...){m_vif1FallbackExecuting=nullptr;throw;}
            m_vif1FallbackExecuting=nullptr;
        },dma,ctl);
        m_vif1Instant->fence();vif1_regs=m_vif1Actual.regs;m_vif1Scan=m_vif1Actual;m_vif1PredictedPath3Masked=m_vif1Actual.path3Masked;
        return;
    }
    catch(...){m_vif1Scanning=previous;m_vif1Scan=initial;m_vif1PredictedPath3Masked=initial.path3Masked;throw;}
    m_vif1Scanning=previous;
    ps2dbg::vu1Mscal.fetch_add(m_vif1ScanMscal,std::memory_order_relaxed);
    ps2dbg::vu1Mscnt.fetch_add(m_vif1ScanMscnt,std::memory_order_relaxed);
    for(uint64_t i=0;i<m_vif1ScanIrqs;++i)ps2intc::queue(5u);
    const auto expected=m_vif1Scan.regs;
    vif1_regs=expected; // final EE-visible state, at the kick's guest cycle
    if(!(ctl&0xC00u) && m_vif1Instant->publishStatus &&
       (ps2dbg::vu1Mscal.load(std::memory_order_relaxed)!=mcBefore || ps2dbg::vu1Mscnt.load(std::memory_order_relaxed)!=mnBefore))
        m_vif1Instant->publishStatus(0);
    m_vif1Instant->enqueue([this,parseAll,bytes=std::move(bytes),segments=std::move(segments),tte=std::move(tte),expected] {
        parseAll(bytes,segments,tte);
        if(std::memcmp(&m_vif1Actual.regs,&expected,sizeof(expected)))
            throw std::runtime_error("VU1 pre-scan oracle mismatch");
        uint32_t statistic=0;std::memcpy(&statistic,m_vu1Data+0xB0,4);
        m_vif1LastStatistic.store(statistic,std::memory_order_release);
    },dma,ctl);
    // D/T is a true dependency. Disabled D/T leaves bits 8..10 known clear.
    if(ctl & 0xC00u)m_vif1Instant->fence();
}
