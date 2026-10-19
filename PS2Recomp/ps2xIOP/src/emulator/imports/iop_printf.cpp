#include "iop_printf.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ps2x::iop::detail
{
    IopVarArgs IopVarArgs::fromCall(const IopMemory &memory, const IopCpuState &cpu, unsigned fixed)
    {
        IopVarArgs args;
        args.m_memory = &memory;
        args.m_cpu = &cpu;
        args.m_index = fixed;
        return args;
    }

    IopVarArgs IopVarArgs::fromVaList(const IopMemory &memory, uint32_t vaList)
    {
        IopVarArgs args;
        args.m_memory = &memory;
        args.m_base = vaList;
        return args;
    }

    uint32_t IopVarArgs::nextU32()
    {
        const uint32_t slot = m_index++;
        if (m_base != 0u)
            return m_memory->read32(m_base + slot * 4u);
        if (slot < 4u)
            return m_cpu->gpr[4u + slot];
        return m_memory->read32(m_cpu->gpr[29] + 16u + (slot - 4u) * 4u);
    }

    uint64_t IopVarArgs::nextU64()
    {
        m_index = (m_index + 1u) & ~1u; // 64-bit values are 8-byte aligned in o32
        const uint64_t lo = nextU32();
        const uint64_t hi = nextU32();
        return lo | (hi << 32u); // little endian: low word first
    }

    namespace
    {
        constexpr size_t kMaxOutput = 8192u;

        void appendFormatted(std::string &out, const std::string &spec, long long value)
        {
            char buffer[128];
            const int n = std::snprintf(buffer, sizeof(buffer), spec.c_str(), value);
            if (n > 0)
                out.append(buffer, static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof(buffer) - 1u))));
        }
    }

    std::string iopFormat(const IopMemory &memory, uint32_t formatAddress, IopVarArgs args)
    {
        std::string out;
        const std::string format = memory.readString(formatAddress, 4096u);
        for (size_t i = 0; i < format.size() && out.size() < kMaxOutput; ++i)
        {
            const char ch = format[i];
            if (ch != '%')
            {
                out.push_back(ch);
                continue;
            }
            if (++i >= format.size())
                break;

            std::string flags;
            while (i < format.size() && std::strchr("-+ #0", format[i]) != nullptr)
                flags.push_back(format[i++]);

            int width = -1;
            if (i < format.size() && format[i] == '*')
            {
                width = static_cast<int32_t>(args.nextU32());
                if (width < 0)
                {
                    flags.push_back('-');
                    width = -width;
                }
                ++i;
            }
            else
            {
                int w = 0;
                bool any = false;
                while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                {
                    w = w * 10 + (format[i++] - '0');
                    any = true;
                }
                if (any)
                    width = w;
            }

            int precision = -1;
            if (i < format.size() && format[i] == '.')
            {
                ++i;
                precision = 0;
                if (i < format.size() && format[i] == '*')
                {
                    precision = static_cast<int32_t>(args.nextU32());
                    ++i;
                }
                else
                {
                    while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                        precision = precision * 10 + (format[i++] - '0');
                }
            }

            int longCount = 0;
            bool shortArg = false;
            bool charArg = false;
            while (i < format.size() && std::strchr("hlLqjzt", format[i]) != nullptr)
            {
                switch (format[i])
                {
                case 'l':
                case 'q':
                case 'L':
                case 'j':
                    ++longCount;
                    break;
                case 'h':
                    if (shortArg)
                        charArg = true;
                    shortArg = true;
                    break;
                default:
                    break;
                }
                ++i;
            }
            if (i >= format.size())
                break;

            const char conv = format[i];
            std::string spec = "%" + flags;
            if (width >= 0)
                spec += std::to_string(width);
            if (precision >= 0)
                spec += "." + std::to_string(precision);

            switch (conv)
            {
            case '%':
                out.push_back('%');
                break;
            case 'd':
            case 'i':
            {
                long long value;
                if (longCount >= 2)
                    value = static_cast<long long>(args.nextU64());
                else
                {
                    value = static_cast<int32_t>(args.nextU32());
                    if (charArg)
                        value = static_cast<int8_t>(value);
                    else if (shortArg)
                        value = static_cast<int16_t>(value);
                }
                appendFormatted(out, spec + "lld", value);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            {
                unsigned long long value;
                if (longCount >= 2)
                    value = args.nextU64();
                else
                {
                    value = args.nextU32();
                    if (charArg)
                        value &= 0xFFu;
                    else if (shortArg)
                        value &= 0xFFFFu;
                }
                appendFormatted(out, spec + "ll" + conv, static_cast<long long>(value));
                break;
            }
            case 'p':
                appendFormatted(out, "%08llx", static_cast<long long>(args.nextU32()));
                break;
            case 'c':
            {
                const char c = static_cast<char>(args.nextU32());
                const int pad = std::max(0, width - 1);
                if (flags.find('-') != std::string::npos)
                {
                    out.push_back(c);
                    out.append(static_cast<size_t>(pad), ' ');
                }
                else
                {
                    out.append(static_cast<size_t>(pad), ' ');
                    out.push_back(c);
                }
                break;
            }
            case 's':
            {
                const uint32_t pointer = args.nextU32();
                std::string text = pointer == 0u ? std::string("(null)") : memory.readString(pointer, 4096u);
                if (precision >= 0 && static_cast<size_t>(precision) < text.size())
                    text.resize(static_cast<size_t>(precision));
                const size_t pad = width > 0 && static_cast<size_t>(width) > text.size()
                                       ? static_cast<size_t>(width) - text.size()
                                       : 0u;
                if (flags.find('-') != std::string::npos)
                {
                    out += text;
                    out.append(pad, ' ');
                }
                else
                {
                    out.append(pad, ' ');
                    out += text;
                }
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            {
                // Soft-float code passes a double in two slots; render it with the host printf.
                const uint64_t bits = args.nextU64();
                double value;
                std::memcpy(&value, &bits, sizeof(value));
                char buffer[128];
                const int n = std::snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), value);
                if (n > 0)
                    out.append(buffer, static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof(buffer) - 1u))));
                break;
            }
            case 'n':
                (void)args.nextU32(); // %n is ignored
                break;
            default:
                out.push_back('%');
                out.push_back(conv);
                break;
            }
        }
        if (out.size() > kMaxOutput)
            out.resize(kMaxOutput);
        return out;
    }
}
