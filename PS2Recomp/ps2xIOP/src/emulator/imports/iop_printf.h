#pragma once

// printf-family formatting for IOP imports (sysclib sprintf/vsprintf, stdio printf/fdprintf/vfdprintf,
// sysmem Kprintf).  The previous implementation only copied the *format string*, so every message built with
// %s/%d (SIRENS3 builds its stream paths, tone names and log lines with sprintf) came out as literal "%s".
// MIPS o32 variadic ABI: named+variadic integer args go in a0-a3 then at sp+16, sp+20, ...; 64-bit values are
// 8-byte aligned (they skip an odd register/slot).  va_list is a char* into the same slot layout.

#include <cstdint>
#include <string>

namespace ps2x::iop::detail
{
    class IopMemory;
    struct IopCpuState;

    class IopVarArgs
    {
    public:
        // `fixed` = number of named integer arguments before the "...", e.g. 2 for sprintf(buf, fmt, ...).
        static IopVarArgs fromCall(const IopMemory &memory, const IopCpuState &cpu, unsigned fixed);
        // `vaList` = the va_list argument of vsprintf/vfdprintf (address of the next 4-byte slot).
        static IopVarArgs fromVaList(const IopMemory &memory, uint32_t vaList);

        [[nodiscard]] uint32_t nextU32();
        [[nodiscard]] uint64_t nextU64();

    private:
        const IopMemory *m_memory = nullptr;
        const IopCpuState *m_cpu = nullptr;
        uint32_t m_index = 0; // slot index counted from a0 (call) or from the va_list base
        uint32_t m_base = 0;  // va_list base address (0 for register-based)
    };

    // Formats the guest format string at `formatAddress`.  Never throws; output is clamped to 8 KiB.
    [[nodiscard]] std::string iopFormat(const IopMemory &memory, uint32_t formatAddress, IopVarArgs args);
}
