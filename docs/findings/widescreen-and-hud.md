# Widescreen and hud

Projection, continuation handling and widget classification require separate checks.

## Projection: separate camera extent from display ratio

The revised projection uses a horizontal reference at least as wide as 16:9.
The actual display aspect supplies a separate projection divisor.
At 16:10 this retains the 16:9 horizontal extent and expands vertical extent by 10/9.
Above 16:9 the factors coincide with the previous Hor+ arithmetic.
This describes the implemented hook model; the original report still required visual and culling qualification.

## Viewport correction must survive yields

A guest viewport routine overwrote an already adjusted perspective value.
The old wrapper corrected it only when the initial host call returned to its caller.
Cooperative yields resumed through separately registered aliases and bypassed that completion step.
The fix wraps all 16 continuation aliases and checks caller return address and stack pointer before correcting.
HUD temporary-state restoration needed the same distinction between host unwind and true guest completion.

## Wider culling increases submitted work

On the long forest test, widening the culling frustum increased in-level VU1 microprogram submissions.
The measured increases were 10.53% at 16:10, 10.40% at 16:9 and 20.78% at 21:9.
These are MSCAL submission-count changes, not draw-call counts or measured CPU/power costs.
Several non-cycle counters remained identical while submitted geometry changed as intended.
Wider visibility therefore needs its own correctness and performance comparison.

## Player armor: include partially visible tagged geometry

The player gauge had a separate boundary-classification defect.
Some full-armor segments extended slightly beyond the shared bottom guard and escaped the widget transform.
The proposed guard accepts sane tagged primitives whose bounds intersect the screen.
Entirely off-screen or invalid geometry is still skipped.
That report alone does not establish complete game qualification; it records incomplete gates and captures.

## Boss gauge: track the whole widget before damage clipping

The boss gauge bypassed the player-widget tracker.
Clipping damaged fill strips changed their bounds and could give them a different anchor from the frame.
The fix tracks the boss widget and applies one shared transform rather than classifying each clipped strip alone.
Offline tests reduced a synthetic 22.4-pixel gap to zero; those were not live measurements.
Later integrated captures reported a 50-pixel boss-fill displacement reduced to zero at 16:10.
The 4:3 comparison was identical; the tested 21:9 fraction was aligned in both builds.
