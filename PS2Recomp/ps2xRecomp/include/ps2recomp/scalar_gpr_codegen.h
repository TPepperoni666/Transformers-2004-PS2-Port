#ifndef PS2RECOMP_SCALAR_GPR_CODEGEN_H
#define PS2RECOMP_SCALAR_GPR_CODEGEN_H
#include <regex>
#include <string>

namespace ps2recomp
{
    inline std::string lowerMemoryReads(std::string code)
    {
        const auto signatureEnd = code.find("PS2Runtime *runtime) {\n");
        if (signatureEnd == std::string::npos) return code;
        const auto insertion = code.rfind("void ", signatureEnd);
        if (insertion == std::string::npos) return code;
        bool changed = false;
        for (int width : {8, 16, 32, 64, 128})
        {
            const std::regex pattern("\\bREAD" + std::to_string(width) + "\\(");
            if (std::regex_search(code, pattern))
            {
                changed = true;
                code = std::regex_replace(code, pattern,
                    "ps2recomp_inline_memory::read" + std::to_string(width) + "(rdram, ctx, runtime, ");
            }
        }
        if (!changed) return code;
        std::string helpers =
            "#ifndef PS2RECOMP_INLINE_MEMORY_HELPERS\n#define PS2RECOMP_INLINE_MEMORY_HELPERS\n"
            "#if defined(_MSC_VER)\n#define PS2RECOMP_MEMORY_INLINE __forceinline\n"
            "#elif defined(__GNUC__) || defined(__clang__)\n#define PS2RECOMP_MEMORY_INLINE inline __attribute__((always_inline))\n"
            "#else\n#define PS2RECOMP_MEMORY_INLINE inline\n#endif\n"
            "namespace ps2recomp_inline_memory {\n";
        for (int width : {8, 16, 32, 64, 128})
        {
            const std::string w = std::to_string(width);
            const std::string type = width == 128 ? "__m128i" : "uint" + w + "_t";
            helpers += "static PS2RECOMP_MEMORY_INLINE " + type + " read" + w +
                "(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t addr) { ";
            if (width == 128) helpers += "addr &= ~0xFu; ";
            helpers += "return PS2Runtime::isSpecialAddress(addr) ? runtime->Load" + w +
                "(rdram, ctx, addr) : Ps2FastRead" + w + "(rdram, addr); }\n";
        }
        helpers += "}\n#undef PS2RECOMP_MEMORY_INLINE\n#endif\n";
        code.insert(insertion, helpers);
        return code;
    }

    // Lower only literal nonzero GPR operands. Register zero retains the original
    // macro, including its suppression of value-expression side effects.
    inline std::string lowerScalarGprAccess(std::string code)
    {
        const auto originalSignature = code.find("PS2Runtime *runtime) {\n");
        if (originalSignature == std::string::npos || code.rfind("void ", originalSignature) == std::string::npos) return code;
        const char *names[] = {"GPR_U32", "GPR_S32", "GPR_U64", "GPR_S64",
                              "SET_GPR_U32", "SET_GPR_S32", "SET_GPR_ZE32",
                              "SET_GPR_U64", "SET_GPR_S64"};
        const char *locals[] = {"ee_gpr_u32", "ee_gpr_s32", "ee_gpr_u64", "ee_gpr_s64",
                               "ee_set_s32", "ee_set_s32", "ee_set_ze32",
                               "ee_set_u64", "ee_set_u64"};
        bool changed = false;
        for (unsigned i = 0; i < 9; ++i)
        {
            const std::regex pattern(std::string("\\b") + names[i] +
                "\\(ctx, ([1-9]|[12][0-9]|3[01])" + (i < 4 ? "\\)" : ", "));
            if (std::regex_search(code, pattern))
            {
                changed = true;
                code = std::regex_replace(code, pattern,
                    std::string("ps2recomp_scalar_gpr::") + locals[i] +
                    "<$1>(ctx" + (i < 4 ? ")" : ", "));
            }
        }
        if (!changed) return code;
        const auto signatureEnd = code.find("PS2Runtime *runtime) {\n");
        if (signatureEnd == std::string::npos) return code;
        const auto insertion = code.rfind("void ", signatureEnd);
        if (insertion == std::string::npos) return code;
        code.insert(insertion,
            "#ifndef PS2RECOMP_SCALAR_GPR_HELPERS\n#define PS2RECOMP_SCALAR_GPR_HELPERS\n"
            "#if defined(_MSC_VER)\n#define PS2RECOMP_GPR_INLINE __forceinline\n"
            "#elif defined(__GNUC__) || defined(__clang__)\n#define PS2RECOMP_GPR_INLINE inline __attribute__((always_inline))\n"
            "#else\n#define PS2RECOMP_GPR_INLINE inline\n#endif\n"
            "namespace ps2recomp_scalar_gpr {\n"
            "// Scalar object-representation access; upper 64 bits stay untouched.\n"
            "template<int r> static PS2RECOMP_GPR_INLINE uint32_t ee_gpr_u32(R5900Context* ctx) { uint32_t v; std::memcpy(&v, &ctx->r[r], 4); return v; }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE int32_t ee_gpr_s32(R5900Context* ctx) { int32_t v; std::memcpy(&v, &ctx->r[r], 4); return v; }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE uint64_t ee_gpr_u64(R5900Context* ctx) { uint64_t v; std::memcpy(&v, &ctx->r[r], 8); return v; }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE int64_t ee_gpr_s64(R5900Context* ctx) { int64_t v; std::memcpy(&v, &ctx->r[r], 8); return v; }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE void ee_set_s32(R5900Context* ctx, int32_t v) { const int64_t w = v; std::memcpy(&ctx->r[r], &w, 8); }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE void ee_set_ze32(R5900Context* ctx, uint32_t v) { const uint64_t w = v; std::memcpy(&ctx->r[r], &w, 8); }\n"
            "template<int r> static PS2RECOMP_GPR_INLINE void ee_set_u64(R5900Context* ctx, uint64_t v) { std::memcpy(&ctx->r[r], &v, 8); }\n"
            "}\n#undef PS2RECOMP_GPR_INLINE\n#endif\n");
        return code;
    }
}
#endif
