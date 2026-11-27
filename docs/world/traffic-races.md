# Traffic and races: remaining-work audit (#197)

**Partial implementation; not full #197 gameplay acceptance.** This audit starts
from OpenSLRR `d1c719d18d6e76db0f3d6b2a8ff54b44a820bdfd`. The accompanying
shared-contact change adds native/mechanical chassis pairs and all-car movable
body contacts, with actual impulse data for damage/detachment. It also keeps
static-world contact/damage active for released driverless cars, including
existing distance accumulation and controlled-car cockpit response.
Complete traffic behavior, audio parity, driver seating and day/night/ROC
acceptance remain outstanding. Keep #197 open.

## Reference and evidence boundaries

Refreshed through GitHub on 19 November 2026 (UTC):

- SLRR_rev `v221`: `7441a697f3b7b2d0ff7ddbe3f20f2e29a03165aa`.
- SLRR_rev `traffic`: `bea8ce06eabef78ad0c25dde00e17a48765199b5` (unchanged).

The implementation comparisons below use the refreshed v221 source, not the old
`88130b3` snapshot. These are upstream gameplay models, not recovered original
MWM behavior. They are not permission to transplant Three.js/WebAudio/worker
ownership. The installed MWM `Bot.java` was also inspected: `enterCar` sends
`renderinstance` before `controllable`; `followCar`/`stopCar` use AI_follow;
`startRace` and `followSplineTrack` include an opponent ID.

Reported visual/audio defects remain reports unless a cause is established
below. Source evidence proves an omitted path, not its complete contribution to
a particular playtest. Prior fixture results in
[shared-races.txt](../../assets/validation/shared-races.txt) are historical, not new
validation of this revision.

## Contacts and motion: baseline findings at d1c719d

| Scenario | Current implementation and evidence | Consequence |
| --- | --- | --- |
| Physical AI versus static walls/buildings/floor | `src/resources/part_runtime.cpp:configure_drive` feeds native or mechanical controls into the same VehicleDynamics; `advance_chassis_tick` advances terrain/suspension and calls `collide_chassis` for released, driven cars. | Not every environment contact is absent. A solver call alone still does not establish correct visible movement. |
| Released/coasting car without Controller or brain | `configure_drive` clears torque/native drive then returns for an unowned car. The old wall-response gate required torque, native drive or a lost engine. | The body continued integrating while static wall response was skipped. The local fix gates response on `physics_released`, independently of its driver. Unreleased catalog placement stays outside that path. |
| Player/AI and AI/AI | `collide_chassis` queries static scene shapes/triangles only. `src/resources/scene_instances.cpp:scene_instances_prepare_physics` excludes vehicle materials. `src/physics/vehicle_dynamics.cpp:VehicleDynamics::step` receives terrain, not a peer-car set. | No chassis/chassis contact production exists. Changing a collision-category flag cannot create equal/opposite pair response. Adding car meshes to the static cache would instead create immovable obstacles and self-contact risks. |
| AI versus movable map props/free PhysicsRef bodies | `part_add_dynamic_native` assigns the controlled car as carrier; `advance_loose_panels` rebinds map props to that car, and `advance_free_bodies` likewise selects only the controlled car. Dynamic bodies are excluded from the static scene cache. | AI can wake a prop through its static-scene query but does not receive a dynamic contact with that promoted prop. This needs an all-vehicle contact owner, not another static collider. |
| Unsupported native chassis | `src/resources/traffic_runtime.cpp:add_car` selects external motion when there is no Part chassis or not exactly four CFG wheels; `race_traffic_tick` then publishes poses, and `advance_chassis_tick` skips external motion. | Transform-driven fallback remains real, including unsupported wheel counts. It must not be mistaken for native four-wheel spring/tyre motion or presented as full physical parity. |

### Why a shared-contact port is not a local enable switch

`SLRR_rev:src/engine/vehicle-physics-world.ts` advances full cars on a shared
substep schedule and resolves each unordered pair once. Its
`SLRR_rev:src/engine/vehicle-pair-test.ts:vehiclePairContact` computes two-body effective
mass and contact impulse; the world applies mass-weighted separation.
`SLRR_rev:src/engine/race-driver.ts:raceTrafficContact` passes opposite impulses to the
full-car and traffic impact owners.

At the baseline, OpenSLRR already had a two-body `RigidBody::contact` primitive,
but callers did not produce car pairs. Its `VehicleDynamics::Impact` carried
only point, normal and speed.
`src/resources/part_runtime.cpp:damage_wheel_mounts` and `damage_engine_mount`
use `src/physics/wheel_mount.cpp:contact_impulse_energy`, which assumes a body
hitting an immovable wall. `pre_contact_velocity` reconstructs velocity under
the same assumption. Passing the full relative closing speed to both cars would
miscompute mount damage and detached-engine velocity even if momentum response
looked plausible.

A faithful port therefore needs coordinated changes to the vehicle-step/contact
ownership and the impact-data contract consumed by chassis damage, mount failure
and detachment, together with replacing the controlled-car-only dynamic-body
selection. Player driving, mechanical/native AI, released parked cars and loose
bodies are all callers/participants. Appending a pair pass after already
published per-car ticks or treating a moving car as static does not preserve the
upstream state/ownership contract. The coordinated implementation below follows that ownership change instead of
silently replacing it with a collision-filter toggle.

## Shared-contact implementation

- The existing 60 Hz host accumulator/control API is preserved. Every accepted
  host tick now advances all cars on the same two 120 Hz substeps; each unordered
  pair resolves once before final body/wheel publication. World, full ancestor
  visibility and incarnation checks exclude hidden/recycled/other-world cars.
- `src/physics/vehicle_contacts.cpp` reuses validated OBB SAT, reciprocal contact
  points and two-body effective inertia. Equal/opposite impulses and capped,
  inverse-mass separation follow the pinned upstream host model. A bounded
  translational sweep is an OpenSLRR extension; it holds current orientations,
  uses at most 64 samples and is not exact rotational/grazing CCD.
- Existing wall-impact callers keep their default contract. Pair damage carries
  actual normal impulse and each participant's pre-impact rigid motion. Legacy
  deformation/wear severity is translated to the participant's equivalent wall
  speed, rather than applying full pair-relative speed twice. Mount energy uses
  the actual impulse; engine detachment retains the first queued pre-hit motion.
- Sleeping parked cars may be woken by contact without acquiring a driver.
  Scripted relocation/park remains authoritative even after prior simulation;
  passive bodies are refreshed rather than republishing stale solver poses.
- Promoted map props, detached assemblies and free PhysicsRef bodies query all
  relevant active cars. Their old carrier is retained only for temporary
  freshly detached engine exit-ignore, not contact ownership. Sleep/reach and
  native-prop demotion consider all cars. Damage/wake events commit only after
  the loose/free body's finite step succeeds; numerical faults remain local.
- Chassis contacts use conservative chassis OBBs, not individual body-panel
  meshes. Loose bodies keep their existing authored-mesh-versus-car path and do
  not collide with one another. Unsupported non-four-wheel traffic still uses
  external motion. The full upstream impact/recovery/queue policy is not added
  by this contact change.

## Presentation and audio

- **Police completeness is not yet reproduced.** Installed
  `cars/misc/Police.rpk:0x6` resolves to the native Police_V3 chassis. Its CFG
  lists nine stock body/siren parts with matching mount slots; the referenced
  native child resources exist. `src/resources/part_runtime.cpp:part_create`
  calls `install_cfg_stock` for native vehicles, and the generic slot matcher
  installs compatible stock parts recursively. This is evidence against
  declaring the whole stock-assembly path missing, not proof of a complete
  rendered police car. A police-specific runtime assembly/material/ownership
  comparison is still required. Upstream `SLRR_rev:docs/java-traffic-models.md` records
  assembling Police as well as the regular traffic catalog.
- **Mechanical AI audio is not globally disabled.**
  `src/resources/part_runtime.cpp:part_physics_advance` updates engine audio for
  every car record, not only the controlled car. The real SfxTables and RPM/load
  path must be checked in a script-loaded reproduction before attributing all
  missing sounds to ownership.
- **Native traffic engine loop (host model).**
  `src/resources/part_runtime.cpp:update_traffic_engine_audio` gives every
  released native car (traffic, fake racers) a loop from the three
  `cars/traffic/sounds/engines/diesel_5.0L` samples, cross-faded by an RPM
  modelled from speed through a stepped gearbox. The gain falls with the
  distance to the world camera; the loop is a placed voice at the car's body
  with its velocity (#390), so the host pans it, adds Doppler and the
  environment reverb while keeping this gain law (`attenuate = false`). Pitch follows
  the CFG mass plus a stable per-car offset, so heavy and light cars differ.
  The CFG `sfx_loop` (`PartConfig::sfx_loop`; Erbilac and Wagon, a radio
  music loop) plays as that car's own loop within 40 m. Only the four nearest
  cars within 80 m hold voices. This is a host fallback modelled on upstream
  `SLRR_rev:src/audio/traffic-audio.ts`, not vehicle-specific SfxTable parity; the
  Police main CFG has no loop of its own.
- **Racer seating is explicitly omitted.**
  `src/resources/race_brain.cpp:race_brain_command` accepts `renderinstance`
  without retaining its render ID. `src/resources/part_runtime.cpp:pose_driver`
  seats only the controlled car. Upstream
  `SLRR_rev:src/render/java-vehicles.ts` resolves the brain's render and binds it for a
  mechanical vehicle; it explicitly leaves native-only fake racers unseated.
  Thus visible drivers in fake racers cannot be claimed as an existing upstream
  mechanic. A local pending-render/seat binding can reuse general seating, but
  must not reuse the player's world-wide same-type duplicate masking: multiple
  independent racers can share a character type. Java retains render ownership,
  and the pre-controllable command order and teardown need coverage.
- **Lamp support is not AI lamp behavior.**
  `src/resources/part_runtime.cpp:part_lamp_lit` currently exposes night-running
  light state (flag 4), not independent brake/reverse/indicator control.
  Upstream `SLRR_rev:src/render/traffic-lamps.ts` has steering-derived indicator state,
  blinking and brake/reverse channels; its documented indicator policy is not
  predictive route/lane-change intent. This distinction matters before claiming
  the requested indicator port complete.

## Traffic, signals and race policy

| Requested area | Finding / remaining work |
| --- | --- |
| Full traffic Bot | Upstream `SLRR_rev:src/engine/bot.ts` contains impact/settle/recover/reverse/ram modes, oriented-hull following, queue restart hysteresis/delay, junction clearance and a per-yellow-phase decision. The host has a subset: its `followingSpeed` corridor (#324) for physical traffic and pursuers, eased stops at signals and halts, a held brake when stopped, a distance-based yellow decision and reverse recovery. Impact/settle/ram modes and restart delays are not ported. |
| Route branches / lanes / speed groups | Physical `race_brain.cpp` traffic routing takes `e.next.front()` on each rebuild. Branch-first random selection is in `traffic_runtime.cpp`'s external/coarse path, not the full physical controller. Upstream retains a route cursor, chooses branch then lane, blends a safe lane change and applies urban/rural speed groups. The host's fixed physical cruise limit is not that policy. |
| Signals / signs | Shared lamp/driver phases and missing-lamp STOP already exist. Upstream `traffic-control.ts` has explicit signal/STOP/YIELD fixture rules; STOP/YIELD waits are themselves a simplified host policy without crossing-traffic checks. Host fixture production remains trlight-only. Do not infer sign identities from arbitrary numbered decorative resources. Repeated approach-specific cycles and pause/resume still need actual game validation. |
| Population / streaming | Host count scale, radii, rounding and initial candidate rules are documented policies. After the first fill, cars neither appear nor recycle inside the world camera's view cone (#324); occlusion is not tested. Upstream `traffic-stream.ts` also checks oriented size/velocity-based spawn safety. Neither implementation is an original-game population oracle. |
| Police / pursuit | Upstream `SLRR_rev:docs/traffic-java-bridge.md` explicitly leaves police wake/follow/horns/drivers/reinsertion chase flow incomplete; source does not implement the MWM AI_follow/AI_GoToTraffic/AI_GoToTarget commands. Host follow behavior is an extension (#324): remTrafficCar keeps the woken car, the Bot's brain replaces the traffic driver, the offset is in the player's frame and getRouteLength is direct for nearby cars, so City's chase, PULL OVER message and fine run. `ai_info_entertraffic` is not sent, so a scout sent back to traffic is only replaced once 600 m away. |
| AI fields / splines | Behavior mode and trailing model values are stored without tuning. The host parses AI_spline width but does not translate width/key speed metadata/opponent ID into driving policy; AI_race's opponent ID is not used. Refreshed upstream has no AI_spline/AI_params implementation to port. Original Bot command text confirms argument order, not native semantics. |
| Day/night/ROC | Existing route/physics fixtures do not exercise the complete challenge, meeting, countdown, result and return state machines. Native registrations or a reached ROC gate are not those flows' acceptance. Preserve the consolidated issue's unchecked gameplay items. |

## Verification record and remaining evidence

The local regression `released_car_without_driver_hits_static_wall` releases a
synthetic native chassis through `part_apply_state`, without a Controller or
brain, towards a static PhysicsRef wall. Nine additional real-assembly runtime
cases cover native/mechanical and retained-player/AI pair contacts, passive wake,
relocation/mass, filtering, free/promoted bodies, damage/detachment, pause,
handoff/deletion and explicit recovery. Those are separate from complete race
state-machine acceptance.

Actual checks and build-only staging deviations are recorded in
[traffic-race-audit.txt](../../assets/validation/traffic-race-audit.txt). Matched
[Before/After full-career captures and natural AI/AI contact evidence](../../assets/validation/traffic-contacts/README.md)
are supplied with clearly labelled read-only diagnostic provenance. The images
show rendering/static-contact smoke, not vehicle-pair impact footage; the
60-simulated-second full-career trace records actual pair response. No audible
comparison, complete police presentation capture or full day/night/ROC
playthrough is supplied. No menu/layout changes are made. The newly eligible released bodies also reach
the existing damage/hinge/mount logic and distance accumulation; a controlled
engine-less car also receives the existing cockpit lean. Those are consequences
of removing the engine gate, not newly implemented damage or camera models.
This validated partial change is not acceptance of the entire #197 checklist.
