#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <xmmintrin.h>

// Admission-time metadata. Never roll the shared EE clock backwards during GS replay.
namespace gsTimeline {
// TEMP(transformers): optional per-read mode-0/worker trace, for route-by-route proof.
inline void traceCsr(uint32_t pc,uint64_t tick,uint32_t field) {
    static const bool enabled=[] {const auto *p=std::getenv("PS2X_VU1_CSR_TRACE");return p && *p!='0';}();
    if(enabled)std::fprintf(stderr,"[vu1t4:csr] pc=%08x tick=%llu field=%04x\n",pc,static_cast<unsigned long long>(tick),field&0x2000u);
}
struct Context { uint64_t tick=0,field=0; uint32_t mxcsr=0; bool valid=false; };
inline thread_local Context current{};
inline uint64_t tick(uint64_t live) { return current.valid ? current.tick : live; }
inline uint64_t csr(uint64_t live) { return current.valid ? (live&~0x2000ull)|current.field : live; }
struct Scope {
    Context previous; uint32_t previousMxcsr;
    explicit Scope(Context value):previous(current),previousMxcsr(_mm_getcsr()) {
        current=value;_mm_setcsr(value.mxcsr);
    }
    ~Scope(){current=previous;_mm_setcsr(previousMxcsr);}
};
}
