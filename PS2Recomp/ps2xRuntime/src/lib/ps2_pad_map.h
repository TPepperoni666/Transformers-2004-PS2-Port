#pragma once
// TEMP(transformers): qualification - host gamepad -> PS2 DualShock 2 mapping as PURE functions (no raylib calls, only its
// enum values), so ps2_pad.cpp and the offline unit test (private qualification evidence) run the same code.
//
// Host side (raylib 5.5 desktop = GLFW gamepad API + SDL_GameControllerDB mappings) is already normalised to an Xbox
// layout before we see it:
//   GAMEPAD_BUTTON_LEFT_TRIGGER_1  = LB / L1 (bumper)       GAMEPAD_BUTTON_RIGHT_TRIGGER_1 = RB / R1 (bumper)
//   GAMEPAD_BUTTON_LEFT_TRIGGER_2  = LT / L2 as a button    GAMEPAD_BUTTON_RIGHT_TRIGGER_2 = RT / R2 as a button
//                                    (raylib derives it from the axis > 0.1; we read the AXIS instead)
//   GAMEPAD_AXIS_LEFT/RIGHT_TRIGGER = -1 released .. +1 fully pressed (GLFW convention)
//   DualSense on Windows (GUID 030000004c050000e60c000000000000, GLFW's bundled mapping): leftshoulder:b4
//   rightshoulder:b5 lefttrigger:a3 righttrigger:a4 -> physical L1/R1 = TRIGGER_1, physical L2/R2 = trigger axes.
// PS2 side: physical L1/R1/L2/R2 = PS2 L1/R1/L2/R2 (PlayStation layout), Xbox LB/RB/LT/RT likewise.
#include "raylib.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_padmap
{
    // PS2 button bit order (active-high here, inverted on output). Byte 2 = bits 0-7, byte 3 = bits 8-15 of scePadRead.
    enum Ps2Button
    {
        B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT,
        B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE, B_COUNT
    };
    inline const char *const kPs2ButtonNames[B_COUNT] = {
        "select", "l3", "r3", "start", "up", "right", "down", "left",
        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};

    // Keyboard stick directions.
    enum KeyStick
    {
        KS_LUP = 0, KS_LDOWN, KS_LLEFT, KS_LRIGHT, KS_RUP, KS_RDOWN, KS_RLEFT, KS_RRIGHT, KS_COUNT
    };
    inline const char *const kKeyStickNames[KS_COUNT] = {
        "lstick_up", "lstick_down", "lstick_left", "lstick_right",
        "rstick_up", "rstick_down", "rstick_left", "rstick_right"};

    // Gamepad sources: raylib GamepadButton values, plus the two trigger AXES.
    constexpr int SRC_LT_AXIS = 100;
    constexpr int SRC_RT_AXIS = 101;

    constexpr int kRawButtons = GAMEPAD_BUTTON_RIGHT_THUMB + 1; // 18 raylib GamepadButton values (0 = UNKNOWN)
    constexpr int kRawAxes = GAMEPAD_AXIS_RIGHT_TRIGGER + 1;    // 6

    inline const char *const kRawButtonNames[kRawButtons] = {
        "?", "dpad_up", "dpad_right", "dpad_down", "dpad_left",
        "north(Y/tri)", "east(B/circle)", "south(A/cross)", "west(X/square)",
        "LB/L1", "LT/L2(btn)", "RB/R1", "RT/R2(btn)", "back/share", "guide/PS", "start/options", "LS/L3", "RS/R3"};

    // TEMP(transformers): qualification - stick response ([sticks] range_* / sensitivity_* in controls.ini).
    //   Circle = the host stick's round range as is (v0.7.0). Square = DualShock 2 style: a full push at any angle drives
    //   the major axis to 0x00/0xFF and diagonals reach the corners (the round gate is stretched onto the square).
    //   Sensitivity = per-axis gain after the dead zone, each axis then clamped to +-1 (= PCSX2's "Analog Sensitivity",
    //   pcsx2/SIO/Pad/PadDualshock2.cpp AxisScale, default 1.33). Default left 1.33: Transformers only runs when the left
    //   stick magnitude it computes is > 99.7% (see tfLeftStick below), which a round modern stick barely reaches and,
    //   5-11 degrees off a cardinal, never reaches at all (the game's per-axis 0.2 dead zone drops the minor axis).
    enum class StickRange
    {
        Circle,
        Square
    };
    inline constexpr float kDefaultSensitivityLeft = 1.33f;
    inline constexpr float kDefaultSensitivityRight = 1.0f;

    struct PadConfig
    {
        std::vector<int> keys[B_COUNT];
        std::vector<int> pads[B_COUNT];
        std::vector<int> keyStick[KS_COUNT];
        float deadzoneLeft = 0.05f;
        float deadzoneRight = 0.05f;
        // GAME(transformers): left cancels walk-start (including off-axis quantization) before 1.33 gain; right cancels axis deadzone.
        float antiDeadzoneLeft = 0.62f;
        float antiDeadzoneRight = 0.2f;
        // GAME(transformers): vehicle travel is per axis; -1 cancels only the live guest axis deadzone.
        float antiDeadzoneLeftVehicle = -1.0f;
        float sensitivityLeftVehicle = 1.0f;
        float responseLeftVehicle = 1.15f;
        float outerLeft = 0.95f;
        float outerRight = 0.95f;
        StickRange rangeLeft = StickRange::Circle;         // TEMP(transformers): qualification
        StickRange rangeRight = StickRange::Circle;
        float sensitivityLeft = kDefaultSensitivityLeft;   // 1.33: Transformers sprints from ~75% tilt in every direction
        float sensitivityRight = kDefaultSensitivityRight; // 1.0: camera as in v0.7.0
        bool invertLX = false, invertLY = false, invertRX = false, invertRY = false;
        bool swapSticks = false;
        float triggerThreshold = 0.30f;
        bool rumble = true;
        float rumbleStrength = 1.0f;
        float rumbleMaxSeconds = 1.5f;
        int gamepadIndex = -1; // -1 = first connected mapped gamepad
        std::string mappingsFile = "gamecontrollerdb.txt";
        bool logInput = false;
    };

    inline PadConfig makeDefaultConfig()
    {
        PadConfig c;
        c.keys[B_UP] = {KEY_UP};
        c.keys[B_DOWN] = {KEY_DOWN};
        c.keys[B_LEFT] = {KEY_LEFT};
        c.keys[B_RIGHT] = {KEY_RIGHT};
        c.keys[B_CROSS] = {KEY_X, KEY_SPACE};
        c.keys[B_CIRCLE] = {KEY_C};
        c.keys[B_SQUARE] = {KEY_Z, KEY_KP_0};
        c.keys[B_TRIANGLE] = {KEY_V, KEY_KP_1};
        c.keys[B_L1] = {KEY_Q};
        c.keys[B_R1] = {KEY_E};
        c.keys[B_L2] = {KEY_LEFT_SHIFT, KEY_ONE};
        c.keys[B_R2] = {KEY_RIGHT_SHIFT, KEY_THREE};
        c.keys[B_START] = {KEY_ENTER};
        c.keys[B_SELECT] = {KEY_TAB, KEY_BACKSPACE};
        c.keys[B_L3] = {KEY_LEFT_CONTROL};
        c.keys[B_R3] = {KEY_RIGHT_CONTROL};
        c.keyStick[KS_LUP] = {KEY_W};
        c.keyStick[KS_LDOWN] = {KEY_S};
        c.keyStick[KS_LLEFT] = {KEY_A};
        c.keyStick[KS_LRIGHT] = {KEY_D};
        c.keyStick[KS_RUP] = {KEY_I};
        c.keyStick[KS_RDOWN] = {KEY_K};
        c.keyStick[KS_RLEFT] = {KEY_J};
        c.keyStick[KS_RRIGHT] = {KEY_L};

        c.pads[B_UP] = {GAMEPAD_BUTTON_LEFT_FACE_UP};
        c.pads[B_DOWN] = {GAMEPAD_BUTTON_LEFT_FACE_DOWN};
        c.pads[B_LEFT] = {GAMEPAD_BUTTON_LEFT_FACE_LEFT};
        c.pads[B_RIGHT] = {GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
        c.pads[B_CROSS] = {GAMEPAD_BUTTON_RIGHT_FACE_DOWN};
        c.pads[B_CIRCLE] = {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT};
        c.pads[B_SQUARE] = {GAMEPAD_BUTTON_RIGHT_FACE_LEFT};
        c.pads[B_TRIANGLE] = {GAMEPAD_BUTTON_RIGHT_FACE_UP};
        c.pads[B_L1] = {GAMEPAD_BUTTON_LEFT_TRIGGER_1};  // LB / DualSense L1 (bumper)
        c.pads[B_R1] = {GAMEPAD_BUTTON_RIGHT_TRIGGER_1}; // RB / DualSense R1 (bumper)
        c.pads[B_L2] = {SRC_LT_AXIS};                    // LT / DualSense L2 (analog trigger axis)
        c.pads[B_R2] = {SRC_RT_AXIS};                    // RT / DualSense R2 (analog trigger axis)
        c.pads[B_L3] = {GAMEPAD_BUTTON_LEFT_THUMB};
        c.pads[B_R3] = {GAMEPAD_BUTTON_RIGHT_THUMB};
        c.pads[B_START] = {GAMEPAD_BUTTON_MIDDLE_RIGHT}; // Xbox Menu / PS Options / Switch +
        c.pads[B_SELECT] = {GAMEPAD_BUTTON_MIDDLE_LEFT}; // Xbox View / PS Share-Create / Switch -
        return c;
    }

    // GAME(transformers): host-only presets. These permute PS2 outputs, including menu controls.
    // Custom starts from the faithful base; explicit INI bindings are applied afterwards.
    enum class Preset { Original, Modern, Custom };
    inline bool parsePreset(const std::string& name, Preset& value)
    {
        std::string n=name;for(char&ch:n)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if(n=="original" || n=="ps2 original" || n=="ps2_original"){value=Preset::Original;return true;}
        if(n=="modern"){value=Preset::Modern;return true;}
        if(n=="custom"){value=Preset::Custom;return true;}
        return false;
    }
    inline PadConfig makePresetConfig(Preset preset)
    {
        PadConfig c=makeDefaultConfig();
        if(preset==Preset::Modern)
        {
            // Jump (L1) south; fire (R1) RT; slots L1/L2/R1/R2 south/LT/RT/RB.
            // Transform remains north. Cross/menu confirm moves to LB, as the host layer shares outputs.
            c.pads[B_L1]={GAMEPAD_BUTTON_RIGHT_FACE_DOWN};
            c.pads[B_CROSS]={GAMEPAD_BUTTON_LEFT_TRIGGER_1};
            c.pads[B_R1]={SRC_RT_AXIS};
            c.pads[B_R2]={GAMEPAD_BUTTON_RIGHT_TRIGGER_1};
        }
        return c;
    }

    inline std::string lowerCopy(std::string s)
    {
        for (char &ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    // controls.ini [gamepad] source names. Bumpers (lb/rb/l1/r1) are buttons, triggers (lt/rt/l2/r2) are the axes;
    // lt_button/rt_button = raylib's digital trigger flag (axis > 0.1), only for odd pads whose triggers are digital.
    inline int padSourceFromName(std::string n)
    {
        n = lowerCopy(n);
        if(n.rfind("gamepad_button_",0)==0) n=n.substr(15);
        static const std::pair<const char *, int> named[] = {
            {"left_face_up", GAMEPAD_BUTTON_LEFT_FACE_UP},
            {"left_face_down", GAMEPAD_BUTTON_LEFT_FACE_DOWN},
            {"left_face_left", GAMEPAD_BUTTON_LEFT_FACE_LEFT},
            {"left_face_right", GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
            {"right_face_up", GAMEPAD_BUTTON_RIGHT_FACE_UP},
            {"right_face_down", GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
            {"right_face_left", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
            {"right_face_right", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
            {"left_trigger_1", GAMEPAD_BUTTON_LEFT_TRIGGER_1},
            {"right_trigger_1", GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
            {"left_thumb", GAMEPAD_BUTTON_LEFT_THUMB},
            {"right_thumb", GAMEPAD_BUTTON_RIGHT_THUMB},
            {"middle", GAMEPAD_BUTTON_MIDDLE},
            {"middle_left", GAMEPAD_BUTTON_MIDDLE_LEFT},
            {"middle_right", GAMEPAD_BUTTON_MIDDLE_RIGHT},
            {"left_trigger_2", SRC_LT_AXIS}, {"right_trigger_2", SRC_RT_AXIS},
            {"south", GAMEPAD_BUTTON_RIGHT_FACE_DOWN}, {"a", GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
            {"cross", GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
            {"east", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT}, {"b", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
            {"circle", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
            {"west", GAMEPAD_BUTTON_RIGHT_FACE_LEFT}, {"x", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
            {"square", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
            {"north", GAMEPAD_BUTTON_RIGHT_FACE_UP}, {"y", GAMEPAD_BUTTON_RIGHT_FACE_UP},
            {"triangle", GAMEPAD_BUTTON_RIGHT_FACE_UP},
            {"dpad_up", GAMEPAD_BUTTON_LEFT_FACE_UP}, {"dpad_down", GAMEPAD_BUTTON_LEFT_FACE_DOWN},
            {"dpad_left", GAMEPAD_BUTTON_LEFT_FACE_LEFT}, {"dpad_right", GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
            {"lb", GAMEPAD_BUTTON_LEFT_TRIGGER_1}, {"l1", GAMEPAD_BUTTON_LEFT_TRIGGER_1},
            {"rb", GAMEPAD_BUTTON_RIGHT_TRIGGER_1}, {"r1", GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
            {"lt", SRC_LT_AXIS}, {"l2", SRC_LT_AXIS}, {"rt", SRC_RT_AXIS}, {"r2", SRC_RT_AXIS},
            {"lt_button", GAMEPAD_BUTTON_LEFT_TRIGGER_2}, {"rt_button", GAMEPAD_BUTTON_RIGHT_TRIGGER_2},
            {"ls", GAMEPAD_BUTTON_LEFT_THUMB}, {"l3", GAMEPAD_BUTTON_LEFT_THUMB},
            {"rs", GAMEPAD_BUTTON_RIGHT_THUMB}, {"r3", GAMEPAD_BUTTON_RIGHT_THUMB},
            {"start", GAMEPAD_BUTTON_MIDDLE_RIGHT}, {"menu", GAMEPAD_BUTTON_MIDDLE_RIGHT},
            {"options", GAMEPAD_BUTTON_MIDDLE_RIGHT}, {"back", GAMEPAD_BUTTON_MIDDLE_LEFT},
            {"view", GAMEPAD_BUTTON_MIDDLE_LEFT}, {"select", GAMEPAD_BUTTON_MIDDLE_LEFT},
            {"share", GAMEPAD_BUTTON_MIDDLE_LEFT}, {"create", GAMEPAD_BUTTON_MIDDLE_LEFT},
            {"guide", GAMEPAD_BUTTON_MIDDLE}, {"home", GAMEPAD_BUTTON_MIDDLE}, {"ps", GAMEPAD_BUTTON_MIDDLE}};
        for (const auto &[name, src] : named)
            if (n == name)
                return src;
        return -1;
    }

    // One sample of a host gamepad, as raylib reports it (filled by ps2_pad.cpp from IsGamepadButtonDown /
    // GetGamepadAxisMovement, or by the unit test with synthetic values).
    struct RawPad
    {
        bool connected = false;
        bool button[kRawButtons] = {};
        float axis[kRawAxes] = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f}; // triggers rest at -1 (GLFW)
    };

    // Per-device trigger state: a mapping that never reports <= -0.5 is treated as resting at 0 (0..1 travel).
    struct TriggerRest
    {
        bool seen[2] = {};
    };

    struct Mapped
    {
        uint16_t buttons = 0;     // active-high PS2 bits (Ps2Button)
        uint8_t pressL2 = 0;      // 0 = released, 1..255 analog travel past the threshold
        uint8_t pressR2 = 0;
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f; // -1..1 after dead zone / invert / swap
        float ltTravel = 0.0f, rtTravel = 0.0f;            // 0..1 trigger travel (for the F7 overlay)
    };

    // 0..1 trigger travel. GLFW reports triggers as -1 (released) .. +1 (full).
    inline float triggerTravel(float v, TriggerRest &rest, int which)
    {
        if (v < -0.5f)
            rest.seen[which] = true;
        const float t = rest.seen[which] ? (v + 1.0f) * 0.5f : v;
        return std::clamp(t, 0.0f, 1.0f);
    }

    inline uint8_t triggerPressure(float travel, float threshold)
    {
        if (travel <= threshold)
            return 0u;
        const float p = (travel - threshold) / std::max(0.01f, 1.0f - threshold);
        return static_cast<uint8_t>(std::clamp<long>(1l + std::lround(p * 254.0f), 1l, 255l));
    }

    // Radial dead zone with rescale: |v| <= dz -> 0, dz..outer -> 0..1 along the same direction.
    inline void radialDeadzone(float &x, float &y, float dz, float outer)
    {
        const float m = std::sqrt(x * x + y * y);
        if (m <= dz || m <= 1e-6f)
        {
            x = y = 0.0f;
            return;
        }
        const float span = std::max(0.01f, outer - dz);
        const float scaled = std::min(1.0f, (m - dz) / span);
        x = x / m * scaled;
        y = y / m * scaled;
    }

    inline uint8_t axisByte(float v)
    {
        v = std::clamp(v, -1.0f, 1.0f);
        const float f = v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f;
        return static_cast<uint8_t>(std::clamp<long>(std::lround(f), 0l, 255l));
    }

    // TEMP(transformers): qualification - full stick shaping: radial dead zone (rescaled to dz..outer), optional square range,
    // per-axis gain + clamp. Circle + gain 1.0 = exactly the v0.7.0 mapping.
    inline void shapeStick(float &x, float &y, float dz, float outer, StickRange range, float gain, float antiDeadzone = 0.0f)
    {
        radialDeadzone(x, y, dz, outer);
        // GAME(transformers): preserve zero inside our radial deadzone; lift nonzero radius before range/gain.
        // antiDeadzone=0 deliberately follows the old path without extra floating-point operations.
        const float radius = std::sqrt(x * x + y * y);
        if (antiDeadzone > 0.0f && radius > 0.0f)
        {
            const float lifted = antiDeadzone + (1.0f - antiDeadzone) * radius;
            x *= lifted / radius;
            y *= lifted / radius;
        }
        if (range == StickRange::Square)
        {
            const float m = std::sqrt(x * x + y * y);
            const float k = std::max(std::fabs(x), std::fabs(y));
            if (k > 1e-6f)
            {
                x = x * (m / k); // same direction, pushed out so that the round edge (m = 1) lands on the square edge
                y = y * (m / k);
            }
        }
        x = std::clamp(x * gain, -1.0f, 1.0f);
        y = std::clamp(y * gain, -1.0f, 1.0f);
    }

    // controls.ini [sticks] range values. circle/round = v0.7.0, square/ps2/dualshock2 = DS2-style square range.
    inline bool parseStickRange(std::string v, StickRange &out)
    {
        for (char &ch : v)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (v == "circle" || v == "round" || v == "circular")
            out = StickRange::Circle;
        else if (v == "square" || v == "ps2" || v == "dualshock2" || v == "ds2")
            out = StickRange::Square;
        else
            return false;
        return true;
    }
    inline const char *stickRangeName(StickRange r) { return r == StickRange::Square ? "square" : "circle"; }

    // TEMP(transformers): qualification - what Transformers (SLUS_206.68) makes of the left-stick bytes 6/7, for the F7
    // overlay, [padtest] log and the unit test (private qualification evidence cross-checks the constants in the ELF).
    // - Each axis is a Playstation2Axis (vtable 0x6b14a0, an ilAxisWithDeadZoneInput; registered for 'Left X'/'Left Y' by
    //   sub_0019A3C0 -> sub_0019B0B0 with lo -1, hi +1, dead zone 0.2 = float gp-0x7f38 @0x6cb1b8). Getter 0x197dc0:
    //   f = -1 + b*2/255; |f| < 0.2 -> 0 (and a steady rest value becomes the centre c); else f > c -> (f-c)/(1-c),
    //   f < c -> -(c-f)/(c+1). A PER-AXIS (cross-shaped) dead zone, no rescale.
    // - sub_0047DBA0 (player input): v = -(logical axes 0x28, 0x27) via taTransformerInputController (vtable 0x6b8000 slot
    //   0x1c = 0x34b040), then ControllerDeadZone (Autobot desc +0x100, METAGAME.TMD default 0, not overridden for any
    //   Autobot) with a CUBIC response: |v| -> min(1, (m-dz)/(1-dz))^3, turned to camera yaw (0x34b430, length kept).
    // - sub_00476700 (target speed): |v| > 0 -> WalkSpeed (desc +0x120, 4.8 m/s), |v| > 0.99 (float gp-0x7904 @0x6cb7ec)
    //   -> MaxRunSpeed (desc +0x124, Optimus 22 m/s). So RUN <=> min(1, m)^3 > 0.99 <=> m > 0.99666.
    // - Idle state check 0x471d00: an idle robot only starts walking when |actor+0x180| (= |v|) > 0.5 (0x471dcc), i.e.
    //   m > 0.794; once moving, any |v| > 0 keeps it walking. (In-game check: private qualification evidence routes/sprint_ab.tfs.)
    inline constexpr float kTfAxisDeadZone = 0.2f;
    inline constexpr float kTfRunThreshold = 0.99f;
    inline constexpr float kTfWalkStartThreshold = 0.5f;
    struct TfStickView
    {
        float x = 0.0f, y = 0.0f; // the game's axis values (after its 0.2 per-axis dead zone)
        float mag = 0.0f;         // their length m
        float move = 0.0f;        // min(1, m)^3 = |move vector| the speed logic sees
        bool walk = false;        // move > 0: a moving robot keeps walking (target WalkSpeed)
        bool start = false;       // move > 0.5: an idle robot starts walking
        bool run = false;         // move > 0.99: target MaxRunSpeed
    };
    // 0 idle, 1 keeps walking only if already moving, 2 walk (also from rest), 3 run
    inline int tfMoveClass(const TfStickView &v) { return v.run ? 3 : (v.start ? 2 : (v.walk ? 1 : 0)); }
    inline const char *tfMoveClassName(int c)
    {
        static const char *const n[4] = {"idle", "keep-walking", "walk", "RUN"};
        return n[c & 3];
    }
    inline float tfAxisRaw(uint8_t b) { return -1.0f + static_cast<float>(b) * (2.0f / 255.0f); }
    inline float tfAxis(uint8_t b, uint8_t restByte = 0x80u)
    {
        const float f = tfAxisRaw(b);
        if (std::fabs(f) < kTfAxisDeadZone)
            return 0.0f;
        const float c = tfAxisRaw(restByte); // our sticks rest at 0x80 -> the game calibrates c = +1/255
        return f > c ? (f - c) / (1.0f - c) : -(c - f) / (c + 1.0f);
    }
    // controllerDeadZone: the Autobot's ControllerDeadZone (0 in the shipped data; any value up to 0.5 only moves the run
    // threshold from m > 0.99666 to m > 0.99833).
    inline TfStickView tfLeftStick(uint8_t lx, uint8_t ly, float controllerDeadZone = 0.0f)
    {
        TfStickView v;
        v.x = tfAxis(lx);
        v.y = tfAxis(ly);
        v.mag = std::sqrt(v.x * v.x + v.y * v.y);
        if (v.mag <= 0.0f)
            return v;
        const float t = std::min(1.0f, (v.mag - controllerDeadZone) / (1.0f - controllerDeadZone));
        v.move = std::fabs(t * t * t);
        v.walk = v.move > 0.0f;
        v.start = v.move > kTfWalkStartThreshold;
        v.run = v.move > kTfRunThreshold;
        return v;
    }

    // Gamepad -> PS2 bits, L2/R2 pressure and sticks. Each PS2 button is driven by its configured sources: a raylib
    // button, or a trigger AXIS (pressed above triggerThreshold; the travel becomes the analog pressure).
    // GAME(transformers): use the guest's float operation order, including its asymmetric byte rounding edge.
    inline float vehicleGuestRaw(uint8_t b) {
        volatile float product=static_cast<float>(b)*(2.0f/255.0f);
        return product-1.0f;
    }
    inline float vehicleAxis(float value,const PadConfig& c,float liveDeadzone,bool invert=false) {
        const float a=std::fabs(value);
        if(!std::isfinite(a)||a<=c.deadzoneLeft||a<=1e-6f)return 0;
        float travel=std::min(1.0f,(a-c.deadzoneLeft)/std::max(0.01f,c.outerLeft-c.deadzoneLeft));
        if(c.responseLeftVehicle!=1.0f)travel=std::pow(travel,c.responseLeftVehicle);
        // Gain changes travel, preserving the cancelling floor; the robot's original gain order is unchanged.
        travel=std::clamp(travel*c.sensitivityLeftVehicle,0.0f,1.0f);
        const float anti=c.antiDeadzoneLeftVehicle<0?liveDeadzone:c.antiDeadzoneLeftVehicle;
        float result=std::copysign(std::clamp(anti+(1.0f-anti)*travel,0.0f,1.0f),value);
        // No extra lift: round only to the first representable byte accepted by the live deadzone.
        // Byte 102 is just INSIDE 0.2 in the guest and would recalibrate its learned centre to -0.2.
        if(anti>=liveDeadzone) {
            const float physical=invert?-value:value;
            auto b=axisByte(std::copysign(anti,physical));
            while(std::fabs(vehicleGuestRaw(b))<liveDeadzone && (physical<0?b>0:b<255))b=static_cast<uint8_t>(b+(physical<0?-1:1));
            const float floor=b<128?(128.0f-static_cast<float>(b))/128.0f:(static_cast<float>(b)-128.0f)/127.0f;
            result=std::copysign(std::max(std::fabs(result),floor),value);
        }
        return result;
    }
    inline void shapeLeft(float&x,float&y,const PadConfig&c,float weight,float liveDeadzone) {
        float cx=x,cy=y;
        shapeStick(x,y,c.deadzoneLeft,c.outerLeft,c.rangeLeft,c.sensitivityLeft,c.antiDeadzoneLeft);
        if(!std::isfinite(weight)||weight<=0)return; // exact original robot operations and bits
        cx=vehicleAxis(cx,c,liveDeadzone,c.invertLX);cy=vehicleAxis(cy,c,liveDeadzone,c.invertLY);
        if(weight>=1){x=cx;y=cy;return;}
        x+=weight*(cx-x);y+=weight*(cy-y);
    }

    inline Mapped mapGamepad(const RawPad &raw, const PadConfig &c, TriggerRest &rest, float vehicleWeight=0, float liveDeadzone=0.2f)
    {
        Mapped m;
        if (!raw.connected)
            return m;
        m.ltTravel = triggerTravel(raw.axis[GAMEPAD_AXIS_LEFT_TRIGGER], rest, 0);
        m.rtTravel = triggerTravel(raw.axis[GAMEPAD_AXIS_RIGHT_TRIGGER], rest, 1);
        for (int b = 0; b < B_COUNT; ++b)
        {
            for (int src : c.pads[b])
            {
                bool pressed = false;
                uint8_t pressure = 0xFFu;
                if (src == SRC_LT_AXIS || src == SRC_RT_AXIS)
                {
                    pressure = triggerPressure(src == SRC_LT_AXIS ? m.ltTravel : m.rtTravel, c.triggerThreshold);
                    pressed = pressure != 0u;
                }
                else if (src > 0 && src < kRawButtons)
                    pressed = raw.button[src];
                if (!pressed)
                    continue;
                m.buttons = static_cast<uint16_t>(m.buttons | (1u << b));
                if (b == B_L2)
                    m.pressL2 = std::max(m.pressL2, pressure);
                if (b == B_R2)
                    m.pressR2 = std::max(m.pressR2, pressure);
            }
        }
        float lx = raw.axis[GAMEPAD_AXIS_LEFT_X], ly = raw.axis[GAMEPAD_AXIS_LEFT_Y];
        float rx = raw.axis[GAMEPAD_AXIS_RIGHT_X], ry = raw.axis[GAMEPAD_AXIS_RIGHT_Y];
        if (c.swapSticks)
        {
            std::swap(lx, rx);
            std::swap(ly, ry);
        }
        shapeLeft(lx, ly, c, vehicleWeight, liveDeadzone); // GAME(transformers): exact robot or live vehicle curve
        shapeStick(rx, ry, c.deadzoneRight, c.outerRight, c.rangeRight, c.sensitivityRight, c.antiDeadzoneRight);
        if (c.invertLX) lx = -lx;
        if (c.invertLY) ly = -ly;
        if (c.invertRX) rx = -rx;
        if (c.invertRY) ry = -ry;
        m.lx = lx;
        m.ly = ly;
        m.rx = rx;
        m.ry = ry;
        return m;
    }

    // DualShock 2 pressure-mode bytes 8..19 of the scePadRead buffer, in libpad's padButtonStatus order. The game uses the
    // same offsets: SLUS_206.68 sub_00199400 registers 'Pressure Right' at +8 ... 'Pressure L1' +0x10, 'Pressure R1'
    // +0x11, 'Pressure L2' +0x12, 'Pressure R2' +0x13 of its pad buffer. Index = byte - 8.
    inline constexpr Ps2Button kPressureByteOrder[12] = {
        B_RIGHT, B_LEFT, B_UP, B_DOWN, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE, B_L1, B_R1, B_L2, B_R2};
}
