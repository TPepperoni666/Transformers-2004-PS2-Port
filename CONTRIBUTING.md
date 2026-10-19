# Contributing

This project uses AI assistance openly. Do not suggest upstream PRs to projects whose contribution norms reject AI-assisted work.

PRs must include the contributor's own testing, with exact commands, configuration, scripted route, counter results and timing. Alternatively, maintainers run the repeatable routes in testing/ before merging. Match every counter in testing/REFERENCES.md exactly; explain intentional guest-behaviour changes. Report at least three quiet timing runs per variant, their spread and total machine CPU load; discard busy-machine measurements. Compare route timelines as well as final counters.

Never include disc images, ELF/IRX files, game assets, generated game C++, VU1 packages, dumps, game-frame screenshots, memory cards, game-data logs or binaries. Run tools/check_no_game_data.py before submitting. Keep private build and extraction directories outside the source tree. Preserve dependency copyright and license notices, and identify the source of adapted code and techniques.

Start with [BUILD.md](docs/BUILD.md) and [TESTING.md](docs/TESTING.md). Changes to host documentation and metadata need the relevant syntax/policy checks; performance measurements are required when making performance claims, rather than for every prose edit. Keep the default interpreter setup distinct from optional static VU builds.

Before sharing changes, run:

```powershell
python -B testing/test_release_prep.py
python -B tools/check_no_game_data.py .
python -B tools/check_staged.py
git diff --check
```

Enable the optional index guard with `git config core.hooksPath tools/git-hooks`. It checks staged bytes rather than just your working files; it does not audit history or prove license compliance. Use a GitHub no-reply address for your project commit identity. Review patches and log excerpts for personal paths, hostnames, email addresses and network details before submitting them. Do not remove upstream author names or copyright contacts from license notices.

For adapted source, identify the upstream project, revision, files, license, original notice and modifications in CREDITS and NOTICE. For independently implemented ideas, label the attribution as research rather than copied code. Resolve publication blockers in [GO_PUBLIC.md](docs/GO_PUBLIC.md) before a release. Full private counter/hash logs and saves stay with the maintainer; a public report should contain reviewed commands and aggregate results.

## Disc ownership and public contributions

Bring your own legally obtained NTSC-U disc, SLUS-20668, for local setup and runtime testing. Keep the original image and all extracted or generated material outside the repository. Do not request or link to downloads of the game. No game-derived data belongs in a PR, issue attachment, fixture, commit history or release archive, even when it is needed to reproduce a bug. Describe the reproduction with text and reviewed aggregate results instead.

## How pull requests are tested

Link the issue being fixed and describe the before/after behaviour. For runtime changes, use the documented scripted routes in [testing/](testing/) and [docs/TESTING.md](docs/TESTING.md), with the same disc, route, settings and fresh card or locally held seed on both sides. Require the route's own PASS verdict and all reference counters, not just a successful process exit. Compare timelines and check relevant visuals/audio separately. Saved routes require private local saves and cannot be reproduced by distributing a save in the PR. If you cannot run a required route, state exactly what remains untested; maintainers must complete those gates before merging.

For VU1 changes, check ordinary counters with shipping settings. Compare per-call state hashes only with the same hash-only observer on both sides and `PS2X_VU1AOT_HASH=1`, `PS2X_VU1_THREAD=0`, `PS2X_KERNEL_HEAP=0`, `PS2X_GLYPHS=off`. Threaded shipping hashes are not a repeatable equality gate. For performance claims, use at least three quiet runs per variant with the same power-throttling opt-out state, and report spread, power limit, foreground/background state and CPU load. Do not infer speed from a correctness gate.

Documentation and issue-form changes need appropriate syntax, links and content review rather than a game run. Keep full runtime logs private; share only redacted host diagnostics and aggregate results that contain no game-derived data. Disc verification and data guards complement human review; neither clears unresolved licensing or release-history checks.

## AI assistance

AI assistance is used openly in this project. State in the PR which tools assisted analysis, code, documentation or testing, what you reviewed yourself, and which checks actually ran. The contributor remains responsible for correctness, licensing, provenance and the absence of game-derived data. Do not claim a generated test result was executed when it was not. Respect other projects' contribution policies when proposing upstream changes.
