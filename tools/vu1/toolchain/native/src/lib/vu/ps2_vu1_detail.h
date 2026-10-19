#ifndef PS2_VU1_DETAIL_H
#define PS2_VU1_DETAIL_H

#include <cstdint>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Count trailing zeros of a NONZERO mask (every caller loops `for (m = ...; m; m &= m - 1)`).
// Adaptation for the KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch (GPL-3.0) port:
// MSVC does not inline std::countr_zero under /Ob1 (our test build) - it is two nested calls
// (ISA dispatch + tzcnt/bsf) - which made every set-bit loop slower than a plain 4-lane scan.
// The intrinsics below always inline; results are identical for nonzero input.
static inline uint32_t vuCtz32(uint32_t mask)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, mask);
    return static_cast<uint32_t>(index);
#else
    return static_cast<uint32_t>(__builtin_ctz(mask));
#endif
}

static inline uint32_t vuCtz64(uint64_t mask)
{
#if defined(_MSC_VER) && defined(_WIN64)
    unsigned long index;
    _BitScanForward64(&index, mask);
    return static_cast<uint32_t>(index);
#elif defined(_MSC_VER)
    const uint32_t low = static_cast<uint32_t>(mask);
    return low != 0u ? vuCtz32(low) : 32u + vuCtz32(static_cast<uint32_t>(mask >> 32));
#else
    return static_cast<uint32_t>(__builtin_ctzll(mask));
#endif
}

// Translation-unit-local helper avoids a cross-file call in /Ob1 builds.
static inline float vuNormalizeOperand(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = bits & 0x7F800000u;
    if (exponent == 0u) bits &= 0x80000000u;
    else if (exponent == 0x7F800000u) bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Instruction field extraction helpers
static inline uint8_t DEST(uint32_t i) { return (uint8_t)((i >> 21) & 0xF); }
static inline uint8_t FT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t FS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t FD(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline uint8_t BC(uint32_t i) { return (uint8_t)(i & 0x3); }

// Lower instruction field helpers
static inline uint8_t LIT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t LIS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t LID(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline uint8_t VIT(uint32_t i) { return (uint8_t)((i >> 16) & 0xF); }
static inline uint8_t VIS(uint32_t i) { return (uint8_t)((i >> 11) & 0xF); }
static inline uint8_t VID(uint32_t i) { return (uint8_t)((i >> 6) & 0xF); }
static inline int16_t IMM11(uint32_t i) { return (int16_t)(int32_t)((int32_t)(i << 21) >> 21); }
static inline int16_t IMM15(uint32_t i)
{
    uint32_t lo11 = i & 0x7FF;
    uint32_t hi4 = (i >> 21) & 0xF;
    uint32_t raw = (hi4 << 11) | lo11;
    return (int16_t)(int32_t)((int32_t)(raw << 17) >> 17);
}

#endif
