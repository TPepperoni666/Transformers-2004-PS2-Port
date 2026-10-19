#pragma once
// TEMP(transformers): GSREC v1, little-endian MSVC x64 ABI, same source/build only.
#include "runtime/gs/gs_types.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfenv>
#include <string>
#include <vector>
#include <iostream>
#include <xmmintrin.h>
#include <type_traits>
#include <atomic>
enum class GSRecOp : uint32_t { Initial=1, State, Submit, Clut, Transfer, Upload, Flush, TextureFlush, Sync, Present, Clear, Consume, Read, Write, Snapshot, TransferSnapshot, Reset, Initialize, Latch, Vsync, Final, Begin };
struct GSRecClut { GSTex0Reg tex0; GSTexClutReg texclut; };
struct GSRecClear { GSContext context; uint32_t rgba; };
struct GSRecPixel { uint32_t psm,base,bw,x,y,value; };
struct GSRecHeader {
    char magic[8]={'G','S','R','E','C','0','1',0};
    uint32_t batchSize=sizeof(GSPrimitiveBatch), requestSize=sizeof(GSPresentationRequest), pointerSize=sizeof(size_t), settings=0;
    char sourceId[65]="ad335ed40035d35853dd4ec942ccd58ff88e9e46783eb6fff7c7ee72e4292a2b";
};
struct GSRecEvent { uint32_t op, size, mxcsr; int rounding; std::fenv_t environment{}; };
class GSRecorder {
    FILE *file=nullptr;
    std::atomic<bool> enabled{false};
    bool done=false;
    uint64_t start=0,count=0,seen=0,bytes=0,ops=0;
    std::string prefix="gsrec";
    std::vector<char> buffer;
    bool Raw(const void *p,size_t n) {
        // Leave space for final VRAM and its event even on cap exhaustion.
        if (bytes+n > 2ull*1024*1024*1024-8ull*1024*1024 || std::fwrite(p,1,n,file)!=n) { Fail("size cap (~2 GiB) or write failure; incomplete recording"); return false; }
        bytes+=n; return true;
    }
public:
    explicit GSRecorder(const char *env=nullptr) {
        if (!env) return;
        // Diagnostic texture dumping performs extra cache reads: it is not a backend command.
        if (std::getenv("PS2X_GSCAPTURE") || std::getenv("PS2X_GSCAPTURE_VS") || std::getenv("PS2X_TEX_DEBUG_DUMPS")) {
            std::cerr<<"[gs:record] disable GSCAPTURE/GSCAPTURE_VS/TEX_DEBUG_DUMPS for replayable capture\n";return;
        }
        char *end=nullptr; start=std::strtoull(env,&end,10);
        if (end==env || *end!=':') { std::cerr<<"[gs:record] invalid start:count[:prefix]\n";return; }
        const char *p=end+1; count=std::strtoull(p,&end,10);
        if (end==p || !count || (*end && *end!=':')) { std::cerr<<"[gs:record] invalid count\n";return; }
        if (*end==':' && end[1]) prefix=end+1;
        enabled=true;
    }
    ~GSRecorder() { if(file) { std::cerr<<"[gs:record] incomplete (no final snapshot)\n";std::fclose(file); } }
    bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
    uint64_t Count() const { return ops; }
    bool Active() const { return file!=nullptr; }
    bool Done() const { return done; }
    uint64_t Start() const { return start; }
    void Fail(const char *why) { std::cerr<<"[gs:record] "<<why<<" bytes="<<bytes<<"\n";if(file)std::fclose(file);file=nullptr;done=true;enabled=false; }
    void Event(GSRecOp op,const void *p=nullptr,uint32_t n=0) {
        if (!file) return;
        GSRecEvent e{uint32_t(op),n,_mm_getcsr(),std::fegetround()};
        std::fegetenv(&e.environment);
        if (Raw(&e,sizeof(e))) { if (!n || Raw(p,n)) ++ops; }
    }
    template<class T> void Pod(GSRecOp op,const T &v) { static_assert(std::is_trivially_copyable_v<T>);Event(op,&v,sizeof(v)); }
    void Begin(const GSPresentationRequest &request,const std::vector<uint8_t> &state,const std::vector<uint8_t> &vram) {
        const std::string name=prefix+"_vs"+std::to_string(request.vsyncTick)+".gsrec";
        // Exclusive creation avoids overwriting a prior run.
        file=std::fopen(name.c_str(),"wbx");
        if(!file) { Fail("cannot exclusively create recording");return; }
        buffer.resize(1024*1024);std::setvbuf(file,buffer.data(),_IOFBF,buffer.size());
        GSRecHeader h;
        const char *names[]={"PS2X_GS_COLCLAMP_WRAP","PS2X_GS_DITHER","PS2X_GS_ZCLAMP","PS2X_GS_DISABLE_EARLY_DEPTH"};
        for(unsigned i=0;i<4;i++) { const char *v=std::getenv(names[i]);if(i==3?(v && v[0] && v[0]!='0'):(v && v[0]=='0'))h.settings|=1u<<i; }
        Raw(&h,sizeof(h));
        Event(GSRecOp::Initial,vram.data(),uint32_t(vram.size()));
        Event(GSRecOp::State,state.data(),uint32_t(state.size()));
        Pod(GSRecOp::Begin,request); // initial effective display-register state, no backend call.
        std::cerr<<"[gs:record] begin "<<name<<" count="<<count<<" cap=2GiB\n";
    }
    bool Boundary() { return ++seen>=count; }
    void Finish(const std::vector<uint8_t> &vram) {
        Event(GSRecOp::Final,vram.data(),uint32_t(vram.size()));
        if (!file) return;
        if (std::fclose(file)) { file=nullptr;Fail("close failure");return; }
        file=nullptr;done=true;enabled=false;
        std::cerr<<"[gs:record] complete bytes="<<bytes<<" vsyncs="<<seen<<" bytes/vsync="<<(bytes-8ull*1024*1024)/seen<<"\n";
    }
};
