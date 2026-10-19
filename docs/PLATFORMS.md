# Choose a platform

| Path | Status | Requirements |
|---|---|---|
| Windows x64 | Current build/play path; fresh-machine setup still needs qualification | Python 3.11+, C++20 compiler, CMake/Ninja, Vulkan SDK/driver |
| Linux with Proton / SteamOS | Windows build used for handheld testing | Proton, Vulkan driver, private external Windows game folder and ISO |
| Native Linux | Source path exists; not release-qualified | Compiler/dependencies and fresh-disc behavioral validation pending |

On SteamOS copy your own locally built game folder and disc to private storage. Use launch/play.sh with PROTON pointing at the Proton launcher, or configure a Steam non-Steam shortcut to the Windows executable with a compatible Proton version. Keep ELF/ISO paths explicit; launch from the private game folder. Configure Steam Input to expose a standard controller, then inspect inputs with F7. Keep saves and generated outputs outside the checkout. See [setup](SETUP.md) and [input](INPUT.md).

Report platform, GPU/driver, Proton version, build revision and settings when investigating compatibility. Keep captures, cards and raw private-data logs local. Windows background timing can be power-throttled; compare variants with the same power state. Platform claims require route counters plus visual/audio checks, not compilation alone.
