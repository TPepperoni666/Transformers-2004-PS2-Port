#pragma once
// GAME(transformers) PC menu state shared by the render thread (menu, pad sampler) and the EE thread (pause gate).
// No ImGui / raylib here: this lives in ps2_runtime; the ImGui overlay is src/lib/ui/ps2_pc_menu.cpp (runner target).
// Design: private qualification evidence 6.2 (combo), 6.4 (pause), 4.7 (input masking). Settings integration.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ps2x::ui
{
    // ---- menu state (render thread writes) -------------------------------------------------------------------
    void setMenuOpen(bool open, bool pauseGame); // pauseGame: hold the EE at its next VBlankStart while open
    bool menuOpen();

    // ---- pad filter (render thread, ps2PadSampleHostInput) ----------------------------------------------------
    // buttons: active-high PS2 mask (PAD_* bits of ps2_pad.cpp); sticks: lx|ly<<8|rx<<16|ry<<24.
    // While the menu is open the game sees no buttons and centred sticks; the menu combo (hold 0.5 s) is detected
    // here and hidden from the game while held; after the menu closes, buttons stay masked until all are released.
    // Returns true when the caller must clear its tap latch.
    enum class Combo : int
    {
        Off = 0,
        SelectStart = 1,
        L3R3 = 2,
    };
    void setCombo(Combo combo);
    bool filterPadInput(uint16_t &buttons, uint32_t &sticks);
    bool consumeComboToggle(); // menu: true once per completed hold

    // ---- pause gate (EE thread, EeScheduler VBlankStart) --------------------------------------------------------
    // One relaxed atomic load when no pause is requested (never changes guest behaviour). When the menu asked for a
    // pause, blocks until it is released or stop() returns true, and returns how long it blocked (the caller shifts
    // its absolute host deadlines by that much so the guest does not burst to catch up).
    extern std::atomic<bool> g_pauseRequested;
    inline bool pauseRequested() noexcept { return g_pauseRequested.load(std::memory_order_relaxed); }
    template <class StopFn> std::chrono::steady_clock::duration waitWhilePaused(StopFn stop);
    std::chrono::steady_clock::duration waitWhilePausedImpl(bool (*stop)(void *), void *user);
    bool eePaused();                // the EE thread is currently parked (Debug page / autotest)
    uint64_t pausedCount();         // completed pauses

    template <class StopFn> std::chrono::steady_clock::duration waitWhilePaused(StopFn stop)
    {
        return waitWhilePausedImpl([](void *u) -> bool { return (*static_cast<StopFn *>(u))(); }, &stop);
    }
}
