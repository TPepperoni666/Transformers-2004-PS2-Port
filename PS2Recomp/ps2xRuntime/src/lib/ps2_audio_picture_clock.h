#pragma once
#include "ps2_audio_present_clock.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
// TEMP(transformers): retain bounded decoded-image identities for the host film clock.
// GetPicture precedes the visible image by multiple guest frames. Compensation must anchor to the actual image,
// not to scanout arrival or the latest GetPicture call. No file IO, guest writes, or guest timing changes.
namespace ps2AudioPictureClock {
using Thumb=std::array<float,768>;
struct Picture { Thumb thumb{}; int64_t us=0; uint64_t tick=0; uint32_t frame=0,mpeg=0; };
inline std::array<Picture,16> pictures{};
inline size_t count=0,next=0;
inline std::mutex mutex;
inline int64_t nowUs() {return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline Thumb thumbnail(const uint8_t* rgba,uint32_t w,uint32_t h,size_t stride,unsigned crop) {
    Thumb a{};float mean=0;
    const uint32_t y0=crop==0?0:crop==1?h/14:h/7,hh=h-2*y0;
    for(uint32_t y=0;y<24;++y)for(uint32_t x=0;x<32;++x) {
        unsigned sum=0;
        for(unsigned sy=0;sy<2;++sy)for(unsigned sx=0;sx<2;++sx) {
            const uint32_t px=(x*4+sx*2+1)*w/128,py=y0+(y*4+sy*2+1)*hh/96;
            const uint8_t* p=rgba+size_t(py)*stride+size_t(px)*4;
            sum+=(77u*p[0]+150u*p[1]+29u*p[2]+128u)/256u;
        }
        a[y*32+x]=float(sum/4);mean+=a[y*32+x];
    }
    mean/=768;float norm=0;for(float& v:a) {v-=mean;norm+=v*v;}
    norm=std::sqrt(norm);if(norm>1e-6f)for(float& v:a)v/=norm;
    return a;
}
inline void record(const uint8_t* rgba,uint32_t w,uint32_t h,uint64_t tick,uint32_t frame,uint32_t mpeg) {
    if(!ps2AudioPresentClock::enabled())return;
    Picture p{thumbnail(rgba,w,h,size_t(w)*4,0),nowUs(),tick,frame,mpeg};
    std::lock_guard<std::mutex> lock(mutex);
    if(frame==0) {count=0;next=0;}
    pictures[next]=p;next=(next+1)%pictures.size();count=std::min(count+1,pictures.size());
}
inline int64_t anchor(const uint8_t* rgba,uint32_t w,uint32_t h,size_t stride,uint64_t tick,int64_t fallback) {
    std::array<Thumb,3> images{thumbnail(rgba,w,h,stride,0),thumbnail(rgba,w,h,stride,1),thumbnail(rgba,w,h,stride,2)};
    float best=.95f;int64_t selected=fallback;
    std::lock_guard<std::mutex> lock(mutex);
    for(size_t i=0;i<count;++i) {
        const auto& p=pictures[i];if(p.tick+8<tick || p.tick>tick+2)continue;
        for(const auto& a:images) {float score=0;for(size_t k=0;k<a.size();++k)score+=a[k]*p.thumb[k];
            if(score>best) {best=score;selected=p.us;}}
    }
    return selected;
}
}
