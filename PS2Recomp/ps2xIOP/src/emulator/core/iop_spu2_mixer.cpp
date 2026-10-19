// adapted from KIexster/god-of-war-recomp (GPL-3.0), parts originally by Waffle565 (GPL-3.0)
#include "iop_spu2_mixer.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail {
namespace {
int32_t clip(int64_t x) { return static_cast<int32_t>(std::clamp<int64_t>(x,-32768,32767)); }
int32_t signed16(uint16_t x) { return x<32768?x:static_cast<int32_t>(x)-65536; }
// Defined floor division for negative operands, matching hardware arithmetic shifts.
int32_t shr(int64_t x,unsigned n) {
    return static_cast<int32_t>(x >> n); // C++20: arithmetic right shift, floor toward negative infinity.
}
int32_t scale(int32_t x,int32_t v) { return shr(int64_t{x}*v,15); }
void le16(std::FILE* f,uint16_t v) { uint8_t b[]{uint8_t(v),uint8_t(v>>8)};std::fwrite(b,1,2,f); }
void le32(std::FILE* f,uint32_t v) { le16(f,uint16_t(v));le16(f,uint16_t(v>>16)); }
void wavHeader(std::FILE* f,uint32_t bytes) {
    std::fwrite("RIFF",1,4,f);le32(f,36+bytes);std::fwrite("WAVEfmt ",1,8,f);le32(f,16);
    le16(f,1);le16(f,2);le32(f,48000);le32(f,192000);le16(f,4);le16(f,16);
    std::fwrite("data",1,4,f);le32(f,bytes);
}
}
Spu2Mixer::Spu2Mixer(std::vector<uint16_t>& regs,std::vector<uint8_t>& ram):m_regs(regs),m_ram(ram) {}
Spu2Mixer::~Spu2Mixer() { closeWav(); }
void Spu2Mixer::reset(uint64_t cycle) {
    m_voices={};m_pcm={};m_sampleCycle=cycle-cycle%768;
    // Reset is called by the producer; retain queued audio for the consumer.
}
uint32_t Spu2Mixer::address(uint32_t o) const { return (((r(o)&15)<<16)|r(o+2))*2; }
void Spu2Mixer::putAddress(uint32_t o,uint32_t bytes) {
    uint32_t a=(bytes/2)&0xfffff;m_regs[o/2]=uint16_t(a>>16);m_regs[o/2+1]=uint16_t(a);
}
int32_t Spu2Mixer::volume(uint16_t v) {
    // Sweeps are not used in the captured game. A future sweep engine must retain current volume.
    return (v&0x8000)?0:signed16(uint16_t(v*2));
}
void Spu2Mixer::decode(const uint8_t* b,int32_t& h1,int32_t& h2,int16_t* out) {
    static constexpr int f0[]{0,60,115,98,122}, f1[]{0,0,-52,-55,-60};
    unsigned filter=b[0]>>4,shift=b[0]&15;
    int p0=filter<5?f0[filter]:0,p1=filter<5?f1[filter]:0;
    for(unsigned i=0;i<28;i++) {
        int n=(b[2+i/2]>>((i&1)*4))&15;if(n&8)n-=16;
        int s=clip(shr(n*4096,shift)+shr(int64_t{h1}*p0+int64_t{h2}*p1+32,6));
        out[i]=int16_t(s);h2=h1;h1=s;
    }
}
void Spu2Mixer::keyOn(int c,int i) {
    auto& v=m_voices[c][i];v=Voice{};uint32_t o=c*0x400+0x1c0+i*12;
    v.address=address(o)&0x1ffff0;v.loop=address(o+4)&0x1ffff0;v.phase=1;
    m_regs[(c*0x400+0x340+(i>=16?2:0))/2]&=uint16_t(~(1u<<(i%16)));
    putAddress(o+8,v.address+2);
}
void Spu2Mixer::write(uint32_t off,uint16_t value) {
    if(off>=0x760)return;
    int c=off/0x400;uint32_t o=off%0x400;
    if(o==0x1a0||o==0x1a2||o==0x1a4||o==0x1a6) {
        int base=(o&2)?16:0;
        for(int b=0;b<std::min(16,24-base);b++)if(value&(1u<<b)) {
            auto& v=m_voices[c][base+b];
            if(o<0x1a4)keyOn(c,base+b);
            else if(v.phase && v.phase!=4) { v.phase=4;v.counter=0; }
        }
    } else if(o>=0x1c0&&o<0x2e0) {
        int i=(o-0x1c0)/12;uint32_t field=(o-0x1c0)%12;
        if(field==4||field==6) { m_voices[c][i].loop=address(c*0x400+0x1c4+i*12)&0x1ffff0;m_voices[c][i].manualLoop=true; }
        if(field==8||field==10) { auto& v=m_voices[c][i];v.address=address(c*0x400+0x1c8+i*12)&0x1ffff0;v.index=28; }
    } else if(o<0x180&&(o%16)==10) m_voices[c][o/16].env=std::min<int>(value,32767);
    else if(o==0x1b0 && !(value&(1u<<c))) { m_pcm[c]={};m_regs[(c*0x400+0x344)/2]&=~0x400; }
    else if(o==0x19a) {
        auto& status=m_regs[(c*0x400+0x344)/2];
        if(value&0x30)status|=0x80;else if(!(status&0x400))status&=~0x80;
    }
}
void Spu2Mixer::loadBlock(int c,int i) {
    auto& v=m_voices[c][i];
    // ENDX is set at the end of the consumed block, never at initial decode.
    if(v.index>=28&&v.flags&1) {
        m_regs[(c*0x400+0x340+(i>=16?2:0))/2]|=uint16_t(1u<<(i%16));
        v.address=v.loop;
        if(!(v.flags&2)) {v.phase=0;v.env=0;return;}
    }
    uint8_t b[16]{};uint32_t a=v.address&0x1ffff0;
    if(m_ram.size()==0x200000)std::memcpy(b,m_ram.data()+a,16);
    v.flags=b[1];
    if((v.flags&4)&&!v.manualLoop) {v.loop=a;putAddress(c*0x400+0x1c4+i*12,a);}
    decode(b,v.h1,v.h2,v.samples.data());v.address=(a+16)&0x1fffff;v.index-=28;
}
void Spu2Mixer::envelope(int c,int i) {
    auto& v=m_voices[c][i];uint32_t o=c*0x400+i*16;
    uint16_t a=r(o+6),b=r(o+8);unsigned shift=0,st=0;bool exp=false,down=false;
    if(v.phase==2&&v.env<=std::min(32767,((a&15)+1)*2048)) {v.phase=3;v.counter=0;}
    switch(v.phase) {
        case 1:shift=(a>>10)&31;st=(a>>8)&3;exp=(a&0x8000)!=0;break;
        case 2:shift=(a>>4)&15;exp=true;down=true;break;
        case 3:shift=(b>>8)&31;st=(b>>6)&3;exp=(b&0x8000)!=0;down=(b&0x4000)!=0;break;
        case 4:shift=b&31;exp=(b&32)!=0;down=true;break;
        default:v.env=0;return;
    }
    // Maximum attack/sustain/release rate encodings freeze the envelope.
    if((v.phase==4&&shift==31)||((v.phase==1||v.phase==3)&&shift==31&&st==3))return;
    int step=(down?(-8+int(st)):(7-int(st)))*(1<<std::max(0,11-int(shift)));
    uint32_t increment=32768u>>std::max(0,int(shift)-11);
    if(exp&&!down&&v.env>0x6000) {
        if(shift<10)step=shr(step,2);
        else if(shift>=11)increment>>=2;
        else {step=shr(step,1);increment>>=1;}
    }
    if(exp&&down)step=scale(step,v.env);
    v.counter+=std::max(1u,increment);
    if(v.counter<32768)return;
    v.counter=0;v.env=std::clamp(v.env+step,0,32767);
    if(v.phase==1&&v.env==32767) {v.phase=2;v.counter=0;}
    if(v.phase==4&&v.env==0)v.phase=0;
}
bool Spu2Mixer::admaEnabled(int c) const {return (r(c*0x400+0x1b0)&(1u<<c))!=0;}
void Spu2Mixer::cancelAdma(int c) {
    m_pcm[c]={};m_regs[(c*0x400+0x344)/2]&=~0x400;
}
void Spu2Mixer::startAdma(int c,const uint8_t* data,uint32_t bytes) {
    // Sony block transfer layout: successive 256-frame pages, 256 L then 256 R halfwords.
    // Snapshot ownership prevents IOP writes from racing this transfer. Only one DMA/core may be pending.
    bytes&=~1023u;m_pcm[c].data.assign(data,data+bytes);m_pcm[c].frame=0;
    auto& s=m_regs[(c*0x400+0x344)/2];s=uint16_t((s&~0x80)|0x400);
}
uint64_t Spu2Mixer::dmaDelay(int c,uint64_t fallback) const {
    const auto& p=m_pcm[c];
    if(!admaEnabled(c)||p.data.empty())return fallback;
    uint64_t frames=p.data.size()/4;
    // Complete after consumption in guest time, rather than firing every ~2K IOP cycles.
    return frames*768; // caller subtracts sub-sample remainder
}
void Spu2Mixer::tick(int16_t* out) {
    int32_t previousL=0,previousR=0;
    for(int c=0;c<2;c++) {
        uint32_t base=c*0x400;int64_t l=0,rr=0;
        uint32_t ml=r(base+0x188)|(uint32_t(r(base+0x18a))<<16),mr=r(base+0x190)|(uint32_t(r(base+0x192))<<16);
        for(int i=0;i<24;i++) {
            auto& v=m_voices[c][i];uint32_t o=base+i*16;
            m_regs[(o+12)/2]=uint16_t(volume(r(o)));m_regs[(o+14)/2]=uint16_t(volume(r(o+2)));
            if(v.phase) {
                if(v.index>=28)loadBlock(c,i);
                if(v.phase) {
                    int s0=v.samples[v.index],s1=v.samples[std::min(27u,v.index+1)];
                    int s=s0+shr(int64_t(s1-s0)*v.fraction,12);
                    envelope(c,i);s=scale(s,v.env);
                    if(ml&(1u<<i))l+=scale(s,volume(r(o)));
                    if(mr&(1u<<i))rr+=scale(s,volume(r(o+2)));
                    v.fraction+=std::min<unsigned>(r(o+4),0x3fff);v.index+=v.fraction>>12;v.fraction&=4095;
                    // NAX approximation tracks decoded blocks; hardware has a small prefetch FIFO.
                    putAddress(base+0x1c8+i*12,(v.address-16+2+(std::min(v.index,27u)/4)*2)&0x1fffff);
                }
            }
            m_regs[(o+10)/2]=uint16_t(v.env);
        }
        uint16_t mmix=r(base+0x198);l=(mmix&0x800)?clip(l):0;rr=(mmix&0x400)?clip(rr):0;
        uint32_t g=0x760+c*0x28;
        auto& p=m_pcm[c];
        if(admaEnabled(c)&&p.frame<p.data.size()/4) {
            size_t page=p.frame/256,index=p.frame%256;
            size_t at=page*1024+index*2;
            int pl=signed16(uint16_t(p.data[at]|(p.data[at+1]<<8)));
            int pr=signed16(uint16_t(p.data[at+512]|(p.data[at+513]<<8)));
            if(mmix&0x80)l+=scale(pl,signed16(r(g+12)));
            if(mmix&0x40)rr+=scale(pr,signed16(r(g+14)));
            // Mirror consumed input at the hardware MEMIN location (addresses are halfwords).
            if(m_ram.size()==0x200000) {
                uint32_t a=2*(0x2000+c*0x400+(p.frame&511));
                m_ram[a]=uint8_t(pl);m_ram[a+1]=uint8_t(pl>>8);
                m_ram[a+1024]=uint8_t(pr);m_ram[a+1025]=uint8_t(pr>>8);
            }
            if(++p.frame==p.data.size()/4)m_regs[(base+0x344)/2]=uint16_t((r(base+0x344)&~0x400)|0x80);
        }
        if(c==1) {
            if(mmix&8)l+=scale(previousL,signed16(r(g+8)));
            if(mmix&4)rr+=scale(previousR,signed16(r(g+10)));
        }
        previousL=scale(clip(l),volume(r(g)));previousR=scale(clip(rr),volume(r(g+2)));
        // Core enable/mute bits follow documented behaviour; all observed active ATTR writes set both.
        if((r(base+0x19a)&0xc000)!=0xc000)previousL=previousR=0;
        m_regs[(g+16)/2]=uint16_t(volume(r(g)));m_regs[(g+18)/2]=uint16_t(volume(r(g+2)));
    }
    out[0]=int16_t(clip(previousL));out[1]=int16_t(clip(previousR));
}
void Spu2Mixer::advanceTo(uint64_t cycle) {
    if(cycle<m_sampleCycle)return;
    if(!m_wavChecked) {
        m_wavChecked=true;const char* path=std::getenv("PS2X_SPU2_WAV");
        if(path&&*path) {m_wav=std::fopen(path,"wb");if(m_wav)wavHeader(m_wav,0);}
    }
    while(cycle-m_sampleCycle>=768) {m_sampleCycle+=768;int16_t sample[2];tick(sample);emit(sample);}
}
void Spu2Mixer::emit(const int16_t* s) {
    uint64_t w=m_write.load(std::memory_order_relaxed),rpos=m_read.load(std::memory_order_acquire);
    if(w-rpos<RingFrames) {
        m_ring[(w%RingFrames)*2]=s[0];m_ring[(w%RingFrames)*2+1]=s[1];m_write.store(w+1,std::memory_order_release);
    } else m_dropped.fetch_add(1,std::memory_order_relaxed);
    // TEMP(transformers): exact guest-time diagnostic capture, independent of ring overflow.
    if(m_wav&&m_wavFrames<(0xffffffffu-36u)/4u) {
        le16(m_wav,uint16_t(s[0]));le16(m_wav,uint16_t(s[1]));++m_wavFrames;
        // TEMP(transformers): checkpoint once per guest second so timeout captures have a usable header.
        if(m_wavFrames%48000==0) {
            std::fseek(m_wav,0,SEEK_SET);wavHeader(m_wav,uint32_t(m_wavFrames*4));
            std::fseek(m_wav,0,SEEK_END);std::fflush(m_wav);
        }
    }
}
size_t Spu2Mixer::drainSamples(int16_t* out,size_t frames) noexcept {
    if(!out)return 0;
    uint64_t rpos=m_read.load(std::memory_order_relaxed),w=m_write.load(std::memory_order_acquire);
    size_t n=static_cast<size_t>(std::min<uint64_t>(frames,w-rpos));
    for(size_t i=0;i<n;i++) {out[i*2]=m_ring[((rpos+i)%RingFrames)*2];out[i*2+1]=m_ring[((rpos+i)%RingFrames)*2+1];}
    m_read.store(rpos+n,std::memory_order_release);return n;
}
void Spu2Mixer::closeWav() {
    m_wavChecked=true; // TEMP(transformers): never open/reopen capture after its endpoint.
    if(m_wav) {std::fseek(m_wav,0,SEEK_SET);wavHeader(m_wav,uint32_t(m_wavFrames*4));std::fclose(m_wav);m_wav=nullptr;}
}
}
