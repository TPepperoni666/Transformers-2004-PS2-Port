#pragma once
#include <array>
#include <cstdint>
#include <vector>
struct Output {std::array<uint8_t,664> state{};uint64_t pairs=0,fallback=0,scheduled=0,hostCycles=0;};
Output reference(uint8_t*,uint8_t*,const uint8_t*,const uint32_t*,uint32_t);
Output native(int,uint8_t*,uint8_t*,const uint8_t*,const uint32_t*,uint32_t);
void tracePc(uint32_t);
