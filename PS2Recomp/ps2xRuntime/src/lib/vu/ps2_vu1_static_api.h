#pragma once
// TEMP(transformers): private-package scheduler step; optional-package stub declines.
#include <cstdint>
class GS;class PS2Memory;
bool ps2Vu1StaticStep(void*,uint8_t*,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&);
extern const bool ps2Vu1StaticStatsOn;
void ps2Vu1StaticNote(unsigned);
void ps2Vu1StaticPrintStats();
