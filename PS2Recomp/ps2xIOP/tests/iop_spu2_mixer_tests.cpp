#include "emulator/core/iop_spu2.h"
#include "emulator/core/iop_memory.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>
#include <cstdlib>
using namespace ps2x::iop::detail;
static int failures=0;
static void check(bool x,const char* message) {if(!x) {std::cerr<<message<<'\n';++failures;}}
static void setup(Spu2Device& s,int c=0) {
    s.write16(0x19a,0xc000);s.write16(0x59a,0xc000);
    s.write16(0x760,0x3fff);s.write16(0x762,0x3fff);s.write16(0x788,0x3fff);s.write16(0x78a,0x3fff);
    s.write16(0x790,0x7fff);s.write16(0x792,0x7fff);
    s.write16(0x198,0xcc0);s.write16(0x598,0xccc);
    s.write16(c*0x400+0x188,1);s.write16(c*0x400+0x190,1);
    s.write16(c*0x400,0x3fff);s.write16(c*0x400+2,0x3fff);
    s.write16(c*0x400+4,0x1000);s.write16(c*0x400+6,0xf);s.write16(c*0x400+8,0x5fc0);
    s.write16(c*0x400+0x1c2,0x2800);s.write16(c*0x400+0x1c6,0x2800);
}
static void upload(Spu2Device& s,int core,const std::vector<uint8_t>& data,uint32_t dest=0x5000) {
    s.write16(core*0x400+0x1a8,uint16_t(dest/2>>16));s.write16(core*0x400+0x1aa,uint16_t(dest/2));
    s.onDma(core,true,0,uint32_t(data.size()),data.data(),data.size(),nullptr);
}
int main() {
    // Independent ADPCM expectations: signed low nibble first, predictor carries across blocks, saturation.
    uint8_t block[16]{};block[0]=12;std::memset(block+2,0x87,14);int32_t h1=0,h2=0;int16_t pcm[28];
    Spu2Mixer::decode(block,h1,h2,pcm);check(pcm[0]==7&&pcm[1]==-8&&pcm[27]==-8,"ADPCM signed nibble order");
    block[0]=0x1c;std::memset(block+2,0,14);h1=64;h2=0;
    Spu2Mixer::decode(block,h1,h2,pcm);check(pcm[0]==60&&pcm[1]==56&&pcm[2]==53,"filter-one predictor golden vector");
    block[0]=0x10;std::memset(block+2,0x77,14);h1=32767;h2=0;
    Spu2Mixer::decode(block,h1,h2,pcm);check(pcm[0]==32767&&pcm[27]==32767,"predictor clips before history feedback");
    for(int core=0;core<2;core++) {
        uint64_t clock=0;Spu2Device s;s.setClock(&clock);setup(s,core);
        std::vector<uint8_t> b(16,0x11);b[0]=0;b[1]=7;upload(s,core,b);
        s.write16(core*0x400+0x1a0,1);
        clock=767;check(s.read16(core*0x400+10)==0,"subsample cycles do not advance envelope");
        clock=768;check(s.read16(core*0x400+10)==14336,"attack independent expected first step");
        clock=4*768;s.advanceTo(clock);check(s.read16(core*0x400+10)==32767,"attack saturation");
        clock=29*768;s.advanceTo(clock);check(s.read16(core*0x400+0x340)&1,"loop ENDX after block consumption");
        check(s.read16(core*0x400+0x1c6)==0x2800,"loop start readback");
        s.write16(core*0x400+0x1a0,1);check(!(s.read16(core*0x400+0x340)&1),"KON clears ENDX");
        s.write16(core*0x400+0x1a4,1);clock+=4*768;s.advanceTo(clock);
        check(s.read16(core*0x400+10)==0,"linear release reaches silence");
        int16_t audio[100]{};check(s.drainSamples(audio,50)==33,"exact guest sample clock and drain count");
        check(audio[10]>1000,"voice reaches output through both core paths");
    }
    {
        uint64_t clock=0;Spu2Device s;s.setClock(&clock);setup(s);s.write16(0x1b0,1);
        s.write16(0x76c,0x7fff);s.write16(0x76e,0x7fff);
        std::vector<uint8_t> b(1024);for(int i=0;i<256;i++) {b[i*2]=0xe8;b[i*2+1]=3;b[512+i*2]=0x30;b[513+i*2]=0xf8;}
        upload(s,0,b,0);check(s.dmaCompletionDelay(0,64)==256*768,"ADMA completion paced by 256 samples");
        check((s.read16(0x344)&0x480)==0x400,"ADMA busy before consumption");
        clock=256*768;s.advanceTo(clock);check((s.read16(0x344)&0x480)==0x80,"ADMA ready only after consumption");
        std::vector<int16_t> out(512);check(s.drainSamples(out.data(),256)==256,"ADMA drain count");
        check(out[0]==996&&out[1]==-2000,"ADMA split stereo, signed AVOL/BVOL, mix ordering");
    }
    {
        uint64_t clock=0;Spu2Device s;s.setClock(&clock);setup(s,1);
        // Highest valid voice exercises the high KON/ENDX half and non-repeating termination.
        for(unsigned o=0;o<10;o+=2)s.write16(0x400+23*16+o,s.read16(0x400+o));
        s.write16(0x400+0x1c0+23*12+2,0x2800);
        s.write16(0x58a,0x80);s.write16(0x592,0x80);
        std::vector<uint8_t> b(16,0x11);b[0]=0;b[1]=1;upload(s,1,b);
        s.write16(0x5a2,0x80);clock=29*768;s.advanceTo(clock);
        check(s.read16(0x742)&0x80,"voice 23 nonrepeat ENDX");
        check(s.read16(0x400+23*16+10)==0,"nonrepeat end mutes ENVX");
        s.write16(0x742,0);check(s.read16(0x742)==0,"ENDX half clears on zero write");
        clock=0;s.reset();setup(s);s.write16(0x1a0,1);clock=768;
        check(s.read16(10)==14336,"clock reset reanchors synthesis");
    }
    {
        Spu2Device s;s.advanceTo(40000ull*768);std::vector<int16_t> out(80000);
        check(s.drainSamples(out.data(),40000)==32768,"ring is bounded and drops newest");
        check(s.drainSamples(out.data(),40000)==0,"ring drains exactly once");
    }
    {
#ifdef _WIN32
        _putenv_s("PS2X_SPU2_TRACE_MAX_MB","1");
#else
        setenv("PS2X_SPU2_TRACE_MAX_MB","1",1);
#endif
        Spu2Device s;check(s.openTrace("spu2_mixer_cap_test.bin"),"open bounded trace");
        std::vector<uint8_t> data(600000,0);upload(s,0,data);upload(s,0,data);s.write16(4,0x1000);s.closeTrace();
        std::ifstream f("spu2_mixer_cap_test.bin",std::ios::binary|std::ios::ate);
        check(f.tellg()==600112,"trace cap stops before whole second payload, never in a record");
#ifdef _WIN32
        _putenv_s("PS2X_SPU2_TRACE_MAX_MB","");
#else
        unsetenv("PS2X_SPU2_TRACE_MAX_MB");
#endif
    }
    std::cout<<(failures?"FAIL":"PASS")<<" mixer tests, failures="<<failures<<'\n';return failures?1:0;
}
