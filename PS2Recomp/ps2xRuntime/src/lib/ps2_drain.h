#pragma once
// TEMP(transformers): interrupt drain runtime knobs and EE-only observation state.
#include <cstdint>
namespace ps2drain {
extern const bool fast;
extern const bool stats;
struct Counts { uint64_t stores=0, drains=0, dmac=0, intc=0, mutexes=0, syncs=0; };
extern thread_local Counts counts;
extern thread_local unsigned depth;
void store();
void flush(uint64_t vs);
}
