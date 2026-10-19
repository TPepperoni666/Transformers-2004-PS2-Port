# Transformers PS2 Port v0.8.3 - first public release

A native PC port of **Transformers (PS2, 2004, NTSC-U SLUS-20668)**, made by statically recompiling the game's code to C++ with PS2Recomp. It is not an emulator: the game's own code runs as a native program, with parallel-gs (Vulkan) drawing the PS2 graphics.

> **Bring your own disc.** This release contains no game files and no code generated from them. Setup verifies your own NTSC-U disc image and generates everything locally. There is no download-and-play build, and there never will be one here.

## What you get
- The game running natively on Windows (and the Windows build under Proton on a Linux handheld), played well into the campaign.
- Widescreen (16:10, 16:9, 21:9) with an in-game PC settings menu, plus an experimental "Show more at screen edges" option.
- Optional sharper output / supersampling, controller and keyboard support.
- VU1 work generated ahead of time from your own disc at setup.
- An optional "optimise for this PC" step (about 1.5 hours), off by default.

## New in v0.8.3
- Faster EE checkpoints and RAM stores.
- Boss health gauge fixed in widescreen: the fill pieces no longer break away from the frame after the first hit (16:10, 21:9).
- Lower lock overhead under Proton (a lock-free check on a hot queue), and a widescreen fix for the Mini-Con pickup menu (from the 0.8.2 work).
- A test-only diagnostic logging stall fixed.
- New default-off presentation trace (`PS2X_PRESENT_TRACE=1`) for investigating frame stalls. Experimental: long sessions can exceed its log limit.
- Setup now downloads the pinned SDK function-fingerprint database from the upstream PS2Recomp project and checks its hash. This repository does not redistribute that database. **Git is needed on first setup.**

## Requirements (short version)
Your NTSC-U disc as a 2048-byte-sector ISO, Windows 11 with Visual Studio 2022 Build Tools, Python 3.11+ (`pyelftools`), CMake 3.21+, Ninja, Git, the Vulkan SDK and a Vulkan GPU. See the README for the full list and [setup](https://github.com/TPepperoni666/Transformers-2004-PS2-Port/blob/main/docs/SETUP.md).

## Tested
- Windows 11 desktops (NVIDIA RTX 40 series, AMD RX 9070 class): limited setup and play coverage.
- A Linux handheld running the Windows build under Proton, at 15-35 W: limited play coverage.
- Only the NTSC-U disc (SLUS-20668) is supported. PAL, macOS, Intel GPUs and other handhelds are untested.

## Known issues
- Brief presentation stalls (short black or frozen frames) have been seen on one Linux handheld under Proton. The trace switch above is there to help find the cause.
- The experimental native high-resolution renderer is not included in this release.
- Testing uses scripted routes and play sessions; this is not proof of a bug-free full playthrough.

## Reporting problems
Use the **Bug report** or **Setup problem** issue forms. Never upload disc images, game files, saves or memory dumps.

## Notes for other porters
Everything we found - runtime bugs and fixes, performance results including what did not help, Proton and widescreen lessons and our testing method - is in [docs/findings](https://github.com/TPepperoni666/Transformers-2004-PS2-Port/blob/main/docs/findings/README.md).

## AI assistance
This port was built with heavy AI assistance, directed and play-tested by the project owner.

## Credits and licence
Built on PS2Recomp and parallel-gs, with other open-source components credited in CREDITS.md and NOTICE. Licensed under GPL-3.0; see LICENSE.
