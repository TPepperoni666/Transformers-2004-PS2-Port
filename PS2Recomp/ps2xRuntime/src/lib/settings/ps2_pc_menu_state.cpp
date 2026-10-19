#include "../ps2_carstick_state.h"
// GAME(transformers) PC menu state: pad masking + menu combo + EE pause gate. See runtime/ps2_pc_menu_state.h. Settings integration.
#include "runtime/ps2_pc_menu_state.h"

#include <condition_variable>
#include <mutex>

namespace ps2x::ui
{
std::atomic<bool> g_pauseRequested{false};
std::atomic<float> g_pcAspect{-1.0f};
std::atomic<float> g_pcAspectPending{-1.0f};
std::atomic<int> g_pcPillarbox{-1};

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto kComboHold = std::chrono::milliseconds(500);
    // A lone Select or Start is held back this long so that a combo press (the partner usually follows within a few
    // frames) never reaches the game; a tap shorter than this is delivered on release, a longer press passes through.
    constexpr auto kComboGrace = std::chrono::milliseconds(0);
    constexpr uint16_t kPadSelect = 0x0001u, kPadL3 = 0x0002u, kPadR3 = 0x0004u, kPadStart = 0x0008u;
    constexpr uint32_t kSticksCentred = 0x80808080u;

    std::atomic<bool> g_menuOpen{false};
    std::atomic<int> g_combo{static_cast<int>(Combo::SelectStart)};
    std::atomic<bool> g_comboToggle{false};
    std::atomic<bool> g_eeParked{false};
    std::atomic<uint64_t> g_pauses{0};
    std::mutex g_pauseMx;
    std::condition_variable g_pauseCv;

    // Render-thread-only filter state.
    struct FilterState
    {
        uint16_t prev = 0;
        uint16_t hold = 0; // combo buttons masked until released
        bool comboHeld = false, comboFired = false;
        Clock::time_point comboSince{};
        bool pending[2] = {false, false};
        Clock::time_point pendingSince[2]{};
        bool wasOpen = false, releaseGuard = false;
    } s_f;
}

void setMenuOpen(bool open, bool pauseGame)
{
    g_menuOpen.store(open, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_pauseMx);
        g_pauseRequested.store(open && pauseGame, std::memory_order_release);
    }
    g_pauseCv.notify_all();
}

bool menuOpen() { return g_menuOpen.load(std::memory_order_acquire); }

void setCombo(Combo combo) { g_combo.store(static_cast<int>(combo), std::memory_order_relaxed); }

bool consumeComboToggle() { return g_comboToggle.exchange(false, std::memory_order_acq_rel); }

bool filterPadInput(uint16_t &buttons, uint32_t &sticks)
{
    const uint16_t raw = buttons;
    const Clock::time_point now = Clock::now();
    const Combo mode = static_cast<Combo>(g_combo.load(std::memory_order_relaxed));
    uint16_t hide = 0, inject = 0;
    if (mode != Combo::Off)
    {
        const uint16_t bits[2] = {mode == Combo::SelectStart ? kPadSelect : kPadL3,
                                  mode == Combo::SelectStart ? kPadStart : kPadR3};
        const uint16_t pair = static_cast<uint16_t>(bits[0] | bits[1]);
        if ((raw & pair) == pair)
        {
            if (!s_f.comboHeld)
            {
                s_f.comboHeld = true;
                s_f.comboFired = false;
                s_f.comboSince = now;
            }
            if (!s_f.comboFired && now - s_f.comboSince >= kComboHold)
            {
                s_f.comboFired = true;
                g_comboToggle.store(true, std::memory_order_release);
            }
            s_f.hold = static_cast<uint16_t>(s_f.hold | pair);
            s_f.pending[0] = s_f.pending[1] = false;
        }
        else
        {
            s_f.comboHeld = false;
            for (int k = 0; k < 2; ++k)
            {
                const uint16_t bit = bits[k];
                const bool down = (raw & bit) != 0;
                const bool wasDown = (s_f.prev & bit) != 0;
                if (down && !wasDown && !(s_f.hold & bit))
                {
                    s_f.pending[k] = true;
                    s_f.pendingSince[k] = now;
                }
                if (!s_f.pending[k])
                    continue;
                if (!down)
                {
                    inject = static_cast<uint16_t>(inject | bit); // short tap: deliver it now (the latch keeps it)
                    s_f.pending[k] = false;
                }
                else if (now - s_f.pendingSince[k] >= kComboGrace)
                {
                    s_f.pending[k] = false; // a plain long press: pass through from now on
                }
                else
                {
                    hide = static_cast<uint16_t>(hide | bit);
                }
            }
        }
        s_f.hold = static_cast<uint16_t>(s_f.hold & raw); // release the mask button by button
        hide = static_cast<uint16_t>(hide | s_f.hold);
    }
    s_f.prev = raw;
    uint16_t out = static_cast<uint16_t>((raw & ~hide) | inject);

    if (g_menuOpen.load(std::memory_order_acquire))
    {
        s_f.wasOpen = true;
        buttons = 0;
        sticks = kSticksCentred;
        return true;
    }
    if (s_f.wasOpen)
    {
        s_f.wasOpen = false;
        s_f.releaseGuard = true;
    }
    if (s_f.releaseGuard)
    {
        if (raw == 0)
            s_f.releaseGuard = false;
        else
            out = 0;
    }
    buttons = out;
    return s_f.releaseGuard;
}

std::chrono::steady_clock::duration waitWhilePausedImpl(bool (*stop)(void *), void *user)
{
    if (!g_pauseRequested.load(std::memory_order_acquire))
        return Clock::duration::zero();
    const Clock::time_point t0 = Clock::now();
    g_eeParked.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(g_pauseMx);
        while (g_pauseRequested.load(std::memory_order_acquire) && !stop(user))
            g_pauseCv.wait_for(lock, std::chrono::milliseconds(20));
    }
    const auto paused = Clock::now() - t0;
    // GAME(transformers): preserve the vehicle snapshot age across host-menu pause.
    { std::lock_guard<std::mutex> lock(ps2_carstick::snapshotMutex); ps2_carstick::latest.sampled += paused; }
    g_eeParked.store(false, std::memory_order_release);
    g_pauses.fetch_add(1, std::memory_order_relaxed);
    return paused;
}

bool eePaused() { return g_eeParked.load(std::memory_order_acquire); }

uint64_t pausedCount() { return g_pauses.load(std::memory_order_relaxed); }
}
