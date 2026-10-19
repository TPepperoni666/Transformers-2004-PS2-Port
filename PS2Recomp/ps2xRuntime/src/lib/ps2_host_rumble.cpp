// TEMP(transformers): playtest rumble backend. raylib 5.5's GLFW desktop backend has no gamepad vibration
// (SetGamepadVibration only logs "not available on target platform"), so on Windows the PS2 actuator commands go
// straight to XInput. The GLFW joystick index cannot be mapped to an XInput user index, so every connected XInput pad
// gets the same command (normally there is exactly one). DirectInput-only pads (e.g. a DualSense without Steam Input)
// do not rumble. Kept out of ps2_pad.cpp because <windows.h> clashes with raylib.h.
#include <algorithm>
#include <cstdint>

#if defined(_WIN32) && !defined(PLATFORM_VITA)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <dbt.h>
#include <cwchar>
#include <atomic>
#include <iostream>

namespace
{
    using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
    using XInputSetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_VIBRATION *);

    struct XInputApi
    {
        XInputGetStateFn getState = nullptr;
        XInputSetStateFn setState = nullptr;
        XInputApi()
        {
            HMODULE dll = LoadLibraryW(L"xinput1_4.dll");
            if (!dll)
                dll = LoadLibraryW(L"xinput9_1_0.dll");
            if (dll)
            {
                getState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(dll, "XInputGetState"));
                setState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(dll, "XInputSetState"));
            }
            std::cerr << "[pad] rumble: " << ((getState && setState) ? "XInput available" : "XInput not found - no rumble")
                      << std::endl;
        }
    };

    const XInputApi &api()
    {
        static const XInputApi a;
        return a;
    }
}

void ps2HostRumbleSet(float lowFrequency, float highFrequency)
{
    const XInputApi &x = api();
    if (!x.getState || !x.setState)
        return;
    XINPUT_VIBRATION vib{};
    vib.wLeftMotorSpeed = static_cast<WORD>(std::clamp(lowFrequency, 0.0f, 1.0f) * 65535.0f);
    vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(highFrequency, 0.0f, 1.0f) * 65535.0f);
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
    {
        XINPUT_STATE st{};
        if (x.getState(i, &st) == ERROR_SUCCESS)
            x.setState(i, &vib);
    }
}

#else
void ps2HostRumbleSet(float, float) {}
#endif

// TEMP(transformers): padhot1; bundled GLFW win32_init.c helperWindowProc handles this exact header.
// Enumerate ONLY our calling thread: no cross-process FindWindow and no cross-thread GLFW calls.
bool ps2HostPadRescan()
{
#if defined(_WIN32) && !defined(PLATFORM_VITA)
    HWND helper=nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM out)->BOOL {
        wchar_t name[64]{};
        GetClassNameW(window,name,64);
        if(std::wcscmp(name,L"GLFW3 Helper")==0) {
            *reinterpret_cast<HWND*>(out)=window;
            return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&helper));
    if(!helper) return false;
    DEV_BROADCAST_HDR header{};
    header.dbch_size=sizeof(header);
    header.dbch_devicetype=DBT_DEVTYP_DEVICEINTERFACE;
    SendMessageW(helper,WM_DEVICECHANGE,DBT_DEVICEARRIVAL,reinterpret_cast<LPARAM>(&header));
    return true;
#else
    return false;
#endif
}
