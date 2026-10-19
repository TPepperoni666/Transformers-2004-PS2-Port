#include <windows.h>
#include <stdlib.h>
// Transformers port: export the worktree decoder, never execute the game.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <cstdio>
#include <fstream>
#include <vector>
void GS::processGIFPacket(const uint8_t*,uint32_t){}
void PS2Memory::submitGifPacket(GifPathId,const uint8_t*,uint32_t,bool,bool){}
void usage(const VU1Interpreter::InstructionUsage& u) {
 printf("{\"read\":[[%u,%u],[%u,%u]],\"count\":%u,\"write\":[%u,%u],\"vr\":%u,\"vw\":%u,\"ar\":%u,\"aw\":%u,\"lat\":%u,\"vfl\":%u,\"vil\":%u,\"pipe\":%u,\"wq\":%u,\"wp\":%u,\"rc\":%u,\"wc\":%u,\"delay\":%u,\"reserved\":%u}",u.vfRead[0].reg,u.vfRead[0].lanes,u.vfRead[1].reg,u.vfRead[1].lanes,u.vfReadCount,u.vfWrite.reg,u.vfWrite.lanes,u.viRead,u.viWrite,u.accRead,u.accWrite,u.latency,u.vfLatency,u.viLatency,u.pipeline,u.waitQ,u.waitP,u.readsClip,u.writesClip,u.delaysNextBranchRead,u.reserved);
}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
 _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
if(argc!=2)return 2;std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>code(16384);f.read((char*)code.data(),16384);if(!f)return 2;VU1Interpreter v;puts("[");for(unsigned pc=0;pc<16384;pc+=8){auto d=v.decodeInstructionPair(code.data(),pc);printf("{\"pc\":%u,\"lo\":%u,\"up\":%u,\"l\":",pc,d.lower,d.upper);usage(d.lowerUsage);printf(",\"u\":");usage(d.upperUsage);printf(",\"i\":%u,\"e\":%u,\"m\":%u,\"d\":%u,\"t\":%u,\"shadow\":%u,\"suppress\":%u}%s\n",d.iBit,d.eBit,d.mBit,d.dBit,d.tBit,d.upperVfShadowReg,d.suppressedLowerVf,pc==16376?"":",");}puts("]");}
