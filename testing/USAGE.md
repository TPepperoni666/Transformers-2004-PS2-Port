# Scripted routes

The runtime's `.tfs` scripts isolate guest pad input from the host. Scripts wait for guest events, emit checkpoint counters and end with a verdict. All generated logs, dumps, cards and images must stay outside this checkout.

From PowerShell, use explicit private paths:

```powershell
./testing/run_test.ps1 -Exe ../private-game/ps2EntryRunner.exe -Elf ../private-disc/SLUS_206.68 -Iso ../private-disc/game.iso -Script ./testing/mvp/level_mvp.tfs -Work ../private-runs/mvp-01 -Seconds 1200
```

`-McSeed` accepts your own card folder and copies it to a fresh card. No seeds are included. `-Env` accepts semicolon-separated `KEY=value` overrides. Output/card/script paths, hidden window and silent audio remain enforced. Set `PS2X_SCRIPT_PARSE_ONLY=1` to print the parsed program and exit before guest execution. Optional `-BusyFlag` paths delay launch while another workload is active; `-SlotFile` holds an exclusive shared admission file throughout the run. Participants must use the same slot path. The portable wrapper does not integrate with private workstation admission helpers.

| Exit | Meaning |
|---|---|
| 0 | Script PASS (confirm `[script] RESULT PASS` and counters) |
| 1 | Runtime error |
| 2 | Guest/host stall |
| 3 | Assertion or route failure |
| 4 | Script wall budget exceeded |
| 5 / exception code | Crash; inspect private crash report |
| 6 | Script parse error |
| 7 | Aborted without verdict |
| 124 | Wrapper timeout; only its own child is stopped |

Each work directory contains environment, route copy, stdout/stderr, script verdict and sampled total CPU load. Do not count a busy run as quiet timing. Preserve the crash reporter; a counter pass is not a visual/audio pass.

## Route selection

- `mvp/level_mvp.tfs`: fresh card, boot/menu/first arrival.
- `mvp/level_mvp_cine.tfs`: fresh card, intro cinematics and gameplay.
- `mvp/level2_save.tfs`: requires a save at the second Amazon warp gate.
- `forest/l2x_forest.tfs`: legacy forest route; the death-menu tail is not gameplay.
- `forest/l2x_forest_v2.tfs`: survivable forest route with objective-speech validation.
- `antarctica/ant_research_camp.tfs`: requires a save with the Research Center gate available.

Additional scouting and development scripts are exploratory; do not assume they carry accepted references. See [references](REFERENCES.md).

## Language essentials

One statement per line; `#` introduces a comment. Labels end in `:`. `budget` bounds wall time; `stall` and `hard` bound guest inactivity. `wait`, `press ... until`, `stick` and `mark` express event-driven routes. `assert`, `pass`, `fail` and watchdogs determine verdicts. Runtime `Kernel/TestScript.cpp` is the authoritative parser; parse-check a changed route before measuring it. Disc file paths within scripts refer to the user's ISO; they do not imply distributed assets.
