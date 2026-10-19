// TEMP(transformers): offline binary trace replay driver, never runs guest code.
#include "emulator/core/iop_spu2.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
using ps2x::iop::detail::Spu2Device;
static uint64_t le(const uint8_t* b,int n) {uint64_t v=0;for(int i=0;i<n;i++)v|=uint64_t(b[i])<<(8*i);return v;}
int main(int argc,char** argv) {
    if(argc!=5) {std::cerr<<"trace raw.pcm startSeconds durationSeconds\n";return 1;}
    std::ifstream f(argv[1],std::ios::binary);std::ofstream out(argv[2],std::ios::binary);
    uint8_t h[16];if(!f.read(reinterpret_cast<char*>(h),16)||std::memcmp(h,"SPU2TRC1",8))return 2;
    uint64_t first=uint64_t(std::stod(argv[3])*48000),last=first+uint64_t(std::stod(argv[4])*48000);
    uint64_t emitted=0,clock=0;Spu2Device spu;spu.setClock(&clock);std::vector<int16_t> samples(8192);
    auto advance=[&](uint64_t to) {
        while(clock<to) {
            clock=std::min(to,clock+uint64_t(2048*768));spu.advanceTo(clock);
            size_t n=spu.drainSamples(samples.data(),4096);
            for(size_t i=0;i<n;i++,emitted++)if(emitted>=first&&emitted<last) {
                uint8_t b[]{uint8_t(samples[i*2]),uint8_t(samples[i*2]>>8),uint8_t(samples[i*2+1]),uint8_t(samples[i*2+1]>>8)};
                out.write(reinterpret_cast<char*>(b),4);
            }
        }
    };
    while(f.read(reinterpret_cast<char*>(h),16)) {
        uint64_t cycle=le(h,8);unsigned type=h[8],core=h[9],offset=unsigned(le(h+10,2)),val=unsigned(le(h+12,4));
        if(cycle>last*768)break;
        advance(cycle);
        if(type==1)spu.write16(offset,uint16_t(val));
        else if(type==0)spu.reset();
        else if(type==3||type==4||type==5) {
            uint8_t ext[16];if(!f.read(reinterpret_cast<char*>(ext),16))return 3;
            unsigned addr=unsigned(le(ext,4));std::vector<uint8_t> data(val);
            if(!f.read(reinterpret_cast<char*>(data.data()),val))return 4;f.seekg((-val)&15,std::ios::cur);
            if(type==3||type==5) {
                // Recorded TSA is informational in ADMA mode; device maps PCM to MEMIN.
                spu.write16(core*0x400+0x1a8,uint16_t(addr/2>>16));spu.write16(core*0x400+0x1aa,uint16_t(addr/2));
                spu.onDma(core,true,0,val,data.data(),data.size(),nullptr);
            }
        }
    }
    advance(last*768);std::cout<<"replayed frames="<<emitted<<" saved="<<last-first<<'\n';
}
