// GAME(transformers): 512-bit exact VU0 arithmetic; compile without contraction.
#include "ps2_ee_hle.h"
#ifndef PS2X_FPU_OPERAND_CLAMP
#define PS2X_FPU_OPERAND_CLAMP 1
#endif
namespace ps2_ee_hle {
namespace {
inline __m512 clamp(__m512 x) {
    const __m512i bits=_mm512_castps_si512(x);
    const __m512i mask=_mm512_set1_epi32(0x7f800000);
    const __mmask16 special=_mm512_cmpeq_epi32_mask(_mm512_and_si512(bits,mask),mask);
    const __m512i saturated=_mm512_or_si512(_mm512_and_si512(bits,_mm512_set1_epi32(int(0x80000000u))),_mm512_set1_epi32(0x7f7fffff));
    return _mm512_castsi512_ps(_mm512_mask_mov_epi32(bits,special,saturated));
}
inline __m512 operand(__m512 x) {
#if PS2X_FPU_OPERAND_CLAMP
    return clamp(x);
#else
    return x;
#endif
}
inline __m512 mul(__m512 a,__m512 b) {return clamp(_mm512_mul_ps(a,b));}
inline __m512 add(__m512 a,__m512 b) {return clamp(_mm512_add_ps(a,b));}
inline __m512 repeat(__m128 a) {return _mm512_broadcast_f32x4(a);}
}
void matrixAvx512(const __m128* a,const __m128* b,__m128 w,__m128* out,__m128* acc,unsigned mode) {
#if !PS2X_FPU_OPERAND_CLAMP
    // Result-only clamping permits NaN operands. EVEX may choose a different NaN sign
    // than legacy SSE, which becomes guest-visible saturation. Preserve SSE here.
    matrixSse(a,b,w,out,acc,mode); return;
#else
    const __m512 a0=operand(repeat(a[0])),a1=operand(repeat(a[1])),a2=operand(repeat(a[2])),a3=operand(repeat(a[3]));
    for(unsigned j=0;j<4;j+=4) {
        const __m512 v=operand(_mm512_loadu_ps(reinterpret_cast<const float*>(b+j)));
        __m512 x=mul(a0,_mm512_permute_ps(v,0));
        x=add(x,mul(a1,_mm512_permute_ps(v,0x55)));
        x=add(x,mul(a2,_mm512_permute_ps(v,0xaa)));
        *acc=_mm512_extractf32x4_ps(x,3);
        if(!mode) x=add(x,mul(a3,_mm512_permute_ps(v,0xff)));
        else {
            x=_mm512_mask_mov_ps(x,0xf000,add(x,mul(a3,operand(repeat(w)))));
        }
        _mm512_storeu_ps(reinterpret_cast<float*>(out+j),x);
    }
#endif
}
}
