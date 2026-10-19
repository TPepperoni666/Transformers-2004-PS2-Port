#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fmt/format.h>
#include <sstream>
#include <cmath>

namespace ps2recomp
{
    std::string CodeGenerator::translateVU_VADD_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUB_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMUL_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADD(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMUL(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VDIV(const Instruction &inst)
    {
        uint8_t fsf = inst.vectorInfo.fsf;
        uint8_t ftf = inst.vectorInfo.ftf;
        uint8_t fs_reg = inst.rd;
        uint8_t ft_reg = inst.rt;

        return fmt::format("{{ float fs = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); float ft = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); ctx->vu0_q = (ft != 0.0f) ? (fs / ft) : (((copysignf(1.0f, fs) * copysignf(1.0f, ft)) < 0.0f) ? -3.402823466e+38f : 3.402823466e+38f); }}", fs_reg, fs_reg, fsf, ft_reg, ft_reg, ftf);
    }

    std::string CodeGenerator::translateVU_VSQRT(const Instruction &inst)
    {
        uint8_t ftf = inst.vectorInfo.ftf;
        uint8_t ft_reg = inst.rt;
        return fmt::format("{{ float ft = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); ctx->vu0_q = sqrtf(fabsf(ft)); }}", ft_reg, ft_reg, ftf);
    }

    std::string CodeGenerator::translateVU_VRSQRT(const Instruction &inst)
    {
        // VRSQRT Q, fs.fsf, ft.ftf computes fs.fsf / sqrt(|ft.ftf|) (the numerator was ignored before).
        uint8_t fsf = inst.vectorInfo.fsf;
        uint8_t ftf = inst.vectorInfo.ftf;
        uint8_t fs_reg = inst.rd;
        uint8_t ft_reg = inst.rt;
        return fmt::format("{{ float fs = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); "
                           "float ft = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); "
                           "ctx->vu0_q = (ft != 0.0f) ? (fs / sqrtf(fabsf(ft))) : (((copysignf(1.0f, fs) * copysignf(1.0f, ft)) < 0.0f) ? (fs == 0.0f ? -0.0f : -3.402823466e+38f) : (fs == 0.0f ? 0.0f : 3.402823466e+38f)); }}",
                           fs_reg, fs_reg, fsf, ft_reg, ft_reg, ftf);
    }

    std::string CodeGenerator::translateVU_VMTIR(const Instruction &inst)
    {
        uint8_t fsf = inst.vectorInfo.fsf;
        if (inst.rt == 0)
            return "// VMTIR to vi0 ignored";
        return fmt::format("{{ uint32_t bits; float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); std::memcpy(&bits, &src, sizeof(bits)); ctx->vi[{}] = (uint16_t)(bits & 0xFFFF); }}", inst.rd, inst.rd, fsf, inst.rt);
    }

    std::string CodeGenerator::translateVU_VMFIR(const Instruction &inst)
    {
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        if (inst.rt == 0)
            return "// VMFIR to vf0 ignored";
        return fmt::format("{{ uint32_t tmp = (uint32_t)(int32_t)(int16_t)ctx->vi[{}]; float val; std::memcpy(&val, &tmp, sizeof(val)); "
                           "__m128 res = _mm_set1_ps(val); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           inst.rd,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VILWR(const Instruction &inst)
    {
        // VILWR.<f> vit, (vis): VU0 data memory at 0x11004000 + vis*16; field x/y/z/w = +0/4/8/12, last set field wins.
        const uint8_t m = inst.vectorInfo.vectorField;
        if (inst.rt == 0 || m == 0)
            return "// VILWR to vi0 / empty field: no-op";
        const uint32_t off = (m & 0x1) ? 12u : (m & 0x2) ? 8u : (m & 0x4) ? 4u : 0u;
        return fmt::format("{{ uint32_t addr = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u) + {}u; ctx->vi[{}] = READ16(addr); }}", inst.rd, off, inst.rt);
    }

    std::string CodeGenerator::translateVU_VISWR(const Instruction &inst)
    {
        // VISWR.<f> vit, (vis): stores vit zero-extended into each selected 32-bit field.
        const uint8_t m = inst.vectorInfo.vectorField;
        std::string s = fmt::format("{{ uint32_t base = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u); uint32_t v = (uint32_t)ctx->vi[{}]; ", inst.rd, inst.rt);
        if (m & 0x8)
            s += "WRITE32(base + 0u, v); ";
        if (m & 0x4)
            s += "WRITE32(base + 4u, v); ";
        if (m & 0x2)
            s += "WRITE32(base + 8u, v); ";
        if (m & 0x1)
            s += "WRITE32(base + 12u, v); ";
        return s + "}";
    }

    std::string CodeGenerator::translateVU_VIADD(const Instruction &inst)
    {
        if (inst.sa == 0)
            return "// VIADD to vi0 ignored";
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VISUB(const Instruction &inst)
    {
        if (inst.sa == 0)
            return "// VISUB to vi0 ignored";
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] - ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIADDI(const Instruction &inst)
    {
        int32_t imm5 = (inst.sa & 0x10) ? static_cast<int32_t>(inst.sa | ~0x1F) : static_cast<int32_t>(inst.sa);
        if (inst.rt == 0)
            return "// VIADDI to vi0 ignored";
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + {};", inst.rt, inst.rd, imm5); // vit, vis, imm5
    }

    std::string CodeGenerator::translateVU_VIAND(const Instruction &inst)
    {
        if (inst.sa == 0)
            return "// VIAND to vi0 ignored";
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] & ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIOR(const Instruction &inst)
    {
        if (inst.sa == 0)
            return "// VIOR to vi0 ignored";
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] | ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VCALLMS(const Instruction &inst)
    {
        // VCALLMS calls a VU0 microprogram at the specified immediate address.
        // VU0 micro memory is 4KB = 512 instructions (8 bytes each). Index is 0-511.
        uint16_t instr_index = static_cast<uint16_t>((inst.raw >> 6) & 0x1FF); // imm15[8:0]
        uint32_t target_byte_addr = static_cast<uint32_t>(instr_index) << 3;   // Convert instruction index to byte address

        return fmt::format(
            "{{ "
            "    ctx->vu0_tpc = 0x{:X}; " // Set target program counter
            "    runtime->executeVU0Microprogram(rdram, ctx, 0x{:X}); "
            "}}",
            target_byte_addr, target_byte_addr);
    }

    std::string CodeGenerator::translateVU_VCALLMSR(const Instruction &inst)
    {
        // VCALLMSR always starts the VU0 microprogram at CMSAR0 (its fs field is hard-wired to 27). CTC2 vi27 stores
        // CMSAR0 in ctx->vu0_cmsar0; ctx->vi[] only has 16 entries.
        (void)inst;
        return std::string(
            "{ "
            "    uint16_t instr_index = (uint16_t)(ctx->vu0_cmsar0 & 0x1FF); "
            "    uint32_t target_byte_addr = (uint32_t)instr_index << 3; "
            "    ctx->vu0_pc = target_byte_addr; "
            "    runtime->vu0StartMicroProgram(rdram, ctx, target_byte_addr); "
            "}");
    }

    std::string CodeGenerator::translateVU_VRNEXT(const Instruction &inst)
    {
        // PCSX2 _vuRNEXT: no-op for ft==0 (R not advanced); otherwise advance the 23-bit LFSR (taps 4 and 22) and
        // store R = 0x3F800000 | (lfsr & 0x7FFFFF) into the selected fields of ft. R lives in vu0_r lane 0.
        if (inst.rt == 0)
            return "// VRNEXT to vf0: no-op";
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ uint32_t r = (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)); "
                           "uint32_t x = (r >> 4) & 1u, y = (r >> 22) & 1u; r = (r << 1) ^ x ^ y; "
                           "r = (r & 0x7FFFFFu) | 0x3F800000u; ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32((int)r)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], ctx->vu0_r, _mm_castsi128_ps(mask)); }}",
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VMADD_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3; // Extract field from function code

        // Pre-construct the shuffle pattern to avoid format string issues
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUB_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3; // Extract field from function code

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINI_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAX_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADD(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADDq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADDi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAX(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAXi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINIi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMULi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMULq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VOPMSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 fs_yzx = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,0,2,1)); "
                           "__m128 ft_zxy = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,1,0,2)); "
                           "__m128 mul_res = PS2_VMUL(fs_yzx, ft_zxy); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vfs, vft, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINI(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUBi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUBq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUBq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUBi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VOPMULA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 fs_yzx = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,0,2,1)); "
                           "__m128 ft_zxy = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,1,0,2)); "
                           "__m128 res = PS2_VMUL(fs_yzx, ft_zxy); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vfs, vft, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VITOF(const Instruction &inst, int shift)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        float scale = (shift == 0) ? 1.0f : (1.0f / static_cast<float>(1 << shift));

        return fmt::format("{{ __m128i src = _mm_castps_si128(ctx->vu0_vf[{}]); "
                           "__m128 res = _mm_cvtepi32_ps(src); "
                           "res = _mm_mul_ps(res, _mm_set1_ps({})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, codegen::formatFloatLiteral(scale),
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VFTOI(const Instruction &inst, int shift)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        float scale = (shift == 0) ? 1.0f : static_cast<float>(1 << shift);

        return fmt::format("{{ __m128 src = ctx->vu0_vf[{}]; "
                           "src = _mm_mul_ps(src, _mm_set1_ps({})); "
                           "__m128i res_i = _mm_cvttps_epi32(src); "
                           "__m128i bits_ = _mm_castps_si128(src); " /* PCSX2 floatToInt: |x| >= 2^31 saturates to 0x7FFFFFFF / 0x80000000 */
                           "__m128i big_ = _mm_cmpgt_epi32(_mm_and_si128(bits_, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x4EFFFFFF)); "
                           "__m128i sat_ = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), _mm_srai_epi32(bits_, 31)); "
                           "res_i = _mm_blendv_epi8(res_i, sat_, big_); "
                           "__m128 res = _mm_castsi128_ps(res_i); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, codegen::formatFloatLiteral(scale),
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    // Macro-mode VU0 data memory is mapped at 0x11004000 (4KB, 16-byte units), not low EE RAM.
    // LQI/LQD: address/increment VI = is (rd), destination VF = ft (rt).
    // SQI/SQD: address/increment VI = it (rt), source VF = fs (rd). vf0/vi0 are never written.
    std::string CodeGenerator::translateVU_VLQI(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        std::string load = vft == 0 ? std::string() :
            fmt::format("__m128 res = _mm_castsi128_ps(READ128(addr)); "
                        "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                        "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); ",
                        (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                        (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                        vft, vft);
        std::string inc = vis == 0 ? std::string() : fmt::format("ctx->vi[{0}] = (uint16_t)(ctx->vi[{0}] + 1); ", vis);
        return fmt::format("{{ uint32_t addr = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u); (void)addr; {}{}}}", vis, load, inc);
    }

    std::string CodeGenerator::translateVU_VSQI(const Instruction &inst)
    {
        uint8_t vit = inst.rt;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        std::string inc = vit == 0 ? std::string() : fmt::format("ctx->vi[{0}] = (uint16_t)(ctx->vi[{0}] + 1); ", vit);
        return fmt::format("{{ uint32_t addr = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u); "
                           "__m128i old_val = READ128(addr); "
                           "__m128 res = _mm_blendv_ps(_mm_castsi128_ps(old_val), ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set_epi32({}, {}, {}, {}))); "
                           "WRITE128(addr, _mm_castps_si128(res)); "
                           "{}}}",
                           vit,
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inc);
    }

    std::string CodeGenerator::translateVU_VLQD(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        std::string dec = vis == 0 ? std::string() : fmt::format("ctx->vi[{0}] = (uint16_t)(ctx->vi[{0}] - 1); ", vis);
        std::string load = vft == 0 ? std::string() :
            fmt::format("__m128 res = _mm_castsi128_ps(READ128(addr)); "
                        "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                        "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); ",
                        (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                        (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                        vft, vft);
        return fmt::format("{{ {}uint32_t addr = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u); (void)addr; {}}}", dec, vis, load);
    }

    std::string CodeGenerator::translateVU_VSQD(const Instruction &inst)
    {
        uint8_t vit = inst.rt;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        std::string dec = vit == 0 ? std::string() : fmt::format("ctx->vi[{0}] = (uint16_t)(ctx->vi[{0}] - 1); ", vit);
        return fmt::format("{{ {}uint32_t addr = 0x11004000u + (((uint32_t)ctx->vi[{}] << 4) & 0xFF0u); "
                           "__m128i old_val = READ128(addr); "
                           "__m128 res = _mm_blendv_ps(_mm_castsi128_ps(old_val), ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set_epi32({}, {}, {}, {}))); "
                           "WRITE128(addr, _mm_castps_si128(res)); }}",
                           dec,
                           vit,
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0);
    }

    std::string CodeGenerator::translateVU_VRGET(const Instruction &inst)
    {
        if (inst.rt == 0)
            return "// VRGET to vf0: no-op";
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t ft_reg = inst.rt;
        return fmt::format("{{ __m128 res = _mm_castsi128_ps(_mm_set1_epi32(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)))); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, ft_reg, ft_reg);
    }

    std::string CodeGenerator::translateVU_VRINIT(const Instruction &inst)
    {
        // PCSX2 _vuRINIT: R = 0x3F800000 | (fs.fsf & 0x7FFFFF).
        uint8_t fs_reg = inst.rd;
        uint8_t fsf = inst.vectorInfo.fsf;
        return fmt::format("{{ float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); "
                           "uint32_t seed; std::memcpy(&seed, &src, sizeof(seed)); "
                           "ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32((int)(0x3F800000u | (seed & 0x007FFFFFu)))); }}",
                           fs_reg, fs_reg, fsf);
    }

    std::string CodeGenerator::translateVU_VRXOR(const Instruction &inst)
    {
        // PCSX2 _vuRXOR: R = 0x3F800000 | ((R ^ fs.fsf) & 0x7FFFFF).
        uint8_t fs_reg = inst.rd;
        uint8_t fsf = inst.vectorInfo.fsf;
        return fmt::format("{{ float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); "
                           "uint32_t src_bits; std::memcpy(&src_bits, &src, sizeof(src_bits)); "
                           "uint32_t r = (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)); "
                           "ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32((int)(0x3F800000u | ((r ^ src_bits) & 0x007FFFFFu)))); }}",
                           fs_reg, fs_reg, fsf);
    }

}
