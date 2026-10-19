// Replay shim (vu1_replay): minimal PS2Memory for linking the real VU1 interpreter offline.
#pragma once
#include <cstdint>
#include "runtime/gs/ps2_gif_arbiter.h"
class PS2Memory
{
public:
    uint8_t *vu1Code = nullptr;
    uint8_t *vu0Code = nullptr;
    uint64_t gen1 = 1, gen0 = 1;
    uint64_t getVU0CodeGeneration() const { return gen0; }
    uint64_t getVU1CodeGeneration() const { return gen1; }
    uint8_t *getVU1Code() { return vu1Code; }
    const uint8_t *getVU1Code() const { return vu1Code; }
    const uint8_t *peekVU1CodeForExecution() const { return vu1Code; } // Transformers port: v0.7.0 worker bridge read view.
    uint8_t *getVU0Code() { return vu0Code; }
    const uint8_t *getVU0Code() const { return vu0Code; }
    void submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately = true, bool path2DirectHl = false);
};

// Transformers port: shipping diagnostic code-size constant for standalone builds.
inline constexpr unsigned PS2_VU1_CODE_SIZE=0x4000;
