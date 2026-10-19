# Performance and power

Game fps measures script pad reads per wall second. Presented pictures and monitor refresh are different quantities: FIELD60 can keep presentation at 60 Hz while simulation is slower.  Local releases use private AOT packages; these figures do not describe an unaccelerated fresh-disc build. Benchmark on a quiet machine, retain Perfscope and presentation logs, compare identical settings/card/route, and report spread. Counters establish repeatability, not image or audio correctness.

## Historical release comparison: after-spawn gameplay

| Machine | Earlier release v0.6.0  | Worker release v0.7.0  | v0.7.1 |
|---|---|---|---|
| Main PC (9800X3D / RTX 4090)  | 23.08 fps  | 30.00 fps  | Not sourced |
| desktop (5800X3D / RX 9070 XT) (5800X3D / RX 9070 XT)  | 19.4 fps  | 29.4; quiet repeat 30.00 fps  | 29.90 fps, range 29.70–30.00  |
| Legion Go 2 (Z2 Extreme)  | 28.30 fps  | 30.00 fps  | See power-specific comparisons below |
| Laptop | No qualified measurement sourced | Not measured in release check: competing runs  | No qualified measurement sourced |

Do not assign a power limit to the historical Legion release comparison: that table does not establish one. The laptop's earlier v0.4.0 observation was 12.3 fps; it is not a missing later-release measurement. 

## Forest routes: keep separate

The old `l2x_forest.tfs` dies at f14; f15 onward measures the Stasis Lock menu.  Full-route averages are invalid as forest gameplay claims. Valid old-route gameplay is f00→f12. 

| Old route, valid gameplay only | Earlier release | Worker release |
|---|---|---|
| Main PC | 15.05 fps  | 29.65 fps  |
| Legion | 17.67 fps  | 29.99 fps  |

The survivable `l2x_forest_v2.tfs` ends with an objective-speech check that fails a dead run.  Its endpoint references are provisional; do not reuse old-route counters or pool its timings with the old route.

| Survivable forest | Released baseline | v0.7.1 |
|---|---|---|
| desktop (5800X3D / RX 9070 XT) game fps | 24.38 (23.82–24.76)  | 25.53 (25.49–25.58)  |
| desktop (5800X3D / RX 9070 XT) game-image 1% low  | 15.26 fps  | 16.98 fps  |
| desktop (5800X3D / RX 9070 XT) worker estimate | 39.23 ms/game frame  | 34.95 ms/game frame  |

desktop (5800X3D / RX 9070 XT) worker milliseconds are cycle-equivalent CPU estimates, not directly measured thread durations. Fresh measurements differ from historical release results; the cause is unestablished. Checkpoint screenshots can affect lows. 

## Legion power comparisons

| Setting / route | Released baseline | v0.7.1 | Meaning |
|---|---|---|---|
| 35 W, old heavy forest, no pinning  | 29.97–29.99 fps, about 31–34 W observed  | Not sourced | Historical affinity experiment |
| 25 W, Amazon real play  | Not sourced | 29.9 mean fps  | Different path from scripted forest |
| 15 W, survivable forest  | 28.94 / 28.78 fps  | 29.41 / 29.02 fps  | Paired scripted runs |
| Same forest, game-image 1% low  | 20.4 / 20.1 fps  | 21.9 / 18.7 fps; one 273 ms hitch  | Improvement is not uniform |
| Same forest, worker busy | 50–51%  | 42–43%  | Less geometry CPU work |
| Same forest, EE busy | 94–95%  | 94–96%  | Game thread limits fps |
| Same low-power cine gameplay | 30.00 fps; VU1 worker 41.0% in gameplay  | 30.00 fps; VU1 worker 35.4% in gameplay  | APU at the 15.0 W cap during gameplay for both (13.8 W whole-run average)  |

In v0.7.1 real play at about 15 W, clean Amazon gameplay averaged 28.65 fps and the VU-heavy areas about 23.3 fps.  Stone bridge saturates EE with no cap sleep or VU fence wait. Antarctica and the post-warp hub instead have uncovered VU programs: Antarctica worker busy 80–88%, with 1,100–1,700 slow-path calls/vsync; post-warp hub about 1,690 calls/vsync and 24 fps.  Compiling the tested Amazon paths does not establish coverage of the whole game.

## Warp loads

| Standard scripted warp | Earlier release | Worker release |
|---|---|---|
| Main PC | 17.0 s  | 12.5 s  |
| Legion | 14.3 s  | 11.0 s  |
| desktop (5800X3D / RX 9070 XT) | 18.8 s  | 12.4 s  |

Fresh desktop (5800X3D / RX 9070 XT) cine measurements give released baseline 18.77 s versus v0.7.1 12.60 s; survivable forest gives 18.77 s versus 12.57 s.  These are gate-to-load marker proxies including scripted entry waits, not isolated asset I/O. The vortex was predominantly geometry execution and packet handling; CD reads were already instant, so a fast-disc switch was not the useful fix. 

## Research Center and post-warp hub, 2026-10-12

Legion Go 2 at 15 W, desktop mode, capped gameplay, same route/settings/save. Values below are individual runs, not confidence intervals.

| Variant | Camp game fps | Samples |
|---|---|---|
| v0.7.0 | 22.53 / 22.32 | 2 |
| v0.7.1 | 23.43 / 23.47 | 2 |
| v0.7.2 | 23.35 / 23.57 | 2 |
| Coverage-only experiment on v0.7.1 | 27.53 / 26.90 | 2, with one interleaved v0.7.2 control at 23.40 |
| v0.7.3 | 27.13 / 27.50 | 2 |
| Reworked static VU1 with recorded package | 29.00 / 28.87 | 2 |
| Disc-only standalone static VU1, no recorded package | 27.0–27.1 | reported range; exact sample count unavailable |

The coverage-only experiment reduced mid-call interpreter fallbacks from about 1,065 to 6.5 per guest vsync and VU worker load from 100% to 86–89%. Its mean camp result was about 16% above the interleaved control. All reported route counters matched. The standalone static result must not be substituted for the recorded-package result.

| Post-warp route segment | v0.7.2, two runs | v0.7.3, two runs |
|---|---|---|
| Antarctica stretch | 27.05 / 27.05 fps | 29.98 / 30.00 |
| Warp vortex | 19.1 / 17.8 | 26.4 / 30.0 |
| Hub after warp | 19.7 / 20.0 | 29.95 / 30.00 |

A later forest observation was 27.3 game fps on v0.7.3; disc-only static measured about 27.0 against about 27.6 with the recorded package. These are separate observations with limited sampling. Forest remained game-thread limited.

## Desktop experiments and Windows power throttling

On the desktop test system, guarded scratchpad fastmem measured 28.16 ms EE/frame off versus 28.23 on, and forest 29.90 versus 28.62 fps across three interleaved pairs with matching Windows power opt-out. It was held. A separate memory-classification experiment reduced median forest EE time by 4.04%; it is outside this baseline. A broader version reduced EE time by 5.19% but worsened forest 1% lows to 11.4 fps versus 20.9, so it is not an unconditional improvement.

Hidden BelowNormal Windows runs can receive EcoQoS and lower CPU clocks. Removing throttling cut observed EE time by about 32% on three routes in an earlier one-to-two-run study; that is primarily a scheduling-state change. Compare both variants with the same `PS2X_POWER_OPTOUT` setting and record it. Older desktop tables above did not establish a matched opt-out state and must not be pooled with these later experiments. Foreground play can be faster than hidden runs. Linux/Proton handheld runs do not have this Windows background-throttling mechanism.

For new claims, collect at least three quiet repetitions per variant, total CPU load, per-segment timings and min/max spread. Preserve logs privately. Historical handheld observations above are transcribed from the dated Antarctica route study and overnight research summary; the original logs and saves are not distributed.
