// Third-party adaptation notice: GPL-3.0; VU1 fast path / XGKICK and run-to-end adapted from Scotho/socom-unzipped; readiness scan informed by KIexster/god-of-war-recomp.
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "../ps2_perfscope.h" // TEMP(transformers): qualification

#include <algorithm>
#include <bit>
#include <cassert>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <iostream>
#include <emmintrin.h>
#include <ps2_log.h>
#include "../debug_counters.h" // TEMP(transformers)

namespace ps2_perfscope { void noteVu0Call(); void noteVu0Pairs(uint64_t); } // TEMP(transformers) vu0fast1

namespace
{
    // TEMP(transformers): one atomic batch per run, no atomic operations per pair.
    struct Vu0PairCount { bool on; uint64_t n=0; ~Vu0PairCount(){if(on && n)ps2_perfscope::noteVu0Pairs(n);} };
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // Env knobs (read once). "0" turns a default-on knob off; any other value turns a default-off knob on.
    bool vu1Knob(const char *name, bool defaultOn)
    {
        const char *e = std::getenv(name);
        if (!e || !*e)
            return defaultOn;
        return !(e[0] == '0' && e[1] == '\0');
    }
    // C1: PS2X_VU1_FAST=0 runs every VU1 pair through the interpreter's write queues (the old path).
    bool vu1FastKnob()
    {
        static const bool s_on = vu1Knob("PS2X_VU1_FAST", true);
        return s_on;
    }
    // TEMP(transformers) vu0fast1: independent, default-ON VU0 fast path.
    bool vu0FastKnob()
    {
        static const bool s_on = vu1Knob("PS2X_VU0_FAST", true);
        return s_on;
    }
    // C2: PS2X_VU1_RUN_TO_END=0 restores the old budget cut (a cut program is resumed by the next MSCNT
    // or dropped by the next MSCAL).
    bool vu1RunToEndKnob()
    {
        static const bool s_on = vu1Knob("PS2X_VU1_RUN_TO_END", true);
        return s_on;
    }
    // C2 runaway guard in VU1 cycles per program (PS2X_VU1_GUARD, default 16M = ~7x the longest legitimate
    // Transformers program, ~2.2M; batch-3 review F2: a 64M runaway cost ~6.5 s on a loaded machine).
    uint64_t vu1GuardCycles()
    {
        static const uint64_t s_guard = []() -> uint64_t
        {
            const char *e = std::getenv("PS2X_VU1_GUARD");
            const unsigned long long v = e ? std::strtoull(e, nullptr, 0) : 0ull;
            return v ? static_cast<uint64_t>(v) : (uint64_t{1} << 24);
        }();
        return s_guard;
    }
    // B8: PS2X_VU1_XGKICK_IMMEDIATE=1 sends the whole PATH1 packet at XGKICK time (A/B candidate).
    bool vu1XgkickImmediateKnob()
    {
        static const bool s_on = vu1Knob("PS2X_VU1_XGKICK_IMMEDIATE", false);
        return s_on;
    }
    // A4: PS2X_VU1_XGKICK_ZERO=1 restores the 64 KB packet-buffer zeroing on every program start / XGKICK.
    bool vu1XgkickZeroKnob()
    {
        static const bool s_on = vu1Knob("PS2X_VU1_XGKICK_ZERO", false);
        return s_on;
    }
    // Fast path hand-over margin: runFast stops this many cycles before the run's cycle budget and the
    // interpreter runs the rest, so a budget cut leaves exactly the interpreter's register/queue state
    // (every early-landed write is resolved long before the cut; see runFast).
    constexpr uint64_t kFastTailCycles = 64u;
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_commitCalendar = {};
    m_commitCycles = 0;
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    for (auto &entry : m_storePipeline) entry.valid = false;
    for (auto &entry : m_vfWritePipeline) entry.valid = false;
    for (auto &entry : m_viWritePipeline) entry.valid = false;
    for (auto &entry : m_accWritePipeline) entry.valid = false;
    if (vu1XgkickZeroKnob())
        m_xgkick = {};
    else
        m_xgkick.clear(); // A4, ported from Scotho/socom-unzipped ps2_vu1.h XgkickPipeline::clear() (GPL-3.0)
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
    m_fastMaxWriteReady = 0;
    m_programPending = false;
}

void VU1Interpreter::reset()
{
    synchronize();
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch (GPL-3.0): normalizeOperand
    // inlined. Our equivalent is the static inline vuNormalizeOperand (ps2_vu1_detail.h; same bits), which
    // also inlines under the /Ob1 test build without touching the PCH header ps2_vu1.h.
    return vuNormalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    return vuNormalizeOperand(value);
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    if (!m_viBranchBackupValid || m_viBranchBackupReg != reg)
        m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dst == m_state.vf[0])
        return; // VF0 is immutable even to the other instruction in this pair.
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(vuNormalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const auto q = [&]() { return operand(m_state.q); };
    const auto i = [&]() { return operand(m_state.i); };

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q();
                break;
            case 0x1Eu:
                result = vs(component) * i();
                break;
            case 0x20u:
                result = vs(component) + q();
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q();
                break;
            case 0x22u:
                result = vs(component) + i();
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i();
                break;
            case 0x24u:
                result = vs(component) - q();
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q();
                break;
            case 0x26u:
                result = vs(component) - i();
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i();
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q();
            break;
        case 0x1Eu:
            result = vs(component) * i();
            break;
        case 0x20u:
            result = vs(component) + q();
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q();
            break;
        case 0x22u:
            result = vs(component) + i();
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i();
            break;
        case 0x24u:
            result = vs(component) - q();
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q();
            break;
        case 0x26u:
            result = vs(component) - i();
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i();
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch (GPL-3.0): inline operand
        // normalisation (was an out-of-line member call per operand, not inlined under /Ob1).
        const float left = vuNormalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = vuNormalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = vuNormalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = vuNormalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = vuNormalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = vuNormalizeOperand(m_state.vf[ft][component]);
        }

        // For normalized, nonzero inputs these exponent bounds place the
        // exact product strictly inside the normal float range, even after
        // rounding. Only the sign sticky flag can be set; avoid the multiply
        // and exact-result classifier entirely. Boundary cases use the old path.
        uint32_t leftBits, rightBits;
        std::memcpy(&leftBits, &left, sizeof(leftBits));
        std::memcpy(&rightBits, &right, sizeof(rightBits));
        const uint32_t leftExp = (leftBits >> 23u) & 255u;
        const uint32_t rightExp = (rightBits >> 23u) & 255u;
        const uint32_t exponentSum = leftExp + rightExp;
        if (leftExp != 0u && rightExp != 0u && exponentSum >= 129u && exponentSum <= 379u)
        {
            extraSticky |= ((leftBits ^ rightBits) >> 30u) & 2u;
            continue;
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        // Spread Z/S/U/O into the four MAC nibbles without four branches.
        const uint32_t spread = (flags & 1u) | ((flags & 2u) << 3) |
                                ((flags & 4u) << 6) | ((flags & 8u) << 9);
        mac |= spread << (3u - component);
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    scheduleCommit(entry->readyCycle, static_cast<uint32_t>(entry - m_flagPipeline.data()));
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    if (m_fast && fastFmac(m_currentUpperInstruction, dst, result))
        return;
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    if (m_fast && fastFmac(m_currentUpperInstruction, m_state.acc, result))
        return;
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            scheduleCommit(entry.readyCycle, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            scheduleCommit(entry.readyCycle, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            scheduleCommit(entry.readyCycle, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    scheduleCommit(m_fdiv.readyCycle, 8u);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            scheduleCommit(entry.readyCycle, 9u + static_cast<uint32_t>(&entry - m_efu.data()));
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    if (m_unit == Unit::VU1) // TEMP(transformers): PS2X_VU1_COLORWATCH=<w0>,<w1>,<w2>,<w3> (hex words) - when a VU1 store
    {                        // lands exactly those four words, log pc/vi and dump code+data+state (vu1dump format) to
        struct ColorWatch    // vu1colorwatch_<n>.bin in the cwd (max 6 dumps).
        {
            bool on = false;
            uint32_t w[4]{};
        };
        static const ColorWatch s_cw = []() -> ColorWatch
        {
            ColorWatch c{};
            const char *e = std::getenv("PS2X_VU1_COLORWATCH");
            if (!e)
                return c;
            char *n = nullptr;
            for (int i = 0; i < 4; ++i)
            {
                c.w[i] = static_cast<uint32_t>(std::strtoul(i == 0 ? e : n + 1, &n, 16));
                if (i < 3 && (!n || *n != ','))
                    return c;
            }
            c.on = true;
            return c;
        }();
        if (s_cw.on && laneMask == 0xFu && words[0] == s_cw.w[0] && words[1] == s_cw.w[1] && words[2] == s_cw.w[2] &&
            words[3] == s_cw.w[3])
        {
            static thread_local unsigned s_hits = 0u; // TEMP(transformers): independent VU0/VU1 diagnostic writers
            const unsigned hit = s_hits++;
            const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
            std::cerr << "[vu1colorwatch] hit=" << hit << " vs=" << vsNow << " pc=0x" << std::hex << m_state.pc << " q=0x"
                      << ((address >> 4) & 0x3FFu) << " fast=" << m_fast << " vi=";
            for (int i = 0; i < 16; ++i)
                std::cerr << (i ? "," : "") << m_state.vi[i];
            std::cerr << std::dec << std::endl;
            if (hit < 6u && m_cachedVuCode && m_activeVuData)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "vu1colorwatch_%02u.bin", hit);
                std::FILE *f = std::fopen(name, "wb");
                if (f)
                {
                    const uint32_t header[4] = {m_state.pc, 0u, 0u, static_cast<uint32_t>(sizeof(VU1State))};
                    std::fwrite(header, 1, sizeof(header), f);
                    std::fwrite(m_cachedVuCode, 1, PS2_VU1_CODE_SIZE, f);
                    std::fwrite(m_activeVuData, 1, m_activeVuDataSize, f);
                    std::fwrite(&m_state, 1, sizeof(VU1State), f);
                    std::fclose(f);
                }
            }
        }
    }
    if (m_unit == Unit::VU1) // TEMP(transformers): PS2X_VU1WATCH also reports VU1 stores into the watched qword range
    {
        struct WatchSpec { uint32_t lo, hi; long long vsFrom, vsTo; };
        static const WatchSpec s_watch = []() -> WatchSpec
        {
            const char *e = std::getenv("PS2X_VU1WATCH");
            if (!e)
                return WatchSpec{1u, 0u, 0, -1};
            char *n = nullptr;
            WatchSpec w{};
            w.lo = static_cast<uint32_t>(std::strtoul(e, &n, 16));
            w.hi = (n && *n == '-') ? static_cast<uint32_t>(std::strtoul(n + 1, &n, 16)) : w.lo;
            w.vsFrom = (n && *n == '@') ? std::strtoll(n + 1, &n, 10) : 0;
            w.vsTo = (n && *n == '-') ? std::strtoll(n + 1, &n, 10) : (1ll << 62);
            return w;
        }();
        const uint32_t q = (address >> 4) & 0x3FFu;
        const long long vsNow = static_cast<long long>(ps2dbg::vsyncTick.load(std::memory_order_relaxed));
        if (s_watch.lo <= s_watch.hi && q >= s_watch.lo && q <= s_watch.hi && vsNow >= s_watch.vsFrom && vsNow <= s_watch.vsTo)
        {
            static thread_local uint32_t s_logs = 0u;
            if (s_logs++ < 3000u)
                std::cerr << "[vu1watch:store] vs=" << vsNow << " pc=0x" << std::hex << m_state.pc << " q=0x" << q << " mask=0x"
                          << unsigned(laneMask) << " w=" << words[0] << "," << words[1] << "," << words[2] << "," << words[3]
                          << std::dec << std::endl;
        }
    }
    if (m_fast)
    {
        // Fast path (after Scotho/socom-unzipped queueStore, GPL-3.0): the store lands now instead of at
        // the next cycle boundary. Same bytes; nothing can observe the difference: the next pair issues
        // at >= m_cycle + 1, and PATH1 already took this cycle's qword before this pair executed.
        if (m_activeVuData && address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = words[component];
            }
            std::memcpy(m_activeVuData + address, oldWords, sizeof(oldWords));
        }
        return;
    }
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            scheduleCommit(store.readyCycle, 11u + static_cast<uint32_t>(&store - m_storePipeline.data()));
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            scheduleCommit(write.readyCycle, 19u + static_cast<uint32_t>(&write - m_vfWritePipeline.data()));
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            scheduleCommit(write.readyCycle, 35u + static_cast<uint32_t>(&write - m_viWritePipeline.data()));
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            scheduleCommit(write.readyCycle, 43u + static_cast<uint32_t>(&write - m_accWritePipeline.data()));
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::scheduleCommit(uint64_t cycle, uint32_t bit)
{
    static_assert(kMaxFlagEntries == 8u && kMaxPendingStores == 8u &&
                  kMaxPendingVfWrites == 16u && kMaxPendingViWrites == 8u &&
                  kMaxPendingAccWrites == 8u, "Update commit calendar bit layout");
    assert(cycle > m_cycle && cycle - m_cycle < m_commitCalendar.size());
    const uint64_t mask = uint64_t{1} << bit;
    auto &bucket = m_commitCalendar[cycle & 63u];
    bucket |= mask;
    m_commitCycles |= uint64_t{1} << (cycle & 63u);
}

void VU1Interpreter::commitReadyPipelines()
{
    // vuCtz64 instead of std::countr_zero: always inlined (see ps2_vu1_detail.h; GoW VU1 perf port adaptation).
    uint64_t due = std::exchange(m_commitCalendar[m_cycle & 63u], uint64_t{0});
    if (!due) return;
    m_commitCycles &= ~(uint64_t{1} << (m_cycle & 63u));
    for (uint64_t slots = (due >> 0u) & 255u; slots; slots &= slots - 1u)
    {
        FlagPipelineEntry &entry = m_flagPipeline[vuCtz64(slots)];
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry.valid = false;
    }

    if ((due & (uint64_t{1} << 8u)) && m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (uint64_t slots = (due >> 9u) & 3u; slots; slots &= slots - 1u)
    {
        ScalarPipelineEntry &entry = m_efu[vuCtz64(slots)];
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry.valid = false;
        }
    }

    for (uint64_t slots = (due >> 11u) & 255u; slots; slots &= slots - 1u)
    {
        PendingStore &store = m_storePipeline[vuCtz64(slots)];
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store.valid = false;
    }

    for (uint64_t slots = (due >> 19u) & 65535u; slots; slots &= slots - 1u)
    {
        PendingVfWrite &write = m_vfWritePipeline[vuCtz64(slots)];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write.valid = false;
    }

    for (uint64_t slots = (due >> 35u) & 255u; slots; slots &= slots - 1u)
    {
        PendingViWrite &write = m_viWritePipeline[vuCtz64(slots)];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write.valid = false;
    }

    for (uint64_t slots = (due >> 43u) & 255u; slots; slots &= slots - 1u)
    {
        PendingAccWrite &write = m_accWritePipeline[vuCtz64(slots)];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write.valid = false;
    }
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        // PERF(transformers) - NOT from the GoW patch (done alongside its port; profile: ~8% of VU1 time was the
        // per-byte modulo here). A qword that does not wrap is the same 16 bytes as the byte loop; only a
        // qword crossing the end of VU memory takes the original loop.
        const uint32_t firstSource = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (firstSource + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + firstSource, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format >= 2u) // TEMP(transformers): FLG=3 is IMAGE (GoW 353ac83c)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    emitPath1(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    if (vu1XgkickZeroKnob())
        m_xgkick = {};
    else
        m_xgkick.clear(); // A4, ported from Scotho/socom-unzipped ps2_vu1.h XgkickPipeline::clear() (GPL-3.0)
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;

    if (!vu1XgkickImmediateKnob())
        return;
    // B8 (PS2X_VU1_XGKICK_IMMEDIATE=1), ported from Scotho/socom-unzipped ps2_vu1_core.cpp startXgkick
    // (GPL-3.0): the whole packet is taken from VU memory at kick time instead of one qword per two
    // cycles while the program runs on. Changes what PATH1 sees when a program rewrites a buffer it
    // already kicked (SOCOM lost object geometry with the cycle model), so it is an A/B knob.
    m_xgkick.cycleCredit = 0x40000000u;
    if (m_activeMemory)
    {
        // Direct submit: walk the GIFtags in VU memory and hand the packet over from there. Falls back
        // to the copying paths below on a wrap past the end of VU memory, an overrun of the 64 KB
        // packet limit or a reserved tag format.
        uint32_t off = 0u;
        bool ok = false;
        for (;;)
        {
            const uint32_t src = sourceAddress + off;
            if (src + 16u > m_activeVuDataSize || off > XgkickPipeline::kBufferSize - 16u)
                break;
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_activeVuData + src, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format >= 2u) // TEMP(transformers): FLG=3 is IMAGE (GoW 353ac83c)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
                break;
            if (tagBytes > XgkickPipeline::kBufferSize - off || src + tagBytes > m_activeVuDataSize)
                break;
            off += static_cast<uint32_t>(tagBytes);
            if (((tagLo >> 15) & 1u) != 0u)
            {
                ok = true;
                break;
            }
        }
        if (ok)
        {
            m_xgkick.totalBytes = off;
            m_xgkick.copiedBytes = off;
            emitPath1(m_activeVuData + sourceAddress, off);
            m_xgkick.active = false;
            return;
        }
    }
    // Bulk copy: one memcpy per GIFtag (tag + payload) as long as the packet does not wrap; the
    // per-qword path (progressXgkick with an unlimited credit) takes the rest and reports errors.
    while (m_xgkick.active)
    {
        const uint32_t off = m_xgkick.copiedBytes;
        const uint32_t src = m_xgkick.sourceAddress + off;
        if (src + 16u > m_activeVuDataSize || off > XgkickPipeline::kBufferSize - 16u)
            break;
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, m_activeVuData + src, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        uint64_t tagBytes = 16u;
        if (format == 0u)
            tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (format == 1u)
            tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
        else if (format >= 2u) // TEMP(transformers): FLG=3 is IMAGE (GoW 353ac83c)
            tagBytes += static_cast<uint64_t>(nloop) * 16u;
        else
            break;
        if (tagBytes > XgkickPipeline::kBufferSize - off || src + tagBytes > m_activeVuDataSize)
            break;
        std::memcpy(m_xgkick.packet.data() + off, m_activeVuData + src, static_cast<size_t>(tagBytes));
        m_xgkick.copiedBytes = off + static_cast<uint32_t>(tagBytes);
        if (((tagLo >> 15) & 1u) != 0u)
        {
            m_xgkick.totalBytes = m_xgkick.copiedBytes;
            finishXgkick();
        }
    }
    progressXgkick();
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
    {
        if (!m_xgkick.active)
        {
            // The next occupied calendar bucket is at most 63 cycles away.
            // Rotate so bit zero denotes the next cycle, then jump directly
            // to that event (or the caller's earlier target).
            uint64_t next = targetCycle;
            if (m_commitCycles)
            {
                const uint64_t upcoming = std::rotr(m_commitCycles,
                    static_cast<int>((m_cycle + 1u) & 63u));
                next = std::min(next, m_cycle + 1u + vuCtz64(upcoming));
            }
            m_cycle = next;
            m_state.cycles = m_cycle;
            commitReadyPipelines();
            continue;
        }
        advanceOneCycle();
    }
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch (GPL-3.0)
        // Visit only the private qualification evidence the instruction reads (qualification 3 = x ... bit 0 = w,
        // i.e. component = 3 - bit). std::max is order-independent, so this is bit-exact.
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            const auto &vfReady = m_vfReady[access.reg];
            for (uint32_t lanes = access.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                ready = std::max(ready, vfReady[3u - vuCtz32(lanes)]);
        }
        for (uint32_t mask = usage->viRead & 0xFFFEu; mask; mask &= mask - 1u)
            ready = std::max(ready, m_viReady[vuCtz32(mask)]);
        for (uint32_t lanes = usage->accRead & 0xFu; lanes != 0u; lanes &= lanes - 1u)
            ready = std::max(ready, m_accReady[3u - vuCtz32(lanes)]);
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    // Same set-bit qualification as calculatePairReadyCycle (technique from
    // KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch, GPL-3.0). Each qualification
    // written at most once per loop and lower is still written before upper, so the
    // resulting scoreboard is identical.
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        auto &vfReady = m_vfReady[lowerWrite.reg];
        for (uint32_t lanes = lowerWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
            vfReady[3u - vuCtz32(lanes)] = m_cycle + latency;
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        auto &vfReady = m_vfReady[upperWrite.reg];
        for (uint32_t lanes = upperWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
            vfReady[3u - vuCtz32(lanes)] = m_cycle + latency;
    }

    for (uint32_t mask = decoded.lowerUsage.viWrite & 0xFFFEu; mask; mask &= mask - 1u)
        m_viReady[vuCtz32(mask)] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    for (uint32_t lanes = decoded.upperUsage.accWrite & 0xFu; lanes != 0u; lanes &= lanes - 1u)
        m_accReady[3u - vuCtz32(lanes)] = m_cycle + kAccForwardLatency;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x78: // qualification / CROSSOVER3 H1 (Sinan-Karakaya 0adeffb5afd5): manual slots; 0x77 and 0x7F are unassigned in the EFU rows and stay reserved.
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x79: // ERSQRT
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7D: // EATAN
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x7C: // ESIN
            usage.latency = 29u;
            break;
        case 0x7E: // EEXP
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        // Shadow registers are 0..31; 0xFF marks a pair not decoded for
        // this code generation. Decode only pairs that execution reaches.
        m_decodedCodeCache[i].upperVfShadowReg = 0xFFu;

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->peekVU1CodeForExecution()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    auto &pair = m_decodedCodeCache[pairIndex];
    if (pair.upperVfShadowReg == 0xFFu)
        pair = decodeInstructionPair(vuCode, pc);
    return pair;
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}


#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
int ps2Vu1CurrentPriority() noexcept {
#ifdef _WIN32
    return GetThreadPriority(GetCurrentThread());
#else
    return 0;
#endif
}
void ps2Vu1SetPriority(int priority) noexcept {
#ifdef _WIN32
    if(priority!=THREAD_PRIORITY_ERROR_RETURN)SetThreadPriority(GetCurrentThread(),priority);
#endif
}
void ps2Vu1WorkerPriority() noexcept
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

VU1Interpreter::~VU1Interpreter()
{
    // Runtime explicitly fences before destroying GS/memory. Never publish from this destructor.
    if (m_worker) m_worker->finishNoThrow();
}

void VU1Interpreter::emitPath1(const uint8_t *p, uint32_t n)
{
    if (m_capturePath1)
    {
        // Bound retained output; runaway programs fail explicitly rather than silently dropping geometry.
        if (m_path1Bytes + n > 64u * 1024u * 1024u)
            throw std::runtime_error("VU1 threaded PATH1 output exceeded 64 MiB");
        m_path1Packets.emplace_back(p, p + n);
        m_path1Bytes += n;
    }
    else if (m_activeMemory) m_activeMemory->submitGifPacket(GifPathId::Path1, p, n);
    else if (m_activeGs) m_activeGs->processGIFPacket(p, n);
}

void VU1Interpreter::synchronize()
{
    if (m_dependencyFence) m_dependencyFence();
    if (!m_hostPending) return;
    try { m_worker->finish(); }
    catch (...) {
        m_hostPending = false;
        m_capturePath1 = false;
        m_path1Packets.clear();
        throw; // No retry: interpreter/data may already have advanced.
    }
    m_hostPending = false; // clear before reentering memory/GS fence callbacks
    m_capturePath1 = false;
    auto packets = std::move(m_path1Packets);
    m_path1Packets.clear();
    for (const auto &p : packets)
        if (m_publishMemory) m_publishMemory->submitGifPacket(GifPathId::Path1, p.data(), static_cast<uint32_t>(p.size()));
        else if (m_publishGs) m_publishGs->processGIFPacket(p.data(), static_cast<uint32_t>(p.size()));
}

void VU1Interpreter::execute(uint8_t *code, uint32_t cs, uint8_t *data, uint32_t ds,
                             GS &gs, PS2Memory *mem, uint32_t pc, uint32_t top, uint32_t itop, uint32_t budget)
{
    synchronize();
    if (m_unit != Unit::VU1 || !Vu1Worker::enabled()) {
        executeInline(code, cs, data, ds, gs, mem, pc, top, itop, budget); return;
    }
    if (!m_worker) m_worker = std::make_unique<Vu1Worker>();
    m_ownerToken.store(Vu1Worker::currentThreadToken(), std::memory_order_relaxed);
    m_publishGs = &gs; m_publishMemory = mem;
    m_path1Packets.clear(); m_path1Bytes = 0;
    m_capturePath1 = true; m_hostPending = true;
    m_worker->submit([=, &gs, this] { executeInline(code, cs, data, ds, gs, mem, pc, top, itop, budget); });
}

void VU1Interpreter::resume(uint8_t *code, uint32_t cs, uint8_t *data, uint32_t ds,
                            GS &gs, PS2Memory *mem, uint32_t top, uint32_t itop, uint32_t budget)
{
    synchronize();
    if (m_unit != Unit::VU1 || !Vu1Worker::enabled()) {
        resumeInline(code, cs, data, ds, gs, mem, top, itop, budget); return;
    }
    if (!m_worker) m_worker = std::make_unique<Vu1Worker>();
    m_ownerToken.store(Vu1Worker::currentThreadToken(), std::memory_order_relaxed);
    m_publishGs = &gs; m_publishMemory = mem;
    m_path1Packets.clear(); m_path1Bytes = 0;
    m_capturePath1 = true; m_hostPending = true;
    m_worker->submit([=, &gs, this] { resumeInline(code, cs, data, ds, gs, mem, top, itop, budget); });
}

void VU1Interpreter::executeInline(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    ps2_perfscope::Scope perf(m_unit == Unit::VU1 ? ps2_perfscope::EeVu1 : ps2_perfscope::EeVu0); // TEMP(transformers) qualification
#if defined(PS2X_WITH_VU1_AOT) // TEMP(transformers): local-only VU1 AOT (PS2X_VU1_AOT=1); declines -> interpreter below
    if (tryAot(vuCode, codeSize, vuData, dataSize, gs, memory, startPC, top, itop, maxCycles, false))
        return;
#endif
    if (m_unit == Unit::VU1) // TEMP(transformers) qualification: this VU1 call runs in the interpreter (AOT declined/off)
        ps2_perfscope::noteVu1Interp();
    if (m_unit == Unit::VU0) ps2_perfscope::noteVu0Call(); // TEMP(transformers)
    // C2 (PS2X_VU1_RUN_TO_END, default on), ported from Scotho/socom-unzipped executeProgram (GPL-3.0):
    // the VIF stalls MSCAL/MSCNT until the VU is idle, so a program never stops at the cycle budget
    // any more. The budget becomes the slice length; the program runs slice after slice to its E bit
    // (or D/T halt) inside this call, and only a runaway past PS2X_VU1_GUARD cycles is dropped.
    const bool runToEnd = m_unit == Unit::VU1 && maxCycles != 0u && vu1RunToEndKnob();
    if (runToEnd && m_programPending) // only reachable if the knob path was bypassed; finish it with its own TOP/ITOP
        finishPendingProgram(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, m_cycle + vu1GuardCycles());
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    const uint64_t guardEnd = m_cycle + vu1GuardCycles();
    m_quietBudget = runToEnd;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (runToEnd)
        finishPendingProgram(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, guardEnd);
    m_quietBudget = false;
}

void VU1Interpreter::resumeInline(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    ps2_perfscope::Scope perf(m_unit == Unit::VU1 ? ps2_perfscope::EeVu1 : ps2_perfscope::EeVu0); // TEMP(transformers) qualification
#if defined(PS2X_WITH_VU1_AOT) // TEMP(transformers): local-only VU1 AOT (PS2X_VU1_AOT=1)
    if (tryAot(vuCode, codeSize, vuData, dataSize, gs, memory, m_state.pc, top, itop, maxCycles, true))
        return;
#endif
    if (m_unit == Unit::VU1) // TEMP(transformers) qualification: this VU1 call runs in the interpreter (AOT declined/off)
        ps2_perfscope::noteVu1Interp();
    // C2: see execute() (Scotho/socom-unzipped continueProgram, GPL-3.0).
    const bool runToEnd = m_unit == Unit::VU1 && maxCycles != 0u && vu1RunToEndKnob();
    if (runToEnd && m_programPending)
        finishPendingProgram(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, m_cycle + vu1GuardCycles());
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    const uint64_t guardEnd = m_cycle + vu1GuardCycles();
    m_quietBudget = runToEnd;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (runToEnd)
        finishPendingProgram(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles, guardEnd);
    m_quietBudget = false;
}

// C2 slice loop, ported from Scotho/socom-unzipped runToEnd (GPL-3.0); the slice is our cycle budget
// (ps2Vu1CycleBudget / PS2X_VU1_BUDGET), the guard PS2X_VU1_GUARD.
void VU1Interpreter::finishPendingProgram(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                          GS &gs, PS2Memory *memory, uint32_t slice, uint64_t guardEnd)
{
    const bool wasQuiet = m_quietBudget;
    m_quietBudget = true;
    while (m_programPending)
    {
        const uint64_t before = m_cycle;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, slice);
        if (m_programPending && (m_cycle == before || m_cycle >= guardEnd))
        {
            static thread_local uint32_t s_drops = 0;
            if (s_drops++ < 20)
                RUNTIME_ERROR("[VU1 run-to-end] program dropped at pc=0x" << std::hex << m_state.pc << std::dec
                              << (m_cycle == before ? " (no progress)" : " (past PS2X_VU1_GUARD)") << '\n');
            m_programPending = false;
            break;
        }
    }
    m_quietBudget = wasQuiet;
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    Vu0PairCount pairCount{m_unit == Unit::VU0}; // TEMP(transformers)

    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    // TEMP(transformers): independent VU1(default ON)/VU0(default ON) fast knobs. It hands back kFastTailCycles before the
    // budget end; the loop below then finishes the run with the interpreter's write queues.
    if (maxCycles > kFastTailCycles &&
        (m_unit == Unit::VU1 ? vu1FastKnob() : vu0FastKnob()))
        runFast(vuCode, codeSize, vuData, dataSize, gs, memory, budgetEnd - kFastTailCycles, programEnded);
    while (!programEnded && m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            m_viBranchBackupValid = false; // A stall expires the integer-branch bypass.
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        const bool takingPriorBranch = m_state.branchPending && m_state.branchDelay == 0u;
        const uint32_t priorBranchTarget = m_state.branchTarget;
        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = immediate; // LOI transfers raw bits; arithmetic normalizes at use.
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = m_viBranchBackupValid &&
                               writtenVi == m_viBranchBackupReg &&
                               decoded.lowerUsage.delaysNextBranchRead;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        ++pairCount.n; // TEMP(transformers): issued pair, excludes stalls
        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (takingPriorBranch)
        {
            // A branch in the delay slot executes one pair at the first target.
            const bool scheduledDelayBranch = m_state.branchPending && m_state.branchDelay == 1u;
            m_state.pc = priorBranchTarget & microAddressMask();
            m_state.branchPending = scheduledDelayBranch;
            m_state.branchDelay = 0u;
        }
        else if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        // Fast path: its early-landed VF/VI/ACC writes are not in the queues, but the interpreter's
        // flush would have kept stepping until the last of them was due (same final cycle count).
        while (m_cycle < m_fastMaxWriteReady)
            advanceOneCycle();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    // TEMP(transformers): a program that hits the cycle budget is silently cut off (geometry truncated);
    // log it so a too-small VU1 budget (65536 per MSCAL/MSCNT) is visible in the run log.
    m_programPending = !programEnded && !m_stopRequested && m_cycle >= budgetEnd;
    if (m_programPending && !m_quietBudget)
    {
        static thread_local uint32_t s_budgetWarnings = 0;
        if (s_budgetWarnings++ < 20)
            RUNTIME_ERROR("[VU" << (m_unit == Unit::VU1 ? "1" : "0") << " budget] program cut off at cycle budget, pc=0x"
                          << std::hex << m_state.pc << std::dec << '\n');
    }
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}

// ============================================================================
// C1: VU1 fast path (PS2X_VU1_FAST, default on; PS2X_VU1_FAST=0 = the interpreter loop above only).
// Idea and structure ported from Scotho/socom-unzipped third_party/ps2recomp ps2_vu1_core.cpp runFast /
// fastReadyCycle / queueStore and ps2_vu1_ops.h normalize4 (GPL-3.0). SOCOM's base (upstream 14b1e5c)
// lacks our VU1 audit fixes, so nothing of its instruction semantics is taken: the pairs execute
// through OUR execUpper/execLower (raw-bit MIN/MAX/ABS, raw LOI and EFU P, JR/JALR current VI, VF0
// rules, EATAN, FE_TOWARDZERO + vuNormalizeOperand, ...) and the loop below is our run() loop line by
// line. What changes:
//   * VF/VI/ACC results stay where execUpper/execLower put them instead of being restored and queued
//     (the interpreter commits them at issue + latency; every reader of a register stalls on the same
//     scoreboard until then, so no instruction can see the difference). VI is narrowed to int16 like
//     the interpreter's commit. Stores land at once (queueStore).
//   * arithmetic FMAC ops: one vector pass (fastFmac) instead of execUpper + the per-qualification double
//     classification (same results, see fastFmac).
//   * the decoded pair is used in place; ACC needs no ready check (forwarded after 1 cycle).
// Kept exactly: the scoreboard and its stall/advance steps, the flag (MAC/STATUS/CLIP), Q and P
// calendar, cycle-stepped PATH1 XGKICK, the branch-delay and branch-VI-bypass rules, D/T/E handling.
// Two places where "landed early" could be seen are closed explicitly:
//   * flush at program end: run() keeps stepping to m_fastMaxWriteReady (same final cycle count);
//   * a budget cut: runFast hands back kFastTailCycles before the budget end, the interpreter loop
//     finishes the run with real write queues, so the cut state (registers + pending writes) is the
//     interpreter's. Write queues the interpreter left behind (cut, then resume) are honoured: until
//     they are due, a landed write supersedes them exactly like a newer queued write would.
// ============================================================================
namespace
{
    // normalizeOperand on four lanes (bit-identical to vuNormalizeOperand). Ported from
    // Scotho/socom-unzipped ps2_vu1_ops.h normalize4 (GPL-3.0).
    inline __m128 vuNormalize4(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i zeroExp = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i maxExp = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        __m128i out = _mm_andnot_si128(_mm_or_si128(zeroExp, maxExp), bits);
        out = _mm_or_si128(out, _mm_and_si128(sign, zeroExp));
        out = _mm_or_si128(out, _mm_and_si128(_mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), maxExp));
        return _mm_castsi128_ps(out);
    }

    // Component bit c (bit 0 = x) -> per-qualification mask (SSE lane 0 = x); after Scotho/socom-unzipped
    // ps2_vu1_ops.h kLaneMaskTable (GPL-3.0).
    alignas(16) constexpr int32_t kVuLaneMask[16][4] = {
        {0, 0, 0, 0}, {-1, 0, 0, 0}, {0, -1, 0, 0}, {-1, -1, 0, 0},
        {0, 0, -1, 0}, {-1, 0, -1, 0}, {0, -1, -1, 0}, {-1, -1, -1, 0},
        {0, 0, 0, -1}, {-1, 0, 0, -1}, {0, -1, 0, -1}, {-1, -1, 0, -1},
        {0, 0, -1, -1}, {-1, 0, -1, -1}, {0, -1, -1, -1}, {-1, -1, -1, -1}};
    // Reverses 4 bits: component bits (bit 0 = x) <-> VU qualification (bit 3 = x, the dest / MAC order).
    constexpr uint8_t kVuRev4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
    inline __m128i vuLaneMask(uint32_t componentBits)
    {
        return _mm_load_si128(reinterpret_cast<const __m128i *>(kVuLaneMask[componentBits & 0xFu]));
    }

    // Z/S/U/O classes of four double lanes (lo = components 0,1, hi = 2,3), as bit c = component c:
    // the classification of normalizeFmacExactResult (|e| == 0, |e| > FLT_MAX, |e| < FLT_MIN, signbit).
    struct VuLaneClass
    {
        uint32_t zero, over, under, sign;
    };
    inline VuLaneClass vuClassify(__m128d lo, __m128d hi)
    {
        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d fltMax = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));
        const __m128d fltMin = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
        const __m128d zero = _mm_setzero_pd();
        const __m128d absLo = _mm_and_pd(lo, absMask);
        const __m128d absHi = _mm_and_pd(hi, absMask);
        VuLaneClass c;
        c.zero = static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpeq_pd(absLo, zero))) |
                 (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpeq_pd(absHi, zero))) << 2);
        c.over = static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpgt_pd(absLo, fltMax))) |
                 (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpgt_pd(absHi, fltMax))) << 2);
        c.under = (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmplt_pd(absLo, fltMin))) |
                   (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmplt_pd(absHi, fltMin))) << 2)) &
                  ~c.zero;
        c.sign = static_cast<uint32_t>(_mm_movemask_pd(lo)) | (static_cast<uint32_t>(_mm_movemask_pd(hi)) << 2);
        return c;
    }

    enum VuFmacKind : uint8_t { VuAdd, VuSub, VuMul, VuMadd, VuMsub };
    enum VuFmacSrc : uint8_t { VuSrcBc, VuSrcQ, VuSrcI, VuSrcVt, VuSrcCross };
    // The arithmetic FMAC ops of execUpper (the ones that go through applyFmacDest/applyFmacDestAcc).
    inline bool vuDecodeFmac(uint32_t upper, VuFmacKind &kind, VuFmacSrc &src, uint8_t &sel, bool &toAcc)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        toAcc = op >= 0x3Cu;
        if (!toAcc && op > 0x2Fu)
            return false;
        sel = toAcc ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        if (sel <= 0x0Fu)
        {
            static constexpr VuFmacKind kBcKinds[4] = {VuAdd, VuSub, VuMadd, VuMsub};
            kind = kBcKinds[sel >> 2];
            src = VuSrcBc;
            return true;
        }
        switch (sel)
        {
        case 0x18: case 0x19: case 0x1A: case 0x1B: kind = VuMul; src = VuSrcBc; return true;
        case 0x1C: kind = VuMul; src = VuSrcQ; return true;
        case 0x1E: kind = VuMul; src = VuSrcI; return true;
        case 0x20: kind = VuAdd; src = VuSrcQ; return true;
        case 0x21: kind = VuMadd; src = VuSrcQ; return true;
        case 0x22: kind = VuAdd; src = VuSrcI; return true;
        case 0x23: kind = VuMadd; src = VuSrcI; return true;
        case 0x24: kind = VuSub; src = VuSrcQ; return true;
        case 0x25: kind = VuMsub; src = VuSrcQ; return true;
        case 0x26: kind = VuSub; src = VuSrcI; return true;
        case 0x27: kind = VuMsub; src = VuSrcI; return true;
        case 0x28: kind = VuAdd; src = VuSrcVt; return true;
        case 0x29: kind = VuMadd; src = VuSrcVt; return true;
        case 0x2A: kind = VuMul; src = VuSrcVt; return true;
        case 0x2C: kind = VuSub; src = VuSrcVt; return true;
        case 0x2D: kind = VuMsub; src = VuSrcVt; return true;
        case 0x2E: kind = toAcc ? VuMul : VuMsub; src = VuSrcCross; return true; // OPMULA / OPMSUB
        default: return false;
        }
    }
}

// Arithmetic FMAC op for the fast path: operands, float result, classification, flags, write-back.
// Returns false (nothing done) for every other upper op; the caller then runs execUpper.
// `given` (nullable) is execUpper's float result when called from applyFmacDest's fast hook; `target`
// is then the destination it passed. Semantics are execUpper + normalizeFmacResult
// (calculateFmacExactResult + normalizeFmacExactResult) + calculateFmacProductSticky + updateFmacFlags
// + applyDest, unchanged:
//   * float result: execUpper's expressions on vuNormalizeOperand'd operands (same order: product,
//     then add/sub), in SSE lanes under the run's MXCSR (FE_TOWARDZERO) - qualification qualification the scalar
//     code computes. OPMULA/OPMSUB's w qualification 0.0f.
//   * the "exact" result is the reference's long double expression evaluated in SSE2 double lanes. On
//     MSVC long double IS double, so this is the reference computation itself, vectorised. (SOCOM's
//     fmacSingle4/fmacProductSum4 instead prove when the float result decides, against an 80-bit long
//     double; that proof is not for our 64-bit reference, so it is not used.) Where long double is
//     wider than double this returns false and execUpper's reference classifier runs.
//   * value: the float result, or +/-0 (exact zero / underflow), or +/-FLT_MAX (overflow).
//   * product-sum ops OR the Z/S/U/O class of every dest lane's exact product (float*float is exact in
//     double) into the sticky bits; OPMSUB includes its w lane (vs.w*vt.w), like the reference.
//   * flag entry: updateFmacFlags (first free slot, ready at +4, calendar bit), skipped for dest == 0.
bool VU1Interpreter::fastFmac(uint32_t instr, float *target, const float *given)
{
    if constexpr (sizeof(long double) != sizeof(double))
        return false;
    VuFmacKind kind;
    VuFmacSrc src;
    uint8_t sel = 0;
    bool toAcc = false;
    if (!vuDecodeFmac(instr, kind, src, sel, toAcc))
    {
        // Upper NOP (special 0x2F/0x30): execUpper only records the instruction.
        if (toAcc && (sel == 0x2Fu || sel == 0x30u))
        {
            m_currentUpperInstruction = instr;
            return true;
        }
        return false;
    }
    m_currentUpperInstruction = instr;
    const uint8_t dest = DEST(instr);
    if (dest == 0u)
        return true; // reference: result computed, no flag entry, no qualification

    const uint8_t fs = FS(instr);
    const uint8_t ft = FT(instr);
    __m128 s = vuNormalize4(_mm_loadu_ps(m_state.vf[fs]));
    __m128 t;
    switch (src)
    {
    case VuSrcBc:
        t = _mm_set1_ps(vuNormalizeOperand(m_state.vf[ft][sel & 3u]));
        break;
    case VuSrcQ:
        t = _mm_set1_ps(vuNormalizeOperand(m_state.q));
        break;
    case VuSrcI:
        t = _mm_set1_ps(vuNormalizeOperand(m_state.i));
        break;
    case VuSrcVt:
        t = vuNormalize4(_mm_loadu_ps(m_state.vf[ft]));
        break;
    default: // cross: x = vs.y*vt.z, y = vs.z*vt.x, z = vs.x*vt.y; w qualification.w*vt.w (product sticky only)
    {
        const __m128 vt = vuNormalize4(_mm_loadu_ps(m_state.vf[ft]));
        s = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
        t = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 1, 0, 2));
        break;
    }
    }

    // Float result first (execUpper's expressions, qualification lane).
    const bool productSum = kind == VuMadd || kind == VuMsub;
    __m128 a = _mm_setzero_ps();
    __m128 product = _mm_setzero_ps();
    __m128 f;
    switch (kind)
    {
    case VuAdd: f = _mm_add_ps(s, t); break;
    case VuSub: f = _mm_sub_ps(s, t); break;
    case VuMul: f = _mm_mul_ps(s, t); break;
    default:
        a = vuNormalize4(_mm_loadu_ps(m_state.acc));
        product = _mm_mul_ps(s, t);
        f = kind == VuMadd ? _mm_add_ps(a, product) : _mm_sub_ps(a, product);
        break;
    }
    if (src == VuSrcCross)
        f = _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(f), _mm_set_epi32(0, -1, -1, -1))); // w = 0.0f
    if (given)
        f = _mm_loadu_ps(given);
    const uint32_t destBits = kVuRev4[dest & 0xFu]; // component order

    // Shortcut lanes whose class provably is "normal, sign only" (proof, with x the real a op b or
    // acc +/- p, s the real acc +/- P, P the exact product, p = chop(P), r the float result, E the
    // reference's double result = chop_double(exact)):
    //   single add/sub/mul, exp(r) in [1,254], |r| != FLT_MAX: FLT_MIN <= |r| <= |E| <= |x| <
    //     next(|r|) <= FLT_MAX and sign(E) = sign(r). r == 0 or denormal is NOT taken (an exact zero vs
    //     an underflow needs the double), nor |r| == FLT_MAX (overflow needs the double).
    //   product-sum, exp(r) in [2,253], p normal, |p| != FLT_MAX, exp(p) <= exp(r) + 21:
    //     |s - x| = |P - p| < ulp(p) <= 2^(exp(r)-129) <= |r|/4, so |s| > (3/4)|r| >= (3/4)2^-125 > FLT_MIN,
    //     |s| < 2^(exp(r)-126) + 2^(exp(r)-129) <= 1.125*2^127 < FLT_MAX, and sign(s) = sign(x) = sign(r);
    //     chop_double keeps all three. The product's own class (sticky) is "sign only" because p is
    //     normal and not FLT_MAX (the single-mul case above). (Same conditions as Scotho/socom-unzipped
    //     productSumFastLanes, GPL-3.0, re-derived here against our double reference.)
    //   Any other dest lane (and OPMULA/OPMSUB's w lane, an exact zero) takes the double path below.
    {
        const __m128i rb = _mm_castps_si128(f);
        const __m128i er = _mm_and_si128(_mm_srli_epi32(rb, 23), _mm_set1_epi32(0xFF));
        const __m128i notMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(rb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                             _mm_set1_epi32(-1));
        __m128i ok;
        if (!productSum)
        {
            ok = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi32(er, _mm_setzero_si128()), _mm_cmplt_epi32(er, _mm_set1_epi32(255))), notMax);
        }
        else
        {
            const __m128i pb = _mm_castps_si128(product);
            const __m128i ep = _mm_and_si128(_mm_srli_epi32(pb, 23), _mm_set1_epi32(0xFF));
            const __m128i pNotMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(pb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                                  _mm_set1_epi32(-1));
            ok = _mm_and_si128(_mm_cmpgt_epi32(er, _mm_set1_epi32(1)), _mm_cmplt_epi32(er, _mm_set1_epi32(254)));
            ok = _mm_and_si128(ok, _mm_and_si128(_mm_cmpgt_epi32(ep, _mm_setzero_si128()), _mm_cmplt_epi32(ep, _mm_set1_epi32(255))));
            ok = _mm_and_si128(ok, _mm_and_si128(pNotMax, _mm_cmpgt_epi32(_mm_add_epi32(er, _mm_set1_epi32(22)), ep)));
        }
        if ((static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(ok))) & destBits) == destBits)
        {
            const uint32_t sign = static_cast<uint32_t>(_mm_movemask_ps(f)) & destBits;
            const uint32_t extraSticky = productSum && (static_cast<uint32_t>(_mm_movemask_ps(product)) & destBits) ? 0x2u : 0u;
            alignas(16) uint32_t valueBits[4];
            _mm_store_ps(reinterpret_cast<float *>(valueBits), f);
            fastFmacCommit(instr, target, toAcc, destBits, valueBits, 0u, sign, 0u, 0u, extraSticky);
            return true;
        }
    }

    // Double path: the reference's exact result and classification.
    const __m128d sLo = _mm_cvtps_pd(s);
    const __m128d sHi = _mm_cvtps_pd(_mm_movehl_ps(s, s));
    const __m128d tLo = _mm_cvtps_pd(t);
    const __m128d tHi = _mm_cvtps_pd(_mm_movehl_ps(t, t));
    __m128d eLo, eHi;
    __m128d pLo = _mm_setzero_pd(), pHi = _mm_setzero_pd();
    switch (kind)
    {
    case VuAdd:
        eLo = _mm_add_pd(sLo, tLo);
        eHi = _mm_add_pd(sHi, tHi);
        break;
    case VuSub:
        eLo = _mm_sub_pd(sLo, tLo);
        eHi = _mm_sub_pd(sHi, tHi);
        break;
    case VuMul:
        eLo = _mm_mul_pd(sLo, tLo);
        eHi = _mm_mul_pd(sHi, tHi);
        break;
    default:
    {
        const __m128d aLo = _mm_cvtps_pd(a);
        const __m128d aHi = _mm_cvtps_pd(_mm_movehl_ps(a, a));
        pLo = _mm_mul_pd(sLo, tLo);
        pHi = _mm_mul_pd(sHi, tHi);
        eLo = kind == VuMadd ? _mm_add_pd(aLo, pLo) : _mm_sub_pd(aLo, pLo);
        eHi = kind == VuMadd ? _mm_add_pd(aHi, pHi) : _mm_sub_pd(aHi, pHi);
        break;
    }
    }
    if (src == VuSrcCross)
        eHi = _mm_unpacklo_pd(eHi, _mm_setzero_pd()); // w: exact +0.0 (reference: 0.0L)

    const VuLaneClass e = vuClassify(eLo, eHi);
    const uint32_t zeroOrUnder = (e.zero | e.under) & destBits;
    const uint32_t over = e.over & destBits;
    const uint32_t under = e.under & destBits;
    const uint32_t sign = e.sign & destBits;
    const __m128i signOnly = _mm_and_si128(vuLaneMask(e.sign), _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i zuMask = vuLaneMask(zeroOrUnder);
    const __m128i overMask = vuLaneMask(over);
    __m128i value = _mm_andnot_si128(_mm_or_si128(zuMask, overMask), _mm_castps_si128(f));
    value = _mm_or_si128(value, _mm_and_si128(zuMask, signOnly));
    value = _mm_or_si128(value, _mm_and_si128(overMask, _mm_or_si128(signOnly, _mm_set1_epi32(0x7F7FFFFF))));

    uint32_t extraSticky = 0u;
    if (productSum)
    {
        const VuLaneClass p = vuClassify(pLo, pHi);
        extraSticky = (((p.zero | p.under) & destBits) ? 0x1u : 0u) | ((p.sign & destBits) ? 0x2u : 0u) |
                      ((p.under & destBits) ? 0x4u : 0u) | ((p.over & destBits) ? 0x8u : 0u);
    }
    alignas(16) uint32_t valueBits[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(valueBits), value);
    fastFmacCommit(instr, target, toAcc, destBits, valueBits, zeroOrUnder, sign, under, over, extraSticky);
    return true;
}

// The flag entry (updateFmacFlags) and the write-back (applyDest) of one fastFmac op.
inline void VU1Interpreter::fastFmacCommit(uint32_t instr, float *target, bool toAcc, uint32_t destBits, const uint32_t *valueBits,
                                    uint32_t zeroOrUnder, uint32_t sign, uint32_t under, uint32_t over, uint32_t extraSticky)
{
    // updateFmacFlags: MAC = Z | S << 4 | U << 8 | O << 12 in qualification (bit 3 = x); Z includes underflow.
    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry)
    {
        const uint32_t mac = kVuRev4[zeroOrUnder] | (kVuRev4[sign] << 4) | (kVuRev4[under] << 8) | (kVuRev4[over] << 12);
        const uint64_t ready = m_cycle + kFmacLatency;
        entry->readyCycle = ready;
        entry->issueCycle = m_cycle;
        entry->mac = mac;
        entry->status = (zeroOrUnder ? 0x1u : 0u) | (sign ? 0x2u : 0u) | (under ? 0x4u : 0u) | (over ? 0x8u : 0u);
        entry->extraSticky = extraSticky;
        entry->clip = 0u;
        entry->valid = true;
        entry->writesMac = true;
        entry->writesStatus = true;
        entry->writesSticky = false;
        entry->writesClip = false;
        // scheduleCommit(ready, slot), inline.
        m_commitCalendar[ready & 63u] |= uint64_t{1} << static_cast<uint32_t>(entry - m_flagPipeline.data());
        m_commitCycles |= uint64_t{1} << (ready & 63u);
    }
    else
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
    }

    // applyDest: VF0 is immutable; masked 16-byte store of the dest lanes.
    float *dst = target ? target : (toAcc ? m_state.acc : m_state.vf[FD(instr)]);
    if (dst != m_state.vf[0])
    {
        const __m128i mask = vuLaneMask(destBits);
        const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst));
        const __m128i value = _mm_load_si128(reinterpret_cast<const __m128i *>(valueBits));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_or_si128(_mm_andnot_si128(mask, old), _mm_and_si128(mask, value)));
    }
}

// calculatePairReadyCycle for the fast path (after Scotho/socom-unzipped fastReadyCycle, GPL-3.0):
// same checks, except the VF/VI scan is skipped once m_cycle has passed every register's ready cycle
// (maxReady) and ACC is not checked (written with kAccForwardLatency = 1, so m_accReady <= m_cycle
// for every later pair).
inline uint64_t VU1Interpreter::fastReadyCycle(const DecodedInstructionPair &decoded, uint64_t maxReady) const
{
    uint64_t ready = m_cycle;
    if (m_cycle < maxReady)
    {
        const InstructionUsage *usages[2] = {&decoded.upperUsage, &decoded.lowerUsage};
        for (const InstructionUsage *usage : usages)
        {
            for (uint32_t index = 0; index < usage->vfReadCount; ++index)
            {
                // Branch-free qualification (qualification 3 = x .. bit 0 = w); max() is order-independent.
                const VfAccess &access = usage->vfRead[index];
                const uint64_t *vfReady = m_vfReady[access.reg].data();
                const uint32_t lanes = access.lanes;
                const uint64_t x = (lanes & 0x8u) ? vfReady[0] : 0u;
                const uint64_t y = (lanes & 0x4u) ? vfReady[1] : 0u;
                const uint64_t z = (lanes & 0x2u) ? vfReady[2] : 0u;
                const uint64_t w = (lanes & 0x1u) ? vfReady[3] : 0u;
                ready = std::max(ready, std::max(std::max(x, y), std::max(z, w)));
            }
            for (uint32_t mask = usage->viRead & 0xFFFEu; mask; mask &= mask - 1u)
                ready = std::max(ready, m_viReady[vuCtz32(mask)]);
        }
    }
    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::runFast(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory, uint64_t fastEnd, bool &programEnded)
{
    Vu0PairCount pairCount{m_unit == Unit::VU0}; // TEMP(transformers)

    if (m_cycle >= fastEnd || m_stopRequested)
        return;

    // Write queues left by the interpreter (a cut in its tail, then MSCNT): until the last of them is
    // due, a landed write must supersede them the way a newer queued write would (sequence bump).
    uint64_t queuedWritesDue = 0;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            queuedWritesDue = std::max(queuedWritesDue, write.readyCycle);
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            queuedWritesDue = std::max(queuedWritesDue, write.readyCycle);
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            queuedWritesDue = std::max(queuedWritesDue, write.readyCycle);
    uint64_t maxReady = 0;
    for (const auto &lanes : m_vfReady)
        for (const uint64_t ready : lanes)
            maxReady = std::max(maxReady, ready);
    for (const uint64_t ready : m_viReady)
        maxReady = std::max(maxReady, ready);

    // The code image cannot change while a program runs: validate the decoded-pair cache once and use
    // its entries in place (after Scotho/socom-unzipped runFast, GPL-3.0).
    const bool directCache = memory != nullptr &&
        (m_unit == Unit::VU1 ? vuCode == memory->peekVU1CodeForExecution() : vuCode == memory->getVU0Code()) &&
                             codeSize / 8u <= kMaxDecodedPairs;
    if (directCache)
        (void)getDecodedInstructionPairForPc(vuCode, codeSize, memory, 0u);
    DecodedInstructionPair uncached{};

    m_fastVfLogCount = 0;
    m_fastViLogCount = 0;
    for (uint32_t r = 0; r < 32u; ++r)
        for (uint32_t c = 0; c < 4u; ++c)
        {
            m_fastVisVf[r][c] = m_state.vf[r][c];
            m_fastHasVf[r][c] = false;
        }
    for (uint32_t r = 0; r < 16u; ++r)
    {
        m_fastVisVi[r] = m_state.vi[r];
        m_fastHasVi[r] = false;
    }
    m_fast = true;
    // The interpreter loop commits at the top of every pair; here only the first pair needs it: every
    // later m_cycle was reached through advanceOneCycle/advanceTo, which commit at each cycle they stop
    // at, and scheduleCommit never targets the current cycle (the call would find an empty bucket).
    commitReadyPipelines();
    while (m_cycle < fastEnd && !m_stopRequested)
    {
        if (m_state.pc + 8u > codeSize)
            break;

        const DecodedInstructionPair *pair = nullptr;
        if (directCache && (m_state.pc & 7u) == 0u)
        {
            DecodedInstructionPair &slot = m_decodedCodeCache[m_state.pc >> 3];
            if (slot.upperVfShadowReg == 0xFFu)
                slot = decodeInstructionPair(vuCode, m_state.pc);
            pair = &slot;
        }
        else
        {
            uncached = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
            pair = &uncached;
        }
        const DecodedInstructionPair &decoded = *pair;
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = fastReadyCycle(decoded, maxReady);
        bool handBack = false;
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= fastEnd)
            {
                handBack = true; // the interpreter loop redoes this pair's stall from here, identically
                break;
            }
            m_viBranchBackupValid = false; // A stall expires the integer-branch bypass.
            advanceTo(readyCycle);
            readyCycle = fastReadyCycle(decoded, maxReady);
        }
        if (handBack)
            break;

        const bool takingPriorBranch = m_state.branchPending && m_state.branchDelay == 0u;
        const uint32_t priorBranchTarget = m_state.branchTarget;
        const uint32_t viWriteMask = decoded.lowerUsage.viWrite & 0xFFFEu;
        const uint8_t writtenVi = viWriteMask != 0u ? static_cast<uint8_t>(vuCtz32(viWriteMask)) : 0u;
        const int32_t oldVi = writtenVi != 0u ? m_state.vi[writtenVi] : 0;
        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const uint8_t accWrite = decoded.upperUsage.accWrite;

        // The branch-VI bypass records the value the interpreter holds in the written VI register at
        // issue (oldVi). With an earlier write to that register still pending, that is NOT what the
        // fast path's register file holds (the pending write landed early): take the interpreter's view.
        const int32_t bypassOldVi =
            (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead && m_viReady[writtenVi] > m_cycle)
                ? fastViVisible(writtenVi)
                : oldVi;
        if (writtenVi != 0u)
        {
            // Per-register tracking (see the VF block): the previous write commits iff this one is issued at or after
            // its due cycle; then its landed value is the interpreter-visible one.
            const uint64_t dueNew = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency
                                                                                 : decoded.lowerUsage.latency);
            if (!m_fastHasVi[writtenVi] || m_cycle >= m_fastReadyVi[writtenVi])
                m_fastVisVi[writtenVi] = oldVi;
            m_fastReadyVi[writtenVi] = dueNew;
            m_fastHasVi[writtenVi] = true;
        }

        // VF write log (pre-pair qualification) for fastVfVisible / fastMaterializeVf: the writes the
        // interpreter would queue (queueVfWrite), all with the 4-cycle latency.
        {
            // Per-qualification (before the pair executes, m_state.vf still holds the previous landed value): the
            // previous write to a qualification iff this write is issued at or after its due cycle; then its value
            // becomes the interpreter-visible one.
            const auto track = [&](const VfAccess &w)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if ((w.lanes & laneForComponent(c)) == 0u)
                        continue;
                    if (!m_fastHasVf[w.reg][c] || m_cycle >= m_fastIssueVf[w.reg][c] + kFmacLatency)
                        m_fastVisVf[w.reg][c] = m_state.vf[w.reg][c];
                    m_fastIssueVf[w.reg][c] = m_cycle;
                    m_fastHasVf[w.reg][c] = true;
                }
            };
            if (hasUpperWrite)
                track(upperWrite);
            if (hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg))
                track(lowerWrite);
        }

        // OPMULA/OPMSUB read lanes their usage does not declare (the cross wiring), so the interpreter
        // does not stall on them and reads whatever is committed. Give the op exactly that view.
        const auto runUpper = [&]()
        {
            const uint32_t upper = decoded.upper;
            const bool cross = (upper & 0x3Fu) == 0x2Eu ||
                               ((upper & 0x3Cu) == 0x3Cu && ((upper & 3u) | ((upper >> 4) & 0x7Cu)) == 0x2Eu);
            if (!cross)
            {
                if (!fastFmac(upper, nullptr, nullptr))
                    execUpper(upper);
                return;
            }
            float saved[2][4];
            const uint32_t patched = fastPatchCrossOperands(upper, saved);
            if (!fastFmac(upper, nullptr, nullptr))
                execUpper(upper);
            if (patched)
                fastUnpatchCrossOperands(upper, patched, saved);
        };

        if (decoded.iBit)
        {
            runUpper();
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = immediate; // LOI transfers raw bits; arithmetic normalizes at use.
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            // The lower instruction reads (or writes) the upper's destination: it sees the old value,
            // and the upper's result wins over a lower write to the same register.
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf, m_state.vf[decoded.upperVfShadowReg], sizeof(oldVf));
            runUpper();
            std::memcpy(upperVf, m_state.vf[decoded.upperVfShadowReg], sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg], oldVf, sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg], upperVf, sizeof(upperVf));
        }
        else
        {
            runUpper();
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = m_viBranchBackupValid &&
                               writtenVi == m_viBranchBackupReg &&
                               decoded.lowerUsage.delaysNextBranchRead;
        // The interpreter's VI commit narrows to 16 bits.
        if (writtenVi != 0u)
            m_state.vi[writtenVi] = static_cast<int16_t>(m_state.vi[writtenVi]);

        if (m_cycle < queuedWritesDue)
        {
            // Supersede interpreter-queued writes to the same lanes (queueVfWrite/ViWrite/AccWrite's
            // latest-sequence rule), so they cannot overwrite the landed value when they come due.
            if (hasUpperWrite)
            {
                const uint64_t seq = ++m_nextWriteSequence;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((upperWrite.lanes & laneForComponent(c)) != 0u)
                        m_vfLatestWrite[upperWrite.reg][c] = seq;
            }
            if (hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg))
            {
                const uint64_t seq = ++m_nextWriteSequence;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((lowerWrite.lanes & laneForComponent(c)) != 0u)
                        m_vfLatestWrite[lowerWrite.reg][c] = seq;
            }
            if (accWrite != 0u)
            {
                const uint64_t seq = ++m_nextWriteSequence;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((accWrite & laneForComponent(c)) != 0u)
                        m_accLatestWrite[c] = seq;
            }
            if (writtenVi != 0u)
                m_viLatestWrite[writtenVi] = ++m_nextWriteSequence;
        }

        // Scoreboard: markPairWrites, plus the latest due cycle (maxReady for the stall scan skip,
        // m_fastMaxWriteReady for the program-end flush).
        if (hasLowerWrite)
        {
            const uint64_t ready = m_cycle + (decoded.lowerUsage.vfLatency != 0u ? decoded.lowerUsage.vfLatency
                                                                                 : decoded.lowerUsage.latency);
            auto &vfReady = m_vfReady[lowerWrite.reg];
            for (uint32_t lanes = lowerWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                vfReady[3u - vuCtz32(lanes)] = ready;
            maxReady = std::max(maxReady, ready);
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (hasUpperWrite)
        {
            const uint64_t ready = m_cycle + (decoded.upperUsage.vfLatency != 0u ? decoded.upperUsage.vfLatency
                                                                                 : decoded.upperUsage.latency);
            auto &vfReady = m_vfReady[upperWrite.reg];
            for (uint32_t lanes = upperWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                vfReady[3u - vuCtz32(lanes)] = ready;
            maxReady = std::max(maxReady, ready);
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (viWriteMask != 0u)
        {
            const uint64_t ready = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency
                                                                                 : decoded.lowerUsage.latency);
            for (uint32_t mask = viWriteMask; mask; mask &= mask - 1u)
                m_viReady[vuCtz32(mask)] = ready;
            maxReady = std::max(maxReady, ready);
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (accWrite != 0u)
        {
            for (uint32_t lanes = accWrite & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                m_accReady[3u - vuCtz32(lanes)] = m_cycle + kAccForwardLatency;
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, m_cycle + kAccForwardLatency);
        }
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, bypassOldVi);

        ++pairCount.n; // TEMP(transformers): issued pair, excludes stalls
        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (takingPriorBranch)
        {
            // A branch in the delay slot executes one pair at the first target.
            const bool scheduledDelayBranch = m_state.branchPending && m_state.branchDelay == 1u;
            m_state.pc = priorBranchTarget & microAddressMask();
            m_state.branchPending = scheduledDelayBranch;
            m_state.branchDelay = 0u;
        }
        else if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }
    m_fast = false;
    // Handing back mid-program (budget tail, stop, pc out of range): turn the still-pending early
    // writes back into interpreter queue entries, so the interpreter loop continues from exactly its
    // own state. (At a program end the flush makes every write final, so nothing is needed.)
    if (!programEnded)
    {
        fastMaterializeVf();
        fastMaterializeVi();
    }
}

// fastVfVisible for a VI register. VI writes have latency 1 or 4, so commits are not in issue order;
// the interpreter commits a write if no later write to the register was issued before it came due.
// Walking back from the newest write: the first one that is due and was not superseded by its
// successor is the visible one (any older candidate came due earlier, so it was overwritten).
int32_t VU1Interpreter::fastViVisible(uint8_t reg) const
{
    if (!m_fastHasVi[reg] || m_cycle >= m_fastReadyVi[reg])
        return m_state.vi[reg]; // no early write, or the newest one is due and nothing superseded it
    return m_fastVisVi[reg];
}

// Hand-back for VI: like fastMaterializeVf (the newest pending write per register is re-queued, the
// register gets the interpreter-visible value; older pending writes were superseded by it).
void VU1Interpreter::fastMaterializeVi()
{
    for (uint32_t r = 1; r < 16u; ++r)
    {
        if (!m_fastHasVi[r] || m_cycle >= m_fastReadyVi[r])
            continue;
        const int32_t value = m_state.vi[r];
        m_state.vi[r] = m_fastVisVi[r];
        queueViWrite(static_cast<uint8_t>(r), value, static_cast<uint32_t>(m_fastReadyVi[r] - m_cycle));
    }
    m_fastViLogCount = 0;
}

// The value the interpreter would hold in VF lane (reg, component) at m_cycle, given the fast path's
// early-landed writes (log of pre-write values). All VF writes have the 4-cycle latency, so they commit in
// issue order, and the interpreter's queue commits a write only if no later write to the qualification issued
// before it came due (m_vfLatestWrite). So: walking back from the newest write to the lane, every write
// that is still pending, or was superseded by its successor, is invisible; the first committed,
// unsuperseded one is visible, and its value is the pre-write value logged by its successor.
float VU1Interpreter::fastVfVisible(uint8_t reg, uint32_t component, bool &exact) const
{
    exact = true;
    if (!m_fastHasVf[reg][component])
        return m_state.vf[reg][component]; // no fast write to this qualification this run: the register holds the visible value
    if (m_cycle >= m_fastIssueVf[reg][component] + kFmacLatency)
        return m_state.vf[reg][component]; // the newest write is due and nothing superseded it
    return m_fastVisVf[reg][component];
}

uint32_t VU1Interpreter::fastPatchCrossOperands(uint32_t upper, float saved[2][4])
{
    // Lanes read per dest lane: x <- vs.y, vt.z; y <- vs.z, vt.x; z <- vs.x, vt.y; w <- vs.w, vt.w
    // (OPMSUB only: the product sticky of the w lane; OPMULA's w is an exact zero and reads nothing).
    // Declared (decodeUpperUsage): fs = dest lanes, ft = xyz. Lane bits: 8 = x .. 1 = w.
    const uint8_t dest = DEST(upper);
    const bool opmsub = (upper & 0x3Fu) == 0x2Eu;
    uint8_t fsRead = 0u, ftRead = 0u;
    if (dest & 0x8u) { fsRead |= 0x4u; ftRead |= 0x2u; }
    if (dest & 0x4u) { fsRead |= 0x2u; ftRead |= 0x8u; }
    if (dest & 0x2u) { fsRead |= 0x8u; ftRead |= 0x4u; }
    if (opmsub && (dest & 0x1u)) { fsRead |= 0x1u; ftRead |= 0x1u; }
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t regs[2] = {fs, ft};
    const uint8_t undeclared[2] = {static_cast<uint8_t>(fsRead & ~dest), static_cast<uint8_t>(ftRead & ~0xEu)};
    uint32_t patched = 0u;
    float visible[2][4];
    for (uint32_t k = 0; k < 2u; ++k)
    {
        std::memcpy(saved[k], m_state.vf[regs[k]], sizeof(saved[k]));
        std::memcpy(visible[k], m_state.vf[regs[k]], sizeof(visible[k]));
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
            if ((undeclared[k] & lane) == 0u || regs[k] == 0u || m_vfReady[regs[k]][c] <= m_cycle)
                continue; // declared (the scoreboard stalled on it), VF0, or nothing pending
            bool exact = true;
            visible[k][c] = fastVfVisible(regs[k], c, exact);
            if (!exact)
            {
                static thread_local uint32_t s_warn = 0;
                if (s_warn++ < 10)
                    RUNTIME_ERROR("[VU1 fast] OPMULA/OPMSUB operand history beyond the write log, pc=0x" << std::hex
                                  << m_state.pc << std::dec << '\n');
            }
            patched |= static_cast<uint32_t>(lane) << (4u * k);
        }
    }
    // Patch after computing both (fs and ft may be the same register).
    for (uint32_t k = 0; k < 2u; ++k)
        for (uint32_t c = 0; c < 4u; ++c)
            if (patched & (static_cast<uint32_t>(1u << (3u - c)) << (4u * k)))
                m_state.vf[regs[k]][c] = visible[k][c];
    return patched;
}

void VU1Interpreter::fastUnpatchCrossOperands(uint32_t upper, uint32_t patched, const float saved[2][4])
{
    // Restore the patched lanes, except a qualification op itself just wrote (OPMSUB's fd == fs/ft).
    const uint8_t dest = DEST(upper);
    const bool opmsub = (upper & 0x3Fu) == 0x2Eu;
    const uint8_t regs[2] = {FS(upper), FT(upper)};
    for (uint32_t k = 0; k < 2u; ++k)
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
            if ((patched & (static_cast<uint32_t>(lane) << (4u * k))) == 0u)
                continue;
            if (opmsub && regs[k] == FD(upper) && (dest & lane) != 0u)
                continue;
            m_state.vf[regs[k]][c] = saved[k][c];
        }
}

// Hand-back: every VF qualification newest fast write is still pending gets the interpreter-visible value
// back and that write re-queued for its due cycle (it is the lane's latest write, so it commits;
// older pending writes to the qualification superseded and need no entry).
void VU1Interpreter::fastMaterializeVf()
{
    for (uint32_t r = 1; r < 32u; ++r)
    {
        uint8_t done = 0u;
        for (uint32_t c0 = 0; c0 < 4u; ++c0)
        {
            const uint8_t lane0 = static_cast<uint8_t>(1u << (3u - c0));
            if ((done & lane0) != 0u || !m_fastHasVf[r][c0] || m_cycle >= m_fastIssueVf[r][c0] + kFmacLatency)
                continue;
            const uint64_t issue = m_fastIssueVf[r][c0];
            uint8_t mask = 0u;
            float value[4];
            std::memcpy(value, m_state.vf[r], sizeof(value));
            for (uint32_t c = c0; c < 4u; ++c)
            {
                const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
                if (m_fastHasVf[r][c] && m_fastIssueVf[r][c] == issue && (done & lane) == 0u)
                    mask |= lane;
            }
            done |= mask;
            for (uint32_t c = 0; c < 4u; ++c)
                if (mask & (1u << (3u - c)))
                    m_state.vf[r][c] = m_fastVisVf[r][c];
            queueVfWrite(static_cast<uint8_t>(r), mask, value, static_cast<uint32_t>(issue + kFmacLatency - m_cycle));
        }
    }
    m_fastVfLogCount = 0;
}
