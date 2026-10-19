// printf-family formatting for IOP imports: sysclib sprintf/vsprintf (o32 variadic ABI incl. stack args).
#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_sysclib.h"

#include <cstring>
#include <iostream>
#include <string>

using namespace ps2x::iop::detail;

namespace
{
    bool expect(bool condition, const char *message)
    {
        if (!condition)
            std::cerr << "FAILED: " << message << '\n';
        return condition;
    }

    void put(IopMemory &mem, uint32_t address, const std::string &text)
    {
        for (size_t i = 0; i <= text.size(); ++i)
            mem.write8(address + static_cast<uint32_t>(i), i < text.size() ? static_cast<uint8_t>(text[i]) : 0u);
    }

    constexpr uint32_t kBuf = 0x40000u;
    constexpr uint32_t kFmt = 0x41000u;
    constexpr uint32_t kStr = 0x42000u;
    constexpr uint32_t kSp = 0x7F000u;
}

int main()
{
    IopMemory mem;
    IopSysclib sysclib(mem);
    bool ok = true;

    // sprintf(buf, "cdrom0:\\%s;1", name): the stream-path case that motivated the change.
    {
        put(mem, kFmt, "cdrom0:\\%s;1");
        put(mem, kStr, "SOUND\\MUSIC\\AMAZON.MS");
        IopCpuState cpu;
        cpu.gpr[4] = kBuf;
        cpu.gpr[5] = kFmt;
        cpu.gpr[6] = kStr;
        ok &= expect(sysclib.dispatchImport(19u, cpu), "sprintf handled");
        ok &= expect(mem.readString(kBuf) == "cdrom0:\\SOUND\\MUSIC\\AMAZON.MS;1", "sprintf %s");
        ok &= expect(cpu.gpr[2] == mem.readString(kBuf).size(), "sprintf returns length");
    }

    // Five variadic ints: a2, a3 then sp+16, sp+20, sp+24; width/flags/hex.
    {
        put(mem, kFmt, "[%d|%5d|%-4d|%04x|%X|%u|%c|%%]");
        mem.write32(kSp + 16u, 0xBEEFu);
        mem.write32(kSp + 20u, 4000000000u);
        mem.write32(kSp + 24u, static_cast<uint32_t>('Z'));
        IopCpuState cpu;
        cpu.gpr[29] = kSp;
        cpu.gpr[4] = kBuf;
        cpu.gpr[5] = kFmt;
        cpu.gpr[6] = static_cast<uint32_t>(-7);
        cpu.gpr[7] = 42u;
        // args: -7, 42, then stack: (%-4d) needs another -> use stack slots: 0xBEEF is %-4d? keep it simple below
        ok &= expect(sysclib.dispatchImport(19u, cpu), "sprintf multi handled");
        // slots: a2=-7 (%d), a3=42 (%5d), sp+16=0xBEEF (%-4d), sp+20=4000000000 (%04x), sp+24='Z' (%X), then 0s
        const std::string got = mem.readString(kBuf);
        ok &= expect(got.rfind("[-7|   42|48879|", 0) == 0, "sprintf d/5d/-4d prefix");
        ok &= expect(got.find("|ee6b2800|") != std::string::npos, "sprintf %04x of 4e9 truncated to 32 bit hex");
        ok &= expect(got.find("|5A|") != std::string::npos, "sprintf %X");
    }

    // vsprintf with a va_list in memory: "%s=%d", {ptr, 123}
    {
        put(mem, kFmt, "%s=%d");
        put(mem, kStr, "volume");
        constexpr uint32_t va = 0x43000u;
        mem.write32(va, kStr);
        mem.write32(va + 4u, 123u);
        IopCpuState cpu;
        cpu.gpr[4] = kBuf;
        cpu.gpr[5] = kFmt;
        cpu.gpr[6] = va;
        ok &= expect(sysclib.dispatchImport(42u, cpu), "vsprintf handled");
        ok &= expect(mem.readString(kBuf) == "volume=123", "vsprintf %s=%d");
    }

    // 64-bit value alignment: sprintf(buf, "%d %lld", 5, 0x100000002) -> a2=5, a3 skipped, 64-bit at sp+16/sp+20
    {
        put(mem, kFmt, "%d %lld");
        mem.write32(kSp + 16u, 2u);
        mem.write32(kSp + 20u, 1u);
        IopCpuState cpu;
        cpu.gpr[29] = kSp;
        cpu.gpr[4] = kBuf;
        cpu.gpr[5] = kFmt;
        cpu.gpr[6] = 5u;
        ok &= expect(sysclib.dispatchImport(19u, cpu), "sprintf lld handled");
        ok &= expect(mem.readString(kBuf) == "5 4294967298", "sprintf %lld 8-byte alignment");
    }

    // %.3s precision, %*d star width, (null)
    {
        put(mem, kFmt, "%.3s|%*d|%s");
        put(mem, kStr, "abcdef");
        mem.write32(kSp + 16u, 77u);
        mem.write32(kSp + 20u, 0u);
        IopCpuState cpu;
        cpu.gpr[29] = kSp;
        cpu.gpr[4] = kBuf;
        cpu.gpr[5] = kFmt;
        cpu.gpr[6] = kStr;
        cpu.gpr[7] = 6u; // width for %*d; value comes from sp+16
        ok &= expect(sysclib.dispatchImport(19u, cpu), "sprintf precision/star handled");
        ok &= expect(mem.readString(kBuf) == "abc|    77|(null)", "sprintf %.3s %*d (null)");
    }

    if (!ok)
        return 1;
    std::cout << "ps2xIOP printf tests passed\n";
    return 0;
}
