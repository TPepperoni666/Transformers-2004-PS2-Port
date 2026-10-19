# Runtime bugs

Recorded failures and fixes. Validation limits are stated where the source reports remain incomplete.

## EE: callback stacks and special memory

Interrupt and callback stacks overlapped the main guest stack, corrupting return addresses.
The stack allocator was changed to avoid guest stacks and runtime pools.
A separate address classifier excluded parts of VU memory, sending accesses into ordinary RAM.
The classifier was expanded to cover all four VU memory regions.
The rebuilt startup route passed the former crash point without the temporary copy workaround.

## EE: formatting and cooperative dispatch

The runtime needed EE-specific decoding of 64-bit floating-point variadic arguments.
The source records a guest printf correction, without the draft's claimed standalone test suite.
A separate branch-dispatch fallthrough occurred when a callee yielded at a checkpoint.
A scheduler yield counter lets dispatch distinguish that unwind from ordinary completion.
The reusable lesson is to preserve guest continuation state across host call returns.

## EE: floating-point translation

Invisible geometry was traced to bad matrices produced by EE translation.
SQRT and RSQRT used incorrect operands; division by zero and float-to-integer conversion also differed.
VU0 macro reciprocal square root likewise ignored its numerator.
Corrected translation produced finite matrices and visible geometry in the character-selection scene.
Offline VU replay showed that the suspected missing VU stores were actually committing correctly.

## EE: duplicated fast stores

Four wide RAM-store helpers copied the value and then fell into a byte-store loop.
The final bytes could still look correct, so a final-buffer comparison alone missed the extra work.
Returning immediately after the fast copy removes the duplicate write.
The correction preserves the surrounding tracing and bookkeeping order.
This was a helper-control-flow defect, rather than evidence of every memory optimization being faster.

## DMA/VIF: completion and guest-visible registers

The startup frame loop waited for a VIF1 interrupt and checked the DMA end address.
Missing interrupt delivery, missing address writeback and unreadable VIF registers triggered a three-second watchdog.
The fix queued the interrupt, updated the terminal DMA address and exposed live VIF registers.
Startup then passed the memory-card screen at a reported 50–60 fps.
That startup result is not a gameplay frame-rate measurement.

## DMA/VIF: DIRECT continuation

Texture corruption came from treating bytes after one DIRECT as image payload too early.
The next DMA tag's VIF commands were not decoded before consuming that payload.
Uploads were consequently shifted by eight bytes.
The correction continued DIRECT parsing across transfer boundaries; it was not an eight-byte alignment rule.
The subsequent rebuilt run showed a clean title/menu backdrop.

## DMA/VIF: packet lengths and capacity

The packet-builder HLE doubled DMA-tag quadword counts, producing black globe and options screens.
Correcting its current-pointer accounting fixed that distinct failure.
Another limit truncated large chains on the equipment screens.
Raising the chain cap from 4,096 to 262,144 quadwords fixed those screens.
The reports do not support the draft's invented transfer-size census or exhaustive inventory test.

## DMA/IPU: raw movie input

The mission preview used raw MPEG-2 elementary-stream input through toIPU DMA.
Ignoring channel 4 left the decoder without pictures and led to a fatal movie path.
The fix fed guest data into the active elementary-stream decoder and completed DMA bookkeeping.
Movie callbacks also had to continue after disc EOF for that stream mode.
The later warp-transition interrupt wait was a separate VIF1 failure, not proof of a toIPU spin loop.

## GS: field masking and disabled depth tests

The level combined two fields using scanline masks.
Ignoring SCANMSK allowed the second field to overwrite every line.
The software rasterizer was corrected to skip the appropriate even or odd lines.
Disabled depth testing also needed to avoid depth testing and depth writes, rather than reject clears.
Bad second-field matrix inputs remained a separate issue at this investigation stage.

## GS: sprite orientation and sample positions

Glyph sprites supplied their corners in reverse order.
Swapping positions without swapping texture coordinates rotated the glyphs.
Discarding the fractional XYOFFSET and sampling at pixel centers also misaligned texture strips.
The software path retained the full offset, used integer sample positions and swapped UVs with corners.
Triangle coverage gained a top-left rule; no invented seam-quality score is retained.

## VU: raw MIN/MAX values and lighting

The VU interpreter normalized denormal operands before MIN/MAX selection.
The game also used these instructions to copy integer-encoded vertex colors.
Normalization erased those colors, producing black models despite valid geometry.
The correction selects using raw encodings and returns the selected raw operand.
Offline replay recovered color, and the following character-selection run showed lit models.

## IOP/SIF: handler context and service timing

SIF command handlers needed the global pointer captured at registration.
Module-loading stubs also needed to deliver queued SIF commands before returning.
An audio tone-bank deadlock was associated with execution outside a real IOP thread context.
The recorded repair used a larger synchronous RPC budget; the proposed RPC-on-thread approach was rejected.
These are separate context and scheduling fixes, not proof that unrestricted asynchronous IOP execution is safe.

## Audio: worker timing can alter PCM

Identical reference executables produced different PCM with the shipping VU worker mode.
A volume-register write crossed a synthesis boundary by four IOP cycles.
The investigation reproduced both waveforms without changing mixer arithmetic.
Controlled worker-zero runs repeated complete PCM and timestamped SPU2 input records.
Fixed-clock infrastructure alone did not establish RTC causality or shipping-mode repeatability.

## Persistence: loading and setup generations

A warp-loading crash was fixed by stopping the loading screen from wiping the heap.
The source does not identify this as a memory-card heap failure.
A separate distribution problem stranded saves across setup-generated build directories.
Later public rehearsals tested an Options save, another setup generation and a positive card read.
That establishes settings persistence for the rehearsal, not full campaign save compatibility.

## Host diagnostics: a test-induced boss freeze

A boss test enabled verbose HUD diagnostics around the first hit.
Character-by-character stderr output blocked the EE in a host file write for over 60 seconds.
The play environment did not enable those diagnostics.
The fix assembled each line in memory and issued one buffered write operation per line.
Initial fixed boss trials passed; later integrated counter comparisons also passed.

## GS: incomplete pipeline-cache snapshots

The Vulkan cache-data query can return VK_INCOMPLETE while its snapshot changes.
The compatibility patch returns a retryable failure to the existing bounded retry loop.
Other read errors still retain their error reporting.
A final observed run returned the incomplete result and then saved the cache successfully.
The source does not establish the draft's claim of corrupted cache files.

## Presentation: asynchronous screenshot encoding proposal

PNG encoding was synchronous inside frame-upload and periodic export paths.
The prepared patch copies pixels into a single ordered encoder queue.
The queue is bounded and rejects overload instead of blocking or growing indefinitely.
Synthetic image tests established encoder equivalence and owned-buffer lifetime.
The report explicitly leaves real game screenshots and counter qualification pending.

## EE: second-field arithmetic correction

A second-field failure was traced to host SSE subtraction on a non-finite operand.
The recorded repair replaced the affected guest routine with PS2-specific arithmetic.
This was a targeted correction, not a demonstrated universal bit-level FPU implementation.
The earlier invisible-geometry investigation also found separate operand-selection and conversion bugs.
Keep these causes distinct when comparing field rendering and transform failures.
