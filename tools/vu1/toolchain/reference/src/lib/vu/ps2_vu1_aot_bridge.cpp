// Transformers port: optional local-only VU1 AOT dispatch (local VU1 tooling round 3, private research evidence).
// GAME(transformers): the generated images (src/vu1_aot_private, gitignored) are derived from the game - never commit/publish them.
// Build: -DPS2X_WITH_VU1_AOT=ON (default OFF). Runtime: PS2X_VU1_AOT=1 (default off). Notes: port/MORNING_A_aot.md
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include "../debug_counters.h"

// Implemented in the private, generated object library.
int ps2Vu1AotIdentify(const uint8_t* code,uint32_t size,uint32_t entry);
bool ps2Vu1AotRun(int image,void* state,bool resume,uint8_t* code,uint8_t* data,
                 GS& gs,PS2Memory* memory,uint32_t entry,uint32_t top,uint32_t itop,uint32_t budget,void* transfer);

// Transformers port: identity memo for the single VU1 instance (kept here, not in ps2_vu1.h, so the class
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
    bool valid=false;
    uint64_t fnv=0; // Transformers port: capture key, only computed with PS2X_VU1AOT_CAPTURE
};
thread_local AotCache aotCache;
// Transformers port: call accounting, printed at power-of-two acceptance counts and at exit.
struct AotStats
{
    std::atomic<uint64_t> accepted{0},transfers{0},declinedMode{0},declinedImage{0},declinedEntry{0},declinedMask{0},unknownImages{0};
    std::array<std::atomic<uint64_t>,64> perImage{}; // Transformers port: 23+ images after the lane-aot census
    void print(const char* why)
    {
        std::fprintf(stderr,"[vu1aot] %s vs=%llu accepted=%llu transfers=%llu declined: mode=%llu image=%llu entry=%llu mask=%llu unknownImages=%llu perImage:",why,
            (unsigned long long)ps2dbg::vsyncTick.load(std::memory_order_relaxed),
            (unsigned long long)accepted.load(),(unsigned long long)transfers.load(),(unsigned long long)declinedMode.load(),
            (unsigned long long)declinedImage.load(),(unsigned long long)declinedEntry.load(),(unsigned long long)declinedMask.load(),
            (unsigned long long)unknownImages.load());
        for(size_t i=0;i<perImage.size();++i)
            if(const uint64_t n=perImage[i].load())std::fprintf(stderr," %zu=%llu",i,(unsigned long long)n);
        std::fprintf(stderr,"\n");
        std::fflush(stderr);
    }
    ~AotStats(){if(accepted.load()||declinedMode.load()||declinedImage.load()||declinedEntry.load())print("exit");}
};
AotStats aotStats;

// Transformers port: PS2X_VU1AOT_CAPTURE=1 (or a directory; 1 = PS2X_DUMP_DIR) - for calls the AOT declines on an
// unknown image or unknown entry, write vu1dump-format files (header {word0 (MSCNT: 0x80000000|pc), top, itop,
// sizeof(VU1State)} + 16K micro memory + 16K data memory + VU1State) before and after the call, for the first
// PS2X_VU1AOT_CAPTURE_PER (default 6) calls of every (image, entry, MSCAL/MSCNT) key and at most PS2X_VU1AOT_CAPTURE_MAX
// (default 6000) calls overall -> local research sources census input. The captured call runs through the ordinary
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
// Transformers port: PS2X_VU1AOT_HASH=1 - in-game exactness check. Every VU1 call (AOT or interpreter, with or without
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
void aotPrintUnknown()
{
    AotCapture& c=aotCapture();
    std::fprintf(stderr,"[vu1aot:cap] unknownImages distinct=%zu captured=%u keys=%zu:",c.unknownCalls.size(),c.written,c.keys.size());
    for(const auto& kv:c.unknownCalls)std::fprintf(stderr," %016llx=%llu",(unsigned long long)kv.first,(unsigned long long)kv.second);
    std::fprintf(stderr,"\n");
    std::fflush(stderr);
}
}

bool VU1Interpreter::tryAot(uint8_t* code,uint32_t codeSize,uint8_t* data,uint32_t dataSize,
                           GS& gs,PS2Memory* memory,uint32_t entry,uint32_t top,uint32_t itop,
                           uint32_t budget,bool continuing)
{
    static const bool enabled=[] {const char* p=std::getenv("PS2X_VU1_AOT");return p&&*p&&std::strcmp(p,"0")!=0;}();
    static const bool runToEnd=[] {const char* p=std::getenv("PS2X_VU1_RUN_TO_END");return !p||std::strcmp(p,"0")!=0;}();
    if(aotHashOn() && !aotHashing && m_unit==Unit::VU1 && dataSize==16384) // Transformers port: PS2X_VU1AOT_HASH wrapper
    {
        aotHashing=true;
        if(continuing)resume(code,codeSize,data,dataSize,gs,memory,top,itop,budget);
        else execute(code,codeSize,data,dataSize,gs,memory,entry,top,itop,budget);
        aotHashing=false;
        aotHashFold(m_state,data);
        return true;
    }
    if(aotBypass)return false; // Transformers port: re-entered from a capture below - run the interpreter
    if(!enabled || !runToEnd || budget<=64 || m_unit!=Unit::VU1 || !memory ||
       code!=memory->getVU1Code() || codeSize!=16384 || dataSize!=16384 || m_programPending ||
       pipelinesPending() || m_cycle<m_fastMaxWriteReady){if(enabled&&m_unit==Unit::VU1)++aotStats.declinedMode;return false;}
    const uint32_t pc=continuing?m_state.pc:entry;
    // Memoize identity, including negative results. Full equality disambiguates hash collisions.
    // Entry changes do not cause rehashing. Generation already covers VIF MPG and Store8..Store128.
    const uint64_t generation=memory->getVU1CodeGeneration();
    AotCache& c=aotCache;
    if(!c.valid || c.owner!=this || c.memory!=memory || c.code!=code || c.generation!=generation){
        c.owner=this;c.memory=memory;c.code=code;c.generation=generation;
        c.image=ps2Vu1AotIdentify(code,codeSize,UINT32_MAX);c.valid=true;
        if(c.image<0)++aotStats.unknownImages;
        c.fnv=aotCapture().on?aotFnv64(code,16384):0;
    }
    // Entry lookup only: passing nullptr must never hash or dereference code.
    bool declined=false;
    if(c.image<0){
        const uint64_t n=++aotStats.declinedImage;declined=true;
        if(aotCapture().on){++aotCapture().unknownCalls[c.fnv];if((n&(n-1))==0&&n>=256)aotPrintUnknown();}
    }
    else if(ps2Vu1AotIdentify(nullptr,uint32_t(c.image),pc)!=c.image){++aotStats.declinedEntry;declined=true;}
    if(declined){
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
    const int image=c.image;
    // Transformers port: PS2X_VU1_AOT_MASK=<hex> - bit i enables image i (default all); per-image disable/bisection.
    static const uint32_t imageMask=[] {const char* p=std::getenv("PS2X_VU1_AOT_MASK");return p&&*p?uint32_t(std::strtoul(p,nullptr,16)):0xFFFFFFFFu;}();
    if(image>=32 || !((imageMask>>image)&1u)){++aotStats.declinedMask;return false;}
    m_state.cycles=m_cycle;
    static thread_local std::array<uint8_t,80000> transfer;
    const bool needsTransfer=ps2Vu1AotRun(image,&m_state,continuing,code,data,gs,memory,entry,top,itop,budget,transfer.data());
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
    // Transformers port: also every 2^16 accepted calls (with the guest vsync) so per-segment per-image call rates can be read.
    if(((n&(n-1))==0 && n>=1024) || (n&0xFFFFu)==0)aotStats.print("progress");
    return true;
}
