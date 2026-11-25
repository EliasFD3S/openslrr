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
  Width, compound, profile and inflation enter through these script inputs.
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
- Stock tyre at 3500 N: lateral peak 1.14 at 8.2 degrees, 1.05 at 20 degrees;
  longitudinal peak 1.14 at 14 % slip, 0.97 locked; at 6 degrees the lateral
  force falls from 1.11 to 0.51 with 20 % wheelspin; at 4 degrees, 0.95,
  0.98 leaning 3 degrees into the force, 0.92 away. GT III rear (295/20,
  stiffness 40): peak 1.56 at 6.4 degrees.
- Low-speed caps: the force grows with the sliding speed, 0.95 (lateral) /
  0.85 (longitudinal) × load per m/s, times max(1, road speed / 2 m/s). Before
  #418 that factor was missing, so the cap also bounded the cornering
  stiffness at speed (16 per radian at 60 km/h, softer than the stock tyre).
  `TireTuning` scales the lateral and longitudinal force (grip), the slope
  (lateral response) and the load sensitivity.
  `setRollRes` supplies the wheel's rolling torque.
- `VehicleDynamics` feeds the curve a relaxed slip angle: the tyre's lateral
  deflection builds over 0.3 m of rolling (`kLateralRelaxation`), integrated
  implicitly, a 12 ms lag at 90 km/h.
- Not used yet: the surface friction and rolling resistance of the map's SCM
  materials (`friction`, `rollres`), which multiply the tyre's in the
  original.

Wheel alignment enters as camber (above); the load itself is no longer
scaled. The copied `setCPatch` tolerance (`patch_angle`) is not read by the
solver. Likewise, `setInstantCenter` values and the
`WheelState::jacking` field are retained but have no active load effect:
there is no instant-centre anti-dive/anti-squat force or jacking feedback.
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
brake application. ABS reduces the service budget and can release an
already-locked wheel while the road moves. Only longitudinal force retains
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
by wheels) and hinged panels (whose floor contact remains unsupported),
respects slot poses/deformation, and refreshes after assembly changes,
damage and repair. Hiding a mesh does not remove its physical support. In an
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
without bodies (stock running gear, rims and tyres) add no mass. Crush zones
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
