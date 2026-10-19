// GAME(transformers): 256-bit exact VU0 arithmetic; compile without contraction.
#include "ps2_ee_hle.h"
#ifndef PS2X_FPU_OPERAND_CLAMP
#define PS2X_FPU_OPERAND_CLAMP 1
#endif
namespace ps2_ee_hle {
namespace {
inline __m256 clamp(__m256 x) {
    const __m256i bits=_mm256_castps_si256(x);
    const __m256i mask=_mm256_set1_epi32(0x7f800000);
    const __m256i special=_mm256_cmpeq_epi32(_mm256_and_si256(bits,mask),mask);
    const __m256i saturated=_mm256_or_si256(_mm256_and_si256(bits,_mm256_set1_epi32(int(0x80000000u))),_mm256_set1_epi32(0x7f7fffff));
    return _mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(special,saturated),_mm256_andnot_si256(special,bits)));
}
inline __m256 operand(__m256 x) {
#if PS2X_FPU_OPERAND_CLAMP
    return clamp(x);
#else
    return x;
#endif
}
inline __m256 mul(__m256 a,__m256 b) {return clamp(_mm256_mul_ps(a,b));}
inline __m256 add(__m256 a,__m256 b) {return clamp(_mm256_add_ps(a,b));}
inline __m256 repeat(__m128 a) {return _mm256_insertf128_ps(_mm256_castps128_ps256(a),a,1);}
}
void matrixAvx2(const __m128* a,const __m128* b,__m128 w,__m128* out,__m128* acc,unsigned mode) {
    const __m256 a0=operand(repeat(a[0])),a1=operand(repeat(a[1])),a2=operand(repeat(a[2])),a3=operand(repeat(a[3]));
    for(unsigned j=0;j<4;j+=2) {
        const __m256 v=operand(_mm256_loadu_ps(reinterpret_cast<const float*>(b+j)));
        __m256 x=mul(a0,_mm256_permute_ps(v,0));
        x=add(x,mul(a1,_mm256_permute_ps(v,0x55)));
        x=add(x,mul(a2,_mm256_permute_ps(v,0xaa)));
        if(j==2) *acc=_mm256_extractf128_ps(x,1);
        if(!mode) x=add(x,mul(a3,_mm256_permute_ps(v,0xff)));
        else {
            if(j==2) x=_mm256_blend_ps(x,add(x,mul(a3,operand(repeat(w)))),0xf0);
        }
        _mm256_storeu_ps(reinterpret_cast<float*>(out+j),x);
    }
}
}
