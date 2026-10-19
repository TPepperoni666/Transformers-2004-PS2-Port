#pragma once

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopMemory;

    class IopSysclib
    {
    public:
        explicit IopSysclib(IopMemory &memory) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);

    private:
        uint32_t ctypeTableAddress();

        IopMemory &m_memory;
        uint32_t m_ctypeTable = 0u; // lazily allocated 128-byte copy of the SCE sysclib ctype table
    };
}
