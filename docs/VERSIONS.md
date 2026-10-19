# Versions

v0.8.3 adds presentation-buffer and pixel-processing improvements, queue wake and interrupt-drain synchronization fixes, shipping observer compile gates, and the optional on-machine optimisation step. Ordinary setup keeps optimisation off; see the README for the ~1.5 h planning allowance.

## Current source: v0.8.3

This source adds faster EE checkpoints and RAM stores, corrected widescreen boss-gauge widget tracking, default-off `PS2X_PRESENT_TRACE`, and single-write HUD diagnostics (`PS2X_WS_HUDLOG` / `PS2X_WS_HUDMAP`) to avoid diagnostic pipe stalls. Setup obtains and hash-checks the pinned SDK fingerprint database privately; its payload is excluded from this repository.


The public runtime now includes v0.8.3 settings/menu/resolution, audio/controller improvements, widescreen yield/HUD/culling fixes, VIF/VU0 fast paths, and optional EE optimizer support. Setup retains external verified generation and builds the optional static VU1 package from the user's disc. Historical measurements below describe their original configurations and do not promise the same performance from this source build. Exact fresh-disc routes, controlled hashes and save persistence are qualified separately; no whole-game or visual/audio result follows from them.


The source baseline is v0.7.3. Local performance builds also contain private packages generated from the owner's disc; those packages are excluded here.

| Version | Integration | Research Center camp, Legion Go 2 at 15 W |
|---|---|---|
| v0.7.0 | VU1 worker, GPU shader cache, expanded compiled coverage | 22.53 / 22.32 game fps |
| v0.7.1 | VU1 execution improvements, warp-entry/kernel-heap fixes, sprint and controller prompts | 23.43 / 23.47 |
| v0.7.2 | Widescreen option and presentation fixes | 23.35 / 23.57 |
| v0.7.3 | Antarctica coverage, compiled-footprint identity, 16:10 FOV/HUD, movie hang fix, anti-deadzone, output-rate/power options and detector | 27.13 / 27.50 |

These paired observations use the same seeded route. The save is private and omitted. They are small samples, not a general performance guarantee. Separate later experiments enabled static VU1 and memory classification; they are not part of this source baseline. Native rendering is also a separate experiment. See [performance](PERFORMANCE.md) for methods and limits.
