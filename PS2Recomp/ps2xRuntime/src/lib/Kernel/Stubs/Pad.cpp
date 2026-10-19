#include "../../ps2_menustick.h"
#include "../../ps2_carstick_state.h" // TEMP(transformers): read-only mode snapshot per pad frame
#include "Common.h"
#include "../../debug_counters.h" // TEMP(transformers)
#include "../TestScript.h"        // TEMP(transformers): PS2X_SCRIPT test harness
#include "Pad.h"
#include "../../ps2_pad_map.h" // TEMP(transformers): qualification - DS2 pressure byte order
#include "runtime/ee_scheduler.h"
#include <atomic>
#include <cctype>
#include <cstring>
#include <iostream>

extern std::atomic<uint32_t> g_ps2xCdAnchorHits; // TEMP(transformers): ps2xIOP iop_cdvd.cpp, CD-read anchors
void ps2PadSetActuators(int port, uint8_t smallMotor, uint8_t largeMotor); // TEMP(transformers): ps2_pad.cpp, rumble
void ps2FrameTimeNote(); // TEMP(transformers) qualification: gs_pgs_backend.cpp, PS2X_FRAMETIME_LOG

namespace ps2_stubs
{
    namespace
    {
        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadModeDualShock2Pressure = 0x79; // TEMP(transformers): DS2 native mode (analog + pressure)
        constexpr uint8_t kBackendPressureValid = 0xA5;      // TEMP(transformers): ps2_pad.cpp data[31] marker
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateExecCmd = 5;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;

        constexpr uint16_t kPadBtnSelect = 1u << 0;
        constexpr uint16_t kPadBtnL3 = 1u << 1;
        constexpr uint16_t kPadBtnR3 = 1u << 2;
        constexpr uint16_t kPadBtnStart = 1u << 3;
        constexpr uint16_t kPadBtnUp = 1u << 4;
        constexpr uint16_t kPadBtnRight = 1u << 5;
        constexpr uint16_t kPadBtnDown = 1u << 6;
        constexpr uint16_t kPadBtnLeft = 1u << 7;
        constexpr uint16_t kPadBtnL2 = 1u << 8;
        constexpr uint16_t kPadBtnR2 = 1u << 9;
        constexpr uint16_t kPadBtnL1 = 1u << 10;
        constexpr uint16_t kPadBtnR1 = 1u << 11;
        constexpr uint16_t kPadBtnTriangle = 1u << 12;
        constexpr uint16_t kPadBtnCircle = 1u << 13;
        constexpr uint16_t kPadBtnCross = 1u << 14;
        constexpr uint16_t kPadBtnSquare = 1u << 15;

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
            bool analogTriggers = false; // TEMP(transformers): host supplied analog L2/R2 pressure (playtest)
            uint8_t pressL2 = 0u;
            uint8_t pressR2 = 0u;
        };

        struct PadPortState
        {
            bool open = false;
            bool analogMode = false;  // real pads power up DIGITAL (CURID=4, mode 0x41)
            bool pressureEnabled = false;
            bool lastUsedOverride = false;
            bool lastUsedBackend = false;
            bool lastReadOk = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            uint32_t transientState = 0u;
            PadInputState lastInput{};
            uint8_t lastData[32]{};
            uint32_t readCount = 0u;
            uint32_t lastReadDataAddr = 0u;
            uint8_t actAlign[6]{0u, 1u, 0xFFu, 0xFFu, 0xFFu, 0xFFu}; // TEMP(transformers): scePadSetActAlign (0 small, 1 large)
        };

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        int g_padReadLogCount = 0;

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        void setButton(PadInputState &state, uint16_t mask, bool pressed)
        {
            if (pressed)
            {
                state.buttons = static_cast<uint16_t>(state.buttons & ~mask);
            }
        }

        int findFirstGamepad()
        {
            for (int i = 0; i < 4; ++i)
            {
                if (IsGamepadAvailable(i))
                {
                    return i;
                }
            }
            return -1;
        }

        void applyGamepadState(PadInputState &state)
        {
            if (!IsWindowReady())
            {
                return;
            }

            const int gamepad = findFirstGamepad();
            if (gamepad < 0)
            {
                return;
            }

            // Raylib mapping (PS2 -> raylib buttons/axes):
            // D-Pad -> LEFT_FACE_*, Cross/Circle/Square/Triangle -> RIGHT_FACE_*
            // L1/R1 -> TRIGGER_1, L2/R2 -> TRIGGER_2, L3/R3 -> THUMB
            // Select/Start -> MIDDLE_LEFT/MIDDLE_RIGHT
            state.lx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X));
            state.ly = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y));
            state.rx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X));
            state.ry = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y));

            setButton(state, kPadBtnUp, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP));
            setButton(state, kPadBtnDown, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
            setButton(state, kPadBtnLeft, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT));
            setButton(state, kPadBtnRight, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT));

            setButton(state, kPadBtnCross, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
            setButton(state, kPadBtnCircle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            setButton(state, kPadBtnSquare, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
            setButton(state, kPadBtnTriangle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP));

            setButton(state, kPadBtnL1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1));
            setButton(state, kPadBtnR1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1));
            setButton(state, kPadBtnL2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            setButton(state, kPadBtnR2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2));

            setButton(state, kPadBtnL3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB));
            setButton(state, kPadBtnR3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB));

            setButton(state, kPadBtnSelect, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT));
            setButton(state, kPadBtnStart, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT));
        }

        void applyKeyboardState(PadInputState &state, bool allowAnalog)
        {
            if (!IsWindowReady())
            {
                return;
            }

            // Keyboard mapping (PS2 -> keys):
            // D-Pad: arrows, Square/Cross/Circle/Triangle: Z/X/C/V
            // L1/R1: Q/E, L2/R2: 1/3, Start/Select: Enter/RightShift
            // L3/R3: LeftCtrl/RightCtrl, Analog left: WASD
            setButton(state, kPadBtnUp, IsKeyDown(KEY_UP));
            setButton(state, kPadBtnDown, IsKeyDown(KEY_DOWN));
            setButton(state, kPadBtnLeft, IsKeyDown(KEY_LEFT));
            setButton(state, kPadBtnRight, IsKeyDown(KEY_RIGHT));

            setButton(state, kPadBtnSquare, IsKeyDown(KEY_Z));
            setButton(state, kPadBtnCross, IsKeyDown(KEY_X));
            setButton(state, kPadBtnCircle, IsKeyDown(KEY_C));
            setButton(state, kPadBtnTriangle, IsKeyDown(KEY_V));

            setButton(state, kPadBtnL1, IsKeyDown(KEY_Q));
            setButton(state, kPadBtnR1, IsKeyDown(KEY_E));
            setButton(state, kPadBtnL2, IsKeyDown(KEY_ONE));
            setButton(state, kPadBtnR2, IsKeyDown(KEY_THREE));

            setButton(state, kPadBtnStart, IsKeyDown(KEY_ENTER));
            setButton(state, kPadBtnSelect, IsKeyDown(KEY_RIGHT_SHIFT));
            setButton(state, kPadBtnL3, IsKeyDown(KEY_LEFT_CONTROL));
            setButton(state, kPadBtnR3, IsKeyDown(KEY_RIGHT_CONTROL));

            if (!allowAnalog)
            {
                return;
            }

            float ax = 0.0f;
            float ay = 0.0f;
            if (IsKeyDown(KEY_D))
                ax += 1.0f;
            if (IsKeyDown(KEY_A))
                ax -= 1.0f;
            if (IsKeyDown(KEY_S))
                ay += 1.0f;
            if (IsKeyDown(KEY_W))
                ay -= 1.0f;

            if (ax != 0.0f || ay != 0.0f)
            {
                state.lx = axisToByte(ax);
                state.ly = axisToByte(ay);
            }
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        // GAME(transformers): SLUS_206.68 never calls scePadSetMainMode (no jal to 0x13AF18 anywhere in the ELF) and only treats the pad as
        // analog when scePadRead returns buf[1] low nibble >= 2 (fn 0x19B510). A real DualShock is switched by the player's ANALOG button;
        // emulate that with PS2X_PAD_ANALOG (default ON for this game, 0 disables).
        bool padDefaultAnalog()
        {
            const char *env = std::getenv("PS2X_PAD_ANALOG");
            return !env || env[0] != '0';
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = padDefaultAnalog();  // GAME(transformers)
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.transientState = 0u;
        }

        void queueExecCmdStateLocked(PadPortState &portState)
        {
            portState.transientState = static_cast<uint32_t>(kPadStateExecCmd);
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            if ((state.buttons & mask) != 0u)
            {
                return 0u;
            }
            if (state.analogTriggers && (mask == kPadBtnL2 || mask == kPadBtnR2)) // TEMP(transformers): analog triggers
            {
                const uint8_t p = (mask == kPadBtnL2) ? state.pressL2 : state.pressR2;
                return p ? p : 0xFFu;
            }
            return 0xFFu;
        }

        // TEMP(transformers): a DualShock 2 in pressure mode answers 0x79 (7 = analog, 9 halfwords = sticks + 12
        // pressure bytes), not 0x73. PS2X_PAD_PRESSURE_ID=0 restores the old 0x73.
        bool padReportsPressureId()
        {
            static const bool on = []()
            {
                const char *env = std::getenv("PS2X_PAD_PRESSURE_ID");
                return !env || env[0] != '0';
            }();
            return on;
        }

        bool padLegacyPressureOrder() // TEMP(transformers): qualification A/B knob, default off
        {
            static const bool on = []()
            {
                const char *env = std::getenv("PS2X_PAD_PRESSURE_LEGACY");
                const bool legacy = env && env[0] == '1';
                if (legacy)
                    std::cerr << "[pad] PS2X_PAD_PRESSURE_LEGACY=1: old L1/L2/R1/R2 pressure byte order (L2<->R1 crossed)" << std::endl;
                return legacy;
            }();
            return on;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            data[1] = portState.analogMode
                          ? ((portState.pressureEnabled && padReportsPressureId()) ? kPadModeDualShock2Pressure : kPadModeDualShock)
                          : kPadModeDigital;
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            // TEMP(transformers): qualification - bytes 8..19 in libpad padButtonStatus order (ps2_padmap::kPressureByteOrder:
            // Right Left Up Down Triangle Circle Cross Square L1 R1 L2 R2). The game reads the same offsets (SLUS_206.68
            // sub_00199400: 'Pressure L1' +0x10, 'Pressure R1' +0x11, 'Pressure L2' +0x12, 'Pressure R2' +0x13). The old
            // code wrote L1, L2, R1, R2, which crossed physical L2 <-> R1 for a game in pressure mode (its 'Dual Shock 2(R)'
            // profile). PS2X_PAD_PRESSURE_LEGACY=1 restores the old order (A/B only).
            for (int i = 0; i < 12; ++i)
                data[8 + i] = pressureValue(state, portState, static_cast<uint16_t>(1u << ps2_padmap::kPressureByteOrder[i]));
            if (padLegacyPressureOrder())
                std::swap(data[17], data[18]);
        }

        // PS2X_AUTOPRESS="start@6000,cross@6600:20,down@7000/300" — scripted pad-0 presses for unattended testing:
        // button@[anchor+]vsync[:holdFrames][/repeatEveryFrames[xCount]][~stopAnchor]. Hold defaults to 12 vsyncs.
        // TEMP(transformers): anchors come from PS2X_AUTOPRESS_ANCHORS="name=lsnLo-lsnHi,..." (ps2xIOP iop_cdvd.cpp sets
        // a bit on the first CD read in the range); "left@hub+300" fires 300 vsyncs after that read was first seen here,
        // "x5" limits repeats, "~movie" disables the entry once anchor "movie" has fired.
        struct AutoPress
        {
            uint16_t mask = 0u;
            uint64_t start = 0u;
            uint64_t hold = 12u;
            uint64_t period = 0u;
            uint64_t count = 0u; // 0 = repeat forever
            int anchor = -1;
            int stopAnchor = -1;
        };

        std::vector<std::string> &autoPressAnchorNames()
        {
            static std::vector<std::string> names = []()
            {
                std::vector<std::string> out;
                const char *env = std::getenv("PS2X_AUTOPRESS_ANCHORS");
                if (!env)
                    return out;
                const std::string spec(env);
                size_t pos = 0;
                while (pos < spec.size() && out.size() < 32u)
                {
                    size_t end = spec.find(',', pos);
                    if (end == std::string::npos)
                        end = spec.size();
                    const std::string item = spec.substr(pos, end - pos);
                    pos = end + 1;
                    out.push_back(item.substr(0, item.find('=')));
                }
                return out;
            }();
            return names;
        }

        int autoPressAnchorIndex(const std::string &name)
        {
            const auto &names = autoPressAnchorNames();
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == name)
                    return static_cast<int>(i);
            std::cerr << "[autopress] unknown anchor '" << name << "'" << std::endl;
            return -2; // never fires
        }

        const std::vector<AutoPress> &autoPresses()
        {
            static const std::vector<AutoPress> presses = []()
            {
                std::vector<AutoPress> out;
                const char *env = std::getenv("PS2X_AUTOPRESS");
                if (!env)
                    return out;
                if (const char *script = std::getenv("PS2X_SCRIPT"); script && *script) // TEMP(transformers): the test script owns the pad
                {
                    std::cerr << "[autopress] ignored: PS2X_SCRIPT owns the pad" << std::endl;
                    return out;
                }
                static const std::pair<const char *, uint16_t> names[] = {
                    {"start", kPadBtnStart}, {"select", kPadBtnSelect}, {"up", kPadBtnUp}, {"down", kPadBtnDown},
                    {"left", kPadBtnLeft}, {"right", kPadBtnRight}, {"cross", kPadBtnCross}, {"circle", kPadBtnCircle},
                    {"square", kPadBtnSquare}, {"triangle", kPadBtnTriangle}, {"l1", kPadBtnL1}, {"r1", kPadBtnR1},
                    {"l2", kPadBtnL2}, {"r2", kPadBtnR2}, {"l3", kPadBtnL3}, {"r3", kPadBtnR3}};
                std::string spec(env);
                size_t pos = 0;
                while (pos < spec.size())
                {
                    size_t end = spec.find(',', pos);
                    if (end == std::string::npos)
                        end = spec.size();
                    const std::string item = spec.substr(pos, end - pos);
                    pos = end + 1;
                    const size_t at = item.find('@');
                    if (at == std::string::npos)
                        continue;
                    AutoPress press;
                    const std::string name = item.substr(0, at);
                    for (const auto &[n, mask] : names)
                        if (name == n)
                            press.mask = mask;
                    if (press.mask == 0u)
                        continue;
                    const char *p = item.c_str() + at + 1;
                    if (std::isalpha(static_cast<unsigned char>(*p)))
                    {
                        const char *plus = std::strchr(p, '+');
                        const std::string anchorName = plus ? std::string(p, plus) : std::string(p);
                        press.anchor = autoPressAnchorIndex(anchorName);
                        p = plus ? plus + 1 : p + anchorName.size();
                    }
                    char *next = nullptr;
                    press.start = std::strtoull(p, &next, 10);
                    if (next && *next == ':')
                        press.hold = std::strtoull(next + 1, &next, 10);
                    if (next && *next == '/')
                        press.period = std::strtoull(next + 1, &next, 10);
                    if (next && *next == 'x')
                        press.count = std::strtoull(next + 1, &next, 10);
                    if (next && *next == '~')
                        press.stopAnchor = autoPressAnchorIndex(std::string(next + 1));
                    out.push_back(press);
                }
                return out;
            }();
            return presses;
        }

        void applyAutoPress(PadInputState &state, int port, PS2Runtime *runtime)
        {
            if (port != 0 || !runtime || autoPresses().empty())
                return;
            const uint64_t vs = runtime->eeScheduler().currentVSyncTick();

            // TEMP(transformers): latch the vsync at which each CD-read anchor was first seen.
            static uint64_t s_anchorVs[32] = {};
            static uint32_t s_anchorSeen = 0u;
            const uint32_t hits = g_ps2xCdAnchorHits.load(std::memory_order_relaxed);
            if (hits & ~s_anchorSeen)
            {
                for (uint32_t i = 0; i < 32u; ++i)
                {
                    if ((hits & ~s_anchorSeen) & (1u << i))
                    {
                        s_anchorVs[i] = vs;
                        const auto &names = autoPressAnchorNames();
                        std::cerr << "[autopress] anchor '" << (i < names.size() ? names[i] : std::string("?"))
                                  << "' at vs=" << vs << std::endl;
                    }
                }
                s_anchorSeen |= hits;
            }
            auto anchorFired = [&](int a) { return a >= 0 && (s_anchorSeen & (1u << a)) != 0u; };

            for (const AutoPress &press : autoPresses())
            {
                if (press.anchor != -1 && !anchorFired(press.anchor))
                    continue;
                if (press.stopAnchor != -1 && anchorFired(press.stopAnchor))
                    continue;
                const uint64_t start = press.start + (press.anchor >= 0 ? s_anchorVs[press.anchor] : 0u);
                if (vs < start)
                    continue;
                const uint64_t since = vs - start;
                if (press.period && press.count && since / press.period >= press.count)
                    continue;
                const uint64_t phase = press.period ? since % press.period : since;
                if (phase < press.hold)
                    setButton(state, press.mask, true);
            }
        }

        // TEMP(transformers): scripted runs (PS2X_AUTOPRESS with or without PS2X_AUTOPRESS_ANCHORS, or PS2X_SCRIPT) never
        // see host keyboard/gamepad/debug-panel input, so keys typed into (or a pad touched near) the game window can't
        // derail them. PSPadBackend::readState (ps2_pad.cpp) always returns true, which made the old
        // "else if (autoPresses().empty())" guard below dead code (private qualification evidence 1.2).
        bool scriptedPadInput()
        {
            static const bool scripted = []()
            {
                const char *autopress = std::getenv("PS2X_AUTOPRESS");
                const char *script = std::getenv("PS2X_SCRIPT");
                const bool on = (autopress && *autopress) || (script && *script);
                if (on)
                    std::cerr << "[pad] scripted run: host keyboard/gamepad/debug-panel input ignored" << std::endl;
                return on;
            }();
            return scripted || ps2_test::ownsPad();
        }

        bool readPadPortData(int port, int slot, PS2Runtime *runtime, uint8_t *outData, uint32_t dataAddr)
        {
            if (!outData)
            {
                return false;
            }

            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot);
                if (!sharedPortState || !sharedPortState->open)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            const bool scripted = scriptedPadInput(); // TEMP(transformers): analog sticks stay centred, buttons released
            PadInputState state;
            bool useOverride = false;
            {
                std::lock_guard<std::mutex> lock(g_padOverrideMutex);
                if (g_padOverrideEnabled && !scripted)
                {
                    state = g_padOverrideState;
                    useOverride = true;
                }
            }

            bool usedBackend = false;
            if (!useOverride && !scripted)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    state.rx = backendData[4];
                    state.ry = backendData[5];
                    state.lx = backendData[6];
                    state.ly = backendData[7];
                    if (backendData[31] == kBackendPressureValid) // TEMP(transformers): analog L2/R2 from the host pad
                    {
                        state.analogTriggers = true;
                        state.pressL2 = backendData[18]; // libpad positions (qualification; was [17])
                        state.pressR2 = backendData[19];
                    }
                    usedBackend = true;
                }
                else
                {
                    applyGamepadState(state);
                    applyKeyboardState(state, portState.analogMode);
                }
            }

            if (!scripted && !useOverride && usedBackend && port == 0 && slot == 0)
                ps2_menustick::apply(state.lx, state.ly); // GAME(transformers): host left stick only
            applyAutoPress(state, port, runtime);
            if (port == 0 && slot == 0)
            {
                ps2_test::onPadRead(state.buttons); // TEMP(transformers): PS2X_SCRIPT counts game pad reads and applies its presses
                ps2_test::onPadSticks(state.lx, state.ly, state.rx, state.ry); // TEMP(transformers) qualification: script 'stick'
            }
            fillPadStatus(outData, state, portState);

            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot))
                {
                    sharedPortState->lastInput = state;
                    std::memcpy(sharedPortState->lastData, outData, sizeof(sharedPortState->lastData));
                    sharedPortState->lastUsedOverride = useOverride;
                    sharedPortState->lastUsedBackend = usedBackend;
                    sharedPortState->lastReadOk = true;
                    sharedPortState->lastReadDataAddr = dataAddr;
                    ++sharedPortState->readCount;
                }
            }

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (!portState->pressureEnabled) // TEMP(transformers): qualification - shows which game pad profile is active
        {
            static int s_logged = 0;
            if (s_logged < 8)
            {
                ++s_logged;
                std::cerr << "[pad] port " << getRegU32(ctx, 4) << ": game enabled DualShock 2 pressure mode (mode 0x79,"
                          << " L1/R1/L2/R2 pressure = bytes 16/17/18/19)" << std::endl;
            }
        }
        portState->pressureEnabled = true;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = false;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        int32_t state = kPadStateDisconnected;
        if (portState && portState->open)
        {
            if (portState->transientState != 0u)
            {
                state = static_cast<int32_t>(portState->transientState);
                portState->transientState = 0u;
            }
            else
            {
                state = kPadStateStable;
            }
        }
        setReturnS32(ctx, state);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const int32_t currentId = portState->analogMode ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS
            setReturnS32(ctx, 0);
            return;
        case 4: // PAD_MODETABLE
            if (index == -1)
            {
                setReturnS32(ctx, 1); // one available mode
            }
            else if (index == 0)
            {
                setReturnS32(ctx, currentId);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
            setReturnS32(ctx, 0);
            return;
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, (portState && portState->open) ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->transientState = 0u;
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->analogMode = padDefaultAnalog(); // GAME(transformers): real pads open DIGITAL, but this game never calls scePadSetMainMode (see private qualification evidence)
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (dmaStr)
        {
            ps2TraceGuestRangeWrite(rdram, dmaAddr, 32u, "scePadPortOpen", ctx);
            std::memset(dmaStr, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (port == 0 && slot == 0) ps2CarstickSample(rdram, runtime->eeScheduler().currentVSyncTick());
        if (port == 0 && slot == 0) ps2MenustickSample(rdram);
        ps2TraceGuestRangeWrite(rdram, dataAddr, 32u, "scePadRead", ctx);
        ps2dbg::padReads.fetch_add(1, std::memory_order_relaxed);
        ps2FrameTimeNote(); // TEMP(transformers) qualification
        if (!readPadPortData(port, slot, runtime, data, dataAddr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (g_padReadLogCount < 48)
            {
                const int gamepad = findFirstGamepad();
                const bool gamepadStartPressed =
                    (gamepad >= 0) && IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
                const bool startPressed = (data[2] != 0xFFu || data[3] != 0xFFu ||
                                           IsKeyDown(KEY_ENTER) || gamepadStartPressed);
                if (startPressed)
                {
                    const uint32_t guestButtons =
                        (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                    std::printf("[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d\n",
                                port, slot, data[2], data[3], guestButtons,
                                IsKeyDown(KEY_ENTER) ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ++g_padReadLogCount;
                }
            }
        });

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        // TEMP(transformers): remember which actuator data byte drives which motor (0 = small, 1 = large, 0xFF = none).
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const uint8_t *align = getConstMemPtr(rdram, getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(port, static_cast<int>(getRegU32(ctx, 5)));
        if (portState && align)
        {
            std::memcpy(portState->actAlign, align, sizeof(portState->actAlign));
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        // TEMP(transformers): forward the motor values to the host pad (ps2_pad.cpp -> XInput). Rumble only, no game state.
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const uint8_t *act = getConstMemPtr(rdram, getRegU32(ctx, 6));
        uint8_t smallMotor = 0u;
        uint8_t largeMotor = 0u;
        bool open = false;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            const PadPortState *portState = lookupPadPortStateLocked(port, static_cast<int>(getRegU32(ctx, 5)));
            if (portState && portState->open && act)
            {
                open = true;
                for (int i = 0; i < 6; ++i)
                {
                    if (portState->actAlign[i] == 0u && act[i] != 0u)
                        smallMotor = 1u;
                    else if (portState->actAlign[i] == 1u)
                        largeMotor = std::max(largeMotor, act[i]);
                }
            }
        }
        if (open)
        {
            ps2PadSetActuators(port, smallMotor, largeMotor);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));
            portState->reqState = 0u;
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 5)
        {
            text = "EXECCMD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    PadDebugSnapshot getPadDebugSnapshot()
    {
        PadDebugSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_padOverrideMutex);
            snapshot.overrideEnabled = g_padOverrideEnabled;
            snapshot.overrideButtons = g_padOverrideState.buttons;
            snapshot.overrideRx = g_padOverrideState.rx;
            snapshot.overrideRy = g_padOverrideState.ry;
            snapshot.overrideLx = g_padOverrideState.lx;
            snapshot.overrideLy = g_padOverrideState.ly;
        }

        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            snapshot.readLogCount = g_padReadLogCount;
            for (size_t port = 0; port < kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < kPadDebugSlotCount; ++slot)
                {
                    const PadPortState &src = g_padPorts[port];
                    PadDebugPortSnapshot &dst = snapshot.ports[port][slot];
                    dst.open = src.open;
                    dst.analogMode = src.analogMode;
                    dst.pressureEnabled = src.pressureEnabled;
                    dst.lastUsedOverride = src.lastUsedOverride;
                    dst.lastUsedBackend = src.lastUsedBackend;
                    dst.lastReadOk = src.lastReadOk;
                    dst.buttonMask = src.buttonMask;
                    dst.lastButtons = src.lastInput.buttons;
                    dst.dmaAddr = src.dmaAddr;
                    dst.reqState = src.reqState;
                    dst.readCount = src.readCount;
                    dst.lastReadDataAddr = src.lastReadDataAddr;
                    dst.rx = src.lastInput.rx;
                    dst.ry = src.lastInput.ry;
                    dst.lx = src.lastInput.lx;
                    dst.ly = src.lastInput.ly;
                    std::memcpy(dst.lastData, src.lastData, sizeof(dst.lastData));
                }
            }
        }
        return snapshot;
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }
}

// GAME(transformers): share the exact pad bypass predicate with menu detector logging.
bool ps2MenustickScriptedInput(){return ps2_stubs::scriptedPadInput();}
