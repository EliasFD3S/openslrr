# Driving model and nitrous

Detailed contract for the tyre/drive model and the nitrous system, moved
from [ENGINE_CONTRACT.md §11](../core/ENGINE_CONTRACT.md#11-physics-and-damage),
which keeps the general physics and damage contract.

## Intended driving model: revision `8ac2ba9`

The maintainer's `8ac2ba9601300b36efca54dd81031fb6bc9de98b` revision is the
intended model for PR #48. It supersedes that PR's earlier script-derived
curve and instant-centre load path. It is OpenSLRR tuning, not recovered
stock physics; do not restore the earlier solver just to make its old tests
pass. The follow-up work awaiting approval is in
[the vehicle-dynamics migration plan](dynamics-plan.md).

`configure_drive` copies the tyre script's `WheelRef` values. Since #418 the
tyre is a Pacejka Magic Formula after the MF 5.2 structure
([tire_model.cpp](../../src/physics/tire_model.cpp)): separate pure lateral
and longitudinal curves, load-dependent friction and cornering stiffness,
camber, and combined slip by normalised slip. The script values set its
coefficients; the MF coefficients are typical passenger/sport tyre values,
OpenSLRR tuning, not recovered from the exe (#383 had one combined-slip curve
after the original law, nicolas-found42/sl-remastered, 00454500):

- Slip ratio kappa = (wheel - road) / road speed and slip angle
  alpha = atan(lateral / road speed), both with a 1.5 m/s floor.
- Nominal load Fz0 = `setMaxLoad` / 2 (4000 N when unset); dfz = (Fz - Fz0) / Fz0.
- D = Fz × mu, mu = 1.15 (`kPeakFriction`) × `setFriction` / 1.76863
  (clamped 0.2–2.5) × (1 - 0.12 dfz × `setLoadSmooth` / 0.4 × load
  sensitivity, floor 0.5) × camber grip. `frictn_x` scales the longitudinal D.
  Width, compound, profile and inflation enter through these script inputs
  (the patched `Tyre` sets them from physical values since #450);
  the patched `Tyre` ([notes](notes.md#tyre-pressure-424)) also scales the
  friction with the pressure and allows over-inflation.
- C from `setSliction`, the slide/grip ratio its script documents: the curve
  ends at sliction × the peak (sliction clamped to 0.4–1; 1 keeps the peak).
- Cornering stiffness Kya = s Fz0 sin(2 atan(Fz / (2 Fz0))) / sin(2 atan(0.5)):
  the zero-slip slope per unit load is s at Fz0 and saturates with load
  (0.625 s per newton at 2 Fz0). s is `setStiffness` (clamped 4–60, times
  `lateral_response`) up to the stock 17.68, and its square root scaled to
  that value above it: `Tyre.java` gives every low-profile tyre its clamp of
  40, which peaked at 4.3 degrees; it now acts as 26.6. The longitudinal
  curve uses the same slope. B = K / (C D), E = -1 (the original's
  `setPacejka(8)`).
- Camber (`setYpr` roll from `Suspension.setSuspensionGeometry`, plus body
  roll, against the road): about one load per radian of camber stiffness,
  0.6 of it as a slip-angle shift and 0.4 as a force shift (MF pHy3/pVy3), so
  a tyre leaning into its force grips more; the friction falls by
  3 × camber² (floor 0.25, which replaces the old cos² load factor). The
  thrust fades in over the first 3 m/s of rolling.
- Combined slip: each slip is divided by its pure curve's peak slip; the
  resultant runs along each pure curve and splits by direction, so wheelspin
  or a locked wheel uses up the cornering grip (power oversteer, drifts,
  burnouts). The camber thrust fades in proportion.
- Aligning torque (#458, [notes](notes.md#tyre-aligning-torque-458)):
  `TireForces::aligning` = pneumatic trail × the slip part of the lateral
  force (not the camber thrust). Trail after MF 5.2: Dt = R0 × Fz / Fz0 ×
  (0.09 - 0.01 dfz), at most 0.2 R0 (R0 = `TireParameters::radius`, the
  wheel's), times cos(1.4 atan(Bt α_eq)) cos α, with Bt putting the zero at
  1.5 × the lateral peak's slip; α_eq is the combined slip on the lateral
  curve, so wheelspin or braking lightens it too. The trail scales with the
  relaxation length over 0.3 m (the script's pressure signal). The
  longitudinal force adds its moment at the patch's lateral offset, Fy x
  relaxation / cornering stiffness (at most 5 cm). The low-speed caps scale
  it with the force. The body takes it about the patch normal; force
  feedback adds the caster trail ([notes](notes.md#tyre-aligning-torque-458)).
- Steering axis (#458): a strut wheel turns about its kingpin
  (`CornerFrames::steer_axis`, ball joint to strut top), with the angle
  raised by 1 / cos(lean) so the road heading is the steering angle; caster
  then leans the outer wheel's top into the turn. Its hub origin swings
  about the kingpin (`wheel_pose(..., steer)`, through a road car's ground
  point), so steering lifts the car; force feedback adds that jacking's
  torque. Other wheels turn about the hub's vertical.
- Stock tyre at 3500 N: lateral peak 1.14 at 8.2 degrees, 1.05 at 20 degrees;
  longitudinal peak 1.14 at 14 % slip, 0.97 locked; at 6 degrees the lateral
  force falls from 1.11 to 0.51 with 20 % wheelspin; at 4 degrees, 0.95,
  0.98 leaning 3 degrees into the force, 0.92 away. GT III rear (295/20,
  stiffness 40): peak 1.56 at 6.4 degrees.
- Low-speed caps: the force grows with the sliding speed, 0.95 (lateral) /
  0.85 (longitudinal) × load per m/s, times max(1, road speed / 2 m/s). The
  lateral cap uses the patch's actual sliding speed, not the relaxed one, and
  below 3 m/s the tyre never pushes along the way it slides
  ([notes](notes.md#a-sideways-slide-swung-back-like-a-spring-426)). Before
  #418 that factor was missing, so the cap also bounded the cornering
  stiffness at speed (16 per radian at 60 km/h, softer than the stock tyre).
  `TireTuning` scales the lateral and longitudinal force (grip), the slope
  (lateral response) and the load sensitivity.
  `setRollRes` supplies the wheel's rolling torque.
- `VehicleDynamics` feeds the curve a relaxed slip angle: the tyre's lateral
  deflection builds over the tyre's relaxation length
  (`TireParameters::relaxation_length`, from the patched
  `WheelRef.setRelaxation`; 0.3 m at the optimal pressure and without the
  patch), integrated implicitly, a 12 ms lag at 90 km/h for 0.3 m.
- Tyre pressure, wear, punctures and flat spots (#461, [notes](notes.md#tyre-temperature-pressure-stiffness-wear-punctures-and-flat-spots-461)):
  the Tyre's cold `inflation` follows the carcass temperature; against its
  compound's working temperature it scales the relaxation length, rolling
  resistance, nominal load and cornering stiffness (with the tread rubber's
  softening). The sliding work, weighted up as the tread overheats, wears
  the tyre part against its `setMaxWear` life; under 15 % condition the grip
  falls. `effective_tyre` combines them with the temperature and ground grip.
  The vertical rate follows the pressure and the wheel rolls on its loaded
  radius (free radius less a third of the deflection). A rim strike pinches
  the tyre, wear to the cords or a cooked carcass blows it out; flat, it
  grips and carries less, and pushed sideways or run 3 km flat it comes off
  the rim, leaving the rim on metal. A wheel locked while sliding grinds a
  flat spot that thumps once a turn and shortens the rolling radius as it
  passes (about 0.75 mm after a locked stop from 100 km/h); the Tyre saves
  it and a garage repair clears it.
- Kerbs beside the tyre (#467): ground beside the hub more than 12 cm up
  (above the sidewall bulge) leaves the tyre's vertical support and pushes
  the body at the carcass' lateral rate, square to the kerb's edge, as a
  wall against the sidewall; a kerb ahead, or met more from ahead than from
  the side, is still rolled onto.
- Ground surface (#452, [notes](notes.md#ground-surface-friction-and-rolling-resistance-452)):
  each map collision face carries its SCM material (`GroundSurface`).
  Friction and rolling are averaged over the 13 tread samples. The
  friction (× `Config.ground_friction`) multiplies `TireParameters::friction`
  (force law and sticking limits, metal and parked contacts too); the
  rolling coefficient adds to the tyre's in the wheel's rolling torque, not
  to the tread heat. SCM `friction` is used as is, `rollres` × 0.05
  (`kRollresCoefficient`), not the original's speed-proportional drag. A
  body contact takes min(0.6, ground friction), and `collcoeff` (the
  original's shape bounce) scales its restitution, capped at 0.5. The
  material's `skid_SMcolor` tints the tyre marks and its `skid_SFX`
  replaces the slide loop; its `skid_PS` (the particle cloud) and
  `skid_SM` (the textured mark) drive the tyre effects (#454, below). A
  material without values takes a class from its name or SCY texture
  (`ground_surface_for_name`): grass 0.65 / 0.08, sand 0.6 / 0.15, gravel
  0.7 / 0.03, dirt 0.7 / 0.04, each with its stock particles.rpk cloud
  (grass clippings, sand dust, pebbles) and the stock grass mark; a paved
  name stays road. Objects, garage shells and props are
  road, as in the original.

Wheel alignment enters as camber (above); the load itself is no longer
scaled. In strut mode (`setHub` mode 0) the strut fit replaces the hub
rotation, so `setYpr` camber and toe do not reach a strut wheel: the patched
stock struts tilt the fit itself to their static camber (#450,
[notes](notes.md#tyres-and-suspension-geometry-450)). The copied `setCPatch` tolerance (`patch_angle`) is not read by the
solver. Likewise, `setInstantCenter` values are retained but have no active
load effect (their frame is unknown and the recovered exe does not use
them). Anti-dive, anti-lift and anti-squat come from typical shares by
suspension type instead (#469, [notes](notes.md#anti-dive-and-anti-squat-469)):
`WheelState::jacking` pushes body and wheel apart (or together) by the share
of each wheel's longitudinal load transfer its arms carry.
Wheel YPR and the existing arm/hub kinematics still affect contact geometry.
Anti-roll bars are an optional host approximation, deliberately enabling a
stock feature whose Java commands and catalog branch were dormant. The patched
Catalog exposes the existing `parts:0220` branch on Running Gear / Suspension;
no resource IDs are changed. The patch adds the missing 701/702 mount slots to
the stock racer chassis CFGs (#311) and uncomments every racer script's stock
`*_parts_list_RGear_sways` list, so newly assembled stock racers come with
bars; the generic `addPart` drops any bar no mount can hold.
`part_runtime` replaces each axle's configuration from its current installed
`Swaybar` on authored mount 701 (front) or 702 (rear). Existing generic CFG
compatibility remains authoritative, not the part class's model/axle name.
The patched stock bars carry per-car rates from the real cars' bar diameters
and no damping (#419, [patch](../../patch/README.md#per-car-bar-rates-419)).
Finite nonnegative `force` / `damping` fields are scaled by clamped wear and
tear; removing, moving, destroying or repairing the part is reflected by the
next refresh/tick without an accumulated command or new save state. Extraction
is independent of the engine and controller.

The kernel reimplements the bounded chassis-couple approach found in SLRR_rev
`77b0fb6` and `4ca11c2`, not a recovered original MWM force law. Both axle corners
must be enabled, touching and non-rigid. After tyre evaluation, opposite world-Y
forces act at the two suspension hardpoints, with magnitude capped at half the
body's weight. Stiffness uses the right-minus-left solved length relative to
each corner's own rest length (unlike the upstream raw-length rule); damping
opposes their relative world-Y velocity. This adds no net lift and does not
rewrite the already-evaluated tyre loads. Zero inputs leave prior behavior
unchanged. Kernel callers' invalid coefficients act as zero; an invalid script
coefficient pair disables that installed bar. The couple uses the same fixed
substeps, including on passive/engine-less cars; it is not a full bar/linkage or
same-step tyre-load-transfer model.

The approved migration keeps that tyre law but corrects brake coupling.
After the drivetrain impulse, one bearing/service/handbrake impulse budget
is consumed before tyre force sees the spin. Its unused portion holds a
locked wheel against the subsequent tyre reaction; it is not a second full
brake application. The service part of the budget is each caliper's
pressure after ABS/ASR times the pad friction at the rotor's temperature
([Brakes, ABS and ASR](#brakes-abs-and-asr-446)). Only longitudinal force retains
the per-wheel no-slip-reversal bound; the earlier shared lateral impulse
bound is absent. Automatic first/reverse launch take-up is still raised
only while both service brake and handbrake are below 0.05, not while
holding the service brake. Previous burnout measurements do not validate
this revision. The `8ac2ba9` brake-hold reproducer (traction despite zero
final wheel spin) is covered by forward/reverse and weak/strong brake tests.

The approved support migration replaces the spring-less corner's old
previous-load/post-step-impulse path with a same-substep compliant tyre /
bump-stop load. Fixed-travel corners and up to eight installed-body low
points solve their implicit normal impulses jointly on a predicted body;
the resulting loads reach the tyre law before integration. Stiffness scales
with mass, the installed damper is retained, and a deep-penetration safety
stop remains. An absent scripted compression limit falls back to CFG spring
`base - travel`, not `base` (which could prop up the missing-spring corner).

`rebuild_body_support` extracts actual low mesh vertices across left/right
and four longitudinal sectors. It excludes running gear (already represented
by wheels), hinged panels (whose floor contact remains unsupported) and
engine hardware (`EnginePart`: exhaust pipes, tips, mufflers; see
[notes](notes.md#a-lowered-car-rode-on-its-exhaust-426)), whose low points
become scrape points instead (#467, [notes](notes.md#hanging-parts-and-tall-kerbs-467)):
a hanger-then-pipe spring with metal friction, raised to 5 mm clear of the
ground the car rests on. It respects slot poses/deformation, and refreshes
after assembly changes, damage and repair. Hiding a mesh does not remove its physical support. In an
upright car these samples replace fictitious underside AABB corners; the
old box remains the fallback for callers without samples and after rollover.
Floor-only support does not turn a low panel rubbing the ground into a
chassis wall-impact damage event.

A rim-less corner with only hub hardware uses a metal friction disk (0.4)
and rolling-resistance factor 0.04, not the lost tyre's script values. An
installed outer dually remains a rubber contact. Parked tyres use a 0.8
friction upper bound; body and metal contacts use 0.4. These are host tuning.
Rubber sticking is additionally bounded by the intended model's zero-slip
friction-ellipse axes (`tire_friction_limits`), including script grip,
load sensitivity and camber. A zero
longitudinal grip cannot be replaced with free parking friction. The same
capacity equations are shared with the dynamic curve; its forces do not
change. Native traffic keeps its separate force policy.
A projected friction solve acts after forces/gravity but before advancing
the pose, allowing real sticking instead of permanent tiny sliding steps.
A mechanical brake's longitudinal hold additionally cannot spend more than
its remaining brake/bearing impulse budget; lateral grip is independent of
brake torque. The remaining friction ellipse still permits slipping when
support is insufficient; no general chassis sleep state or new wake-state
machine is introduced. `RigidBody::integrate` keeps its existing behaviour;
the vehicle owner uses its force/pose split to insert the constraints.

## Brakes, ABS and ASR (#446)

`src/physics/brake_system.cpp` holds the brake hardware model and the two
driving aids; `VehicleDynamics::step_drive` runs them once per substep.
Notes and measurements: [notes](notes.md#brakes-as-hardware-abs-and-asr-446).

- **Torque.** With the patched `DiscBrake` each wheel's `setBrake` is its
  torque at full pedal (100 bar) and `setBrakeSpec` its pad compound, rotor
  type and diameter. A chassis with any specified wheel uses physical
  brakes: `brake_scale` is `brakepower / 1000` (1 on every stock car), the
  handbrake gain 2, and `distribute_brakes` (#400) is skipped because the
  patched `Brake` applies the balance as a valve on real front/rear
  hardware. Without the patch nothing changes: gain 25 / `brake_inertia`,
  the 70 % front split, no heat.
- **Pressure.** `service_brake_` (the pedal through an 80 ms lag) is the
  driver's pressure. Each caliper gets the ABS channel's pressure, or the
  ASR's when it brakes a spinning wheel. Without ABS a rear proportioning
  valve passes the full pressure to 30 % pedal and 0.3 of the rise above.
- **Heat.** Each friction application adds `0.5 I (w0^2 - w1^2)` to the
  rotor (the hold of a locked wheel adds almost nothing: the tyre takes that
  energy). The rotor cools by convection over its faces and vanes, growing
  with the car's speed, and by radiation. The pad friction factor (cold,
  working window, fade) multiplies the service torque; the handbrake does
  not fade.
- **Sensors.** ABS and ASR see the wheels' circumferential speeds along the
  direction of travel, their rates (4 ms filter) and the body's longitudinal
  acceleration, saturated at 1.5 g. `ReferenceSpeed` integrates it between
  wheel corrections: the fastest wheel while braking, the slowest undriven
  wheel while driving (with every wheel driven, the slowest one, slowly).
  While braking, 0.4 s with every wheel below 70 % of the estimate and none
  spinning up resets it to the fastest wheel.
- **ABS** (`AbsModulator`, Options ABS slider): per channel follow, dump
  (35 ms e-folding), hold, rebuild (80 % of the lock pressure at once, then
  a ramp). It dumps past 16 % slip (26 % at the lowest setting) or when the
  wheel decelerates 6 m/s^2 faster than the car with 60 % of that slip. The
  rear channels use 0.7 of the threshold (electronic distribution, which
  replaces the valve) and share the lower pressure (select-low). A front
  wheel alone modulating limits the other front to its pressure plus a
  margin growing over 0.8 s. Below 2 m/s the pressure follows the pedal.
- **Fluid.** The caliper's fluid follows 45 % of the rotor's rise above
  ambient with a 40 s lag; past 230 deg C (DOT 4) the pressure reaching the
  pads falls, to 0.4 at 290 deg C.
- **Tyres** (`src/physics/tyre_thermal.cpp`): with the patched `Tyre`
  (`WheelRef.setTyreSpec`, the compound), each tyre has a surface skin, a
  tread bulk and a carcass temperature (#472). Half the sliding work at the
  patch heats the skin, which passes it to the bulk and loses it to the air
  and the road within seconds; the rolling resistance and the rotor (through
  the rim) heat the carcass; all cool faster with speed. The tread rubber's
  temperature (`tyre_grip_temperature`, the mean of skin and bulk, what the
  HUD shows) scales the tyre's friction (`TireParameters::friction`) for the
  force law and the sticking limits alike, through a curve per compound.
  Wear and rubber smoke follow the skin.
- **Feedback.** While ABS works, a force feedback wheel buzzes at 14 Hz and
  the pad the brake is bound to rumbles; the HUD lights the stock ABS / ASR
  lamps. Driverless cars with physical brakes have ABS.
- **ASR** (`TractionControl`, Options ASR slider): while the driver
  accelerates (clutch engaged, no brake, no shift), the engine torque factor
  follows the driven axles' mean slip past 8 % or 0.8 m/s (20 % or 2 m/s at
  the lowest setting), a fast cut and a 0.12 s recovery, at any speed and in
  any gear. Below 80 km/h a driven wheel spinning 0.6 m/s more than the
  other one of its axle gets up to 40 % brake pressure. The auto clutch
  still follows the driver's pedal, not the cut (#380).

For the dyno, a new configuration must rebuild the matching curve banks,
including nitro. Do not mix an old nitro bank with a new RPM step or a new
geometry. Bound indices before the integer conversion. The last exact sample
must remain reachable.

#203 defines dyno torque and `DriveInput::torque` as **net open-throttle crank
Nm**. A four-stroke cycle contributes N·work/(4π), minus the shared recovered
mechanical friction torque (`engine_friction_torque` in
[dyno_model.hpp](../../include/slrr/dyno_model.hpp)). Power is torque × angular speed,
including between samples and for nitrous blends. The stock `getHP` native still
returns watts, while `maxHP` remains mechanical horsepower. Driving has no ×2
gain: it recovers indicated torque before pedal/combustion limiting and subtracts
mechanical friction once. The previous drag curve is incremental closed-throttle
pumping scaled by (1 − effective pedal); it is not subtracted again at WOT.
The nitrous wet bank is net torque too and blends in the same indicated space.
Fuel cut retains friction, and existing starter/idle/clutch behavior remains.
These loss curves are empirical, not a new manifold or combustion calibration.
Stock outputs intentionally change under the issue owner's physical-consistency
direction; see [the investigation](physics-tick.md).

The #203 Java port patch computes `in/out_min/max` from capped curtain area:
`c = clamp(efficiency * discharge * A_eff * U_ref * 0.001 / V_swept, 0, 1)`
in SI. The declared ideal-air M=1 reference has `U_ref = 198.629616711 m/s`.
The existing 0.9 breather factor stays empirical; this does not make the native
linear pressure relaxation an exact compressible valve solver. The shared
CylinderHead helper must ship with both updated block classes; DOHC/SOHC/OHV
continue supplying the same disk-area/lift proxy, and Vee averaging is unchanged.
Efficiency now restricts minimum as well as maximum flow multiplicatively, so
authored nonzero closed lifts can change leakage. No DynoData field/schema or
native signature changes. See [patch installation/reproduction](../../patch/README.md#port-flow-normalization-203).

The #203 fuel patch (`AirFuelDeliverySystem`) sets `mixture_H` to the heat per
kg of charge at a fuel's best mixture, lower heating value / stoichiometric
air:fuel ratio of the fuel blend (95 is E5, 98 is E10: 2.953 and 2.954 MJ/kg),
times the stock mixture-match factor, without the old ×1.55 (#407).

Knock (#407): the fuel sets `DynoData.knockCompression`, the knock-limited
static compression of a naturally aspirated 20 °C charge (95: 10.5, 98: 11.5,
100: 12.5, methanol: 15; 0 for diesel and any other fuel system, which
disables it). For every dyno row, `prepare()` in `src/physics/dyno_model.cpp`
compresses the unburned charge to TDC with n = 1.3 from the boosted intake
pressure and the intake temperature (intercooling and nitrous cooling
included), then finds the naturally aspirated compression with the same
Douaud-Eyzat ignition delay (tau ~ p^-1.7 exp(3800/T)). Above the limit it
scales the burned heat by 1 − 0.025 × excess, down to 0.6, like a spark
retard. The boost heating `prepare()` applies to combustion (T × sqrt(1 + boost))
is left out of the knock state: DynoData carries no intercooler, so every
stock turbo would knock as if it had none. The driving curve is the dyno curve,
so the loss applies on the road too. `minComp`/`maxComp` no longer follow the
mixture; they only refuse engines that cannot run on the fuel. See
[patch](../../patch/README.md#fuel-heat-blends-and-knock-203-407).

Superchargers (#415): `DynoData.charger_type` 1 selects a positive-displacement
law in `prepare()` (delivery proportional to blower speed with internal
leakage, adiabatic outlet temperature for the drive power, charge cooled
back to intake temperature by `charger_intercooled`, default 1) instead of the
turbo bell curve, and `blower_drive()` in `src/physics/dyno_model.cpp`
subtracts the blower's drive torque from each dyno row and lets an overloaded
belt slip. `dyno_boost_fraction` follows the same law. See
[patch](../../patch/README.md#superchargers-positive-displacement-pulleys-and-belt-415).

## Nitrous driving and exhaust (#303, #304)

The Part chassis owns a native nitrous aggregate, separate from the script's
`Canister.actual`. Capacity and injection rate come from the installed scripts'
`tank_nitro` and `consumption_nitro`. The aggregate starts empty; `forceUpdate`
only clamps it to capacity, and the explicit `reload` command fills it. Reset,
repair, a new bottle and serialization do not silently refill it. Stock Track
calls `reload` when locking the car for driving; a newly loaded chassis is
otherwise empty, matching MWM's native/script split.

Mapped axis 31 (stock N) supplies a clamped analogue input. A controlled,
released, running mechanical engine with input > 0.1, positive rate and remaining
nitrous can select the existing wet dyno bank. The solver blends its limited dry
output toward the wet bank independently of the accelerator; no new dyno math or
engine torque gain is introduced. Each fixed solver step consumes
`RPM * 0.0002 * input * rev_limiter_fraction * dt * consumption_nitro`.
The HUD reports remaining/capacity only with an injection kit, and the configured
nitrous sound (default `sound:25`) is a press-triggered one-shot, not a loop.

Flag-256 exhaust meshes and fake-light children share the chassis's presentation
sample: eligible nitrous, >1000 RPM and throttle >0.01, with a 30% flicker gate.
The original samples random state per rendered child; the host deliberately uses
one deterministic MSVC-LCG sample per 60-Hz simulation tick, shared by all outlets
and cameras. Pausing cannot consume nitrous or resample the flame. Detached,
broken, preview and stopped parts do not show an exhaust effect.

The stock flame is the authored SCX3 mesh and premultiplied PNG, not a particle
emitter. It must not be snapped onto lamp glass, write depth, or multiply alpha
twice. Associated FakeSpot lights draw only the authored central flare glow,
with depth/camera/range/cone checks and the flares option; they never become
400-metre floodlights. Glow sizing follows the existing sun-flare approximation,
not recovered native units; ray/ghost distribution and original view-angle
fidelity are not established. Generic ParticleSystem and purge remain separate.
See [EXTERNAL_RESEARCH §7](../research/EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev) and [notes.md "Nitrous driving and exhaust"](notes.md#nitrous-driving-and-exhaust-303-304) for
binary/source provenance and verification entry points.

Mass distribution and crush zones are separate models. The car's mass,
centre of mass and inertia come from the installed parts' CFG `body` lines
(`assembly_mass_properties`): each line's mass is spread over its own shape (a
box, a sphere, or its phys SCX hull when that is a closed surface, else the
hull's bounds), and the pieces are summed in the chassis frame with the
parallel-axis theorem. The stock game only sums point masses and has no
authored centre; this is a host model, and the products of inertia are dropped
because the rigid body takes principal moments along the chassis axes
(`dynamics_configure` passes them to `RigidBody::configure`; each supplied
axis is floored at 30 % of the full-bounds box moment). In `8ac2ba9` the
explicit-moment overload accepts the vector only when all three components
are positive and finite; otherwise all three axes use the default. That
default uses a box with dimensions 0.75 times the bounds, so its moments are
0.5625 times the full-bounds box values. This generic fallback also affects
loose bodies and native traffic, not just the player car. The collision
bounds themselves are not shrunk. Angular momentum is damped by
`exp(-0.015 × dt)` each substep. Parts
without bodies add no mass; in stock data that is the running gear, exhausts,
batteries and several accessories, and tyres weigh about 0.1 kg. The patch
gives them `nocollision` bodies and rebalances the stock chassis CFGs
([notes](notes.md#weight-distribution-and-centre-of-gravity-430)). Crush zones
are not those meshes: a deformation cage is generated over the part's visible
render shell, as SLRR_rev (77b0fb6, `vehicle-3d.ts`) does, and its cage mass is
the part's own CFG mass (the chassis shell's own, not the assembly's). The phys
mesh stays the part's collider and follows the cage.

A corner loses its wheels when either of two independent conditions reaches
zero: its strut, once the impact energy |J|²/2M weighted by exp(-(d/0.65 m)²) of
the horizontal distance to the corner has added up to 55 kJ, or its mount, once
the chassis cage, settled after an impact, holds the wheel centre or strut top
0.3 m from where they were when the wheel was fitted. The transient crush peak right after an impact does
not count, only the settled (plastic) shape. These are SLRR_rev values
(`releaseFailedWheels`, `suspension-3d.ts` `deform`, `damageImpact`), not stock
ones; floor contacts below 8 m/s do not count. Every part on a wheel slot of
that corner (the rim with its tyre, an outer dually wheel) then breaks off like
a panel and becomes a loose body of at least 14 kg. The chassis is refreshed,
so the corner rests on the hardware still on its hub or, with none, on the
chassis box. The hub hardware stays, and the dented chassis does not move the
remaining wheels' hardpoints. Fitting a wheel to that corner restores a sound
mount: a corner that is still dented keeps its current dent as the baseline,
so the new wheel only falls off after a further 0.3 m of settled crush or 55 kJ
of new impact energy. Repair clears the dent, the baseline and the mount.

The engine breaks off on a much bigger impact. Its mount takes the same impact
energy weighted by 0.5 exp(-(d/1.1 m)²) of the horizontal distance to the engine
and fails at 120 kJ, so 240 kJ at the engine itself (SLRR_rev `damageImpact` and
its block release; not stock values). Earlier Baiern captures retained the
engine at 73–74 km/h and released it near 95 km/h; those approach speeds and
outcomes have not been revalidated with the `8ac2ba9` solver. The engine is
the chassis child that carries the `dynodata` (the stock `Block`), found as
`configure_drive` finds it, and everything installed on it goes too. It leaves
as one loose body with its pre-impact velocity, so it carries on out of the car;
as it starts inside the car's collision shell, it skips the car contacts until
none of its corners is inside the car's bounds (at most 1 s). Whenever a part
breaks off, the reconfigured chassis keeps its velocity and angular velocity, as
SLRR_rev does, not its angular momentum: with less inertia the same momentum
would spin it up (a car that lost its engine pitched over).
The mechanical car then has no dyno, so no drive torque, engine audio or
rpm. `configure_drive` gives it a neutral drive with a zero-torque engine
that never starts; the tyre and brake paths stay active. This is the coasting
mechanism, not a guarantee that strong-brake holding is correct in the new
solver. Mechanical AI input is preserved as well as player input.
Released bodies keep wall collision, damage and distance accumulation
regardless of engine or controller ownership; native traffic uses the separate
`drive.native_traffic` force path without Java engine/gear data. Damage
options still distinguish the player from other cars. Repair restores the
mount but not the engine; fitting an engine again clears the state.

Deformation and detached parts need consistency between the impact, the local
frame, the deformed mesh, the part state, and collision. Check separately:
vibration at rest, permanent deformation, a panel opening, and an attachment
breaking. A visual filter does not fix an unstable solver. A hinged panel
opening from a hit is the hinge of [ENGINE_CONTRACT §10](../core/ENGINE_CONTRACT.md#10-cars-cfg-and-suspension), not deformation: the impact releases
its latch and the hinge, not the cage, moves it.

Stock `Part.repair()` owns the wear/tear repair caps and recursively visits the
installed tree. Its native `repair` command performs a one-shot local reset of
that part's deformation, deformed collision/render data, and damage-associated
slot/wheel state, then restores the affected CFG mount frames and base meshes.
Use the script's `attachedParts` coverage and recurse natively only into detached
assembly children that those lists omit; never traverse a child twice. Do not add
a whole-car relink per part, a world reload, or a `GameRef.setState` call. The
latter controls pose and velocities and can release the solver; it is not a damage
repair API.
Keep the latest intentional `setSlotPos` state separate from damage overlays so
repair preserves it even on the same slot. Detached inventory damage must remain
intact. Queue affected link visuals once per repaired part and flush them on the
next existing part update, after the complete scripted walk; do not relink every
ancestor/descendant subtree during each native call.

## Tyre smoke, ground dust and skid marks (#454)

A wheel's ground material names its effects: `skid_PS`, an RPK entry
`particle <cfg>` plus `texture`, and `skid_SM`, an entry `skidmark <cfg>` plus
`texture`. When the SCM leaves them out, the original's defaults apply:
particles:2 (asphalt smoke) and particles:0xA (the asphalt mark).
`surface_effects.cpp` reads each entry once and registers the cloud
(`particle_cloud.hpp`) or the mark style (`skid_marks.hpp`).

- **Clouds.** `parse_particle_cloud` reads the stock Particle System API
  format (`move m0 a b` holds a cloud's position when a is 0; `type 1`
  debris always moves itself): `[STATE]` domains for size, colour, alpha, angle and lifetime, plus
  `type` and `velscale`; `[ACTION]` lines for `source` (rate and domains),
  `gravity`, `damping` and `bounce`. `ParticleSystem` has the shape of the
  stock natives: `init`, `set_freq`, `set_source`, `emit_direct`, the counter,
  `mode_permanent` and `stop`. All particles share one pool of 3000.
  `particles_update` runs once per host frame: gravity, PAPI damping toward
  the wind (not toward rest), slow eddies, and the emission's ground plane.
- **Emission.** One emitter per wheel (`emit_tyre_dust`, `part_runtime.cpp`),
  fed with the wheel state the frame published.
  - Amount, from the tyre: the friction power at the patch, |Fx slip| +
    |Fy lateral| (what the tyre thermal model heats with). It gives the
    cfg's rate per 20 kW for rubber smoke, scaled by the tread surface's
    temperature (10 % cold, full from 200 °C, ramp from 80 °C). Smoke and
    dust feel 15 % of their cfg's gravity, and a puff swells with the square
    root of its age. The patch retunes asphalt.cfg (4-7.5 m, 4-9 s). Loose
    ground (rolling drag) and debris give the rate per 30 kW, with no
    temperature.
  - Place: round the tyre, from the patch up the side the tread runs to (the
    back of a spinning wheel, the front of a locked one), across the tread
    width.
  - Motion and size: the particles leave with 0.6 × the car's speed, the
    tread's surface speed and an outward push from the arch. They turn about
    the axle while young (`Particle::swirl`, fading in 0.35 s), and smoke is
    born at least 1.6 tyre radii wide.
  - Option 33, `tyre_smoke_density` (percent, default 100), scales the
    amount. **Options → Graphics → TYRE SMOKE** and Advanced Video Options
    set it (0-400 %). The part above 100 % shrinks for big long-lived clouds by size²
    × lifetime against asphalt.cfg's.
  - Marks keep the exe's law (0x453680, `tyre_combined_slip`). The combined
    slip σ = √(sin² a + (cos a × k)²) uses 0x43BBD0's stock constants, and a
    mark exists above σ 0.4.
  - Native traffic within 60 m of the camera smokes and lays marks too (the
    driven and scripted cars within 120 m); it still plays no slide loop.
- **Script natives.** The nine `ParticleSystem` natives
  (`natives_particles.cpp`, `script_particles.cpp`) run the same pool:
  `init` resolves the RenderRef type to a cloud, each source action emits in
  its parent's frame at the parent's current pose, with no ground plane. A
  source names a bone of the parent's skeleton to follow it.
  `modePermanent` follows the exe's node flags: an update in which no
  source emits sends a system that is not permanent idle, which drops its
  sources; a positive `setFreq` or a new source wakes it.
- **Traffic audio.** At most two native traffic cars within 40 m play the
  slide loop.
- **Dust colour, sparks and spray.**
  - Loose ground's dust takes its texture's average colour
    (`GroundSurface::dust_color`).
  - A rim on the road and a hit faster than 4 m/s throw the stock sparks:
    emissive streaks, added over the scene.
  - A wheel in the map's water (`water.hpp`, from `GroundRef.setWater`)
    throws water.cfg spray.
- **Rendering.** `draw_particles` runs after the marks in the world view.
  Particles are sorted back to front, and each one is a camera-facing
  impostor of a sphere of smoke (`fs_particle.sc`).
  - Its density is the sphere's thickness eroded by 3D fbm noise.
  - Light reaches it through an optical depth toward the sun, with
    Henyey-Greenstein forward scattering at the thin rims and sky ambient.
  - Lighting is per vertex (`vs_particle.sc`), interpolated across the puff.
    The sun takes a 4-tap lookup in the sun cascades, or in the single fitted
    field indoors. Scenery lamps and car lights, headlights included, take
    the lamp law without its facing term, with their shadow tiles, weighted
    by HG g = 0.55 toward the camera (capped at 3×), so a beam seen through
    smoke glows.
  - The smoke casts a soft shadow (`update_smoke_shadow`, view 139): an
    RGBA16F map seen from the sun, 1024 px, fitted every frame to all the
    smoke (up to 200 m a side). Its RGB multiplies the puffs'
    transmittance, and its alpha keeps by MIN the light depth of their
    fronts. `smokeShadowTransmit` (`scene_shadow.sh`) darkens only the
    receivers below that front: the world (`fs_mesh.sc`) and the car bodies
    (`fs_vehicle.sc`).
  - It is soft against the AO depth prepass, which also runs while AO is off
    for as long as particles live (`soft_particles` debug toggle), and always
    against its own ground plane. Output is premultiplied alpha.
  - `type 1` clouds (grass, pebbles) are small lit sprites of their texture.
- **Marks.** A textured style splits its quads at the texture tiles
  (`height` metres). The first tile is the `uv_end` cap, the rest repeat
  `uv_normal`, and a finished track closes with the cap mirrored. The width
  follows the exe's intensity min(σ − 0.4, 1) from the cfg's minimum to its
  maximum, capped at 0.8 m. Its alpha is the exe's: the colour's alpha × the
  load over the nominal load (at most 1).
  Each style keeps its own ring of `maxcount` × 14 quads (1400 for the stock
  100), so one ground's marks never erase another's.
  Marks multiply the ground (DST_COLOR, INV_SRC_ALPHA) with the texture
  tinted by `skid_SMcolor`. An unset colour gives half opacity, the
  original's default 0x80000000.
