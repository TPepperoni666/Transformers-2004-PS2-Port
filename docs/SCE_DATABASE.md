# External SDK signature input

Source distributions omit the SCE signature payload. BYOD setup uses Git to fetch
"https://github.com/ran-j/PS2Recomp.git" at revision
`7562ec14c9b3b4f9f09040a5e950a4bb67ab7a66`, file
`ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`.
SHA-256 of the exact upstream bytes: `0fdd1168a815c8af2529cae9076ae813a1304c5c3a41e48d88d38e21e7b5833d`.
This adds Git/network access on first setup; a matching private cached header is
verified and reused offline. Corrupt cached data fails closed; use a fresh work
directory rather than bypassing the check. Normal setup cannot silently proceed
without SDK matching. The analyzer binary and object files embed the private data.
Do not redistribute the private work directory, analyzer binaries/libraries, or
compile outputs. The game runner is a separate target; audit any binary release.

The local upstream commit names Ranieri as the integrator. Its root GPL-3.0 text
does not establish a separate license for the database or rights to SDK-derived
instruction fingerprints. Original sce-symbol-scanner repository/author/license
were not found locally. This fetch arrangement avoids bundling the data; it does
not assert permission to redistribute it or certify the upstream data's legality.

An ordinary source-only analyzer builds with an empty database and can still use
ELF/debug symbols and heuristics. That mode is not qualified for this game's BYOD
setup: losing library names/function extents can change runtime stub selection.
See setup/sce_database.py for the pinned fetch and cache validation.
