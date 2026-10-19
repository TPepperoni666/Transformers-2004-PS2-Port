# Changelog

Point releases describe source integrations. Generated game code, recorded VU1 packages and saves are not distributed. TF identifiers in retained source comments are historical change-tracking numbers, not issue links or release versions.

The attribution references retained in CREDITS use these historical IDs:

| ID | Change |
|---|---|
| Adaptation | Correct VU EFU opcode slots |
| Adaptation | Correct graphics block addressing |
| Adaptation | Track guest branch unwinding |
| Adaptation | Run VU0 microcode through completion slices |

## v0.8.3

- Speed up EE checkpoints and use single wide RAM stores.
- Correct widescreen boss-gauge widget tracking.
- Build each optional HUD diagnostic line in memory and write it once, avoiding diagnostic pipe stalls when `PS2X_WS_HUDLOG` or `PS2X_WS_HUDMAP` is enabled. Normal play with these diagnostics unset is unchanged.
- Add default-off `PS2X_PRESENT_TRACE` presentation diagnostics.
- Fetch the pinned, hash-verified SDK fingerprint database during setup rather than redistribute it. All existing legal credits remain.
- Known issue: brief presentation stalls observed on one Linux handheld under Proton; tracing is available. The native renderer is not included.

## v0.7.3 — 2026-10-12

- Extend local VU1 coverage to Antarctica and use entry-specific compiled-footprint identity to avoid rejecting unchanged compiled code after unrelated uploads.
- Adjust 16:10 FOV and HUD anchors; preserve movie aspect and repair sign/dialog anchoring.
- Add a 30/60 presentation-rate setting, Windows power-throttling opt-out and optional core placement.
- Add physical-stick anti-deadzone defaults (left 0.62, right 0.20), retaining the 5% host deadzone and user overrides.
- Feed IPU input to the active movie object to fix a warp-deploy hang after menu changes.
- Include optional static VU1 dispatch and a new-code detector. Ship source generators, not their disc-derived outputs. Static dispatch remains experimental and disabled by default.
- Publish scripted routes and controlled hash-gate instructions. Four reference routes passed 40 cumulative counter checks in the accepted integration; these are correctness checks, not fresh-disc performance certification.

## v0.7.2 — 2026-10-11

Add widescreen menu integration and related presentation fixes on the v0.7.1 base. At the Research Center camp, two 15 W handheld runs measured 23.35 and 23.57 game fps.

## v0.7.1 — 2026-10-11

Integrate VU1 execution improvements, warp-entry and kernel-heap fixes, sprint handling and controller prompts on v0.7.0. Two Research Center camp runs at 15 W measured 23.43 and 23.47 game fps. See [versions](docs/VERSIONS.md), [performance](docs/PERFORMANCE.md) and [setup](docs/SETUP.md).
