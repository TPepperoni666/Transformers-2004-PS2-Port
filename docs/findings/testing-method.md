# Testing method

Regression checks establish specific invariants and coverage. They do not prove the entire port correct.

## Routes and disc-read anchors

The project uses scripted pad input to replay startup, cinematic, combat and driving scenarios.
Anchoring input to disc-read milestones helped reach stable menu and loading transitions.
Host controller and keyboard input must be isolated during those scripts.
Anchors improve repeatability; they do not guarantee identical execution on every machine.
The sources still document worker-timing variation and route-specific endpoint signatures.

## Counters are a gate, not a full-state proof

Checkpoint tuples track guest vsyncs, pad polls, EE cycles, disc activity, VIF1 interrupts and VU1 calls.
Compare the whole timeline and terminal tuple against the reference for that build and route.
Exact tuples alone cannot establish renderer health, complete memory equivalence or audible correctness.
Use separate controlled state hashes, PCM, readback, liveness and visual checks where relevant.
Do not rename disc activity as a verified sector total without checking the observer's definition.

## Deep-route signatures: establish the baseline first

Driving-route endpoint cycles sometimes differed by eight while the other five counters matched.
The documented policy accepted two known endpoints only with those other counters identical and no faults.
Training acceptance was initially handled separately from unchanged final qualification.
Later integration repeated OFF versus OFF when the baseline itself differed.
This is a bounded route-specific signature, not a general allowance for any small cycle delta.

## Controlled VU1 state hashes

Shipping threaded execution did not always produce repeatable per-call state-hash observations.
Controlled gates use worker mode zero, kernel heap disabled, glyphs off and a HASH-only observer.
Compare both sides with the same observer and controls; keep shipping-setting counter gates separately.
Compare ordered call/hash pairs and the terminal tuple.
Presentation labels can shift with host observation timing and are diagnostic, not guest call timestamps.

## PGO: training coverage and qualification have different jobs

Freeze source and build inputs before collecting and merging profiles.
Audit the actual training routes: a profile may be exact but omit expensive later scenes.
An ocean scene was missing from an earlier training set, and later training added ocean coverage.
A narrowly accepted training checkpoint variation is not permission to weaken final regression gates.
Controlled PCM comparisons are also needed when shipping worker timing makes reference audio nonrepeatable.

## Bring-your-own-disc: rehearse from an empty work directory

Early clean-source setup missed required IOP boot files and disc-layout information.
A corrected second rehearsal completed setup in 38.3 minutes without manual steps.
That rehearsal passed smoke and exact startup/cinematic counters, with zero source-guard violations.
Later rehearsals added disc-generated static VU1, controlled hashes and save persistence across setup generations.
These are specific clean-setup results, not a universal build-time promise or whole-campaign qualification.

## SDK matching data: fetch privately at setup

The analyzer's bundled SDK fingerprint database included sampled instruction words as well as names and hashes.
The public-source decision removes that payload instead of redistributing it in source or binaries.
Setup obtains a pinned upstream copy and checks its hash as a private dependency.
The later fresh-disc rehearsal passed with that fetch and 760 SDK matches.
This packaging decision does not establish a separate redistribution licence for the database.

## Diagnostics can change the phenomenon being tested

The HUD-logging boss freeze was induced by test-only output rather than the ordinary play environment.
Build complete log lines in memory and minimize synchronous writes on the EE thread.
Keep observers identical for OFF/ON comparisons and disclose enabled diagnostics.
Separate host presentation timestamps from guest state observations.
Retain failed runs and their diagnosis rather than counting a retry as proof that the original passed.
