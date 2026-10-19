#pragma once
#include "ps2_vu1_worker.h"
#include "ps2_wake.h"
#include <deque>
#include <optional>
#include <vector>
#include <memory>
#include <cstdio>
#include <algorithm>
#include "ps2_gs_timeline.h"
int ps2Vu1CurrentPriority() noexcept;
void ps2Vu1SetPriority(int) noexcept;
// TEMP(transformers) qualification: perfscope hooks (ps2_perfscope.cpp). Worker qualification = busy/idle; EE fence waits are
// charged to one EE wait bucket per reason (g_vu1FenceReason, set by Vu1FenceReason at the call sites).
enum : uint8_t { kVu1FenceOther=0, kVu1FenceCsr=1, kVu1FenceMem=2, kVu1FenceGs=3, kVu1FenceCap=4 };
inline thread_local uint8_t g_vu1FenceReason=kVu1FenceOther;
struct Vu1FenceReason {
    uint8_t prev;
    explicit Vu1FenceReason(uint8_t r) noexcept : prev(g_vu1FenceReason) { g_vu1FenceReason=r; }
    ~Vu1FenceReason() { g_vu1FenceReason=prev; }
};
void ps2Vu1PerfWorkerStart() noexcept;
void ps2Vu1PerfIdle(bool begin) noexcept;
void ps2Vu1PerfFence(bool begin,uint8_t reason) noexcept;
void ps2Vu1PerfWork(bool begin,uint8_t which) noexcept; // 0 = EE queue admission, 1 = EE replay of queued output
struct Vu1PerfWorkScope {
    explicit Vu1PerfWorkScope(uint8_t w) noexcept { ps2Vu1PerfWork(true,w); }
    ~Vu1PerfWorkScope() { ps2Vu1PerfWork(false,0); }
};
struct Vu1PerfFenceScope {
    uint8_t reason;bool on=false;
    explicit Vu1PerfFenceScope(uint8_t r) noexcept : reason(r) {}
    void begin() noexcept { if(!on){on=true;ps2Vu1PerfFence(true,reason);} }
    ~Vu1PerfFenceScope() { if(on)ps2Vu1PerfFence(false,reason); }
};

struct Vif1ScanDependency : std::exception {};

// Guest-instant VIF jobs. Only the EE drains GS events; the worker never enters GS.
class Vif1Instant {
public:
    static bool enabled() { static const bool on=[] { auto p=std::getenv("PS2X_VU1_THREAD");return p&&p[0]=='3'&&!p[1]; }();return on; }
    inline static thread_local Vif1Instant *executing=nullptr;
    bool inWorker() const { return executing==this; }
    bool replaying() const { return ownerToken.load(std::memory_order_acquire)==Vu1Worker::currentThreadToken() && draining; }
    bool pending() const { return !idle.load(std::memory_order_acquire); }
    uint32_t queuedEvents() const { return nEvents.load(std::memory_order_acquire); } // TEMP(transformers) qualification
    uint32_t controls=0,stopBits=0;
    uint64_t fences=0,capacityWaits=0;
    std::function<void(uint32_t)> publishStatus;
    std::function<gsTimeline::Context()> captureContext;
    std::function<void()> afterReplay;
    std::function<void(const uint8_t*,uint32_t,uint32_t,bool,bool)> packetReplay;
    uint64_t oracleField=0; // worker-only serial reference of the EE timeline CSR FIELD
    std::atomic<uint64_t> csrOracleReads{0},csrOracleFailures{0}; // TEMP(transformers)
    std::atomic<uint64_t> directPackets{0},batchedPackets{0},packetBatches{0}; // TEMP(transformers)
    gsTimeline::Context context() const {
        auto c=inWorker()?gsTimeline::current:(captureContext?captureContext():gsTimeline::Context{});
        c.mxcsr=_mm_getcsr();return c;
    }
    bool directReady() {
        bindOwner();if(idle.load(std::memory_order_acquire))return true;
        drain();std::lock_guard<std::mutex> l(mutex);check();
        return completed==submitted && events.empty();
    }
    // Packet bytes and descriptors share one allocation-growing batch. EE packets
    // are placed AFTER the last admitted ticket, without a worker job or wakeup.
    void packet(const uint8_t *data,uint32_t size,uint32_t path,bool immediate,bool hl) {
        const bool worker=inWorker();
        // TEMP(transformers) qualification: the worker appends its packets to a private chunk without the queue mutex and
        // publishes the chunk as ONE event once it holds PS2X_VU1_WCHUNK bytes (default 64 KiB) or 4096 packets, before
        // any other worker event (emit) and at the end of every job (before `completed` moves). Queue order is (ticket,
        // after) as before, every packet keeps its own admission context, so the replayed GS stream is unchanged; only
        // the host time at which the EE sees the packets moves (drain timing is host-only, round 4 already batched).
        // PS2X_VU1_WCHUNK=0 = round 4 (one locked insertion per worker packet).
        if(worker && chunkLimit()) {
            const auto c=context();
            if(!wchunk) {
                wchunk=std::make_shared<PacketBatch>();
                wchunk->bytes.reserve(std::max<size_t>(size,std::min<size_t>(chunkLimit(),std::max<size_t>(wLastBytes,4096u))));
                wchunk->packets.reserve(std::min<size_t>(4096u,std::max<size_t>(wLastPackets,64u)));
            }
            wchunk->packets.push_back({wchunk->bytes.size(),size,path,immediate,hl,c});
            wchunk->bytes.insert(wchunk->bytes.end(),data,data+size);batchedPackets.fetch_add(1,std::memory_order_relaxed);
            if(wchunk->bytes.size()>=chunkLimit() || wchunk->packets.size()>=4096u) {std::lock_guard<std::mutex> l(mutex);check();publishChunk();}
            return;
        }
        std::optional<Vu1PerfWorkScope> perf;if(!worker)perf.emplace(0); // TEMP(transformers) qualification
        const auto c=context();
        std::lock_guard<std::mutex> l(mutex);check();
        if(size>128u*1024u*1024u-retained)throw std::runtime_error("VU1 GS event capacity exceeded");
        idle.store(false,std::memory_order_release);
        const uint64_t ticket=worker?executingTicket:submitted;
        auto at=insertPos([&](const Event &e){return e.ticket>ticket || (e.ticket==ticket && e.after>!worker);});
        std::shared_ptr<PacketBatch> batch;
        if(at!=events.begin()) {
            auto prev=std::prev(at);
            if(prev->ticket==ticket && prev->after==!worker && prev->batch && prev->bytes+size<=65536u)batch=prev->batch;
        }
        if(!batch) {
            batch=std::make_shared<PacketBatch>();batch->bytes.reserve(std::max<uint32_t>(size,4096u));batch->packets.reserve(128);
            at=events.insert(at,Event{{},0,ticket,!worker,c,batch});++packetBatches;nEvents.store(static_cast<uint32_t>(events.size()),std::memory_order_release);
        } else --at;
        batch->packets.push_back({batch->bytes.size(),size,path,immediate,hl,c});
        batch->bytes.insert(batch->bytes.end(),data,data+size);at->bytes+=size;retained+=size;++batchedPackets;
    }
    void emit(std::function<void()> event,size_t bytes=0) {
        std::lock_guard<std::mutex> l(mutex);
        if(inWorker())publishChunk(); // TEMP(transformers) qualification: earlier worker packets stay in front
        if(bytes>128u*1024u*1024u-retained)throw std::runtime_error("VU1 GS event capacity exceeded");
        idle.store(false,std::memory_order_release);retained+=bytes;
        const uint64_t ticket=executingTicket;
        auto at=insertPos([&](const Event &e){return e.ticket>ticket || (e.ticket==ticket && e.after);});
        events.insert(at,Event{std::move(event),bytes,ticket,false,context(),{}});nEvents.store(static_cast<uint32_t>(events.size()),std::memory_order_release);wakeChanged();
    }
    void enqueue(std::function<void()> work,bool stream=false,uint32_t ctl=0) {
        if(inWorker()) {work();return;}
        bindOwner();
        drain();
        if(stream) {
            Vu1PerfFenceScope perf(kVu1FenceCap); // TEMP(transformers) qualification
            for(;;) {
                {std::unique_lock<std::mutex> l(mutex);check();if(streams<2)break;++capacityWaits;perf.begin();eeWaiting=true;changed.wait(l);eeWaiting=false;}
                drain();
            }
        }
        const auto admission=context();const int priority=ps2Vu1CurrentPriority();
        std::lock_guard<std::mutex> l(mutex);check();
        if(!thread.joinable())thread=std::thread([this]{loop();});
        idle.store(false,std::memory_order_release);
        const uint64_t ticket=++submitted;if(stream)++streams;
        jobs.push_back({std::move(work),ticket,stream,ctl,admission,priority});wakeChanged();
    }
    void drain() {
        if(inWorker())return;
        if(ownerToken.load(std::memory_order_acquire)!=Vu1Worker::currentThreadToken()) return;
        if(draining)return; // presenter never consumes the EE queue
        // TEMP(transformers) qualification: checkpointDue drains at every guest checkpoint; with nothing queued skip the mutex
        // (PS2X_VU1_DRAIN_FAST=0 = round 4: always lock). Queue errors still surface at the next enqueue/fence/packet.
        static const bool fast=[]{const char *p=std::getenv("PS2X_VU1_DRAIN_FAST");return !(p && p[0]=='0');}();
        if(fast && nEvents.load(std::memory_order_acquire)==0u)return;
        draining=true;
        try {
            for(;;) {
                Event e;
                {std::lock_guard<std::mutex> l(mutex);check();if(events.empty() || (events.front().after && completed<events.front().ticket)){idle.store(completed==submitted && events.empty(),std::memory_order_release);break;}e=std::move(events.front());events.pop_front();nEvents.store(static_cast<uint32_t>(events.size()),std::memory_order_release);retained-=e.bytes;}
                Vu1PerfWorkScope perf(1); // TEMP(transformers) qualification
                if(e.batch) {
                    for(const auto &p:e.batch->packets) {
                        gsTimeline::Scope scope(p.context);
                        packetReplay(e.batch->bytes.data()+p.offset,p.size,p.path,p.immediate,p.hl);
                    }
                } else {gsTimeline::Scope scope(e.context);e.fn();}
                if(afterReplay)afterReplay();
            }
        }catch(...){draining=false;throw;}
        draining=false;
    }
    void fence() {
        if(inWorker())return;
        const auto owner=ownerToken.load(std::memory_order_acquire);if(!owner)return;
        if(owner!=Vu1Worker::currentThreadToken())throw std::logic_error("VU1 fence off EE");
        if(draining)return;
        uint64_t target;bool waits=false;{std::lock_guard<std::mutex> l(mutex);target=submitted;if(completed<target){++fences;waits=true;}}
        Vu1PerfFenceScope perf(g_vu1FenceReason);if(waits)perf.begin(); // TEMP(transformers) qualification: EE wait by reason
        for(;;) {
            drain();std::unique_lock<std::mutex> l(mutex);check();if(completed>=target)break;eeWaiting=true;changed.wait(l);eeWaiting=false;
        }
        drain();if(publishStatus)publishStatus(stopBits); // bit 8 is never published
    }
    void abandon() noexcept {
        {std::unique_lock<std::mutex> l(mutex);abandonWaiting.store(true);changed.wait(l,[this]{return completed==submitted;});abandonWaiting.store(false);stop=true;if(ps2Wake::lazy)wakeChanged();}
        if(!ps2Wake::lazy)changed.notify_all();if(thread.joinable())thread.join();
    }
    ~Vif1Instant(){
        abandon();
        // TEMP(transformers): oracle is always active; this knob only prints its totals.
        if(const auto *p=std::getenv("PS2X_VU1_CSR_ORACLE");p && *p!='0')
            std::fprintf(stderr,"[vu1t4] csrReads=%llu csrFailures=%llu directPackets=%llu batchedPackets=%llu packetBatches=%llu fences=%llu capacityWaits=%llu\n",
                static_cast<unsigned long long>(csrOracleReads.load()),static_cast<unsigned long long>(csrOracleFailures.load()),
                static_cast<unsigned long long>(directPackets.load()),static_cast<unsigned long long>(batchedPackets.load()),static_cast<unsigned long long>(packetBatches.load()),
                static_cast<unsigned long long>(fences),static_cast<unsigned long long>(capacityWaits));
    }
private:
    // TEMP(transformers) qualification: insertion point of a new event (mutex held). PS2X_VU1_QSCAN=0 = round-4 forward scan.
    template<class P> auto insertPos(P pred) {
        static const bool rev=[]{const char *p=std::getenv("PS2X_VU1_QSCAN");return !(p && p[0]=='0');}();
        uint64_t steps=0;std::deque<Event>::iterator at;
        if(rev) {at=events.end();while(at!=events.begin() && pred(*std::prev(at))){--at;++steps;}}
        else {at=std::find_if(events.begin(),events.end(),pred);steps=static_cast<uint64_t>(at-events.begin())+(at!=events.end());}
        qScanSteps+=steps;if(events.size()>qMaxEvents)qMaxEvents=events.size();
        if((++qScans & 0xFFFFFu)==0u) {
            std::fprintf(stderr,"[VU1 queue:q] scans=%llu avgScan=%.2f maxEvents=%llu rev=%d directPackets=%llu batchedPackets=%llu packetBatches=%llu fences=%llu\n",
                static_cast<unsigned long long>(qScans),static_cast<double>(qScanSteps)/static_cast<double>(qScans),
                static_cast<unsigned long long>(qMaxEvents),rev?1:0,static_cast<unsigned long long>(directPackets.load()),
                static_cast<unsigned long long>(batchedPackets.load()),static_cast<unsigned long long>(packetBatches.load()),
                static_cast<unsigned long long>(fences));
        }
        return at;
    }
    uint64_t qScans=0,qScanSteps=0,qMaxEvents=0;
    struct Packet {size_t offset;uint32_t size,path;bool immediate,hl;gsTimeline::Context context;};
    struct PacketBatch {std::vector<uint8_t> bytes;std::vector<Packet> packets;};
    struct Job {std::function<void()> fn;uint64_t ticket;bool stream;uint32_t ctl;gsTimeline::Context context;int priority;};
    struct Event {std::function<void()> fn;size_t bytes;uint64_t ticket;bool after;gsTimeline::Context context;std::shared_ptr<PacketBatch> batch;};
    uint64_t executingTicket=0;
    // TEMP(transformers) VU1 workerload: worker-private PATH1 chunk (worker thread only) and its publication.
    std::shared_ptr<PacketBatch> wchunk;size_t wLastBytes=0,wLastPackets=0;
    std::atomic<bool> workerWaiting{false},abandonWaiting{false};
    std::atomic<bool> eeWaiting{false}; // EE blocked in fence/capacity wait (mutex held): wake it per published chunk so it replays meanwhile
    void wakeChanged() { // mutex held, including predicate and flag reads
        const bool waiting=workerWaiting.load()||eeWaiting.load()||abandonWaiting.load();
        if(!ps2Wake::lazy || waiting){changed.notify_all();}
    }
    static size_t chunkLimit() {
        static const size_t n=[]{const char *p=std::getenv("PS2X_VU1_WCHUNK");if(!p||!*p)return size_t{65536};const long v=std::strtol(p,nullptr,10);return v>0?static_cast<size_t>(v):size_t{0};}();
        return n;
    }
    static bool fenceWake() {static const bool on=[]{const char *p=std::getenv("PS2X_VU1_FENCE_WAKE");return !(p && p[0]=='0');}();return on;}
    void publishChunk() { // mutex held, worker thread
        if(!wchunk)return;
        if(wchunk->packets.empty()){wchunk.reset();return;}
        const size_t n=wchunk->bytes.size();
        if(n>128u*1024u*1024u-retained)throw std::runtime_error("VU1 GS event capacity exceeded");
        idle.store(false,std::memory_order_release);
        const uint64_t ticket=executingTicket;
        auto at=insertPos([&](const Event &e){return e.ticket>ticket || (e.ticket==ticket && e.after);});
        wLastBytes=n;wLastPackets=wchunk->packets.size();
        const auto c=wchunk->packets.front().context;
        events.insert(at,Event{{},n,ticket,false,c,std::move(wchunk)});wchunk.reset();
        retained+=n;++packetBatches;nEvents.store(static_cast<uint32_t>(events.size()),std::memory_order_release);
        if(eeWaiting && fenceWake())wakeChanged();
    }
    void bindOwner() {
        uint64_t unbound=0;const auto token=Vu1Worker::currentThreadToken();
        if(!ownerToken.load(std::memory_order_acquire))ownerToken.compare_exchange_strong(unbound,token,std::memory_order_acq_rel);
        if(ownerToken.load(std::memory_order_acquire)!=token)throw std::logic_error("VU1 submit off EE owner");
    }
    std::atomic<bool> idle{true};
    std::atomic<uint32_t> nEvents{0}; // TEMP(transformers) qualification: events.size() mirror for the lock-free drain early-out
    std::atomic<uint64_t> ownerToken{0}; // constructor may run on the render thread; bind at first EE submission
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::deque<Event> events;
    uint64_t submitted=0,completed=0;size_t retained=0;unsigned streams=0;
    bool stop=false,draining=false;
    std::exception_ptr error;
    std::thread thread;
    void check(){if(error)std::rethrow_exception(error);}
    void loop() {
        executing=this;
        ps2Vu1PerfWorkerStart(); // TEMP(transformers) VU1 worker
        for(;;) {
            Job j;
            {std::unique_lock<std::mutex> l(mutex);if(!stop&&jobs.empty()){ps2Vu1PerfIdle(true);workerWaiting.store(true);changed.wait(l,[this]{return stop||!jobs.empty();});workerWaiting.store(false);ps2Vu1PerfIdle(false);}if(stop)return;j=std::move(jobs.front());jobs.pop_front();controls=j.ctl;}
            executingTicket=j.ticket;ps2Vu1SetPriority(j.priority);
            try{gsTimeline::Scope scope(j.context);j.fn();}catch(...){std::lock_guard<std::mutex> l(mutex);if(!error)error=std::current_exception();}
            {std::lock_guard<std::mutex> l(mutex);
                try{publishChunk();}catch(...){if(!error)error=std::current_exception();} // TEMP(transformers) VU1 workerload
                wchunk.reset();
                completed=j.ticket;if(j.stream)--streams;
                if(!error && completed==submitted && events.empty())idle.store(true,std::memory_order_release);if(ps2Wake::lazy)wakeChanged();}
            if(!ps2Wake::lazy)changed.notify_all();
        }
    }
};
