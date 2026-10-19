# Troubleshooting

| Symptom | What to do |
|---|---|
| Unsupported disc SHA256 | Use the exact NTSC-U SLUS-20668, 2048-byte-sector ISO described in SETUP. Other revisions, raw-sector images and compressed formats are unsupported. Do not disable the identity check. |
| ISO does not exist | Quote the absolute ISO path and check it is readable. |
| Private work must be outside source | Choose a separate private work folder, outside the checkout. |
| CMake, Ninja, cl or Python not found | Install the build requirements and open an x64 VS developer shell. Check `python --version`, `cmake --version`, `ninja --version`, `cl`. |
| Missing Python ELF module | Install pyelftools in the same Python environment used by setup. |
| Offline configure fails | Supply all source caches and a local FFmpeg archive. Disconnected mode cannot replace a missing dependency. |
| Interrupted setup | Repeat the identical setup command. Verified steps are reused; partial attempts are retained. |
| Stale generation or changed extracted ELF | Rerun setup; if the selected ELF is modified, use a new private work folder. Do not edit inventories or generated code to bypass verification. |
| Launcher cannot find disc.json | Use the `generation-<id>/game` folder printed by setup, rather than the top-level work folder. |
| Missing DLL or Vulkan initialization failure | Rerun setup to deploy DLLs; confirm Vulkan SDK/driver and GPU support. Keep the executable and deployed DLLs together. |
| Low speed compared with published measurements | Default fresh-disc setup has no compiled VU1 package. Packaged AOT measurements are not a guarantee for this build. |
| Wrong controller action | Press F7 and verify SDL button/axis mapping, especially shoulders and triggers. |

For a bug report, include the source revision, OS, CPU/GPU, toolchain, setup stage, command with personal paths replaced, and a short reviewed error excerpt. Keep disc images, generated sources, extracted modules, saves, captures and full logs private. See [CONTRIBUTING.md](../CONTRIBUTING.md).
