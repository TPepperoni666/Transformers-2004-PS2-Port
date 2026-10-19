# Public decisions ledger

| Decision | Reason | Revisit when |
|---|---|---|
| User supplies exact NTSC-U disc; all products external | Source distribution must exclude game-derived data; full SHA-256 fixes input identity | Another disc is independently supported |
| Completed hashed generation inventory required by runner | Prevent mixed/stale output; interrupted attempts retained | New generation stages are added |
| Index-content commit guard, 1 MiB cap on staged changes | Working-tree scans cannot see staged/worktree disagreement; existing reviewed vendor sources need explicit audit | Dependency provenance changes |
| Vulkan parallel-gs; Proton handheld path | Existing Windows/SteamOS testing; native Linux qualification remains open | Native Linux passes behavioral gates |
| 30 Hz simulation; optional 60 Hz fields | Presentation rate is separate from game logic | Interpolation is independently qualified |
| Presets describe existing capabilities | v0.8.3 includes the settings registry/menu; disabled features remain unavailable | New settings become wired and qualified |

Setup checkpoints, staged-content guarding, inventory checks, categorized environment docs, preset organization and task-oriented docs were inspired by rac1-native-game, Copyright (c) 2026 project contributors, MIT licensed. Implementation and prose here were independently written; no upstream code was copied. The upstream sound implementation has separate third-party licensing and was not imported.

The source scanner permits exact reviewed hashes for tools/vu1/toolchain/generated/lower_template.inc and tools/vu1/toolchain/native/src/lib/vu/ps2_vu1_lower.cpp. These are generic lower-opcode implementations (lowerStatic template / execLower decoder taking instruction words), reused by independently generated packages; they contain no per-disc instruction bodies. Hash equality with a private package reflects shared authored machinery. Changes invalidate this narrow exception.
