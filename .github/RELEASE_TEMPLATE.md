# v0.8.3 release notes template

Draft: replace every placeholder below using verified results before publishing. This file is a format, not a claim that a fix or release has shipped. Use relative issue links so the template does not need a hard-coded repository owner.

Release date: <YYYY-MM-DD>
Source revision: <immutable revision>
Previous release: <tag>

## Fixed issues

<!-- Replace ISSUE_NUMBER and the description; repeat one row per verified fix. Do not invent issue numbers or mark an unresolved report fixed. -->
| Issue | User-visible fix | Verification |
|---|---|---|
| [#ISSUE_NUMBER](../../issues/ISSUE_NUMBER) | <symptom, trigger and resulting behaviour> | <route/check, configuration and result> |

For changes with no existing report, create or identify a tracking issue before publishing an issue-linked entry. If there are no gameplay fixes, say so and list only the documentation/setup changes that actually shipped.

## Setup and update

Bring your own legally obtained NTSC-U disc SLUS-20668. This release contains no game files or generated game code. Follow [setup](docs/SETUP.md); optional optimise is off by default and can take about 1.5 hours depending on hardware.

Manual update: obtain the tagged source from this repository's Releases page, keep your disc and private work folder, and follow [build and resume](docs/BUILD.md). Back up your saves before regenerating or rebuilding. Record any release-specific migration steps here: <steps or none>. Do not overwrite a working installation until the new build launches and the existing saves are verified. No self-updater is included.

## Tested for this release

<List only the platforms, GPU/driver versions, disc, routes and save-persistence checks actually tested for v0.8.3. Separate older coverage from this release's results. Record counter/hash verdicts and any checks not run. Do not paste private logs.>

See [Support](README.md#support) for the general coverage matrix. Limited scripted routes do not prove a complete campaign playthrough. PAL, macOS, Intel GPUs and other handhelds remain untested unless qualified explicitly.

## Known issues

- Brief presentation stalls have been seen on one Linux handheld under Proton; record whether this release reproduced them.
- The experimental native renderer is not included; rendering uses parallel-gs.
- <Link remaining reports, limitations and workarounds; do not silently drop unresolved issues.>

## Contributors and provenance

<Link merged PRs, disclose AI assistance and describe review/testing. Preserve applicable notices; no game-derived data may be included.>
