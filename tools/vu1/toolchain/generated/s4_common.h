#pragma once
// Transformers port: VU1 AOT round 4 (lane aot4) helpers. PRIVATE generated support code.
#include "common.h"
#include <bit>
#include <smmintrin.h>
#if defined(AOT4_STATS)   // standalone harness builds only (build.py passes /DAOT4_STATS)
extern uint64_t g_aot4Count[4];
#define AOT4_COUNT(k) (++g_aot4Count[k])
#define AOT4_CYC(n) (g_aot4Count[3]+=(n))
extern uint64_t g_aot4R[][12];
#define AOT4_RCALL(id) (++g_aot4R[id][0])
#define AOT4_RENTER(id) (++g_aot4R[id][1])
#define AOT4_FAIL(id,k) {++g_aot4R[id][2+(k)];return false;}
#define AOT4_PRESTALL(id) (++g_aot4R[id][11])
#else
#define AOT4_COUNT(k) ((void)0)
#define AOT4_CYC(n) ((void)0)
#define AOT4_RCALL(id) ((void)0)
#define AOT4_RENTER(id) ((void)0)
#define AOT4_FAIL(id,k) return false;
#define AOT4_PRESTALL(id) ((void)0)
#endif
#if defined(AOT5_RSTATS)   // lane aot5 profile (standalone harness only): chain entries at non-region pcs
void aot5Erec(int img,uint32_t pc,uint32_t pc0);
#define AOT5_EREC(img,pc0) (v.m_state.pc!=(pc0)?aot5Erec(img,v.m_state.pc,pc0):(void)0)
#else
#define AOT5_EREC(img,pc0) ((void)0)
#endif
// Commit calendar events already pending at region entry, in cycle order, up to target (inclusive).
// Region events are never in the calendar while a region runs, so only older events are committed here.
inline void aot4Drain(AotVU& v,uint64_t target){
 while(v.m_commitCycles){
  const uint64_t upcoming=std::rotr(v.m_commitCycles,int((v.m_cycle+1u)&63u));
  const uint64_t next=v.m_cycle+1u+vuCtz64(upcoming);
  if(next>target)break;
  v.m_cycle=next;v.commitReadyPipelines();
 }
}
inline float aot4OldP(bool a,uint64_t ar,float av,bool b,uint64_t br,float bv,float p,uint64_t t){
 uint64_t best=0;bool have=false;
 if(a&&ar<=t){p=av;best=ar;have=true;}
 if(b&&br<=t&&(!have||br>best))p=bv;
 return p;
}
struct VectorResult4 {__m128 value;uint32_t zero,sign,under,over,extra,cur;};
// vuNormalizeOperand / aotNormalizeOperand, bit for bit, always inlined
__forceinline float aot4Norm1(float value){
 uint32_t b;std::memcpy(&b,&value,4);const uint32_t e=b&0x7f800000u;
 if(e==0)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;
 std::memcpy(&value,&b,4);return value;
}
// Round 6: unsigned magnitude range test, normal [min,max] or exact signed zero.
__forceinline __m128i aot6NormBad(__m128 v,unsigned vuLanes){
 const __m128i b=_mm_and_si128(_mm_castps_si128(v),_mm_set1_epi32(0x7FFFFFFF));
 const __m128i nonzero=_mm_cmpgt_epi32(b,_mm_setzero_si128());
 const __m128i small=_mm_cmpgt_epi32(_mm_set1_epi32(0x00800000),b);
 const __m128i large=_mm_cmpgt_epi32(b,_mm_set1_epi32(0x7F7FFFFF));
 return _mm_and_si128(_mm_or_si128(_mm_and_si128(nonzero,small),large),vuLaneMask(kVuRev4[vuLanes&15u]));
}
__forceinline bool aot4NormOk(__m128 v,unsigned vuLanes){
 // vuNormalize4 is the identity on a lane iff its exponent is 1..254 or it is an exact +-0
 const __m128i b=_mm_castps_si128(v);
 const __m128i e=_mm_and_si128(b,_mm_set1_epi32(0x7F800000));
 const __m128i m=_mm_and_si128(b,_mm_set1_epi32(0x007FFFFF));
 const __m128i den=_mm_andnot_si128(_mm_cmpeq_epi32(m,_mm_setzero_si128()),_mm_cmpeq_epi32(e,_mm_setzero_si128()));
 const __m128i bad=_mm_or_si128(den,_mm_cmpeq_epi32(e,_mm_set1_epi32(0x7F800000)));
 return (static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(bad)))&kVuRev4[vuLanes&15u])==0;
}
// ---- lane aot5 (round 5) helpers
// Packed FMAC lane classes, component order (bit c = component c, x = 0): zero[3:0] sign[7:4] under[11:8] over[15:12]
// (zero includes underflow, as VectorResult.zero). x = the product's classes for MADD/MSUB (sticky only), same packing.
struct R5 {__m128 v;uint32_t m,x;};
// Round 6: reverse bits within each class nibble without four dependent table loads.
__forceinline uint32_t aot5Mac(uint32_t m){
 m=((m&0x5555u)<<1)|((m>>1)&0x5555u);
 return ((m&0x3333u)<<2)|((m>>2)&0x3333u);
}
// Round 6: reduce four lane-class nibbles without four scalar test/set chains.
__forceinline uint32_t aot5Cur(uint32_t m){
 uint32_t t=m|(m>>1);t|=t>>2;t&=0x1111u;t|=t>>3;t|=t>>6;return t&15u;
}
// Cold path: flags only, packed in one 64-bit return value (m | x<<32). The value is rebuilt by the caller exactly as
// vectorCold builds it: dest lanes classed zero/under -> signed zero, over -> signed FLT_MAX, else the float result f.
__declspec(noinline) inline uint64_t aot5Cold(__m128 s,__m128 t,__m128 a,__m128 f,__m128 product,uint8_t kind,uint8_t src,uint32_t destBits){
 const VectorResult c=vectorCold(s,t,a,f,product,kind,src,destBits);
 const uint32_t m=c.zero|(c.sign<<4)|(c.under<<8)|(c.over<<12);
 const uint32_t x=(c.extra&1u)|((c.extra&2u)<<3)|((c.extra&4u)<<6)|((c.extra&8u)<<9);
 return uint64_t(m)|(uint64_t(x)<<32);
}
__forceinline __m128 aot5ColdValue(__m128 f,uint32_t m){
 const __m128i zu=vuLaneMask(m&15u),ov=vuLaneMask((m>>12)&15u);
 const __m128i sg=_mm_and_si128(vuLaneMask((m>>4)&15u),_mm_set1_epi32(static_cast<int>(0x80000000u)));
 __m128i v=_mm_andnot_si128(_mm_or_si128(zu,ov),_mm_castps_si128(f));
 v=_mm_or_si128(v,_mm_and_si128(zu,sg));
 v=_mm_or_si128(v,_mm_and_si128(ov,_mm_or_si128(sg,_mm_set1_epi32(0x7F7FFFFF))));
 return _mm_castsi128_ps(v);
}
// vuFloatToInt on 4 lanes, bit for bit: scaled = double(v)*SCALE (exact: SCALE is 2^0/4/12/15); >= INT_MAX -> INT_MAX,
// <= INT_MIN -> INT_MIN, else truncation. Clamping in double then cvttpd gives the same three cases.
template<int SCALE> __forceinline __m128 aot5Ftoi(__m128 v){
 const __m128d sc=_mm_set1_pd(double(SCALE)),hi=_mm_set1_pd(2147483647.0),lo=_mm_set1_pd(-2147483648.0);
 const __m128d a=_mm_max_pd(_mm_min_pd(_mm_mul_pd(_mm_cvtps_pd(v),sc),hi),lo);
 const __m128d b=_mm_max_pd(_mm_min_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(v,v)),sc),hi),lo);
 return _mm_castsi128_ps(_mm_unpacklo_epi64(_mm_cvttpd_epi32(a),_mm_cvttpd_epi32(b)));
}
// vuMinMax on 4 lanes, bit for bit: chooseLeft = reverse ? (max ? a<b : a>b) : (max ? a>b : a<b), reverse = a<0 && b<0
template<bool MAXIMUM> __forceinline __m128 aot5MinMax(__m128 l,__m128 r){
 const __m128i a=_mm_castps_si128(l),b=_mm_castps_si128(r);
 const __m128i gt=_mm_cmpgt_epi32(a,b),lt=_mm_cmpgt_epi32(b,a);
 const __m128i rev=_mm_srai_epi32(_mm_and_si128(a,b),31);
 const __m128i left=MAXIMUM?_mm_or_si128(_mm_and_si128(rev,lt),_mm_andnot_si128(rev,gt)):_mm_or_si128(_mm_and_si128(rev,gt),_mm_andnot_si128(rev,lt));
 return _mm_blendv_ps(r,l,_mm_castsi128_ps(left));
}
// Exact FMAC (same results/flags as the round-3 vectorFmac): fast-path test rewritten as integer range compares:
//   single op:  exp(r) in [1,254] && |r| != FLT_MAX   <=>  0x007FFFFF < |r| < 0x7F7FFFFF
//   product-sum: exp(r) in [2,253] <=> 0x00FFFFFF < |r| < 0x7F000000; exp(p) in [1,254] && |p| != FLT_MAX <=> 0x007FFFFF < |p| < 0x7F7FFFFF;
//                exp(p) <= exp(r)+21 <=> |p| < (exp(r)+22)<<23 <=> |p| - 0x0B000000 < (|r| & 0x7F800000)  (no int32 overflow)
// MASK=true zeroes the non-dest lanes of the operands (their results are discarded by the merge; avoids denormal assists).
template<uint32_t U,bool NS,bool NT,bool NA,bool MASK>
__forceinline R5 vectorFmac5(__m128 rawS,__m128 rawT,__m128 rawA,float rawQ,float rawI){
 VuFmacKind kind;VuFmacSrc src;uint8_t sel=0;bool toAcc=false;vuDecodeFmac(U,kind,src,sel,toAcc);
 constexpr uint32_t dest=(U>>21)&15u;constexpr uint32_t destBits=(dest&8u?1u:0u)|(dest&4u?2u:0u)|(dest&2u?4u:0u)|(dest&1u?8u:0u);
 constexpr unsigned BC=U&3u;
 __m128 s=NS?rawS:vuNormalize4(rawS);__m128 t;
 switch(src){
 case VuSrcBc:{const __m128 b=_mm_shuffle_ps(rawT,rawT,_MM_SHUFFLE(BC,BC,BC,BC));t=NT?b:vuNormalize4(b);break;}
 case VuSrcQ:t=_mm_set1_ps(rawQ);break;
 case VuSrcI:t=_mm_set1_ps(rawI);break;
 case VuSrcVt:t=NT?rawT:vuNormalize4(rawT);break;
 default:{const __m128 vt=NT?rawT:vuNormalize4(rawT);s=_mm_shuffle_ps(s,s,_MM_SHUFFLE(3,0,2,1));t=_mm_shuffle_ps(vt,vt,_MM_SHUFFLE(3,1,0,2));break;}
 }
 const bool productSum=kind==VuMadd||kind==VuMsub;
 if constexpr(MASK&&destBits!=15u){const __m128 dm=_mm_castsi128_ps(vuLaneMask(destBits));s=_mm_and_ps(s,dm);t=_mm_and_ps(t,dm);}
 __m128 a=_mm_setzero_ps(),product=_mm_setzero_ps(),f;
 switch(kind){
 case VuAdd:f=_mm_add_ps(s,t);break;
 case VuSub:f=_mm_sub_ps(s,t);break;
 case VuMul:f=_mm_mul_ps(s,t);break;
 default:
  a=NA?rawA:vuNormalize4(rawA);
  if constexpr(MASK&&destBits!=15u)a=_mm_and_ps(a,_mm_castsi128_ps(vuLaneMask(destBits)));
  product=_mm_mul_ps(s,t);f=kind==VuMadd?_mm_add_ps(a,product):_mm_sub_ps(a,product);break;
 }
 if(src==VuSrcCross)f=_mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(f),_mm_set_epi32(0,-1,-1,-1)));
 const __m128i rb=_mm_and_si128(_mm_castps_si128(f),_mm_set1_epi32(0x7FFFFFFF));
 __m128i ok;
 if(!productSum)ok=_mm_and_si128(_mm_cmpgt_epi32(rb,_mm_set1_epi32(0x007FFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF),rb));
 else{
  const __m128i pb=_mm_and_si128(_mm_castps_si128(product),_mm_set1_epi32(0x7FFFFFFF));
  ok=_mm_and_si128(_mm_cmpgt_epi32(rb,_mm_set1_epi32(0x00FFFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F000000),rb));
  ok=_mm_and_si128(ok,_mm_and_si128(_mm_cmpgt_epi32(pb,_mm_set1_epi32(0x007FFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF),pb)));
  ok=_mm_and_si128(ok,_mm_cmpgt_epi32(_mm_and_si128(rb,_mm_set1_epi32(0x7F800000)),_mm_sub_epi32(pb,_mm_set1_epi32(0x0B000000))));
 }
 if(_mm_testc_si128(ok,vuLaneMask(destBits))){
  R5 r;r.v=f;r.m=(static_cast<uint32_t>(_mm_movemask_ps(f))&destBits)<<4;
  r.x=productSum?((static_cast<uint32_t>(_mm_movemask_ps(product))&destBits)<<4):0u;return r;
 }
 if(src!=VuSrcCross){
  // exact-zero lanes (see aot5_patch_p2a.py): z = lanes whose exact result is provably the float result and normal-or-zero
  const __m128i zero=_mm_setzero_si128();
  __m128i z;
  if(kind==VuAdd||kind==VuSub)z=_mm_cmpeq_epi32(rb,zero);
  else{
   const __m128i sa=_mm_and_si128(_mm_castps_si128(s),_mm_set1_epi32(0x7FFFFFFF)),ta=_mm_and_si128(_mm_castps_si128(t),_mm_set1_epi32(0x7FFFFFFF));
   z=_mm_or_si128(_mm_cmpeq_epi32(sa,zero),_mm_cmpeq_epi32(ta,zero));   // exact zero product (operands are normal-or-zero)
  }
  if(_mm_testc_si128(_mm_or_si128(ok,z),vuLaneMask(destBits))){
   R5 r;r.v=f;
   r.m=(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(rb,zero))))&destBits)|((static_cast<uint32_t>(_mm_movemask_ps(f))&destBits)<<4);
   if(productSum){
    const __m128i pb=_mm_and_si128(_mm_castps_si128(product),_mm_set1_epi32(0x7FFFFFFF));
    r.x=(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(pb,zero))))&destBits)|((static_cast<uint32_t>(_mm_movemask_ps(product))&destBits)<<4);
   }else r.x=0u;
   return r;
  }
 }
 const uint64_t mx=aot5Cold(s,t,a,f,product,kind,src,destBits);
 R5 r;r.m=uint32_t(mx);r.x=uint32_t(mx>>32);r.v=aot5ColdValue(f,r.m);return r;
}
// runFast's per-cycle XGKICK stall (ready = m_cycle+1 while PATH1 is active), in bulk: step to the cycle where the
// kick can next change state (end of its current tag), never past end-1 (runFast hands back when ready >= end).
// Returns false if the budget end was reached with the kick still active (round 3 continues the stall from there).
__declspec(noinline) inline bool aot5StallXgkick(AotVU& v,uint64_t end){
 v.m_viBranchBackupValid=false;   // a stall expires the integer-branch bypass
 while(v.m_xgkick.active){
  if(v.m_cycle+1u>=end)return false;
  const uint32_t qwords=v.m_xgkick.currentTagEnd?(v.m_xgkick.currentTagEnd-v.m_xgkick.copiedBytes)/16u:1u;
  const uint64_t need=uint64_t(qwords)*2u;
  const uint64_t step=need>v.m_xgkick.cycleCredit?need-v.m_xgkick.cycleCredit:1u;
  uint64_t next=v.m_cycle+step;
  if(next>end-1u)next=end-1u;
  v.advanceTo(next);
 }
 return true;
}
__forceinline void aot4Store(uint8_t* data,uint32_t addr,const uint32_t* words,uint8_t dest){
 const __m128i mask=vuLaneMask(kVuRev4[dest]);
 const __m128i prior=_mm_loadu_si128(reinterpret_cast<const __m128i*>(data+addr));
 const __m128i value=_mm_loadu_si128(reinterpret_cast<const __m128i*>(words));
 _mm_storeu_si128(reinterpret_cast<__m128i*>(data+addr),_mm_or_si128(_mm_andnot_si128(mask,prior),_mm_and_si128(mask,value)));
}
// In-flight FMAC flag entry (updateFmacFlags / fastFmacCommit layout), ready at issue+4.
__declspec(noinline) inline void aot4PushFlag(AotVU& v,uint64_t issue,uint32_t mac,uint32_t cur,uint32_t extra){
 for(auto& e:v.m_flagPipeline)if(!e.valid){
  e.readyCycle=issue+AotVU::kFmacLatency;e.issueCycle=issue;e.mac=mac;e.status=cur;e.extraSticky=extra;e.clip=0u;
  e.valid=true;e.writesMac=true;e.writesStatus=true;e.writesSticky=false;e.writesClip=false;
  v.scheduleCommit(e.readyCycle,static_cast<uint32_t>(&e-v.m_flagPipeline.data()));return;}
 v.reportReservedInstruction(true,0xFFFFFFFFu);
}
__declspec(noinline) inline void aot4PushClip(AotVU& v,uint64_t issue,uint32_t clip){
 for(auto& e:v.m_flagPipeline)if(!e.valid){
  e={};e.valid=true;e.issueCycle=issue;e.readyCycle=issue+AotVU::kFmacLatency;e.clip=clip;e.writesClip=true;
  v.scheduleCommit(e.readyCycle,static_cast<uint32_t>(&e-v.m_flagPipeline.data()));return;}
 v.reportReservedInstruction(true,0xFFFFFFFDu);
}
__declspec(noinline) inline void aot4PushP(AotVU& v,uint64_t ready,float value){
 for(auto& e:v.m_efu)if(!e.valid){
  e.valid=true;e.readyCycle=ready;e.value=value;
  v.scheduleCommit(ready,9u+static_cast<uint32_t>(&e-v.m_efu.data()));return;}
 v.reportReservedInstruction(false,0xFFFFFFF9u);
}

// vectorFmac4: copy of the round-3 vectorFmac (vector_template.inc) with force-inlined scalar normalisation.
template<uint32_t U,bool NS,bool NT,bool NA>
__forceinline VectorResult4 vectorFmac4(__m128 rawS,__m128 rawT,__m128 rawA,float rawQ,float rawI){
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
        t = _mm_set1_ps((NT?vectorLane(rawT,sel&3u):aot4Norm1(vectorLane(rawT,sel&3u))));
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
            return {f,0u,sign,0u,0u,extraSticky,sign?2u:0u};
        }
    }

    // Cold operand classes reuse the shipping exact SIMD/double helper.
    {const VectorResult c=vectorCold(s,t,a,f,product,kind,src,destBits);return {c.value,c.zero,c.sign,c.under,c.over,c.extra,(c.zero?1u:0u)|(c.sign?2u:0u)|(c.under?4u:0u)|(c.over?8u:0u)};}
}

