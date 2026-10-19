// TEMP(transformers): bounded asynchronous GPU observer; uses existing fence-recycled Granite queries.
#pragma once
#include "ps2_present_trace.h"
#include "device.hpp"
#include "command_buffer.hpp"
#include <memory>
namespace ps2PresentTrace {
struct GpuTrace {
    struct Pending {
        Vulkan::QueryPoolHandle begin,end;
        uint64_t frame=0,tick=0,id=0,meta=0;
        Stage stage=GpuScanout;
    };
    Vulkan::Device &device;
    std::array<Pending,256> pending{};
    uint64_t frame=0,tick=0,dropped=0,completed=0,unsupported=0,observerNs=0;
    explicit GpuTrace(Vulkan::Device &d):device(d) {}
    void poll(bool final=false) {
        const auto start=clockNs();
        uint64_t depth=0;
        for(auto &p:pending) if(p.begin) {
            // is_signalled is populated only by Granite's existing frame-context recycling,
            // AFTER PerFrame::wait() has completed its pre-existing submission fences.
            // No vkGetQueryPoolResults, WAIT, fence wait or submission is added here.
            if(p.end && (final || frame>=p.frame+3) && p.begin->is_signalled() && p.end->is_signalled()) {
                const auto a=p.begin->get_timestamp_ticks(),b=p.end->get_timestamp_ticks();
                const double ns=device.convert_device_timestamp_delta(a,b)*1e9;
                event(p.stage,p.tick,0,a,b,uint64_t(ns<0?0:ns),p.meta,4,p.id);
                p={};++completed;
            } else ++depth;
        }
        observerNs+=clockNs()-start;
        event(GpuStatus,tick,0,completed,dropped,depth,unsupported);
        event(Overhead,tick,0,observerNs,frame,1); // component 1: GPU observer wall ns, inclusive of trace emits
    }
    void advance(uint64_t t) {tick=t;++frame;poll();}
    Pending *reserve(Stage stage,uint64_t meta) {
        if(!device.get_queue_info().timestamp_valid_bits) {++unsupported;return nullptr;}
        for(auto &p:pending) if(!p.begin) {
            p.stage=stage;p.meta=meta;p.frame=frame;p.tick=tick;p.id=nextId();return &p;
        }
        ++dropped;return nullptr;
    }
};
inline thread_local GpuTrace *activeGpu=nullptr;
struct GpuBind {
    GpuTrace *previous=nullptr;
    bool on=false;
    GpuBind(Vulkan::Device &device,std::unique_ptr<GpuTrace> &owner) {
        if(!(on=enabled()))return;
        previous=activeGpu;
        if(!owner)owner=std::make_unique<GpuTrace>(device);
        activeGpu=owner.get();
    }
    ~GpuBind() {if(on)activeGpu=previous;}
};
struct GpuScope {
    Vulkan::CommandBuffer &cmd;
    GpuTrace *owner=nullptr;
    GpuTrace::Pending *p=nullptr;
    GpuScope(Vulkan::CommandBuffer &c,Stage stage,uint64_t meta=0):cmd(c) {
        if(!enabled() || !(owner=activeGpu))return;
        const auto start=clockNs();
        p=owner->reserve(stage,meta);
        if(p) {
            p->begin=cmd.write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
            if(!p->begin) {++owner->unsupported;*p={};p=nullptr;}
        }
        owner->observerNs+=clockNs()-start;
    }
    void finish() {
        if(!p)return;
        const auto start=clockNs();
        p->end=cmd.write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        if(!p->end) {++owner->unsupported;*p={};}
        else event(p->stage,p->tick,0,p->frame,p->meta,0,0,5,p->id); // recorded, not GPU execution/submit time
        owner->observerNs+=clockNs()-start;p=nullptr;
    }
    ~GpuScope() {finish();}
    GpuScope(const GpuScope&)=delete;
};
}
