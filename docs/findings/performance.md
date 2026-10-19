# Performance

Measurements retain their route, power setting and build context. Historical prototypes are not combined into a single speedup.

## VU0 fast execution: measured ocean benefit

On the tested Proton handheld at a measured 12 W package limit, the ocean-standing route improved.
Fast execution OFF measured 23.6/23.2 game fps; ON measured 29.6/29.8 game fps.
Guest field throughput rose from about 47 to 60 per second.
Reported VU0 share of EE time fell from 47% to 28%.
This comparison concerns the fast interpreter loop, not the later experimental VU0 AOT backend.

## VU0 AOT: offline exactness, speed still unmeasured

The separate VU0 compiler path is default off and has not earned a whole-game speed claim.
Corrected tests preserve architectural constant registers; randomizing VI0 caused the original smoke failure.
The rebased backend passed 100,000 states per admitted entry with state, memory, cycles and resume checks.
Repeated generation was byte-identical and affected translation units passed syntax checks.
The report explicitly leaves runner qualification and handheld A/B timing outstanding.

## Static VU1: correctness and coverage are separate from speed

A static package was generated from executable-resident programs rather than a recorded route alone.
Residency and byte guards support relocations, partial overwrites and interpreter fallback for unknown spans.
The first conservative backend passed local correctness gates but was substantially slower in one handheld camp test.
Later static-only testing covered sampled camp calls without interpreter tails or declines.
That sampled coverage report explicitly deferred handheld speed and left the final unsampled interval unproved.

## AOT telemetry must count mid-call fallback

Entry refusal counters can read zero while compiled execution falls back inside a call.
An early package lost about 58–59% of gameplay VU1 time that way in the reported coverage investigation.
The fix direction expanded program coverage and added mid-call telemetry.
Static-only sampled camp coverage later showed no such tails.
Neither result establishes zero fallback throughout every unvisited level.

## PGO: keep power, route and baseline attached

A release-0.8 comparison on the tested Proton handheld at 15 W measured combat at 29.38→29.98 fps.
The long combat route measured 29.30→29.99 and driving 29.20→30.00 fps.
At measured 12 W, two repetitions per case gave combat 25.26→28.86 and driving 25.12→29.12 fps.
Those are the same release baseline versus its PGO build, rather than a universal PGO multiplier.
The comparison does not establish whole-campaign performance or results on a different handheld.

## Proton: avoid locking empty completion queues

Sampling the release-0.8.1-based build at 15 W found about 10.5% of EE samples in Wine lock/wake code.
The profile lacked call stacks, so it did not prove the caller attribution.
Follow-up analysis identified repeated empty completion-queue locking as a target under Proton 11.
Acquire empty checks and release publication retained the original producer/consumer mutexes for real work.
Qualified statistics later showed about 99.8% fewer queue mutex pairs in a sampled combat interval.
The draft's 5–9% CPU recovery was a forecast, not a measured gain in this evidence.

## Negative result: scalar scratchpad fast memory

Three accepted interleaved pairs measured 28.16 ms OFF versus 28.23 ms ON per game frame of EE CPU time.
Both variants used the same verified Windows power-throttling opt-out.
This was an exploratory desktop comparison and showed no EE-time gain.
The requested hash gate was nonrepeatable even for identical OFF runs.
The report therefore held the change rather than declaring the optimization qualified.

## Profiling: target more than generated guest code

User-space cycle sampling of a release-0.8.1-based build used three heavy routes at 15 W under Proton.
IOP work accounted for roughly 17–19.5% of EE samples and Wine lock/wake work about 10.5%.
Strict VU1 worker bookkeeping was about 10%; including more glue raises that estimate.
The report did not promote a large pipeline-scheduling rewrite on that evidence.
VIF unpacking and memory movement remained significant targets; samples are attribution, not guaranteed savings.

## SSAA: compare within one build and power envelope

A non-PGO SSAA preview on the tested Proton handheld at 15 W measured combat at 29.2 fps with SSAA off.
The same test reported 27.9, 26.9 and 24.9 fps at 2×, 4× and 8× respectively.
The camp route had a different cost curve and remained at the cap through 2×.
A later PGO build sustained about 30 fps in the tested combat route at 2×.
These results do not imply that supersampling has a fixed percentage cost across builds or scenes.

## Shared package power: presentation can slow the EE

A release-0.8-derived play configuration was compared at measured 12 W with two repetitions.
Disabling high-resolution scanout improved the tested heavy combat segment from 23.8 to 25.4 fps.
Disabling it and using 1× sampling reached 28.6 fps.
Pinning critical work to faster cores reached 25.0 fps in the corresponding tested setting.
Treat these as same-machine configuration comparisons, not transferable percentages or new compiler gains.

## VU1 worker: preserve semantics while moving execution

The release-0.7 record describes moving geometry work onto a VU1 worker thread.
It reports full-rate gameplay on the tested desktop and Proton handheld routes.
Those historical figures combine a release stack and do not isolate threading as the sole cause.
They also do not establish a locked frame rate for all later heavy scenes.
Shipping-thread counter checks and controlled single-thread state checks remain separate requirements.
