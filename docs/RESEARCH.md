# Rendering and execution findings

## Static VU1 from the disc

An offline scan inspected 5,756 files including archive members, about 3.18 GB, and found no VU1 microcode outside the supported boot ELF. Constant registrations identify executable ranges; some registered programs have not been observed executing. This supports disc-only generation, but does not prove every entry/combination is fast or that every level has been played. The detector compares actual upload bytes against verified ranges. Missing `vu1known.bin` means coverage is unavailable, not zero unknown code.

The first static experiment ran at about 5.4 game fps at the camp. Reworked dispatch and guarded multi-pair spans reached 28.87–29.00 with the recorded package; a standalone disc-only experiment reached 27.0–27.1 at 15 W with exact counters on six gates. This repository supplies the independent static generator and optional integration. It does not supply or reproduce the private 50-image recorded package. Full fresh-disc gameplay and static integration still require local qualification.

## Ultrawide culling

Widening the projection does not widen all visibility tests. Scene traversal, ecological quadtree and sub-scene/portal systems derive separate frusta from the camera's FOV and half-angle tangent (`camera data +0x8/+0xC`). The projection hook changes the perspective builder at guest address 0x20CA00, while visibility builders at 0x1F4B00, 0x20AB70 and 0x2B89C0 can retain the original narrow frustum. Objects in the added side bands are rejected before drawing, then pop in when the camera turns. Render-only culling must be widened without changing physics, targeting, AI priority or size/LOD factors. v0.8.3 imports the title-specific culling and HUD/yield fixes. Their visual playtest coverage is separate from the exact fresh-card counters.

## Reusing VU1 results

No useful exact-input repeats were found among 23,857 within-frame calls; tiny initialization stubs represented about 0.1% of VU1 time. Moving-camera cross-frame repeats were 0.0–0.1%. A still camera offered 23–34% potential reuse, but checking inputs costs about 3–5% of a call and standing-still results do not represent moving gameplay. The HUD goes directly through graphics packet paths rather than VU1. No replay cache was accepted.

## Controlled state-hash gate

Shipping threaded VU1 hashes are not repeatable even for two identical disabled runs. For an AOT on/off state-hash comparison, use the same HASH-only observer and executable on both sides:

```
PS2X_VU1AOT_HASH=1
PS2X_VU1_THREAD=0
PS2X_KERNEL_HEAP=0
PS2X_GLYPHS=off
PS2X_VU1NEW=0
```

Disable perfscope/captures and other observers for this comparison. Accepted v0.7.3 comparisons matched 47/47 cinematic and 169/169 Antarctica/warp/hub checkpoints. Counter gates still use shipping threading and heap settings; the controlled hash configuration does not replace them. Exact counters and hashes do not establish visual or audio fidelity.
