#pragma once
#include <cstdint>
#include <cstring>
#if defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif
struct R5900Context;
class PS2Runtime;
namespace ps2_ee_hle {
enum class Level { sse4, avx2, avx512 };
struct Caps { bool avx2, avx512; uint64_t xcr0; };
Caps capabilities();
Level choose(const char*,Caps);
// mode 0: full 4x4; mode 1: affine xyzw; mode 2: affine xyz with W preserved.
// a/b remain raw in guest registers. out/acc are clamped after each operation.
using Kernel=void(*)(const __m128*,const __m128*,__m128,__m128*,__m128*,unsigned);
void matrixSse(const __m128*,const __m128*,__m128,__m128*,__m128*,unsigned);
void matrixAvx2(const __m128*,const __m128*,__m128,__m128*,__m128*,unsigned);
void matrixAvx512(const __m128*,const __m128*,__m128,__m128*,__m128*,unsigned);
Kernel implementation(Level);
using Function=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
struct State {
    Function originals[3]{};
    Kernel selected=matrixSse;
};
// The generated dispatch table is process-global. Enforce its existing
// single-live-runtime contract before any startup hooks can touch that table.
void acquireRuntime(PS2Runtime&);
void releaseRuntime(PS2Runtime&) noexcept;
class RuntimeClaim {
    PS2Runtime& runtime;
    bool committed=false;
public:
    explicit RuntimeClaim(PS2Runtime& rt):runtime(rt) { acquireRuntime(rt); }
    ~RuntimeClaim() { if(!committed) releaseRuntime(runtime); }
    void commit() noexcept { committed=true; }
    RuntimeClaim(const RuntimeClaim&)=delete;
    RuntimeClaim& operator=(const RuntimeClaim&)=delete;
};
// ELF CRC of the reviewed, unmodified US release. Never identify by parent path.
inline constexpr uint32_t reviewedCrc32=0x74c982c0u;
inline bool acceptedIdentity(const char* name,uint32_t entry,uint32_t crc,bool valid) {
    return valid && crc==reviewedCrc32 && entry==0x00100008u &&
           name && std::strcmp(name,"SLUS_206.68")==0;
}
void install(PS2Runtime&,const char*,uint32_t,uint32_t,bool);
void uninstall(PS2Runtime&);
void full(uint8_t*,R5900Context*,PS2Runtime*);
void affine(uint8_t*,R5900Context*,PS2Runtime*);
void affineXyz(uint8_t*,R5900Context*,PS2Runtime*);
}
