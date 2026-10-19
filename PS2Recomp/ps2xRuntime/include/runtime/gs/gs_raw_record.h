#pragma once
// TEMP(transformers): GSRAW v1 portable stream framing + explicitly sized private CPU resume state.
#include <array>
#include <atomic>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <xmmintrin.h>
namespace gsraw {
inline thread_local uint32_t path = 3;
struct PathScope { uint32_t previous; explicit PathScope(uint32_t p):previous(path){path=p;} ~PathScope(){path=previous;} };
// Single runtime GS. MMIO/HLE hooks pass register-owner identity; unrelated instances are ignored.
inline std::function<void(const void*,uint32_t,uint64_t,uint32_t)> privHook;
inline void PrivWrite(const void* owner,uint32_t address,uint64_t value,uint32_t width=8) {
    if (privHook) privHook(owner,address,value,width);
}
struct Header {
    char magic[8]={'G','S','R','A','W','0','1',0};
    uint32_t version=1, headerBytes=128, vramBytes=4194304, privBytes=8192;
    uint32_t adCount=0, frontBytes=0, cpuBytes=0, portableBytes=0, settings=0, markerPeriod=256;
    uint64_t startTick=0, decodedStart=0;
    char sourceId[64]{};
};
static_assert(sizeof(Header)==128);
struct AD { uint64_t reg, value; };
enum Op : uint32_t { Gif=1, Priv=2, Vsync=3, Readback=4, Marker=5, Final=6, DirectAD=7, Reset=8, Pixel=9, Clear=10 };
struct Event {
    uint32_t op=0, size=0;
    uint64_t sequence=0, decoded=0, tick=0;
    uint32_t mxcsr=0; int32_t rounding=0;
    std::fenv_t environment{};
};
static_assert(sizeof(Event)==48 && sizeof(std::fenv_t)==8);
struct PrivPayload { uint32_t address,width; uint64_t value; };
struct VsyncPayload { uint64_t csr; };
struct ReadbackPayload { uint32_t requested, returned; };
inline uint32_t Settings() {
    uint32_t bits=0; const char *names[]={"PS2X_GS_COLCLAMP_WRAP","PS2X_GS_DITHER","PS2X_GS_ZCLAMP","PS2X_GS_DISABLE_EARLY_DEPTH"};
    for(unsigned i=0;i<4;i++){const char*v=std::getenv(names[i]);if(i==3?(v&&v[0]&&v[0]!='0'):(v&&v[0]=='0'))bits|=1u<<i;}return bits;
}
class Recorder {
    FILE* file=nullptr; bool enabled=false,done=false;
    uint64_t start=0,count=0,seen=0,bytes=0,sequence=0;
    std::string prefix; std::vector<char> buffer;
    bool Write(const void*p,size_t n) {
        if (!file) return false;
        if(n>2147483648ull-8*1024*1024 || bytes>2147483648ull-8*1024*1024-n || (n && std::fwrite(p,1,n,file)!=n)) { Fail("2 GiB cap/write failure; incomplete");return false; }
        bytes+=n;return true;
    }
public:
    Recorder() {
        const char*env=std::getenv("PS2X_GS_RAWREC");if(!env)return;
        char*e=nullptr;start=std::strtoull(env,&e,10);if(e==env||*e!=':'){Fail("invalid start:count:prefix");return;}
        const char*p=e+1;count=std::strtoull(p,&e,10);if(e==p||!count||*e!=':'||!e[1]){Fail("invalid count/prefix");return;}prefix=e+1;
        if(std::getenv("PS2X_GSCAPTURE")||std::getenv("PS2X_GSCAPTURE_VS")||std::getenv("PS2X_TEX_DEBUG_DUMPS")){Fail("unset texture capture diagnostics");return;}
        const char*lag=std::getenv("PS2X_STOREIMAGE_LAG");if(lag&&lag[0]=='1'){Fail("capture requires PS2X_STOREIMAGE_LAG=0 (lag callbacks are not raw GIF)");return;}
        enabled=true;
    }
    ~Recorder(){if(file){std::cerr<<"[gs:rawrec] incomplete: final snapshot absent\n";std::fclose(file);}}
    bool Enabled()const{return enabled;} bool Active()const{return file!=nullptr;} bool Done()const{return done;} uint64_t Start()const{return start;}
    void Fail(const char*why){std::cerr<<"[gs:rawrec] "<<why<<" bytes="<<bytes<<"\n";if(file)std::fclose(file);file=nullptr;done=true;enabled=false;}
    void EventRecord(uint32_t op,const void*p,uint32_t n,uint64_t tick,uint64_t decoded) {
        if(!file)return;gsraw::Event e;e.op=op;e.size=n;e.sequence=++sequence;e.decoded=decoded;e.tick=tick;e.mxcsr=_mm_getcsr();e.rounding=std::fegetround();std::fegetenv(&e.environment);
        if(Write(&e,sizeof(e)))Write(p,n);
        if(file && op!=Marker && op!=Final && sequence%256==0) EventRecord(Marker,nullptr,0,tick,decoded);
    }
    template<class T> void Pod(uint32_t op,const T&v,uint64_t tick,uint64_t decoded){EventRecord(op,&v,sizeof(v),tick,decoded);}
    void Begin(Header h,const std::vector<AD>&ad,const std::array<uint8_t,8192>&priv,const std::vector<uint8_t>&portable,const std::vector<uint8_t>&front,const std::vector<uint8_t>&cpu,const std::vector<uint8_t>&vram) {
        if(vram.size()!=4194304){Fail("GS VRAM must be 4 MiB");return;}
        auto name=prefix+"_vs"+std::to_string(h.startTick)+".gsraw";file=std::fopen(name.c_str(),"wbx");if(!file){Fail("exclusive create failed");return;}
        buffer.resize(1024*1024);std::setvbuf(file,buffer.data(),_IOFBF,buffer.size());
        h.adCount=uint32_t(ad.size());h.frontBytes=uint32_t(front.size());h.cpuBytes=uint32_t(cpu.size());h.portableBytes=uint32_t(portable.size());h.settings=Settings();
        Write(&h,sizeof(h));Write(ad.data(),ad.size()*sizeof(AD));Write(priv.data(),priv.size());Write(portable.data(),portable.size());Write(front.data(),front.size());Write(cpu.data(),cpu.size());Write(vram.data(),vram.size());
        if(file){std::cerr<<"[gs:rawrec] begin "<<name<<" intervals="<<count<<"\n";EventRecord(Marker,nullptr,0,h.startTick,h.decodedStart);}
    }
    bool Boundary(){return ++seen>=count;}
    void Finish(const std::vector<uint8_t>&vram,uint64_t tick,uint64_t decoded){
        EventRecord(Marker,nullptr,0,tick,decoded);EventRecord(Final,vram.data(),uint32_t(vram.size()),tick,decoded);if(!file)return;
        int rc=std::fclose(file);file=nullptr;if(rc){Fail("close failure");return;}done=true;enabled=false;
        std::cerr<<"[gs:rawrec] complete bytes="<<bytes<<" intervals="<<seen<<" stream+state bytes/vsync="<<(bytes-8388608)/seen<<"\n";
    }
};
} // namespace gsraw
