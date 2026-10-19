// TEMP(transformers): optional-package stubs. No game-derived data.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
bool ps2Vu1StaticPrepare(const uint8_t*,unsigned,const PS2Memory*,uint64_t,unsigned){return false;}
bool ps2Vu1StaticRun(int,void*,bool,uint8_t*,uint8_t*,GS&,PS2Memory*,uint32_t,uint32_t,uint32_t,uint32_t,void*){return false;}
uint64_t ps2Vu1StaticFallbackCalls(){return 0;}
bool ps2Vu1StaticStep(void*,uint8_t*,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&){return false;}
