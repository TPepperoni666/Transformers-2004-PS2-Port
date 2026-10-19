#include "ps2_pc_bindings.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_pc_menu_state.h"
#include "ps2_menustick.h"
#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "ps2_pad_hotplug.h" // TEMP(transformers): padhot1
#include "debug_counters.h"
#include "ps2_glyphs.h"
#include "ps2_carstick_state.h" // TEMP(transformers): synchronized EE snapshot, no render-thread guest reads
#include "ps2_pad_map.h"      // TEMP(transformers): qualification
#include "ps2_perfscope.h"    // TEMP(transformers): qualification
#include "Kernel/Stubs/Pad.h" // TEMP(transformers): qualification - F7 overlay shows what the game received
#include "rlgl.h"            // TEMP(transformers): qualification - rlDrawRenderBatchActive for PS2X_PADTEST_SHOT_FRAMES
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// TEMP(transformers): playtest controller support (private qualification evidence, PLAYTEST_README.md).
// - Any connected, MAPPED gamepad (GLFW/SDL_GameControllerDB mapping: Xbox/XInput, DualShock 4, DualSense, Switch Pro,
//   ...), first one wins, hot-plug re-scan every host frame; optional gamecontrollerdb.txt next to the ELF adds mappings.
// - Triggers read from the analog axes (digital L2/R2 with a threshold, analog value kept for pressure mode).
// - qualification: the mapping itself is ps2_padmap::mapGamepad (ps2_pad_map.h, unit-tested offline); F7 = PAD TEST overlay
//   (raw raylib buttons/axes -> PS2 bits + pressure -> the bytes the game last read), changes logged as [padtest].
// - Radial dead zones per stick, inversion, stick swap; keyboard mapping kept (and remappable).
// - qualification: per-stick response = range (circle | square) + per-axis sensitivity (default left 1.33, like PCSX2), so
//   Transformers' run rule (left stick magnitude > 99.7% after its own 0.2 per-axis dead zone) works on round modern
//   sticks; F7 shows / [padtest] logs whether the game would RUN at the current stick position.
// - Optional controls.ini next to the ELF (working dir) or PS2X_CONTROLS_INI=<path>; absent = built-in defaults.
// - Rumble: the game's scePadSetActDirect data reaches ps2PadSetActuators(); the render thread forwards it to XInput
//   pads (raylib 5.5's GLFW backend has no SetGamepadVibration: it only logs "not available on target platform").
// Scripted runs (PS2X_SCRIPT / PS2X_AUTOPRESS) never reach PSPadBackend::readState (Kernel/Stubs/Pad.cpp), skip the
// controls.ini and never rumble.

#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
#define PS2X_PAD_HAS_GLFW 1
extern "C"
{
    // raylib's desktop backend links GLFW statically (rglfw.c); these are not wrapped by the raylib API.
    int glfwJoystickIsGamepad(int jid);
    const char *glfwGetJoystickGUID(int jid);
}
#else
#define PS2X_PAD_HAS_GLFW 0
#endif

bool ps2HostPadRescan(); // TEMP(transformers): owner-thread GLFW arrival notification
void ps2HostRumbleSet(float lowFrequency, float highFrequency); // ps2_host_rumble.cpp (XInput, Windows; no-op elsewhere)

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;
    constexpr uint8_t kPressureValidMarker = 0xA5; // data[31]: data[18]/data[19] hold analog L2/R2 pressure (Pad.cpp)

    using namespace ps2_padmap; // TEMP(transformers): qualification - mapping tables + pure mapping live in ps2_pad_map.h

    std::string lower(std::string s)
    {
        for (char &ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    std::string trim(const std::string &s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
            --b;
        return s.substr(a, b - a);
    }

    int keyFromName(std::string n) { return ps2x::input::keyFromName(n); }

    std::vector<std::string> splitList(const std::string &v)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : v)
        {
            if (ch == ',' || ch == ' ' || ch == '|' || ch == '\t')
            {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            }
            else
                cur.push_back(ch);
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    bool parseBool(const std::string &v)
    {
        const std::string s = lower(trim(v));
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }

    float parseFloat(const std::string &v, float lo, float hi, float fallback)
    {
        char *end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        if (end == v.c_str() || !std::isfinite(f))
            return fallback;
        return std::clamp(f, lo, hi);
    }

    bool scriptedRun()
    {
        static const bool scripted = []()
        {
            const char *autopress = std::getenv("PS2X_AUTOPRESS");
            const char *script = std::getenv("PS2X_SCRIPT");
            return (autopress && *autopress) || (script && *script);
        }();
        return scripted;
    }

    std::string controlsIniPath()
    {
        if (const char *p = std::getenv("PS2X_CONTROLS_INI"); p && *p)
            return p;
        return "controls.ini"; // working directory = the disc folder next to the ELF (play_*.bat cd there)
    }

    // Loads controls.ini over the defaults. Any binding line REPLACES that button's defaults for its device;
    // "none" clears it. Unknown names are reported and ignored.
    bool menuFlickSteps=true; // GAME(transformers): default on, INI then environment override.
    PadConfig loadConfig()
    {
        PadConfig c = makeDefaultConfig();
        // TEMP(transformers) qualification F4: PS2X_GLYPHS_INI_IN_SCRIPT=1 lets a scripted run read controls.ini so the
        // button PROMPTS show a remap; the game still never sees physical input in scripted runs (Pad.cpp).
        const char *iniInScript = std::getenv("PS2X_GLYPHS_INI_IN_SCRIPT");
        if (scriptedRun() && !(iniInScript && iniInScript[0] == '1'))
        {
            std::cerr << "[pad] scripted run: controls.ini not read, physical input ignored" << std::endl;
            return c;
        }
        const std::string path = controlsIniPath();
        std::ifstream in(path);
        if (!in)
        {
            std::cerr << "[pad] no " << path << " - default controls" << std::endl;
            return c;
        }
        // GAME(transformers): select the base in a pre-pass, independent of INI section order.
        // Existing binding lines still win. Scripted runs returned above and retain original input.
        std::string presetSection, presetLine;
        Preset preset=Preset::Original;
        while(std::getline(in,presetLine))
        {
            presetLine=trim(presetLine.substr(0,presetLine.find_first_of(";#")));
            if(presetLine.size()>1 && presetLine.front()=='[' && presetLine.back()==']')
                presetSection=lower(trim(presetLine.substr(1,presetLine.size()-2)));
            const size_t eq=presetLine.find('=');
            if(presetSection=="general" && eq!=std::string::npos && lower(trim(presetLine.substr(0,eq)))=="preset")
                parsePreset(trim(presetLine.substr(eq+1)),preset);
        }
        c=makePresetConfig(preset);in.clear();in.seekg(0);
        std::string section, line;
        int lineNo = 0, applied = 0;
        auto warn = [&](const std::string &what)
        { std::cerr << "[pad] " << path << ":" << lineNo << ": " << what << std::endl; };
        while (std::getline(in, line))
        {
            ++lineNo;
            const size_t hash = line.find_first_of(";#");
            if (hash != std::string::npos)
                line = line.substr(0, hash);
            line = trim(line);
            if (line.empty())
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = lower(trim(line.substr(1, line.size() - 2)));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                warn("expected key = value");
                continue;
            }
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            ++applied;
            if (section == "keyboard" || section == "gamepad")
            {
                const bool kb = section == "keyboard";
                std::vector<int> *target = nullptr;
                for (int b = 0; b < B_COUNT; ++b)
                    if (key == kPs2ButtonNames[b])
                        target = kb ? &c.keys[b] : &c.pads[b];
                if (kb)
                    for (int s = 0; s < KS_COUNT; ++s)
                        if (key == kKeyStickNames[s])
                            target = &c.keyStick[s];
                if (!target)
                {
                    warn("unknown button '" + key + "'");
                    continue;
                }
                target->clear();
                for (const std::string &tok : splitList(value))
                {
                    if (lower(tok) == "none")
                        continue;
                    const int code = kb ? keyFromName(tok) : padSourceFromName(tok);
                    if (code < 0)
                        warn("unknown " + std::string(kb ? "key" : "gamepad input") + " '" + tok + "'");
                    else
                        target->push_back(code);
                }
            }
            else if (section == "sticks")
            {
                if (key == "menu_flick_steps")
                {
                    const std::string s = lower(trim(value));
                    if (s == "1" || s == "true" || s == "yes" || s == "on") menuFlickSteps = true;
                    else if (s == "0" || s == "false" || s == "no" || s == "off") menuFlickSteps = false;
                    else
                    {
                        menuFlickSteps = true;
                        static bool warned = false;
                        if (!warned && !scriptedRun())
                        {
                            std::cerr << "[pad] unrecognised menu_flick_steps; keeping default on" << std::endl;
                            warned = true;
                        }
                    }
                }
                else if (key == "menu_repeat_delay_ms")
                    ps2_menustick::repeatDelay = parseFloat(value, 250.0f, 1000.0f, 500.0f) / 1000.0;
                else if (key == "deadzone_left") c.deadzoneLeft = parseFloat(value, 0.0f, 0.9f, c.deadzoneLeft);
                else if (key == "anti_deadzone_left_vehicle") {
                    if(lower(trim(value))=="auto")c.antiDeadzoneLeftVehicle=-1.0f;
                    else {auto v=parseFloat(value,0.0f,0.9f,c.antiDeadzoneLeftVehicle);if(std::isfinite(v))c.antiDeadzoneLeftVehicle=v;}
                }
                else if (key == "sensitivity_left_vehicle") {auto v=parseFloat(value,0.1f,2.0f,c.sensitivityLeftVehicle);if(std::isfinite(v))c.sensitivityLeftVehicle=v;}
                else if (key == "response_left_vehicle") {auto v=parseFloat(value,0.25f,4.0f,c.responseLeftVehicle);if(std::isfinite(v))c.responseLeftVehicle=v;}
                else if (key == "anti_deadzone_left") c.antiDeadzoneLeft = parseFloat(value, 0.0f, 0.9f, c.antiDeadzoneLeft);
                else if (key == "anti_deadzone_right") c.antiDeadzoneRight = parseFloat(value, 0.0f, 0.9f, c.antiDeadzoneRight);
                else if (key == "deadzone_right") c.deadzoneRight = parseFloat(value, 0.0f, 0.9f, c.deadzoneRight);
                else if (key == "deadzone") c.deadzoneLeft = c.deadzoneRight = parseFloat(value, 0.0f, 0.9f, c.deadzoneLeft);
                else if (key == "outer_left") c.outerLeft = parseFloat(value, 0.3f, 1.0f, c.outerLeft);
                else if (key == "outer_right") c.outerRight = parseFloat(value, 0.3f, 1.0f, c.outerRight);
                else if (key == "invert_left_x") c.invertLX = parseBool(value);
                else if (key == "invert_left_y") c.invertLY = parseBool(value);
                else if (key == "invert_right_x") c.invertRX = parseBool(value);
                else if (key == "invert_right_y") c.invertRY = parseBool(value);
                else if (key == "swap_sticks") c.swapSticks = parseBool(value);
                else if (key == "trigger_threshold") c.triggerThreshold = parseFloat(value, 0.02f, 0.98f, c.triggerThreshold);
                // TEMP(transformers): qualification - stick response (ps2_pad_map.h shapeStick / StickRange)
                else if (key == "sensitivity_left") c.sensitivityLeft = parseFloat(value, 0.5f, 2.0f, c.sensitivityLeft);
                else if (key == "sensitivity_right") c.sensitivityRight = parseFloat(value, 0.5f, 2.0f, c.sensitivityRight);
                else if (key == "sensitivity") c.sensitivityLeft = c.sensitivityRight = parseFloat(value, 0.5f, 2.0f, c.sensitivityLeft);
                else if (key == "range_left" || key == "range_right" || key == "range")
                {
                    StickRange r = StickRange::Circle;
                    if (!parseStickRange(value, r))
                        warn("unknown range '" + value + "' (use circle or square)");
                    else
                    {
                        if (key != "range_right") c.rangeLeft = r;
                        if (key != "range_left") c.rangeRight = r;
                    }
                }
                else warn("unknown [sticks] key '" + key + "'");
            }
            else if (section == "rumble")
            {
                if (key == "enabled") c.rumble = parseBool(value);
                else if (key == "strength") c.rumbleStrength = parseFloat(value, 0.0f, 1.0f, c.rumbleStrength);
                else if (key == "max_seconds") c.rumbleMaxSeconds = parseFloat(value, 0.0f, 30.0f, c.rumbleMaxSeconds);
                else warn("unknown [rumble] key '" + key + "'");
            }
            else if (section == "general")
            {
                if (key == "preset") { Preset checked; if(!parsePreset(value,checked))warn("unknown preset: use original, modern or custom"); }
                else if (key == "gamepad_index") c.gamepadIndex = std::clamp(std::atoi(value.c_str()), -1, 3);
                else if (key == "mappings_file") c.mappingsFile = value;
                else if (key == "log_input") c.logInput = parseBool(value);
                else warn("unknown [general] key '" + key + "'");
            }
            else
                warn("line outside a known section ([keyboard] [gamepad] [sticks] [rumble] [general])");
        }
        std::cerr << "[pad] loaded " << path << " (" << applied << " settings)" << std::endl;
        return c;
    }

    const PadConfig &config()
    {
        static PadConfig c = []()
        {
            PadConfig loaded = loadConfig();
            if(const char* value=std::getenv("PS2X_MENU_STICK");value && (value[0]=='0'||value[0]=='1') && value[1]=='\0')
                menuFlickSteps=value[0]=='1';
            ps2_menustick::setEnabled(menuFlickSteps);
            // TEMP(transformers): qualification - PS2X_PAD_LOG=1 (e.g. in play_env.txt) = controls.ini log_input = 1 without
            // a controls.ini: host button changes + every game walk/run change of the left stick go to the log.
            if (const char *pl = std::getenv("PS2X_PAD_LOG"); pl && pl[0] == '1' && !scriptedRun())
                loaded.logInput = true;
            // TEMP(transformers): qualification - say how the left stick is shaped (Transformers sprints only when the stick
            // magnitude it computes is > 99.7%; sensitivity > 1 lets a round modern stick get there, see ps2_pad_map.h).
            std::fprintf(stderr, "[pad] sticks: left range=%s sensitivity=%.2f deadzone=%.2f outer=%.2f anti_deadzone=%.3f | right range=%s "
                                 "sensitivity=%.2f deadzone=%.2f outer=%.2f anti_deadzone=%.3f\n",
                         stickRangeName(loaded.rangeLeft), loaded.sensitivityLeft, loaded.deadzoneLeft, loaded.outerLeft, loaded.antiDeadzoneLeft,
                         stickRangeName(loaded.rangeRight), loaded.sensitivityRight, loaded.deadzoneRight, loaded.outerRight, loaded.antiDeadzoneRight);
            return loaded;
        }();
        static const PadConfig legacy = c;
        static const bool migrated = [&]() {
            if(scriptedRun()) return true;
            std::ifstream controls(controlsIniPath()); if(!controls) return true;
            namespace st=ps2x::settings;
            for(int btn=0;btn<B_COUNT;++btn) for(int device=0;device<2;++device) {
                const auto& codes=device ? c.pads[btn] : c.keys[btn]; std::string value;
                for(int code:codes) {
                    const auto name=device ? ps2x::input::padNameFromSource(code) : ps2x::input::keyNameFromCode(code);
                    if(!name.empty()) { if(!value.empty()) value+=","; value+=name; }
                }
                const std::string key=std::string(device ? "input.pad." : "input.kb.")+kPs2ButtonNames[btn];
                st::adoptLegacy(key.c_str(),value.empty() ? "none" : value);
            }
            for(int stick=0;stick<2;++stick) {
                const int base=stick ? KS_RUP : KS_LUP, dirs[]={0,2,1,3}; std::string value;
                for(int dir:dirs) { if(!value.empty()) value+=","; value+=c.keyStick[base+dir].empty() ? "none" : ps2x::input::keyNameFromCode(c.keyStick[base+dir][0]); }
                st::adoptLegacy(stick ? "input.kb.right_stick" : "input.kb.left_stick",value);
            }
            auto number=[&](const char* name,float value) { char text[64]; std::snprintf(text,sizeof(text),"%.9g",double(value)); const std::string key=std::string("input.pad.")+name; st::adoptLegacy(key.c_str(),text); };
            number("deadzone_left",c.deadzoneLeft); number("deadzone_right",c.deadzoneRight);
            number("anti_deadzone_left",c.antiDeadzoneLeft); number("anti_deadzone_right",c.antiDeadzoneRight);
            number("outer_left",c.outerLeft); number("outer_right",c.outerRight);
            number("sensitivity_left",c.sensitivityLeft); number("sensitivity_right",c.sensitivityRight);
            number("sensitivity_left_vehicle",c.sensitivityLeftVehicle); number("response_left_vehicle",c.responseLeftVehicle);
            if(c.antiDeadzoneLeftVehicle<0) st::adoptLegacy("input.pad.anti_deadzone_left_vehicle","auto"); else number("anti_deadzone_left_vehicle",c.antiDeadzoneLeftVehicle);
            st::adoptLegacy("input.pad.index",std::to_string(c.gamepadIndex));
            st::adoptLegacy("input.pad.range_left",stickRangeName(c.rangeLeft)); st::adoptLegacy("input.pad.range_right",stickRangeName(c.rangeRight));
            st::adoptLegacy("input.invert_x_camera",c.invertRX ? "on" : "off"); st::adoptLegacy("input.rumble",c.rumble ? "on" : "off");
            return true;
        }();
        (void)migrated;
        static uint64_t pcVersion = 0;
        if (pcVersion != ps2x::settings::version() && !scriptedRun()) {
            pcVersion = ps2x::settings::version(); c = legacy;
            namespace st = ps2x::settings;
            for (int btn=0; btn<B_COUNT; ++btn) {
                for (int device=0; device<2; ++device) {
                    const std::string key=std::string(device ? "input.pad." : "input.kb.")+kPs2ButtonNames[btn];
                    if (st::source(key.c_str())==st::Source::Default) continue;
                    auto& dest=device ? c.pads[btn] : c.keys[btn]; dest.clear();
                    for (const auto& token:splitList(st::get(key.c_str()))) {
                        const int code=device ? padSourceFromName(token) : keyFromName(token);
                        if (code>=0) dest.push_back(code);
                    }
                }
            }
            for(int stick=0;stick<2;++stick) {
                const char* key=stick ? "input.kb.right_stick" : "input.kb.left_stick";
                if(st::source(key)!=st::Source::Default) {
                    const auto tokens=splitList(st::get(key));
                    if(tokens.size()==4) {
                        const int base=stick ? KS_RUP : KS_LUP;
                        const int directions[]={0,2,1,3};
                        for(int d=0;d<4;++d) { auto& dest=c.keyStick[base+directions[d]]; dest.clear(); const int code=keyFromName(tokens[d]); if(code>=0) dest.push_back(code); }
                    }
                }
            }
            auto number=[&](const char* name,float& value) {
                const std::string key=std::string("input.pad.")+name;
                if(st::source(key.c_str())!=st::Source::Default) value=float(st::getFloat(key.c_str()));
            };
            number("deadzone_left",c.deadzoneLeft); number("deadzone_right",c.deadzoneRight);
            number("anti_deadzone_left",c.antiDeadzoneLeft); number("anti_deadzone_right",c.antiDeadzoneRight);
            number("sensitivity_left",c.sensitivityLeft); number("sensitivity_right",c.sensitivityRight);
            number("outer_left",c.outerLeft); number("outer_right",c.outerRight);
            number("sensitivity_left_vehicle",c.sensitivityLeftVehicle); number("response_left_vehicle",c.responseLeftVehicle);
            if(st::source("input.pad.anti_deadzone_left_vehicle")!=st::Source::Default) c.antiDeadzoneLeftVehicle=st::get("input.pad.anti_deadzone_left_vehicle")=="auto" ? -1.0f : float(st::getFloat("input.pad.anti_deadzone_left_vehicle"));
            if(st::source("input.pad.range_left")!=st::Source::Default) parseStickRange(st::get("input.pad.range_left"),c.rangeLeft);
            if(st::source("input.pad.range_right")!=st::Source::Default) parseStickRange(st::get("input.pad.range_right"),c.rangeRight);
            if(st::source("input.pad.index")!=st::Source::Default) c.gamepadIndex=st::getInt("input.pad.index");
            if(st::source("input.rumble")!=st::Source::Default) c.rumble=st::getBool("input.rumble");
            if (st::source("input.invert_x_camera")!=st::Source::Default) c.invertRX=st::getBool("input.invert_x_camera");
        }
        return c;
    }

    // TEMP(transformers): the game polls the pad once per game frame, which is ~0.1-0.6 s at today's frame rate, so a
    // tap between two polls was lost. The render thread samples the host every frame (ps2PadSampleHostInput) and ORs
    // every button seen down into this latch; the next port-0 readState reports it once and clears it. Active-high.
    std::atomic<uint16_t> g_latchedButtons{0};
    // Latest host sample, written ONLY by the render thread (raylib input is not thread-safe; reading IsKeyDown /
    // gamepad state from the EE thread raced PollInputEvents - concurrency review). Packed sticks: lx|ly<<8|rx<<16|ry<<24.
    std::atomic<uint16_t> g_currentButtons{0};
    std::atomic<uint32_t> g_currentSticks{0x80808080u};
    std::atomic<uint16_t> g_currentPressure{0}; // L2 | R2 << 8, 0 = not pressed
    std::atomic<uint16_t> g_latchedPressure{0}; // max pressure seen since the last poll (per byte)

    // Rumble request from the EE thread (Pad.cpp scePadSetActDirect): small | large << 8, plus a change sequence.
    std::atomic<uint32_t> g_actuatorState{0};
    std::atomic<uint32_t> g_actuatorSeq{0};

    // Render-thread-only state.
    struct HostPadState
    {
        bool initialised = false;
        int active = -1;
        bool unmappedLogged[4] = {};
        TriggerRest triggerRest;
        RawPad raw;         // last host sample (F7 overlay)
        Mapped mapped;      // its gamepad-only mapping
        uint16_t allDown = 0u; // keyboard + gamepad PS2 bits of the last sample
        uint16_t pressure = 0u;
        uint32_t sticks = 0x80808080u;
        bool overlay = false;
        uint16_t lastTestButtons = 0xFFFFu;
        uint32_t lastTestRaw = 0xFFFFFFFFu;
        int lastTestMove = -1; // TEMP(transformers): qualification - last logged game move class (0 idle, 1 walk, 2 run)
        uint32_t appliedSeq = 0;
        uint32_t appliedState = 0;
        bool rumbleOn = false;
        std::chrono::steady_clock::time_point rumbleSince{};
        uint16_t lastLoggedButtons = 0;
    } g_host;
    std::atomic<int> g_activeGamepad{-1};

    bool isMappedGamepad(int i)
    {
        if (!IsGamepadAvailable(i))
            return false;
#if PS2X_PAD_HAS_GLFW
        return glfwJoystickIsGamepad(i) != 0;
#else
        return true;
#endif
    }

    const char *joystickGuid(int i)
    {
#if PS2X_PAD_HAS_GLFW
        const char *guid = glfwGetJoystickGUID(i);
        return guid ? guid : "?";
#else
        (void)i;
        return "?";
#endif
    }

    void loadMappingsOnce()
    {
        const PadConfig &c = config();
        if (c.mappingsFile.empty() || lower(c.mappingsFile) == "none")
            return;
        std::ifstream in(c.mappingsFile, std::ios::binary);
        if (!in)
            return;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const int ok = SetGamepadMappings(text.c_str());
        std::cerr << "[pad] gamepad mappings from " << c.mappingsFile << ": " << (ok ? "applied" : "rejected") << std::endl;
    }

    // TEMP(transformers): padhot1. All observation and rescans stay on the host render thread.
    int selectGamepad()
    {
        // TEMP(transformers): preserve RC4 scripted startup, glyphs and PADTEST path.
        if (scriptedRun()) {
            if (!g_host.initialised)
                std::cerr << "[pad] no gamepad - keyboard only (connect one any time)" << std::endl;
            g_host.active=-1;
            g_activeGamepad.store(-1,std::memory_order_relaxed);
            return -1;
        }
        static ps2_padhot::State selection;
        static std::string previousTable;
        static std::array<bool,4> previousMapped{};
        static const auto epoch = std::chrono::steady_clock::now();
        static ps2_padhot::Backoff rescan;
        const auto now = std::chrono::steady_clock::now();
        // Arrival updates GLFW now; raylib refreshes its cached ready/state at the next event pump.
        static bool rescanPending=false;
        const bool rescanned=rescanPending;
        rescanPending=false;
        const PadConfig &c = config();
        std::array<ps2_padhot::Slot,4> slots{};
        std::ostringstream table;
        for (int i=0; i<4; ++i)
        {
            const bool present=IsGamepadAvailable(i);
            const bool mapped=isMappedGamepad(i);
            const char *name=present?GetGamepadName(i):"";
            table << (i?" | ":"") << i << ':' << present << '/' << mapped << " '"
                  << (name?name:"") << "' " << (present?joystickGuid(i):"-");
            slots[i].mapped=mapped;
            slots[i].identity=std::string(present?joystickGuid(i):"")+"|"+(name?name:"");
            if(mapped)
            {
                for(int btn=1; btn<kRawButtons; ++btn)
                    if(IsGamepadButtonDown(i,btn)) slots[i].buttons |= uint32_t(1)<<btn;
                slots[i].left=std::hypot(GetGamepadAxisMovement(i,GAMEPAD_AXIS_LEFT_X),
                                        GetGamepadAxisMovement(i,GAMEPAD_AXIS_LEFT_Y));
                slots[i].right=std::hypot(GetGamepadAxisMovement(i,GAMEPAD_AXIS_RIGHT_X),
                                         GetGamepadAxisMovement(i,GAMEPAD_AXIS_RIGHT_Y));
            }
            if (present && !mapped && !g_host.unmappedLogged[i])
            {
                g_host.unmappedLogged[i]=true;
                std::cerr << "[pad] joystick " << i << " '" << (name?name:"") << "' has NO gamepad mapping (GUID "
                          << joystickGuid(i) << ") - ignored; add an SDL_GameControllerDB line to "
                          << c.mappingsFile << " next to the ELF to use it" << std::endl;
            }
            if(!present) g_host.unmappedLogged[i]=false;
        }
        const auto choice=ps2_padhot::select(slots,c.gamepadIndex,selection);
        const bool pinEvent=choice.state.pinnedMissing!=selection.pinnedMissing;
        const int pick=choice.state.active;
        const bool changed=pick!=g_host.active || !g_host.initialised;
        const char* reason=rescanned && pick>=0?"rescan":choice.reason;
        if(!changed && !pinEvent)
            for(int i=0;i<4;++i)
                if(previousMapped[i] && !slots[i].mapped) reason="disconnect";
        for(int i=0;i<4;++i) previousMapped[i]=slots[i].mapped;
        const bool tableChanged=table.str()!=previousTable;
        if(tableChanged || changed || pinEvent)
        {
            std::cerr << "[pad] slots t=" << std::chrono::duration<double>(now-epoch).count()
                      << " vs=" << ps2dbg::vsyncTick.load(std::memory_order_relaxed) << ": "
                      << table.str() << " | active=" << pick << " reason=" << reason << std::endl;
            previousTable=table.str();
        }
        if(rescanned && tableChanged && pick<0)
            std::cerr << "[pad] rescan found joystick table change" << std::endl;
        if(rescanned && pick>=0)
            std::cerr << "[pad] rescan found mapped gamepad " << pick << std::endl;
        if(changed)
        {
            if(g_host.active>=0 && !slots[g_host.active].mapped)
                std::cerr << "[pad] gamepad " << g_host.active << " disconnected" << std::endl;
            if(pick>=0)
                std::cerr << "[pad] using gamepad " << pick << " '" << GetGamepadName(pick)
                          << "' (GUID " << joystickGuid(pick) << ")" << std::endl;
            else
                std::cerr << "[pad] no gamepad - keyboard only (connect one any time)" << std::endl;
            g_host.active=pick;
            g_activeGamepad.store(pick,std::memory_order_relaxed);
            g_host.triggerRest=TriggerRest{};
        }
        selection=choice.state;
        bool anyMapped=false;
        for(const auto& slot:slots) anyMapped |= slot.mapped;
        const double seconds=std::chrono::duration<double>(now-epoch).count();
        if(rescan.due(seconds,anyMapped)) {
            const auto begin=std::chrono::steady_clock::now();
            rescanPending=ps2HostPadRescan();
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            std::fprintf(stderr,"[pad] rescan took %.3f ms\n",ms);
            rescan.completed(seconds);
        }
        return pick;
    }

    bool anyKeyDown(const std::vector<int> &keys)
    {
        for (int k : keys)
            if (IsKeyDown(k))
                return true;
        return false;
    }

    // Keyboard stick: returns true (and the -1..1 axes) when any of its keys is held. base = KS_LUP or KS_RUP.
    bool keyboardStick(const PadConfig &c, int base, float &x, float &y)
    {
        x = (anyKeyDown(c.keyStick[base + 3]) ? 1.0f : 0.0f) - (anyKeyDown(c.keyStick[base + 2]) ? 1.0f : 0.0f);
        y = (anyKeyDown(c.keyStick[base + 1]) ? 1.0f : 0.0f) - (anyKeyDown(c.keyStick[base + 0]) ? 1.0f : 0.0f);
        return x != 0.0f || y != 0.0f;
    }

    void applyRumble(const PadConfig &c, int pad)
    {
        const uint32_t seq = g_actuatorSeq.load(std::memory_order_acquire);
        const uint32_t state = g_actuatorState.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        bool want = false;
        float low = 0.0f, high = 0.0f;
        if (c.rumble && pad >= 0 && state != 0u)
        {
            const uint8_t small = static_cast<uint8_t>(state & 0xFFu);
            const uint8_t large = static_cast<uint8_t>((state >> 8) & 0xFFu);
            low = (large / 255.0f) * c.rumbleStrength;      // large motor = low-frequency (left) motor
            high = (small ? 0.6f : 0.0f) * c.rumbleStrength; // small motor is on/off on a DualShock
            want = low > 0.0f || high > 0.0f;
        }
        if (seq != g_host.appliedSeq || state != g_host.appliedState)
        {
            g_host.appliedSeq = seq;
            g_host.appliedState = state;
            g_host.rumbleSince = now;
            if (want)
            {
                ps2HostRumbleSet(low, high);
                g_host.rumbleOn = true;
            }
            else if (g_host.rumbleOn)
            {
                ps2HostRumbleSet(0.0f, 0.0f);
                g_host.rumbleOn = false;
            }
            return;
        }
        // The game runs well below full speed: cap one unchanged rumble command so a slow frame never buzzes for seconds.
        if (g_host.rumbleOn && (!want || (c.rumbleMaxSeconds > 0.0f &&
            std::chrono::duration<float>(now - g_host.rumbleSince).count() > c.rumbleMaxSeconds)))
        {
            ps2HostRumbleSet(0.0f, 0.0f);
            g_host.rumbleOn = false;
        }
    }
}

// TEMP(transformers): called by the EE thread from scePadSetActDirect (Kernel/Stubs/Pad.cpp). Port 0 only.
void ps2PadSetActuators(int port, uint8_t smallMotor, uint8_t largeMotor)
{
    if (port != 0 || scriptedRun())
        return;
    g_actuatorState.store(static_cast<uint32_t>(smallMotor) | (static_cast<uint32_t>(largeMotor) << 8),
                          std::memory_order_relaxed);
    g_actuatorSeq.fetch_add(1u, std::memory_order_release);
}

// TEMP(transformers): stop any rumble (render loop exit). Render thread only.
void ps2PadStopHostRumble()
{
    if (g_host.rumbleOn)
    {
        ps2HostRumbleSet(0.0f, 0.0f);
        g_host.rumbleOn = false;
    }
}

// TEMP(transformers): active gamepad index (-1 = keyboard only) for F12 marks / the F11 overlay.
int ps2PadActiveGamepad()
{
    return g_activeGamepad.load(std::memory_order_relaxed);
}

// TEMP(transformers): qualification - pad state for the F12 mark line (render thread, ps2_playtest): buttons + left stick
// bytes the game last read on port 0, what Transformers makes of that stick (ps2_pad_map.h tfLeftStick), and the host
// left-stick magnitude of the last sample.
std::string ps2PadMarkInfo()
{
    const ps2_stubs::PadDebugSnapshot snap = ps2_stubs::getPadDebugSnapshot();
    const ps2_stubs::PadDebugPortSnapshot &p0 = snap.ports[0][0];
    if (!p0.open || !p0.lastReadOk)
        return "pad=(not read yet)";
    const uint8_t *gd = p0.lastData;
    const TfStickView v = tfLeftStick(gd[6], gd[7]);
    const unsigned held = static_cast<uint16_t>(~(gd[2] | (gd[3] << 8)));
    char buf[160];
    std::snprintf(buf, sizeof(buf), "pad=btn%04X LS=%02X/%02X game|m|=%.3f move=%.3f %s host|LS|=%.2f", held, gd[6], gd[7],
                  v.mag, v.move, tfMoveClassName(tfMoveClass(v)),
                  std::hypot(g_host.raw.axis[GAMEPAD_AXIS_LEFT_X], g_host.raw.axis[GAMEPAD_AXIS_LEFT_Y]));
    return buf;
}

// TEMP(transformers): qualification - F7 PAD TEST overlay. Render thread only (ps2_playtest::frame, between BeginDrawing
// and EndDrawing). Shows the last host sample (taken after the previous EndDrawing), its PS2 mapping, and the bytes the
// GAME received on its last scePadRead of port 0 (incl. the DS2 pressure bytes 16-19 = L1 R1 L2 R2).
void ps2PadSetTestOverlay(bool on)
{
    if (on == g_host.overlay)
        return;
    g_host.overlay = on;
    g_host.lastTestButtons = 0xFFFFu;
    g_host.lastTestRaw = 0xFFFFFFFFu;
    g_host.lastTestMove = -1;
    std::cerr << "[padtest] PAD TEST overlay " << (on ? "ON" : "OFF") << " (F7)" << std::endl;
}

bool ps2PadTestOverlayOn()
{
    return g_host.overlay;
}

namespace
{
    struct ChipRow
    {
        int x0, x, y, h, font, maxX;
        void chip(const char *label, bool on, Color onColor = Color{40, 200, 90, 235})
        {
            const int w = MeasureText(label, font) + 8;
            if (x + w > maxX && x > x0)
            {
                x = x0;
                y += h + 3;
            }
            DrawRectangle(x, y, w, h, on ? onColor : Color{55, 55, 60, 220});
            DrawRectangleLines(x, y, w, h, on ? Color{230, 255, 230, 255} : Color{110, 110, 120, 255});
            DrawText(label, x + 4, y + (h - font) / 2, font, on ? Color{0, 0, 0, 255} : Color{200, 200, 200, 255});
            x += w + 3;
        }
        void text(const char *t, Color col = Color{230, 230, 230, 255})
        {
            const int w = MeasureText(t, font);
            if (x + w > maxX && x > x0)
            {
                x = x0;
                y += h + 3;
            }
            DrawText(t, x, y + (h - font) / 2, font, col);
            x += w + 6;
        }
        void newline(int extra = 0)
        {
            x = x0;
            y += h + 3 + extra;
        }
    };
}

static void padTestMaybeScreenshot(); // unattended evidence: PS2X_PADTEST_SHOT_FRAMES

void ps2PadDrawTestOverlay(int fontSize, int top)
{
    if (!g_host.overlay)
        return;
    struct ShotAtEnd
    {
        ~ShotAtEnd() { padTestMaybeScreenshot(); }
    } shotAtEnd;
    const int font = std::max(10, fontSize - 2);
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const int panelW = std::min(sw - 12, std::max(560, sw / 2));
    const int h = font + 6;
    // Two passes: measure the height with an off-screen dry run would need a second renderer; a fixed estimate is fine.
    const int panelH = std::min(sh - top - 6, 17 * (h + 3) + 20); // qualification: +1 row (left stick as the game sees it)
    DrawRectangle(6, top, panelW, panelH, Color{0, 0, 0, 200});
    ChipRow r{14, 14, top + 6, h, font, 6 + panelW - 8};

    const RawPad &raw = g_host.raw;
    const Mapped &m = g_host.mapped;
    const int pad = g_host.active;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "PAD TEST (F7 closes)  gamepad %d: %s", pad, pad >= 0 ? GetGamepadName(pad) : "none - keyboard only");
    r.text(buf, Color{255, 220, 80, 255});
    r.newline(2);

    // 1) physical controller as raylib reports it
    r.text("PHYSICAL", Color{140, 200, 255, 255});
    std::snprintf(buf, sizeof(buf), "L2|LT %.2f", raw.axis[GAMEPAD_AXIS_LEFT_TRIGGER]);
    r.chip(buf, m.ltTravel > 0.0f && raw.connected);
    r.chip("L1|LB", raw.button[GAMEPAD_BUTTON_LEFT_TRIGGER_1]);
    r.chip("R1|RB", raw.button[GAMEPAD_BUTTON_RIGHT_TRIGGER_1]);
    std::snprintf(buf, sizeof(buf), "R2|RT %.2f", raw.axis[GAMEPAD_AXIS_RIGHT_TRIGGER]);
    r.chip(buf, m.rtTravel > 0.0f && raw.connected);
    r.newline();
    r.chip("Up", raw.button[GAMEPAD_BUTTON_LEFT_FACE_UP]);
    r.chip("Down", raw.button[GAMEPAD_BUTTON_LEFT_FACE_DOWN]);
    r.chip("Left", raw.button[GAMEPAD_BUTTON_LEFT_FACE_LEFT]);
    r.chip("Right", raw.button[GAMEPAD_BUTTON_LEFT_FACE_RIGHT]);
    r.chip("Tri|Y", raw.button[GAMEPAD_BUTTON_RIGHT_FACE_UP]);
    r.chip("Cir|B", raw.button[GAMEPAD_BUTTON_RIGHT_FACE_RIGHT]);
    r.chip("X|A", raw.button[GAMEPAD_BUTTON_RIGHT_FACE_DOWN]);
    r.chip("Sq|X", raw.button[GAMEPAD_BUTTON_RIGHT_FACE_LEFT]);
    r.newline();
    r.chip("Create|View", raw.button[GAMEPAD_BUTTON_MIDDLE_LEFT]);
    r.chip("Options|Menu", raw.button[GAMEPAD_BUTTON_MIDDLE_RIGHT]);
    r.chip("PS|Guide", raw.button[GAMEPAD_BUTTON_MIDDLE]);
    r.chip("L3", raw.button[GAMEPAD_BUTTON_LEFT_THUMB]);
    r.chip("R3", raw.button[GAMEPAD_BUTTON_RIGHT_THUMB]);
    std::snprintf(buf, sizeof(buf), "LS %+.2f %+.2f (%.2f)  RS %+.2f %+.2f", raw.axis[GAMEPAD_AXIS_LEFT_X],
                  raw.axis[GAMEPAD_AXIS_LEFT_Y], std::hypot(raw.axis[GAMEPAD_AXIS_LEFT_X], raw.axis[GAMEPAD_AXIS_LEFT_Y]),
                  raw.axis[GAMEPAD_AXIS_RIGHT_X], raw.axis[GAMEPAD_AXIS_RIGHT_Y]); // qualification: + LS magnitude
    r.text(buf);
    r.newline(4);

    // 2) PS2 pad the runtime builds from it (keyboard included)
    const uint16_t d = g_host.allDown;
    auto on = [d](int b) { return (d & (1u << b)) != 0u; };
    r.text("PS2 PAD", Color{140, 200, 255, 255});
    std::snprintf(buf, sizeof(buf), "L2 %u", static_cast<unsigned>(g_host.pressure & 0xFFu));
    r.chip(buf, on(B_L2));
    r.chip("L1", on(B_L1));
    r.chip("R1", on(B_R1));
    std::snprintf(buf, sizeof(buf), "R2 %u", static_cast<unsigned>(g_host.pressure >> 8));
    r.chip(buf, on(B_R2));
    r.newline();
    r.chip("Up", on(B_UP));
    r.chip("Down", on(B_DOWN));
    r.chip("Left", on(B_LEFT));
    r.chip("Right", on(B_RIGHT));
    r.chip("Triangle", on(B_TRIANGLE));
    r.chip("Circle", on(B_CIRCLE));
    r.chip("Cross", on(B_CROSS));
    r.chip("Square", on(B_SQUARE));
    r.newline();
    r.chip("Select", on(B_SELECT));
    r.chip("Start", on(B_START));
    r.chip("L3", on(B_L3));
    r.chip("R3", on(B_R3));
    std::snprintf(buf, sizeof(buf), "sticks LX %02X LY %02X RX %02X RY %02X", g_host.sticks & 0xFFu, (g_host.sticks >> 8) & 0xFFu,
                  (g_host.sticks >> 16) & 0xFFu, g_host.sticks >> 24);
    r.text(buf);
    { // TEMP(transformers): qualification - the left stick response in use ([sticks] range_left / sensitivity_left)
        const PadConfig &cfg = config();
        std::snprintf(buf, sizeof(buf), "(robot: %s x%.2f)", stickRangeName(cfg.rangeLeft), cfg.sensitivityLeft);
        r.text(buf, Color{170, 170, 170, 255});
    }
    r.newline();
    { // TEMP(transformers): F7 names the active mode and the separate vehicle curve.
        const auto vehicle=ps2_carstick::snapshot();const auto&cfg=config();
        const float anti=cfg.antiDeadzoneLeftVehicle<0?vehicle.deadzone:cfg.antiDeadzoneLeftVehicle;
        std::snprintf(buf,sizeof(buf),"vehicle: %s %.0f%%  lift %s%.2f x%.2f exp%.2f",ps2_carstick::name(vehicle.mode),100*vehicle.weight,cfg.antiDeadzoneLeftVehicle<0?"auto ":"",anti,cfg.sensitivityLeftVehicle,cfg.responseLeftVehicle);
        r.text(buf,Color{170,220,255,255});
    }
    r.newline(4);

    // 3) what the game actually received on its last read of port 0
    const ps2_stubs::PadDebugSnapshot snap = ps2_stubs::getPadDebugSnapshot();
    const ps2_stubs::PadDebugPortSnapshot &p0 = snap.ports[0][0];
    r.text("GAME READ", Color{140, 200, 255, 255});
    if (!p0.open || !p0.lastReadOk)
    {
        r.text("(port 0 not read by the game yet)");
        r.newline();
        return;
    }
    const uint8_t *gd = p0.lastData;
    const uint16_t gb = static_cast<uint16_t>(~(gd[2] | (gd[3] << 8)));
    std::snprintf(buf, sizeof(buf), "mode %02X %s  reads %u", gd[1],
                  p0.pressureEnabled ? "(DS2 pressure ON)" : (p0.analogMode ? "(analog)" : "(digital)"), p0.readCount);
    r.text(buf);
    r.newline();
    static const int kShown[] = {B_L2, B_L1, B_R1, B_R2, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_TRIANGLE, B_CIRCLE, B_CROSS,
                                 B_SQUARE, B_SELECT, B_START, B_L3, B_R3};
    for (int b : kShown)
        r.chip(kPs2ButtonNames[b], (gb & (1u << b)) != 0u, Color{80, 170, 255, 235});
    r.newline();
    { // TEMP(transformers): qualification - left stick as Transformers sees it (bytes 6/7 -> its 0.2 per-axis dead zone ->
      // |v|^3 > 0.99 = run; ps2_pad_map.h tfLeftStick). RUN lit = the robot sprints at this stick position.
        const TfStickView v = tfLeftStick(gd[6], gd[7]);
        std::snprintf(buf, sizeof(buf), "robot view LX %02X LY %02X -> x %+.2f y %+.2f |m| %.3f move %.3f", gd[6], gd[7],
                      v.x, v.y, v.mag, v.move);
        r.text(buf);
        r.chip("RUN", v.run, Color{40, 200, 90, 235});
        r.chip("WALK", v.start && !v.run, Color{230, 200, 60, 235});
        r.chip("keep walking", v.walk && !v.start, Color{150, 130, 60, 235});
        r.text("(move = m^3: walk from rest > 0.50, run > 0.99)", Color{170, 170, 170, 255});
        r.newline();
    }
    if (p0.pressureEnabled)
    {
        std::snprintf(buf, sizeof(buf), "pressure bytes: L1 %u  R1 %u  L2 %u  R2 %u   (16-19)   X %u  Sq %u  Tri %u  Cir %u",
                      gd[16], gd[17], gd[18], gd[19], gd[14], gd[15], gd[12], gd[13]);
        r.text(buf, Color{180, 255, 180, 255});
        r.newline();
    }
}

// TEMP(transformers): qualification - PS2X_PADTEST_SHOT_FRAMES=<n>[,<n>...] saves the window (game + overlay) after the
// n-th overlay frame to PS2X_DUMP_DIR (else the working dir) as padtest_<n>.png - for unattended checks of the panel.
static void padTestMaybeScreenshot()
{
    static std::vector<long> s_frames = []()
    {
        std::vector<long> out;
        for (const char *p = std::getenv("PS2X_PADTEST_SHOT_FRAMES"); p && *p;)
        {
            char *end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p)
                break;
            out.push_back(v);
            p = (*end == ',') ? end + 1 : end;
        }
        return out;
    }();
    static long s_count = 0;
    ++s_count;
    if (s_frames.empty() || s_count != s_frames.front())
        return;
    s_frames.erase(s_frames.begin());
    const char *dir = std::getenv("PS2X_DUMP_DIR");
    const std::string path = std::string(dir && *dir ? dir : ".") + "/padtest_" + std::to_string(s_count) + ".png";
    rlDrawRenderBatchActive(); // DrawText/DrawRectangle are batched until EndDrawing: flush so the panel is in the read
    Image img = LoadImageFromScreen();
    if (img.data)
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8); // opaque, like the F12 marks
    const bool ok = img.data && ExportImage(img, path.c_str());
    if (img.data)
        UnloadImage(img);
    std::cerr << "[padtest] screenshot " << path << (ok ? " saved" : " FAILED") << std::endl;
}

// TEMP(transformers): called by the render loop once per host frame (ps2_runtime.cpp, after EndDrawing).
void ps2PadSampleHostInput()
{
    const PadConfig &c = config();
    if (!g_host.initialised)
    {
        loadMappingsOnce();
        if (const char *v = std::getenv("PS2X_PADTEST"); v && v[0] == '1') // qualification: open the F7 overlay at start
            ps2PadSetTestOverlay(true);
    }

    uint16_t down = 0u;
    for (int b = 0; b < B_COUNT; ++b)
        if (anyKeyDown(c.keys[b]))
            down = static_cast<uint16_t>(down | (1u << b));

    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    uint8_t pressL2 = 0u, pressR2 = 0u;
    const int pad = selectGamepad();
    g_host.initialised = true;
    RawPad raw;
    if (pad >= 0)
    {
        raw.connected = true;
        for (int btn = 1; btn < kRawButtons; ++btn)
            raw.button[btn] = IsGamepadButtonDown(pad, btn);
        for (int ax = 0; ax < kRawAxes; ++ax)
            raw.axis[ax] = GetGamepadAxisMovement(pad, ax);
    }
    const auto vehicle=ps2_carstick::snapshot();
    const Mapped m = mapGamepad(raw, c, g_host.triggerRest, vehicle.weight, vehicle.deadzone);
    if (ps2_glyphs::knob() != ps2_glyphs::Knob::Off)
    { // GAME(transformers): publish host-only glyph state; GS consumes it under its own ordering lock.
        static bool lastKeyboard = false;
        static uint16_t priorKeys = 0, priorPad = 0;
        static int priorDevice = -2;
        static float priorAxes[4] = {};
        bool keyboardActivity = (down & ~priorKeys) != 0;
        for (int i = 0; i < KS_COUNT; ++i)
            for (int key : c.keyStick[i]) keyboardActivity |= IsKeyPressed(key);
        bool padActivity = (m.buttons & ~priorPad) != 0;
        const float axes[] = {m.lx, m.ly, m.rx, m.ry};
        for (int i = 0; i < 4; ++i) {
            padActivity |= std::fabs(axes[i]) > .25f && std::fabs(axes[i] - priorAxes[i]) > .10f;
            priorAxes[i] = axes[i];
        }
        if (pad != priorDevice) lastKeyboard = pad < 0;
        if (keyboardActivity) lastKeyboard = true;
        else if (padActivity) lastKeyboard = false;
        priorKeys = down; priorPad = m.buttons; priorDevice = pad;
        ps2_glyphs::Input prompts;
        const char *guid = "";
#if PS2X_PAD_HAS_GLFW
        if (pad >= 0) guid = glfwGetJoystickGUID(pad);
#endif
        prompts.style = (lastKeyboard || pad < 0) ? ps2_glyphs::Style::Keyboard :
            ps2_glyphs::deviceStyle(GetGamepadName(pad) ? GetGamepadName(pad) : "", guid ? guid : "");
        { // qualification F2: log every change of the device-picked style (and what PS2X_GLYPHS finally draws)
            static int loggedStyle = -1, loggedPad = -3;
            if (static_cast<int>(prompts.style) != loggedStyle || pad != loggedPad)
            {
                loggedStyle = static_cast<int>(prompts.style);
                loggedPad = pad;
                const char *knobText = std::getenv("PS2X_GLYPHS");
                std::fprintf(stderr, "[glyphs] auto style=%s (pad %d '%s' GUID %s%s) PS2X_GLYPHS=%s -> draws %s\n",
                             ps2_glyphs::styleName(prompts.style), pad,
                             pad >= 0 && GetGamepadName(pad) ? GetGamepadName(pad) : "-", guid && *guid ? guid : "-",
                             lastKeyboard && pad >= 0 ? ", last input keyboard" : "",
                             knobText && *knobText ? knobText : "(unset=auto)",
                             ps2_glyphs::styleName(ps2_glyphs::selected(prompts.style)));
            }
        }
        for (int b = 0; b < B_COUNT; ++b) {
            prompts.binding[b].pad = c.pads[b].empty() ? -1 : c.pads[b].front();
            prompts.binding[b].key = c.keys[b].empty() ? -1 : c.keys[b].front();
        }
        ps2_glyphs::publish(prompts);
    }

    if (pad >= 0)
    {
        down = static_cast<uint16_t>(down | m.buttons);
        pressL2 = m.pressL2;
        pressR2 = m.pressR2;
        lx = m.lx;
        ly = m.ly;
        rx = m.rx;
        ry = m.ry;
    }
    g_host.raw = raw;
    g_host.mapped = m;
    // Keyboard (or any digital source) L2/R2 = full pressure.
    if ((down & (1u << B_L2)) && pressL2 == 0u)
        pressL2 = 0xFFu;
    if ((down & (1u << B_R2)) && pressR2 == 0u)
        pressR2 = 0xFFu;

    float kx = 0.0f, ky = 0.0f;
    const bool keyboardLeft = keyboardStick(c, KS_LUP, kx, ky);
    if (keyboardLeft) // WASD = left stick (movement)
    {
        lx = kx;
        ly = ky;
    }
    if (keyboardStick(c, KS_RUP, kx, ky)) // IJKL = right stick (camera)
    {
        rx = kx;
        ry = ky;
    }
    float menuX=raw.axis[c.swapSticks?GAMEPAD_AXIS_RIGHT_X:GAMEPAD_AXIS_LEFT_X];
    float menuY=raw.axis[c.swapSticks?GAMEPAD_AXIS_RIGHT_Y:GAMEPAD_AXIS_LEFT_Y];
    if(c.invertLX)menuX=-menuX;
    if(c.invertLY)menuY=-menuY;
    ps2_menustick::observe(menuX,menuY,pad>=0,keyboardLeft); // GAME(transformers): raw gesture, no robot lift
    uint32_t sticks = static_cast<uint32_t>(axisByte(lx)) | (static_cast<uint32_t>(axisByte(ly)) << 8) |
                            (static_cast<uint32_t>(axisByte(rx)) << 16) | (static_cast<uint32_t>(axisByte(ry)) << 24);
    // TEMP(transformers): PC1 focus isolation, scripted pad bytes remain literal.
    if (!scriptedRun() && ps2x::ui::filterPadInput(down, sticks)) {
        g_latchedButtons.store(0, std::memory_order_relaxed);
        g_latchedPressure.store(0, std::memory_order_relaxed);
    }
    if (!scriptedRun() && ps2x::ui::menuOpen()) pressL2=pressR2=0;
    const uint16_t pressure = static_cast<uint16_t>(pressL2 | (pressR2 << 8));
    g_host.allDown = down;
    g_host.pressure = pressure;
    g_host.sticks = sticks;
    g_currentSticks.store(sticks, std::memory_order_relaxed);
    g_currentPressure.store(pressure, std::memory_order_relaxed);
    g_currentButtons.store(down, std::memory_order_relaxed);
    g_latchedButtons.fetch_or(down, std::memory_order_relaxed);
    { // latched pressure: keep the per-trigger maximum since the last poll
        uint16_t old = g_latchedPressure.load(std::memory_order_relaxed);
        for (;;)
        {
            const uint16_t merged = static_cast<uint16_t>(std::max<uint16_t>(old & 0xFFu, pressL2) |
                                                          (std::max<uint16_t>(old >> 8, pressR2) << 8));
            if (merged == old || g_latchedPressure.compare_exchange_weak(old, merged, std::memory_order_relaxed))
                break;
        }
    }

    if (c.logInput && down != g_host.lastLoggedButtons)
    {
        g_host.lastLoggedButtons = down;
        std::fprintf(stderr, "[pad] host buttons=0x%04x sticks=%08x press=%04x pad=%d\n", down, sticks, pressure, pad);
    }

    if (g_host.overlay) // qualification: while the PAD TEST overlay is open, log every change for the playtest log
    {
        uint32_t rawBits = 0u;
        for (int btn = 1; btn < kRawButtons; ++btn)
            if (raw.button[btn])
                rawBits |= 1u << btn;
        if (down != g_host.lastTestButtons || rawBits != g_host.lastTestRaw)
        {
            g_host.lastTestButtons = down;
            g_host.lastTestRaw = rawBits;
            std::string rawNames, ps2Names;
            for (int btn = 1; btn < kRawButtons; ++btn)
                if (rawBits & (1u << btn))
                    rawNames += std::string(rawNames.empty() ? "" : "+") + kRawButtonNames[btn];
            for (int b = 0; b < B_COUNT; ++b)
                if (down & (1u << b))
                    ps2Names += std::string(ps2Names.empty() ? "" : "+") + kPs2ButtonNames[b];
            std::fprintf(stderr, "[padtest] raw=%s LT=%+.2f RT=%+.2f -> ps2=%s pressL2=%u pressR2=%u\n",
                         rawNames.empty() ? "-" : rawNames.c_str(), raw.axis[GAMEPAD_AXIS_LEFT_TRIGGER],
                         raw.axis[GAMEPAD_AXIS_RIGHT_TRIGGER], ps2Names.empty() ? "-" : ps2Names.c_str(),
                         static_cast<unsigned>(pressL2), static_cast<unsigned>(pressR2));
        }
    }

    // TEMP(transformers): qualification - with the F7 overlay open (or log_input = 1) log every change of what Transformers
    // makes of the left stick (idle / walk / RUN, ps2_pad_map.h tfLeftStick), so playtest marks carry the stick state.
    if (g_host.overlay || c.logInput)
    {
        const uint8_t lxb = static_cast<uint8_t>(sticks & 0xFFu), lyb = static_cast<uint8_t>((sticks >> 8) & 0xFFu);
        const TfStickView v = tfLeftStick(lxb, lyb);
        const int moveClass = tfMoveClass(v);
        if (moveClass != g_host.lastTestMove)
        {
            g_host.lastTestMove = moveClass;
            std::fprintf(stderr,
                         "[padtest] left stick raw=(%+.3f,%+.3f) |r|=%.3f -> ps2 LX=%02X LY=%02X -> game |m|=%.3f move=%.3f %s\n",
                         raw.axis[GAMEPAD_AXIS_LEFT_X], raw.axis[GAMEPAD_AXIS_LEFT_Y],
                         std::hypot(raw.axis[GAMEPAD_AXIS_LEFT_X], raw.axis[GAMEPAD_AXIS_LEFT_Y]), lxb, lyb, v.mag, v.move,
                         tfMoveClassName(moveClass));
        }
    }

    if (!scriptedRun())
        applyRumble(c, pad);
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    ps2_perfscope::Scope perf(ps2_perfscope::EePad); // TEMP(transformers) qualification
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker; // informational only: Pad.cpp reports the port's real mode (0x41/0x73/0x79)
    data[2] = data[3] = 0xFFu;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
    if (port != 0)
        return true; // one physical controller: port 1 is connected but idle (and must not consume port 0's latch)

    // Held at the last host sample, or pressed at any point since the previous poll (see g_latchedButtons). The EE
    // thread only reads the atomics filled by ps2PadSampleHostInput() on the render thread.
    const uint16_t pressed = static_cast<uint16_t>(g_currentButtons.load(std::memory_order_relaxed) |
                                                   g_latchedButtons.exchange(0, std::memory_order_relaxed));
    const uint16_t curPress = g_currentPressure.load(std::memory_order_relaxed);
    const uint16_t latPress = g_latchedPressure.exchange(0, std::memory_order_relaxed);
    const uint32_t sticks = g_currentSticks.load(std::memory_order_relaxed);
    data[6] = static_cast<uint8_t>(sticks);
    data[7] = static_cast<uint8_t>(sticks >> 8);
    data[4] = static_cast<uint8_t>(sticks >> 16);
    data[5] = static_cast<uint8_t>(sticks >> 24);

    const uint16_t btns = static_cast<uint16_t>(~pressed); // PS2 pad buttons are active-low
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);

    // Analog L2/R2 pressure (0 when released): the larger of the current and the latched-since-last-poll value.
    uint8_t l2 = static_cast<uint8_t>(std::max<unsigned>(curPress & 0xFFu, latPress & 0xFFu));
    uint8_t r2 = static_cast<uint8_t>(std::max<unsigned>(curPress >> 8, latPress >> 8));
    if ((pressed & (1u << B_L2)) && l2 == 0u)
        l2 = 0xFFu;
    if ((pressed & (1u << B_R2)) && r2 == 0u)
        r2 = 0xFFu;
    data[18] = l2; // libpad positions: 16 L1, 17 R1, 18 L2, 19 R2 (qualification; L2 used to travel in [17])
    data[19] = r2;
    data[31] = kPressureValidMarker;
    return true;
}
