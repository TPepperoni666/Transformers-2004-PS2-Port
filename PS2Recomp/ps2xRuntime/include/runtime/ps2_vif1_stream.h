#pragma once
#include "ps2_vu1_worker.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

// One immutable VIF stream, one execution owner, one EE publication owner.
// All guest timing uses integers; host readiness is never a guest busy bit.
class Vif1Stream
{
public:
    static bool enabled() {
        static const bool on = [] { const char *p = std::getenv("PS2X_VU1_THREAD"); return p && p[0]=='2' && !p[1]; }();
        return on;
    }
    inline static thread_local Vif1Stream *executing = nullptr;
    std::function<uint64_t()> now;
    std::function<void(uint64_t)> advance;
    std::function<uint32_t()> captureControls;
    std::function<void(uint32_t)> publishStatus;
    uint32_t controls = 0, err = 0, stopBits = 0;
    uint64_t vuCycles = 0;
    std::function<void()> dmaComplete;
    bool inWorker() const { return executing == this; }
    bool ownsCurrentThread() const { return owner.load()==Vu1Worker::currentThreadToken(); }
    bool pending() const { return active; }
    uint64_t deadline() { metadata(); return end; }
    uint64_t nextBoundary() const { return active && !advancing ? (haveMetadata ? end : earliest) : UINT64_MAX; }
    template<class Charge> bool chargeCheckpoint(uint32_t cycles, Charge charge) {
        uint64_t remaining=std::max<uint64_t>(1,cycles); bool completed=false;
        while (remaining) {
            const uint64_t cycle=now ? now() : 0;
            const uint64_t boundary=nextBoundary();
            if (boundary<=cycle) { completed=poll(cycle)||completed;continue; }
            const uint32_t step=static_cast<uint32_t>(std::min<uint64_t>(remaining,boundary-cycle));
            charge(step);remaining-=step;
            if (now && now()==boundary) completed=poll(boundary)||completed;
        }
        return completed;
    }
    void emit(std::function<void()> event, size_t bytes = 0) {
        if (!inWorker()) throw std::logic_error("VIF event emitted off worker");
        if (bytes > 64u*1024u*1024u-retained) throw std::runtime_error("VIF stream output exceeded 64 MiB");
        retained += bytes; events.push_back(std::move(event));
    }
    void submit(std::vector<uint8_t> bytes, std::function<void(const uint8_t *, uint32_t)> parse) {
        fence();
        if (bytes.size()>64u*1024u*1024u) throw std::runtime_error("VIF stream input exceeded 64 MiB");
        if (!worker) worker=std::make_unique<Vu1Worker>();
        owner.store(Vu1Worker::currentThreadToken());
        start=now ? now() : 0;
        transferCycles=2*((bytes.size()+15)/16); // VIF bus clock = EE/2; serial cost model.
        earliest=start+std::max<uint64_t>(1,transferCycles);
        controls=captureControls ? captureControls() : 0;
        stopBits=0; vuCycles=0; retained=0; events.clear(); dmaComplete={};
        active=true; haveMetadata=false;
        if (publishStatus) publishStatus(0x100u);
        worker->submit([this, bytes=std::move(bytes), parse=std::move(parse)] {
            struct Scope { Vif1Stream *previous; Scope(Vif1Stream *s):previous(executing){executing=s;} ~Scope(){executing=previous;} } scope(this);
            parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
        });
    }
    // Deterministic lookahead rendezvous. Never choose publication by host ready/late.
    bool poll(uint64_t cycle) {
        if (!active || inWorker() || publishing || advancing) return false;
        if (!ownsCurrentThread()) throw std::logic_error("VIF completion polled off EE owner");
        if (cycle<earliest) return false;
        metadata();
        if (cycle<end) return false;
        publish(); return true;
    }
    void fence() {
        if (!active || inWorker() || publishing || advancing) return;
        if (!ownsCurrentThread()) throw std::logic_error("VIF dependency fenced off EE owner");
        metadata();
        if (advance && now && now()<end) {
            advancing=true;
            try { advance(end-now()); } catch (...) { advancing=false;throw; }
            advancing=false;
        }
        publish();
    }
    void abandon() noexcept {
        if (worker) worker->finishNoThrow();
        active=false; events.clear(); dmaComplete={};
    }
    ~Vif1Stream() { abandon(); }
private:
    std::unique_ptr<Vu1Worker> worker;
    std::atomic<uint64_t> owner{0};
    bool active=false, haveMetadata=false, publishing=false, advancing=false;
    uint64_t start=0, earliest=0, end=0, transferCycles=0;
    size_t retained=0;
    std::vector<std::function<void()>> events;
    void metadata() {
        if (!active || haveMetadata) return;
        try { worker->finish(); }
        catch (...) { active=false; events.clear(); dmaComplete={}; throw; }
        if (vuCycles>(UINT64_MAX-start-transferCycles)/2) throw std::overflow_error("VIF deadline overflow");
        end=start+std::max<uint64_t>(1,transferCycles+vuCycles);
        haveMetadata=true;
    }
    void publish() {
        // Clear active before callbacks enter memory/GS fences.
        active=false; publishing=true;
        auto out=std::move(events); auto dma=std::move(dmaComplete);
        try {
            for (auto &event:out) event();
            if (dma) dma();
            if (publishStatus) publishStatus(stopBits);
        } catch (...) { publishing=false; throw; }
        publishing=false;
    }
};
