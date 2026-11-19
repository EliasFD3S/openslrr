# PR #48 — vehicle-dynamics migration and mitigation plan

## Baseline, authority and scope

Use maintainer revision
[`8ac2ba9601300b36efca54dd81031fb6bc9de98b`](https://github.com/EliasFD3S/openslrr/commit/8ac2ba9601300b36efca54dd81031fb6bc9de98b)
as the intended model. This supersedes the pre-revision proposals in
[the original plan](https://github.com/EliasFD3S/openslrr/pull/48#issuecomment-6886055973)
and [the missing-spring addendum](https://github.com/EliasFD3S/openslrr/pull/48#issuecomment-6905136573)
where they assume the previous tyre curve, jacking or brake ordering.
The request is to update documentation, revise this plan and review the code;
this documentation revision does not implement the proposed physics/UI changes.

Keep the reference Magic Formula model, its script-input scaling and its
0.75-bounds fallback inertia. Do not restore the old solver or pursue stock
solver parity. Stored instant-centre and patch-tolerance fields are currently
inactive, not evidence that those mechanics work. Reintroducing them would be
a separate handling decision, not an incidental fix for old tests.

The previous numeric reproductions are useful leads, not measurements of
`8ac2ba9`. Preserve their identified revisions in
[the historical validation record](../assets/validation/vehicle-dynamics/README.md).
New implementation work needs a fresh baseline, the same car/input/camera and
settings on both sides, and separately reported build, kernel and in-game checks.

## Current review gates

The GPT review at `8ac2ba9` found:

1. **Strong brakes do not suppress the current substep's drive force.** Tyre
   force is accumulated before braking zeroes spin. In an isolated version
   of the existing 1,300 kg flat-ground test car, pre-applying full brakes,
   then applying full throttle for 3 s, moves the car 4.84 m and leaves it
   travelling 1.74 m/s despite all four final wheel speeds being zero.
   This is a kernel reproduction, not an in-game measurement. Validate and
   correct brake/traction coupling without reverting the intended tyre curve.
2. **The API/test migration is incomplete.** `tire_effective_load`,
   `tire_camber_factor` and `RigidBody::inertia` remain declared but their
   definitions were removed. The vehicle-dynamics suite cannot link; old
   instant-centre, exact-slope and peak-mu assertions also target the former
   model. Retire or replace obsolete helpers/callers together, retain useful
   physical invariants and add assertions for the new force law. Do not add
   fake compatibility implementations merely to turn the old suite green.
3. **The wheel-mount test's old calibration is stale.** The default box
   moments now scale by 0.5625; its offset/central impact-energy ratio is
   0.321149 rather than 0.488961. The old `> 0.4` assertion fails. Verify the
   analytic effective-mass result for the intended inertia instead of
   restoring the old box or simply deleting the test.

Standalone checks actually run: flap hinge 12/12, mass properties 6/6 and
mass response 2/2 pass; wheel mount 6/7; vehicle dynamics fails to link.
No fresh Windows build or game run was performed for this review. Do not
present the older passing suite or captures as validation of this revision.

## Staged work

### A. Re-establish a trustworthy baseline

- Reconcile the declared interfaces and tests above, then add a brake-hold
  regression (preloaded brake, weak/strong drive, forward/reverse, ABS off/on).
- Measure launch, braking, coast-down, full-lock handling, low-speed rest and
  tyre changes with `8ac2ba9`. Preserve the new model's default tuning;
  distinguish a reproducible numerical defect from a subjective handling
  preference.
- Include player, mechanical AI and native traffic. The latter uses the
  separate `native_traffic` force path; the player-car curve is not its driver.
- Revalidate two-frequency operation (120 Hz solver / 60 Hz host), varying
  render rate and long-frame catch-up, without changing simulated time.

Owners: the existing tyre/vehicle modules and their standalone tests. No
new module is needed for interface/test reconciliation or brake coupling.
Complete this gate before interpreting changed crash thresholds.

### B. Three-wheel road pose and garage support (original items 1 and 3)

**Still applicable:** `dynamics_wheels` retains hardware radius when a rim is
missing, but remaining panels do not contribute ground support. Stale tyre
parameters can remain on the hardware corner. Earlier Baiern measurements
put the sill, brake and bumper within a few centimetres of each other;
the disc is not automatically the correct sole support for every car.

Propose bounded support samples from the installed sill/body and remaining
hardware, using their actual transforms. Rest on whichever surface really
contacts the ground, not an invented fourth support. Recompute after part
changes and deformation. Hardware uses a distinct sliding/rolling contact,
not the absent rubber tyre's curve. Start metal friction testing around
0.3–0.5 only as host tuning; do not claim a recovered stock coefficient or
apply it to a remaining outer dually tyre.

Entry points: `dynamics_wheels`, `corner_hardware_reach`,
`dynamics_configure`, `VehicleDynamics::step`. Integrate additional supports
with the same time-stepping policy as stage C; do not create another
force-versus-late-impulse mismatch.

Checks: front/rear missing rims on road and garage rails, lower/higher body
kits, an outer dually still fitted, true air beneath the car, kerbs and
bottoming. Capture actual disc/sill/bumper clearance and corner loads.
Floor contact must not turn ordinary kerbs into high-energy attachment loss.

### C. Garage creep and missing-spring slide (original items 4 and 7)

**Reproduce again:** the earlier garage car crept off inspection rails into
the pit, and the missing rear-right spring caused about 0.05 m/s left drift.
`8ac2ba9` changes forces/inertia but retains the underlying branches:
parked friction removes only a fraction of tangent velocity; a spring-less
corner uses a rigid contact, previous-step load and post-integration impulse.
Its fallback stop remains the CFG spring base rather than a verified
compression limit.

- Determine the real compression stop from the installed assembly/script
  data, with a documented fallback when the spring supplying it is absent.
  The previous 0.19 m proposal is not a universal constant. A corner should
  sag with a missing spring, not be extended above its neighbours.
- Unify support/load evaluation with the other corners. A compliant tyre /
  bump-stop force in the same substep is a candidate; about 200 kN/m was an
  initial estimate, not a validated value. Use the remaining damper when
  appropriate and retain a non-penetration safety stop. Check timestep
  stability and avoid double-counting its force and impulse.
- Add load-limited static sticking near rest, including a held service
  brake. It must not pin a car when gravity exceeds available friction or
  leave a support discontinuity on release of the brake.
- Only then consider chassis sleep: require a supported equilibrium,
  bounded residual forces/torques and no requested motion, not merely three
  contacts and a low speed. Wake on player/AI input, jack/start commands,
  impacts, external pose changes, and installation/removal/repair. Do not
  freeze a physically unstable tripod or hide an unsolved drift.

Owners: `VehicleDynamics::step`, linked/rigid support and `part_runtime`'s
placement/wake lifecycle. Coordinate stage B's contact samples here.
Checks: ten-minute garage-rail idle with three/four wheels; rear-right and
front-right missing-spring cases with a same-location four-spring control;
flat and sloped ground; drive/brake/stop, sleep/wake and a controlled drop.
Compare net COM displacement and support velocities, not just apparent tyre
slip. Unequal load is physically possible; do not require artificial equal
corner loads on a car with a missing spring.

### D. Wear-gauge layout and loss state (original items 2 and 5)

The gauge code is unchanged by the revised solver. The earlier inspection
identified the inboard green strip as the left front-fender glyph (`q`),
not the bonnet: the left fender/side/rear atlas mappings are flipped
front-to-back rather than mirrored left-to-right.

Correct those mappings in the renderer while preserving already-correct
wheel mappings and custom font layouts. Record damage detachments before
`detach_panel` unparents their subtrees, retain their region/bounds for the
HUD, and clear the matching loss record on replacement. A lost engine
includes its gearbox/clutch regions; a lost rim includes its tyre region.
Never-installed parts remain distinct from parts lost in a crash. Decide
whether loss state survives save/reload and document that lifecycle.

**Unresolved presentation choice:** a blip can cover several parts (front
bumper, headlights and battery). The original proposal made any loss turn
it black; the alternative is black only when its principal/largest part is
lost. Neither has been selected. Do not silently choose a policy or treat
black as recovered stock behaviour. Still-attached destroyed parts remain
on the condition scale unless that choice changes too.

Owners: `VehicleHudState`, `hud_damage_parts`, damage-detach/install lifecycle
and the gauge renderer. Before/After evidence: intact/damaged/lost/replaced
hood, bumper/headlight, door, wheel and engine, with matched enlarged crops;
real crash plus deterministic loss tests and a reload check.

### E. Chassis crush under large impacts (original item 5)

The unchanged cage still has 5×2×7 nodes, a radius-dependent direct-crush
cap and springback. Its collision input changes with the new solver, so
re-measure impact speed/impulse, applied deformation settings, plastic crush
at frame-rail height and permanent mount displacement before tuning it.

Make permanent crush respond monotonically over a calibrated range and
reach the impacted cross-section, not only the top row. Keep positive
beam lengths, bounded strain and stable collision refits. Campbell-style
linear crush or the upstream impulse-driven cage are candidates to compare,
not approved parameters. Withdraw the earlier promised 0.5/0.7 m results:
a universal coefficient or global front-column crush cannot be justified
for every car or impact direction from one pole crash.

Owners: `DeformationCage` and `damage_chassis`, not the mass-property model.
Use the SLRR_rev `77b0fb6` cage/impulse work as a reference, not proof of
stock fidelity. Compare frontal, offset, side and rear impacts at several
energies, with permanent render/collider shape, wheel loss and performance
recorded. Then recheck engine-mount outcomes; old approach-speed thresholds
are no longer acceptance values.

### F. Engine release and impact break-up (original item 6)

The current whole subtree leaves at reconstructed pre-contact velocity and
skips carrier contacts while exiting (up to one second). Its loose-body
world sweep is an approximation; it does not decompose. These are unchanged
mechanisms, not evidence that the old 95 km/h flight remains identical.

First improve actual release/obstacle contact accounting: joint-failure
energy, inherited velocity and angular velocity, and the obstacle's local
contact should determine the result. Use the engine's extent in the sweep
and avoid tunnelling or an artificial full second of obstacle immunity.
Do not force every released engine to land beside the car or promise the
old proposed 7 m/s: some pre-impact momentum is physically valid, and a
chassis hit normal alone does not prove that the engine struck that obstacle.

A separate joint model can then detach external assemblies on release or
later impacts. Prefer verified slot/attachment semantics; bounding-box
centres alone cannot reliably classify internal versus bolted-on parts in
modded engines. Keep uncertain/internal components attached conservatively.
The previous 600/1,350 J/kg values are uncalibrated proposals, not stock
strengths or validated per-part thresholds. Bound fragment count and mass,
conserve linear/angular momentum when splitting, refresh parent bounds and
inertia, and preserve ownership/GC/save behaviour and repair semantics.

Owners: damage mounts, `detach_panel` and loose-body contact/publication.
This is the largest coordinated change and needs its joint/fragment
interface reviewed before implementation. SLRR_rev `77b0fb6` supplies the
incoming-velocity precedent but not engine decomposition. Check its current
implementation before any port. Validate modest/heavy frontal and offset
hits, engine contact with a pole/wall/floor, an isolated drop, small fragments,
empty-bay coasting/braking, mechanical AI and native-traffic controls.

## Delivery order and decision gates

1. Stage A: restore reliable API/tests and establish the intended-model baseline.
2. Stages B/C together: contact consistency, missing hardware/spring support and rest.
3. Stage D: gauge mapping and loss policy, after the shared-blip choice.
4. Stage E: calibrated crush and renewed attachment-loss checks.
5. Stage F: obstacle-aware engine release, then separately reviewed break-up.

Keep each stage focused, review its interface changes and run the relevant
regressions before continuing. UI changes need matched actual Before/After
captures; all physics evidence must identify the code revision and settings.
Do not silently implement this whole proposal under a docs-only update.
A person, not the agent, merges PR #48.
