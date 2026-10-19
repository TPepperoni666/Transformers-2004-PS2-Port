#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "iop_printf.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        switch (ordinal)
        {
        case 4: // printf
        {
            const std::string text = iopFormat(m_memory, a0, IopVarArgs::fromCall(m_memory, cpu, 1u));
            m_host.log(LogLevel::Info, "[IOP printf] " + text);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf(fd, fmt, ...)
        {
            const std::string text = iopFormat(m_memory, a1, IopVarArgs::fromCall(m_memory, cpu, 2u));
            m_host.log(LogLevel::Info, "[IOP fdprintf] " + text);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf(fd, fmt, va_list)
        {
            const std::string text = iopFormat(m_memory, a1, IopVarArgs::fromVaList(m_memory, cpu.gpr[6]));
            m_host.log(LogLevel::Info, "[IOP vfdprintf] " + text);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        default:
            return false;
        }
    }
}
