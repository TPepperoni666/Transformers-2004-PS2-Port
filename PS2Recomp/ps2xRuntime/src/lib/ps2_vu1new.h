// TEMP(transformers): host-only new microcode observer; no runtime/PCH layout changes.
#pragma once
#include <cstdint>
namespace ps2_vu1new {
bool enabled();
void mpg(const uint8_t* payload, uint32_t pairs, uint32_t dest);
void directStore(uint32_t address, uint32_t bytes);
void cdRead(uint32_t lsn);
void vsync(uint64_t vs);
void shutdown();
void aotCounters(uint64_t& declined, uint64_t& unknown, uint64_t& midcall);
}
