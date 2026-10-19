// TEMP(transformers): opt-in cumulative diagnostic counters; OFF for timing.
#include "ps2_vu1_static_api.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
const bool ps2Vu1StaticStatsOn=[] {const char* p=std::getenv("PS2X_VU1_STATIC_STATS");return p&&*p&&std::strcmp(p,"0")!=0;}();
static std::atomic<uint64_t> counts[8]{};
void ps2Vu1StaticNote(unsigned k){++counts[k];}
void ps2Vu1StaticPrintStats(){if(ps2Vu1StaticStatsOn)std::fprintf(stderr,"[vu1static] tailCalls=%llu tailPairs=%llu pendingVF=%llu pendingVI=%llu pendingACC=%llu declinedSteps=%llu midSteps=%llu entrySteps=%llu\n",counts[0].load(),counts[1].load(),counts[2].load(),counts[3].load(),counts[4].load(),counts[5].load(),counts[6].load(),counts[7].load());}
