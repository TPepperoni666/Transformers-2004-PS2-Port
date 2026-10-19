#include "ps2_ee_hle.h"
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif
namespace ps2_ee_hle {
static void cpuid(unsigned leaf,unsigned sub,unsigned* r) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    __cpuidex(reinterpret_cast<int*>(r),leaf,sub);
#elif defined(__x86_64__)
    __cpuid_count(leaf,sub,r[0],r[1],r[2],r[3]);
#else
    r[0]=r[1]=r[2]=r[3]=0;
#endif
}
Caps capabilities() {
    Caps c{}; unsigned r[4];cpuid(0,0,r);unsigned max=r[0];
    if(max<1) return c;
    cpuid(1,0,r);
    if((r[2]&((1u<<27)|(1u<<28)))!=((1u<<27)|(1u<<28))) return c;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    c.xcr0=_xgetbv(0);
#elif defined(__x86_64__)
    unsigned lo,hi; __asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));c.xcr0=lo|(uint64_t(hi)<<32);
#endif
    if(max<7 || (c.xcr0&6)!=6) return c;
    cpuid(7,0,r);c.avx2=(r[1]&(1u<<5))!=0;
    // MSVC /arch:AVX512 allows F,CD,DQ,BW,VL. Require all, including OS opmask/ZMM state.
    constexpr unsigned need=(1u<<16)|(1u<<17)|(1u<<28)|(1u<<30)|(1u<<31);
    c.avx512=c.avx2 && (c.xcr0&0xe6)==0xe6 && (r[1]&need)==need;
    return c;
}
Level choose(const char* e,Caps c) {
    if(e && !std::strcmp(e,"sse4")) return Level::sse4;
    if(e && !std::strcmp(e,"avx2")) return c.avx2?Level::avx2:Level::sse4;
    return c.avx512?Level::avx512:c.avx2?Level::avx2:Level::sse4;
}
}
