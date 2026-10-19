# Findings and rejected approaches

## What improved the port

The VU1 worker moves geometry off the EE thread while preserving ordered packet delivery. Load batching reduced queue overhead; compiled-code coverage fixes removed mid-call interpreter work. These are complementary: a worker cannot make an expensive geometry program cheap. See [Performance](PERFORMANCE.md). 

| Compiler work | Finding |
|---|---|
| Round 4  | Expanded compiled program coverage, including loading vortex; desktop test system gameplay 17.4 fps  |
| Round 5  | Gameplay improvement 11–15%  |
| Round 7  | Cine VU execution 1.12× faster; Ruined Temple 1.05×  |
| Round 8  | AVX2 used 5.5% fewer cycles; AVX-512 was 3% slower in the reported experiment  |
| Candidate integration | Runtime instruction-set dispatch; AVX2 default, SSE fallback, AVX-512 opt-in. Whole-route VU time fell 44% on desktop test system cine and 40% on Legion Go 2 cine.  These are lane measurements, not new universal fps claims. |

Early native preview cost claims were promising but incomplete: later replay calibration excludes shared passes and uncaptured families, and low-power game tests expose EE preparation overhead. Exact fixture output does not prove correct full-scene scale, lighting or layering. 

## What did not justify promotion

- Multiple VU workers have limited benefit: the frame's geometry is a long dependency chain, with an estimated ceiling around 1.4×; extra threads do not imply lower power. 
- Reusing geometry across frames saves little with a moving camera. GPU-resident VU execution was dropped: the prototype was 13× slower and would compete for handheld GPU power with native drawing.  Native drawing still needs CPU geometry state because later batches consume earlier outputs. 
- The separate vortex package was not promoted into v0.7.1. Registry evidence records exactness passes but discards contended timings and flags possible load regression; it does not prove a speedup.  Treat “dropped” as excluded from this candidate, not a claim that the algorithm can never work.
- True 60 Hz simulation was benched: vehicle launch distance differed by about 21%, because the wheel/engine integrator behaves differently at the smaller timestep.  FIELD60 presentation is a separate feature and does not double game logic.
- At 35 W, Zen 5c-only affinity held about 29.8 fps at about 21 W; at 15 W, fast Zen cores and hybrid placement won.  This is workload-specific, not a universal affinity rule; later live bridge pinning gained at most about 1 fps. 
- Legion native 2x performance is limited by EE throughput/preparation. EE work was 100% in cine and 97–98% in forest; disabling native grass recovered forest from about 18.1 to 22.7 fps.  A manual GPU clock of 1600 MHz gave identical performance.  Increasing GPU clocks alone did not solve it.

## PCSX2 adoption and disc-only geometry compilation

PCSX2 is a semantics reference and a source of credited, license-compatible improvements, not a claim that its complete renderer was adopted. VIF unpack accuracy was cross-checked against its model; the chosen emulated GS path is parallel-gs. The roadmap considers sound, MTVU hand-off and microVU analysis ideas; acceptance needs individual gates.  Sprint input scaling follows the same principle as PCSX2's stick scaling. 

Static extraction from the user's disc is implemented for the supported ELF. The index selects 69 ranges including registration/descriptor candidates and a dedicated context program; overlapping ranges can share the same program hash. It builds partial-upload window records from verified ELF bytes. The independent static generator compiles relocatable guarded spans without recorded images or captures. Whole-image recorded packages still need independently derived compositions and identity guards; this source distribution does not recreate the private 50-image package. See [setup](SETUP.md), [VU1 integration](VU1_AOT.md) and [current findings](RESEARCH.md).
