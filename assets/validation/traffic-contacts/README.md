# Shared contacts: application evidence (#197)

These are **actual bgfx backbuffer captures**, not mockups or desktop captures.
Both runs load the same complete `final-CareerQA-2` save through the full script
runtime, enter Valocity, settle for two simulated seconds, then run the existing
full-throttle benchmark. The player car is the blue Einvagen 110 GT.

Scenario: **1280×720, daylight at 13:00, same chase camera, HUD/GPS, options and
initial save state**. Before/After/trace input trees were independently copied
and all 8,462 files (535,972,931 bytes per tree) verified SHA-256-identical before
launch. No original save or reference installation was changed.

## Before

Engine baseline: **`d1c719d18d6e76db0f3d6b2a8ff54b44a820bdfd`**.

![Before — baseline d1c719d, benchmark start](<before-start.png>)

## After

Production contact implementation: **`ca7e4fcb70d50312a9ff731fe412efde6a27680c`**.
Later evidence/documentation commits do not change that production code.

![After — ca7e4fc contact implementation, benchmark start](<after-start.png>)

Both 12-simulated-second application runs reached `drive done` and exited 0.
At [Before +10 seconds](<before-10s.png>) and
[After +10 seconds](<after-10s.png>) the car has reached the same static tree.
That is preserved driving/static-contact smoke, **not vehicle-pair proof**.
The start captures intentionally show the same scenario, not an invented visual
redesign or a claim that a car-to-car collision happened in the screenshot.

## Capture instrumentation and provenance

The captures use diagnostic executables with the same
[read-only gate logging/backbuffer request patch](<capture-instrumentation.diff>)
on each source revision. It prints state at 2/10/30 wall seconds and requests an
additional real screenshot; it does not change the gate, time, control, physics,
Java, placements or AI. Normal benchmark `t00`/`t10` are shown above.

| Executable | SHA-256 |
| --- | --- |
| Preserved baseline, no diagnostics | `9ae04306ae7c69288bb17606b46e5e0c12d9d69250d7430368ddbf813359b736` |
| Baseline + capture diagnostics | `4cb141349cff146a85aba744aa1a965e8b5c8a74651d173707625c5c35e83f31` |
| Preserved production contact build | `61305b1c0cfde26c3eea5002f985c5af0921cee64e30a11bcbdcd532e7adda41` |
| Contact build + same capture diagnostics | `e38d2979920f96932c2f9a34cef0ec00c877120e654255638154427ef8195637` |
| Contact trace diagnostic build | `3348eb8e0a5a415ca939e3aa16f8ccda301a9f5ada538a4cdcca83c01ce91a64` |

## Actual full-career contact observation

A third identical private input copy ran **60 simulated seconds**, naturally,
through the same full script-loaded career and exited 0. The extra
[bounded printf-only observer](<contact-trace-instrumentation.diff>) recorded
[23 accepted AI/AI contact steps above 1,000 N·s](<natural-contact-steps.txt>), all
with actual native traffic type `0x1a0006`. The first normal impulse was
5,504.699899 N·s at 8.200541 m/s relative normal speed. Observed normal impulses
were 1,039.545665–5,557.582260 N·s; no numerical-recovery event was logged.

No cars were repositioned, spawned for the test or given replacement AI. These
are contact solver steps, **not 23 distinct crashes**, player-impact footage or
complete race acceptance. Retained player-versus-AI, mechanical/native mixtures,
props, damage and lifecycle cases are separately covered by the runtime suite.

The [raw frame-stage timings](<career-60s.csv>) include startup and 59 complete
60-frame measurement windows. Across those windows mean physics was 4.041 ms
(max window mean 5.321 ms), vehicles 3.666 ms and traffic 0.021 ms. This is an
inspection at the copied profile's settings on a shared host/GPU, not a controlled
FPS comparison, allocation-churn profile or original-game performance guarantee.

The initially supplied incomplete career lacked its car file; another complete
QA save entered the city but did not reach the automatic benchmark gate. Those
attempts are not asserted as successful or diagnosed beyond their evidence.
QA2 demonstrably reached the benchmark with a controlled car, ground props and
no loading/transition gate. Full day/night/ROC challenges/results/returns,
audio, police presentation and remaining #197 items are still not accepted here.
