#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fmt/format.h>
#include <sstream>
#include <cmath>

namespace ps2recomp
{
    namespace
    {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
        // PSLLVW/PSRLVW/PSRAVW shift rt words 0 and 2 by rs words 0 and 2 (& 31) and sign-extend each 32-bit result into
        // a 64-bit lane (PCSX2 MMI.cpp). The old translation shifted rs by rt and produced four word lanes.
        // shiftExpr0/2 see the values as `mmiT0`/`mmiT2` (uint32_t) and the amounts as `mmiS0`/`mmiS2`; each must yield
        // an int32_t (which is then sign-extended to 64 bits).
        std::string emitMmiVariableWordShift(uint8_t rd, uint8_t rs, uint8_t rt, const char *shiftExpr0, const char *shiftExpr2)
        {
            return fmt::format(
                "{{ __m128i mmiRs = GPR_VEC(ctx, {}); __m128i mmiRt = GPR_VEC(ctx, {}); "
                "uint32_t mmiT0 = (uint32_t)_mm_cvtsi128_si32(mmiRt); "
                "uint32_t mmiT2 = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(mmiRt, 8)); "
                "uint32_t mmiS0 = (uint32_t)_mm_cvtsi128_si32(mmiRs) & 31u; "
                "uint32_t mmiS2 = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(mmiRs, 8)) & 31u; "
                "SET_GPR_VEC(ctx, {}, _mm_set_epi64x((int64_t)({}), (int64_t)({}))); }}",
                rs, rt, rd, shiftExpr2, shiftExpr0);
        }

        // Transformers port (NOT in the GoW patch; semantics from PCSX2 MMI.cpp PMULTH/PMADDH/PMSUBH/PHMADH/PHMSBH):
        // the halfword multiply family writes eight 32-bit results into the 128-bit LO/HI pair in the order
        // LO.UL0, LO.UL1, HI.UL0, HI.UL1, LO.UL2, LO.UL3, HI.UL2, HI.UL3 (= ctx->lo, ctx->hi, ctx->lo1, ctx->hi1 as
        // 2 words each) and rd = { LO.UL0, HI.UL0, LO.UL2, HI.UL2 }. The old translation summed _mm_madd_epi16 pairs
        // into one 64-bit scalar, so the game's colour lerps (pmulth + pmaddh + pmfhl.lh + psrah 7, e.g. sub_003711A0,
        // fn_00555E30, sub_00407270) produced garbage.
        // body must fill `uint32_t mmiP[8]` (slot order above) from `int32_t mmiS[8]`, `int32_t mmiT[8]` (signed
        // halfwords of rs / rt) and, when it accumulates, `uint32_t mmiOld[8]` (current LO/HI in slot order).
        std::string emitMmiHalfwordHiLo(uint8_t rd, uint8_t rs, uint8_t rt, const char *body)
        {
            return fmt::format(
                "{{ alignas(16) int16_t mmiS16[8]; alignas(16) int16_t mmiT16[8]; \n"
                "   _mm_store_si128((__m128i *)mmiS16, GPR_VEC(ctx, {})); _mm_store_si128((__m128i *)mmiT16, GPR_VEC(ctx, {})); \n"
                "   int32_t mmiS[8]; int32_t mmiT[8]; for (int mmiI = 0; mmiI < 8; ++mmiI) {{ mmiS[mmiI] = mmiS16[mmiI]; mmiT[mmiI] = mmiT16[mmiI]; }} \n"
                "   const uint32_t mmiOld[8] = {{ (uint32_t)ctx->lo, (uint32_t)(ctx->lo >> 32), (uint32_t)ctx->hi, (uint32_t)(ctx->hi >> 32), \n"
                "                                (uint32_t)ctx->lo1, (uint32_t)(ctx->lo1 >> 32), (uint32_t)ctx->hi1, (uint32_t)(ctx->hi1 >> 32) }}; \n"
                "   (void)mmiOld; uint32_t mmiP[8]; \n"
                "   {} \n"
                "   ctx->lo = (uint64_t)mmiP[0] | ((uint64_t)mmiP[1] << 32); ctx->hi = (uint64_t)mmiP[2] | ((uint64_t)mmiP[3] << 32); \n"
                "   ctx->lo1 = (uint64_t)mmiP[4] | ((uint64_t)mmiP[5] << 32); ctx->hi1 = (uint64_t)mmiP[6] | ((uint64_t)mmiP[7] << 32); \n"
                "   SET_GPR_VEC(ctx, {}, _mm_setr_epi32((int)mmiP[0], (int)mmiP[2], (int)mmiP[4], (int)mmiP[6])); }}",
                rs, rt, body, rd);
        }
    }

    std::string CodeGenerator::translateMMI0Instruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        uint8_t rs = inst.rs;
        uint8_t rt = inst.rt;
        uint8_t rd = inst.rd;
        switch (subfunc)
        {
        case MMI0_PADDW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PADDW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PSUBW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PCGTW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCGTW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PMAXW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PMAXW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PADDH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PADDH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PSUBH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PCGTH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCGTH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PMAXH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PMAXH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PADDB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PADDB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PSUBB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PCGTB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCGTB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PADDSW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PADDSW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBSW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PSUBSW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PEXTLW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTLW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PPACW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PPACW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PADDSH:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_adds_epi16(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBSH:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_subs_epi16(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PEXTLH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTLH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PPACH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PPACH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PADDSB:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_adds_epi8(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PSUBSB:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_subs_epi8(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PEXTLB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTLB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PPACB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PPACB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI0_PEXT5:
            return translatePEXT5(inst);
        case MMI0_PPAC5:
            return translatePPAC5(inst);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled MMI0 instruction: function 0x{:X}", subfunc));
        }
    }


    std::string CodeGenerator::translateMMI1Instruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        uint8_t rs = inst.rs;
        uint8_t rt = inst.rt;
        uint8_t rd = inst.rd;
        switch (subfunc)
        {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
        // PABSW/PABSH read rt (not rs) and saturate the minimum (0x80000000 -> 0x7FFFFFFF, 0x8000 -> 0x7FFF) like the
        // R5900 (PCSX2 MMI.cpp _PABSW/_PABSH). Emitted inline so the fix does not depend on ps2_runtime_macros.h
        // (whose PS2_PABSW/PS2_PABSH are plain _mm_abs_*).
        case MMI1_PABSW:
            return fmt::format("{{ __m128i mmiAbs = _mm_abs_epi32(GPR_VEC(ctx, {})); "
                               "SET_GPR_VEC(ctx, {}, _mm_xor_si128(mmiAbs, _mm_cmpeq_epi32(mmiAbs, _mm_set1_epi32(INT32_MIN)))); }}",
                               rt, rd);
        case MMI1_PCEQW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCEQW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PMINW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PMINW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PADSBH:
            return translatePADSBH(inst);
        case MMI1_PABSH:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0) - operand rt, 0x8000 saturates.
            return fmt::format("{{ __m128i mmiAbs = _mm_abs_epi16(GPR_VEC(ctx, {})); "
                               "SET_GPR_VEC(ctx, {}, _mm_xor_si128(mmiAbs, _mm_cmpeq_epi16(mmiAbs, _mm_set1_epi16(INT16_MIN)))); }}",
                               rt, rd);
        case MMI1_PCEQH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCEQH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PMINH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PMINH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PCEQB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCEQB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PADDUW:
            return fmt::format(
                "SET_GPR_VEC(ctx, {}, ps2_paddu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PSUBUW:
            return fmt::format(
                "SET_GPR_VEC(ctx, {}, ps2_psubu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PEXTUW:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTUW(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
        // PADDUH/PSUBUH saturate unsigned (0..0xFFFF), as PCSX2 _PADDUH/_PSUBUH; they used to wrap.
        case MMI1_PADDUH:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_adds_epu16(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PSUBUH:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_subs_epu16(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PEXTUH:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTUH(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PADDUB:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_adds_epu8(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PSUBUB:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_subs_epu8(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_PEXTUB:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PEXTUB(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI1_QFSRV:
            return translateQFSRV(inst);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled MMI1 instruction: function 0x{:X}", subfunc));
        }
    }


    std::string CodeGenerator::translateMMI2Instruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        uint8_t rs = inst.rs;
        uint8_t rt = inst.rt;
        uint8_t rd = inst.rd;
        switch (subfunc)
        {
        case MMI2_PMADDW:
            return translatePMADDW(inst);
        case MMI2_PSLLVW:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
            return emitMmiVariableWordShift(rd, rs, rt, "(int32_t)(mmiT0 << mmiS0)", "(int32_t)(mmiT2 << mmiS2)");
        case MMI2_PSRLVW:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
            return emitMmiVariableWordShift(rd, rs, rt, "(int32_t)(mmiT0 >> mmiS0)", "(int32_t)(mmiT2 >> mmiS2)");
        case MMI2_PMSUBW:
            return translatePMSUBW(inst);
        // Transformers port (NOT in the GoW patch; PCSX2 MMI.cpp PMFHI/PMFLO): HI/LO are 128-bit on the R5900
        // (upper halves = ctx->hi1/lo1). The old code copied only the low 64 bits and left rd's upper half stale.
        case MMI2_PMFHI:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_set_epi64x((int64_t)ctx->hi1, (int64_t)ctx->hi));", rd);
        case MMI2_PMFLO:
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_set_epi64x((int64_t)ctx->lo1, (int64_t)ctx->lo));", rd);
        case MMI2_PINTH:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
            // PINTH interleaves the LOW four halfwords of rt with the HIGH four halfwords of rs:
            // rd.US = { rt0, rs4, rt1, rs5, rt2, rs6, rt3, rs7 } (PCSX2 MMI.cpp).
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_unpacklo_epi16(GPR_VEC(ctx, {}), _mm_srli_si128(GPR_VEC(ctx, {}), 8)));",
                               rd, rt, rs);
        case MMI2_PMULTW:
            return translatePMULTW(inst);
        case MMI2_PDIVW:
            return translatePDIVW(inst);
        case MMI2_PCPYLD:
            return translatePCPYLD(inst);
        case MMI2_PAND:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PAND(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI2_PXOR:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PXOR(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI2_PMADDH:
            return translatePMADDH(inst);
        case MMI2_PHMADH:
            return translatePHMADH(inst);
        case MMI2_PMSUBH:
            return translatePMSUBH(inst);
        case MMI2_PHMSBH:
            return translatePHMSBH(inst);
        case MMI2_PEXEH:
            return translatePEXEH(inst);
        case MMI2_PREVH:
            return translatePREVH(inst);
        case MMI2_PMULTH:
            return translatePMULTH(inst);
        case MMI2_PDIVBW:
            return translatePDIVBW(inst);
        case MMI2_PEXEW:
            return translatePEXEW(inst);
        case MMI2_PROT3W:
            return translatePROT3W(inst);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled MMI2 instruction: function 0x{:X}", subfunc));
        }
    }


    std::string CodeGenerator::translateMMI3Instruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        uint8_t rs = inst.rs;
        uint8_t rt = inst.rt;
        uint8_t rd = inst.rd;
        switch (subfunc)
        {
        case MMI3_PMADDUW:
            return translatePMADDUW(inst);
        case MMI3_PSRAVW:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
            return emitMmiVariableWordShift(rd, rs, rt, "((int32_t)mmiT0 >> mmiS0)", "((int32_t)mmiT2 >> mmiS2)");
        case MMI3_PMTHI:
            return translatePMTHI(inst);
        case MMI3_PMTLO:
            return translatePMTLO(inst);
        case MMI3_PINTEH:
            // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
            // PINTEH interleaves the EVEN halfwords: rd.US = { rt0, rs0, rt2, rs2, rt4, rs4, rt6, rs6 } (PCSX2 MMI.cpp).
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_or_si128(_mm_and_si128(GPR_VEC(ctx, {}), _mm_set1_epi32(0x0000FFFF)), "
                               "_mm_slli_epi32(GPR_VEC(ctx, {}), 16)));",
                               rd, rt, rs);
        case MMI3_PMULTUW:
            return translatePMULTUW(inst);
        case MMI3_PDIVUW:
            return translatePDIVUW(inst);
        case MMI3_PCPYUD:
            return translatePCPYUD(inst);
        case MMI3_POR:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_POR(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI3_PNOR:
            return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PNOR(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));", rd, rs, rt);
        case MMI3_PEXCH:
            return translatePEXCH(inst);
        case MMI3_PCPYH:
            return translatePCPYH(inst);
        case MMI3_PEXCW:
            return translatePEXCW(inst);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled MMI3 instruction: function 0x{:X}", subfunc));
        }
    }


    // Transformers port (NOT in the GoW patch; semantics from PCSX2 MMI.cpp PMFHL): HI/LO are 128-bit
    // (LO = ctx->lo | ctx->lo1 << 64, HI = ctx->hi | ctx->hi1 << 64). The old PS2_PMFHL_* macros only saw the low
    // 64 bits and LH used a saturating pack (LH truncates). Emitted inline, independent of ps2_runtime_macros.h.
    std::string CodeGenerator::translatePMFHLInstruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        switch (subfunc)
        {
        case PMFHL_LW: // rd.UL = { LO.UL0, HI.UL0, LO.UL2, HI.UL2 }
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_setr_epi32((int)(uint32_t)ctx->lo, (int)(uint32_t)ctx->hi, "
                               "(int)(uint32_t)ctx->lo1, (int)(uint32_t)ctx->hi1));",
                               inst.rd);
        case PMFHL_UW: // rd.UL = { LO.UL1, HI.UL1, LO.UL3, HI.UL3 }
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_setr_epi32((int)(uint32_t)(ctx->lo >> 32), (int)(uint32_t)(ctx->hi >> 32), "
                               "(int)(uint32_t)(ctx->lo1 >> 32), (int)(uint32_t)(ctx->hi1 >> 32)));",
                               inst.rd);
        case PMFHL_SLW: // rd.UD[i] = clamp_s32((s64)(HI.UL[2i] << 32 | LO.UL[2i]))
            return fmt::format("{{ auto mmiSlw = [](uint64_t mmiLo, uint64_t mmiHi) -> int64_t {{ "
                               "const int64_t mmiV = (int64_t)(((uint64_t)(uint32_t)mmiHi << 32) | (uint64_t)(uint32_t)mmiLo); "
                               "if (mmiV >= 0x7FFFFFFFLL) return 0x7FFFFFFFLL; "
                               "if (mmiV <= -0x80000000LL) return -0x80000000LL; "
                               "return (int64_t)(int32_t)(uint32_t)mmiLo; }}; "
                               "SET_GPR_VEC(ctx, {}, _mm_set_epi64x(mmiSlw(ctx->lo1, ctx->hi1), mmiSlw(ctx->lo, ctx->hi))); }}",
                               inst.rd);
        case PMFHL_LH: // rd.US = { LO.US0, LO.US2, HI.US0, HI.US2, LO.US4, LO.US6, HI.US4, HI.US6 } (truncating)
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_setr_epi16("
                               "(int16_t)(uint16_t)ctx->lo, (int16_t)(uint16_t)(ctx->lo >> 32), "
                               "(int16_t)(uint16_t)ctx->hi, (int16_t)(uint16_t)(ctx->hi >> 32), "
                               "(int16_t)(uint16_t)ctx->lo1, (int16_t)(uint16_t)(ctx->lo1 >> 32), "
                               "(int16_t)(uint16_t)ctx->hi1, (int16_t)(uint16_t)(ctx->hi1 >> 32)));",
                               inst.rd);
        case PMFHL_SH: // rd.US = sat16 of { LO.UL0, LO.UL1, HI.UL0, HI.UL1, LO.UL2, LO.UL3, HI.UL2, HI.UL3 } (signed)
            return fmt::format("SET_GPR_VEC(ctx, {}, _mm_packs_epi32(_mm_set_epi64x((int64_t)ctx->hi, (int64_t)ctx->lo), "
                               "_mm_set_epi64x((int64_t)ctx->hi1, (int64_t)ctx->lo1)));",
                               inst.rd);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled PMFHL instruction: function 0x{:X}", subfunc));
        }
    }


    std::string CodeGenerator::translatePMTHLInstruction(const Instruction &inst)
    {
        uint8_t subfunc = inst.sa;
        switch (subfunc)
        {
        case PMFHL_LW:
            // Transformers port (NOT in the GoW patch; PCSX2 MMI.cpp PMTHL): LO.UL0 = rs.UL0, HI.UL0 = rs.UL1,
            // LO.UL2 = rs.UL2, HI.UL2 = rs.UL3; the odd words of LO/HI are preserved. The old code sign-extended rs.UL0
            // over all of ctx->lo and never wrote the upper halves.
            return fmt::format("{{ alignas(16) uint32_t mmiW[4]; _mm_store_si128((__m128i *)mmiW, GPR_VEC(ctx, {})); "
                               "ctx->lo = (ctx->lo & 0xFFFFFFFF00000000ull) | mmiW[0]; ctx->hi = (ctx->hi & 0xFFFFFFFF00000000ull) | mmiW[1]; "
                               "ctx->lo1 = (ctx->lo1 & 0xFFFFFFFF00000000ull) | mmiW[2]; ctx->hi1 = (ctx->hi1 & 0xFFFFFFFF00000000ull) | mmiW[3]; }}",
                               inst.rs);
        default:
            return emitUnhandledInstruction(inst, fmt::format("Unhandled PMTHL instruction: function 0x{:X}", subfunc));
        }
    }


    std::string CodeGenerator::translatePEXT5(const Instruction &inst)
    {
        return fmt::format(
            "{{ __m128i rt = GPR_VEC(ctx, {}); \n"
            "   __m128i m1 = _mm_set1_epi32(0x0000001F); \n"
            "   __m128i m2 = _mm_set1_epi32(0x000003E0); \n"
            "   __m128i m3 = _mm_set1_epi32(0x00007C00); \n"
            "   __m128i m4 = _mm_set1_epi32(0x00008000); \n"
            "   __m128i a1 = _mm_slli_epi32(_mm_and_si128(rt, m1), 3); \n"
            "   __m128i a2 = _mm_slli_epi32(_mm_and_si128(rt, m2), 6); \n"
            "   __m128i a3 = _mm_slli_epi32(_mm_and_si128(rt, m3), 9); \n"
            "   __m128i a4 = _mm_slli_epi32(_mm_and_si128(rt, m4), 16); \n"
            "   SET_GPR_VEC(ctx, {}, _mm_or_si128(_mm_or_si128(a1, a2), _mm_or_si128(a3, a4))); }}",
            inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePPAC5(const Instruction &inst)
    {
        return fmt::format(
            "{{ __m128i rt = GPR_VEC(ctx, {}); \n"
            "   __m128i m1 = _mm_set1_epi32(0x0000001F); \n"
            "   __m128i m2 = _mm_set1_epi32(0x000003E0); \n"
            "   __m128i m3 = _mm_set1_epi32(0x00007C00); \n"
            "   __m128i m4 = _mm_set1_epi32(0x00008000); \n"
            "   __m128i a1 = _mm_and_si128(_mm_srli_epi32(rt, 3), m1); \n"
            "   __m128i a2 = _mm_and_si128(_mm_srli_epi32(rt, 6), m2); \n"
            "   __m128i a3 = _mm_and_si128(_mm_srli_epi32(rt, 9), m3); \n"
            "   __m128i a4 = _mm_and_si128(_mm_srli_epi32(rt, 16), m4); \n"
            "   SET_GPR_VEC(ctx, {}, _mm_or_si128(_mm_or_si128(a1, a2), _mm_or_si128(a3, a4))); }}",
            inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePADSBH(const Instruction &inst)
    {
        return fmt::format(
            "{{ __m128i rs = GPR_VEC(ctx, {}); __m128i rt = GPR_VEC(ctx, {}); \n"
            "   __m128i sub = _mm_sub_epi16(rs, rt); \n"
            "   __m128i add = _mm_add_epi16(rs, rt); \n"
            "   SET_GPR_VEC(ctx, {}, _mm_unpacklo_epi64(sub, _mm_unpackhi_epi64(add, add))); }}",
            inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePMADDW(const Instruction &inst)
    {
        return fmt::format("{{ __m128i p01 = _mm_mul_epu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})); \n"
                           "   __m128i p23 = _mm_mul_epu32(_mm_srli_si128(GPR_VEC(ctx, {}), 8), _mm_srli_si128(GPR_VEC(ctx, {}), 8)); \n"
                           "   uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); \n"
                           "   acc += _mm_cvtsi128_si64(p01); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p01, 8)); \n"
                           "   acc += _mm_cvtsi128_si64(p23); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p23, 8)); \n"
                           "   ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32); \n"
                           "   SET_GPR_U64(ctx, {}, acc); }}",
                           inst.rs, inst.rt, inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePMSUBW(const Instruction &inst)
    {
        return fmt::format("{{ __m128i p01 = _mm_mul_epu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})); \n"
                           "   __m128i p23 = _mm_mul_epu32(_mm_srli_si128(GPR_VEC(ctx, {}), 8), _mm_srli_si128(GPR_VEC(ctx, {}), 8)); \n"
                           "   uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); \n"
                           "   acc -= _mm_cvtsi128_si64(p01); \n"
                           "   acc -= _mm_cvtsi128_si64(_mm_srli_si128(p01, 8)); \n"
                           "   acc -= _mm_cvtsi128_si64(p23); \n"
                           "   acc -= _mm_cvtsi128_si64(_mm_srli_si128(p23, 8)); \n"
                           "   ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32); \n"
                           "   SET_GPR_U64(ctx, {}, acc); }}",
                           inst.rs, inst.rt, inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePMULTW(const Instruction &inst)
    {
        return fmt::format("{{ __m128i p01 = _mm_mul_epu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})); \n"
                           "   __m128i p23 = _mm_mul_epu32(_mm_srli_si128(GPR_VEC(ctx, {}), 8), _mm_srli_si128(GPR_VEC(ctx, {}), 8)); \n"
                           "   uint64_t acc = 0; \n"
                           "   acc += _mm_cvtsi128_si64(p01); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p01, 8)); \n"
                           "   acc += _mm_cvtsi128_si64(p23); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p23, 8)); \n"
                           "   ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32); \n"
                           "   SET_GPR_U64(ctx, {}, acc); }}",
                           inst.rs, inst.rt, inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePMADDUW(const Instruction &inst)
    {
        return fmt::format("{{ __m128i p01 = _mm_mul_epu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})); \n"
                           "   __m128i p23 = _mm_mul_epu32(_mm_srli_si128(GPR_VEC(ctx, {}), 8), _mm_srli_si128(GPR_VEC(ctx, {}), 8)); \n"
                           "   uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); \n"
                           "   acc += _mm_cvtsi128_si64(p01); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p01, 8)); \n"
                           "   acc += _mm_cvtsi128_si64(p23); \n"
                           "   acc += _mm_cvtsi128_si64(_mm_srli_si128(p23, 8)); \n"
                           "   ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32); \n"
                           "   SET_GPR_U64(ctx, {}, acc); }}",
                           inst.rs, inst.rt, inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePDIVW(const Instruction &inst)
    {
        // Only divides the first word element rs[0] / rt[0]
        return fmt::format("{{ int32_t rs0 = GPR_S32(ctx, {}); int32_t rt0 = GPR_S32(ctx, {}); \n"
                           "   if (rt0 != 0) {{ \n"
                           "       if (rt0 == -1 && rs0 == INT32_MIN) {{ ctx->lo = (uint32_t)INT32_MIN; ctx->hi = 0; }} \n"
                           "       else {{ ctx->lo = (uint32_t)(rs0 / rt0); ctx->hi = (uint32_t)(rs0 % rt0); }} \n"
                           "   }} else {{ ctx->lo = (rs0 < 0) ? 1 : -1; ctx->hi = (uint32_t)rs0; }} \n"
                           "   SET_GPR_U32(ctx, {}, ctx->lo); }}",
                           inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePCPYLD(const Instruction &inst)
    {
        // PCPYLD uses rs as the upper source and rt as the lower source.
        return fmt::format("SET_GPR_VEC(ctx, {}, PS2_PCPYLD(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));",
                           inst.rd, inst.rs, inst.rt);
    }


    // Transformers port (NOT in the GoW patch): PMADDH/PHMADH/PMSUBH/PHMSBH/PMULTH rewritten from PCSX2 MMI.cpp, see
    // emitMmiHalfwordHiLo above. All arithmetic is done in uint32_t (the hardware wraps; avoids signed-overflow UB).
    std::string CodeGenerator::translatePMADDH(const Instruction &inst)
    {
        // LO/HI slot i += rs.SS[i] * rt.SS[i]
        return emitMmiHalfwordHiLo(inst.rd, inst.rs, inst.rt,
                                   "for (int mmiI = 0; mmiI < 8; ++mmiI) mmiP[mmiI] = mmiOld[mmiI] + (uint32_t)(mmiS[mmiI] * mmiT[mmiI]);");
    }


    std::string CodeGenerator::translatePHMADH(const Instruction &inst)
    {
        // slot 2k = p(2k+1) + p(2k), slot 2k+1 = p(2k+1)   (p(i) = rs.SS[i] * rt.SS[i])
        return emitMmiHalfwordHiLo(inst.rd, inst.rs, inst.rt,
                                   "for (int mmiK = 0; mmiK < 4; ++mmiK) { "
                                   "const uint32_t mmiOdd = (uint32_t)(mmiS[2 * mmiK + 1] * mmiT[2 * mmiK + 1]); "
                                   "const uint32_t mmiEven = (uint32_t)(mmiS[2 * mmiK] * mmiT[2 * mmiK]); "
                                   "mmiP[2 * mmiK] = mmiOdd + mmiEven; mmiP[2 * mmiK + 1] = mmiOdd; }");
    }


    std::string CodeGenerator::translatePMSUBH(const Instruction &inst)
    {
        // LO/HI slot i -= rs.SS[i] * rt.SS[i]
        return emitMmiHalfwordHiLo(inst.rd, inst.rs, inst.rt,
                                   "for (int mmiI = 0; mmiI < 8; ++mmiI) mmiP[mmiI] = mmiOld[mmiI] - (uint32_t)(mmiS[mmiI] * mmiT[mmiI]);");
    }


    std::string CodeGenerator::translatePHMSBH(const Instruction &inst)
    {
        // slot 2k = p(2k+1) - p(2k), slot 2k+1 = ~p(2k+1) (undocumented, per PCSX2)
        return emitMmiHalfwordHiLo(inst.rd, inst.rs, inst.rt,
                                   "for (int mmiK = 0; mmiK < 4; ++mmiK) { "
                                   "const uint32_t mmiOdd = (uint32_t)(mmiS[2 * mmiK + 1] * mmiT[2 * mmiK + 1]); "
                                   "const uint32_t mmiEven = (uint32_t)(mmiS[2 * mmiK] * mmiT[2 * mmiK]); "
                                   "mmiP[2 * mmiK] = mmiOdd - mmiEven; mmiP[2 * mmiK + 1] = ~mmiOdd; }");
    }


    // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0)
    // Halfword/word permutations corrected to the R5900 order (PCSX2 MMI.cpp), low lane first:
    // PEXEH [2,1,0,3 | 6,5,4,7], PREVH [3,2,1,0 | 7,6,5,4], PEXCH [0,2,1,3 | 4,6,5,7] (halfwords),
    // PEXEW [2,1,0,3], PROT3W [1,2,0,3], PEXCW [0,2,1,3] (words). Emitted inline (no dependency on the
    // PS2_PEXEH/PS2_PEXEW/PS2_PROT3W macros in ps2_runtime_macros.h, which still hold the old wrong orders).
    std::string CodeGenerator::translatePEXEH(const Instruction &inst)
    {
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shufflelo_epi16(_mm_shufflehi_epi16(GPR_VEC(ctx, {}), _MM_SHUFFLE(3,0,1,2)), _MM_SHUFFLE(3,0,1,2)));",
                           inst.rd, inst.rt);
    }


    std::string CodeGenerator::translatePREVH(const Instruction &inst)
    {
        // Reverses the halfwords within each 64-bit half (not across the whole register).
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shufflelo_epi16(_mm_shufflehi_epi16(GPR_VEC(ctx, {}), _MM_SHUFFLE(0,1,2,3)), _MM_SHUFFLE(0,1,2,3)));",
                           inst.rd, inst.rt);
    }


    std::string CodeGenerator::translatePMULTH(const Instruction &inst)
    {
        // Transformers port (NOT in the GoW patch): LO/HI slot i = rs.SS[i] * rt.SS[i] (PCSX2 MMI.cpp PMULTH).
        return emitMmiHalfwordHiLo(inst.rd, inst.rs, inst.rt,
                                   "for (int mmiI = 0; mmiI < 8; ++mmiI) mmiP[mmiI] = (uint32_t)(mmiS[mmiI] * mmiT[mmiI]);");
    }


    std::string CodeGenerator::translatePDIVBW(const Instruction &inst)
    {
        return fmt::format(
            "{{\n"
            "    __m128i rsVec = GPR_VEC(ctx, {});\n"
            "    __m128i rtVec = GPR_VEC(ctx, {});\n"
            "    alignas(16) int32_t rsWords[4];\n"
            "    alignas(16) int32_t rtWords[4];\n"
            "    _mm_store_si128((__m128i*)rsWords, rsVec);\n"
            "    _mm_store_si128((__m128i*)rtWords, rtVec);\n"
            "    int32_t div = rtWords[0];\n"
            "    int32_t q0 = 0, q1 = 0, q2 = 0, q3 = 0;\n"
            "    if (div != 0) {{\n"
            "        q0 = rsWords[0] / div; ctx->lo = (uint32_t)q0; ctx->hi = (uint32_t)(rsWords[0] % div);\n"
            "        q1 = rsWords[1] / div;\n"
            "        q2 = rsWords[2] / div;\n"
            "        q3 = rsWords[3] / div;\n"
            "    }} else {{\n"
            "        ctx->lo = (rsWords[0] < 0) ? 1 : -1;\n"
            "        ctx->hi = (uint32_t)rsWords[0];\n"
            "    }}\n"
            "    SET_GPR_VEC(ctx, {}, _mm_set_epi32(q3, q2, q1, q0));\n"
            "}}",
            inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePEXEW(const Instruction &inst)
    {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0): words [2,1,0,3].
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shuffle_epi32(GPR_VEC(ctx, {}), _MM_SHUFFLE(3,0,1,2)));",
                           inst.rd, inst.rt);
    }


    std::string CodeGenerator::translatePROT3W(const Instruction &inst)
    {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0): rotates the low three words,
        // rd.UL = { rt1, rt2, rt0, rt3 } (word 3 unchanged).
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shuffle_epi32(GPR_VEC(ctx, {}), _MM_SHUFFLE(3,0,2,1)));",
                           inst.rd, inst.rt);
    }


    std::string CodeGenerator::translatePMULTUW(const Instruction &inst)
    {
        // Parallel multiply unsigned word -> results to HI/LO and rd (lower 32 bits)
        return fmt::format("{{ __m128i p01 = _mm_mul_epu32(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})); \n"
                           "   __m128i p23 = _mm_mul_epu32(_mm_srli_si128(GPR_VEC(ctx, {}), 8), _mm_srli_si128(GPR_VEC(ctx, {}), 8)); \n"
                           "   uint64_t res0 = _mm_cvtsi128_si64(p01); uint64_t res1 = _mm_cvtsi128_si64(_mm_srli_si128(p01, 8)); \n"
                           "   uint64_t res2 = _mm_cvtsi128_si64(p23); uint64_t res3 = _mm_cvtsi128_si64(_mm_srli_si128(p23, 8)); \n"
                           "   ctx->lo = (uint32_t)res0; ctx->hi = (uint32_t)(res0 >> 32); \n" // HI/LO from first product only
                           "   SET_GPR_VEC(ctx, {}, _mm_set_epi32((uint32_t)res3, (uint32_t)res2, (uint32_t)res1, (uint32_t)res0)); }}",
                           inst.rs, inst.rt, inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePDIVUW(const Instruction &inst)
    {
        // Parallel divide unsigned word (only first element) -> results to HI/LO and rd (quotient)
        return fmt::format("{{ uint32_t rs0 = GPR_U32(ctx, {}); uint32_t rt0 = GPR_U32(ctx, {}); \n"
                           "   if (rt0 != 0) {{ ctx->lo = rs0 / rt0; ctx->hi = rs0 % rt0; }} \n"
                           "   else {{ ctx->lo = 0xFFFFFFFF; ctx->hi = rs0; }} \n" // Div by zero behavior
                           "   SET_GPR_U32(ctx, {}, ctx->lo); }}",
                           inst.rs, inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePCPYUD(const Instruction &inst)
    {
        // Copies upper 64 of rs to lower 64 of rd, upper 64 of rt to upper 64 of rd
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_unpackhi_epi64(GPR_VEC(ctx, {}), GPR_VEC(ctx, {})));",
                           inst.rd, inst.rs, inst.rt); // Order matters
    }


    std::string CodeGenerator::translatePEXCH(const Instruction &inst)
    {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0): halfwords [0,2,1,3 | 4,6,5,7]
        // (swaps the centre halfwords of each doubleword; it is NOT the same as PEXEH).
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shufflelo_epi16(_mm_shufflehi_epi16(GPR_VEC(ctx, {}), _MM_SHUFFLE(3,1,2,0)), _MM_SHUFFLE(3,1,2,0)));",
                           inst.rd, inst.rt);
    }


    std::string CodeGenerator::translatePCPYH(const Instruction &inst)
    {
        // Parallel Copy Halfword (Broadcast lower 16 bits of each 64-bit half)
        return fmt::format("{{ __m128i src = GPR_VEC(ctx, {}); uint16_t l = _mm_extract_epi16(src, 0); uint16_t h = _mm_extract_epi16(src, 4); \n"
                           "   SET_GPR_VEC(ctx, {}, _mm_set_epi16(h,h,h,h, l,l,l,l)); }}",
                           inst.rt, inst.rd);
    }


    std::string CodeGenerator::translatePEXCW(const Instruction &inst)
    {
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-mmi.patch (GPL-3.0): swaps the centre words,
        // rd.UL = { rt0, rt2, rt1, rt3 }.
        return fmt::format("SET_GPR_VEC(ctx, {}, _mm_shuffle_epi32(GPR_VEC(ctx, {}), _MM_SHUFFLE(3,1,2,0)));",
                           inst.rd, inst.rt);
    }


    // Transformers port (NOT in the GoW patch; PCSX2 MMI.cpp PMTHI/PMTLO): HI/LO are 128-bit, the whole rs is copied
    // (HI.UD0 = rs.UD0 -> ctx->hi, HI.UD1 = rs.UD1 -> ctx->hi1). The old code zero-extended rs.UL0 into ctx->hi only.
    std::string CodeGenerator::translatePMTHI(const Instruction &inst)
    {
        return fmt::format("{{ __m128i mmiV = GPR_VEC(ctx, {}); ctx->hi = (uint64_t)_mm_cvtsi128_si64(mmiV); "
                           "ctx->hi1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(mmiV, mmiV)); }}",
                           inst.rs);
    }


    std::string CodeGenerator::translatePMTLO(const Instruction &inst)
    {
        return fmt::format("{{ __m128i mmiV = GPR_VEC(ctx, {}); ctx->lo = (uint64_t)_mm_cvtsi128_si64(mmiV); "
                           "ctx->lo1 = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(mmiV, mmiV)); }}",
                           inst.rs);
    }


    std::string CodeGenerator::translateQFSRV(const Instruction &inst)
    {
        uint8_t rd = inst.rd;
        uint8_t rs = inst.rs;
        uint8_t rt = inst.rt;
        // QFSRV semantics are centralized in runtime macro helpers.
        return fmt::format("SET_GPR_VEC(ctx, {}, PS2_QFSRV(GPR_VEC(ctx, {}), GPR_VEC(ctx, {}), ctx->sa & 0x7F));",
                           rd, rs, rt);
    }

}
