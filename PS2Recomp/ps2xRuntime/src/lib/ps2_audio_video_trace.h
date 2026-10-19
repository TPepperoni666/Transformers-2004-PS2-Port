#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
// TEMP(transformers): diagnostic movie image identity, independent of GetPicture->next-tick guessing.
// Decode and FIELD60 selection log 32x24 luminance thumbnails. Presentation logs full height plus 1/14 and 1/7
// symmetric letterbox crops. Swap-return time is logged separately by ps2_runtime.cpp.
namespace ps2AudioVideoTrace {
#pragma pack(push,1)
struct Record { int64_t us; uint64_t tick; int64_t pts; uint32_t frame,mpeg,width,height,type; std::array<uint8_t,2304> pixels{}; };
#pragma pack(pop)
inline void log(uint32_t type,const uint8_t* rgba,uint32_t width,uint32_t height,size_t stride,
                uint64_t tick,int64_t pts,uint32_t frame,uint32_t mpeg) {
    static FILE* file=[] {const char* p=std::getenv("PS2X_AUDIO_VIDEO_TRACE");return p&&*p?std::fopen(p,"wb"):nullptr;}();
    if(!file || !rgba || !width || !height)return;
    Record r{};r.us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    r.tick=tick;r.pts=pts;r.frame=frame;r.mpeg=mpeg;r.width=width;r.height=height;r.type=type;
    for(unsigned crop=0;crop<(type==0?1u:3u);++crop) {
        const uint32_t y0=crop==0?0:crop==1?height/14:height/7, hh=height-2*y0;
        for(uint32_t y=0;y<24;++y)for(uint32_t x=0;x<32;++x) {
            unsigned sum=0;
            for(unsigned sy=0;sy<2;++sy)for(unsigned sx=0;sx<2;++sx) {
                const uint32_t px=(x*4+sx*2+1)*width/128,py=y0+(y*4+sy*2+1)*hh/96;
                const uint8_t* p=rgba+size_t(py)*stride+size_t(px)*4;
                sum+=(77u*p[0]+150u*p[1]+29u*p[2]+128u)/256u;
            }
            r.pixels[crop*768+y*32+x]=uint8_t(sum/4);
        }
    }
    static std::mutex mutex;std::lock_guard<std::mutex> lock(mutex);std::fwrite(&r,sizeof(r),1,file);std::fflush(file);
}
}
