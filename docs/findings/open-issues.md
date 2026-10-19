# Open issues

Remaining limitations and experiments as recorded in the supplied development reports.

## Campaign and platform coverage

Public-source setup rehearsals passed a defined set of startup and cinematic gates.
They did not establish campaign completion, every saved route or all controller behavior.
The public-rehearsal report explicitly excludes whole-game, cross-platform and speed qualification.
Keep supported configurations and untested platforms distinct.
Add coverage with actual reproducible routes and captures before broadening claims.

## VU0 AOT and VU1 optimization

VU0 AOT remains an offline-validated, default-off experiment pending whole-game gates and timing.
VU1 sampled coverage does not establish zero fallback at every point in every level.
Memory movement, unpacking and runtime glue remain useful profiling targets.
Proposed caching or vector kernels need their own exactness checks and controlled measurements.
Do not publish estimated recoverable percentages as implemented or measured gains.

## Audio repeatability

Shipping-worker PCM repeats remained unresolved in the detailed timing investigation.
Controlled worker-zero captures isolated repeatable register input and mixer output.
The discrepancy involved input-write timing, not demonstrated changes in mixer evaluation.
Fixed RTC inputs alone did not remove the shipping-mode variation.
Do not infer an audio-queue drift controller or a universal sample-exact shipping guarantee from these tests.

## Higher-rate game logic

The logic-rate experiment ran real 60 Hz logic at the intended speed in the tested scenarios.
The source still listed hardcoded 1/30 assumptions and untested audio, AI and boss behavior.
It estimated twice the emulated work compared with 30 Hz logic.
The recorded plan favored 30 Hz logic plus interpolation, with native 60 Hz as later experimental work.
Feasibility is not complete gameplay qualification or an implemented universal interpolation solution.

## Native rendering is still experimental

The native producer/transport continuation did not have accepted live scene ownership closure.
Its report explicitly records no qualification runner or successful game gate for that continuation.
An offline overlay and more bindings do not establish a playable replacement renderer.
Keep exported geometry, live-state binding and rendering qualification as distinct milestones.
The support-document review likewise labels the native renderer absent from the supported public configuration.
