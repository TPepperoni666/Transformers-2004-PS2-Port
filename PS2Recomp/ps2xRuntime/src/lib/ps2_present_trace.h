// TEMP(transformers): default-off bounded asynchronous host presentation diagnostics.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <array>
#include <string>
namespace ps2PresentTrace {
enum Stage : uint16_t { Origin=1, HostFrame, Upload, Capture, Draw, Ui, EndDrawing, Pad,
    ScanoutFlush, Scanout, ScanoutSubmit, CopySubmit, FenceWait, FencePoll, Map, Copy,
    GuestReadback, FieldCapture, Ingest, Select, Display, Brightness, Status, NullScanout, TextureUpload, HiresCapture, ImagePoll, Retire, Swap, EventPoll, FrameWait, Pacing, GpuScanout, GpuCircuit, GpuShade, GpuReadback, Overhead, GpuStatus };
// File: 8-byte magic PTRC0001, then fixed 72-byte little-endian records (Windows x64).
struct Record { uint64_t us,id,tick,seq,a,b,c,d; uint32_t thread; uint16_t stage,type; };
static_assert(sizeof(Record)==72);
inline uint64_t clockUs() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline uint64_t clockNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline bool enabled() { static const bool on=[] { const char* e=std::getenv("PS2X_PRESENT_TRACE");
    return e && std::strcmp(e,"1")==0; }(); return on; }
struct Writer {
    static constexpr size_t Capacity=8192;
    static constexpr uint64_t Limit=1024ull*1024*1024;
    std::array<Record,Capacity> ring{};
    size_t head=0,tail=0;
    std::mutex mutex;
    std::atomic<uint64_t> lost{0}, capped{0}, origin{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> available{false};
    std::thread worker;
    FILE* file=nullptr;
    uint64_t limit;
    uint64_t activeNs=0;
    explicit Writer(uint64_t byteLimit=Limit):limit(byteLimit) {
        const char* field=std::getenv("PS2X_FIELD60_LOG");
        if(!field || !*field) return; // never silently write elsewhere
        std::string path(field); auto sep=path.find_last_of("/\\");
        path=(sep==std::string::npos?std::string():path.substr(0,sep+1))+"present_trace.bin";
        file=std::fopen(path.c_str(),"wb"); if(!file) return;
        std::fwrite("PTRC0001",1,8,file);
        available=true;
        worker=std::thread([this]{ run(); });
    }
    void push(Record r) {
        if(!available.load(std::memory_order_relaxed)) { ++capped; return; }
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
        if(!lock || head-tail==Capacity) { ++lost; return; }
        ring[head++%Capacity]=r;
    }
    void run() {
        std::array<Record,Capacity> batch;
        uint64_t bytes=8,lastStatus=0; bool wroteOrigin=false;
        do {
            if(!wroteOrigin && origin.load()) {
                Record r{};r.us=clockUs();r.stage=Origin;r.a=origin.load();
                std::fwrite(&r,sizeof(r),1,file);bytes+=sizeof(r);wroteOrigin=true;
            }
            const auto now=clockUs();
            const auto activeStart=clockNs();
            if(now-lastStatus>=1000000 && bytes+3*sizeof(Record)<=limit) {
                Record r{};r.us=now;r.stage=Status;r.a=lost;r.b=capped;r.c=activeNs;r.d=bytes;
                std::fwrite(&r,sizeof(r),1,file);bytes+=sizeof(r);lastStatus=now;
            }
            size_t n=0;
            { std::lock_guard<std::mutex> lock(mutex);
              while(tail!=head) batch[n++]=ring[tail++%Capacity]; }
            if(n && bytes+n*sizeof(Record)+2*sizeof(Record)<=limit) {
                if(std::fwrite(batch.data(),sizeof(Record),n,file)!=n) { available=false; break; }
                bytes+=n*sizeof(Record);
            } else if(n) { capped+=n; available=false; }
            // Flush only on writer thread; begin records survive a later blocked host call.
            std::fflush(file);
            activeNs+=clockNs()-activeStart;
            if(!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while(!stop.load());
        // Shutdown occurs after runtime producer threads have joined; drain the last batch.
        { std::lock_guard<std::mutex> lock(mutex);
          while(tail!=head) { const auto& r=ring[tail++%Capacity];
            if(bytes+3*sizeof(Record)<=limit) { std::fwrite(&r,sizeof(r),1,file);bytes+=sizeof(r); } else ++capped; } }
        if(!wroteOrigin && origin.load() && bytes+2*sizeof(Record)<=limit) {
            Record r{};r.us=clockUs();r.stage=Origin;r.a=origin.load();
            std::fwrite(&r,sizeof(r),1,file);bytes+=sizeof(r);
        }
        Record status{}; status.us=clockUs();status.stage=Status;status.type=3;status.a=lost;status.b=capped;status.c=activeNs;status.d=bytes;
        std::fwrite(&status,sizeof(status),1,file);std::fclose(file);
    }
    ~Writer() { stop=true; if(worker.joinable()) worker.join(); else if(file) std::fclose(file); }
};
inline Writer& writer() { static Writer w; return w; }
inline uint32_t threadId() { static std::atomic<uint32_t> next{0}; thread_local const uint32_t id=++next;return id; }
inline uint64_t nextId() { thread_local uint64_t id=0;return ++id; }
inline void event(Stage stage,uint64_t tick=0,uint64_t seq=0,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,
                  uint16_t type=0,uint64_t id=0) {
    if(!enabled())return;
    const auto overheadStart=clockNs();
    if(stage==Origin) {
        auto &w=writer();w.origin=a;
        w.push({clockUs(),0,0,0,clockNs()-overheadStart,1,2,0,threadId(),Overhead,0});
        return;
    }
    writer().push({clockUs(),id,tick,seq,a,b,c,d,threadId(),uint16_t(stage),type});
    thread_local uint64_t totalNs=0,calls=0;
    totalNs+=clockNs()-overheadStart;
    if(calls==0) writer().push({clockUs(),0,0,0,totalNs,1,2,0,threadId(),Overhead,0});
    if((++calls & 255)==0) writer().push({clockUs(),0,0,0,totalNs,calls,0,0,threadId(),Overhead,0});
}
struct Scope {
    Stage stage; uint64_t tick,seq,id=0;
    Scope(Stage s,uint64_t t=0,uint64_t q=0):stage(s),tick(t),seq(q) {
        if(enabled()) { id=nextId();event(s,t,q,0,0,0,0,1,id); }
    }
    ~Scope() { if(id) event(stage,tick,seq,0,0,0,0,2,id); }
};
inline void brightness(const uint8_t* p,uint32_t w,uint32_t h,size_t stride,uint64_t tick,uint64_t seq) {
    if(!enabled() || !p || !w || !h)return;
    uint64_t sum=0,black=0,mx=0;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
        const uint8_t* q=p+size_t((uint64_t(y)*h)/8)*stride+size_t((uint64_t(x)*w)/8)*4;
        unsigned l=(54u*q[0]+183u*q[1]+19u*q[2])>>8;
        sum+=l;black+=l<=3; if(l>mx)mx=l;
    }
    event(Brightness,tick,seq,sum,mx,black,(uint64_t(w)<<32)|h);
}
}

// TEMP(transformers): C ABI callback installed only by the tracing presenter thread.
namespace ps2PresentTrace {
inline thread_local uint64_t rayTick=0,raySeq=0,rayIds[4]{};
inline void rayEvent(int stage,int begin) {
    if(stage<Swap || stage>Pacing)return;
    auto &id=rayIds[stage-Swap];
    if(begin) {id=nextId();event(Stage(stage),rayTick,raySeq,0,0,0,0,1,id);}
    else if(id) {event(Stage(stage),rayTick,raySeq,0,0,0,0,2,id);id=0;}
}
}
