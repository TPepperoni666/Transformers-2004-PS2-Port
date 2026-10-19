#pragma once
// TEMP(transformers): presentation kernels byte-exact presentation kernels; SSE2 baseline on x64.
#include <emmintrin.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
namespace pdiet {
inline bool rn() { static const bool v=[]{const char *p=std::getenv("PS2X_PDIET_RN");return p && std::atoi(p)!=0;}();return v; }
inline bool gs() { static const bool v=[]{const char *p=std::getenv("PS2X_PDIET_GS");return p && std::atoi(p)!=0;}();return v; }
inline bool lazy() { static const bool v=[]{const char *p=std::getenv("PS2X_SHARP_NATIVE");return p && std::strcmp(p,"lazy")==0;}();return rn() && v; }
inline bool scalar() { static const bool v=[]{const char *p=std::getenv("PS2X_F60_SCALAR");return p && std::atoi(p)!=0;}();return v; }
inline void downRow(const uint8_t *a,const uint8_t *b,uint8_t *o,uint32_t n) {
 uint32_t x=0; const auto z=_mm_setzero_si128(), two=_mm_set1_epi16(2);
 for(;x+2<=n;x+=2) {
  auto A=_mm_loadu_si128((const __m128i*)(a+x*8)), B=_mm_loadu_si128((const __m128i*)(b+x*8));
  auto lo=_mm_add_epi16(_mm_unpacklo_epi8(A,z),_mm_unpacklo_epi8(B,z));
  auto hi=_mm_add_epi16(_mm_unpackhi_epi8(A,z),_mm_unpackhi_epi8(B,z));
  lo=_mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(lo,_mm_srli_si128(lo,8)),two),2);
  hi=_mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(hi,_mm_srli_si128(hi,8)),two),2);
  auto q=_mm_packus_epi16(lo,hi);q=_mm_unpacklo_epi32(q,_mm_srli_si128(q,8));
  _mm_storel_epi64((__m128i*)(o+x*4),q);
 }
 for(;x<n;++x)for(unsigned c=0;c<4;++c)o[x*4+c]=uint8_t((unsigned(a[x*8+c])+a[x*8+4+c]+b[x*8+c]+b[x*8+4+c]+2)>>2);
}
inline void alphaCopy(uint8_t *o,const uint8_t *s,size_t n) {
 size_t i=0;const auto alpha=_mm_set1_epi32(int(0xff000000u));
 for(;i+16<=n;i+=16)_mm_storeu_si128((__m128i*)(o+i),_mm_or_si128(_mm_loadu_si128((const __m128i*)(s+i)),alpha));
 for(;i<n;i+=4){std::memcpy(o+i,s+i,3);o[i+3]=255;}
}
inline bool rgbEqual(const uint8_t *a,const uint8_t *b,size_t n) {
 size_t i=0; const auto rgb=_mm_set1_epi32(0x00ffffff),z=_mm_setzero_si128();
 for(;i+16<=n;i+=16)if(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i*)(a+i)),_mm_loadu_si128((const __m128i*)(b+i))),rgb),z))!=65535)return false;
 for(;i<n;i+=4)if(a[i]!=b[i]||a[i+1]!=b[i+1]||a[i+2]!=b[i+2])return false;return true;
}
inline __m128i delta(__m128i a,__m128i b) {return _mm_or_si128(_mm_subs_epu8(a,b),_mm_subs_epu8(b,a));}
inline void fieldRow(const uint8_t *r,const uint8_t *u,const uint8_t *d,const uint8_t *pr,const uint8_t *pu,const uint8_t *pd,uint8_t *o,uint32_t w,int filter,int T,bool simd) {
 uint32_t x=0;const auto alpha=_mm_set1_epi32(int(0xff000000u)),rgb=_mm_set1_epi32(0x00ffffff),z=_mm_setzero_si128();
 if(simd)for(;x+4<=w;x+=4) {
  size_t i=size_t(x)*4;auto U=_mm_loadu_si128((const __m128i*)(u+i)),D=_mm_loadu_si128((const __m128i*)(d+i));
  auto V=_mm_loadu_si128((const __m128i*)(r+i));auto q=_mm_avg_epu8(U,D);
  if(filter>=2) {
   if(filter!=2)q=_mm_max_epu8(_mm_min_epu8(U,V),_mm_min_epu8(_mm_max_epu8(U,V),D));
   auto mask=z;
   if(pr && T>=0) {
    auto diff=_mm_max_epu8(delta(V,_mm_loadu_si128((const __m128i*)(pr+i))),_mm_max_epu8(delta(U,_mm_loadu_si128((const __m128i*)(pu+i))),delta(D,_mm_loadu_si128((const __m128i*)(pd+i)))));
    auto bad=_mm_and_si128(_mm_subs_epu8(diff,_mm_set1_epi8(char(std::min(T,255)))),rgb);
    bad=_mm_or_si128(bad,_mm_srli_epi32(bad,8));bad=_mm_or_si128(bad,_mm_srli_epi32(bad,16));
    mask=_mm_cmpeq_epi32(_mm_and_si128(bad,_mm_set1_epi32(255)),z);
   }
   q=_mm_or_si128(_mm_and_si128(mask,V),_mm_andnot_si128(mask,q));
  }
  _mm_storeu_si128((__m128i*)(o+i),_mm_or_si128(q,alpha));
 }
 for(;x<w;++x) {
  size_t i=size_t(x)*4;bool still=pr!=nullptr;
  for(int c=0;c<3&&still;++c)still=std::abs(int(r[i+c])-int(pr[i+c]))<=T&&std::abs(int(u[i+c])-int(pu[i+c]))<=T&&std::abs(int(d[i+c])-int(pd[i+c]))<=T;
  for(int c=0;c<3;++c)o[i+c]=(filter>=2&&still)?r[i+c]:filter<=2?uint8_t((unsigned(u[i+c])+d[i+c]+1)>>1):std::max(std::min(u[i+c],r[i+c]),std::min(std::max(u[i+c],r[i+c]),d[i+c]));
  o[i+3]=255;
 }
}
} // namespace pdiet
