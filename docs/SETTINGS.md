# Choose settings for play

v0.8.3 includes a PC settings menu (F1) and typed `settings.ini`, environment aliases and `--set key=value`. Precedence is built-in default < settings.ini < environment < --set; environment overrides are locked in the menu. Scripted tests ignore settings.ini unless PS2X_SETTINGS explicitly names a file. Ordinary play prefers settings beside the ELF and may use a per-user fallback; set PS2X_SETTINGS to an explicit private file, or `none` for defaults. The profiles below describe presentation choices; only wired features are available.

| Profile | Current-tree choices | Settings keys |
|---|---|---|
| Original | Unset PS2X_WIDESCREEN; PS2X_UPSCALE=off; PS2X_FIELD60=0; PS2X_REALTIME=1 | display.aspect=game_option; display.scaling_filter=off; original 30 Hz frame mode |
| Modern | launch/play_env.txt; optional PS2X_WIDESCREEN=16:9; PS2X_UPSCALE=fsr1; PS2X_SHARPNESS=0.8; PS2X_FIELD60=1 | display.aspect, display.scaling_filter, display.sharpness and field presentation |
| Experimental | Explicitly select independently qualified optional VU packages; experimental modes require separate qualification | Disabled or pending registry rows remain unavailable until implemented |

All profiles retain native 30 Hz game simulation. FIELD60 presents fields; it does not provide 60 Hz simulation or general object interpolation. Widescreen can expose culling/pop-in and stretch 2D screens. Original describes presentation intent, not cycle-perfect hardware emulation. Modern does not enable native high-resolution rendering.

Window, monitor, audio device and controller bindings are machine settings. Difficulty, vibration and game-owned preferences remain in the game's own menus/card. Do not restore machine choices from a shared save or put experimental engine switches in guest save bytes. See [options](OPTIONS.md) for the current environment interface.
