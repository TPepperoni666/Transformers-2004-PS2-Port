#include "common.h"
VectorResult vectorCold(__m128 s,__m128 t,__m128 a,__m128 f,__m128 product,uint8_t kind,uint8_t src,uint32_t destBits){
 const bool productSum=kind==VuMadd||kind==VuMsub;
    // Double path: the reference's exact result and classification.
    const __m128d sLo = _mm_cvtps_pd(s);
    const __m128d sHi = _mm_cvtps_pd(_mm_movehl_ps(s, s));
    const __m128d tLo = _mm_cvtps_pd(t);
    const __m128d tHi = _mm_cvtps_pd(_mm_movehl_ps(t, t));
    __m128d eLo, eHi;
    __m128d pLo = _mm_setzero_pd(), pHi = _mm_setzero_pd();
    switch (kind)
    {
    case VuAdd:
        eLo = _mm_add_pd(sLo, tLo);
        eHi = _mm_add_pd(sHi, tHi);
        break;
    case VuSub:
        eLo = _mm_sub_pd(sLo, tLo);
        eHi = _mm_sub_pd(sHi, tHi);
        break;
    case VuMul:
        eLo = _mm_mul_pd(sLo, tLo);
        eHi = _mm_mul_pd(sHi, tHi);
        break;
    default:
    {
        const __m128d aLo = _mm_cvtps_pd(a);
        const __m128d aHi = _mm_cvtps_pd(_mm_movehl_ps(a, a));
        pLo = _mm_mul_pd(sLo, tLo);
        pHi = _mm_mul_pd(sHi, tHi);
        eLo = kind == VuMadd ? _mm_add_pd(aLo, pLo) : _mm_sub_pd(aLo, pLo);
        eHi = kind == VuMadd ? _mm_add_pd(aHi, pHi) : _mm_sub_pd(aHi, pHi);
        break;
    }
    }
    if (src == VuSrcCross)
        eHi = _mm_unpacklo_pd(eHi, _mm_setzero_pd()); // w: exact +0.0 (reference: 0.0L)

    const VuLaneClass e = vuClassify(eLo, eHi);
    const uint32_t zeroOrUnder = (e.zero | e.under) & destBits;
    const uint32_t over = e.over & destBits;
    const uint32_t under = e.under & destBits;
    const uint32_t sign = e.sign & destBits;
    const __m128i signOnly = _mm_and_si128(vuLaneMask(e.sign), _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i zuMask = vuLaneMask(zeroOrUnder);
    const __m128i overMask = vuLaneMask(over);
    __m128i value = _mm_andnot_si128(_mm_or_si128(zuMask, overMask), _mm_castps_si128(f));
    value = _mm_or_si128(value, _mm_and_si128(zuMask, signOnly));
    value = _mm_or_si128(value, _mm_and_si128(overMask, _mm_or_si128(signOnly, _mm_set1_epi32(0x7F7FFFFF))));

    uint32_t extraSticky = 0u;
    if (productSum)
    {
        const VuLaneClass p = vuClassify(pLo, pHi);
        extraSticky = (((p.zero | p.under) & destBits) ? 0x1u : 0u) | ((p.sign & destBits) ? 0x2u : 0u) |
                      ((p.under & destBits) ? 0x4u : 0u) | ((p.over & destBits) ? 0x8u : 0u);
    }
    return {_mm_castsi128_ps(value),zeroOrUnder,sign,under,over,extraSticky};
}
