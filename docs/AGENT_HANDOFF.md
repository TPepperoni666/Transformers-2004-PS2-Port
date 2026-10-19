# Handoff for contributors and their AI agents

Read [Setup](SETUP.md), [Testing](TESTING.md), [accepted counters](../testing/REFERENCES.md), [Options](OPTIONS.md) and [AOT setup](VU1_AOT.md) before changing runtime behavior. Build using your own disc in a private work directory outside this repository. The staged setup remains a qualification candidate; do not claim a fresh-disc end-to-end success merely because source compilation passes.

## Repository layout

| Path | Purpose |
|---|---|
| `PS2Recomp/` | Analyzer, static recompiler and runtime source; generated runner lives outside the distributed tree |
| `deps/` | Staged dependency sources |
| `setup/` | User-disc extraction, analysis and local build entry points |
| `launch/` | Play launchers and environment defaults |
| `testing/` | Scripted routes, references and comparison support |
| `tools/` | Distribution checker and supporting source tools |
| `docs/` | Setup, options, performance history and engineering findings |
| `NOTICE/`, `CREDITS.md`, `LICENSE` | Provenance and licensing |

## Golden rules

- Never commit discs, extracted game assets, ELF files, generated game code, compiled geometry packages, saves, captures or build products. Run `python tools/check_no_game_data.py .` and review provenance manually. Only the inventoried approved comparison images under `docs/images/` are included.
- Exactness means identical guest counters on the same scripted route, seed and settings, with matching timelines. Use the current references; historical cycle values changed. A PASS flag without counter comparison is insufficient.
- Make one change at a time, with a clear hypothesis, off-switch/baseline comparison and evidence. Preserve visual/audio checks as well as deterministic counters.
- Use Perfscope to separate EE guest work, waits/cap, geometry worker, GIF processing and presentation costs. A busy worker and a saturated EE thread call for different fixes. See [Performance](PERFORMANCE.md).
- Time on a quiet machine. Keep competing builds/runners away from benchmarks, retain raw timing and machine/environment details, and reject contaminated results. Report variation rather than the fastest sample.

## Routes and pull requests

Fresh-card `level_mvp.tfs` and `level_mvp_cine.tfs` cover boot/menu/arrival and cinematic gameplay. `level2_save.tfs` needs the private seed for the published reference. The old `l2x_forest.tfs` includes a death-menu tail and must not be timed as full gameplay. The survivable `l2x_forest_v2.tfs` uses an objective-speech survival check; its references remain provisional and are not currently a row in the staged accepted-reference table.  Do not distribute private player cards. With your own seed, establish new baseline counters and compare your change against those.

Contributors bring repeatable testing, or maintainers run their scripted routes before merging.  Include the change's purpose, source scope, exact configuration, counter/timeline results, qualified timing where relevant, picture/audio observations and unresolved gates. Never label an untested lane or a preview as shipped.

## Environment and agent workflow

[Options](OPTIONS.md) is the knob reference. Useful switches include `PS2X_SCRIPT`, `PS2X_MC_ROOT`, `PS2X_AUDIO`, `PS2X_HIDDEN_WINDOW`, `PS2X_REALTIME`, `PS2X_FIELD60`, `PS2X_VU1_THREAD`, `PS2X_VU1_AOT`, `PS2X_LOAD_SKIP`, `PS2X_QUIET_IO`, `PS2X_CONTROLS_INI` and `PS2X_GLYPHS`. Use documented settings and inspect startup logs for actual activation; do not assume a switch can enable a package absent from your build.

Our private workspace organized experiments into isolated lane copies, named task outputs, lane handoffs, integration reports and a change registry. Each lane recorded baseline, ownership, proposed patch, exactness gates, rejected timings and remaining work; the lead assembled accepted changes and recorded release status. Those private packages and reports are not all staged here. Recreate the workflow with source-only notes and private test artifacts outside the repository. Give your agent explicit write scope, machine/run ownership, current references and a reviewable deliverable. Keep coordination records separate from game data. 
