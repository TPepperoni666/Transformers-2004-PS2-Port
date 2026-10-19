#ifndef PS2_PC_MENU_H
#define PS2_PC_MENU_H
// GAME(transformers) PC options menu, stage 1 (qualification; design private qualification evidence section 6).
// In-game Dear ImGui overlay sharing the F1 Runtime Debugger's ImGui context (one rlImGui host, see main.cpp).
// Open/close: F10, Esc, or hold Select+Start 0.5 s on a pad (input.menu_combo).
// Controller: D-pad/left stick move, A change, B back/close, X reset row, L1/R1 page. Keyboard: arrows, Enter,
// Esc/Backspace back, Delete reset, Q/E or PageUp/PageDown page. The game pauses while the menu is open (ui.pause_game).

#include <cstdint>
#include <string>

class PS2Runtime;

class PS2PcMenu
{
public:
    // Render thread, right after InitWindow (the runtime's debug-UI init callback; ImGui is already set up).
    void initialize(PS2Runtime &runtime);
    void shutdown();

    // Once per host frame before any ImGui frame: hotkeys, combo, live settings appliers, autotest.
    void update(PS2Runtime &runtime);
    bool wantsImGuiFrame() const;   // the menu (or its FPS overlay / hint) has something to draw this frame
    // Starts the shared ImGui frame with our own (corrected) gamepad mapping instead of rlImGuiBegin's.
    void beginImGuiFrame();
    void draw(PS2Runtime &runtime); // inside the ImGui frame
    void afterRender(PS2Runtime &runtime); // after rlImGuiEnd (window captures for the autotest)

    void serviceResolutionTest(PS2Runtime& runtime); // TEMP(transformers): host gate observer
    bool isOpen() const { return m_open; }
    void setOpen(bool open);

private:
    void applyLiveSettings(bool force);
    void applyWindowMode(int mode);
    void applyResolutionSettings(bool force);
    void revertResolutionSettings();
    void keepResolutionSettings();
    void drawMenuWindow(PS2Runtime &runtime);
    void drawPage(int page, PS2Runtime &runtime);
    void drawSettingRow(const void *def, bool debugPage);
    void drawDebugPage(PS2Runtime &runtime);
    void drawFpsOverlay(PS2Runtime &runtime);
    void drawHint();
    void requestClose();
    void autotestStep(PS2Runtime &runtime);
    float uiScale() const;

    bool m_initialized = false;
    bool m_open = false;
    bool m_justOpened = false;
    bool m_focusFirstRow = false;
    int m_page = 0;
    int m_requestedTab = -1;
    uint64_t m_appliedVersion = 0;
    int m_appliedWindowMode = -1;
    int m_fullscreenPreferredMode=1;
    int m_appliedMonitor = -1, m_revertMonitor = 0;
    double m_monitorDeadline = 0;
    double m_resolutionDeadline=0;
    int m_revertWindowMode=0;
    std::string m_appliedResolution, m_revertResolution="desktop";
    int m_lastWindowedW = 0, m_lastWindowedH = 0;
    float m_appliedVolume = -1.0f;
    bool m_confirmQuit = false;
    bool m_confirmClose = false;
    std::string m_status;
    double m_statusUntil = 0.0;
    std::string m_focusedKey;
    std::string m_focusedHelp;
    double m_hintUntil = 0.0;
    void *m_font = nullptr;
    // FPS overlay
    double m_fpsT0 = 0.0;
    uint64_t m_fpsVs0 = 0, m_fpsPads0=0;
    float m_gameFps=0;
    float m_guestVsPerSec = 0.0f;
    // TEMP(transformers): PS2X_PCMENU_AUTOTEST=<vsync> opens the menu, walks the pages, saves window PNGs, closes it.
    int64_t m_autotestVs = -1;
    int m_autotestStage = -1;
    double m_autotestT = 0.0;
    uint64_t m_autotestVsOpen = 0;
    std::string m_autotestDir;
    std::string m_pendingCapture;
};

#endif // PS2_PC_MENU_H
