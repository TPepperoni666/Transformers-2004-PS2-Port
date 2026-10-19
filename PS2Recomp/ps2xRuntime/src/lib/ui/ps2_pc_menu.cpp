int ps2PadActiveGamepad(); // TEMP(transformers): runtime render-thread selector
#include "runtime/ps2_resolution.h"
#include "../ps2_perfscope.h"
#include "../debug_counters.h"
#include "../ps2_pc_bindings.h"
#include "pc1_font.h"
#include "runtime/ps2_pc_live.h"
// GAME(transformers) PC options menu, stage 1 - Dear ImGui overlay over the game (qualification, 2026-10-09).
// Design: private qualification evidence sections 2 (registry), 4 (pages), 6 (UI). Settings: runtime/ps2_settings.h.
// Shares the F1 Runtime Debugger's ImGui context; main.cpp owns the single rlImGui setup and calls us every frame.
#include "ps2_pc_menu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_pc_menu_state.h"
#include "runtime/ps2_settings.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "pcnav_input.h"
#include "imgui_impl_raylib.h"
#include "rlImGui.h"
#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <array>
#include <sstream>

#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
struct GLFWwindow; struct GLFWmonitor;
extern "C" GLFWwindow* glfwGetCurrentContext();
extern "C" GLFWmonitor** glfwGetMonitors(int* count);
struct ResoptVideoMode {int width,height,redBits,greenBits,blueBits,refreshRate;};
extern "C" const ResoptVideoMode* glfwGetVideoModes(GLFWmonitor*,int*);
extern "C" const ResoptVideoMode* glfwGetVideoMode(GLFWmonitor*);
extern "C" void glfwGetWindowPos(GLFWwindow*,int*,int*);
extern "C" void glfwGetWindowSize(GLFWwindow*,int*,int*);
extern "C" void glfwGetFramebufferSize(GLFWwindow*,int*,int*);
extern "C" void glfwGetWindowContentScale(GLFWwindow*,float*,float*);
extern "C" void glfwSetWindowPos(GLFWwindow*,int,int);
extern "C" void glfwSetWindowMonitor(GLFWwindow*,GLFWmonitor*,int,int,int,int,int);
#endif

void ps2ResolutionWakeGS();
const char* ps2ResolutionOutputApplied();
namespace st = ps2x::settings;

namespace
{
    const char *const kPageNames[] = {"Display", "Aspect", "Presentation", "Frame rate", "Audio", "Controls", "Debug"};
    constexpr int kPageCount = 7;
    static_assert(kPageCount == static_cast<int>(st::Page::Count), "page list");

    std::map<int,std::array<int,3>> g_desktopModes;
    // TEMP(transformers): enumerate anew for each edit/apply; no monitor-pointer cache survives hotplug.
    std::vector<ps2x::resolution::Mode> displayModes(int monitor) {
        std::vector<ps2x::resolution::Mode> modes;
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
        int count=0;auto monitors=glfwGetMonitors(&count);
        static std::vector<GLFWmonitor*> topology;
        std::vector<GLFWmonitor*> current;if(monitors && count>0)current.assign(monitors,monitors+count);
        if(current!=topology){g_desktopModes.clear();topology=current;}
        if(monitor>=0 && monitor<count) {int n=0;auto list=glfwGetVideoModes(monitors[monitor],&n);
            for(int i=0;list && i<n;++i)modes.push_back({list[i].width,list[i].height,list[i].refreshRate});}
#endif
        return ps2x::resolution::normalizeModes(std::move(modes));
    }
    const char* displayKeys[]={"display.window_mode","display.monitor","display.fullscreen_resolution","display.output_resolution","display.window_width","display.window_height","renderer.pgs_ssaa"};
    std::vector<st::Entry> appliedDisplay,previousDisplay;
    int priorMonitor=0,priorX=0,priorY=0,priorW=0,priorH=0,priorMode=0;
    uint64_t renderRequest=0;
    bool displayAwaitingValidation=false;
    std::vector<st::Entry> displayEntries(){std::vector<st::Entry> v;for(auto key:displayKeys)v.push_back(st::entry(key));return v;}
    bool displayEqual(const std::vector<st::Entry>& a,const std::vector<st::Entry>& b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].value!=b[i].value)return false;return true;}
    bool isResolutionKey(const char* key){return std::strcmp(key,"display.output_resolution")==0||std::strcmp(key,"display.fullscreen_resolution")==0;}
    std::vector<int> bindingCodes(const std::string& value,bool keyboard) {
        std::vector<int> codes; std::istringstream in(value); std::string token;
        while(std::getline(in,token,',')) {
            token.erase(std::remove_if(token.begin(),token.end(),[](unsigned char c){return std::isspace(c);}),token.end());
            const int code=keyboard ? ps2x::input::keyFromName(token) : ps2_padmap::padSourceFromName(token);
            if(code>=0) codes.push_back(code);
        }
        return codes;
    }
    const char *featureStatus(const char *feature)
    {
        if (!feature)
            return "";
        if (std::strcmp(feature,"native")==0) return "Needs native renderer";
        if (std::strcmp(feature,"carstick2")==0) return "Needs qualified carstick2 adapter";
        if (std::strcmp(feature, "widescreen") == 0)
            return "not in this build yet (widescreen integration, PS2X_WIDESCREEN)";
        if (std::strcmp(feature, "upscale") == 0)
            return "not in this build yet (sharpening integration, PS2X_UPSCALE)";
        if (std::strcmp(feature, "field60") == 0)
            return "coming soon (PS2X_FIELD60)";
        if (std::strcmp(feature, "pgs") == 0)
            return "needs a PS2X_WITH_PGS build";
        if (std::strcmp(feature, "vu1aot") == 0)
            return "needs a PS2X_WITH_VU1_AOT build";
        return "not available in this build";
    }

    const char *featureShort(const char *feature)
    {
        if (!feature)
            return "";
        if (std::strcmp(feature,"native")==0) return "Needs native renderer";
        if (std::strcmp(feature,"carstick2")==0) return "Needs qualified carstick2 adapter";
        if (std::strcmp(feature, "field60") == 0)
            return "coming soon";
        if (std::strcmp(feature, "pgs") == 0)
            return "needs PGS build";
        if (std::strcmp(feature, "vu1aot") == 0)
            return "needs AOT build";
        return "not in this build";
    }

    std::string valueLabel(const st::Def &d, const st::Entry &e)
    {
        switch (d.type)
        {
        case st::Type::Bool:
        {
            const bool on = e.value == "on";
            return on ? "On" : "Off";
        }
        case st::Type::Enum:
        {
            if(e.value=="game_option") return "Follow saved game option";
            for (int i = 0; d.enumValues && d.enumValues[i]; ++i)
                if (e.value == d.enumValues[i])
                    return (d.enumLabels && d.enumLabels[i]) ? d.enumLabels[i] : d.enumValues[i];
            return e.value; // custom / raw env value
        }
        case st::Type::Float:
        {
            if(e.value=="auto") return "Auto";
            const double v = std::strtod(e.value.c_str(), nullptr);
            char buf[64];
            if (std::strcmp(d.key, "display.ui_scale") == 0 && v <= 0.0)
                return "Auto";
            if (std::strcmp(d.key, "audio.master_volume") == 0)
                std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(v * 100.0)));
            else if (std::strcmp(d.key, "display.ui_scale") == 0)
                std::snprintf(buf, sizeof(buf), "%.2fx", v);
            else
                std::snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
        }
        case st::Type::Int:
            return e.value;
        case st::Type::KeyBinding:
        case st::Type::String:
            return e.value.empty() ? std::string("(empty)") : e.value;
        }
        return e.value;
    }

    int activeGamepad()
    {
        return ps2PadActiveGamepad(); // TEMP(transformers): share runtime selection
    }

    // Our own gamepad -> ImGui mapping. rlImGui's (tag Raylib_5_5) swaps Circle/Square and Start/Select
    // (PC_OPTIONS_DESIGN.md 6.3), so B would not go back; we keep its handler off and feed these instead.
    struct PadMap
    {
        GamepadButton button;
        ImGuiKey key;
    };
    const PadMap kPadMap[] = {
        {GAMEPAD_BUTTON_LEFT_FACE_UP, ImGuiKey_GamepadDpadUp},
        {GAMEPAD_BUTTON_LEFT_FACE_DOWN, ImGuiKey_GamepadDpadDown},
        {GAMEPAD_BUTTON_LEFT_FACE_LEFT, ImGuiKey_GamepadDpadLeft},
        {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, ImGuiKey_GamepadDpadRight},
        {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, ImGuiKey_GamepadFaceDown},   // A / Cross   = activate
        {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, ImGuiKey_GamepadFaceRight}, // B / Circle  = back
        {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, ImGuiKey_GamepadFaceLeft},   // X / Square  = reset row
        {GAMEPAD_BUTTON_RIGHT_FACE_UP, ImGuiKey_GamepadFaceUp},       // Y / Triangle
        {GAMEPAD_BUTTON_LEFT_TRIGGER_1, ImGuiKey_GamepadL1},
        {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, ImGuiKey_GamepadR1},
        {GAMEPAD_BUTTON_LEFT_TRIGGER_2, ImGuiKey_GamepadL2},
        {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, ImGuiKey_GamepadR2},
        {GAMEPAD_BUTTON_LEFT_THUMB, ImGuiKey_GamepadL3},
        {GAMEPAD_BUTTON_RIGHT_THUMB, ImGuiKey_GamepadR3},
        {GAMEPAD_BUTTON_MIDDLE_RIGHT, ImGuiKey_GamepadStart},
        {GAMEPAD_BUTTON_MIDDLE_LEFT, ImGuiKey_GamepadBack},
    };
    bool g_padKeysDown = false;
    pcnav::Input g_navigation;
    pcnav::Direction g_step = pcnav::None;
    int g_navigationPad = -1;
    bool g_focusKeep = false;

    void releaseGamepadKeys(ImGuiIO &io);
    void feedGamepad(ImGuiIO &io)
    {
        const int pad = activeGamepad();
        g_step = pcnav::None;
        if (pad != g_navigationPad) { releaseGamepadKeys(io); g_navigation.reset(); g_navigationPad=pad; }
        if (pad < 0) { releaseGamepadKeys(io); return; }
        unsigned dpad=0;
        for (int i=0;i<4;++i) if(IsGamepadButtonDown(pad,kPadMap[i].button)) dpad|=1u<<i;
        g_step=g_navigation.step(GetGamepadAxisMovement(pad,GAMEPAD_AXIS_LEFT_X),
            GetGamepadAxisMovement(pad,GAMEPAD_AXIS_LEFT_Y),dpad,GetTime());
        const pcnav::Direction directions[]={pcnav::Up,pcnav::Down,pcnav::Left,pcnav::Right};
        for(int i=0;i<4;++i)
            io.AddKeyEvent(kPadMap[i].key,g_step==directions[i] && i<2);
        for (int i=4;i<IM_ARRAYSIZE(kPadMap);++i)
            io.AddKeyEvent(kPadMap[i].key,IsGamepadButtonDown(pad,kPadMap[i].button));
        g_padKeysDown = true;
    }

    void releaseGamepadKeys(ImGuiIO &io)
    {
        if (!g_padKeysDown)
            return;
        for (const PadMap &m : kPadMap)
            io.AddKeyEvent(m.key, false);
        for (ImGuiKey k : {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp,
                           ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight,
                           ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown})
            io.AddKeyAnalogEvent(k, false, 0.0f);
        g_padKeysDown = false;
    }

    bool pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true)
    {
        for (ImGuiKey k : keys)
            if (ImGui::IsKeyPressed(k, repeat))
                return true;
        return false;
    }

    uint64_t guestVsync(PS2Runtime &rt) { return rt.memory().gs().vsyncTick.load(std::memory_order_relaxed); }

    // TEMP(transformers): autotest key injection queue (ImGui keys pressed+released on the next frame).
    std::vector<ImGuiKey> g_injectKeys;
}

// ================================================================================================================
void PS2PcMenu::initialize(PS2Runtime &runtime)
{
    (void)runtime;
    if (m_initialized)
        return;
    m_initialized = true;
    st::declareWired("widescreen"); st::declareWired("upscale");
    if(st::wired("pgs")) st::declareWired("field60");
    SetExitKey(KEY_NULL); // Esc no longer kills the game; quitting moves into the menu (input.esc_action)

    // A clearer font than ImGui's 13 px default at handheld scale, when the OS has one (Windows; not under Wine).
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig; fontConfig.FontDataOwnedByAtlas=false;
    m_font=io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(pc1Font),int(sizeof(pc1Font)),18.0f,&fontConfig);

    if (!st::harnessMode())
        m_hintUntil = GetTime() + 8.0;
    if (const char *at = std::getenv("PS2X_PCMENU_AUTOTEST"); at && *at) // TEMP(transformers)
    {
        m_autotestVs = std::atoll(at);
        m_autotestStage = 0;
        const char *dir = std::getenv("PS2X_DUMP_DIR");
        m_autotestDir = (dir && *dir) ? dir : ".";
        std::cout << "[pcmenu] autotest armed at vs " << m_autotestVs << " -> " << m_autotestDir << std::endl;
    }
    if (!st::harnessMode() || m_autotestStage >= 0) applyLiveSettings(true);
    std::cout << "[pcmenu] ready: F10 / Esc / hold Select+Start 0.5 s" << (m_font ? " (font Karla)" : "") << std::endl;
}

void PS2PcMenu::shutdown()
{
    if (!m_initialized)
        return;
    if (m_open)
        ps2x::ui::setMenuOpen(false, false);
    st::saveIfDirty(); // design 2.2: written on Save and at clean exit
    m_initialized = false;
}

void PS2PcMenu::setOpen(bool open)
{
    if (open == m_open)
        return;
    m_open = open;
    g_navigation.reset();
    g_step=pcnav::None;
    m_confirmQuit = m_confirmClose = false;
    ps2x::ui::setMenuOpen(open, st::getBool("ui.pause_game"));
    if (open)
    {
        m_justOpened = true;
        m_focusFirstRow = true;
        m_requestedTab = m_page;
        m_hintUntil = 0.0;
    }
    std::cout << "[pcmenu] " << (open ? "open" : "close") << std::endl;
}

float PS2PcMenu::uiScale() const
{
    double s = st::getFloat("display.ui_scale");
    if (s <= 0.0)
        s = std::max(1.0, GetScreenHeight() / 720.0);
    return static_cast<float>(std::clamp(s, 0.75, 3.0));
}

void PS2PcMenu::applyWindowMode(int mode)
{
    // 0 windowed, 1 borderless, 2 full screen (exclusive at the desktop resolution)
    const bool isFull = IsWindowFullscreen();
    const bool isBorderless = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    if (!isFull && !isBorderless)
    {
        m_lastWindowedW = GetScreenWidth();
        m_lastWindowedH = GetScreenHeight();
    }
    if (mode != 2 && isFull)
        ToggleFullscreen();
    if (mode != 1 && isBorderless)
        ToggleBorderlessWindowed();
    if (mode == 1 && !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        ToggleBorderlessWindowed();
    if (mode == 2) {
        const int monitor=GetCurrentMonitor();
        if(!g_desktopModes.count(monitor)) g_desktopModes[monitor]={GetMonitorWidth(monitor),GetMonitorHeight(monitor),GetMonitorRefreshRate(monitor)};
        const auto desktop=g_desktopModes[monitor];
        int w=desktop[0],h=desktop[1],hz=desktop[2];
        const auto resolution=st::get("display.fullscreen_resolution");
        if(resolution!="desktop") std::sscanf(resolution.c_str(),"%dx%d@%d",&w,&h,&hz);
        if(IsWindowFullscreen()) ToggleFullscreen();
        SetWindowSize(w,h); ToggleFullscreen();
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
        int count=0; auto monitors=glfwGetMonitors(&count);
        if(monitor>=0 && monitor<count) glfwSetWindowMonitor(glfwGetCurrentContext(),monitors[monitor],0,0,w,h,hz);
#endif
    }
    if (mode == 0 && m_lastWindowedW > 0 && (isFull || isBorderless))
        SetWindowSize(m_lastWindowedW, m_lastWindowedH);
    m_appliedWindowMode = mode;
    if(mode>0) m_fullscreenPreferredMode=mode;
}

// TEMP(transformers): combined display/output/GS transaction, one ten-second monotonic deadline.
void PS2PcMenu::revertResolutionSettings() {
    if(previousDisplay.empty())return;
    if(priorMonitor>=GetMonitorCount()) {
        priorMonitor=std::max(0,GetCurrentMonitor());priorMode=0;
        previousDisplay[0].value="windowed";previousDisplay[1].value=std::to_string(priorMonitor);previousDisplay[2].value="desktop";
    }
    if(priorMode==2 && previousDisplay[2].value!="desktop") {
        ps2x::resolution::Mode oldMode;const auto modes=displayModes(priorMonitor);
        if(!ps2x::resolution::parseMode(previousDisplay[2].value,oldMode) || std::find(modes.begin(),modes.end(),oldMode)==modes.end()) {
            priorMode=0;previousDisplay[0].value="windowed";previousDisplay[2].value="desktop";
        }
    }
    st::restoreHostEntries(previousDisplay);
    if(priorMonitor<GetMonitorCount())SetWindowMonitor(priorMonitor);
    applyWindowMode(priorMode);
    if(priorMode==0)SetWindowSize(priorW,priorH);
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
    if(priorMode==0)glfwSetWindowPos(glfwGetCurrentContext(),priorX,priorY);
#endif
    renderRequest=ps2x::resolution::mailbox().request(std::atoi(previousDisplay.back().value.c_str()));
    std::fprintf(stderr,"[resopt2:display] rollback requested output=%s ssaa=%s generation=%llu\n",previousDisplay[3].value.c_str(),previousDisplay.back().value.c_str(),(unsigned long long)renderRequest);
    appliedDisplay=previousDisplay;previousDisplay.clear();
    m_resolutionDeadline=0;displayAwaitingValidation=false;
    m_appliedVersion=0;m_status="Display settings reverted";m_statusUntil=GetTime()+8;
}
void PS2PcMenu::keepResolutionSettings(){previousDisplay.clear();m_resolutionDeadline=0;displayAwaitingValidation=false;}
void PS2PcMenu::applyResolutionSettings(bool force) {
    namespace res=ps2x::resolution;
    ps2ResolutionWakeGS();
    if(m_resolutionDeadline>0 && (GetTime()>=m_resolutionDeadline || (!st::harnessMode() && !IsWindowFocused()))) {revertResolutionSettings();return;}
    if(st::getInt("display.monitor")>=GetMonitorCount() && GetMonitorCount()>0) {
        if(!previousDisplay.empty()){revertResolutionSettings();return;}
        auto safe=displayEntries();safe[0].value="windowed";safe[1].value=std::to_string(std::max(0,GetCurrentMonitor()));safe[2].value="desktop";safe[3].value="desktop";
        st::restoreHostEntries(safe);appliedDisplay=safe;applyWindowMode(0);
        m_status="Monitor removed; restored a safe window";m_statusUntil=GetTime()+8;return;
    }
    const auto requested=displayEntries();
    if(appliedDisplay.empty()) {
        appliedDisplay=requested;
        // The actual startup window is the rollback baseline even with CLI locks.
        appliedDisplay[0].value=IsWindowFullscreen()?"fullscreen":IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)?"borderless":"windowed";
        appliedDisplay[1].value=std::to_string(GetCurrentMonitor());
        appliedDisplay[2].value="desktop";appliedDisplay[3].value="desktop";
        appliedDisplay[4].value=std::to_string(GetScreenWidth());appliedDisplay[5].value=std::to_string(GetScreenHeight());
        appliedDisplay[6].value="1";
        if(st::source("display.window_width")==st::Source::Default)appliedDisplay[4]=requested[4];
        if(st::source("display.window_height")==st::Source::Default)appliedDisplay[5]=requested[5];
    }
    const auto gs=res::mailbox().status();
    if(m_resolutionDeadline>0 && st::getInt("display.monitor")>=GetMonitorCount()){revertResolutionSettings();return;}
    if(displayAwaitingValidation) {
        const int monitor=st::getInt("display.monitor");
        if(monitor>=GetMonitorCount() || (gs.acknowledged>=renderRequest && !gs.error.empty())) {revertResolutionSettings();return;}
        const auto output=st::get("display.output_resolution");
        if(output!=appliedDisplay[3].value) {revertResolutionSettings();return;}
        if(output!=ps2ResolutionOutputApplied())return; // wait for render-owner allocation/ack
        // Validate the physical client for selected window/exclusive modes after the OS has pumped events.
        res::Mode mode;const int windowMode=std::max(0,st::getEnumIndex("display.window_mode"));
        const auto text=windowMode==2?st::get("display.fullscreen_resolution"):output;
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
        if(windowMode==2 && text!="desktop" && res::parseMode(text,mode)) {
            int count=0;auto monitors=glfwGetMonitors(&count);const auto actual=(monitor>=0&&monitor<count)?glfwGetVideoMode(monitors[monitor]):nullptr;
            if(!actual || actual->width!=mode.width || actual->height!=mode.height || actual->refreshRate!=mode.hz){revertResolutionSettings();return;}
        }
#endif
        if(windowMode!=1 && text!="desktop" && res::parseMode(text,mode) && (GetRenderWidth()!=mode.width || GetRenderHeight()!=mode.height)) {revertResolutionSettings();return;}
        if((!gs.ready && st::getInt("renderer.pgs_ssaa")==1) || (gs.ready && gs.acknowledged>=renderRequest))displayAwaitingValidation=false;
    }
    if(displayEqual(requested,appliedDisplay))return;
    const int monitor=st::getInt("display.monitor");
    auto modes=displayModes(monitor);res::Mode selected;
    const int windowMode=std::max(0,st::getEnumIndex("display.window_mode"));
    const auto output=requested[3].value;
    auto valid=[&](const std::string& text){return text=="desktop" || (res::parseMode(text,selected) && std::find(modes.begin(),modes.end(),selected)!=modes.end());};
    const int samples=std::atoi(requested.back().value.c_str());
    if(modes.empty() || !valid(output) || (windowMode==2 && !valid(requested[2].value)) || (gs.ready && samples>gs.maxSamples)) {
        st::restoreHostEntries(appliedDisplay);m_status="Requested mode/rate unavailable; previous settings retained";m_statusUntil=GetTime()+8;return;
    }
    if(!m_open && !st::harnessMode())setOpen(true); // TEMP(transformers): startup/external changes expose keep/revert immediately
    if(previousDisplay.empty()) {
        previousDisplay=appliedDisplay;priorMonitor=GetCurrentMonitor();priorW=GetScreenWidth();priorH=GetScreenHeight();
        priorMode=IsWindowFullscreen()?2:IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)?1:0;
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
        glfwGetWindowPos(glfwGetCurrentContext(),&priorX,&priorY);
#endif
        m_resolutionDeadline=GetTime()+10;
    }
    if(monitor!=GetCurrentMonitor())SetWindowMonitor(monitor);
    applyWindowMode(windowMode);
    if(windowMode==0) {
        int w=st::getInt("display.window_width"),h=st::getInt("display.window_height");
        if(output!="desktop" && res::parseMode(output,selected)){w=selected.width;h=selected.height;}
        if(w>0 && h>0) {
            float sx=1,sy=1;
#if !defined(PLATFORM_VITA) && !defined(__ANDROID__)
            glfwGetWindowContentScale(glfwGetCurrentContext(),&sx,&sy);
#endif
            SetWindowSize(int(std::round(w/std::max(0.1f,sx))),int(std::round(h/std::max(0.1f,sy))));
        }
    }
    renderRequest=res::mailbox().request(samples);appliedDisplay=requested;displayAwaitingValidation=true;
    std::fprintf(stderr,"[resopt2:display] requested output=%s mode=%d ssaa=%d generation=%llu\n",output.c_str(),windowMode,samples,(unsigned long long)renderRequest);
}

void PS2PcMenu::applyLiveSettings(bool force)
{
    applyResolutionSettings(force);
    const uint64_t v = st::version();
    if (st::get("display.aspect")=="auto") ps2x::ui::g_pcAspectPending.store(std::clamp(float(GetScreenWidth())/std::max(1,GetScreenHeight()),4.0f/3.0f,32.0f/9.0f),std::memory_order_relaxed);
    if (force || v != m_appliedVersion)
    {
        m_appliedVersion = v;
        static std::string priorPreset;
        const std::string preset=st::get("ui.preset");
        if(preset!=priorPreset && preset!="custom") {
            priorPreset=preset;
            st::set("display.window_mode",preset=="original" ? "windowed" : "borderless");
            st::set("display.aspect",preset=="original" ? "4:3" : "auto");
            st::set("display.scaling_filter",preset=="original" ? "nearest" : "fsr1");
            st::set("display.ui_scale",preset=="handheld" ? "1.75" : "auto");
            st::setBool("display.vsync",true);
        } else priorPreset=preset;
        if (st::source("display.aspect") != st::Source::Default) {
            const std::string value = st::get("display.aspect");
            float aspect=4.0f/3.0f;
            if (value=="game_option") aspect=-2.0f;
            else if (value=="auto") aspect=float(GetScreenWidth())/std::max(1,GetScreenHeight());
            else { char* end=nullptr; aspect=std::strtof(value.c_str(),&end); if(end && *end==':') aspect/=std::max(.01f,std::strtof(end+1,nullptr)); }
            ps2x::ui::g_pcAspectPending.store(aspect==-2.0f ? -2.0f : aspect>0 ? std::clamp(aspect,4.0f/3.0f,32.0f/9.0f) : 0.0f,std::memory_order_relaxed);
        } else ps2x::ui::g_pcAspectPending.store(-1.0f,std::memory_order_relaxed);
        if (force) ps2x::ui::g_pcAspect.store(ps2x::ui::g_pcAspectPending.load(std::memory_order_relaxed),std::memory_order_relaxed);
        ps2x::ui::g_pcPillarbox.store(st::source("display.fmv_aspect")==st::Source::Default ? -1 : st::get("display.fmv_aspect")=="pillarbox",std::memory_order_relaxed);
        if (st::source("display.vsync")!=st::Source::Default) {
            if(st::getBool("display.vsync")) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
        }
        int rate=st::get("framerate.present_rate")=="display" ? GetMonitorRefreshRate(GetCurrentMonitor()) : 60;
        const std::string cap=st::get("framerate.fps_cap");
        if(st::source("framerate.fps_cap")!=st::Source::Default) {
            if(cap=="off") rate=0;
            else if(cap=="auto") { rate=GetMonitorRefreshRate(GetCurrentMonitor()); if(st::get("framerate.vrr")=="on") rate-=std::max(4,int(rate*.05f)); }
            else rate=int(std::strtod(cap.c_str(),nullptr));
        }
        if (st::source("framerate.present_rate")!=st::Source::Default || st::source("framerate.fps_cap")!=st::Source::Default) SetTargetFPS(std::max(0,rate));
        const int combo = st::getEnumIndex("input.menu_combo");
        ps2x::ui::setCombo(combo == 1 ? ps2x::ui::Combo::L3R3 : combo == 2 ? ps2x::ui::Combo::Off : ps2x::ui::Combo::SelectStart);
        if (m_open)
            ps2x::ui::setMenuOpen(true, st::getBool("ui.pause_game"));
    }

}

// TEMP(transformers): deterministic host-only requests, literal guest input/script untouched.
void PS2PcMenu::serviceResolutionTest(PS2Runtime& runtime) {
    if(!std::getenv("PS2X_RESOPT2_HOST_SEQUENCE"))return;
    struct Step{uint64_t tick;std::string output;int samples;};
    static std::vector<Step> steps;static size_t index=0;static bool parsed=false;
    if(!parsed){parsed=true;std::istringstream in(std::getenv("PS2X_RESOPT2_HOST_SEQUENCE"));std::string item;
        while(std::getline(in,item,',')){const auto a=item.find(':'),b=item.rfind(':');if(a==std::string::npos||a==b)continue;
            steps.push_back({std::strtoull(item.substr(0,a).c_str(),nullptr,10),item.substr(a+1,b-a-1),std::atoi(item.substr(b+1).c_str())});}}
    const auto tick=guestVsync(runtime);
    if(index<steps.size()&&tick>=steps[index].tick && m_resolutionDeadline==0){const auto& step=steps[index++];
        st::set("display.output_resolution",step.output);st::set("renderer.pgs_ssaa",std::to_string(step.samples));
        std::fprintf(stderr,"[resopt2:host] vs=%llu output=%s ssaa=%d requested\n",(unsigned long long)tick,step.output.c_str(),step.samples);}
    applyResolutionSettings(false);
    if(m_resolutionDeadline>0&&!displayAwaitingValidation){keepResolutionSettings();std::fprintf(stderr,"[resopt2:host] vs=%llu confirmed\n",(unsigned long long)tick);}
}

void PS2PcMenu::update(PS2Runtime &runtime)
{
    if (!m_initialized)
        return;
    applyLiveSettings(false);
    bool toggle = IsKeyPressed(KEY_F10);
    if (ps2x::ui::consumeComboToggle())
        toggle = true;
    if (toggle)
    {
        if (m_open)
            requestClose();
        else
            setOpen(true);
    }
    else if (!m_open && IsKeyPressed(KEY_ESCAPE))
    {
        setOpen(true);
    }
    if (IsKeyPressed(KEY_ENTER) && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)))
    {
        const int mode = std::max(0, st::getEnumIndex("display.window_mode"));
        st::setEnumIndex("display.window_mode", mode == 0 ? m_fullscreenPreferredMode : 0); // Alt+Enter
    }
    if (m_autotestStage >= 0)
        autotestStep(runtime);
}

bool PS2PcMenu::wantsImGuiFrame() const
{
    return m_initialized && (m_open || (st::get("framerate.show_fps") != "off") || GetTime() < m_hintUntil);
}

void PS2PcMenu::beginImGuiFrame()
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplRaylib_NewFrame(); // display size, delta time, cursor (the first half of rlImGuiBegin)
    constexpr ImGuiConfigFlags kNav = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags &= ~kNav; // rlImGui's own (swapped) gamepad map only runs with NavEnableGamepad set
    ImGui_ImplRaylib_ProcessEvents();
    if (m_open)
    {
        io.ConfigFlags |= kNav;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigNavCursorVisibleAlways = true;
        feedGamepad(io);
        for (ImGuiKey k : g_injectKeys) // TEMP(transformers): autotest
        {
            io.AddKeyEvent(k, true);
            io.AddKeyEvent(k, false);
        }
        g_injectKeys.clear();
    }
    else
    {
        io.ConfigNavCursorVisibleAlways = false;
        releaseGamepadKeys(io);
    }
    ImGui::NewFrame();
    // Rows own horizontal edits; ImGui must not move focus horizontally on the page.
    if(m_open && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) &&
       (GImGui->NavMoveDir==ImGuiDir_Left || GImGui->NavMoveDir==ImGuiDir_Right))
        ImGui::NavMoveRequestCancel();
}

void PS2PcMenu::requestClose()
{
    if (st::dirty() && st::writable())
    {
        m_confirmClose = true;
        return;
    }
    setOpen(false);
}

void PS2PcMenu::draw(PS2Runtime &runtime)
{
    if (!m_initialized)
        return;
    if (st::get("framerate.show_fps") != "off")
        drawFpsOverlay(runtime);
    if (!m_open)
    {
        if (GetTime() < m_hintUntil)
            drawHint();
        return;
    }
    drawMenuWindow(runtime);
    m_justOpened = false;
}

void PS2PcMenu::drawHint()
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + vp->WorkSize.y - 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##pcmenu_hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextUnformatted("PC settings: F10 or hold Select + Start");
    }
    ImGui::End();
}

void PS2PcMenu::drawFpsOverlay(PS2Runtime &runtime)
{
    const double now = GetTime();
    const uint64_t vs = guestVsync(runtime);
    const auto pads=ps2dbg::padReads.load(std::memory_order_relaxed);
    if (m_fpsT0 == 0.0 || now - m_fpsT0 >= 1.0)
    {
        if (m_fpsT0 != 0.0)
        {
            m_guestVsPerSec = static_cast<float>((vs - m_fpsVs0) / (now - m_fpsT0));
            m_gameFps = static_cast<float>((pads-m_fpsPads0)/(now-m_fpsT0));
        }
        m_fpsT0 = now;
        m_fpsVs0 = vs; m_fpsPads0=pads;
    }
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##pcmenu_fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Game %.1f fps | host %d fps | guest %.1f vsync/s (%.0f%% speed)%s", m_gameFps, GetFPS(), m_guestVsPerSec,
                    m_guestVsPerSec / 59.94f * 100.0f, ps2x::ui::eePaused() ? " | paused" : "");
        if(st::get("framerate.show_fps")=="detailed") ImGui::TextUnformatted(ps2_perfscope::overlayText().c_str());
    }
    ImGui::End();
}

void PS2PcMenu::drawSettingRow(const void *defPtr, bool debugPage)
{
    const st::Def &d = *static_cast<const st::Def *>(defPtr);
    const st::Entry e = st::entry(d.key);
    const bool avail = st::available(d);
    const bool isLocked = e.source == st::Source::Env || e.source == st::Source::Cli;
    const bool editable = avail && !isLocked && d.type != st::Type::String;
    std::string status, shortStatus; // full text for the footer, short tag for the row
    if (!avail)
    {
        status = featureStatus(d.feature);
        shortStatus = featureShort(d.feature);
    }
    else if (isLocked)
    {
        status = e.source == st::Source::Env ? std::string("set by ") + d.envAlias + " (environment wins over settings.ini)"
                                             : std::string("set by --set on the command line");
        shortStatus = "locked";
    }
    else if (e.restartPending)
    {
        status = shortStatus = "restart required";
    }
    else if (d.apply == st::Apply::Restart)
    {
        status = "applies on restart";
        shortStatus = "restart";
    }

    if(d.type==st::Type::KeyBinding) {
        const bool keyboard=std::strncmp(d.key,"input.kb.",9)==0; const auto codes=bindingCodes(e.value,keyboard);
        for(const auto& other:st::defs()) if(other.type==st::Type::KeyBinding && &other!=&d && (std::strncmp(other.key,"input.kb.",9)==0)==keyboard) {
            const auto otherCodes=bindingCodes(st::get(other.key),keyboard);
            bool match=false; for(int code:codes) match |= std::find(otherCodes.begin(),otherCodes.end(),code)!=otherCodes.end();
            if(match) { shortStatus=isLocked ? "locked / conflict" : "conflict"; status += std::string(" Also bound to ")+other.key+"."; break; }
        }
    }
    ImGui::PushID(d.key);
    // Read-only status rows remain focusable so a pad can scroll and read help.
    if (m_focusFirstRow && avail)
    {
        ImGui::SetKeyboardFocusHere();
        m_focusFirstRow = false;
    }
    const float rowH = std::max(48.0f,ImGui::GetFrameHeight() * 1.15f);
    const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, rowH));
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    if(focused) ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
        ImGui::GetItemRectMax(),IM_COL32(65,100,155,100),4.0f);
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float textY = rmin.y + (rowH - ImGui::GetFontSize()) * 0.5f;
    const float pad = ImGui::GetStyle().FramePadding.x;
    const ImU32 colText = ImGui::GetColorU32(avail ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 colWarn = avail && !isLocked && e.restartPending ? IM_COL32(255, 196, 64, 255)
                          : isLocked                                ? IM_COL32(120, 190, 255, 255)
                                                                    : colDim;
    const float width = rmax.x - rmin.x;
    auto clippedText = [&](float x0, float x1, ImU32 col, const char *text)
    {
        if (x1 <= x0)
            return;
        dl->PushClipRect(ImVec2(x0, rmin.y), ImVec2(x1, rmax.y), true);
        dl->AddText(ImVec2(x0, textY), col, text);
        dl->PopClipRect();
    };
    std::string value = valueLabel(d, e);
    if (focused && editable && d.type != st::Type::String && d.type != st::Type::KeyBinding)
        value = "<  " + value + "  >";
    if (debugPage)
    {
        // columns: key | value | source [env alias] (+ availability / restart state)
        const float c1 = rmin.x + width * 0.34f, c2 = rmin.x + width * 0.57f;
        clippedText(rmin.x + pad, c1 - pad, colText, d.key);
        clippedText(c1, c2 - pad, colText, value.c_str());
        std::string src = st::sourceName(e.source);
        if (d.envAlias)
            src += std::string(" [") + d.envAlias + "]";
        if (!avail)
            src = std::string(featureShort(d.feature)) + (d.envAlias ? std::string(" [") + d.envAlias + "]" : std::string());
        else if (e.restartPending)
            src += " (restart)";
        clippedText(c2, rmax.x - pad, (e.source == st::Source::Default && !e.restartPending) ? colDim : colWarn, src.c_str());
    }
    else
    {
        const std::string tag = shortStatus;
        const float tagW = tag.empty() ? 0.0f : ImGui::CalcTextSize(tag.c_str()).x;
        const float statusX = tag.empty() ? rmax.x : rmax.x - tagW - pad;
        const float valueX = rmin.x + width * 0.42f;
        clippedText(rmin.x + pad, valueX - pad, colText, d.label);
        float textX = valueX;
        if (d.type == st::Type::Float && d.maxValue > d.minValue && std::strcmp(d.key, "display.ui_scale") != 0)
        {
            // a small bar for 0..1 values
            const float frac = static_cast<float>((std::strtod(e.value.c_str(), nullptr) - d.minValue) / (d.maxValue - d.minValue));
            const float barW = width * 0.16f, barH = ImGui::GetFontSize() * 0.45f;
            const ImVec2 b0(valueX, rmin.y + (rowH - barH) * 0.5f);
            dl->AddRectFilled(b0, ImVec2(b0.x + barW, b0.y + barH), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
            dl->AddRectFilled(b0, ImVec2(b0.x + barW * std::clamp(frac, 0.0f, 1.0f), b0.y + barH),
                              ImGui::GetColorU32(avail ? ImGuiCol_SliderGrabActive : ImGuiCol_TextDisabled), 3.0f);
            textX = b0.x + barW + pad * 2.0f;
        }
        clippedText(textX, statusX - pad, colText, value.c_str());
        if (!tag.empty())
            dl->AddText(ImVec2(statusX, textY), colWarn, tag.c_str());
    }

    if(editable && isResolutionKey(d.key)) {
        const auto modes=displayModes(st::getInt("display.monitor"));
        if(clicked)ImGui::OpenPopup("Resolution");
        if(ImGui::BeginPopup("Resolution")) {
            if(ImGui::Selectable("Desktop / automatic",e.value=="desktop")){st::set(d.key,"desktop");ImGui::CloseCurrentPopup();}
            for(const auto mode:modes){const auto text=ps2x::resolution::modeText(mode);if(ImGui::Selectable(text.c_str(),text==e.value)){st::set(d.key,text);ImGui::CloseCurrentPopup();}}
            if(ImGui::IsKeyPressed(ImGuiKey_Escape)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (editable && d.type == st::Type::KeyBinding) {
        static double captureUntil=0,captureStart=0; static std::string captureKey; static int captureDirection=-1;
        static bool append=false;
        const bool keyboard=std::strncmp(d.key,"input.kb.",9)==0;
        const bool stick=std::strstr(d.key,"_stick")!=nullptr;
        std::array<std::string,4> tuple={"none","none","none","none"};
        if(stick) { std::istringstream in(e.value); for(auto& v:tuple) std::getline(in,v,','); }
        auto choose=[&](const std::string& token,int direction) {
            std::string value=token;
            if(direction>=0) { tuple[direction]=token; value=tuple[0]+","+tuple[1]+","+tuple[2]+","+tuple[3]; }
            else if(append && e.value!="none" && e.value.find(',')==std::string::npos && token!="none" && e.value!=token) value=e.value+", "+token;
            st::set(d.key,value);
        };
        if(clicked) { captureUntil=0; ImGui::OpenPopup("Binding"); }
        if(ImGui::BeginPopup("Binding")) {
            ImGui::TextUnformatted(stick ? "Stick directions: up, left, down, right" : "Choose or capture a binding");
            if(!stick && ImGui::Button(append ? "Add second binding" : "Replace bindings")) append=!append;
            const char* padNames[]={"none","RIGHT_FACE_DOWN","RIGHT_FACE_RIGHT","RIGHT_FACE_LEFT","RIGHT_FACE_UP","LEFT_TRIGGER_1","RIGHT_TRIGGER_1","LEFT_TRIGGER_2","RIGHT_TRIGGER_2","MIDDLE_LEFT","MIDDLE_RIGHT","LEFT_THUMB","RIGHT_THUMB","LEFT_FACE_UP","LEFT_FACE_DOWN","LEFT_FACE_LEFT","LEFT_FACE_RIGHT",nullptr};
            auto choices=[&](int direction) {
                if(ImGui::Button(keyboard ? "Press a key (5 seconds)" : "Press a pad button (5 seconds)")) { captureStart=GetTime(); captureUntil=captureStart+5; captureKey=d.key; captureDirection=direction; }
                if(keyboard) {
                    if(ImGui::Selectable("none")) { choose("none",direction); ImGui::CloseCurrentPopup(); }
                    for(char c='A';c<='Z';++c) { const std::string name(1,c); if(ImGui::Selectable(name.c_str())) { choose(name,direction); ImGui::CloseCurrentPopup(); } }
                    const char* names[]={"UP","DOWN","LEFT","RIGHT","SPACE","ENTER","TAB","BACKSPACE","LEFT_SHIFT","RIGHT_SHIFT","LEFT_CONTROL","RIGHT_CONTROL","LEFT_ALT","RIGHT_ALT","ONE","THREE","KP_0","KP_1",nullptr};
                    for(int i=0;names[i];++i) if(ImGui::Selectable(names[i])) { choose(names[i],direction); ImGui::CloseCurrentPopup(); }
                } else for(int i=0;padNames[i];++i) if(ImGui::Selectable(padNames[i])) { choose(padNames[i],direction); ImGui::CloseCurrentPopup(); }
            };
            if(stick) {
                const char* directionNames[]={"Up","Left","Down","Right"};
                for(int i=0;i<4;++i) if(ImGui::BeginMenu(directionNames[i])) { choices(i); ImGui::EndMenu(); }
            } else choices(-1);
            if(captureUntil>0 && captureKey==d.key) {
                ImGui::Text("Waiting %.0f seconds (Esc/B cancels)",std::max(0.0,captureUntil-GetTime()));
                std::string token;
                if(GetTime()-captureStart>.1 && GetTime()<captureUntil && !ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                    if(keyboard) token=ps2x::input::keyNameFromCode(GetKeyPressed());
                    else { const int pad=activeGamepad(); if(pad>=0) for(int i=1;i<ps2_padmap::kRawButtons;++i) if(IsGamepadButtonPressed(pad,i)) {
                        if(i==GAMEPAD_BUTTON_LEFT_TRIGGER_2) token="LEFT_TRIGGER_2";
                        else if(i==GAMEPAD_BUTTON_RIGHT_TRIGGER_2) token="RIGHT_TRIGGER_2";
                        else for(int n=1;padNames[n];++n) if(ps2_padmap::padSourceFromName(padNames[n])==i) token=padNames[n];
                        break;
                    } }
                }
                if(!token.empty()) { choose(token,captureDirection); captureUntil=0; ImGui::CloseCurrentPopup(); }
                if(GetTime()>=captureUntil || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) captureUntil=0;
            }
            ImGui::EndPopup();
        }
    }
    if (editable)
    {
        int dir = 0;
        if (clicked && !isResolutionKey(d.key))
            dir = +1;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            dir = -1;
        if (focused && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId))
        {
            if (g_step==pcnav::Left || pressed({ImGuiKey_LeftArrow}))
                dir = -1;
            if (g_step==pcnav::Right || pressed({ImGuiKey_RightArrow}))
                dir = +1;
            if (pressed({ImGuiKey_GamepadFaceUp}, false)) { m_status=d.help ? d.help : ""; m_statusUntil=GetTime()+8.0; }
            if (pressed({ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft}, false))
                st::resetToDefault(d.key);
        }
        if (dir != 0)
        {
            if (d.type == st::Type::Int || d.type == st::Type::Float)
                st::stepNumber(d.key, dir * ((ImGui::IsKeyDown(ImGuiKey_GamepadL2) || ImGui::IsKeyDown(ImGuiKey_GamepadR2)) ? 5 : 1));
            else if(isResolutionKey(d.key)) {
                std::vector<std::string> choices={"desktop"};for(auto mode:displayModes(st::getInt("display.monitor")))choices.push_back(ps2x::resolution::modeText(mode));
                auto it=std::find(choices.begin(),choices.end(),e.value);int index=it==choices.end()?0:int(it-choices.begin());
                st::set(d.key,choices[(index+dir+int(choices.size()))%choices.size()]);
            } else if(std::strcmp(d.key,"renderer.pgs_ssaa")==0) {
                const int cap=ps2x::resolution::mailbox().status().maxSamples;std::vector<int> choices;for(int n:{1,2,4,8,16})if(n<=cap)choices.push_back(n);
                auto it=std::find(choices.begin(),choices.end(),std::atoi(e.value.c_str()));int index=it==choices.end()?0:int(it-choices.begin());
                st::set(d.key,std::to_string(choices[(index+dir+int(choices.size()))%choices.size()]));
            } else st::cycleEnum(d.key, dir);
        }
    }
    if (focused)
    {
        m_focusedKey = d.key;
        m_focusedHelp = d.help ? d.help : "";
        if (!status.empty())
            m_focusedHelp += m_focusedHelp.empty() ? status : "  (" + status + ")";
    }

    ImGui::PopID();
}

void PS2PcMenu::drawDebugPage(PS2Runtime &runtime)
{
    const std::filesystem::path file = st::filePath();
    ImGui::Text("settings.ini: %s", file.empty() ? "-" : file.string().c_str());
    ImGui::Text("status: %s%s", st::fileStatus().c_str(), st::dirty() ? "  (unsaved changes)" : "");
    const std::string wine = st::wineVersion();
    ImGui::Text("build: PGS %s | VU1 AOT %s | widescreen %s | upscale %s | field60 %s | host: %s",
                st::wired("pgs") ? "yes" : "no", st::wired("vu1aot") ? "yes" : "no", st::wired("widescreen") ? "yes" : "no",
                st::wired("upscale") ? "yes" : "no", st::wired("field60") ? "yes" : "no",
                wine.empty() ? "Windows" : ("Wine/Proton " + wine).c_str());
    ImGui::Text("guest vsync %llu | EE %s | pauses %llu | window %dx%d | monitor %d Hz",
                static_cast<unsigned long long>(guestVsync(runtime)), ps2x::ui::eePaused() ? "paused" : "running",
                static_cast<unsigned long long>(ps2x::ui::pausedCount()), GetScreenWidth(), GetScreenHeight(),
                GetMonitorRefreshRate(GetCurrentMonitor()));
    ImGui::SeparatorText("Every setting (value, source [env alias])");
    for (const st::Def &d : st::defs())
        drawSettingRow(&d, true);
    ImGui::SeparatorText("PS2X_* environment at start-up");
    const auto env = st::environmentPs2x();
    if (env.empty())
        ImGui::TextDisabled("(none)");
    for (const auto &kv : env)
    {
        // Selectable so a controller can scroll through the list
        const std::string line = kv.first + "=" + kv.second;
        ImGui::Selectable(line.c_str(), false);
        if (ImGui::IsItemFocused())
        {
            m_focusedKey.clear();
            m_focusedHelp = "Environment variables win over settings.ini (read-only here).";
        }
    }
}

void PS2PcMenu::drawPage(int page, PS2Runtime &runtime)
{
    if (page == static_cast<int>(st::Page::Debug))
    {
        drawDebugPage(runtime);
        return;
    }
    for (const st::Def &d : st::defs())
        if (static_cast<int>(d.page) == page)
            drawSettingRow(&d, false);

    if (page == static_cast<int>(st::Page::Audio)) ImGui::TextWrapped("Music, effects, dialogue and sound mode are saved by the game. Use its Audio Options.");
    if (page == static_cast<int>(st::Page::Controls)) ImGui::TextWrapped("Select a keyboard or pad binding to replace it. Vibration and Invert Y belong to the game's Options.");

}

void PS2PcMenu::drawMenuWindow(PS2Runtime &runtime)
{
    const float scale = uiScale();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 150));

    ImGui::PushFont(static_cast<ImFont *>(m_font), (m_font ? 18.0f : 13.0f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 24, 32, 240));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(60, 90, 140, 120));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(70, 110, 170, 160));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(80, 120, 190, 200));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(255, 200, 60, 255));

    const ImVec2 size(std::min(vp->WorkSize.x - 24.0f, 900.0f * scale), std::min(vp->WorkSize.y - 24.0f, 600.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (m_justOpened)
        ImGui::SetNextWindowFocus();
    m_focusedHelp.clear();
    const bool popupAtFrameStart=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
    if (ImGui::Begin("PC Settings##pcmenu", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings))
    {
        if(!m_justOpened && pressed({ImGuiKey_GamepadStart},false)) {
            if(m_resolutionDeadline>0) revertResolutionSettings();
            ImGui::ClosePopupToLevel(0,true);
            m_confirmQuit=m_confirmClose=false;
            requestClose();
        }
        // Page switching: L1/R1 on a pad, Q/E or PageUp/PageDown on the keyboard.
        if (!m_justOpened && !m_confirmQuit && !m_confirmClose && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId))
        {
            int delta = 0;
            if (pressed({ImGuiKey_GamepadL1, ImGuiKey_PageUp, ImGuiKey_Q}, false))
                delta = -1;
            if (pressed({ImGuiKey_GamepadR1, ImGuiKey_PageDown, ImGuiKey_E}, false))
                delta = +1;
            if (delta != 0)
            {
                m_page = (m_page + delta + kPageCount) % kPageCount;
                m_requestedTab = m_page;
                m_focusFirstRow = true;
            }
        }
        ImGui::TextDisabled("[L1]");
        ImGui::SameLine();
        if (ImGui::BeginTabBar("pages", ImGuiTabBarFlags_NoTooltip))
        {
            for (int p = 0; p < kPageCount; ++p)
            {
                const ImGuiTabItemFlags flags = (m_requestedTab == p) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kPageNames[p], nullptr, flags))
                {
                    if (m_page != p && m_requestedTab < 0)
                    {
                        m_page = p; // clicked with the mouse
                        m_focusFirstRow = true;
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        m_requestedTab = -1;
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("[R1]").x - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("[R1]");

        const float footerH = ImGui::GetFrameHeightWithSpacing() * 5.0f + ImGui::GetTextLineHeightWithSpacing() * 4.0f;
        if (ImGui::BeginChild("page", ImVec2(0.0f, -footerH), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_None)) {
            drawPage(m_page, runtime);
            const int pad=activeGamepad();
            if(pad>=0 && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
                const float y=GetGamepadAxisMovement(pad,GAMEPAD_AXIS_RIGHT_Y);
                if(std::fabs(y)>=0.35f) ImGui::SetScrollY(ImGui::GetScrollY()+y*600.0f*ImGui::GetIO().DeltaTime);
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::TextWrapped("%s", m_focusedHelp.empty() ? " " : m_focusedHelp.c_str());
        const double now = GetTime();
        if (!m_status.empty() && now < m_statusUntil)
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", m_status.c_str());
        else if (st::anyRestartPending())
            ImGui::TextColored(ImVec4(1.0f, 0.77f, 0.25f, 1.0f), "Some changes take effect after a restart.");
        else
            ImGui::TextDisabled("Up/Down row   Left/Right value   A confirm   B back   LB/RB tab   Start close");

        const bool canSave = st::writable() && st::dirty();
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save"))
        {
            std::string msg;
            st::save(&msg);
            m_status = msg;
            m_statusUntil = now + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!st::dirty());
        if (ImGui::Button("Revert"))
            st::revert();
        ImGui::EndDisabled();
        if (ImGui::Button("Defaults (this page)"))
            st::resetPage(static_cast<st::Page>(m_page));
        if (ImGui::Button("Close"))
            requestClose();
        if (ImGui::Button("Quit game"))
            m_confirmQuit = true;
        if (!st::writable())
        {
                ImGui::TextDisabled(st::harnessMode() ? "(test run: not saved)" : "(no settings file)");
        }

        if(m_resolutionDeadline>0) {
            if(!ImGui::IsPopupOpen("Keep display settings?")) g_focusKeep=true;
            ImGui::OpenPopup("Keep display settings?");
        }
        if(ImGui::BeginPopupModal("Keep display settings?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Reverting in %.0f seconds",std::max(0.0,m_resolutionDeadline-GetTime()));
            const auto gs=ps2x::resolution::mailbox().status();
            ImGui::Text("SSAA requested %d / applied %d%s",gs.wanted,gs.samples,gs.error.empty()?"":gs.error.c_str());
            if(g_focusKeep && !displayAwaitingValidation) {
                ImGui::SetKeyboardFocusHere(); g_focusKeep=false;
            }
            ImGui::BeginDisabled(displayAwaitingValidation);
            if(ImGui::Button("Keep")) {keepResolutionSettings();ImGui::CloseCurrentPopup();}
            ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
            if(ImGui::Button("Revert") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {revertResolutionSettings();ImGui::CloseCurrentPopup();}
            if(m_resolutionDeadline==0)ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Back / close (B, Esc, Backspace) - not on the frame the menu opened (the same Esc press opened it).
        const bool back = !m_justOpened && pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight, ImGuiKey_Backspace}, false);

        if (m_confirmQuit)
            ImGui::OpenPopup("Quit the game?");
        if (ImGui::BeginPopupModal("Quit the game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Unsaved game progress is lost.");
            if (ImGui::Button("Quit"))
            {
                std::cout << "[pcmenu] quit requested from the menu" << std::endl;
                st::saveIfDirty();
                runtime.requestStop();
                m_confirmQuit = false;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::Button("Cancel") || back)
            {
                m_confirmQuit = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (m_confirmClose)
        {
            ImGui::OpenPopup("Save changes?");
        }
        if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Keep the changes in settings.ini?");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::Button("Save"))
            {
                if (!st::save(&m_status)) { m_statusUntil=GetTime()+8; ImGui::EndPopup(); ImGui::End(); ImGui::PopStyleColor(5); ImGui::PopStyleVar(5); ImGui::PopFont(); return; }
                m_confirmClose = false;
                ImGui::CloseCurrentPopup();
                setOpen(false);
            }
            if (ImGui::Button("Discard"))
            {
                st::revert();
                m_confirmClose = false;
                ImGui::CloseCurrentPopup();
                setOpen(false);
            }
            if (ImGui::Button("Cancel") || back)
            {
                m_confirmClose = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (back && !popupAtFrameStart && !m_confirmQuit && !m_confirmClose && m_open && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId))
        {
            requestClose();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
}

void PS2PcMenu::afterRender(PS2Runtime &runtime)
{
    (void)runtime;
    if (m_pendingCapture.empty())
        return;
    rlDrawRenderBatchActive();
    Image img = LoadImageFromScreen();
    if (img.data)
    {
        ExportImage(img, m_pendingCapture.c_str());
        UnloadImage(img);
        std::cout << "[pcmenu] captured " << m_pendingCapture << std::endl;
    }
    m_pendingCapture.clear();
}

// TEMP(transformers): PS2X_PCMENU_AUTOTEST=<vsync>. Opens the menu at that guest vsync, walks every page with the
// same key paths a pad/keyboard uses (R1 = next page; Left x3 on Audio = volume 100% -> 85%), captures one window PNG
// per page into PS2X_DUMP_DIR, checks that guest time stood still while open, then closes the menu.
void PS2PcMenu::autotestStep(PS2Runtime &runtime)
{
    const double now = GetTime();
    const uint64_t vs = guestVsync(runtime);
    if (m_autotestStage == 0)
    {
        if (static_cast<int64_t>(vs) < m_autotestVs)
            return;
        m_page = 0;
        setOpen(true);
        m_autotestStage = 1;
        m_autotestT = now;
        m_autotestVsOpen = 0;
        std::cout << "[pcmenu] autotest: open at vs " << vs << std::endl;
        return;
    }
    if (m_autotestVsOpen == 0 && ps2x::ui::eePaused())
    {
        m_autotestVsOpen = vs;
        std::cout << "[pcmenu] autotest: EE parked at vs " << vs << std::endl;
    }
    const int pageStage = m_autotestStage - 1; // 0..kPageCount-1
    if (pageStage < kPageCount)
    {
        const double dt = now - m_autotestT;
        static int s_switchedStage = -1, s_injectedStage = -1;
        if (pageStage > 0 && dt >= 0.2 && s_switchedStage != m_autotestStage)
        {
            s_switchedStage = m_autotestStage;
            g_injectKeys = {ImGuiKey_GamepadR1}; // next page through the pad path (after the previous capture)
        }
        if (pageStage == static_cast<int>(st::Page::Audio) && dt >= 0.7 && s_injectedStage != m_autotestStage)
        {
            s_injectedStage = m_autotestStage;
            g_injectKeys.clear(); // keyboard path: 100% -> 85%
        }
        if (dt >= 1.5 && m_pendingCapture.empty())
        {
            char name[96];
            std::snprintf(name, sizeof(name), "pcmenu_%d_%s.png", pageStage + 1, kPageNames[pageStage]);
            std::string fname = name;
            std::replace(fname.begin(), fname.end(), ' ', '_');
            m_pendingCapture = (std::filesystem::path(m_autotestDir) / fname).string();
            ++m_autotestStage;
            m_autotestT = now;
        }
        return;
    }
    if (m_autotestStage == kPageCount + 1 && now - m_autotestT >= 0.5)
    {
        const bool frozen = m_autotestVsOpen != 0 && vs == m_autotestVsOpen;
        std::cout << "[pcmenu] autotest: vs parked=" << m_autotestVsOpen << " vs now=" << vs
                  << " frozen=" << (frozen ? 1 : 0) << " volume=" << st::get("audio.master_volume") << std::endl;
        setOpen(false);
        m_autotestStage = kPageCount + 2;
        m_autotestT = now;
        return;
    }
    if (m_autotestStage == kPageCount + 2 && now - m_autotestT >= 2.0)
    {
        std::cout << "[pcmenu] autotest: done, vs after close=" << vs << " (resumed=" << (vs > m_autotestVsOpen ? 1 : 0)
                  << ") pauses=" << ps2x::ui::pausedCount() << std::endl;
        m_autotestStage = -1;
    }
}
