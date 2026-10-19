# Optional version notice (proposal for a later release)

Design only. No networking, updater or settings implementation ships with this document. The first release uses manual updates. A notice would say “New version available”, offer the release notes and manual update instructions, and never download or execute code.

## Source and request

Once the public repository address is final, configure one fixed HTTPS endpoint: `https://api.github.com/repos/<public-owner>/<public-repository>/releases/latest`. These placeholders must be resolved and reviewed before implementation; there is no private fallback service. Fetch public release metadata only: `tag_name`, `html_url`, `draft` and `prerelease`. Do not fetch release assets, source archives or game data. Accept only stable version tags with a strict semantic-version parser; do not compare strings lexically. Ignore drafts, prereleases and older/equal versions; a development build should direct the user to the Releases page without guessing an upgrade.

Use an unauthenticated GET without cookies, authorization, user identifiers, query parameters, disc serial/hash, settings, logs or hardware information. Use a constant application user-agent without the local version. The current version is compared locally and never sent. A direct HTTPS request necessarily exposes the client IP, request timing and normal transport metadata to GitHub and its infrastructure. There is no project telemetry or separate analytics endpoint; do not call this anonymous in the UI.

## Consent, frequency and controls

Proposed local preference: `check_for_updates`, default off until the user chooses to enable automatic checks. Explain the destination and network metadata before that choice. Once enabled, check at most once per 24 hours on a worker outside gameplay, never on a frame-critical thread. **Check now** makes one explicit request even with automatic checks off; it does not enable them. **Skip this version** stores only that tag locally. Disabling automatic checks persists across launches and cancels pending work; a proposed `PS2X_VERSION_CHECK=0` override disables all checks, including Check now, for offline deployments. These are proposed names, not supported current settings.

Store only enabled/disabled, last attempt time, last valid version and skipped tag locally. No install ID, server registration, history upload or automatic bug submission. Do not add the feature's preference to a game save or modify the disc. Exclude request metadata and private paths from diagnostic output.

## Failure and untrusted input

Use system HTTPS certificate validation, a short total timeout (for example 3 seconds), a 64 KiB response cap and no automatic retries within the same day. Rate limits, malformed JSON, captive portals and offline failures must never delay launch or block play. An explicit check can show “Could not check; open Releases manually”; automatic failures stay quiet. Cache is informational, never proof the installation is current.

Reject redirects to a different host and reject unexpected content. Validate `html_url` against the configured HTTPS GitHub repository's `/releases/tag/` path; never open an arbitrary URL returned by the network. Render a bounded tag as plain text. Do not render remote release HTML or Markdown in the game, pass remote strings to a shell, follow asset links or start a build. Opening the validated release page requires a user click. HTTPS protects transport; unsigned metadata is not authenticated release content suitable for executing an update.

## Manual update flow

Show the installed version, available version, **Release notes**, **Remind me later** and **Skip this version**. Release notes open the validated browser page after a click. The user chooses the source release and follows the documented setup/build workflow with their own disc. Settings, saves, generated files and existing executables remain untouched by the check. Automatic replacement, rollback and signed update manifests are separate future work.

## Acceptance before implementation ships

Use an injected HTTP adapter and local fixtures to check off/no-network, manual-only, frequency limits, cancellation, offline/rate-limit handling, malformed/oversized responses, prereleases, semantic ordering, skip persistence, redirects and malicious URLs. Inspect captured synthetic requests to verify that no version, hardware, disc or identifier is sent. Verify normal play is unchanged while a request stalls and that disabling checks prevents requests on the next launch. Network integration and privacy review remain prerequisites for the later feature.
