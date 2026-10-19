# Publication checklist

This source prepares the v0.8.3 release; publication requires a separate maintainer decision. Complete every blocking item before the first public release.

- [ ] Select the exact release source revision and reconcile the older README performance/version claims with it.
- [ ] Fresh supported-disc setup from a clean checkout completes extraction, VU indexing, generation, build and deployment using no pre-generated runner or private VU package.
- [ ] Interrupt/resume at extraction, indexing, generation and build; verified completed outputs are reused, partial attempts retained, changed dependencies regenerate and stale inventories fail before runner compilation.
- [ ] Record full setup time, disk use, tool/dependency versions and offline cache recipe; test setup on a fresh player machine.
- [ ] First boot reaches an agreed visible landmark with a fresh card. A zero process exit alone does not establish that landmark. Use hidden, quiet runs at lower priority for unattended smoke tests.
- [ ] Both fresh-card reference routes pass all six counters and route timelines exactly against testing/REFERENCES.md; resolve mismatches rather than silently rebasing counters.
- [ ] Optional static VU build gets shipping-setting counter gates and controlled HASH-only GH: `PS2X_VU1AOT_HASH=1`, `PS2X_VU1_THREAD=0`, `PS2X_KERNEL_HEAP=0`, `PS2X_GLYPHS=off`, same observer on both sides. An interpreter-only smoke is not this proof.
- [ ] Renderer comparison, audible movie/game audio, controller shoulders/triggers, settings, save/reload and supported Proton route receive separate playtesting.
- [ ] Fresh before/after comment-only generator products compare exactly with tools/check_generation_equal.py.
- [ ] Review all reachable history and the final tree for private identities, paths, hosts, network addresses and internal identifiers; use only a no-reply address for the project owner's email. Upstream copyright contacts retain their notices. Resolve personal commit metadata before publishing the existing history; this task does not rewrite it.
- [ ] Independently run the source/game-data policy checks over the intended published history and tree. A privacy audit does not establish absence of game-derived data. Review every retained image and its provenance policy.
- [ ] Verify the source export excludes the SCE signature payload and private build products (docs/SCE_DATABASE.md); run the fresh BYOD rehearsal. Published history must also omit the old embedded payload; a final-tree deletion alone is insufficient. Separate database redistribution rights remain unestablished; verify unresolved upstream author links.
- [ ] Preserve GPL-3.0 port notices, LGPL parallel-gs notices, MIT Granite and all nested dependency notices. For any binary package provide corresponding source and satisfy LGPL relinking requirements, including exact FFmpeg configuration. Research acknowledgements do not grant redistribution rights.
- [ ] Audit the final release archive independently: no disc, ELF/IRX, generated C++, VU programs, private paths, saves, build logs or undocumented binaries.
- [ ] Review README, setup, controls, troubleshooting, settings, build instructions, credits and third-party notices from a player perspective.
- [ ] Maintainer separately authorizes publishing after reviewing the above evidence. No push or remote change belongs to this preparation task.
