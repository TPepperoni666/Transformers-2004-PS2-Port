#include "../ps2_pc_bindings.h"
// GAME(transformers) PC options stage 1: settings registry + settings.ini + env/--set precedence. See runtime/ps2_settings.h
// and private qualification evidence (sections 2.1-2.4). Settings integration, 2026-10-09.
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define CloseWindow WindowsCloseWindow
#define ShowCursor WindowsShowCursor
#include <windows.h>
#undef CloseWindow
#undef ShowCursor
#include <shlobj.h>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib") // SHGetFolderPathW (APPDATA fallback)
#pragma comment(lib, "user32.lib")  // GetSystemMetrics (aspect = auto)
#endif
#endif

#if !defined(_WIN32)
extern char **environ;
#endif

namespace ps2x::settings
{
namespace
{
    // ---- the registry (one list; design 2.3). Defaults == today's no-env behaviour. ---------------------------
    const char *const kWindowMode[] = {"windowed", "borderless", "fullscreen", nullptr};
    const char *const kWindowModeL[] = {"Windowed", "Borderless", "Full screen", nullptr};
    const char *const kAspect[] = {"4:3", "16:10", "16:9", "21:9", "32:9", "auto", nullptr};
    const char *const kAspectL[] = {"4:3 (original)", "16:10", "16:9", "21:9", "32:9", "Auto (desktop)", nullptr};
    const char *const kAspectEnv[] = {nullptr, "0", "16:10", "16:9", "21:9", "32:9", "auto", nullptr};
    const char *const kAspectMethod[] = {"horplus", "game", nullptr};
    const char *const kAspectMethodL[] = {"Hor+ (more view at the sides)", "Game's own 16:9 mode", nullptr};
    const char *const kAspectMethodEnv[] = {"horplus", "game", "stretch", nullptr};
    const char *const kFmvAspect[] = {"pillarbox", "stretch", nullptr};
    const char *const kFmvAspectL[] = {"Pillarbox 4:3", "Stretch", nullptr};
    const char *const kFmvAspectEnv[] = {nullptr, "0", nullptr};
    const char *const kFilter[] = {"nearest", "bilinear", nullptr};
    const char *const kFilterL[] = {"Nearest (sharp pixels)", "Bilinear (smooth)", nullptr};
    const char *const kBackend[] = {"cpu", "pgs", nullptr};
    const char *const kBackendL[] = {"CPU rasteriser", "parallel-gs (Vulkan)", nullptr};
    const char *const kBackendEnv[] = {"CPU", "PGS", nullptr};
    // TEMP(transformers): live scanout policy.
    const char *const kHires[] = {"auto", "on", "off", nullptr};
    const char *const kHiresEnv[] = {"auto", "1", "0", nullptr};
    const char *const kUpscale[] = {"off", "fsr1", "cas", nullptr};
    const char *const kUpscaleL[] = {"Off", "FSR 1 (EASU + RCAS)", "CAS (sharpen only)", nullptr};
    const char *const kUpscaleEnv[] = {"off", "fsr1", "cas", nullptr}; // qualification: unset = fsr1
    const char *const kFpsMode[] = {"original", "interpolated60", nullptr};
    const char *const kFpsModeL[] = {"Original (30)", "Interpolated 60", nullptr};
    const char *const kFpsModeEnv[] = {nullptr, "1", nullptr};
    const char *const kCombo[] = {"select+start", "l3+r3", "off", nullptr};
    const char *const kComboL[] = {"Hold Select + Start", "Hold L3 + R3", "Off", nullptr};
    const char *const kEsc[] = {"menu", "quit", nullptr};
    const char *const kEscL[] = {"Open this menu", "Quit the game", nullptr};

    constexpr double kNoMin = 0.0, kNoMax = 0.0, kNoStep = 0.0;
    using T = Type;
    using A = Apply;
    using P = Page;

    const char *const pc1enum0[] = {"windowed", "borderless", "fullscreen", nullptr};
    const char *const pc1enum1[] = {"game_option", "4:3", "16:10", "16:9", "21:9", "32:9", "auto", nullptr};
    const char *const pc1enum2[] = {"horplus", "game", "stretch", nullptr};
    const char *const pc1enum3[] = {"maps_to_aspect", "ignore", nullptr};
    const char *const pc1enum4[] = {"pillarbox", "stretch", nullptr};
    const char *const pc1enum5[] = {"stretch", "centered", "anchored", nullptr};
    const char *const pc1enum6[] = {"fsr1", "cas", "bilinear", "nearest", nullptr};
    const char *const pc1enum7[] = {"pgs", "cpu", nullptr};
    const char *const pc1enum8[] = {"emulated", "native", nullptr};
    const char *const pc1enum9[] = {"faithful", "pcplus", nullptr};
    const char *const pc1enum10[] = {"auto","match_display","1x","1.25x","1.5x","1.75x","2x","2.25x","2.5x","2.75x","3x","3.25x","3.5x","3.75x","4x",nullptr};
    const char *const pc1enum11[] = {"off","msaa2","msaa4","msaa8",nullptr};
    const char *const pc1enum12[] = {"off", "fsr3", "dlss", "xess", nullptr};
    const char *const pc1enum13[] = {"quality", "native_aa", "balanced", "performance", nullptr};
    const char *const pc1enum14[] = {"off", "fsr3", "dlss", nullptr};
    const char *const pc1enum15[] = {"original", "max", nullptr};
    const char *const pc1enum16[] = {"original", "bilinear", "aniso16", nullptr};
    const char *const pc1enum17[] = {"auto", "weave", "bob", nullptr};
    const char *const pc1enum18[] = {"original", "interpolated", nullptr};
    const char *const pc1enum19[] = {"display", "60", "90", "120", "144", "uncapped", nullptr};
    const char *const pc1enum20[] = {"auto", "on", "off", nullptr};
    const char *const pc1enum21[] = {"60", "display", nullptr};
    const char *const pc1enum22[] = {"off", "simple", "detailed", nullptr};
    const char *const pc1enum23[] = {"stretch", "fade", "silence", nullptr};
    const char *const pc1enum24[] = {"48000", "device", nullptr};
    const char *const pc1enum25[] = {"gaussian", "linear", nullptr};
    const char *const pc1enum26[] = {"select+start", "l3+r3", "off", nullptr};
    const char *const pc1enum27[] = {"custom", "original", "recommended", "handheld", nullptr};
    const char *const pc1enum28[] = {"original", "far", nullptr};
    const char *const pc1enum29[] = {"1", "2", "4", "8", "16", nullptr};
    const char *const pc1range[]={"circle","square",nullptr};
    const char *const pc1cull[]={"original","wide",nullptr};
    const char *const pc1cullLabels[]={"Off","On",nullptr};
    const char *const pc1cullEnv[]={"0","1",nullptr};
    const char *const pc1output[]={"60","30",nullptr};
    const char *const pc1res[]={"desktop","1280x800@60","1920x1200@60","2560x1080@60","3440x1440@60","3840x2160@60",nullptr};
    const char *const resoptSSAA[]={"1","2","4","8","16",nullptr};
    const char *const resoptSSAAL[]={"1x1 (Off)","1x2 (2 samples)","2x2 (4 samples)","2x4 (8 samples)","4x4 (16 samples)",nullptr};
    const std::vector<Def> &registry() {
        static const std::vector<Def> s_defs = {
            {"game.disc_image", T::String, "", 0, 0, 0, nullptr, nullptr, "PS2X_CD_IMAGE", nullptr, A::Restart, P::Debug, nullptr, "disc image", "Host PC setting.", Scope::Guest},
            {"display.window_mode", T::Enum, "windowed", 0, 0, 0, pc1enum0, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "window mode", "Host PC setting."},
            {"display.window_width", T::Int, "0", 0, 7680, 64, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "window width", "Host PC setting."},
            {"display.window_height", T::Int, "0", 0, 4320, 8, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "window height", "Host PC setting."},
            {"display.fullscreen_resolution", T::Enum, "desktop", 0, 0, 0, pc1res, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "Exclusive resolution", "Exclusive mode only. A ten-second keep/revert prompt protects display changes."},
            {"display.output_resolution", T::Enum, "desktop", 0,0,0, pc1res,nullptr,nullptr,nullptr,A::Live,P::Display,nullptr,"Output resolution","Enumerated display modes. Borderless keeps desktop size and uses a separate output surface."},
            {"renderer.pgs_ssaa",T::Enum,"1",0,0,0,resoptSSAA,resoptSSAAL,"PS2X_PGS_SSAA",nullptr,A::Live,P::Presentation,"pgs-live-ssaa","Internal scale / SSAA","Emulated GS sample grid, not MSAA. Output remains independent; guest HUD is part of the GS image."},
            {"renderer.pgs_hires_scanout",T::Enum,"auto",0,0,0,kHires,nullptr,"PS2X_PGS_HIRES",kHiresEnv,A::Live,P::Presentation,"pgs-live-ssaa","Sharp output (show supersampled detail)","Auto preserves supersampled detail at 4x, 8x and 16x. Off resolves to the original PS2 output."},
            {"display.monitor", T::Int, "0", 0, 7, 1, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "monitor", "Host PC setting."},
            {"display.vsync", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "vsync", "Host PC setting."},
            {"display.aspect", T::Enum, "game_option", 0, 0, 0, pc1enum1, nullptr, "PS2X_WIDESCREEN", kAspectEnv, A::NextFrame, P::Aspect, nullptr, "aspect", "C2 Hor+: wider at the sides; below 16:9 retains the horizontal view and reveals more vertically.", Scope::Guest},
            {"display.aspect_method", T::Enum, "horplus", 0, 0, 0, pc1enum2, nullptr, "PS2X_WIDESCREEN_MODE", kAspectMethodEnv, A::Restart, P::Aspect, "aspect-method", "aspect method", "Hor+ mode in stage 1; game/stretch adapters pending.", Scope::Guest},
            {"display.game_toggle", T::Enum, "maps_to_aspect", 0, 0, 0, pc1enum3, nullptr, nullptr, nullptr, A::Restart, P::Aspect, "game-toggle", "game toggle", "Host PC setting.", Scope::Guest},
            {"display.fmv_aspect", T::Enum, "pillarbox", 0, 0, 0, pc1enum4, nullptr, "PS2X_WIDESCREEN_PILLARBOX", kFmvAspectEnv, A::Live, P::Aspect, nullptr, "fmv aspect", "Host PC setting."},
            {"display.hud_layout", T::Enum, "stretch", 0, 0, 0, pc1enum5, nullptr, nullptr, nullptr, A::Restart, P::Aspect, "native", "hud layout", "Needs native renderer."},
            {"display.scaling_filter", T::Enum, "fsr1", 0, 0, 0, pc1enum6, nullptr, "PS2X_UPSCALE", nullptr, A::Live, P::Presentation, nullptr, "scaling filter", "Host PC setting."},
            {"display.sharpness", T::Float, "0.8", 0, 1, 0.05, nullptr, nullptr, "PS2X_SHARPNESS", nullptr, A::Live, P::Presentation, nullptr, "sharpness", "Host PC setting."},
            {"display.integer_scale", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Presentation, "integer-scale", "integer scale", "Host PC setting."},
            {"display.brightness", T::Float, "1.0", 0.5, 2, 0.05, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "post-chain", "brightness", "Host PC setting."},
            {"display.gamma", T::Float, "1.0", 0.5, 2, 0.05, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "post-chain", "gamma", "Host PC setting."},
            {"display.ui_scale", T::Float, "auto", 0.75, 3, 0.25, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "ui scale", "Host PC setting."},
            {"renderer.backend", T::Enum, "cpu", 0, 0, 0, pc1enum7, nullptr, "PS2X_GS_BACKEND", nullptr, A::Restart, P::Presentation, nullptr, "backend", "Host PC setting."},
            {"renderer.renderer", T::Enum, "emulated", 0, 0, 0, pc1enum8, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "renderer", "Needs native renderer."},
            {"renderer.native_mode", T::Enum, "faithful", 0, 0, 0, pc1enum9, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "native mode", "Needs native renderer."},
            {"renderer.internal_resolution", T::Enum, "auto", 0, 0, 0, pc1enum10, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "internal resolution", "Needs native renderer."},
            {"renderer.aa", T::Enum, "off", 0, 0, 0, pc1enum11, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "Native MSAA", "True MSAA 1x/2x/4x/8x requires the native renderer. SSAA is separately named."},
            {"renderer.alpha_to_coverage", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "Hardware alpha to coverage", "Unavailable. Native prototype uses per-sample GS alpha tests instead."},
            {"renderer.upscaler", T::Enum, "off", 0, 0, 0, pc1enum12, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "upscaler", "Needs native renderer."},
            {"renderer.upscaler_quality", T::Enum, "quality", 0, 0, 0, pc1enum13, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "upscaler quality", "Needs native renderer."},
            {"renderer.frame_generation", T::Enum, "off", 0, 0, 0, pc1enum14, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "frame generation", "Needs native renderer."},
            {"renderer.detail_level", T::Enum, "original", 0, 0, 0, pc1enum15, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "detail-probe", "detail level", "Host PC setting.", Scope::Guest},
            {"renderer.texture_filtering", T::Enum, "original", 0, 0, 0, pc1enum16, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "native", "texture filtering", "Needs native renderer."},
            {"renderer.deinterlace", T::Enum, "auto", 0, 0, 0, pc1enum17, nullptr, nullptr, nullptr, A::Restart, P::Presentation, "deinterlace", "deinterlace", "Host PC setting."},
            {"renderer.shader_cache", T::Bool, "on", 0, 0, 0, nullptr, nullptr, "PS2X_PGS_PIPECACHE", nullptr, A::Restart, P::Presentation, nullptr, "shader cache", "Host PC setting."},
            {"framerate.mode", T::Enum, "original", 0, 0, 0, pc1enum18, nullptr, nullptr, nullptr, A::Restart, P::FrameRate, "native", "mode", "Needs native renderer."},
            {"framerate.target_fps", T::Enum, "display", 0, 0, 0, pc1enum19, nullptr, nullptr, nullptr, A::Restart, P::FrameRate, "native", "target fps", "Needs native renderer."},
            {"framerate.fps_cap", T::Float, "auto", 0, 360, 5, nullptr, nullptr, nullptr, nullptr, A::Live, P::FrameRate, nullptr, "fps cap", "Host PC setting."},
            {"framerate.vrr", T::Enum, "auto", 0, 0, 0, pc1enum20, nullptr, nullptr, nullptr, A::Live, P::FrameRate, nullptr, "vrr", "Host PC setting."},
            {"framerate.catch_up", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::FrameRate, "catch-up", "catch up", "Host PC setting."},
            {"framerate.present_rate", T::Enum, "display", 0, 0, 0, pc1enum21, nullptr, nullptr, nullptr, A::Live, P::FrameRate, nullptr, "present rate", "Host PC setting."},
            {"framerate.show_fps", T::Enum, "off", 0, 0, 0, pc1enum22, nullptr, nullptr, nullptr, A::Live, P::FrameRate, nullptr, "show fps", "Host PC setting."},
            {"audio.enabled", T::Bool, "on", 0, 0, 0, nullptr, nullptr, "PS2X_AUDIO", nullptr, A::Restart, P::Audio, nullptr, "enabled", "Host PC setting."},
            {"audio.master_volume", T::Float, "1.0", 0, 1, 0.05, nullptr, nullptr, "PS2X_AUDIO_VOLUME", nullptr, A::Live, P::Audio, nullptr, "master volume", "Host PC setting."},
            {"audio.latency_ms", T::String, "auto", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Audio, "audio-latency", "latency ms", "Host PC setting."},
            {"audio.underrun", T::Enum, "stretch", 0, 0, 0, pc1enum23, nullptr, nullptr, nullptr, A::Restart, P::Audio, "audio-underrun", "underrun", "Host PC setting."},
            {"audio.device_rate", T::Enum, "48000", 0, 0, 0, pc1enum24, nullptr, nullptr, nullptr, A::Restart, P::Audio, "audio-device", "device rate", "Host PC setting."},
            {"audio.interpolation", T::Enum, "gaussian", 0, 0, 0, pc1enum25, nullptr, nullptr, nullptr, A::Restart, P::Audio, "audio-interpolation", "interpolation", "Host PC setting."},
            {"audio.mute_on_focus_loss", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Audio, nullptr, "mute on focus loss", "Host PC setting."},
            {"input.pad.index", T::Int, "-1", -1, 3, 1, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / index", "Host PC setting."},
            {"input.pad.swap_confirm", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, "swap-confirm", "pad / swap confirm", "Host PC setting."},
            {"input.analog_mode", T::Bool, "on", 0, 0, 0, nullptr, nullptr, "PS2X_PAD_ANALOG", nullptr, A::Restart, P::Controls, nullptr, "analog mode", "Host PC setting.", Scope::Guest},
            {"input.invert_x_camera", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "invert x camera", "Host PC setting."},
            {"input.rumble", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Controls, nullptr, "Host rumble", "Host output gate; the game owns its memory-card vibration option."},
            {"input.menu_key", T::String, "F10", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Controls, "menu-key", "menu key", "Host PC setting."},
            {"input.menu_combo", T::Enum, "select+start", 0, 0, 0, pc1enum26, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "menu combo", "Host PC setting."},
            {"input.pad.deadzone_left", T::Float, ".05", 0, 0.9, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "deadzone left", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.deadzone_right", T::Float, ".05", 0, 0.9, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "deadzone right", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.anti_deadzone_left", T::Float, ".62", 0, 0.9, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "anti deadzone left", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.anti_deadzone_right", T::Float, ".2", 0, 0.9, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "anti deadzone right", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.sensitivity_left", T::Float, "1.33", 0.5, 2, 0.05, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "sensitivity left", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.sensitivity_right", T::Float, "1", 0.5, 2, 0.05, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "sensitivity right", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.outer_left", T::Float, ".95", 0.3, 1, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "outer left", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.outer_right", T::Float, ".95", 0.3, 1, 0.01, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "outer right", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.response_left_vehicle", T::Float, "1.15", 0.25, 4, 0.1, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "response left vehicle", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.sensitivity_left_vehicle", T::Float, "1", 0.1, 2, 0.05, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "sensitivity left vehicle", "Radial deadzone / stick response. Vehicle curve, independent of the unchanged robot response."},
            {"input.pad.anti_deadzone_left_vehicle", T::Float, "auto", 0, 0.9, 0.05, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "vehicle anti deadzone", "auto cancels the live game axis deadzone; 0 disables lift."},
            {"input.pad.range_left",T::Enum,"circle",0,0,0,pc1range,nullptr,nullptr,nullptr,A::Live,P::Controls,nullptr,"Left stick range","Circle or square response."},
            {"input.pad.range_right",T::Enum,"circle",0,0,0,pc1range,nullptr,nullptr,nullptr,A::Live,P::Controls,nullptr,"Right stick range","Circle or square response."},
            {"input.kb.up", T::KeyBinding, "UP", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / up", "Host PC setting."},
            {"input.kb.down", T::KeyBinding, "DOWN", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / down", "Host PC setting."},
            {"input.kb.left", T::KeyBinding, "LEFT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / left", "Host PC setting."},
            {"input.kb.right", T::KeyBinding, "RIGHT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / right", "Host PC setting."},
            {"input.kb.cross", T::KeyBinding, "X, SPACE", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / cross", "Host PC setting."},
            {"input.kb.circle", T::KeyBinding, "C", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / circle", "Host PC setting."},
            {"input.kb.square", T::KeyBinding, "Z, KP_0", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / square", "Host PC setting."},
            {"input.kb.triangle", T::KeyBinding, "V, KP_1", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / triangle", "Host PC setting."},
            {"input.kb.l1", T::KeyBinding, "Q", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / l1", "Host PC setting."},
            {"input.kb.r1", T::KeyBinding, "E", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / r1", "Host PC setting."},
            {"input.kb.l2", T::KeyBinding, "LEFT_SHIFT, ONE", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / l2", "Host PC setting."},
            {"input.kb.r2", T::KeyBinding, "RIGHT_SHIFT, THREE", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / r2", "Host PC setting."},
            {"input.kb.start", T::KeyBinding, "ENTER", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / start", "Host PC setting."},
            {"input.kb.select", T::KeyBinding, "TAB, BACKSPACE", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / select", "Host PC setting."},
            {"input.kb.l3", T::KeyBinding, "LEFT_CONTROL", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / l3", "Host PC setting."},
            {"input.kb.r3", T::KeyBinding, "RIGHT_CONTROL", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / r3", "Host PC setting."},
            {"input.kb.left_stick", T::KeyBinding, "W, A, S, D", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / left stick", "Host PC setting."},
            {"input.kb.right_stick", T::KeyBinding, "I, J, K, L", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "kb / right stick", "Host PC setting."},
            {"input.pad.cross", T::KeyBinding, "RIGHT_FACE_DOWN", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / cross", "Host PC setting."},
            {"input.pad.circle", T::KeyBinding, "RIGHT_FACE_RIGHT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / circle", "Host PC setting."},
            {"input.pad.square", T::KeyBinding, "RIGHT_FACE_LEFT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / square", "Host PC setting."},
            {"input.pad.triangle", T::KeyBinding, "RIGHT_FACE_UP", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / triangle", "Host PC setting."},
            {"input.pad.l1", T::KeyBinding, "LEFT_TRIGGER_1", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / l1", "Host PC setting."},
            {"input.pad.r1", T::KeyBinding, "RIGHT_TRIGGER_1", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / r1", "Host PC setting."},
            {"input.pad.l2", T::KeyBinding, "LEFT_TRIGGER_2", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / l2", "Host PC setting."},
            {"input.pad.r2", T::KeyBinding, "RIGHT_TRIGGER_2", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / r2", "Host PC setting."},
            {"input.pad.start", T::KeyBinding, "MIDDLE_RIGHT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / start", "Host PC setting."},
            {"input.pad.select", T::KeyBinding, "MIDDLE_LEFT", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / select", "Host PC setting."},
            {"input.pad.l3", T::KeyBinding, "LEFT_THUMB", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / l3", "Host PC setting."},
            {"input.pad.r3", T::KeyBinding, "RIGHT_THUMB", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Controls, nullptr, "pad / r3", "Host PC setting."},
            {"ui.pause_game", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "pause game", "Host PC setting."},
            {"ui.preset", T::Enum, "custom", 0, 0, 0, pc1enum27, nullptr, nullptr, nullptr, A::Live, P::Display, nullptr, "preset", "Host PC setting."},
            {"pcplus.detail_max", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "detail-probe", "detail max", "Host PC setting.", Scope::Guest},
            {"pcplus.fov_extra", T::Int, "0", 0, 15, 1, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "fov-extra", "fov extra", "Host PC setting.", Scope::Guest},
            {"pcplus.skip_logos", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "skip-logos", "skip logos", "Host PC setting.", Scope::Guest},
            {"pcplus.fast_warp_load", T::Bool, "off", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "native", "fast warp load", "Needs native renderer.", Scope::Guest},
            {"pcplus.ps2_softening", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "native", "ps2 softening", "Needs native renderer."},
            {"pcplus.field_blur", T::Bool, "on", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "native", "field blur", "Needs native renderer."},
            {"pcplus.draw_distance", T::Enum, "original", 0, 0, 0, pc1enum28, nullptr, nullptr, nullptr, A::Restart, P::Debug, "draw-distance", "draw distance", "Host PC setting.", Scope::Guest},
            {"pcplus.texture_pack", T::String, "", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "native", "texture pack", "Needs native renderer."},
            {"debug.vu1_aot", T::Bool, "off", 0, 0, 0, nullptr, nullptr, "PS2X_VU1_AOT", nullptr, A::Restart, P::Debug, nullptr, "vu1 aot", "Host PC setting."},
            {"debug.vu1_fast", T::Bool, "on", 0, 0, 0, nullptr, nullptr, "PS2X_VU1_FAST", nullptr, A::Restart, P::Debug, nullptr, "vu1 fast", "Host PC setting."},
            {"debug.gs_thread", T::Bool, "off", 0, 0, 0, nullptr, nullptr, "PS2X_GS_THREAD", nullptr, A::Restart, P::Debug, nullptr, "gs thread", "Host PC setting."},
            {"debug.storeimage_lag", T::Bool, "off", 0, 0, 0, nullptr, nullptr, "PS2X_STOREIMAGE_LAG", nullptr, A::Restart, P::Debug, nullptr, "storeimage lag", "Host PC setting."},
            {"debug.gif_fifo", T::Bool, "on", 0, 0, 0, nullptr, nullptr, "PS2X_GIF_FIFO", nullptr, A::Restart, P::Debug, nullptr, "gif fifo", "Host PC setting."},
            {"debug.iop_rpc_budget", T::Int, "50000000", 0, 4000000000, 1000000, nullptr, nullptr, "PS2X_IOP_RPC_BUDGET", nullptr, A::Restart, P::Debug, nullptr, "iop rpc budget", "Host PC setting.", Scope::Guest},
            {"debug.mpeg_ahead", T::Int, "0", 0, 1000, 10, nullptr, nullptr, "PS2X_MPEG_AHEAD", nullptr, A::Restart, P::Debug, nullptr, "mpeg ahead", "Host PC setting.", Scope::Guest},
            {"debug.pgs_warmup_ms", T::Int, "20000", 0, 600000, 1000, nullptr, nullptr, "PS2X_PGS_WARMUP_MS", nullptr, A::Restart, P::Debug, nullptr, "pgs warmup ms", "Host PC setting."},
            {"debug.profile", T::String, "", 0, 0, 0, nullptr, nullptr, "PS2X_PROFILE", nullptr, A::Restart, P::Debug, nullptr, "profile", "Host PC setting."},
            {"debug.debugger_key", T::String, "F1", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "debug-key", "debugger key", "Host PC setting."},
            {"debug.paired_capture_key", T::String, "F11", 0, 0, 0, nullptr, nullptr, nullptr, nullptr, A::Restart, P::Debug, "native", "paired capture key", "Needs native renderer."},
            {"display.culling",T::Enum,"original",0,0,0,pc1cull,pc1cullLabels,"PS2X_WIDESCREEN_CULL",pc1cullEnv,A::Restart,P::Aspect,"ws2-culling","Show more at screen edges (Experimental)","Draws scenery the original game hides at the sides in widescreen. Experimental: may change game timing slightly. Off = original.",Scope::Guest},
            {"framerate.output_rate",T::Enum,"60",0,0,0,pc1output,nullptr,"PS2X_PRESENT_RATE",nullptr,A::Restart,P::FrameRate,nullptr,"Field presentation","60 shows both fields; 30 is the power-saving output. Game timing stays unchanged."},
            {"debug.vu1_thread",T::Int,"0",0,3,1,nullptr,nullptr,"PS2X_VU1_THREAD",nullptr,A::Restart,P::Debug,nullptr,"VU1 worker","0 is inline; 3 is the shipping chain worker."},
            {"debug.vu1_aot_static",T::Bool,"off",0,0,0,nullptr,nullptr,"PS2X_VU1_AOT_STATIC",nullptr,A::Restart,P::Debug,"vu1aot","Static VU1 fragments","Keep whole-image AOT priority and use qualified static fragments."},
            {"debug.kernel_heap",T::Bool,"on",0,0,0,nullptr,nullptr,"PS2X_KERNEL_HEAP",nullptr,A::Restart,P::Debug,nullptr,"EE kernel heap","Guest-visible allocation layout; pin this for deterministic comparisons.",Scope::Guest},
            {"debug.power_optout",T::Bool,"off",0,0,0,nullptr,nullptr,"PS2X_POWER_OPTOUT",nullptr,A::Restart,P::Debug,nullptr,"Windows power opt-out","Disable background EcoQoS. Use the same value in both timing variants."},
        }; return s_defs;
    }

    // ---- small helpers ----------------------------------------------------------------------------------------
    std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string trim(const std::string &s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return s.substr(b, e - b);
    }

    bool parseBool(const std::string &text, bool &out)
    {
        const std::string t = lower(trim(text));
        if (t == "1" || t == "on" || t == "true" || t == "yes")
        {
            out = true;
            return true;
        }
        if (t == "0" || t == "off" || t == "false" || t == "no")
        {
            out = false;
            return true;
        }
        return false;
    }

    std::string formatFloat(double v) {
        char buf[64]; std::snprintf(buf,sizeof(buf),"%.9g",v);
        std::string text(buf); if(text.find_first_of(".eE")==std::string::npos) text+=".0";
        return text;
    }

    int enumIndexOf(const Def &d, const std::string &value)
    {
        if (!d.enumValues)
            return -1;
        const std::string v = lower(trim(value));
        for (int i = 0; d.enumValues[i]; ++i)
            if (v == d.enumValues[i])
                return i;
        return -1;
    }

    int enumCount(const Def &d)
    {
        int n = 0;
        while (d.enumValues && d.enumValues[n])
            ++n;
        return n;
    }

    // Canonicalise a value for a def; false when it is not valid for the type.
    bool canonical(const Def &d, const std::string &in, std::string &out)
    {
        switch (d.type)
        {
        case Type::Bool:
        {
            bool b = false;
            if (!parseBool(in, b))
                return false;
            out = b ? "on" : "off";
            return true;
        }
        case Type::Int:
        {
            const std::string t = trim(in);
            char *end = nullptr;
            const double v = std::strcmp(d.key,"input.pad.index")==0 && lower(t)=="auto" ? (end=const_cast<char*>(t.c_str()+t.size()),-1.0) : std::strtod(t.c_str(), &end);
            if (t.empty() || !end || *end != '\0' || !std::isfinite(v))
                return false;
            double c = std::round(v);
            if (d.maxValue > d.minValue)
                c = std::clamp(c, d.minValue, d.maxValue);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f", c);
            out = buf;
            return true;
        }
        case Type::Float:
        {
            const std::string t = trim(in);
            if (((std::strcmp(d.key,"display.ui_scale")==0 || std::strcmp(d.key,"input.pad.anti_deadzone_left_vehicle")==0) && lower(t)=="auto") || (std::strcmp(d.key,"framerate.fps_cap")==0 && (lower(t)=="auto" || lower(t)=="off"))) { out=lower(t); return true; }
            char *end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            if (t.empty() || !end || *end != '\0' || !std::isfinite(v))
                return false;
            double c = v;
            if (d.maxValue > d.minValue)
                c = std::clamp(c, d.minValue, d.maxValue);
            out = formatFloat(c);
            return true;
        }
        case Type::Enum:
        {
            const int i = enumIndexOf(d, in);
            if (i < 0) {
                if ((std::strcmp(d.key,"display.fullscreen_resolution")==0 || std::strcmp(d.key,"display.output_resolution")==0)) {
                    int w=0,h=0,hz=0,n=0;
                    if(std::sscanf(in.c_str(),"%dx%d@%d%n",&w,&h,&hz,&n)!=3 || n!=int(in.size()) || w<320 || w>7680 || h<240 || h>4320 || hz<24 || hz>1000) return false;
                    out=in; return true;
                }
                if (std::strcmp(d.key,"display.aspect")!=0) return false;
                char* end=nullptr; const double v=std::strtod(in.c_str(),&end);
                if (!end || *end || !std::isfinite(v) || v<4.0/3.0 || v>32.0/9.0) return false;
                out=formatFloat(v); return true;
            }
            out = d.enumValues[i];
            return true;
        }
        case Type::KeyBinding:
        {
            std::string value=trim(in); if(value.empty()) return false;
            if(value.size()>128 || value.find_first_of(";#=\r\n")!=std::string::npos) return false;
            std::istringstream tokens(value); std::string token; int count=0;
            while(std::getline(tokens,token,',')) {
                token=trim(token); if(token.empty()) return false;
                const bool pad=std::strncmp(d.key,"input.pad.",10)==0;
                const int code=pad ? ps2_padmap::padSourceFromName(token) : ps2x::input::keyFromName(token);
                if(lower(token)!="none" && code<0) return false;
                ++count;
            }
            const bool stick=std::strstr(d.key,"_stick")!=nullptr;
            if(stick && lower(value)=="none") { out="none, none, none, none"; return true; }
            if(stick ? count!=4 : count<1 || count>2) return false;
            out=value; return true;
        }
        case Type::String:
            out = trim(in);
            return true;
        }
        return false;
    }

    // Registry value from the env text of its alias (display only: the env text itself reaches the runtime untouched).
    std::string valueFromEnv(const Def &d, const std::string &env)
    {
        if (d.type == Type::Enum)
        {
            const std::string e = lower(trim(env));
            if (d.enumEnv)
                for (int i = 0; d.enumValues[i]; ++i)
                    if (d.enumEnv[i] && e == lower(d.enumEnv[i]))
                        return d.enumValues[i];
            const int i = enumIndexOf(d, env);
            if (i >= 0)
                return d.enumValues[i];
            return trim(env); // custom (e.g. PS2X_WIDESCREEN=2.3333): shown raw
        }
        std::string c;
        if (canonical(d, env, c))
            return c;
        return trim(env);
    }

    // Env text for a registry value; nullptr-equivalent (false) = the env var must be unset.
    bool envFromValue(const Def &d, const std::string &value, std::string &out)
    {
        switch (d.type)
        {
        case Type::Bool:
        {
            bool b = false;
            parseBool(value, b);
            out = b ? "1" : "0";
            return true;
        }
        case Type::Enum:
        {
            const int i = enumIndexOf(d, value);
            if (i < 0)
            {
                out = value;
                return !value.empty();
            }
            const char *e = d.enumEnv ? d.enumEnv[i] : d.enumValues[i];
            if (!e)
                return false;
            if (std::strcmp(d.key, "display.aspect") == 0 && std::strcmp(e, "auto") == 0)
            {
                // auto = the desktop aspect, clamped to [4:3, 32:9] (design 4.2; the ws code wants a number).
                double aspect = 16.0 / 9.0;
#if defined(_WIN32)
                const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
                if (w > 0 && h > 0)
                    aspect = static_cast<double>(w) / static_cast<double>(h);
#endif
                aspect = std::clamp(aspect, 4.0 / 3.0, 32.0 / 9.0);
                if (aspect < 1.41)
                    return false; // 4:3 desktop: leave the game at 4:3
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.4f", aspect);
                out = buf;
                return true;
            }
            out = e;
            return true;
        }
        case Type::Int:
        case Type::Float:
        case Type::KeyBinding:
        case Type::String:
            out = value;
            return !value.empty();
        }
        return false;
    }

    void setEnv(const char *name, const std::string *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value->c_str() : ""); // "" removes the variable
#else
        if (value)
            setenv(name, value->c_str(), 1);
        else
            unsetenv(name);
#endif
    }

    // ---- INI document: lines kept verbatim; only value substrings are replaced on save ------------------------
    struct IniLine
    {
        std::string text;
        int kind = 0; // 0 = blank/comment/other, 1 = [section], 2 = key = value
        std::string section; // lower-case current section
        std::string key;     // lower-case key
        size_t valueBegin = 0, valueEnd = 0, commentBegin = std::string::npos;
    };

    struct IniDoc
    {
        std::vector<IniLine> lines;

        void parse(const std::string &content)
        {
            lines.clear();
            std::string current;
            size_t pos = 0;
            std::string body = content;
            if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
                static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
                body.erase(0, 3);
            while (pos <= body.size())
            {
                size_t nl = body.find('\n', pos);
                if (nl == std::string::npos)
                    nl = body.size();
                std::string text = body.substr(pos, nl - pos);
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                const bool last = nl >= body.size();
                if (!(last && text.empty()))
                    lines.push_back(parseLine(text, current));
                pos = nl + 1;
                if (last)
                    break;
            }
        }

        static IniLine parseLine(const std::string &text, std::string &current)
        {
            IniLine line;
            line.text = text;
            const std::string t = trim(text);
            if (t.empty() || t[0] == ';' || t[0] == '#')
            {
                line.section = current;
                return line;
            }
            if (t[0] == '[')
            {
                const size_t close = t.find(']');
                if (close != std::string::npos)
                {
                    current = lower(trim(t.substr(1, close - 1)));
                    line.kind = 1;
                    line.section = current;
                    return line;
                }
            }
            const size_t eq = text.find('=');
            if (eq == std::string::npos)
            {
                line.section = current;
                return line;
            }
            line.kind = 2;
            line.section = current;
            line.key = lower(trim(text.substr(0, eq)));
            size_t vb = eq + 1;
            while (vb < text.size() && (text[vb] == ' ' || text[vb] == '\t'))
                ++vb;
            // Inline comment: ';' or '#' at the value start or after whitespace.
            size_t cb = std::string::npos;
            for (size_t i = vb; i < text.size(); ++i)
            {
                if ((text[i] == ';' || text[i] == '#') && (i == vb || text[i - 1] == ' ' || text[i - 1] == '\t'))
                {
                    cb = i;
                    break;
                }
            }
            size_t ve = (cb == std::string::npos) ? text.size() : cb;
            while (ve > vb && (text[ve - 1] == ' ' || text[ve - 1] == '\t'))
                --ve;
            line.valueBegin = vb;
            line.valueEnd = ve;
            line.commentBegin = cb;
            return line;
        }

        std::string value(const IniLine &l) const { return l.text.substr(l.valueBegin, l.valueEnd - l.valueBegin); }

        void setValue(const std::string &section, const std::string &key, const std::string &sectionTitle,
                      const std::string &newValue)
        {
            const std::string &value = newValue;
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            {
                IniLine &l = *it;
                if (l.kind == 2 && l.section == section && l.key == key)
                {
                    if (this->value(l) == value)
                        return;
                    std::string rebuilt = l.text.substr(0, l.valueBegin) + value;
                    if (l.commentBegin != std::string::npos)
                    {
                        const size_t oldSpan = l.commentBegin - l.valueBegin; // value + padding
                        const size_t pad = oldSpan > value.size() + 1 ? oldSpan - value.size() : 1;
                        rebuilt += std::string(pad, ' ') + l.text.substr(l.commentBegin);
                    }
                    std::string cur = section;
                    l = parseLine(rebuilt, cur);
                    return;
                }
            }
            // New key: after the last line of its section, or a new section at the end.
            int lastInSection = -1;
            bool sectionFound = false;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i].section == section && (lines[i].kind == 1 || lines[i].kind == 2))
                {
                    sectionFound = true;
                    lastInSection = static_cast<int>(i);
                }
            }
            std::string cur = section;
            IniLine nl = parseLine(key + " = " + value, cur);
            // keep the original key spelling of the registry (dotted keys like kb.cross)
            if (sectionFound)
            {
                lines.insert(lines.begin() + lastInSection + 1, nl);
            }
            else
            {
                if (!lines.empty() && !trim(lines.back().text).empty())
                    lines.push_back(IniLine{});
                IniLine header;
                header.text = "[" + sectionTitle + "]";
                header.kind = 1;
                header.section = section;
                lines.push_back(header);
                lines.push_back(nl);
            }
        }

        std::string serialize() const
        {
            std::string out;
            for (const IniLine &l : lines)
            {
                out += l.text;
                out += "\r\n";
            }
            return out;
        }
    };

    std::string sectionTitleOf(const std::string &prefix)
    {
        static const std::map<std::string, std::string> titles = {
            {"display", "Display"}, {"renderer", "Renderer"}, {"framerate", "FrameRate"}, {"audio", "Audio"},
            {"input", "Input"},     {"ui", "UI"},             {"debug", "Debug"},         {"game", "Game"},
            {"pcplus", "PCPlus"},   {"meta", "Meta"}};
        const auto it = titles.find(prefix);
        if (it != titles.end())
            return it->second;
        std::string t = prefix;
        if (!t.empty())
            t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
        return t;
    }

    void splitKey(const char *key, std::string &section, std::string &name)
    {
        const char *dot = std::strchr(key, '.');
        if (!dot)
        {
            section.clear();
            name = lower(key);
            return;
        }
        section = lower(std::string(key, dot));
        name = lower(std::string(dot + 1));
    }

    // ---- the store --------------------------------------------------------------------------------------------
    struct Store
    {
        std::mutex mx;
        bool loaded = false;
        bool harness = false;
        bool canWrite = false;
        std::filesystem::path path;
        std::string status = "not loaded";
        IniDoc doc;
        std::vector<Entry> entries;                   // parallel to registry()
        std::vector<Source> startSource;
        std::vector<std::string> startValue;          // effective value at start-up (restart-pending detection)
        std::map<std::string, std::string> fileValues; // key -> value meant for the file (loaded + menu edits)
        std::map<std::string, std::string> savedValues; // key -> value as last loaded / saved
        std::set<std::string> features;
        std::vector<std::pair<std::string, std::string>> envAtStart;
        std::atomic<uint64_t> version{1};
    };

    Store &S()
    {
        static Store s;
        return s;
    }

    int indexOf(const char *key)
    {
        if (!key)
            return -1;
        const auto &d = registry();
        for (size_t i = 0; i < d.size(); ++i)
            if (std::strcmp(d[i].key, key) == 0)
                return static_cast<int>(i);
        return -1;
    }

    void ensureEntries(Store &s)
    {
        if (!s.entries.empty())
            return;
        const auto &d = registry();
        s.entries.resize(d.size());
        s.startValue.resize(d.size());
        s.startSource.resize(d.size(),Source::Default);
        for (size_t i = 0; i < d.size(); ++i)
        {
            s.entries[i].def = &d[i];
            s.entries[i].value = d[i].defaultValue;
            s.startValue[i] = d[i].defaultValue;
        }
    }

    bool readFile(const std::filesystem::path &p, std::string &out)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    std::filesystem::path appDataPath()
    {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
            return std::filesystem::path(buf) / L"TransformersRecomp" / L"settings.ini";
#else
        if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x)
            return std::filesystem::path(x) / "transformers-recomp" / "settings.ini";
        if (const char *h = std::getenv("HOME"); h && *h)
            return std::filesystem::path(h) / ".config" / "transformers-recomp" / "settings.ini";
#endif
        return {};
    }

    bool writeAtomic(const std::filesystem::path &p, const std::string &content, std::string &err)
    {
        std::error_code ec;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        std::filesystem::path tmp = p;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                err = "cannot write " + tmp.string();
                return false;
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out)
            {
                err = "write failed: " + tmp.string();
                return false;
            }
        }
        if (std::filesystem::exists(p, ec))
        {
            std::filesystem::path bak = p;
            bak += ".bak";
            std::filesystem::copy_file(p, bak, std::filesystem::copy_options::overwrite_existing, ec);
        }
#if defined(_WIN32)
        if (!MoveFileExW(tmp.wstring().c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            err = "MoveFileEx failed (" + std::to_string(GetLastError()) + ") for " + p.string();
            return false;
        }
#else
        std::filesystem::rename(tmp, p, ec);
        if (ec)
        {
            err = "rename failed: " + ec.message();
            return false;
        }
#endif
        return true;
    }

    void captureEnvironment(Store &s)
    {
#if defined(_WIN32)
        char **env = _environ;
#else
        char **env = environ;
#endif
        for (char **e = env; e && *e; ++e)
        {
            const char *kv = *e;
            if (std::strncmp(kv, "PS2X_", 5) != 0)
                continue;
            const char *eq = std::strchr(kv, '=');
            if (!eq)
                continue;
            s.envAtStart.emplace_back(std::string(kv, eq), std::string(eq + 1));
        }
        std::sort(s.envAtStart.begin(), s.envAtStart.end());
    }

    const char *envAtStart(Store &s, const char *name)
    {
        for (const auto &kv : s.envAtStart)
            if (kv.first == name)
                return kv.second.c_str();
        return nullptr;
    }

    bool isAvailableLocked(Store &s, const Def &d)
    {
        return !d.feature || s.features.count(d.feature) != 0;
    }

    void bump(Store &s) { s.version.fetch_add(1, std::memory_order_acq_rel); }

    // Apply a new value for index i coming from the menu; caller holds the lock.
    bool setLocked(Store &s, int i, const std::string &value)
    {
        if (i < 0)
            return false;
        Entry &e = s.entries[static_cast<size_t>(i)];
        if (e.source == Source::Env || e.source == Source::Cli)
            return false;
        std::string c;
        if (!canonical(*e.def, value, c))
            return false;
        if (c == e.value)
            return false;
        e.value = c;
        e.source = Source::Ini;
        s.fileValues[e.def->key] = c;
        e.restartPending = e.def->apply == Apply::Restart && c != s.startValue[static_cast<size_t>(i)];
        bump(s);
        return true;
    }
} // namespace

// ================================================================================================================
void load(const std::filesystem::path &elfDirectory, int argc, char **argv)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    if (s.loaded)
        return;
    s.loaded = true;
    ensureEntries(s);
    captureEnvironment(s);
    // Built-in feature gates known at compile time (ps2_runtime is compiled with these defines).
#if defined(PS2X_WITH_PGS)
    s.features.insert("pgs");
#endif
#if defined(PS2X_WITH_VU1_AOT)
    s.features.insert("vu1aot");
#endif

    const char *script = std::getenv("PS2X_SCRIPT");
    const char *explicitFile = std::getenv("PS2X_SETTINGS");
    s.harness = script && *script;
    bool readIni = true;
    if (explicitFile && *explicitFile)
    {
        if (lower(explicitFile) == "none")
        {
            readIni = false;
            s.status = "none (PS2X_SETTINGS=none: built-in defaults)";
        }
        else
        {
#if defined(_WIN32)
            const wchar_t* wideSettings=_wgetenv(L"PS2X_SETTINGS");
            s.path = std::filesystem::path(wideSettings ? wideSettings : L"");
#else
            s.path = std::filesystem::u8path(explicitFile);
#endif
        }
    }
    else if (s.harness)
    {
        readIni = false;
        s.status = "ignored: test run (PS2X_SCRIPT set, PS2X_SETTINGS unset)";
    }
    else
    {
        const std::filesystem::path beside = elfDirectory.empty() ? std::filesystem::path("settings.ini")
                                                                  : elfDirectory / "settings.ini";
        std::error_code ec;
        const std::filesystem::path fallback = appDataPath();
        if (!std::filesystem::exists(beside, ec) && !fallback.empty() && std::filesystem::exists(fallback, ec))
            s.path = fallback;
        else
            s.path = beside;
    }
    s.canWrite = readIni && !s.harness && !s.path.empty();

    // 1) settings.ini
    if (readIni && !s.path.empty())
    {
        std::string content;
        if (readFile(s.path, content))
        {
            s.doc.parse(content);
            s.status = "loaded";
            for (const IniLine &l : s.doc.lines)
            {
                if (l.kind != 2)
                    continue;
                const std::string full = l.section + "." + l.key;
                const int i = indexOf(full.c_str());
                if (i < 0)
                    continue; // unknown keys stay in the document untouched
                Entry &e = s.entries[static_cast<size_t>(i)];
                std::string c;
                const std::string raw = s.doc.value(l);
                if (!canonical(*e.def, raw, c))
                {
                    std::cout << "[settings] ignoring invalid value " << full << "=" << raw << std::endl;
                    continue;
                }
                e.value = c;
                e.source = Source::Ini;
                s.fileValues[e.def->key] = c;
            }
        }
        else
        {
            s.status = "missing (built-in defaults; created on Save)";
        }
    }
    s.savedValues = s.fileValues;

    // 2) environment (wins over the file)
    for (Entry &e : s.entries)
    {
        if (!e.def->envAlias)
            continue;
        const char *v = envAtStart(s, e.def->envAlias);
        if (!v)
            continue;
        e.rawEnv = v;
        e.value = valueFromEnv(*e.def, v);
        e.source = Source::Env;
    }

    // 3) command line: --set key=value / --set=key=value (argv[1] is the guest ELF)
    for (int a = 2; argv && a < argc; ++a)
    {
        std::string kv;
        if (std::strcmp(argv[a], "--set") == 0 && a + 1 < argc)
            kv = argv[++a];
        else if (std::strncmp(argv[a], "--set=", 6) == 0)
            kv = argv[a] + 6;
        else
            continue;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = lower(trim(kv.substr(0, eq)));
        const int i = indexOf(key.c_str());
        if (i < 0)
        {
            std::cout << "[settings] --set: unknown key " << key << std::endl;
            continue;
        }
        Entry &e = s.entries[static_cast<size_t>(i)];
        std::string c;
        if (!canonical(*e.def, kv.substr(eq + 1), c))
        {
            std::cout << "[settings] --set: invalid value for " << key << std::endl;
            continue;
        }
        e.value = c;
        e.source = Source::Cli;
        e.rawEnv.clear();
    }

    // 4) export ini / cli values of restart-class aliases into the environment (never the built-in defaults)
    for (Entry &e : s.entries)
    {
        if (!e.def->envAlias || (e.source != Source::Ini && e.source != Source::Cli))
            continue;
        std::string text;
        if (envFromValue(*e.def, e.value, text))
            setEnv(e.def->envAlias, &text);
        else
            setEnv(e.def->envAlias, nullptr);
    }

    for (size_t i = 0; i < s.entries.size(); ++i) { s.startValue[i] = s.entries[i].value; s.startSource[i]=s.entries[i].source; }

    // 5) boot log: the file state + one line per non-default value (design 2.2)
    std::cout << "[settings] file=" << (s.path.empty() ? std::string("-") : s.path.string()) << " status=" << s.status
              << (s.harness ? " harness=1" : "") << std::endl;
    for (const Entry &e : s.entries)
    {
        if (e.source == Source::Default)
            continue;
        std::cout << "[settings] " << e.def->key << "=" << e.value << " (" << sourceName(e.source)
                  << (e.source == Source::Env ? std::string(" ") + e.def->envAlias : std::string()) << ")" << std::endl;
    }
    bump(s);
}

bool adoptLegacy(const char* key,const std::string& value) {
    Store& s=S(); std::lock_guard<std::mutex> lock(s.mx); ensureEntries(s);
    if(s.harness || !key || std::strncmp(key,"input.",6)!=0) return false;
    const int i=indexOf(key); if(i<0 || s.entries[i].source!=Source::Default) return false;
    std::string canonicalValue; if(!canonical(*s.entries[i].def,value,canonicalValue)) return false;
    s.entries[i].value=canonicalValue; s.entries[i].source=Source::Legacy; s.startValue[i]=canonicalValue; s.startSource[i]=Source::Legacy;
    bump(s); return true;
}

bool loaded()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.loaded;
}

const std::vector<Def> &defs() { return registry(); }

const Def *find(const char *key)
{
    const int i = indexOf(key);
    return i < 0 ? nullptr : &registry()[static_cast<size_t>(i)];
}

Entry entry(const char *key)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    ensureEntries(s);
    const int i = indexOf(key);
    return i < 0 ? Entry{} : s.entries[static_cast<size_t>(i)];
}

std::string get(const char *key)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    ensureEntries(s);
    const int i = indexOf(key);
    return i < 0 ? std::string() : s.entries[static_cast<size_t>(i)].value;
}

bool getBool(const char *key)
{
    bool b = false;
    parseBool(get(key), b);
    return b;
}

double getFloat(const char *key)
{
    const std::string v = get(key);
    return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

int getInt(const char *key) { return static_cast<int>(std::llround(getFloat(key))); }

int getEnumIndex(const char *key)
{
    const Def *d = find(key);
    return d ? enumIndexOf(*d, get(key)) : -1;
}

Source source(const char *key)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    ensureEntries(s);
    const int i = indexOf(key);
    return i < 0 ? Source::Default : s.entries[static_cast<size_t>(i)].source;
}

const char *sourceName(Source source)
{
    switch (source)
    {
    case Source::Default:
        return "default";
    case Source::Legacy:
        return "legacy controls.ini";
    case Source::Ini:
        return "settings.ini";
    case Source::Env:
        return "env";
    case Source::Cli:
        return "--set";
    }
    return "?";
}

bool locked(const char *key)
{
    const Source src = source(key);
    return src == Source::Env || src == Source::Cli;
}

bool available(const Def &def)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return isAvailableLocked(s, def);
}

uint64_t version() { return S().version.load(std::memory_order_acquire); }

// TEMP(transformers): internal display rollback can restore a locked startup request after a failed apply.
void restoreHostEntries(const std::vector<Entry>& entries) {
    Store& s=S();std::lock_guard<std::mutex> l(s.mx);ensureEntries(s);
    for(const auto& old:entries) {
        if(!old.def || old.def->scope!=Scope::Host)continue;
        const int i=indexOf(old.def->key);if(i<0)continue;
        s.entries[size_t(i)]=old;
        if(old.source==Source::Ini)s.fileValues[old.def->key]=old.value;
        else if(old.source==Source::Default || old.source==Source::Legacy)s.fileValues.erase(old.def->key);
    }
    bump(s);
}
bool set(const char *key, const std::string &value)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    ensureEntries(s);
    const int i = indexOf(key);
    if (i < 0 || !isAvailableLocked(s, registry()[static_cast<size_t>(i)]))
        return false;
    return setLocked(s, i, value);
}

bool setBool(const char *key, bool value) { return set(key, value ? "on" : "off"); }

bool setFloat(const char *key, double value) { return set(key, formatFloat(value)); }

bool setEnumIndex(const char *key, int index)
{
    const Def *d = find(key);
    if (!d || d->type != Type::Enum || index < 0 || index >= enumCount(*d))
        return false;
    return set(key, d->enumValues[index]);
}

bool cycleEnum(const char *key, int delta)
{
    const Def *d = find(key);
    if (!d)
        return false;
    if (d->type == Type::Bool)
        return setBool(key, !getBool(key));
    if (d->type != Type::Enum)
        return false;
    const int n = enumCount(*d);
    if (n <= 0)
        return false;
    int i = getEnumIndex(key);
    if (i < 0)
        i = 0;
    i = ((i + delta) % n + n) % n;
    return setEnumIndex(key, i);
}

bool stepNumber(const char *key, int steps)
{
    const Def *d = find(key);
    if (!d || (d->type != Type::Int && d->type != Type::Float))
        return false;
    double v = getFloat(key) + d->step * steps;
    if (d->maxValue > d->minValue)
        v = std::clamp(v, d->minValue, d->maxValue);
    if (d->type == Type::Float)
        v = std::round(v / d->step) * d->step; // keep the steps clean (0.05 grid)
    char buf[64];
    std::snprintf(buf, sizeof(buf), d->type == Type::Int ? "%.0f" : "%.6f", v);
    return set(key, buf);
}

void resetToDefault(const char *key)
{
    const Def *d = find(key);
    if (d)
        set(key, d->defaultValue);
}

void resetPage(Page page)
{
    for (const Def &d : registry())
        if (d.page == page)
            resetToDefault(d.key);
}

bool dirty()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.fileValues != s.savedValues;
}

bool anyRestartPending()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    for (const Entry &e : s.entries)
        if (e.restartPending)
            return true;
    return false;
}

void revert()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    ensureEntries(s);
    for (size_t i = 0; i < s.entries.size(); ++i)
    {
        Entry &e = s.entries[i];
        if (e.source == Source::Env || e.source == Source::Cli)
            continue;
        const auto it = s.savedValues.find(e.def->key);
        e.value = it != s.savedValues.end() ? it->second : s.startValue[i];
        e.source = it != s.savedValues.end() ? Source::Ini : s.startSource[i];
        e.restartPending = e.def->apply == Apply::Restart && e.value != s.startValue[i];
    }
    s.fileValues = s.savedValues;
    bump(s);
}

bool save(std::string *message)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    if (!s.canWrite)
    {
        if (message)
            *message = s.harness ? "not saved: test run (PS2X_SCRIPT)" : "not saved: no settings file (PS2X_SETTINGS=none)";
        return false;
    }
    if (s.doc.lines.empty())
    {
        IniDoc fresh;
        fresh.parse("; Transformers (SLUS_206.68) PC port settings - written by the PC menu (F10 / hold Select+Start).\r\n"
                    "; Precedence: built-in default < this file < PS2X_* environment < --set key=value.\r\n"
                    "; Values are text. Comments and unknown keys are kept when the game rewrites this file.\r\n"
                    "; PC display and input settings\r\n\r\n[Meta]\r\nversion = 1\r\n");
        s.doc = fresh;
    }
    for (const auto &kv : s.fileValues)
    {
        std::string section, name;
        splitKey(kv.first.c_str(), section, name);
        s.doc.setValue(section, name, sectionTitleOf(section), kv.second);
    }
    std::string err;
    const std::string content = s.doc.serialize();
    bool ok = writeAtomic(s.path, content, err);
    if (!ok)
    {
        const std::filesystem::path fallback = appDataPath();
        std::string err2;
        if (!fallback.empty() && fallback != s.path && writeAtomic(fallback, content, err2))
        {
            s.path = fallback;
            ok = true;
        }
    }
    if (ok)
    {
        s.savedValues = s.fileValues;
        s.status = "saved";
        if (message)
            *message = "saved to " + s.path.string();
        std::cout << "[settings] saved " << s.fileValues.size() << " value(s) to " << s.path.string() << std::endl;
    }
    else if (message)
    {
        *message = "save failed: " + err;
    }
    bump(s);
    return ok;
}

bool saveIfDirty()
{
    if (!dirty())
        return true;
    return save(nullptr);
}

void declareWired(const char *feature)
{
    if (!feature)
        return;
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    s.features.insert(feature);
    bump(s);
}

bool wired(const char *feature)
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return feature && s.features.count(feature) != 0;
}

std::filesystem::path filePath()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.path;
}

std::string fileStatus()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.status;
}

bool harnessMode()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.harness;
}

bool writable()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.canWrite;
}

std::vector<std::pair<std::string, std::string>> environmentPs2x()
{
    Store &s = S();
    std::lock_guard<std::mutex> lock(s.mx);
    return s.envAtStart;
}
std::string wineVersion()
{
#if defined(_WIN32)
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        using WineGetVersion = const char *(__cdecl *)();
        if (auto fn = reinterpret_cast<WineGetVersion>(GetProcAddress(ntdll, "wine_get_version")))
        {
            const char *v = fn();
            return v ? std::string(v) : std::string("unknown");
        }
    }
#endif
    return {};
}
} // namespace ps2x::settings
