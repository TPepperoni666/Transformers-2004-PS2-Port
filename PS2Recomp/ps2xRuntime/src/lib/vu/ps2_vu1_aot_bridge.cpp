#include "../ps2_vu1new.h" // TEMP(transformers)
// TEMP(transformers): optional local-only VU1 AOT dispatch (Codex vu1aot round 3, private qualification evidence).
// GAME(transformers): the generated images (src/vu1_aot_private, gitignored) are derived from the game - never commit/publish them.
// Build: -DPS2X_WITH_VU1_AOT=ON (default OFF). Runtime: PS2X_VU1_AOT=1 (default off). Notes: private qualification evidence
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include <cstdint>
#include "ps2_vu1_static_api.h"
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include "../debug_counters.h"

// Implemented in the private, generated object library.
int ps2Vu1AotIdentify(const uint8_t* code,uint32_t size,uint32_t entry);
bool ps2Vu1AotRun(int image,void* state,bool resume,uint8_t* code,uint8_t* data,
                 GS& gs,PS2Memory* memory,uint32_t entry,uint32_t top,uint32_t itop,uint32_t budget,void* transfer);
// TEMP(transformers) qualification (release fix): running count of calls the package's AOT started and then finished in its
// copied interpreter (mid-call fallback: the call reached a pc without generated code). Exported by the packages from
// coverage round 3 on (generated/backend.cpp); an older package resolves to the stub (UINT64_MAX = not reported).
extern "C" uint64_t ps2Vu1AotFallbackCalls();
#if defined(_MSC_VER)
extern "C" uint64_t ps2Vu1AotFallbackCallsUnsupported(){return UINT64_MAX;}
#pragma comment(linker,"/alternatename:ps2Vu1AotFallbackCalls=ps2Vu1AotFallbackCallsUnsupported")
#else
extern "C" __attribute__((weak)) uint64_t ps2Vu1AotFallbackCalls(){return UINT64_MAX;}
#endif
namespace ps2_perfscope { void noteVu1MidFallback(); void noteVu1Declined(); } // TEMP(transformers) qualification: ps2_perfscope.cpp (header unchanged)
// TEMP(transformers) qualification (release fix): mid-call fallback probe. The package records where/why an accepted call dropped to its copied interpreter and
// how long that interpreter ran (hooks added by private qualification evidence into generated/backend.cpp). A package without
// the hooks resolves to the stub (pc=UINT32_MAX -> rows show fbpc=n/a).
extern "C" void ps2Vu1AotProbeLast(uint32_t* pc,uint32_t* why,uint64_t* ns);
#if defined(_MSC_VER)
extern "C" void ps2Vu1AotProbeLastUnsupported(uint32_t* pc,uint32_t* why,uint64_t* ns){*pc=UINT32_MAX;*why=0;*ns=0;}
#pragma comment(linker,"/alternatename:ps2Vu1AotProbeLast=ps2Vu1AotProbeLastUnsupported")
#else
extern "C" __attribute__((weak)) void ps2Vu1AotProbeLast(uint32_t* pc,uint32_t* why,uint64_t* ns){*pc=UINT32_MAX;*why=0;*ns=0;}
#endif

// TEMP(transformers): identity memo for the single VU1 instance (kept here, not in ps2_vu1.h, so the class
// layout is independent of PS2X_WITH_VU1_AOT). Negative results are memoized too; generation covers VIF MPG
// and EE stores into VU1 micro memory.
namespace
{
struct AotCache
{
    const void* owner=nullptr;
    const PS2Memory* memory=nullptr;
    const uint8_t* code=nullptr;
    uint64_t generation=0;
    int image=-1;
    std::array<int,2048> entries{}; // TEMP(transformers): generation-local entry-aware footprint lookup
    bool valid=false;
    uint64_t fnv=0; // TEMP(transformers): capture key, only computed with PS2X_VU1AOT_CAPTURE
};
thread_local AotCache aotCache;
// TEMP(transformers): call accounting, printed at power-of-two acceptance counts and at exit.
struct AotStats
{
    std::atomic<uint64_t> accepted{0},transfers{0},declinedMode{0},declinedImage{0},declinedEntry{0},declinedMask{0},unknownImages{0};
    std::atomic<uint64_t> midFallback{0}; // TEMP(transformers) qualification (release fix): accepted calls that fell back mid-call
    std::array<std::atomic<uint64_t>,64> perImage{}; // TEMP(transformers): 23+ images after the lane-aot census
    void print(const char* why)
    {
        std::fprintf(stderr,"[vu1aot] %s vs=%llu accepted=%llu transfers=%llu declined: mode=%llu image=%llu entry=%llu mask=%llu unknownImages=%llu",why,
            (unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed),
            (unsigned long long)accepted.load(),(unsigned long long)transfers.load(),(unsigned long long)declinedMode.load(),
            (unsigned long long)declinedImage.load(),(unsigned long long)declinedEntry.load(),(unsigned long long)declinedMask.load(),
            (unsigned long long)unknownImages.load());
        // TEMP(transformers) qualification (release fix): midcall = accepted calls that dropped to the interpreter mid-call.
        if(ps2Vu1AotFallbackCalls()==UINT64_MAX)std::fprintf(stderr," midcall=n/a perImage:");
        else std::fprintf(stderr," midcall=%llu perImage:",(unsigned long long)midFallback.load());
        for(size_t i=0;i<perImage.size();++i)
            if(const uint64_t n=perImage[i].load())std::fprintf(stderr," %zu=%llu",i,(unsigned long long)n);
        std::fprintf(stderr,"\n");
        ps2Vu1StaticPrintStats();
        std::fflush(stderr);
    }
    ~AotStats(){if(accepted.load()||declinedMode.load()||declinedImage.load()||declinedEntry.load())print("exit");}
};
AotStats aotStats;

// TEMP(transformers): PS2X_VU1AOT_CAPTURE=1 (or a directory; 1 = PS2X_DUMP_DIR) - for calls the AOT declines on an
// unknown image or unknown entry, write vu1dump-format files (header {word0 (MSCNT: 0x80000000|pc), top, itop,
// sizeof(VU1State)} + 16K micro memory + 16K data memory + VU1State) before and after the call, for the first
// PS2X_VU1AOT_CAPTURE_PER (default 6) calls of every (image, entry, MSCAL/MSCNT) key and at most PS2X_VU1AOT_CAPTURE_MAX
// (default 6000) calls overall -> private qualification evidence census input. The captured call runs through the ordinary
// interpreter (execute()/resume() re-entered with the AOT bypassed), so guest behaviour is unchanged.
struct AotCapture
{
    bool on=false;
    std::string dir;
    uint32_t per=6,max=6000,written=0;
    std::unordered_map<uint64_t,uint32_t> keys;          // fnv(image) ^ entry/kind -> captured calls
    std::unordered_map<uint64_t,uint64_t> unknownCalls;  // fnv(image) -> declined calls (unknown images only)
};
AotCapture& aotCapture()
{
    static AotCapture c=[] {
        AotCapture out;
        const char* p=std::getenv("PS2X_VU1AOT_CAPTURE");
        if(!p||!*p||std::strcmp(p,"0")==0)return out;
        out.on=true;
        if(std::strcmp(p,"1")==0){const char* d=std::getenv("PS2X_DUMP_DIR");out.dir=d&&*d?d:".";}else out.dir=p;
        if(const char* n=std::getenv("PS2X_VU1AOT_CAPTURE_PER"))out.per=uint32_t(std::strtoul(n,nullptr,10));
        if(const char* n=std::getenv("PS2X_VU1AOT_CAPTURE_MAX"))out.max=uint32_t(std::strtoul(n,nullptr,10));
        std::fprintf(stderr,"[vu1aot:cap] capture on dir=%s per=%u max=%u\n",out.dir.c_str(),out.per,out.max);
        return out;
    }();
    return c;
}
thread_local bool aotBypass=false;
uint64_t aotFnv64(const uint8_t* p,size_t n)
{
    uint64_t h=1469598103934665603ull;
    for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}
    return h;
}
void aotWriteDump(const std::string& path,uint32_t word0,uint32_t top,uint32_t itop,const uint8_t* code,const uint8_t* data,const VU1State& st)
{
    std::ofstream out(path,std::ios::binary);
    const uint32_t header[4]={word0,top,itop,uint32_t(sizeof(VU1State))};
    out.write(reinterpret_cast<const char*>(header),sizeof(header));
    out.write(reinterpret_cast<const char*>(code),16384);
    out.write(reinterpret_cast<const char*>(data),16384);
    out.write(reinterpret_cast<const char*>(&st),sizeof(VU1State));
}
// TEMP(transformers): PS2X_VU1AOT_HASH=1 - in-game exactness check. Every VU1 call (AOT or interpreter, with or without
// PS2X_VU1_AOT) is wrapped; afterwards VU1State (registers, flags, pc, cycles) + the 16 KB data memory are folded into a
// running 64-bit hash, printed every 2^16 calls as "[vu1aot:hash] calls=N vs=V hash=H". Runs with AOT on and off must print
// identical lines (same guest call sequence). Costs one 16 KB pass per call.
thread_local bool aotHashing=false;
struct AotHash { uint64_t h=0x9E3779B97F4A7C15ull, calls=0; };
AotHash aotHashState;
bool aotHashOn()
{
    static const bool on=[] {const char* p=std::getenv("PS2X_VU1AOT_HASH");return p&&*p&&std::strcmp(p,"0")!=0;}();
    return on;
}
void aotHashFold(const VU1State& st,const uint8_t* data)
{
    uint64_t h=aotHashState.h;
    auto mix=[&h](uint64_t v){h^=v;h*=0x100000001B3ull;h^=h>>29;};
    // Field by field (VU1State has padding bytes whose contents differ between the AOT copy and the interpreter).
    {uint64_t v;const uint8_t* vf=reinterpret_cast<const uint8_t*>(st.vf);for(size_t i=0;i<sizeof(st.vf);i+=8){std::memcpy(&v,vf+i,8);mix(v);}}
    for(int i=0;i<16;++i)mix(uint32_t(st.vi[i]));
    {uint32_t w[8];std::memcpy(w,st.acc,16);std::memcpy(w+4,&st.q,4);std::memcpy(w+5,&st.p,4);std::memcpy(w+6,&st.i,4);w[7]=st.r;for(uint32_t x:w)mix(x);}
    mix(st.pc);mix(st.mac);mix(st.clip);mix(st.status);mix(st.cycles);
    mix(uint64_t(st.ebit)|uint64_t(st.haltAfterDelaySlot)<<1|uint64_t(st.dBitEnabled)<<2|uint64_t(st.tBitEnabled)<<3|
        uint64_t(st.stoppedByD)<<4|uint64_t(st.stoppedByT)<<5|uint64_t(st.branchPending)<<6);
    mix(st.top);mix(st.itop);mix(st.branchTarget);mix(st.branchDelay);
    for(size_t i=0;i<16384;i+=8){uint64_t v;std::memcpy(&v,data+i,8);mix(v);}
    aotHashState.h=h;
    const uint64_t n=++aotHashState.calls;
    if((n&0xFFFFu)==0)
    {
        std::fprintf(stderr,"[vu1aot:hash] calls=%llu vs=%llu hash=%016llx\n",(unsigned long long)n,
                     (unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed),(unsigned long long)h);
        std::fflush(stderr);
    }
}
// TEMP(transformers) vu0fast1: independent VU0 field-wise hash stream.
thread_local bool vu0Hashing=false;
struct Vu0Hash { uint64_t h=0x9E3779B97F4A7C15ull, calls=0; };
Vu0Hash vu0HashState;
bool vu0HashOn()
{
    static const bool on=[] {const char* p=std::getenv("PS2X_VU0AOT_HASH");return p&&*p&&std::strcmp(p,"0")!=0;}();
    return on;
}
void vu0HashFold(const VU1State& st,const uint8_t* data)
{
    uint64_t h=vu0HashState.h;
    auto mix=[&h](uint64_t v){h^=v;h*=0x100000001B3ull;h^=h>>29;};
    // Field by field (VU1State has padding bytes whose contents differ between the AOT copy and the interpreter).
    {uint64_t v;const uint8_t* vf=reinterpret_cast<const uint8_t*>(st.vf);for(size_t i=0;i<sizeof(st.vf);i+=8){std::memcpy(&v,vf+i,8);mix(v);}}
    for(int i=0;i<16;++i)mix(uint32_t(st.vi[i]));
    {uint32_t w[8];std::memcpy(w,st.acc,16);std::memcpy(w+4,&st.q,4);std::memcpy(w+5,&st.p,4);std::memcpy(w+6,&st.i,4);w[7]=st.r;for(uint32_t x:w)mix(x);}
    mix(st.pc);mix(st.mac);mix(st.clip);mix(st.status);mix(st.cycles);
    mix(uint64_t(st.ebit)|uint64_t(st.haltAfterDelaySlot)<<1|uint64_t(st.dBitEnabled)<<2|uint64_t(st.tBitEnabled)<<3|
        uint64_t(st.stoppedByD)<<4|uint64_t(st.stoppedByT)<<5|uint64_t(st.branchPending)<<6);
    mix(st.top);mix(st.itop);mix(st.branchTarget);mix(st.branchDelay);
    for(size_t i=0;i<4096;i+=8){uint64_t v;std::memcpy(&v,data+i,8);mix(v);}
    vu0HashState.h=h;
    const uint64_t n=++vu0HashState.calls;
    if((n&0x3FFu)==0)
    {
        std::fprintf(stderr,"[vu0aot:hash] calls=%llu vs=%llu hash=%016llx\n",(unsigned long long)n,
                     (unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed),(unsigned long long)h);
        std::fflush(stderr);
    }
}
// TEMP(transformers) qualification (release fix): PS2X_VU1AOT_MIDPROBE=1 (or a directory; 1 = PS2X_DUMP_DIR) - mid-call fallback probe.
// For every accepted AOT call: host time of the call. For calls that fell back mid-call: a row per (image, call pc, MSCAL/MSCNT,
// fallback pc, why) with count, interpreter ns (time the package's copied interpreter ran after the hand-over), call ns, first/last
// vsync; plus vu1dump-format pre/post files of the first PS2X_VU1AOT_MIDPROBE_PER (default 8) calls per row, at most
// PS2X_VU1AOT_MIDPROBE_MAX (default 3000) calls -> coverage census input (private qualification evidence). Table: stderr every 2^18 accepted calls
// ("[vu1aot:mid]" lines) and <dir>/midprobe_table.txt rewritten every 2^16 accepted calls (the game exits without destructors).
// Off (default): one predictable branch per accepted call, nothing else.
struct MidProbe
{
    bool on=false;
    std::string dir;
    uint32_t per=8,max=3000,written=0;
    struct Row{uint64_t n=0,interpNs=0,callNs=0,firstVs=0,lastVs=0;uint32_t dumps=0;};
    std::map<std::tuple<int,uint32_t,int,uint32_t,uint32_t>,Row> rows; // image, call pc, MSCNT, fallback pc, why
    std::array<uint64_t,160> perBucket{};                             // fallbacks per 300 vsyncs
    uint64_t calls=0,callNs=0,fbCalls=0,fbCallNs=0,fbInterpNs=0;
    std::mutex mu;
    void table(std::FILE* f,size_t maxRows)
    {
        std::vector<std::pair<std::tuple<int,uint32_t,int,uint32_t,uint32_t>,Row>> v(rows.begin(),rows.end());
        std::sort(v.begin(),v.end(),[](const auto& a,const auto& b){return a.second.interpNs>b.second.interpNs;});
        std::fprintf(f,"[vu1aot:mid] vs=%llu calls=%llu call_ms=%.1f fbCalls=%llu fbCall_ms=%.1f fbInterp_ms=%.1f rows=%zu dumps=%u\n",
            (unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed),(unsigned long long)calls,callNs/1e6,
            (unsigned long long)fbCalls,fbCallNs/1e6,fbInterpNs/1e6,rows.size(),written);
        size_t k=0;
        for(const auto& kv:v)
        {
            if(k++>=maxRows)break;
            const int img=std::get<0>(kv.first);const uint32_t pc=std::get<1>(kv.first);const int cont=std::get<2>(kv.first);
            const uint32_t fb=std::get<3>(kv.first);const uint32_t why=std::get<4>(kv.first);const Row& r=kv.second;
            std::fprintf(f,"[vu1aot:mid] row image=%d pc=0x%x %s fbpc=%s0x%x why=%s n=%llu interp_ms=%.2f call_ms=%.2f vs=%llu-%llu dumps=%u\n",
                img,pc,cont?"MSCNT":"MSCAL",fb==UINT32_MAX?"n/a ":"",fb==UINT32_MAX?0u:fb,why==1?"nocode":why==2?"declined":"?",
                (unsigned long long)r.n,r.interpNs/1e6,r.callNs/1e6,(unsigned long long)r.firstVs,(unsigned long long)r.lastVs,r.dumps);
        }
    }
    void writeFile()
    {
        const std::string path=dir+"/midprobe_table.txt";
        std::FILE* f=std::fopen(path.c_str(),"w");
        if(!f)return;
        table(f,SIZE_MAX);
        std::fprintf(f,"[vu1aot:mid] perBucket(300vs):");
        for(size_t i=0;i<perBucket.size();++i)if(perBucket[i])std::fprintf(f," %zu=%llu",i*300,(unsigned long long)perBucket[i]);
        std::fprintf(f,"\n");
        std::fclose(f);
    }
};
MidProbe& midProbe()
{
    static MidProbe p;
    static const bool init=[] {
        const char* e=std::getenv("PS2X_VU1AOT_MIDPROBE");
        if(!e||!*e||std::strcmp(e,"0")==0)return true;
        p.on=true;
        if(std::strcmp(e,"1")==0){const char* d=std::getenv("PS2X_DUMP_DIR");p.dir=d&&*d?d:".";}else p.dir=e;
        if(const char* n=std::getenv("PS2X_VU1AOT_MIDPROBE_PER"))p.per=uint32_t(std::strtoul(n,nullptr,10));
        if(const char* n=std::getenv("PS2X_VU1AOT_MIDPROBE_MAX"))p.max=uint32_t(std::strtoul(n,nullptr,10));
        uint32_t pc=0,why=0;uint64_t ns=0;ps2Vu1AotProbeLast(&pc,&why,&ns);
        std::fprintf(stderr,"[vu1aot:mid] probe on dir=%s per=%u max=%u packageHooks=%s\n",p.dir.c_str(),p.per,p.max,pc==UINT32_MAX?"no":"yes");
        std::fflush(stderr);
        return true;
    }();
    (void)init;
    return p;
}
void aotPrintUnknown()
{
    AotCapture& c=aotCapture();
    std::fprintf(stderr,"[vu1aot:cap] unknownImages distinct=%zu captured=%u keys=%zu:",c.unknownCalls.size(),c.written,c.keys.size());
    for(const auto& kv:c.unknownCalls)std::fprintf(stderr," %016llx=%llu",(unsigned long long)kv.first,(unsigned long long)kv.second);
    std::fprintf(stderr,"\n");
    std::fflush(stderr);
}
}

// TEMP(transformers): opt-in static dispatch; default whole-image footprint package.
bool ps2Vu1StaticPrepare(const uint8_t*,unsigned,const PS2Memory*,uint64_t,unsigned);
bool ps2Vu1StaticRun(int,void*,bool,uint8_t*,uint8_t*,GS&,PS2Memory*,uint32_t,uint32_t,uint32_t,uint32_t,void*);
uint64_t ps2Vu1StaticFallbackCalls();

void ps2_vu1new::aotCounters(uint64_t& d,uint64_t& u,uint64_t& m) { // TEMP(transformers)
    d=aotStats.declinedMode.load(std::memory_order_relaxed)+aotStats.declinedImage.load(std::memory_order_relaxed)
      +aotStats.declinedEntry.load(std::memory_order_relaxed)+aotStats.declinedMask.load(std::memory_order_relaxed);
    u=aotStats.unknownImages.load(std::memory_order_relaxed);
    m=ps2Vu1AotFallbackCalls()==UINT64_MAX?UINT64_MAX:aotStats.midFallback.load(std::memory_order_relaxed);
}

bool VU1Interpreter::tryAot(uint8_t* code,uint32_t codeSize,uint8_t* data,uint32_t dataSize,
                           GS& gs,PS2Memory* memory,uint32_t entry,uint32_t top,uint32_t itop,
                           uint32_t budget,bool continuing)
{
    static const bool enabled=[] {const char* p=std::getenv("PS2X_VU1_AOT");return p&&*p&&std::strcmp(p,"0")!=0;}();
    static const bool runToEnd=[] {const char* p=std::getenv("PS2X_VU1_RUN_TO_END");return !p||std::strcmp(p,"0")!=0;}();
    if(aotHashOn() && !aotHashing && m_unit==Unit::VU1 && dataSize==16384) // TEMP(transformers): PS2X_VU1AOT_HASH wrapper
    {
        aotHashing=true;
        if(continuing)resume(code,codeSize,data,dataSize,gs,memory,top,itop,budget);
        else execute(code,codeSize,data,dataSize,gs,memory,entry,top,itop,budget);
        aotHashing=false;
        aotHashFold(m_state,data);
        return true;
    }
    // TEMP(transformers) vu0fast1: hash observer only; VU1 AOT still declines VU0.
    if(vu0HashOn() && !vu0Hashing && m_unit==Unit::VU0 && dataSize==4096)
    {
        vu0Hashing=true;
        if(continuing)resume(code,codeSize,data,dataSize,gs,memory,top,itop,budget);
        else execute(code,codeSize,data,dataSize,gs,memory,entry,top,itop,budget);
        vu0Hashing=false;
        vu0HashFold(m_state,data);
        return true;
    }
    if(aotBypass)return false; // TEMP(transformers): re-entered from a capture below - run the interpreter
    if(!enabled || !runToEnd || budget<=64 || m_unit!=Unit::VU1 || !memory ||
       code!=memory->peekVU1CodeForExecution() || codeSize!=16384 || dataSize!=16384 || m_programPending ||
       pipelinesPending() || m_cycle<m_fastMaxWriteReady){if(enabled&&m_unit==Unit::VU1)++aotStats.declinedMode;return false;}
    const uint32_t pc=continuing?m_state.pc:entry;
    static const bool staticOn=[] {const char* p=std::getenv("PS2X_VU1_AOT_STATIC");return p&&*p&&std::strcmp(p,"0")!=0;}();
    // Memoize identity, including negative results. Full equality disambiguates hash collisions.
    // Entry changes do not cause rehashing. Generation already covers VIF MPG and Store8..Store128.
    const uint64_t generation=memory->getVU1CodeGeneration();
    AotCache& c=aotCache;
    if(!c.valid || c.owner!=this || c.memory!=memory || c.code!=code || c.generation!=generation){
        c.owner=this;c.memory=memory;c.code=code;c.generation=generation;
        c.image=ps2Vu1AotIdentify(code,codeSize,UINT32_MAX);c.valid=true;c.entries.fill(-2);
        if(c.image<0)++aotStats.unknownImages;
        c.fnv=aotCapture().on?aotFnv64(code,16384):0;
    }
    // Entry lookup only: passing nullptr must never hash or dereference code.
    // TEMP(transformers): compatible images can expose different entry sets.
    int entryImage=-1;
    if(pc<16384 && !(pc&7u)) {
        int& cached=c.entries[pc/8];
        if(cached==-2)cached=c.image>=0 && ps2Vu1AotIdentify(nullptr,uint32_t(c.image),pc)==c.image
            ?c.image:ps2Vu1AotIdentify(code,codeSize,pc);
        entryImage=cached;
    }
    // TEMP(transformers): coverage and compiled-footprint entry identity retain priority.
    const bool wholeReady=entryImage>=0;
    const bool staticReady=staticOn && !wholeReady && ps2Vu1StaticPrepare(code,codeSize,memory,generation,pc);
    bool declined=false;
    if(!staticReady && entryImage<0 && c.image<0){
        const uint64_t n=++aotStats.declinedImage;declined=true;
        if(aotCapture().on){++aotCapture().unknownCalls[c.fnv];if((n&(n-1))==0&&n>=256)aotPrintUnknown();}
    }
    else if(!staticReady && entryImage<0){++aotStats.declinedEntry;declined=true;}
    if(declined){
        ps2_perfscope::noteVu1Declined(); // TEMP(transformers): counts on EE and VU worker
        AotCapture& cap=aotCapture();
        if(!cap.on||cap.written>=cap.max)return false;
        const uint64_t key=c.fnv^(uint64_t(pc)<<1)^(continuing?1u:0u);
        uint32_t& seen=cap.keys[key];
        if(seen>=cap.per)return false;
        const uint32_t nth=seen++;
        const uint32_t seq=cap.written++;
        const unsigned long long vs=(unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed);
        char name[160];
        std::snprintf(name,sizeof(name),"/aotcap_%05u_v%06llu_%c_%s_%016llx_%04x_",seq,vs,continuing?'c':'m',c.image<0?"img":"ent",
                      (unsigned long long)c.fnv,pc);
        const std::string base=cap.dir+name;
        const uint32_t word0=continuing?(0x80000000u|pc):entry;
        aotWriteDump(base+"pre.bin",word0,top,itop,code,data,m_state);
        if(nth==0)
            std::fprintf(stderr,"[vu1aot:cap] new key image=%016llx known=%d entry=0x%x %s vs=%llu seq=%u\n",(unsigned long long)c.fnv,c.image,pc,
                         continuing?"MSCNT":"MSCAL",vs,seq);
        aotBypass=true;
        if(continuing)resume(code,codeSize,data,dataSize,gs,memory,top,itop,budget);
        else execute(code,codeSize,data,dataSize,gs,memory,entry,top,itop,budget);
        aotBypass=false;
        aotWriteDump(base+"post.bin",word0,top,itop,code,data,m_state);
        return true;
    }
    const int image=staticReady?0:entryImage;
    // TEMP(transformers): PS2X_VU1_AOT_MASK=<hex> - bit i enables image i (default all); per-image disable/bisection.
    // TEMP(transformers) qualification (release fix): 64-bit image mask - the coverage-round-2 package has 38 images (was image>=32 -> declined).
    static const uint64_t imageMask=[] {const char* p=std::getenv("PS2X_VU1_AOT_MASK");return p&&*p?uint64_t(std::strtoull(p,nullptr,16)):~uint64_t(0);}();
    if(image>=64 || !((imageMask>>image)&1u)){++aotStats.declinedMask;return false;}
    m_state.cycles=m_cycle;
    static thread_local std::array<uint8_t,80000> transfer;
    // TEMP(transformers) qualification (release fix): mid-call fallback probe - pre-state snapshot + call timing (PS2X_VU1AOT_MIDPROBE only).
    MidProbe& probe=midProbe();
    static thread_local VU1State probePre;
    static thread_local std::array<uint8_t,16384> probePreData;
    std::chrono::steady_clock::time_point probeT0;
    if(probe.on){probePre=m_state;std::memcpy(probePreData.data(),data,16384);probeT0=std::chrono::steady_clock::now();}
    const bool needsTransfer=(staticReady?ps2Vu1StaticRun:ps2Vu1AotRun)(image,&m_state,continuing,code,data,gs,memory,entry,top,itop,budget,transfer.data());
    bool probeFell=false;
    { // TEMP(transformers) qualification (release fix): the package counter moved during this call = mid-call fallback.
        static thread_local uint64_t lastFallback=0;
        const uint64_t fb=ps2Vu1AotFallbackCalls()+ps2Vu1StaticFallbackCalls();
        if(fb!=lastFallback && fb!=UINT64_MAX){lastFallback=fb;++aotStats.midFallback;ps2_perfscope::noteVu1MidFallback();probeFell=true;}
    }
    if(probe.on)
    { // TEMP(transformers) qualification (release fix)
        const uint64_t callNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-probeT0).count());
        const uint64_t vs=ps2dbg::vsyncTick.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(probe.mu);
        ++probe.calls;probe.callNs+=callNs;
        if(probeFell)
        {
            uint32_t fbPc=0,why=0;uint64_t interpNs=0;ps2Vu1AotProbeLast(&fbPc,&why,&interpNs);
            ++probe.fbCalls;probe.fbCallNs+=callNs;probe.fbInterpNs+=interpNs;
            ++probe.perBucket[std::min<size_t>(size_t(vs/300),probe.perBucket.size()-1)];
            MidProbe::Row& r=probe.rows[std::make_tuple(image,pc,continuing?1:0,fbPc,why)];
            const bool newRow=r.n==0;
            if(newRow)r.firstVs=vs;
            ++r.n;r.interpNs+=interpNs;r.callNs+=callNs;r.lastVs=vs;
            if(r.dumps<probe.per && probe.written<probe.max)
            {
                ++r.dumps;const uint32_t seq=probe.written++;
                char name[200];
                std::snprintf(name,sizeof(name),"/midcap_%05u_v%06llu_i%02d_%c%04x_fb%04x_w%u_",seq,(unsigned long long)vs,image,continuing?'c':'m',pc,
                              fbPc==UINT32_MAX?0xffffu:fbPc,why);
                const std::string base=probe.dir+name;
                const uint32_t word0=continuing?(0x80000000u|pc):entry;
                aotWriteDump(base+"pre.bin",word0,top,itop,code,probePreData.data(),probePre);
                aotWriteDump(base+"post.bin",word0,top,itop,code,data,m_state);
            }
            if(newRow)
            {
                std::fprintf(stderr,"[vu1aot:mid] new row image=%d pc=0x%x %s fbpc=0x%x why=%u vs=%llu\n",image,pc,continuing?"MSCNT":"MSCAL",fbPc,why,
                             (unsigned long long)vs);
                probe.writeFile();
            }
        }
        if((probe.calls&0xFFFFu)==0)probe.writeFile();
        if((probe.calls&0x3FFFFu)==0){probe.table(stderr,24);std::fflush(stderr);}
    }
    // Only complete-program boundaries are admitted. The generated backend handles cold/unknown
    // control flow with its copied batch-3 interpreter and preserves the final observable state.
    m_cycle=m_state.cycles;
    resetScheduler();
    if(needsTransfer){size_t offset=0;
std::memcpy(&m_flagPipeline,transfer.data()+offset,sizeof(m_flagPipeline));offset+=sizeof(m_flagPipeline);
std::memcpy(&m_fdiv,transfer.data()+offset,sizeof(m_fdiv));offset+=sizeof(m_fdiv);
std::memcpy(&m_efu,transfer.data()+offset,sizeof(m_efu));offset+=sizeof(m_efu);
std::memcpy(&m_storePipeline,transfer.data()+offset,sizeof(m_storePipeline));offset+=sizeof(m_storePipeline);
std::memcpy(&m_vfWritePipeline,transfer.data()+offset,sizeof(m_vfWritePipeline));offset+=sizeof(m_vfWritePipeline);
std::memcpy(&m_viWritePipeline,transfer.data()+offset,sizeof(m_viWritePipeline));offset+=sizeof(m_viWritePipeline);
std::memcpy(&m_accWritePipeline,transfer.data()+offset,sizeof(m_accWritePipeline));offset+=sizeof(m_accWritePipeline);
std::memcpy(&m_commitCalendar,transfer.data()+offset,sizeof(m_commitCalendar));offset+=sizeof(m_commitCalendar);
std::memcpy(&m_commitCycles,transfer.data()+offset,sizeof(m_commitCycles));offset+=sizeof(m_commitCycles);
std::memcpy(&m_xgkick,transfer.data()+offset,sizeof(m_xgkick));offset+=sizeof(m_xgkick);
std::memcpy(&m_vfReady,transfer.data()+offset,sizeof(m_vfReady));offset+=sizeof(m_vfReady);
std::memcpy(&m_viReady,transfer.data()+offset,sizeof(m_viReady));offset+=sizeof(m_viReady);
std::memcpy(&m_accReady,transfer.data()+offset,sizeof(m_accReady));offset+=sizeof(m_accReady);
std::memcpy(&m_vfLatestWrite,transfer.data()+offset,sizeof(m_vfLatestWrite));offset+=sizeof(m_vfLatestWrite);
std::memcpy(&m_viLatestWrite,transfer.data()+offset,sizeof(m_viLatestWrite));offset+=sizeof(m_viLatestWrite);
std::memcpy(&m_accLatestWrite,transfer.data()+offset,sizeof(m_accLatestWrite));offset+=sizeof(m_accLatestWrite);
std::memcpy(&m_cycle,transfer.data()+offset,sizeof(m_cycle));offset+=sizeof(m_cycle);
std::memcpy(&m_nextWriteSequence,transfer.data()+offset,sizeof(m_nextWriteSequence));offset+=sizeof(m_nextWriteSequence);
std::memcpy(&m_efuResourceReady,transfer.data()+offset,sizeof(m_efuResourceReady));offset+=sizeof(m_efuResourceReady);
std::memcpy(&m_workingClip,transfer.data()+offset,sizeof(m_workingClip));offset+=sizeof(m_workingClip);
std::memcpy(&m_currentUpperInstruction,transfer.data()+offset,sizeof(m_currentUpperInstruction));offset+=sizeof(m_currentUpperInstruction);
std::memcpy(&m_viBranchBackupValue,transfer.data()+offset,sizeof(m_viBranchBackupValue));offset+=sizeof(m_viBranchBackupValue);
std::memcpy(&m_viBranchBackupReg,transfer.data()+offset,sizeof(m_viBranchBackupReg));offset+=sizeof(m_viBranchBackupReg);
std::memcpy(&m_viBranchBackupValid,transfer.data()+offset,sizeof(m_viBranchBackupValid));offset+=sizeof(m_viBranchBackupValid);
std::memcpy(&m_stopRequested,transfer.data()+offset,sizeof(m_stopRequested));offset+=sizeof(m_stopRequested);
std::memcpy(&m_pendingHaltD,transfer.data()+offset,sizeof(m_pendingHaltD));offset+=sizeof(m_pendingHaltD);
std::memcpy(&m_pendingHaltT,transfer.data()+offset,sizeof(m_pendingHaltT));offset+=sizeof(m_pendingHaltT);
std::memcpy(&m_fast,transfer.data()+offset,sizeof(m_fast));offset+=sizeof(m_fast);
std::memcpy(&m_fastMaxWriteReady,transfer.data()+offset,sizeof(m_fastMaxWriteReady));offset+=sizeof(m_fastMaxWriteReady);
std::memcpy(&m_programPending,transfer.data()+offset,sizeof(m_programPending));offset+=sizeof(m_programPending);
std::memcpy(&m_quietBudget,transfer.data()+offset,sizeof(m_quietBudget));offset+=sizeof(m_quietBudget);
std::memcpy(&m_fastVfLog,transfer.data()+offset,sizeof(m_fastVfLog));offset+=sizeof(m_fastVfLog);
std::memcpy(&m_fastVfLogCount,transfer.data()+offset,sizeof(m_fastVfLogCount));offset+=sizeof(m_fastVfLogCount);
std::memcpy(&m_fastViLog,transfer.data()+offset,sizeof(m_fastViLog));offset+=sizeof(m_fastViLog);
std::memcpy(&m_fastViLogCount,transfer.data()+offset,sizeof(m_fastViLogCount));offset+=sizeof(m_fastViLogCount);
std::memcpy(&m_fastVisVf,transfer.data()+offset,sizeof(m_fastVisVf));offset+=sizeof(m_fastVisVf);
std::memcpy(&m_fastIssueVf,transfer.data()+offset,sizeof(m_fastIssueVf));offset+=sizeof(m_fastIssueVf);
std::memcpy(&m_fastHasVf,transfer.data()+offset,sizeof(m_fastHasVf));offset+=sizeof(m_fastHasVf);
std::memcpy(&m_fastVisVi,transfer.data()+offset,sizeof(m_fastVisVi));offset+=sizeof(m_fastVisVi);
std::memcpy(&m_fastReadyVi,transfer.data()+offset,sizeof(m_fastReadyVi));offset+=sizeof(m_fastReadyVi);
std::memcpy(&m_fastHasVi,transfer.data()+offset,sizeof(m_fastHasVi));offset+=sizeof(m_fastHasVi);
}
    const uint64_t n=++aotStats.accepted;
    if(size_t(image)<aotStats.perImage.size())++aotStats.perImage[size_t(image)];
    if(needsTransfer)++aotStats.transfers;
    // TEMP(transformers): also every 2^16 accepted calls (with the guest vsync) so per-segment per-image call rates can be read.
    if(((n&(n-1))==0 && n>=1024) || (n&0xFFFFu)==0)aotStats.print("progress");
    return true;
}
