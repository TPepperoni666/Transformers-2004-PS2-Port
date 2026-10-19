# Third-party notices

Release-preparation research credits include rac1-native-game (MIT) and ps2-port-engine (MIT); see CREDITS.md. This task independently authored documentation and orchestration, rather than copying their source. If future changes import MIT source, retain the exact upstream copyright and permission notice for the imported revision. A research acknowledgement is not a substitute for a copied-source license notice.

Their unmodified license texts are preserved as reference snapshots in `NOTICE/licenses/rac1_native/LICENSE` (Copyright 2026 Daino Selvatico) and `NOTICE/licenses/ps2_port_engine/LICENSE` (Copyright 2026 Daniel Gomes Vieira), copied from the existing offline research checkouts. These snapshots do not imply their implementations are bundled. License applicability for imported files still requires file-level review.

The port's own PS2Recomp-derived source is GPL-3.0. Original file notices take precedence for third-party components; GPL-3.0 in the root does not replace their licenses. No dependency authors endorse this project.

See CREDITS.md for file-level adaptations. parallel-gs is LGPL-3.0-or-later; Granite and volk are MIT; PCSX2 adaptations/references are GPL-3.0-or-later; AMD FSR1/CAS MIT; raylib/SDL2 zlib; Dear ImGui MIT; FFmpeg LGPL-2.1-or-later under the selected LGPL build; OpenEXR/Imath BSD-3-Clause. Recompiler dependencies include ELFIO, toml11, fmt and Rabbitizer (MIT), libdwarf (file-dependent LGPL/BSD). SDL2, FFmpeg, OpenEXR and Imath are not vendored binary distributions here.

Granite bundles Khronos Vulkan/SPIR-V headers/tools, shaderc and SPIRV-Cross (Apache-2.0 and project-specific notices), glslang (mixed BSD/MIT/Apache notices), stb (MIT/public domain), MikkTSpace (zlib), RenderDoc API header (MIT), dirent (MIT), and AMD FidelityFX shaders (MIT). Read individual source notices before redistributing optional tools. Upstream asset images and compiled binaries were omitted.

Preserve all notices when redistributing. Binary distribution requires the corresponding source and dependency license compliance, including the LGPL requirements for static linking of parallel-gs and the exact FFmpeg build's licenses. This stage is not a binary release compliance certification.

The sce-symbol-scanner SDK signature payload is excluded from source distributions. BYOD setup obtains the pinned PS2Recomp snapshot into private work; see docs/SCE_DATABASE.md. A separate database license was not found locally. Do not distribute that private input or an analyzer embedding it until permission is established.

## Bundled license-text index

- `deps/parallel-gs/COPYING.LGPLv3`
- `deps/parallel-gs/Granite/LICENSE`
- `deps/parallel-gs/Granite/third_party/glslang/LICENSE.txt`
- `deps/parallel-gs/Granite/third_party/khronos/vulkan-headers/LICENSE.md`
- `deps/parallel-gs/Granite/third_party/shaderc/LICENSE`
- `deps/parallel-gs/Granite/third_party/shaderc/third_party/LICENSE.glslang`
- `deps/parallel-gs/Granite/third_party/shaderc/third_party/LICENSE.spirv-tools`
- `deps/parallel-gs/Granite/third_party/spirv-cross/LICENSE`
- `deps/parallel-gs/Granite/third_party/spirv-cross/LICENSES/LicenseRef-KhronosFreeUse.txt`
- `deps/parallel-gs/Granite/third_party/spirv-headers/LICENSE`
- `deps/parallel-gs/Granite/third_party/spirv-tools/LICENSE`
- `deps/parallel-gs/Granite/third_party/spirv-tools/utils/vscode/src/lsp/LICENSE`
- `deps/parallel-gs/Granite/third_party/volk/LICENSE.md`

Fetched-dependency license snapshots (from the offline cache used for build verification):

- `NOTICE/licenses/elfio/LICENSE.txt`
- `NOTICE/licenses/fmt/LICENSE`
- `NOTICE/licenses/imgui/LICENSE.txt`
- `NOTICE/licenses/libdwarf/COPYING`
- `NOTICE/licenses/nlohmann_json/LICENSE.MIT`
- `NOTICE/licenses/rabbitizer/LICENSE`
- `NOTICE/licenses/raylib/LICENSE`
- `NOTICE/licenses/rlimgui/LICENSE`
- `NOTICE/licenses/sdl2/LICENSE.txt`
- `NOTICE/licenses/toml11/LICENSE`
