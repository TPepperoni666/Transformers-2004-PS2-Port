# Accepted route counters

These are the v0.7/v0.8 guest-unwind references supplied by the player, overriding historical v4 references.

| Route | vs | pads | cyc | cd | i5 | mscal |
|---|---:|---:|---:|---:|---:|---:|
| mvp/level_mvp.tfs | 5396 | 2021 | 26522953417 | 2070 | 2422 | 116720 |
| mvp/level_mvp_cine.tfs | 9604 | 4121 | 47206531601 | 3443 | 4526 | 981847 |
| mvp/level2_save.tfs | 5932 | 2370 | 29157553679 | 1759 | 2692 | 811124 |
| forest/l2x_forest.tfs | 22548 | 10678 | 110830161871 | 2675 | 11000 | 8211913 |

The last two routes require the exact private the player save seed (undistributed saved-game state), which is deliberately omitted. Creating your own save changes the expected state and requires new independently recorded references. Fresh-card routes need no distributed seed. Counters alone do not establish visual correctness.

Use PGS, STOREIMAGE_LAG=1, LOAD_SKIP=4, QUIET_IO=1, shipping play environment, audio off for quiet tests, and identical build/script/card. AOT off should preserve counters. Capture route timelines and wall timing privately; at least three runs per variant, record spread and total CPU load and reject busy runs. The standalone launch scripts are for user play; workspace main-PC gates continue to use the existing run_test.ps1 slot/quiet wrapper.

## Controlled VU1 hash gate (GH)

Compare a nonempty ordered sequence of (call, hash) pairs and the exact six terminal
route counters, with static VU1 OFF/ON. Record checkpoint vsync labels as diagnostics.
This is the definition used for private release qualification. Use HASH as the
only observer, VU1_THREAD=0, KERNEL_HEAP=0, GLYPHS=off, VU1NEW=0, audio off, identical
scripts with dumpsteps off and a fresh card for each side. Saved routes need the
matching private seed; do not substitute a fresh card.

Both dispatch paths use tryAot's hash wrapper and aotHashFold in
PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_aot_bridge.cpp (lines 166-167, 264-270).
The label reads ps2dbg::vsyncTick, published by UploadFrame on the host presentation
thread (ps2_runtime.cpp:503,624), not the EE VBlank event. The EE instead publishes
GS::vsyncTick (Kernel/EeScheduler.cpp:2620-2625). Presentation can lag by two fields
while guest state/cycles and terminal counters agree. The label is not a guest
timestamp. Public cine static-ON matched the private qualification's full 47
checkpoint sequence and terminal tuple; OFF matched that sequence too. Thus label
shifts are an observation-boundary effect. No state/counter tolerance is allowed,
and this is not whole-game timing qualification.

Use `python testing/check_gh.py OFF_RUN ON_RUN` to enforce this definition.
Static-ON smoke also requires accepted > 0 in aotStats and perfscope EE+VU
interpreter calls <= 64. Enable PS2X_PERFSCOPE=1, PS2X_AOTCOV4_METRICS=1,
PS2X_PERFSCOPE_SEC=1 and PS2X_PERFSCOPE_CSV=<private path>; missing metrics fail.

Run `python testing/check_static_coverage.py SMOKE_RUN COVERAGE_CSV` after static-ON smoke.

Coverage also requires `[perfscope:vu1] interp_calls=N` at shutdown, an exact EE+VU
total including the final partial reporting window. CSV deltas alone are insufficient.
