#include <bit>
struct VectorResult {__m128 value;uint32_t zero,sign,under,over,extra;};
__declspec(noinline) VectorResult vectorCold(__m128,__m128,__m128,__m128,__m128,uint8_t,uint8_t,uint32_t);
__forceinline float vectorLane(__m128 v,unsigned lane){alignas(16) float x[4];_mm_store_ps(x,v);return x[lane];}
__forceinline uint32_t vectorLaneBits(__m128 v,unsigned lane){return std::bit_cast<uint32_t>(vectorLane(v,lane));}
__forceinline __m128 vectorMerge(__m128 old,__m128 value,unsigned mask){const __m128 m=_mm_castsi128_ps(vuLaneMask(kVuRev4[mask]));return _mm_or_ps(_mm_andnot_ps(m,old),_mm_and_ps(m,value));}
template<uint32_t U,bool NS,bool NT,bool NA>
__forceinline VectorResult vectorFmac(__m128 rawS,__m128 rawT,__m128 rawA,float rawQ,float rawI){
 VuFmacKind kind;VuFmacSrc src;uint8_t sel=0;bool toAcc=false;vuDecodeFmac(U,kind,src,sel,toAcc);

    const uint8_t dest = DEST(U);
    if (dest == 0u)
        return {};

    const uint8_t fs = FS(U);
    const uint8_t ft = FT(U);
    __m128 s = (NS ? rawS : vuNormalize4(rawS));
    __m128 t;
    switch (src)
    {
    case VuSrcBc:
        t = _mm_set1_ps((NT?vectorLane(rawT,sel&3u):aotNormalizeOperand(vectorLane(rawT,sel&3u))));
        break;
    case VuSrcQ:
        t = _mm_set1_ps(rawQ);
        break;
    case VuSrcI:
        t = _mm_set1_ps(rawI);
        break;
    case VuSrcVt:
        t = (NT ? rawT : vuNormalize4(rawT));
        break;
    default: // cross: x = vs.y*vt.z, y = vs.z*vt.x, z = vs.x*vt.y; w lane vs.w*vt.w (product sticky only)
    {
        const __m128 vt = (NT ? rawT : vuNormalize4(rawT));
        s = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
        t = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 1, 0, 2));
        break;
    }
    }

    // Float result first (execUpper's expressions, lane for lane).
    const bool productSum = kind == VuMadd || kind == VuMsub;
    __m128 a = _mm_setzero_ps();
    __m128 product = _mm_setzero_ps();
    __m128 f;
    switch (kind)
    {
    case VuAdd: f = _mm_add_ps(s, t); break;
    case VuSub: f = _mm_sub_ps(s, t); break;
    case VuMul: f = _mm_mul_ps(s, t); break;
    default:
        a = (NA ? rawA : vuNormalize4(rawA));
        product = _mm_mul_ps(s, t);
        f = kind == VuMadd ? _mm_add_ps(a, product) : _mm_sub_ps(a, product);
        break;
    }
    if (src == VuSrcCross)
        f = _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(f), _mm_set_epi32(0, -1, -1, -1))); // w = 0.0f
    
    const uint32_t destBits = kVuRev4[dest & 0xFu]; // component order

    // Shortcut lanes whose class provably is "normal, sign only" (proof, with x the real a op b or
    // acc +/- p, s the real acc +/- P, P the exact product, p = chop(P), r the float result, E the
    // reference's double result = chop_double(exact)):
    //   single add/sub/mul, exp(r) in [1,254], |r| != FLT_MAX: FLT_MIN <= |r| <= |E| <= |x| <
    //     next(|r|) <= FLT_MAX and sign(E) = sign(r). r == 0 or denormal is NOT taken (an exact zero vs
    //     an underflow needs the double), nor |r| == FLT_MAX (overflow needs the double).
    //   product-sum, exp(r) in [2,253], p normal, |p| != FLT_MAX, exp(p) <= exp(r) + 21:
    //     |s - x| = |P - p| < ulp(p) <= 2^(exp(r)-129) <= |r|/4, so |s| > (3/4)|r| >= (3/4)2^-125 > FLT_MIN,
    //     |s| < 2^(exp(r)-126) + 2^(exp(r)-129) <= 1.125*2^127 < FLT_MAX, and sign(s) = sign(x) = sign(r);
    //     chop_double keeps all three. The product's own class (sticky) is "sign only" because p is
    //     normal and not FLT_MAX (the single-mul case above). (Same conditions as Scotho/socom-unzipped
    //     productSumFastLanes, GPL-3.0, re-derived here against our double reference.)
    //   Any other dest lane (and OPMULA/OPMSUB's w lane, an exact zero) takes the double path below.
    {
        const __m128i rb = _mm_castps_si128(f);
        const __m128i er = _mm_and_si128(_mm_srli_epi32(rb, 23), _mm_set1_epi32(0xFF));
        const __m128i notMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(rb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                             _mm_set1_epi32(-1));
        __m128i ok;
        if (!productSum)
        {
            ok = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi32(er, _mm_setzero_si128()), _mm_cmplt_epi32(er, _mm_set1_epi32(255))), notMax);
        }
        else
        {
            const __m128i pb = _mm_castps_si128(product);
            const __m128i ep = _mm_and_si128(_mm_srli_epi32(pb, 23), _mm_set1_epi32(0xFF));
            const __m128i pNotMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(pb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                                  _mm_set1_epi32(-1));
            ok = _mm_and_si128(_mm_cmpgt_epi32(er, _mm_set1_epi32(1)), _mm_cmplt_epi32(er, _mm_set1_epi32(254)));
            ok = _mm_and_si128(ok, _mm_and_si128(_mm_cmpgt_epi32(ep, _mm_setzero_si128()), _mm_cmplt_epi32(ep, _mm_set1_epi32(255))));
            ok = _mm_and_si128(ok, _mm_and_si128(pNotMax, _mm_cmpgt_epi32(_mm_add_epi32(er, _mm_set1_epi32(22)), ep)));
        }
        if ((static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(ok))) & destBits) == destBits)
        {
            const uint32_t sign = static_cast<uint32_t>(_mm_movemask_ps(f)) & destBits;
            const uint32_t extraSticky = productSum && (static_cast<uint32_t>(_mm_movemask_ps(product)) & destBits) ? 0x2u : 0u;
            return {f,0u,sign,0u,0u,extraSticky};
        }
    }

    // Cold operand classes reuse the shipping exact SIMD/double helper.
    return vectorCold(s,t,a,f,product,kind,src,destBits);
}
