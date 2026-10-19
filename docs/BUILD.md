# Build and resume

Use [SETUP.md](SETUP.md) for supported disc identity and generation details. On Windows, open an **x64 Native Tools Command Prompt for VS 2022**. Install Python 3.11+, pyelftools, CMake 3.21+, Ninja and the Vulkan SDK before setup. Setup does not install them for you. A first configure normally fetches dependencies; an offline build needs existing source caches and a local LGPL FFmpeg archive.

```powershell
python -B setup/setup.py --iso "<absolute-iso-path>" --work "<absolute-private-work-folder>"
```

Repeat this exact command to resume. Extraction, VU indexing and completed generated code are verified before reuse. Interrupted generation restarts in a retained attempt; Ninja resumes compilation. Changed disc, source dependencies or CMake arguments select another `generation-<id>` directory. Setup never deletes earlier attempts.

The final console line names the actual game folder: `<work>/generation-<id>/game`. Read `<work>/setup-state.json` for the selected generation. Build output is `<work>/generation-<id>/build-game`. Do not use the obsolete `<work>/game` path. A successful build is a compile result; fresh-disc gameplay remains a separate qualification.

For offline setup, repeat `--cmake-arg=-DFETCHCONTENT_SOURCE_DIR_<NAME>=<absolute-cache-path>` for every FetchContent dependency used by the selected targets, add `--cmake-arg=-DFETCHCONTENT_FULLY_DISCONNECTED=ON`, and set `--cmake-arg=-DFFMPEG_PREBUILT_URL=<absolute-local-zip>`. Disconnected mode does not supply absent caches and does not prevent ExternalProject downloads if its URL is still remote. Check all paths first. Keep compiler, dependency versions and configure arguments identical when comparing generations.

After a runtime-only edit, run setup again. Its generator dependency inventory may require a fresh generation; do not bypass the manifest target. Manual runner configurations require both `PS2X_GENERATION_MANIFEST` and its matching `PS2X_GENERATED_DIR`. Rebuilding an already valid configuration uses:

```powershell
cmake --build "<work>/generation-<id>/build-game" --target ps2EntryRunner --parallel 4
```

That command does not refresh the deployed game folder: rerun setup to copy the executable and DLLs. Never edit generated runner files to fix a runtime bug. Default setup indexes VU programs but uses the interpreter, without the optional static package; older packaged-build performance figures do not describe this setup build. See [VU1_AOT.md](VU1_AOT.md) for the separate optional workflow.

Retain setup output, tool versions, configure arguments, private manifest and route results locally. Review logs for personal paths and game-derived material before sharing an excerpt. Disk usage and full clean build duration remain unmeasured; allow room for two generated source trees, two build trees and retained attempts.
