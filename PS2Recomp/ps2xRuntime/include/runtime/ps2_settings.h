#pragma once
// GAME(transformers) PC options, stage 1 (qualification; design: private qualification evidence sections 2 and 6).
// One settings registry drives settings.ini, the PS2X_* environment aliases, the --set command line, the PC menu
// (src/lib/ui/ps2_pc_menu.cpp) and its Debug page.
//
// Precedence (design decision 2): built-in default < settings.ini < environment (PS2X_*) < command line --set key=value.
// File (design 2.2): PS2X_SETTINGS=<file> (or "none" = built-in defaults, nothing written), else <guest ELF dir>\settings.ini
// (beside the memory card), else %APPDATA%\TransformersRecomp\settings.ini when the game folder is read-only.
// Harness rule (design decision 3): when PS2X_SCRIPT is set and PS2X_SETTINGS is not, settings.ini is neither read nor
// written, so a tester's personal settings can never move the RESULT counters.
//
// Restart-class settings that have an env alias reach the runtime through that env var: load() exports every value
// that came from settings.ini or --set into the process environment before anything reads it, so the existing getenv()
// sites keep working unchanged (and an env var that was already set wins, exactly like before).
// Built-in defaults are never exported: every default equals today's no-env behaviour.
//
// Thread model: load() runs on the main thread before the runtime starts. Everything else is called from the render
// thread (menu); the store is mutex-protected, so other threads may read too.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2x::settings
{
    enum class Type : uint8_t
    {
        Bool,
        Int,
        Float,
        Enum,
        String,
        KeyBinding,
    };

    enum class Apply : uint8_t
    {
        NextFrame,
        Live,    // takes effect at once (menu appliers on the render thread)
        Restart, // stored; takes effect at the next start (env alias exported at load)
    };

    enum class Page : uint8_t
    {
        Display,
        Aspect,
        Presentation,
        FrameRate,
        Audio,
        Controls,
        Debug, // shown only on the Debug page (which lists every setting anyway)
        Count,
    };

    enum class Source : uint8_t
    {
        Default,
        Legacy, // controls.ini migration fallback, below primary settings.ini
        Ini,
        Env,
        Cli,
    };

    enum class Scope : uint8_t { Host, Guest };
    struct Def
    {
        const char *key;                // "display.window_mode" -> [Display] window_mode
        Type type;
        const char *defaultValue;       // text, never an index
        double minValue, maxValue, step; // Int / Float range and menu step
        const char *const *enumValues;  // Enum: stored names, nullptr-terminated
        const char *const *enumLabels;  // Enum: menu labels (parallel to enumValues; nullptr = use the values)
        const char *envAlias;           // "PS2X_AUDIO" or nullptr
        const char *const *enumEnv;     // Enum: env text per value (parallel; a nullptr entry = leave the env var unset)
        Apply apply;
        Page page;
        const char *feature;            // nullptr = always available; else available once declareWired(feature) ran
        const char *label;              // menu label
        const char *help;               // one line, menu footer
        Scope scope = Scope::Host;
    };

    struct Entry
    {
        const Def *def = nullptr;
        std::string value;    // effective value (canonical text)
        Source source = Source::Default;
        std::string rawEnv;   // the env text when source == Env (passed to the runtime untouched)
        bool restartPending = false; // changed in the menu, needs a restart to take effect
    };

    // ---- start-up ----------------------------------------------------------------------------------------------
    // Reads settings.ini (unless exempt), the environment and argv "--set key=value" / "--set=key=value" (any argv
    // index >= 2), exports env aliases, prints the [settings] boot log. Safe to call once; later calls are no-ops.
    void load(const std::filesystem::path &elfDirectory, int argc, char **argv);
    bool loaded();
    bool adoptLegacy(const char* key,const std::string& value); // seed an unset host input value, never dirty/save it

    // ---- queries -----------------------------------------------------------------------------------------------
    const std::vector<Def> &defs();
    const Def *find(const char *key);
    Entry entry(const char *key);           // copy; def == nullptr for unknown keys
    std::string get(const char *key);
    bool getBool(const char *key);
    double getFloat(const char *key);
    int getInt(const char *key);
    int getEnumIndex(const char *key);      // -1 when the value is not one of the enum names (custom / raw env)
    Source source(const char *key);
    const char *sourceName(Source source);
    bool locked(const char *key);           // set by env or --set: the menu shows it read-only
    bool available(const Def &def);         // feature wired (or no feature needed)
    uint64_t version();                     // bumps on every change (hot readers poll this)

    // ---- menu edits (render thread) ----------------------------------------------------------------------------
    void restoreHostEntries(const std::vector<Entry>& entries); // rollback preserves source/locks
    bool set(const char *key, const std::string &value); // false when locked / unknown / unchanged / invalid
    bool setBool(const char *key, bool value);
    bool setFloat(const char *key, double value);
    bool setEnumIndex(const char *key, int index);
    bool cycleEnum(const char *key, int delta);           // wraps
    bool stepNumber(const char *key, int steps);          // Int / Float, clamped
    void resetToDefault(const char *key);
    void resetPage(Page page);
    bool dirty();                           // unsaved menu changes
    bool anyRestartPending();
    void revert();                          // back to the last loaded / saved file values
    bool save(std::string *message = nullptr); // atomic write (tmp + MoveFileEx) + one .bak; comments and unknown keys kept
    bool saveIfDirty();                     // clean exit

    // ---- features ----------------------------------------------------------------------------------------------
    // A subsystem that consumes a gated setting calls this once (any thread), e.g. the widescreen code
    // declareWired("widescreen"), the presenter upscaler declareWired("upscale"), the 60 fps path declareWired("field60").
    void declareWired(const char *feature);
    bool wired(const char *feature);

    // ---- status (Debug page) -----------------------------------------------------------------------------------
    std::filesystem::path filePath();       // empty when none (PS2X_SETTINGS=none or harness)
    std::string fileStatus();               // "loaded", "missing (defaults)", "ignored: test run (PS2X_SCRIPT)", ...
    bool harnessMode();
    bool writable();                        // save() allowed at all
    std::vector<std::pair<std::string, std::string>> environmentPs2x(); // every PS2X_* variable set at start-up
    std::string wineVersion();              // non-empty under Wine / Proton (ntdll wine_get_version)
}
