#include "api.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <cstring>
#include <windows.h>
Output reference(uint8_t* code,uint8_t*data,const uint8_t*initial,const uint32_t*h,uint32_t budget){
 static thread_local VU1Interpreter v;static thread_local PS2Memory m;static thread_local std::array<uint8_t,16384> stable{};
 if(!m.vu1Code||memcmp(code,stable.data(),16384)){memcpy(stable.data(),code,16384);++m.gen1;}m.vu1Code=stable.data();
 v.resetScheduler();v.m_cycle=0;memcpy(&v.state(),initial,664);v.state().cycles=0;GS g;
 ULONG64 start,end;QueryThreadCycleTime(GetCurrentThread(),&start);
 if(h[0]&0x80000000)v.resume(stable.data(),16384,data,16384,g,&m,h[1],h[2],budget);else v.execute(stable.data(),16384,data,16384,g,&m,h[0],h[1],h[2],budget);
 QueryThreadCycleTime(GetCurrentThread(),&end);Output o;o.hostCycles=end-start;memcpy(o.state.data(),&v.state(),664);return o;
}
