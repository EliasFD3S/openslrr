# Spike #203: physics clock, throttle and engine math

Investigation for [#203](https://github.com/EliasFD3S/openslrr/issues/203), based
on `d60b907`, including #205's shared-world collision changes. The
[owner's follow-up](https://github.com/EliasFD3S/openslrr/issues/203#issuecomment-7030114967)
allows changed stock outputs for physically consistent torque/power. This branch
corrects those units and the driving loss path; **handling/output changes are
intentional**. The default clock remains 120 Hz; native APIs are unchanged.
The Java port-flow and fuel-heat patches described here were withdrawn in
[#352](https://github.com/EliasFD3S/openslrr/issues/352): see
[Stock calibration](#stock-calibration-352).
Following the [maintainers' review](https://github.com/EliasFD3S/openslrr/pull/215#issuecomment-7069913565),
physics now steps on its own thread ([§1](#physics-thread)). The compile-time
rate override and [standalone experiment](../tests/test_physics_tick_rate.cpp)
support further work. New default controls are not part of this branch.

## Findings and remaining work

| Goal | Finding | Remaining |
| --- | --- | --- |
| Clock decoupling | Fixed 60 Hz host ticks and 120 Hz shared-world substeps now run on a physics thread, in one batch per frame, while the host draws the frame; the host prepares and publishes each batch. Catch-up is bounded, not independent of wall-clock stalls. | The batch overlaps only the frame's drawing, not scripts. A 1000 Hz constant alone loses 4% of simulation time in the current caller. |
| CPU cost | Compare 120 and 960 Hz in the same Valo City drive below; the new `interpolation_ms` subzone measures actual pose publication. | Pose-reader/scene-cache overhead is not isolated. |
| Adaptive setting | Only selects render interpolation; it never selects a physics clock. References listed below. | Removing Classic is a separate UI/compatibility decision. |
| Throttle | A spike-only digital/analog shaping, plate/airflow, hysteresis and latched-BOV prototype runs inside the actual solver's high-frequency test loop, retaining the existing crank/idle/clutch. | It is not enabled in production controls; stock TUFA lacks the named telemetry declarations, and boost fraction is not absolute manifold pressure. |
| Torque/power | Corrected cycle work to SI net crank torque, derived sampled watts from torque × ω, selected net torque peaks and removed the runtime ×2 gain/duplicate WOT loss. | Stock outputs change with owner authorization. Thermochemistry, friction and stock-car calibration remain empirical. |
| Port flow (withdrawn, #352) | Replaced squared lift-volume normalization with capped valve-curtain area × reference exchange speed × 1 ms / swept volume, shared by Inline and both Vee banks. Compiled script patch is included. | This is a declared nominal-flow normalization, not a full compressible valve solver or measured stock calibration. |
| Fuel heat (withdrawn, #352) | Stock raw 95-octane H was 1.0995 MJ/kg, 1.704225 MJ/kg after a ×1.55 factor at the script's optimal AFR 12.5. The patch uses lower heating value / stoichiometric AFR per kg of charge (gasoline 2.952 MJ/kg) without the factor; the compression window keeps the stock values. | Values are handbook fuel data, not a combustion calibration; dissociation and variable heat capacity are not modelled. |
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
| Wheel/drive configuration and AI sampling | Once per frame, for the first host tick |
| Steering filter | 60 Hz host tick, on the physics thread after the first tick |
| Suspension, drivetrain, tyres, rigid body, wall sweep, pair contacts and loose/free bodies | Shared 120 Hz world substeps, on the physics thread |
| Damage, crush cages, detaching and prop promotion | Once per frame at the commit, in solver order |
| Hinged-panel solver | 240 Hz from frame simulation time |
| Engine audio | Once per frame that advances physics |
| Render pose publication | Every frame, blended between host-tick poses |

The host-tick accumulator therefore decouples **integration step size** from
render cadence within its catch-up budget; it does not supply an independently
scheduled clock or frame-independent input sampling.

### Physics thread

The shared collision pass used to mutate live parts, scene state and damage
from inside the substeps, so moving only
[`VehicleDynamics::advance`](../src/physics/vehicle_dynamics.cpp) onto a thread
would not have decoupled the world. The tick is therefore split in three
([contract §6](ENGINE_CONTRACT.md#host-threads-and-the-worker-pool)):

1. `prepare_batch` (host, before `render.flush()`): count the host ticks owed,
   rebuild scene collision, configure each car for the first tick
   (`configure_drive`, wheels, body supports, external placement), and copy
   every car, loose body and free PhysicsRef body with the inputs it needs.
2. `simulate_batch` (physics thread, during the flush): all ticks and shared
   substeps, cross-car pairs and movable-body contacts on those copies. Damage,
   props to wake, loose-body settling and diagnostics are recorded.
3. `commit_batch` (host, after the flush): copy the bodies back, publish
   GameRef poses and running gear, then apply the recorded damage in solver
   order with the chassis as it was when hit, crush cages, loose-body lifetimes,
   prop promotion and detaching.

Scripts still run only on the host with no batch in flight, so they see the
ticked state as before. The drawn frame is one frame behind it, as in any
pipelined physics thread. `--physics-inline` runs the same batch on the host.

Measured on the shared desktop, Windows x64 MSVC Release, private game copy
reset before each run, Baiern `0x6`, Valo City, hour 13, 640×480 window, no
VSync, fixed 1/60 s benchmark (`--bench-drive`), per-second car state printed
by a local, uncommitted probe:

- With the PR patch of the merge (stock fuel), threaded and `--physics-inline`
  runs gave bit-identical positions and velocities every second for 20 s,
  through a crash at about 8 s. Against the single-threaded build of the same
  merge they were identical until the crash and within 3 cm after it: damage
  and crush cages now apply at the end of each frame's batch. Two runs of the
  single-threaded build already differed by about 0.3 mm from the first
  second, from the wall-clock frames before the benchmark starts.
- 30 s drive with the shipped patch, mean of 29 one-second windows: frame
  28.7 ms threaded, 39.0 ms inline, 38.2 ms for the single-threaded build.
  The host's physics share fell from 25.8 ms (inline) to 15.5 ms. The physics
  thread's batch took 21.1 ms against an 11.9 ms flush, so the host still
  waits about 9 ms per frame here. Main `786e5b4` spent 23–38 ms per frame in
  physics in the same scene; this heavy scene is not addressed here.

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
The new opt-in `interpolation_ms` CSV subzone scopes **actual pose publication**
inside Physics (not Vehicles); it does not time downstream reader/cache work.
It includes record traversal and pose generation, rather than inferring cost from
an arithmetic inventory. End-to-end interpolation-related rendering overhead is
not isolated; repeated Classic/Adaptive totals would also include scene/cache
and visibility differences.

Before later upstream font/paint integration (engine/patch source of `5f22eca`),
at default 120 Hz with the three-class patch, one reset 40-s run each of
Baiern (Inline) and MC (Vee) completed. Excluding warm-up
and using 39 complete 60-frame windows per run, publication averaged **0.0043 ms**
(Baiern, per-window range 0.003–0.007 ms) and **0.0051 ms** (MC, 0.004–0.006 ms).
CSV samples round to 0.001 ms. These are single shared-desktop scenario measurements,
not an isolated-machine guarantee or a paired engine-performance comparison.
Both assemblies rendered and moved; ten-second captures were inspected. This
smoke test is not stock-output calibration or validation of every mod/engine.

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

The [spike-only prototype](../tests/test_physics_tick_rate.cpp) evaluates
raw-pedal shaping and plate/airflow immediately before each real solver substep,
not in a second crank integrator. It uses 3.5/s digital slew versus direct analog,
exact first-order 40-ms plate lag, the issue's RPM-dependent nonlinear flow curve,
a 150-rpm cut hysteresis and one latched BOV event consumed at a 60-Hz host cadence.
The existing feed-forward/feedback idle bypass is retained instead of adding the
reference's proportional-only idle controller a second time. Fuel cut is represented
by a motoring net curve `−F`, so indicated combustion becomes zero while friction
and the real plate/pumping state remain active. A loaded launch uses the actual
clutch/gears/wheels rather than inventing an external load-torque integrator.
The prototype also checks that an NA lift produces no BOV event.

This is an **experiment**, not a silent production input change. Its input must
be unfiltered; adopting it would bypass existing Controller smoothing, then publish
plate/load/rpm and latched events once at the host boundary. The manifold ratio
is supplied explicitly only for BOV detection; it never multiplies an already
boosted torque curve. Existing native declarations/callers remain unchanged.

The original issue prototype would need the same integration boundary: The issue prototype's external load-torque
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

#### Derived correction, now shipped

The [follow-up instruction](https://github.com/EliasFD3S/openslrr/issues/203#issuecomment-7030583795)
is to find/derive the formula. Scoped public-source/history searches found the
same squared law in [canonical scripts](https://github.com/imagecode-git/slrr-javas/blob/7e622de774c46c57b6598f79ad8dc065975b3a0a/parts/scripts/enginepart/block/SRC/Block_Inline.java)
and their inspected forks, not an established corrected SLRR law. The replacement
below is therefore an explicit, physics-motivated model, not a claimed recovery.

Continuity gives mass flow `ṁ = ρ A u`. For n valves of diameter d and lift L,
curtain area is `A_c = w n π d L`, not valve-disk area multiplied by lift and
squared. The available head fields retain enough geometry **before** block
normalization: `A_c = 4X/d`, `A_d = w n π d²/4`, and
`A_eff = min(A_c, A_d)`. Disk area is the available throat **proxy**, not a measured
runner/seat/stem flow area. Wear w is already present once in X and A_d.

[NASA's compressible mass-flow equation](https://www.grc.nasa.gov/www/k-12/airplane/mflchk.html)
at its M=1 maximum, divided by upstream density, gives reference volume speed

```text
U* = sqrt(γ R T) × [2/(γ+1)]^[(γ+1)/(2(γ−1))]
V_exchange = η Cd A_eff U* t0
c = clamp(V_exchange / V_swept, 0, 1)
```

Take the legacy reference interval `t0 = 0.001 s` and declared ideal-air reference
`γ=1.4, R=287.05 J/(kg K), T=293.15 K`, giving `U*=198.629616711 m/s`.
In the scripts' cm²/cm³/mm units this is:

```text
A_eff_cm² = min(tunnel_area_cm², 4 X_cm³ / (0.1 d_mm))
c = clamp(19.8629616711 × η × Cd × A_eff_cm² / V_swept_cm³, 0, 1)
```

There is no fitted global horsepower multiplier. The existing `breather_magic`
(default 0.9) is retained as an **empirical nominal discharge factor**, not a
measured Cd. Manifold/header flow efficiency η multiplies both minimum and maximum
flow; it no longer increases minimum leakage when placed in a denominator. Tuning
can make η exceed 1, so ηCd is not asserted to be a strict physical bound.

The shared [CylinderHead helper](https://github.com/EliasFD3S/openslrr/blob/d661d25/patch/parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/src/CylinderHead.java)
serves DOHC/SOHC/OHV proxies without subtype exceptions. Both
[Inline](https://github.com/EliasFD3S/openslrr/blob/d661d25/patch/parts/scripts/enginepart/block/src/Block_Inline.java) and
[Vee banks](https://github.com/EliasFD3S/openslrr/blob/d661d25/patch/parts/scripts/enginepart/block/src/Block_Vee.java) call it,
then retain the existing Vee average. Intake/exhaust zero geometry or efficiency
is handled independently. Sources **and compiled TUFA** ship in [the patch](../patch/README.md).
No new DynoData schema/native API is needed; the earlier draft's claim that every
curtain-based correction needed one was too broad.

At low lift, doubling lift or valve count now doubles capacity rather than
quadrupling it. Raising lift beyond d/4 no longer invents extra throat area;
31-mm valves reach the disk proxy at 7.75 mm, not an artificial oversized cam.
At fixed gas state, doubling all lengths halves the 1-ms capacity/volume ratio;
the old squared law instead multiplied it by eight.

Actual authored geometry (wear=1; source π=3.1416), verified by executing the
compiled Java helper in VM tests (`tests/test_port_flow.cpp`, removed in #352):

| Intake | Bore/stroke mm | d / lift mm | η | Legacy c | New c |
| --- | --- | --- | --- | --- | --- |
| Kraftwerk | 80 / 90 | 31 / 11.9 | .75 | .481474 | .447382 |
| DevilSport | 93 / 88 | 33 / 11.9 | .9375 | .584879 | .479587 |
| SuperDuty open | 90 / 106 | 42.5 / 19 | .92 | 1 (clipped) | .691976 |
| SuperDuty closed | 90 / 106 | 42.5 / 1.27 | .92 | .018836 | .082712 |

**Limitations:** this normalizes the legacy *linear pressure relaxation* with a
nominal choked exchange capacity. Unchoked orifice flow scales with
`A sqrt(ρ |Δp|)`; its square root belongs to the pressure difference, not to area
or cam lift. This is not a pressure-ratio-dependent compressible mass/enthalpy
solver or a flowbench calibration. Native min/max rectangular valve
timing is unchanged. Authored nonzero closed lifts are treated as leakage;
SuperDuty's 1.27-mm minimum changes substantially and may need data retuning.
The physical gas-state/seat/discharge details and absolute stock-car outputs remain
unvalidated; these limitations do not justify retaining a unit-dependent X²/V law.

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

#### Correction, now shipped

The [owner's follow-up](https://github.com/EliasFD3S/openslrr/issues/203#issuecomment-7069947601)
repeats the 1.09 MJ/kg report. The patched
[AirFuelDeliverySystem](https://github.com/EliasFD3S/openslrr/blob/d661d25/patch/parts/scripts/enginepart/src/AirFuelDeliverySystem.java)
now uses `H = LHV / (A/F)stoich`, per kg of charge, and drops the ×1.55
factor: `mixture_H = H × (0.8 + 0.2 × match)`. The cycle's charge mass is
inducted air at ambient density, so heat per kg of air at the stoichiometric
(or richer, oxygen-limited) mixture is the matching quantity; it lies inside
the 2.84–3.15 MJ/kg the report quotes.

| Fuel | LHV | (A/F)stoich | H |
| --- | --- | --- | --- |
| 95 octane | 43.4 MJ/kg | 14.7 | 2.952381 MJ/kg |
| 98 / 100 octane | 43.4 MJ/kg | 14.7 | 95 × stock 1100000/1099500 and 1100250/1099500 |
| Methanol | 19.9 MJ/kg | 6.47 (CH₃OH + 1.5 O₂) | 3.075734 MJ/kg |
| Diesel | 43.4 MJ/kg | 14.5 | 2.993103 MJ/kg |

The fuel values are handbook figures chosen here, not measured fuels. The
compression window keeps the stock heat: `minComp`/`maxComp` use
`stock H × match × 1.55`, as Fuel Revamp 1.2 decoupled them, so every stock
engine stays dynoable. At 95's best mixture `mixture_H` rises from 1.704225 to
2.952381 MJ/kg (×1.73). The VM suite `port_flow` (removed in #352) ran `updatevariables` for
every fuel at three mixtures.

Same benchmark, horizontal speed at 2/3/4/5 s of full throttle: main `786e5b4`
32.4/52.4/62.7/75.1 km/h (it still has the ×2 driving gain), this branch before
the fuel fix 8.3/16.4/24.4/31.5 km/h, with it 16.4/29.4/41.2/54.3 km/h.

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

MSVC `/O2`, corrected net-torque integration, nine checks passing at each
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

The original `d60b907` baseline regression could not link its obsolete
`tire_effective_load` call. Main's later #48 migration replaced that test/API;
after integrating `7f629d5`, the current vehicle-dynamics, missing-support and
wheel-mount suites pass 14, 16 and 7 checks respectively. The burnout-only fixture
now explicitly supplies a 470 Nm **net** curve: its former 235 Nm input relied on
the removed hidden ×2 gain. The normal 235 Nm fixture and main's brake/support/
tyre model remain unchanged.

These are bounded fixture results, not universal numerical stability or
original-game compatibility. They exclude mixed dynamic contacts, full terrain,
render-rate/input variation, stalls/catch-up and time warp. The separate prototype
adds 50 digital/analog/inertia/rate sweeps with bounded RPM and one BOV event per
boosted lift, plus a loaded launch at each rate. It is not a claim that new TUFA
bindings or production control integration are shipped. The 1000 Hz kernel result
does not validate the game's integer shared-world scheduler.

### Real-time pacing check after main integration

The PR follow-up requested current main and fixes for observed stutters. After
integrating `7f629d5`, two reset-private-root 40-second Baiern/Valo City drives
used `--bench-realtime --no-vsync --bench-time 13`, with the parts patch installed.
Unlike the earlier fixed-delta throughput experiment, these retain wall-clock
stalls/catch-up. Neither run took screenshots or ran alongside this ticket's
build/tests. Loading/settle rows before the profiler restart were excluded.

Each emitted 39 complete wall-second windows: 6,713 and 7,614 measured frames.
Maximum frame times were 23.205 and 24.204 ms; the highest per-window p95 values
were 15.050 and 14.539 ms. Neither run contained a measured frame over 33.333 ms.
No repeatable driving hitch was reproduced, so no speculative runtime stutter
patch is added. The separate clean-main screenshot run had 55–62 ms maxima;
capture/loading and changed trajectories make it unsuitable as a no-capture
hitch comparison. These shared-desktop runs do not establish universal smoothness
or isolate GPU/presentation contention, manual input, career transitions or every
vehicle/route. CSV zone values are window means, not hitch-frame attribution.

## Stock calibration (#352)

In game the stock engines showed much less power after #215. Measured on the
stock MWM install (`vehicle_spawn_stock`, DynoData `maxHP` of each of the 24
stock racers), main `d661d25` with `patch/parts` gave 36% (SuperDuty 500) to
100% (Baiern CoupeSport) of the `614eef1` values, 65% on average. Separated:

| Change | Share of the previous maxHP |
| --- | --- |
| SI torque law only (stock Java) | 46–50%, uniform |
| + fuel heat patch | 84–93%, uniform |
| + port-flow patch | 36–100% |

The port-flow law changes each engine differently, so no single factor can
restore it. Both Java patches are withdrawn; the cycle scales indicated torque
by `engine_stock_calibration = 2.0`. Then net power
`N·2W/(4π)·ω − F·ω` equals the legacy `N·W·rpm/60 − rpm³·2·10⁻⁸` exactly:
all 24 racers keep their maxHP and peak rpm (`tests/test_stock_engine_power.cpp`).
Torque stays P/ω, so displayed peak torque is about 26% above the legacy table
torque, which understated power by 4/π. Driving still uses the net curve
without the legacy ×2 gain, so cars accelerate less than before #215 (the owner
accepts this with the new throttle model). The witness self-check uses the
same factor. The port-flow and fuel-heat derivations above stay as research.

## 6. Remaining contract/design work

The output-compatibility question is resolved: the owner explicitly permits
changed torque/power, and the focused C++ correction is implemented. It is not a
newly calibrated stock engine, manifold, fuel or port-flow model.

The narrow port normalization is now corrected in Java, **before** clipping and
bank averaging lose the head geometry; it needs no new DynoData schema. A full
instantaneous pressure-dependent valve solver is a separate extension, not claimed
by the nominal reference-flow correction. Fuel heat is corrected in the Java fuel
system with the compression window decoupled, as
[Fuel Revamp 1.2](https://github.com/Gorgoil/slrr-javas/commit/f1d91cf826987f24dd53aa138d57965a43d72b26)
did; its numeric fuel calibration is not copied.

The physics thread overlaps the frame's drawing only. Scripts, natives and
callbacks keep running with no batch in flight; overlapping them too would need
host authority over solver state that scripts change during a frame and
published snapshots for every native that reads it. Production
raw-input/telemetry publication and full renderer interpolation overhead remain
future work. No default-rate, Classic-mode or native API change is made here.

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
