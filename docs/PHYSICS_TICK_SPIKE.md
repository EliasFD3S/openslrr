# Spike #203: physics clock, throttle and engine math

Investigation for [#203](https://github.com/EliasFD3S/openslrr/issues/203), based
on `d60b907`, including #205's shared-world collision changes. The
[owner's follow-up](https://github.com/EliasFD3S/openslrr/issues/203#issuecomment-7030114967)
allows changed stock outputs for physically consistent torque/power. This branch
corrects those units and the driving loss path; **handling/output changes are
intentional**. The default clock remains 120 Hz, with no Java/native API change.
The compile-time rate override and
[standalone experiment](../tests/test_physics_tick_rate.cpp) support further work.
This is not completion of the broader thread, throttle, port and fuel redesign.

## Findings and remaining work

| Goal | Finding | Remaining |
| --- | --- | --- |
| Clock decoupling | Fixed 60 Hz host ticks, 120 Hz shared-world substeps and interpolated drawing already exist on the host thread. Catch-up is bounded, not independent of wall-clock stalls. | Independent scheduling requires world ownership and input/output snapshots. A 1000 Hz constant alone loses 4% of simulation time in the current caller. |
| CPU cost | Compare 120 and 960 Hz in the same Valo City drive below. | Interpolation's end-to-end cost is not isolated; no physics-thread prototype. |
| Adaptive setting | Only selects render interpolation; it never selects a physics clock. References listed below. | Removing Classic is a separate UI/compatibility decision. |
| Throttle | Existing torque integration, input filtering, starter, idle, boost curve and clutch exchange must be retained. The issue prototype duplicates some of them and can stick at its limiter. | No plate/airflow prototype or new TUFA hooks in this change. |
| Torque/power | Corrected cycle work to SI net crank torque, derived sampled watts from torque × ω, selected net torque peaks and removed the runtime ×2 gain/duplicate WOT loss. | Stock outputs change with owner authorization. Thermochemistry, friction and stock-car calibration remain empirical. |
| Port flow | Located the squared valve-area/lift volume law in both Inline and Vee scripts. It is an empirical pressure-relaxation coefficient, not geometric optimal port volume. | A physical replacement and stock-engine calibration are not established. |
| Fuel heat | Raw 95-octane H is 1.0995 MJ/kg; effective mixture heat is 1.704225 MJ/kg at the script's optimal AFR 12.5. Heat also sets allowed compression bounds. | A constant-only change can reject stock engines. No Java patch or recalibration here. |
| Stability | Neutral idle/limiter/lift and one synthetic launch are sampled at 120–1000 Hz and five inertias. | These fixtures do not establish stability for every car, collision, terrain, input or rendering cadence. |

## 1. Clock and ownership

[`mainloop_frame`](../src/boot/mainloop.cpp) passes raw elapsed time to
[`Jvm::advance_sim_time`](../src/jvm/jvm.cpp), separately from its clamped
input/camera delta. The VM applies time warp then clamps to 0.1 s.
[`part_physics_advance`](../src/resources/part_runtime.cpp) admits at most 4/60 s
per frame, accumulates that and runs at most four host ticks. Excess time is
**discarded**, not retained as simulation debt. Below 15 FPS, or at fast time
warp, chassis time can lag behind VM/flap time.

| Stage | Current cadence |
| --- | --- |
| Wheel/drive configuration, steering and AI sampling | 60 Hz host tick; inputs were filtered once per frame |
| Suspension, drivetrain, tyres, rigid body, wall sweep, damage, pair contacts, deformation and loose/free bodies | Shared 120 Hz world substeps |
| Hinged-panel solver | 240 Hz from frame simulation time |
| Engine audio | Once per frame that advances physics |
| Render pose publication | Every frame, blended between host-tick poses |

The host-tick accumulator therefore decouples **integration step size** from
render cadence within its catch-up budget; it does not supply an independently
scheduled clock or frame-independent input sampling.

The host owns the VM, GameRefs, physics and bgfx submission by
[contract §6](ENGINE_CONTRACT.md#host-threads-and-the-worker-pool). The shared
collision pass mutates live parts, scene state and damage. Moving only
[`VehicleDynamics::advance`](../src/physics/vehicle_dynamics.cpp) onto a thread
would not decouple the world. A threaded version needs an input snapshot,
exclusive shared-world ownership, published render/native snapshots and a policy
for script mutations/events at boundaries. Cross-car contacts also prevent simply
running the present tick independently for each car.

### Rate changes are not just a constant

`advance_chassis_tick` in [the runtime](../src/resources/part_runtime.cpp) executes
`frequency / update_frequency` iterations with integer division, passing exactly
one solver timestep to each body in each iteration. Thus:

- 120, 240, 480 and 960 Hz divide into complete 60 Hz host ticks.
- **1000 Hz runs 16 × 60 = 960 steps per host second, covering only 0.96 s.**
- Calling the kernel directly with `advance(1/60)` instead would alternate 16/17
  steps, but that is not the production caller after #205. The standalone
  experiment measures both patterns so the kernel result cannot mask the loss.

Other rate-sensitive behavior:

- The predictive limiter band shrinks with timestep.
- With full grip, the clutch applies a 99.9% mismatch-exchange factor per
  substep, not per second; open/slipping auto-clutch engagement scales it down.
- Tyre impulse bounds and up to four 60% positional contact corrections act per step.
- Wall sweeps, pair contacts, damage, loose bodies and publication now scale
  with substeps too, unlike the earlier `0935ac6` draft.
- Chassis settling/upload thresholds scale with frequency; non-car deformation
  still uploads every three substeps (40 Hz at 120; 320 Hz at 960).
- Input, AI and steering filters remain at their existing frame/60 Hz cadence.

Exactly 1000 Hz requires a shared-world scheduler change, not adopting this
experimental override unchanged. The game CMake option therefore rejects rates
that are not positive multiples of 60; the standalone kernel experiment permits
1000 Hz to measure the discrepancy.

### Interpolation and CPU cost

`publish_render_poses` in [the runtime](../src/resources/part_runtime.cpp) is
O(cars/frame), independent of substep count. Consumption also involves
[GameRef checks](../src/resources/gameref.cpp),
[scene hierarchy traversal](../src/resources/scene_instances.cpp) and cameras.
**End-to-end interpolation cost has not been isolated.** Physics profiler time
includes pose publication; its Vehicles subzone does not. The arithmetic
inventory is not timing evidence. Repeated Classic/Adaptive comparisons could
estimate incremental overhead under controlled identical workloads, but none is
claimed here.

**Pre-math-fix rate comparison:** baseline `d60b907` versus the experimental
rate hook at `cdced33`. These are not timings of the later corrected engine.
Windows x64/MSVC Release, 1280×720 window, no VSync, pristine private game copy
reset before **each** run; Baiern `0x6`, Valo City, hour 13, two-second settling
then 40 simulated seconds full throttle (command below). Three runs per rate;
no other builds/tests from this ticket ran during these drives. These are a
shared desktop's measurements, not an isolated machine benchmark.

Each trial contributes 39 completed 60-frame driving windows after the profiler
restart; all loading/pre-drive rows are excluded. Mean milliseconds per frame,
with the range of the three trial means in parentheses:

| Solver | Physics total | Vehicles subzone | Whole frame | Presentation wait |
| --- | --- | --- | --- | --- |
| Base 120 Hz | 2.910 (2.616–3.123) | 2.672 (2.397–2.868) | 9.182 (8.875–9.371) | 3.426 |
| Experimental 960 Hz | 18.163 (17.681–18.922) | 17.825 (17.342–18.582) | 21.117 (20.637–21.914) | 0.036 |

Raising the rate adds **15.25 ms/frame to Physics**, including **15.15 ms** in
Vehicles (6.67×). The collision-preparation subzone stays about 0.05 ms; it does
not include the shared-world pair/wall contact work inside Vehicles. The zones
are nested: do not sum Physics and Vehicles. This world includes traffic,
pedestrians, terrain queries and loose-body work, unlike the one-car kernel test.

The fixed 1/60 benchmark measures throughput, **not** an independent 960 Hz
wall-clock thread. Mean Physics alone exceeds a 16.67 ms host-tick budget here.
Whole-frame growth is smaller than CPU-physics growth because presentation wait
falls; GPU/load noise and rate-dependent trajectories prevent interpreting this
as a controlled isolated interpolation or terrain-query measurement. These data
do not justify raising the shipped default.

## 2. `FRAMERATE ADAPTIVE BEHAVIOUR`

The setting is in the OpenSLRR patch, not either reference MWM/vanilla
OptionsDialog source. References:

| Source | Role |
| --- | --- |
| [OptionsDialog.java](../patch/sl/Scripts/game/src/OptionsDialog.java) | Menu labels, command and renderer option 5 |
| [Patch README](../patch/README.md) | Option table and explanation |
| [natives_script.cpp](../src/natives/natives_script.cpp) | Renderer getter/setter case 5 |
| [host_game_options.hpp](../include/slrr/host_game_options.hpp), [host_game_options.cpp](../src/platform/host_game_options.cpp) | Enum, Adaptive default and persisted `framerate_mode=` key |
| [part_runtime.cpp](../src/resources/part_runtime.cpp) | Only behavioral consumer: Classic skips pose blending |
| [gameref.cpp](../src/resources/gameref.cpp), [gameref.hpp](../include/slrr/gameref.hpp) | Consume the published render pose |
| [test_options_retry.cpp](../tests/test_options_retry.cpp), [validation](../assets/validation/options-retry/after.txt) | Save round trip |
| [test_vehicle_contacts_runtime.cpp](../tests/test_vehicle_contacts_runtime.cpp) | Adaptive setup/restore and passive-car wake interpolation |
| [Engine contract](ENGINE_CONTRACT.md), [knowledge base](KNOWLEDGE_BASE.md) | Physics/options documentation |

Both modes use identical physics. Even with 960 Hz substeps, Classic draws at
60 Hz host-pose cadence. Removal would require recompiling/distributing the
OptionsDialog patch, always blending, ignoring the old persisted key, and updating
tests/docs. Preserve option 5 returning Adaptive (1) for older patched callers;
dropping its getter case would report Classic (0). No removal is made here.

## 3. Throttle integration boundary

The current chain is:

1. [Controller input shaping](../src/platform/host_input.cpp) applies mapped axis
   smoothing once per frame. This is not exactly the prototype's constant digital
   slew, and does not inherently bypass filtering for analog pedals.
2. [`configure_drive`](../src/resources/part_runtime.cpp) supplies pedal, idle,
   limit, inertia `0.28 + 0.01 × max(0, engine_inertia)`, starter and the dyno torque
   sampler (nitro 0) once per host tick.
3. [`step_drive`](../src/physics/vehicle_dynamics.cpp) now recovers indicated
   torque as `net_table_torque + F`, applies pedal/idle/combustion/limiter gates,
   then subtracts shared mechanical friction F and incremental closed-throttle
   pumping `D × (1 − gas)`, where `D = 14 + 0.012 × max(0, rpm − idle)`.
   Starter torque and the crank/wheel clutch impulse exchange remain unchanged.
4. [HUD/audio](../src/resources/part_runtime.cpp) consume live rpm and pedal.
   Turbo gauge response is `dyno_boost_fraction × pedal`.

A plate/airflow state should be evaluated inside the existing substep drivetrain,
not in a second crank integrator. The issue prototype's external load-torque
argument does not match the current clutch impulse exchange. Its full-load limiter
sets WOT torque to zero while weighting drag by `1 − load`; with load 1 and no
external load, rpm cannot fall below redline. Its proportional-only idle controller
also settles below target. Keep existing start/stall/idle/loss behavior explicit.

The sampled dyno curve already includes boost. `dyno_boost_fraction` is normalized
boost response, **not absolute manifold pressure ratio**; multiplying torque by
another pressure curve would double count boost. A BOV event must be latched once
across substeps until the host consumes it, not reset or retriggered on every step
whose plate derivative crosses the threshold.

No named live rpm/plate/manifold/BOV getters were found in the reviewed Chassis
reference declarations or [pinned native census](../assets/progress/stock-natives.json).
Pedal input is already available through `Controller.user_GetAxisVal` and
`Input.AXIS_THROTTLE`; pedal is not plate position. New native registrations alone
cannot make unchanged TUFA call undeclared methods. A mod-facing API needs shipped
declarations/callers and explicit units/lifetime/event semantics; internal HUD/audio
wiring does not require a new Java API.

## 4. Engine/dyno audit

### Torque and power

The [cycle model](../src/physics/dyno_model.cpp) uses 720 crank degrees
(`period = 120/rpm`). For work W per cylinder and N cylinders:

| Quantity | Legacy output | Corrected output |
| --- | --- | --- |
| Indicated torque | N·W/8 | N·W/(4π) |
| Gross power | N·W·rpm/60 | N·W·rpm/120 |
| Net crank torque | Table torque had no mechanical loss subtraction | N·W/(4π) − F |
| Net power | Gross W − rpm³ × 2·10⁻⁸ | Net torque × ω |
| Horsepower conversion | net W × 0.001341 | Unchanged mechanical-hp conversion |

F is the shared recovered mechanical loss `rpm² × 2·10⁻⁸ / (2π/60)` in Nm,
well-defined at zero rpm. The legacy gross-power/table-torque ratio was 4/π,
and driving doubled table torque before subtracting another independent drag.
That gain is removed. In normal running WOT (no idle assist, ramp complete,
no limiter or shift cut), the crank now receives exactly the sampled **net**
curve. At part throttle the model reconstructs indicated torque, scales combustion,
then subtracts F once and the existing incremental closed-throttle pumping loss.
F remains active during fuel cut. These are still empirical loss/linear-pedal
curves, not a validated physical manifold model.

Watts are derived **after** torque interpolation and nitrous blending, so
`P = T·ω` holds between nodes too. Net torque peaks are selected by net torque,
not by mean cylinder pressure. Peak horsepower/rpm stats still refer to table
nodes, not an analytic search for continuous-curve extrema. Native `getHP`
continues to return watts; `maxHP` remains horsepower. Java schemas, compression
gates and saved fuel/AFR semantics are unchanged.

The old 437 rpm witness (82.0871 Nm, 4781.27 W) now anchors recovered **cycle
work**, not the old output scales. Its SI-converted result is about 52.2220 Nm
and 2389.81 W. This deliberately changes original-game behavior with the owner's
permission; neither version proves calibration for every MWM engine. The
[research provenance and endpoint disagreement](EXTERNAL_RESEARCH.md#4-game-mechanics-and-telemetry)
remain relevant (Steam build 940 evidence versus target MWM).

### Port-volume/flow law

Both reference source trees have the same relevant formulas in
`parts/scripts/enginepart/block/SRC/Block_Inline.java` (122–132) and
`Block_Vee.java` (150–160 and 224–234). Heads compute
`X = valve_disk_area_cm² × lift_cm`; blocks use
`c = 0.9 × efficiency × X² / cylinder_volume_cm³`, clamped to 1 for maxima.
Minima divide by efficiency. In [the cycle](../src/physics/dyno_model.cpp), c is
used as `min(1, c × dt × 1000)` pressure relaxation, not geometric optimal volume.

The formula has implicit unitful calibration, grows with lift², and at fixed
port/cylinder volume ratio grows with cylinder size. For wear/efficiency 1,
two 33 mm valves, 10 mm lift and 417 cm³ give c = 0.6315; the clamp needs
12.58 mm lift. This is algebra, not a measured stock head.

Replacing it with `k × efficiency × X/V` is dimensionless but not automatically
physical. Matching one old head requires `k = 0.9 × X`; a single k cannot preserve
all heads/lifts. A real flow model needs valve curtain area, discharge coefficient,
pressure/density dependence and calibration. No verified intended replacement was
found; a Java formula change must ship as a compiled [patch](ENGINE_CONTRACT.md#java-changes),
not merely edit the private game copy.

### Fuel heat and compression coupling

In both reference `parts/scripts/enginepart/SRC/AirFuelDeliverySystem.java`
(84–147), raw H is 1,099,500 J/kg (95), 1,100,000 (98), 1,100,250 (100),
1,400,000 (methanol) and 1,600,000 (diesel). The transformed mixture-match factor
then gives `mixture_H = H × (0.8 + 0.2 × match) × 1.55`.
At optimal 95 AFR **12.5**, effective heat is **1.704225 MJ/kg**.

The cycle applies that heat to charge mass, not just fuel mass. For an illustrative
fuel LHV of 43.4 MJ/kg (an assumption, not a measured fuel specification), the
matching mixture value is `43.4/(12.5+1) = 3.2148 MJ/kg`. The often-quoted 2.89
MJ/kg instead uses AFR 14. Neither is a universal replacement for every fuel.
The issue's 1.09 MJ/kg screenshot quotes raw H before the ×1.55 factor.

Crucially, the same Java code sets `minComp = mixture_H/250000` and
`maxComp = mixture_H/100000`. `FuelInjectorSystem.java` (126–140) copies these
into DynoData; `Block.java:isDynoable` (344–350) rejects compression outside them.
Raising heat from 1.704 to 3.215 MJ/kg raises minimum allowed compression from
6.82 to **12.86**, potentially rejecting stock engines. This needs explicit gate
recalibration/decoupling, not a constant-only patch.

The cycle also assumes cv = 712 J/(kg·K), γ = 1.3, and no modeled dissociation.
Changing heat alone does not establish realistic combustion efficiency. Its
`kEfficiency` is charge mass / swept mass (volumetric), not thermal; the screenshot's
“300% efficiency” cannot be assigned to a specific game stat from this evidence.

MSVC Release-style (`/O2`) public-API sweep, default geometry/timing, maxRPM 7000,
28 samples, **AFR held at 12.5 in every row** (not a stock-car calibration):

| Effective heat | Peak table torque | Peak table power | P/(T·ω) at power peak |
| --- | --- | --- | --- |
| 1.000 MJ/kg | 108.58 N·m at 1750 rpm | 33.76 kW at 4250 rpm | 1.0000 |
| 1.704225 MJ/kg | 188.62 N·m at 2000 rpm | 63.18 kW at 5750 rpm | 1.0000 |
| 3.214815 MJ/kg | 347.22 N·m at 2000 rpm | 128.20 kW at 6250 rpm | 1.0000 |

Changing only heat from the second to third row raises peak net torque by 84%.
The converted cycle-work witness passes (52.2220 N·m / 2389.81 W at 437 rpm).
This sensitivity experiment does not validate a replacement combustion model,
thermal efficiency, or any actual stock engine's output.

## 5. Stability experiment

The [standalone fixture](../tests/test_physics_tick_rate.cpp) uses a synthetic
235 N·m peak table curve, 800 rpm idle, 7000 rpm limit, neutral idle for 3 s,
full throttle for 3 s, then lift for 7 s. It samples every kernel step at five
flywheel inertias (0.10, 0.22, 0.28, 0.38 and 0.60 kg·m²), plus one automatic
rear-drive launch on flat analytic ground. The host's 0.28 floor means the two
smallest inertias are intentionally below normal game configurations.

MSVC `/O2`, corrected net-torque integration, eight checks passing at each
of the five compiled rates:

| Rate | Inertia kg·m² | Idle ripple ± rpm | Peak / steady limiter band rpm | 6 s launch km/h |
| --- | --- | --- | --- | --- |
| 120 Hz | 0.10 | 9.7 | 7187 / 6994–7186 | 86.28 |
| 120 Hz | 0.28 | 3.2 | 7067 / 6998–7066 | |
| 120 Hz | 0.60 | 1.6 | 7030 / 6999–7030 | |
| 240 Hz | 0.10 | 5.0 | 7093 / 6996–7093 | 87.62 |
| 480 Hz | 0.10 | 2.5 | 7047 / 6998–7047 | 87.60 |
| 960 Hz | 0.10 | 1.2 | 7023 / 6999–7023 | 87.58 |
| 960 Hz | 0.28 | 0.4 | 7008 / 7000–7008 | |
| 1000 Hz | 0.10 | 1.2 | 7022 / 6999–7022 | 87.60 |
| 1000 Hz | 0.60 | 0.2 | 7004 / 7000–7004 | |

All 25 neutral rate/inertia combinations remain running; mean idle is within
0.4% of target. Peak limiter overshoot is bounded by one acceleration step in
these checks. Measured time to 95% of limit is 0.290–1.742 s, and lift back to
2000 rpm takes 0.94–5.63 s depending on inertia. Higher rate tightens resolution;
it does not prove that the existing 120 Hz integration is generally unstable.

Additional checks cover one-step WOT/part-throttle net torque, limiter friction,
starter/restart and zero-engine states, sampled/nitrous power identity and net
peak selection. The fixture also prints timing for a 20-s one-car analytic-ground
workload; it includes per-step call/callback overhead, excludes production terrain
and shared-world bookkeeping, and is not a portable performance guarantee.

The existing standalone vehicle-dynamics regression could **not** be linked on
the unmodified base: MSVC LNK2019 reports missing `tire_effective_load`, declared
in [tire_model.hpp](../include/slrr/tire_model.hpp) but not implemented under
`src/`. This pre-existing defect is not repaired by this spike. New experiment
checks passing do not mean that the existing regression suite passes.

These are bounded fixture results, not universal numerical stability or
original-game compatibility. They exclude mixed dynamic contacts, full terrain,
render-rate/input variation, stalls/catch-up, time warp and the issue's unintegrated
throttle prototype. The 1000 Hz kernel result does not validate its game scheduler.

## 6. Remaining contract/design work

The output-compatibility question is resolved: the owner explicitly permits
changed torque/power, and the focused C++ correction is implemented. It is not a
newly calibrated stock engine, manifold, fuel or port-flow model.

The remaining physical port/fuel work cannot be a local coefficient fix. The
Java head→block→DynoData chain clips and averages empirical coefficients before
C++ sees them, losing valve dimensions; a curtain-flow model needs a new input
contract across head/block scripts and the native cycle. Fuel heat also changes
saved AFR/fuel behavior and driveability through compression gates, so values
cannot be rescaled blindly in C++ without fuel identity. No verified replacement
model/calibration was established in the inspected references.

Independent-thread ownership likewise needs a coordinated shared-world/VM
mutation and publication contract; moving the current live-GameRef tick locally
would be unsafe. The throttle-plate/TUFA prototype and isolated interpolation
measurement remain incomplete. No default-rate, Classic-mode, Java or native API
change is made here; these remaining goals are not marked completed.

## Reproduce

From an x64 Native Tools Command Prompt (one executable per rate):

```bat
cl /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /Iinclude /Itests /DSLRR_VEHICLE_PHYSICS_HZ=960 tests/test_physics_tick_rate.cpp src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp src/physics/suspension_geometry.cpp src/physics/dyno_model.cpp /Fe:build\tick_960.exe /Fo:build\
build\tick_960.exe
```

Repeat with 120, 240, 480 and 1000. The shared-world check intentionally reports
the non-multiple's lost clock time; passing it is not an endorsement of that rate.

Configure the game with `-DSLRR_VEHICLE_PHYSICS_HZ=960`, then follow the normal
[Release build recipe](../README.md#build). A fresh configure defaults to 120;
explicitly set 120 to restore the shipped rate in a reused build directory.
All host translation units must use the same value. Use a private,
reset game copy for each benchmark:

```powershell
.\build\Release\slrr_host.exe --game --root data\game --window --no-vsync `
  --spawn-car cars/racers/Baiern.rpk:0x6 --bench-map ValoCity `
  --bench-drive 40 --bench-time 13 --perf-log build\city.csv
```
