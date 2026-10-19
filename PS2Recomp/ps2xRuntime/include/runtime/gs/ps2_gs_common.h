#ifndef PS2_GS_COMMON_H
#define PS2_GS_COMMON_H

#include "runtime/gs/gs_types.h"
#include <cstdint>

// TEMP(transformers): SMODE1/SYNCV as the BIOS SetGsCrt programs them for analog NTSC/PAL (ps2sdk graph mode table:
// SMODE1 RC=4 LC=32 T1248=1 CMOD=2|3 PRST=1 SINT=1 SPML=4 CLKSEL=1 NVCK=1 SLCK2=1 VCKSEL=1; SYNCV NTSC VFP=1 VFPE=6
// VBP=26 VBPE=6 VDP=480 VS=6, PAL VFP=1 VFPE=5 VBP=33 VBPE=5 VDP=576 VS=5). Our libgs HLE used to leave both 0;
// GSCpuBackend's presenter ignores them, but parallel-gs (M1, PS2X_GS_BACKEND=PGS) refuses to scan out with SMODE1=0.
namespace GSCrtDefaults
{
inline constexpr uint64_t kSmode1Ntsc = 0x0000000740834504ull;
inline constexpr uint64_t kSmode1Pal = 0x0000000740836504ull;
inline constexpr uint64_t kSyncvNtsc = 0x00C7800601A01801ull;
inline constexpr uint64_t kSyncvPal = 0x00A9000502101401ull;
}

namespace GSInternal
{
static inline uint32_t bitsPerPixel(uint8_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
        return 32;
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        return 32;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return 16;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        return 8;
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return 4;
    default:
        return 32;
    }
}

static inline uint32_t fbStride(uint32_t fbw, uint8_t psm)
{
    uint32_t pixelsPerRow = fbw * 64u;
    return pixelsPerRow * (bitsPerPixel(psm) / 8u);
}

static inline uint32_t framePageBaseToBlock(uint32_t fbp)
{
    return fbp << 5u;
}

static inline int clampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t clampU8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}
}

#endif
