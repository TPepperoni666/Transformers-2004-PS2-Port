#pragma once
// TEMP(transformers) qualification (2026-10-10, maintainer's v0.7.0 playtest mark 9 = "OUT OF MEMORY TDG_MemoryManager.cpp(430)").
//
// EE kernel heap semantics. On a real PS2, SetupHeap(start, -1) ends the heap at the calling thread's STACK BASE and
// EndOfHeap() returns that address (BIOS kernel; Play! PS2OS.cpp sc_SetupHeap/sc_EndOfHeap do the same). Transformers'
// crt0 calls SetupThread(stack=0x1FE0000, size=0x20000) then SetupHeap(_end=0x743080, -1) -> heap end 0x1FE0000, and its
// sbrk() (0x11FAE0) refuses to grow past EndOfHeap(). The runtime used a FIXED 0x1F00000 end because it keeps its own
// RPC/TLS/boot-mode pools at 0x1F00000-0x1F31000 and its interrupt/callback invocation stacks just below the main stack
// (0x1FD8000-0x1FE0000). So the game's malloc arena was 0xE0000 = 896 KB smaller than on hardware, and the game's own
// TDG heap - sized ONCE at boot from the largest malloc() it can get (0x1071D0) - came out at 23.55 MB instead of the
// PS2's 24.42 MB. A memory-heavy moment (Antarctica level still resident + the warp-gate UI loading its Autobot models)
// then fails: OUT OF MEMORY -> the game's fatal "Message Dialog" (0x255F90) clears the screen to black and spins forever
// on a byte that is initialised to 1 and never written (0x6CD9C4, `while (halt) {}` at 0x256220).
//
// PS2X_KERNEL_HEAP=1: SetupHeap(-1)/EndOfHeap = the thread's stack base like the kernel (0x1FE0000 here); the runtime's
// pools move to 0x80000-0xB1000 and the invocation stacks to 0xB4000-0x100000 - RAM between the kernel area (<0x80000) and
// the game ELF (>=0x100000) that nothing uses once the game runs (EELOAD's old home). PS2X_KERNEL_HEAP=0 = old; unset = enabled
// layout (byte-identical behaviour).
#include <cstdint>
#include <cstdlib>

namespace ps2_kheap
{
    inline bool enabled()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_KERNEL_HEAP");
            return !v || v[0] != '0';
        }();
        return on;
    }

    // Legacy layout (knob off): pools at 0x1F00000, invocation stacks carved down from the end of RAM above 0x1F00000.
    constexpr uint32_t kLegacyPoolsBase = 0x01F00000u;
    constexpr uint32_t kLegacyHeapEnd = 0x01F00000u;
    // Knob on: pools + invocation stacks below the game ELF.
    constexpr uint32_t kLowPoolsBase = 0x00080000u;
    constexpr uint32_t kPoolsBytes = 0x00031000u; // packet 64K + server 64K + TLS 64K + boot mode 4K
    constexpr uint32_t kLowInvokeStacksFloor = 0x000B4000u;
    constexpr uint32_t kLowInvokeStacksTop = 0x00100000u; // 19 stacks of 16 KB (Transformers uses 2)

    inline uint32_t poolsBase() { return enabled() ? kLowPoolsBase : kLegacyPoolsBase; }
    inline uint32_t rpcPacketPoolBase() { return poolsBase() + 0x00000000u; }
    inline uint32_t rpcServerPoolBase() { return poolsBase() + 0x00010000u; }
    inline uint32_t tlsPoolBase() { return poolsBase() + 0x00020000u; }
    inline uint32_t bootModePoolBase() { return poolsBase() + 0x00030000u; }
} // namespace ps2_kheap
