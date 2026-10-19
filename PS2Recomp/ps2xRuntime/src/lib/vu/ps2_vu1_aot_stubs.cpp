#include <cstdint>
class GS;class PS2Memory;
// Whole-image package absent: static dispatch and interpreter fallback still work.
int ps2Vu1AotIdentify(const uint8_t*,uint32_t,uint32_t){return -1;}
bool ps2Vu1AotRun(int,void*,bool,uint8_t*,uint8_t*,GS&,PS2Memory*,uint32_t,uint32_t,uint32_t,uint32_t,void*){return false;}
extern "C" uint64_t ps2Vu1AotFallbackCalls(){return 0;}
