# Bring your own disc

Setup uses Python 3.11+ and the included stdlib-only setup/extract_elf.py to verify the full supported disc SHA-256 and read 2048-byte-sector ISO9660 without mounting or modifying the disc. Setup extracts /SLUS_206.68 and the nine /IOP boot files, because the current IOP module loader opens those files through host paths. Other assets are streamed from the original ISO through PS2X_CD_IMAGE. It also generates isomap.tsv for scripted route path anchors. Install `pyelftools` in your Python environment first. Setup requires an external private work directory and leaves all extracted/generated/build products there.

First setup also requires Git and access to the pinned upstream SDK signature input; see [SCE_DATABASE.md](SCE_DATABASE.md). The verified header and analyzer build remain private. A cached matching header is reused offline. Source-only builds without this input disable SDK matching and are not a qualified substitute for normal BYOD setup.

The analyzer accepts `<elf> <toml>` and the recompiler accepts that TOML. Both executables are built from the included sources. Dependency fetching is controlled by their original CMake files; offline users supply FETCHCONTENT_SOURCE_DIR_* caches. No dependency downloads were performed during staging.

Function-map and qualification scope: the historical Transformers function map was refined with constructor, indirect-target and delay-slot discovery across multiple regenerations. The stock analyzer's output has not been proven to reproduce that map. tools/make_map.py parameterizes the historical mapper and takes the fresh analyzer/recompiler output as its baseline; setup now runs that second pass locally. This removes its pre-existing output_clean dependency, and automatically removes real branch-delay-slot splits. Offline testing on the local ELF reproduces 21,941 mapped function entries and 21,944 generated C++ files, matching the accepted runner file names; sampled function bodies match after newline normalization. Fresh-disc Windows MVP and cinematic routes have exact counter references; these limited paths, controlled state hashes and save persistence are qualified separately from whole-game, visual/audio and Proton behavior. A compile alone proves none of those behavioral gates.

Generated output is referenced via PS2X_GENERATED_DIR; it is never copied into the source tree. The runner's generated headers also come from that directory. VU1 AOT is disabled by default; the optional static package workflow in VU1_AOT.md builds it from your disc and enables it in the play launchers. Setup defaults to the Vulkan parallel-gs backend; a Vulkan SDK and supported GPU are needed. The native Linux path is not yet qualified; launch/play.sh can instead launch a Windows build with Proton via PROTON.

The ISO extractor was verified against the local disc: its extracted ELF matches the existing ELF byte-for-byte by SHA-256. Python and Windows/Bash script syntax checks passed. The analyzer/recompiler and standalone PGS runtime built from this source using offline dependency caches.

## Verified VU1 tooling after ELF extraction

Python 3.11+ is required for these tools. The supported NTSC-U ELF SHA-256 is `ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb`; full ISO SHA-256 is `89fc6ee694303bac0993b72bbdd89be8cc58d25af831b7072b6ec67fad2b37cf`. Offsets, sizes and hashes are metadata; extracted programs and generated outputs are private game data.

Use fresh external work/output directories. Replace the example relative paths with your own private locations:

```powershell
python tools/vu1/index.py --elf ../private-disc/SLUS_206.68 --work ../private-index
python tools/vu1/package.py ../private-disc/SLUS_206.68 --verify-only
python tools/vu1/package.py ../private-disc/SLUS_206.68 --work ../private-static-work --out ../private-static-package
python tools/vu1/scan_disc_vu1.py --iso ../private-disc/game.iso --manifest ../private-index/extracted/manifest.json --out ../private-scan
```

The index step creates `vu1known.bin`, metadata and extracted programs. Set `PS2X_VU1NEW_TABLE` to the private table or copy it beside your locally built executable. `setup/setup.py` runs index generation automatically after extraction and installs the table beside the runner. Static generation is separate and optional; see [VU1 integration](VU1_AOT.md). For a manual build pass `-DPS2X_WITH_VU1_STATIC=ON -DPS2X_VU1_STATIC_DIR=<external-package>` along with your external generated runner directory. No build searches parent folders for disc files.

The generated `game` folder is a local launch directory, not a standalone portable
package. Its `disc.json` records absolute ISO, ELF and save paths. Keep the entire
private work directory (including the ELF, adjacent `IOP/` and `isomap.tsv`) and
original ISO in place. On another machine, rerun setup with that machine's paths;
copying only `game` or adding an ELF to it does not create a working deployment.

## Resume setup

Run `python setup/setup.py --iso <absolute-iso> --work <external-private-work>` with Python 3.11+, CMake, Ninja, a C++20 compiler, Vulkan SDK and pyelftools installed. Repeat the same command after an interruption. Setup retains every prior attempt; no cleanup is automatic. Its state records the ISO SHA-256, CMake arguments and source dependency hashes. Changed dependencies select a fresh generation. Verified ELF/index/code products are reused; tool and runner builds resume through Ninja. A failed code-generation step is restarted in a fresh attempt directory.

`generation-<id>/generation.json` is the authoritative private inventory: extracted ELF, VU table/programs/metadata, analyzer TOML, baseline code, function CSV, mapped TOML and mapped runner sources, each with SHA-256. It records the disc identity and generator dependencies. Build tools, game binaries/DLLs and local disc.json are deployment products, outside the generation inventory. Optional static VU packages remain a separate workflow and are not selected by setup.

Every runner build checks the manifest, dependency hashes, product hashes and generated file set before compilation. Manual runner configurations must supply `PS2X_GENERATION_MANIFEST` from setup along with its matching `PS2X_GENERATED_DIR`. Do not edit private output to repair freshness failures; rerun setup. Retained failed attempts may consume substantial disk space.

For comment-only generator edits, generate into two fresh private directories from the same disc/toolchain, then run `python tools/check_generation_equal.py <before-output> <after-output>`. This requires identical file sets and bytes, including annotations. It does not replace route qualification. Absolute paths in TOML differ between directories, so compare runner output directories rather than whole work directories.

Contributors enable the staged-data guard with `git config core.hooksPath tools/git-hooks`.

## Fresh-disc rehearsal findings

An ELF-only deployment can compile successfully but reaches a guest fatal error at TDG_PlatformSupport.cpp:790 while finding TwoFish.irx. The private /IOP files must remain next to the ELF in their disc directory layout. Do not redistribute them. The runtime-files helper verifies the full disc hash and retains a hash inventory. Scripted tests also need PS2X_ISOMAP; the portable runner selects isomap.tsv beside the ELF when available.

The Windows portable runner treats CPU sampling as optional when CIM access is denied and retains the child process handle before reading ExitCode. Always require a runtime script verdict and exact counters, since a wrapper exit alone does not establish a route pass.

Saves live at `<work>/saves/mc0`, recorded as `mc_root` in `disc.json`, across setup
generations. Setup copies the previous generation's `mc0` once if the persistent
card does not exist. Keep the old generation until you verify your save. Play
launchers keep five rotating ZIP backups in `<work>/saves/backups` before launch;
scripted gates retain their own throwaway cards and do not rotate user backups.
A static VU1 build records `static_vu1: true`; both play launchers then enable the
fast VU1 preset automatically. Python must be on PATH when launching on Windows.
