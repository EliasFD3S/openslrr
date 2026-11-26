# Vehicles, parts and driver: issue notes

Verified notes on physics bodies, steering, stock part data, the
dashboard, nitrous, opening panels and the seated driver. The driving
model is in [driving-model.md](driving-model.md); the physics clock and
engine calibration in [physics-tick.md](physics-tick.md).

## Per-body physics failure containment (#75)

The rigid-body kernel deliberately throws on invalid mass/bounds/orientation,
non-finite frames and non-finite integration state. Keep those kernel checks:
the owning runtime now contains failures at each chassis, loose-part and free
PhysicsRef body rather than allowing a frame pump to unwind a Java dialog or
terminate the host. std::exception failures are reported and handled locally.

Chassis configuration is transactional (configure/set_frame on a candidate).
An invalid chassis is suspended with physics_pending_placement cleared; a step
failure restores its saved rigid body and the last finite local pose, resets
motion and interpolation and leaves other chassis ticking. Pose orientation is
reconstructed as in dynamics_sync. With no valid pose to restore, the node is
hidden. The normal start/wakeup path can retry simulation; unchanged bad data
will suspend it again. A chassis failure is logged once per record lifetime.

Loose-body creation also configures a candidate. Failed native map promotion
cleans partial nodes, keeps the ordinary scenery and suppresses repeated retries
for that resource until the part runtime is reset. A failed active loose body
is removed; a promoted map prop returns to its authored placement. A failed free
PhysicsRef restores its last published finite pose and is removed from simulated
bodies. Failed loose/free contact steps restore the collision partner's saved
rigid body too. These guards do not roll back arbitrary script/asset mutations
or promise recovery from allocation failure.

GameRef.setMatrix rejects non-finite position/rotation before mutation. WheelRef
setters reject non-finite scalar or Vector3/Ypr fields atomically; finite values
retain their existing semantics. Numeric overflow generated later in the solver
is caught before an invalid body frame is published. Free bodies now have an
explicit first-configuration flag (live_id can legitimately still be zero), and
free-body state plus failed-native-type suppression reset with part registration.
No native signature is added; the kernel and normal physics tick remain intact.

Validation: physics_recovery is 0/8 passing on f3638a1 and 8/8 on the fix. Its
isolated synthetic RPK/CFG/SCX fixtures cover NaN poses, malformed free-body
bounds, invalid free frames, zero-height chassis bounds, corrupted chassis
pose/orientation, wheel updates, extreme finite velocity overflowing the render
frame, and transactional invalid map-body creation. A native pump callback
returns after the numerical failure while a healthy chassis keeps ticking.
This is not an automated real Dialog.display test. Active detached-panel failure
and contact-partner rollback paths are implemented but not separately fault-
injected by this suite. map_props and physics_position regressions pass. The
Release build and native audit checks pass. In-game driving/collision/dialog
smoke validation is pending. Evidence: assets/validation/physics-recovery.

## Initial garage steering (#125)

The setsteer command updated steering_input/request but placed attached parts
using the existing dynamic hub matrices. The steering wheel read the new scalar
while the wheels kept the prior pose until another operation recomputed it.
When dynamic_frames are already available, setsteer now calls pose_wheels,
which rebuilds steered/rolling frames from the current suspension state and
places/relinks the assembly. It does not integrate physics, change chassis pose,
repark the car or reset wheel spin. Before dynamic frame initialization the
existing rest placement path is preserved; later parking reads the saved steer.

Release x64 compiles; knowledge references 219 checked, zero errors. Source
trace: Garage.java sends setsteer -0.7 on entry and mechanic-mode changes.
No automated assembled-vehicle regression or agent in-game validation claimed.
Manual check: enter/load the garage and inspect wheel braquage before touching
any part; change mechanic modes and return from catalog/dealer, then confirm
normal driving steering still works. Notes stay in this file.

## Anti-roll bars: stock data behind the catalog branch (#300, PR #307)

Read-only scan of `game/parts.rpk` (RPAK TOC; the 16-byte TOC header sits after
the dependency table, as in `src/resources/rpk_index.cpp:RpkIndex::parse_rpak`):

- The stock RPK already holds 48 bar resources. They hang under
  `0x0220 "anti-roll"` → `0x021F "_hidden_"` → `0x0019 "runinggear"`, in nine
  brand folders `0x00DC..0x00E4` (Baiern … Shimutshibu). The stock Catalog only
  browses `0xF229` (Suspensions), so the hidden branch never showed. PR #307
  adds `parts:0x0220r` as a second root of the Running Gear/Suspension page;
  no RPK byte changes. `0xF232 "anti-roll"` under Shocks is an empty folder.
- The brand folders live in parts.rpk, so the Catalog's car-pack filter
  (`id() >> 16` against car packs) never hides them: every car sees all 48 bars,
  as it already does for the Suspensions brand folders.
- Each bar RSD is `script parts\running_gear\scripts\sways\swaybar_*.class` +
  `native part parts\running_gear\scripts\swaybar.cfg`. That CFG has
  `compatible 0x00004000 1`; `parts:0x4000` is the base class node
  `Swaybar` (`script java.game.parts.rgearpart.Swaybar`). Car mounts use
  `attach 0x00044000 1`, dependency 4 = parts.rpk. Fit is therefore generic:
  any bar fits any 701 (front) / 702 (rear) mount; the front/rear in the name
  is not a restriction.
- Only ten stock chassis CFGs author mounts: Duhen ×4 (701+702), Focer ×3
  (701+702), Einvagen ×3 (701 only). Other cars can buy a bar but cannot fit it.
- All 48 scripts set `force = 25000`, `damping = 2500`: every bar is
  functionally identical. Stock data also gives the six Ishima bars and the
  Duhen 1.8/2.0/2.2 bars `name_prefix = "Duhen SunStrip 1.5 DVC"`, which the
  patched `Swaybar.calcStuffs` now shows in their names.
- The Java `updatevariables` still has its `addrollbar` commands commented
  out; the host reads `force`/`damping` from the installed part instead
  (`src/resources/part_runtime.cpp:configure_anti_roll`).
- Every stock racer script lists its bars in commented-out
  `stock_parts_list_RGear_sways` lines (Focer RC 300 also `stg_1_`). `Chassis`
  already pushes that list with the other running-gear lists, and
  `src/resources/part_runtime.cpp:add_part` destroys a part no slot accepts, so
  a listed bar without a mount adds nothing. #311 adds the missing mounts to
  the 17 other racer chassis CFGs (Einvagen gets 702 only) and uncomments all
  24 lists. Stock lists that named the wrong bar are corrected: Duhen 1.5 rear
  `0x41FF` to `0x017C`, Ishima Enula WRZ `0x41FF` to `0x0195/0x0196` (stage 1
  `0x0197/0x0198`), WR SuperTurizmo `0x41FF` to `0x0197/0x0198`.
- **The installed MWM 2.2.1 car classes are not built from `sources/`.** MWM
  added stock neons to the NOS branches: five `addPart( parts:0xD2xxr, "<color>" )`
  after the canisters of each branch (`parts:0xD273` Red, `0xD285` Green,
  `0xD288` Blue, `0xD291` Cyan, `0xD294` Aqua, `0xD279` Pink, `0xD276` Purple,
  `0xD282` Orange, `0xD267` Yellow, `0xD270` White; neon RSDs under
  `parts:0xF24B`). The count per branch varies (5, 7 or 8). The SuperDuty top
  branch replaces the four canisters with two bottles (`parts:0xD215` Botle,
  `0xD339` Double Botle), ten neons and `cars.racers.SuperDuty:0xD1` rollbar
  (the RID form must match the `stg_` list one exactly, or the constant pool
  differs). No MWM car
  source ships, and recompiling `sources/` would drop them. The bundled compiler
  reproduces original classes byte for byte (only header bytes 9-11 differ;
  source line numbers are encoded in the tree), so a reconstruction is proven
  by an identical output. The reconstructed sources are in
  `patch/cars/racers/*_data/scripts/src/`; see scripting/compiler.md.
- **The local `game/` may hold an installed patch.** On 2026-11-22 the patch
  (sl, parts and ten car classes) was copied into `game/` for in-game tests, so
  tests that expect stock classes failed there. To test against stock, build a
  hard-linked view whose changed files come from the untouched MWM 2.2.1 copy
  and run the test executable from it; `tests/test_swaybars.cpp` builds its
  patched game view the same way (patched files replace their link, never
  written through).

## Dashboard pins: CFG `cockpit_rpm` / `cockpit_speed` (#301)

- Format, from the MWM executable (SHA-256 `b284a7a5…2b03`, read-only
  disassembly): `cockpit_rpm` (0x00444B60) and `cockpit_speed` (0x00444CB0)
  scan `x y z yaw pitch roll render offset factor low high` with
  `%f %f %f %f %f %f %d %f %f %f %f`. The game's own scanner (0x00553890)
  reads `0x` hex for `%d`, so stock ids parse. Defaults (constructor
  0x004467D1): offset 2.2, factor -0.0005 rpm / -0.02 speed, low -2.2,
  high 2.2. The render is resolved as type 14 against the owning pack.
- Update (0x0043C0A0, mode 1): `angle = value * factor + offset`; below `low`
  it becomes `low`, otherwise above `high` it becomes `high` (no swap). The
  pin is posed at `(yaw, pitch, roll + angle)`, which with
  `src/resources/part_config.cpp:part_pose_matrix` (yaw, pitch, then roll) is
  a turn about the posed local Z axis. Values (0x0045283C): rpm is
  `|engine rad/s| * 9.5493` (0x005F0C14), speed `|velocity| * 3.6`
  (0x005F0C04), the same sources as `osd_rpmpin` / `osd_spdpin`.
- The modding tool's `needles.ts` description (field order, defaults, Z spin,
  km/h) matches the binary. Its "SLRR units, pi = 3.147" remark has no
  counterpart in the engine: the angle goes straight into the roll in radians.
- Stock data: 17 racer cfgs in `patch/` and 24 in `game/` carry both lines,
  all with pin `0x00020037` = `cars:0x37` (`cars\meshes\pin_small.SCX`).
  Enula, Nonus, SuperDuty pitch their pins; Einvagen rests at its `high` 1.5
  because its offset 2.2 is above it.
- OpenSLRR: `include/slrr/part_config.hpp:CockpitNeedle`, parsed in
  `src/resources/part_config.cpp:parse_part_config` (a line without pose and
  render, or with a bad number, goes to `unknown` and does not invalidate the
  cfg), angle in `src/resources/part_config.cpp:cockpit_needle_angle`. The
  chassis (`native_kind == "car"`) gets one RenderRef child per defined pin in
  `src/resources/part_runtime.cpp:render_part`, kept in `Record::needles`, not
  in `renders` (deformation, lamp snapping and HUD bounds must not see a part
  that turns). `src/resources/part_runtime.cpp:pose_cockpit_needles` sets
  their matrix every `part_physics_advance` after the steering column;
  `part_destroy` removes them and `part_visit_gc_roots` marks them.
- Build 606 only updates the pins in a view whose flag `+0x3C` is set and
  destroys them otherwise (0x00452836, view record `car + idx * 0x2B8 +
  0x11C`); the flag is not decoded (probably the internal camera). OpenSLRR
  keeps them on every car with the lines, in every view.
- Tests: `tests/test_cockpit_needles.cpp` (suite `cockpit_needles`): parse
  defaults and limits, stock Baiern (flat) and Enula (pitched) at rest and
  driving, pivot and axis fixed, pins removed with the car.
- Pin material: `cars\meshes\pin_small.SCX` is SCX3 with one group of flags
  `0x101` (SCX3 blend) on `cars	extures\Pin.dds`, a DXT5 card that is clear
  except for the needle. The vehicle path
  (`src/render/bgfx_lighting.cpp:draw_instance`) only sent the SCX4
  transparent bit `0x40000000` and flag-256 flames to its blended pass, so the
  card was drawn opaque: a black square behind each needle. SCX3 blend now
  blends on any vehicle mesh without `light_flags`. In the MWM `cars/` and
  `parts/`, the other SCX3 blend groups are lamp glows (`lampglow.SCX`,
  Focer and Enula light meshes), drawn with `light_flags`, so unchanged.
- In game (`--bench-drive`, ValoCity, Baiern, F1 with the #318 fix): both pins
  show on the dashboard dials at rest, thin needles with no card. The chase
  view and the overlay pins are unchanged.

## Nitrous driving and exhaust (#303, #304)

- Ownership: `src/resources/part_runtime.cpp:configure_drive` takes mapped axis
  31 from the controlled mechanical car. Native remaining starts at zero;
  `reload` fills the script capacity, while refresh only clamps. `Canister.actual`
  remains unchanged script save data. Stock Track reloads on entering driving;
  arbitrary save/load does not preserve the native aggregate. HUD fill is the
  remaining/capacity ratio, only for positive injector consumption.
- Physics: `src/physics/vehicle_dynamics.cpp:VehicleDynamics::step_drive` keeps
  dry torque and accepts an optional wet dyno callback/input, blending after the
  predictive limiter. No #215 dyno or torque-gain correction is copied. The Part
  owner consumes `RPM * .0002 * input * limiter * dt * consumption_nitro` once per
  fixed solver step. The stock injector's nominal RPM/throttle fields are not
  exported. Audio uses the configured one-shot or default `sound:25` once per
  press; release, pause, reset and teardown do not create extra triggers.
- Evidence: direct read-only MWM executable disassembly, SHA-256
  `b284a7a53909d0b9ce59d59ddc11e14df40a7eb74d86fa76efb06305aadc2b03`,
  plus MWM Chassis, Canister, NOSInjectorSystem, Track and ControlSet sources.
  Addresses, version distinctions and upstream checks are in
  [EXTERNAL_RESEARCH §7](../research/EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev).
- Actual authored asset: mufflers resolve `particles:1E` through `MW_Mod:28`
  (`NOS_Flames.SCX`, SCX3 flags 0x101, 48 vertices/16 triangles) and `MW_Mod:27`
  (`NOS_Flames.png`, RGBA8 425x554). Independent SCX parsing and full PNG
  CRC/zlib/filter decode establish alpha 0..209, not an opaque card. PNG uploads
  are premultiplied; only flag-256 transparent vehicle sections use the matching
  blend and no depth write. Flame geometry is excluded from lamp-glass snapping.
- Flag-256 mesh/fake-light visibility uses eligible injection, >1000 RPM,
  throttle >.01 and a shared deterministic 30% simulation-clock pulse. This is
  explicitly not the original per-render-child RNG cadence. Collection and
  cameras do not spend fuel or resample it; paused physics freezes it.
- Baiern's pipe carries two separate `particles:1F` FakeSpot children. All
  placements survive collection. `src/render/bgfx_lighting.cpp:LibBgfxBackend::draw_nitro_flares`
  draws their authored central atlas glow with source depth; they never enter
  physical road-light arrays. The Flares option gates this view-only draw.
  Sizing follows the existing sun approximation, with mode-2 atlas/cm-offset
  evidence from SLRR_rev `2663d686`. Original size units, directional convention
  and ray/ghost fidelity remain unverified; no arbitrary backward rotation,
  floodlight, replacement texture or particle emitter is introduced.
- Regression entry points: `tests/test_nitrous.cpp`, `tests/test_sdl_input.cpp`,
  `tests/test_vehicle_dynamics.cpp`, `tests/test_script_lights.cpp` and the
  `scene_shadow_gpu` nitrous case. Compilation and actual gameplay captures
  must still be reported separately; these sources alone are not pixel acceptance.

## Doors and hoods open with build 606's spring (#327, from #204)

- Before: a script open/close (`Part.flap`, `flap_toggle`) drove the hinge
  with a critically damped servo (9 rad/s): the panel glided to its stop like
  an interpolation. In the original game it visibly shakes.
- Build 606 (read-only disassembly): `flap_toggle` (0x00474B00, switch on the
  command) only sets flag 2 (open) or 4 (close) at joint+0x88, bit 1 being
  the latch (0x0043F280 releases it). The flap solver (0x0043F850; the drive
  at 0x0044030D) adds `gain * (target - edge) * dt` to the free edge's
  velocity, with no damping term there: open targets the edge at
  `0.05 lo + 0.95 hi` of the CFG travel and close the `lo` pose, both with
  gain 60 (0x42700000); otherwise the gain is a per-joint value at +0x34,
  used only above 0.1. Gravity acts on the same edge, so a heavy hood sags
  about 9.81 / 60 m below the target. Not decoded: the term scaled by
  joint+0x7C at 0x0043FF05 (likely a damping), and the CFG field order of
  +0x24 / +0x28 / +0x34.
- OpenSLRR (`src/physics/flap_hinge.cpp:flap_step`, `FlapTuning`): the open
  hold is `script_spring * (0.95 upper - angle) - script_damping * rate`
  (60 s⁻², 2.3 s⁻¹, a host damping ratio near 0.15 in place of the undecoded
  term), the close drive the same spring aimed 0.35 rad through the closed
  pose. The opening counts as finished once the panel has been free for 1 s
  and moves under 0.05 rad/s; the weaker door-check limit applies only then.
- Simulated (Baiern door, 1.134 rad travel): it reaches the stop at 0.25 s,
  falls back to 0.95, returns to the stop at 0.8 s and rests near 1.05-1.08
  by about 1.2 s. The Baiern hood rests near 0.66 of its 0.873 travel. A close
  latches in 0.2 s. Tests: `tests/test_flap_hinge.cpp` (12/12), including the
  overshoot onto the stop and the fall back below the target.

## Driver duplicate visibility (#112)

`DriverSeat` now owns a `GameRefRenderMask`. The duplicate-render mask is rebuilt
when the scene revision, root, kept render or resource type changes, rather
than undoing visibility and breadth-first walking the world on every pose.
Creation/upsert, removal, clear, alias binding, changed parenting and resource
type changes invalidate it. All production GameRef type assignments use
`gameref_set_type`; direct metadata mutation outside these entry points must
explicitly use upsert or the appropriate setter. Upsert conservatively
invalidates even an existing entry, as callers may initialize more metadata
afterwards. Ordinary transform changes and redundant parent assignments do not
invalidate. Frequent scene mutations can therefore still cause repeated walks;
there is no measured in-game frame-time claim.

Rebuild traversal has no 4,096-node cutoff and visits each canonical key once,
terminating even with aliases/cycles. Already-empty renders are left alone.
Release restores only entries hidden by this mask, checking their per-creation
incarnation so a recycled key or cleared world is never unhidden accidentally.
Incarnations continue across clear and do not alter live IDs or Java GC roots.

Release build and driver_mask, live_lookup, script_safety, ped_crossing pass.
The driver-mask fixture checks a duplicate after 5,000 nodes, 10,000 unchanged
cache hits, motion/no-op parenting, late creation, type/reparent/world changes,
alias/cycle traversal, original hidden visibility, removal/reused keys and
clear/release. This tests the shared production mask used by pose_driver, not
seated animation or a full garage transition. Evidence:
`assets/validation/driver-mask/after.txt`. No production Java/native API change.

## Driver head in the cockpit camera (#320)

- `src/resources/part_runtime.cpp:pose_driver` aims the seated driver's neck
  at the car's first cfg camera, 7.2 cm below it, so camera 0 sits in the
  head. The cockpit camera also leans with acceleration (`cockpit_offset`, in
  `part_camera_pose`), and the face then filled the view (seen in a bench
  drive: the driver's face from the front at full throttle).
- While this car's `_hostCockpitCamera` is camera 0 and is the scene camera
  (`fullscreen_script_camera`), `pose_driver` shrinks `Bip01 Head` to 1/1000
  about its joint with `src/resources/character.cpp:character_scale_bone`.
  Hair and face follow the head bone; neck, body and hands stay. The pose is
  rebuilt from bind every frame, so the head is back in every other view,
  for other cars and for the other internal cameras.
- Not checked: whether the original exe hides the head the same way (the
  view flag `+0x3C` of #301 is a candidate). The head is also missing from
  the car's shadow and mirrors while in the cockpit view.
- In game (`--bench-drive`, ValoCity, Baiern, F1 with #318): before, the face
  in front of the camera on six shots at full throttle; after, none. F3 back to
  the chase view runs unchanged.

## Driver hands on the wheel (#302, P2)

- `src/resources/part_runtime.cpp:pose_driver` aims each hand at the rim at
  ±1 rad from the top (radius = rim radius − 1.5 cm). It used to put the wrist
  7.8 cm behind that point along the seat's +Z and leave the hand on the
  forearm's bind orientation.
- Measured on stock Baiern and Enula, wheel centred and at both locks (bench
  drive, temporary log; the "palm" is the wrist plus 8 cm along the hand
  bone's X axis): the wrist target was always reached (reach 44–52 cm for a
  62 cm arm, no clamping), but the palm sat 2–3 cm inside the rim, 0.5 cm at
  Enula's right lock. On a raked column the seat's +Z is not the wheel axis,
  so the wrist landed 4.1 cm (Baiern) or 6.5 cm (Enula) out of the wheel
  plane instead of 7.8 cm.
- First fix (06610f4): the wrist on the elbow-to-palm line and the hand's X
  axis pointed at the rim. The palm point then sat within 1.2 mm of the rim,
  but in game the fists only rested on the wheel: the stock driver
  (`humans\drivers\vin.SCX`, 20 bones) has a single `Bip01 L/R Hand` bone and
  no finger bones; the fist is modelled closed.
- Grip: in hand space (bind pose, vertices weighted to the hand bone) the
  fist spans x 1.7–12.1 cm, palm toward -Z, and its grip hole is at about
  (9.5, 0, -1.2) cm, open along the hand's Y axis (both hands; the left one
  is mirrored in Y). `pose_driver` now puts that hole on the rim's centreline
  with Y along the rim, the palm (-Z) toward the wheel's centre and X as
  close to the forearm as that allows, solves the arm to the resulting wrist
  (two passes) and turns the hand with two `character_align_axis` calls (X,
  then Y, which is a twist about X).
- In game (cockpit view, Baiern, with #318 and #320 applied): the rim now
  runs into both fists. Not checked: steering locks in game, other drivers
  (the grip-hole numbers are for `vin.SCX`), P3 (gear-shift gesture).

## Gear-shift gesture (#302, P3)

- Build 606 (read-only disassembly of `StreetLegal_Redline.exe`): the chassis
  cfg line `shifter x y z [yaw pitch roll]` (handler 0x00444990) stores the
  lever knob at car+0x9AC. No stock racer has the line (only
  `cars\traffic\Wreck_data`), so the knob stays at the chassis origin.
- On a gear change (0x0044D0C0, gear at +0x1DF8, 0 = N, 1-6, 7 = R) the exe
  plays `sfx_gearchange`, sets two timers to now + 0.3 s and now + 0.7 s,
  and re-targets the console-side hand (right when the seat x < 0, otherwise
  left) to the knob plus an H-pattern offset per gear (table 0x0063C6B0,
  metres): N 0, 1 (-0.05, 0, -0.05), 2 (-0.05, 0, 0.05), 3 (0, 0, -0.05),
  4 (0, 0, 0.05), 5 (0.05, 0, -0.05), 6 (0.05, 0, 0.05), R (-0.09, 0, -0.05).
  When both timers have run out, 0x0044D260 puts the hand back on the wheel.
  The driver setup (0x00449200) reads the bone names `lfbone`, `rfbone`,
  `lhbone`, `rhbone`, `sthbone` and `gsbone` from the human, with Biped
  defaults; no stock human overrides them.
- OpenSLRR: `PartConfig::shifter` (parsed in
  `src/resources/part_config.cpp:parse_part_config`), and
  `src/resources/part_runtime.cpp:pose_driver` keeps the last gear per
  `DriverSeat`. A change starts a 0.7 s gesture counted in physics ticks (a
  pause holds it; releasing the seat cancels it). The console-side fist moves
  to the knob over 0.15 s, closes across the car with the palm down, and
  eases back over the last 0.15 s. The exe switches the hand at once.
- Tests: `cockpit_needles` case `parse_shifter_line`. In game (bench drive,
  Baiern, cockpit view): on the 1 to 2 shift the right hand leaves the rim for
  about 0.7 s and returns. With the knob at the chassis origin it goes below
  the view, as the stock data implies.

## PhysicsRef position readback (#86)

World PhysicsRef objects with a physics shape now return a fresh Vector3 from
canonical GameRefState.pos, the position updated by the free-body simulation.
Previously the stored setMatrix argument always won, hiding subsequent motion.
A body made static after moving must also report its current state.

Shape-less OSD hit rectangles retain their script-space position copy. They use
setMatrix for hotspot coordinates but physicsref_set_matrix deliberately does
not create a physical pose for them. Both paths return independent vectors so
Osd.moveFocus cannot mutate the stored rectangle position through its result.
No Java changes or section-specific exceptions are introduced. This fixes an
existing native readback contract; it does not change integration or collisions.

### Validation

Release x64 compilation succeeds. Tests invoke registered native methods,
create real box state and update its position as the simulation does. They
cover moved bodies, frozen bodies, independent results, and shape-less OSD
coordinates. The state update is deterministic; this is not a solver test.
Main c26ef62: 2 pass / 2 fail. Corrected: 4 pass / 0 fail.
Knowledge references: 219 checked, zero errors.

```powershell
./scripts/run_save_io_tests.ps1 -Suite physics_position
./scripts/run_save_io_tests.ps1 -Suite physics_position -BaselineRef c26ef62
python scripts/check_knowledge_refs.py
```

[Before](../../assets/validation/physics-position/before.txt) /
[after](../../assets/validation/physics-position/after.txt).
In-game validation remains pending: inspect moving PhysicsRef readback and
navigate menu focus with the arrow keys. There is no in-game screenshot yet.

## Tyre squeal, skid marks and collision sounds

Cause: the host never played the stock tyre or impact clips and drew no tyre
marks; only the SfxTable engine loops, nitrous, horn and radio sounded.

Stock data: `sound.rpk` has `surface/slide` (0x06, SLIP3.WAV), `surface/roll`
(0x08) and `collision` (0x0b, Collision.wav); `collision/chassis` (0x13) and
`collision/window` (0x14) have empty payloads, so the parent 0x0b holds the
clip. The exe reads per-material `skid_PS`/`skid_SM`/`skid_SMcolor`/`skid_SFX`
from map SCM files and `particles/scripts/skids/sliding/*.cfg` (maxcount 100,
width, uv, fade) for marks. None of that is read yet. SLRR_rev 4ca11c2
implements squeal and impacts (`SLRR_rev:src/audio/vehicle-audio.ts`) and marks
(`SLRR_rev:src/render/tire-effects.ts`); both were migrated.

Fix:

- `update_slide_audio` (`part_runtime.cpp`): one SLIP3 loop per car from its
  most slipping wheel in contact (not a hub on the road), gain
  `clamp((slip - 2) / 10) * 0.3 * skidLoudness(speed)`, pitch 0.92-1.15 with
  the slip (a host addition; SLRR_rev keeps 1). One loop per car, not per
  wheel, keeps the mixer's 24-loop budget for engines. A sideways snap above
  14 m/s with 3.4 m/s lateral slip plays `surface/skid_ok.wav` once.
- `play_impact_audio`: `commit_batch` keeps each car's hardest impact of the
  batch (at 1000 Hz one crash reports many ticks) and plays Collision.wav,
  plus `MetalHit2.WAV` above 7 m/s and `Metal_plane_hit_04.WAV` above 14 m/s,
  at most every 160 ms per car.
- `skid_marks.cpp`: SLRR_rev's mark strips without the smoke. Per wheel, a
  scrub over the threshold (load >= 300 N and slip ratio > 0.25 with slip
  speed > 2 m/s, or lateral speed > 5.5 m/s) starts a track; 0.28 m quads
  follow the contact patch, fade in and out over 0.45 m, and a still burnout
  adds a short quad every 0.14 s. A ring of 1400 quads caps the count.
  `update_skid_marks` samples the published wheel state once per host frame
  in `finish_physics_frame`, so the 1000 Hz free-running clock adds no work.
  Map unload (`part_drop_actors`) clears the ring.
- `draw_skid_marks` (`bgfx_lighting.cpp`): one transient draw in the world
  view after the world, multiplicative blend (the mark only darkens),
  emission path over a white mask so sun, shadow and fog do not touch it,
  SLRR_rev's road colour 0x17191a at 0.62.
- Both audio paths use the inverse distance gain of SLRR_rev's panner (ref 2
  or 3 m, rolloff 0.25, silent past 55 or 70 m) on the world camera distance;
  the host mixer has no 3D one-shots. Marks skip cars past 120 m.
- Native traffic (`native_vehicle`) gets neither squeal nor marks, so ~100
  traffic cars cost nothing here.

Validation:

- Compilation: `slrr_host` Release builds.
- In game (`build/Release/perf-roots/saved`, Baiern CoupeSport on ValoCity,
  13:00), with temporary env-gated hooks since removed:
  - Stock bench launch: rear slip stays under 1 m/s (soft auto clutch) and ABS
    braking holds the slip near 15 % (1.4 m/s), so no squeal or mark there,
    as with SLRR_rev's thresholds.
  - ABS off, full brake and full lock from 37 km/h: the wheels lock, 143 quads
    are laid, and a screenshot after driving on shows two dark rear-wheel
    tracks behind the car, depth-tested.
  - An earlier needle-game launch with 13-15 m/s slip started the SLIP3 loop
    (gain 0.18), and an 8.9 m/s hit played Collision.wav.
  - A free-running (`--bench-realtime`) drive exited cleanly.
- Not listened to: the sounds were confirmed by trace only.

Limitations: no ground materials (grass/sand marks and clips, `surface/roll`
rolling noise, SCM `skid_SM` colours, the stock skid textures), marks are flat
quads, so a strip crossing a kerb step floats across it; no tyre smoke; no
object `material` impact sounds (`material 0 0 0x567 500` on map objects); no
stereo pan on one-shots; traffic leaves no marks.

## Tyre model (#383)

Cause: the tyre law put the lateral peak near 16 degrees of slip angle and
kept 99 % of it at 20 degrees; the longitudinal and lateral curves were
separate, joined by an ellipse; peak friction was about 1.42 sideways and 1.50
lengthwise for the stock tyre; and `sliction` was divided by the stock value,
so it did nothing for the stock tyre. The cars could not break loose, slide
or burn out. The original law (nicolas-found42/sl-remastered, recovered from
the Steam EXE at 00454500) uses one Magic Formula curve on the combined slip,
with C from `setSliction`, B from `setStiffness` and E = -1; its stock C
(0.81) has no peak at all.

Fix (`src/physics/tire_model.cpp`, [driving model](driving-model.md)): one
combined-slip curve with the original's structure and load smoothing; C set
from sliction read as the slide/grip ratio its script documents; peak
friction 1.15 for the stock tyre; a 0.3 m lateral relaxation length in
`VehicleDynamics`.

Validation (Physics Lab, 1000 Hz, `tyre_curve` and the driving scenarios):

| | Before | After |
|---|---|---|
| Lateral peak (3500 N) | 1.314 at 15.8 deg | 1.072 at 8.1 deg |
| Lateral at 20 deg | 1.309 | 0.988 |
| Longitudinal peak / locked | 1.493 at 14.4 % / 1.161 | 1.072 at 14 % / 0.913 |
| Lateral at 6 deg, with 20 % wheelspin | 0.819 | 0.482 |
| Skidpad | 1.111 g | 0.985 g |
| 100-0 km/h, locked wheels (no ABS) | 30.8 m | 43.0 m |
| 0-100 km/h, ASR | 7.64 s | 7.69 s |
| Burnout, rear wheel speed | 2.4 m/s | 11.8 m/s |
| Power oversteer at 40 km/h, rear slip angle | 4.8 deg | 24.7 deg |

The suites pass; three `vehicle_dynamics` tests pinned the old curve and were
restated for the new one, the coasting grip test now opens the clutch (in
first gear at 72 km/h the engine braking locked the rear wheels and spun the
car), and the race AI gets 7 s to stop from 166 km/h. Its line error on the
ROC fell from 12.6 m to 6.6 m (8.4 m on the second lap; the test allows 9 m). In game, the bench drive in Valo City runs as
before; the handling itself is not judged yet.

Keyboard steering: with the game's steering help at 0 (a career save's
`player_steeringhelp`), a key turned the wheels to full lock at 2 rad/s at any
speed, which with a peak near 8 degrees was a slide: "very lively on the
keyboard". `steering_filter` now shapes digital steering whatever the help:
the wheel angle is capped at what a 1.2 g turn needs plus 8 degrees of slip,
about the tyre's peak (17 degrees at 50 km/h, 12.5 at 72, 9.1 at 144, full
lock below about 27 km/h; first 1 g and 5 degrees, which felt too tight on
every device), and it turns in at 2 / (1 + (v / 15 m/s)^2) rad/s (0.72 at
72 km/h), returning at 2 rad/s. The help still adds its curvature bound and
counter-steer. An analog wheel is unchanged.

The window follows the slide: towards the inside of the turn the cap above
holds, but countersteer may reach the direction the front axle travels in,
plus the same margin. Capped around the body alone it stopped near 9 degrees
at 72 km/h, so a rear sliding at 20 degrees could not be caught; that showed
on the AWD Focer (its stock tyre's stiffness 22 against the Baiern's 17.7
puts its peak near 6 degrees). The Focer's script and CFG values are
consistent: 1475 kg, 47/53 front/rear, 50 % drive per wheel, diff lock 0.2,
springs 46/42 kN/m, yaw inertia 2350 kg m2.

Steering input on every device: the stock ControlSet smoothing of
AXIS_TURN_LEFTRIGHT (`ControlSet.java`: power `Config.steeringPower` 1.5,
rate `Config.steerSpeed` 0.01) gave a half-turned wheel or stick 35 % of the
lock and lagged about 400 ms. `host_input_update_controller` now reads the
steering axis directly (a script-forced steer still wins): an analog device
steers linearly and follows at up to 6 rad/s, a key is shaped by
`steering_filter` alone. The cockpit steering wheel turned like the road
wheels, a few degrees at speed; `pose_steering_wheels` now applies a 14:1
ratio (about 1.4 turns to the 0.65 rad lock every stock car has).

Steering options (Options → Controls → Response): the direct steering axis
still applies the STEERING GAMMA slider (the ControlSet power) and its STEERING
DEAD ZONE. The pad's left stick lost its fixed XInput dead zone (7849 of
32767, 24 %, which felt like a dead zone on the steering) for a 5 % noise
floor (`kLeftStickFloor`), so the slider's dead zone is the one that counts.
A new STEERING SPEED SENS. slider (host option 21, `InputTuning`, saved in
`openslrr_options.json`) blends an analog device's full lock towards the keyboard
window as speed rises: 0 keeps full lock, 50 (default) halfway, 100 like the
keyboard. `SteeringGeometry::speed_sensitivity` carries it to the physics
thread.

Stock assembly slot order: `Chassis.addStockParts` installs the rims and
tyres of a list in order (front left, front right, rear left, rear right),
each with `addPart` on the chassis, and the native install takes the first
free compatible slot (`Part.addPart` leaves `installCheck` commented out). The
host's `select` walked a part's children from an `unordered_map`, so that first
slot came in hash order: a newly assembled Baiern CoupeSport GT III got its
295 rear rims and tyres on the left side and its 255 front ones on the right.
`select` now walks children in the order the parent's CFG declares their slots
(501-504: front left, front right, rear left, rear right), then any other by
id. Cars already saved keep the tyres they were assembled with. With the
staggered tyres in place the Lab's GT III (`physics_lab --gt3`) holds 1.25 g on
the skidpad, stops from 100 km/h in 34 m on locked wheels and keeps 9 degrees
of rear slip in the power oversteer test; with the left/right swap (`--mixed`)
its rear slid to 28 degrees.

Limitations: at 120 Hz (`--physics-hz 120`) full-throttle wheelspin in a
turn runs away and spins the car (stable from 160 Hz; the adaptive rate never
goes below 250 Hz). The SCM surface friction is not read yet. No
longitudinal relaxation, camber or aligning torque.

## Force feedback (#395)

Cause: no force reached a force feedback wheel. SDL was built with
`SDL_HAPTIC` off (`CMakeLists.txt`), and nothing in the host sent an effect;
the stock FORCE FEEDBACK slider (`Config.FFB_strength`) was stored only.

Fix: `physics::SteeringFeedback` (`src/physics/steering_feedback.cpp`) turns the
front tyres' state into a steering torque. A lateral force acts behind the
patch centre: with the solver's axes (side = forward x up, steer positive to
the left) its moment on the steering axis is trail x `tire_force.lateral`. The
trail is a pneumatic part (3.5 cm at zero slip, gone at 1.5 times the slip
angle of the lateral peak, which is found on the tyre's own curve) and a
caster part (1.5 cm), so the torque peaks before the grip and keeps 30 % in a
slide. It is divided by twice the peak of that moment at the resting front
load (smoothed over 3 s), faded out below 3 m/s, and low-passed at 20 ms. A
damper runs from 0.6 parked to 0.1 at speed. `host_input_force_feedback`
sends it to the first steering binding on a haptic device, with the binding's
sign, the strength slider and INVERT FFB (host option 25); SDL plays a
constant force and a damper on `SDL_HAPTIC_STEERING_AXIS`. Options > Devices
shows FORCE FEEDBACK and its TEST button turns the wheel left then right.

Validation: Release build; `vehicle_dynamics` 38/38 with three new tests
(sign and symmetry, lighter past the peak, full scale at most 1 and
independent of load, fade when parked or airborne, a coasting car steered
left is pulled right and vice versa); `sdl_input` 2/2. Java compiled (the
compile root first rebuilt HEAD's classes byte for byte). A bench drive
starts with haptics initialised. Not checked on a force feedback wheel: the
SDL direction convention for a positive level is assumed (INVERT FFB covers a
wheel that disagrees), and the strength and feel are not tuned on hardware.

Not done: emulated force feedback for devices without it
(`Config.FFB_strength_emulated`) and a per-car caster from the suspension
geometry.

## Brake strength and balance (#400)

Play test with a pedal set: "the brakes brake far too hard". A probe with the
stock Baiern CoupeSport's torques (script discs 3718 Nm front, 2928 Nm rear
before Brake.java's split, `brake_inertia` 32) on the solver, 100-0 km/h,
linear pedal:

| Pedal | Before | After |
|---|---|---|
| 20 % | 0.47 g | 0.24 g |
| 40 % | 0.86 g, rears lock | 0.47 g |
| 50 % | all four locked | 0.58 g |
| 70 % | locked | 0.81 g |
| 80 % | locked | 0.84 g, fronts lock |
| 100 % | 0.89 g, 43.5 m | 0.89 g, 43.0 m; with ABS 0.98 g, 40.4 m |

Cause: the host gain `50 / brake_inertia` gave about 2.5 g of brake torque.
Halving it alone failed `race_vehicle` (ROC line error 9.3 and 11.1 m
against 9): the stock split (`brake_balance` 0.5, rear discs x b and front x
(1 - b)) left about 56 % of the braking on the front, so once the brakes no
longer locked all four wheels the rears locked first and the AI's Enula spun
braking into a bend at 125 km/h (yaw -1.7 rad/s). With everything locked the
car had slid straight, which hid the split. An AI-side ABS, a higher AI brake
gain, a pedal cap and releasing the brake while steering each moved the
error from one car to the other.

Fix: `kBrakeGain` 25 (the handbrake keeps 50), and `distribute_brakes`
(`include/slrr/brake_balance.hpp`) reads the chassis `brake_balance` as a
front share: 0.5 gives 70 %, an adjuster (`brake_balance_can_be_set`: Focer
RC 300 and WRC, Enula WR SuperTurizmo, Baiern GT III...) moves it from 100 %
(b = 0) to 40 % (b = 1). The total is what the stock split gives at b = 0.5
and each wheel keeps its share of its axle; saved cars keep their balance
value. The race AI is unchanged.

Validation: `vehicle_dynamics` 42/42 (two new tests: 70 % at the stock
balance with the total kept, the adjuster moving the share monotonically
within 40-100 %, a car without rear brakes left alone); `race_vehicle`
passes with line errors 5.9 and 6.2 m (6.6 and 8.4 before) and stops from
170 and 133 km/h; `race_brain`, `traffic_runtime`, `physics_recovery` pass;
the Valo City bench ends with `drive done`, exit 0. `swaybars` fails 4 of 8
from `build/sr`, identically on `main`. Not tried by a player yet; the
default brake gamma (`Config.brakePower` 2.5) still softens the start of the
pedal on top of this.

## Force feedback detail (#399)

Play test of #395: the pull was right but bumps, kerbs, impacts and scrub
were not felt. `SteeringFeedback::update` now also returns:

- a bump kick, 0.6 per m/s of the two front suspension rates
  (`WheelState::speed`) moving apart, limited to 0.5 and faded below 5 m/s: a
  bump under the left wheel shortens it and drags it back, so the wheel turns
  left. A dip across both wheels gives no kick;
- an impact jolt: `commit_batch` keeps the controlled car's hardest hit
  (10 m/s and harder is full scale, positive on a hit on the left of the car)
  and the wheel rings at 12 Hz, dying out over about 0.1 s;
- a vibration: past the grip peak, 0.4 per unit of slip angle over the peak
  (at most 0.3) at 20-30 Hz; on a rough road, half the smoothed sum of both
  front suspension rates (at most 0.3) at 35-60 Hz. It plays as an
  `SDL_HAPTIC_SINE` on wheels that have one.

Kicks and jolts add to the constant force; FFB DETAIL (host option 26, 0-200
%, default 100) scales them and the vibration, then FORCE FEEDBACK scales all.

Validation: `vehicle_dynamics` 40/40 (new: kicks by side, none for a dip
across both wheels, a jolt that dies within 0.5 s, scrub vibration past the
peak only); `sdl_input`, `race_vehicle`, `race_brain` pass; the Valo City
bench drive ends with `drive done` and exit 0, with the same speed trace as
`main`. Not checked on a force feedback wheel. Roll on turn-in moves the two
suspensions apart too, so it gives a small kick; kerbs are felt only through
the suspension (the SCM surface type is not read).

## Force feedback felt like a pad's rumble (#405)

Report: on a wheel the vibrations were violent and felt like a pad's rumble
motor rather than a steering wheel.

Cause, from the code (#399): three parts played as buzz on a belt or gear
wheel. The rough-road vibration was an `SDL_HAPTIC_SINE` at 35-60 Hz that ran
whenever the front suspensions moved, so on almost every road at speed. The
bump kick read the raw suspension rates, which change from one solver step
to the next, so the constant force stepped every frame. The impact jolt rang
at 12 Hz up to full scale. FFB DETAIL scaled all three together, up to 2x.

Fix (`src/physics/steering_feedback.cpp`, `host_input.cpp`):

- no periodic effect for the road; road texture comes only through the kicks;
- kicks: rates low-passed over 25 ms, a 0.05 m/s dead band, 0.5 per m/s,
  limited to 0.35;
- impacts: one knock limited to 0.6 that decays over 0.05 s, no ring;
- scrub vibration: 0.25 per unit of slip over the peak, at most 0.15, 18-26 Hz;
- separate gains (host options 26-29: road feel, impacts, vibration at 50 %
  by default, damper), a minimum force for the wheel's dead band (30) and a
  low-pass on the summed force (31, 10 ms by default);
- Options > Controls > FORCE FEEDBACK groups them with a WHEEL TYPE preset
  (gear, belt or direct drive) and a TEST button.

Validation: `vehicle_dynamics` 44/44 (kicks by side, none for a dip across
both wheels, no kick from step-to-step chatter, one knock that never changes
sign, scrub vibration at most 0.15); `sdl_input` 2/2. Not checked on a force
feedback wheel; the preset values are a starting point, not measured.

## Wheels locked when a spin turns the car round

Play test: in a spin, once the car travelled backwards it could not be
caught, because every wheel locked. `step_drive` had a direction brake from
the first drive model: with a forward gear and the car going backwards faster
than 1 m/s (or reverse held while going forwards), it cut the throttle,
decoupled the clutch and set the service brake to 1 on all four wheels, pedal
or not. Nothing in SLRR_rev or the docs backs it.

It is gone. A gear now only turns the driven wheels its own way through the
clutch: going backwards in first, the undriven wheels roll freely and the
throttle spins the driven ones forwards; holding reverse while rolling
forwards spins them backwards once the auto clutch takes up (launch rpm), and
the car then backs away. Only the brake pedal and handbrake brake.

The vehicle tab of the debug overlay shows each wheel's script drive weight
(`setDrive`). A stock Einvagen spawned on the Valo City bench reads
FL/FR 1.00 and RL/RR 0.00, so front-wheel drive is applied as authored.

Validation: `vehicle_dynamics` 44/44 (new: sliding backwards in first keeps
the wheels rolling, with and without throttle; reverse while rolling forwards
drives the wheels backwards and reverses the car). Before the fix the
reverse case locked all four wheels within 0.15 s at 7 m/s with no brake
pedal. Confirmed in game: a spin can be caught again. A manual (non-auto) clutch still
couples a stalling engine to the driven wheels, as on a real car.

## SuperDuty cams: negative torque below 2,500 rpm (#409)

Cause: the owner saw negative power curves on the Hauler and the MC. A VM
probe assembled the 24 stock racers (`vehicle_spawn_stock`) on private copies
of the stock MWM game and read the host dyno: with the stock Java in SI units
every curve is positive from 1,000 rpm; with the #203 patch the Hauler's
SuperDuty 500 and Extra 750, the Prime DLH 500 and the MC GTLE are negative up
to 2,000–2,500 rpm. Their eight SuperDuty cams declare a closed lift of 1.27 mm
(0.050 in, the duration-measurement lift). The stock X²/V law made that leak
negligible; the #203 linear curtain law makes it 12 % of the open flow.

Fix: closed lift 0 in those eight cams (data re-spec, as every other cam). No
coefficient and no change to the flow law.

Validation: the eight sources rebuild the stock classes (only one-ULP float
constants differ). The probe re-run changes exactly those four cars; their
curves are positive from 1,000 rpm with the stock peak region (figures in
[patch](../../patch/README.md#superduty-camshafts-seated-closed-valves-409)).
Not checked in game. Their output now exceeds the real engines (Hauler 500
342 PS / 561 Nm peak against a Ram 5.9's 248 PS / 454 Nm): left to the engine
re-spec ([planning](../planning/stock-respec.md)).

## Fuel blends and knock (#407)

Cause: reviewer feedback on the #203 fuel model. The three gasolines used one
stoichiometric ratio (14.7) although pump 95 is E5 and 98 is E10; their stock
`knock` values (1099500/1100000/1100250) left octane without effect; the
mixture clamp and the tuning slider stopped at 8:1, above methanol's
stoichiometric 6.47:1, and methanol's "optimal" was 16:1. The FT_GAS_95 branch
had no braces. The compression window (6.8–17:1 at the best mixture) was too
wide to model knock, and a strict octane window would refuse stock engines:
from their stock part lists, the Einvagen PerPro GTA reaches 11.75:1 and the
Duhen D18V 11.2:1 on 95.

Fix: blend-dependent stoichiometric ratio and heating value, real best-power
mixtures, a 4–20:1 mixture range, fixed per-fuel compression windows and a
knock limit sent to the host (`DynoData.knockCompression`), where the dyno
retards the spark above it, boost and intercooling included. Details:
[driving-model.md](driving-model.md) and
[patch](../../patch/README.md#fuel-heat-blends-and-knock-203-407).

Choices to revisit with the engine re-spec: knock limits 10.5/11.5/12.5/15,
2.5% heat per compression point, the knock state at intake temperature (no
boost heating, since DynoData has no intercooler), and methanol rail flow
0.0655. Fuel Revamp 1.2 (Gorgoil/slrr-javas) uses hard windows only
(95: 6.5–14.5, 98: –15, 100: –15.5, methanol 9.5–18); its maxima would refuse
stock builds up to the old 17:1, so this patch keeps them as a soft knock.

Validation: `port_flow` suite 5/5 on a private copy of the stock game with
`patch/parts` (fuel values at four mixtures, the 4:1 clamp, and the host dyno:
knock limit 8.5 costs 5.3% peak torque on the 10.5:1 synthetic engine, limit 4
costs 17%, 0.8 bar of boost makes a 10.6 limit knock, intercooling 0.9 reduces
it). Not checked in game.

## Stock engine re-spec (#411)

Goal (owner): realistic specs considering the real models and the base game's
performance, not pixel perfect, with no hardcoded power coefficient.

Findings: with the stock flow law (X²/V) the 24 stock racers reach 79–93 % of
the base game's displayed power (that display counted the cycle power twice);
the #203 curtain law moves each engine by −45 % to 0 % depending on its
valves, so the data had to be re-specced per engine. Reverting #203 was
declined.

Method: a throwaway VM probe assembles all 24 cars (`vehicle_spawn_stock`) on
a private copy of the stock game plus `patch/`, reads the host dyno in 4 s,
and a bisection adjusts one lever per car (a part only that car or its group
uses), in dependency order. Bore, stroke and fitments stay, so catalog parts
still fit. The coupling of the five SuperDuty-headed V8s needed one stock-list
change: the Haulers got the MC cams (the GTLE kept the SuperDuty cams at
8.8 / 6.2 mm lift). #417 reverted both when it re-specced the Hauler engine.

Validation: final probe on the branch, results and every changed value in
[patch](../../patch/README.md#stock-engine-re-spec-411). Every family keeps its
trim order. Not checked in game.

Left: WRZ, SuperTurizmo and GTK stay 10–18 % below their real power at
realistic boost (shared heads, no intercooler in the knock model); torque peaks
sit at higher rpm than real on several engines; owned turbo cars keep their
saved wastegate setting; owned Haulers keep their SuperDuty cams (335 PS
instead of 246).

## Superchargers: positive displacement, pulleys and belt (#415)

Cause: the owner found that the Hauler's SuperDuty Extra 750 supercharger
gave no felt power. Superchargers ran on the turbo bell curve
(`turbo_pressure`): +10 Nm at 1,000 rpm, +31 Nm at 1,500 rpm, peak near
3,300 rpm, no drive cost. The driver pulley's `surface_diameter` was never set
and the belt was only checked for presence.

Fix: positive-displacement law in the host (delivery with leakage, adiabatic
heating, drive power, belt slip), drive ratio from the crank and blower
pulleys, belt capacity from its width and condition; data re-spec of the E750
(0.31 bar) and GTK (0.8 bar, efficiency 0.65) blowers to keep #411's targets.
Details in [patch](../../patch/README.md#superchargers-positive-displacement-pulleys-and-belt-415).

Validation: `SuperCharger` and `Pulley` rebuild their stock classes byte for
byte before the edit; `port_flow` suite 6/6 (boost rises with rpm and beats the
turbo law at low rpm, a less efficient blower and a slipping belt cost torque,
no belt limit never holds back); the 24-car probe changes only the GTK and the
Extra 750. Not checked in game.

Left: the charge heating is bypassed (no intercooler part, #417); the leak speed,
efficiencies and belt rating are handbook values, not measurements; the
blower pulley is not a player tuning (no save-format change).

## Hauler's SuperDuty engine re-spec (#417)

Cause: the owner found the Haulers far too weak (246 / 300 PS after #411) and
asked for at least double; the stock description promised "500 Nm and nearly
280 HP", the base game's dyno screen 388 / 529 HP. Their own parts capped them:
7.85:1 compression, 4,500 rpm redline, rods at 5,250 rpm, crank at 5,500,
gearbox at 6,000, and #411 had moved them to the small MC cams.

Fix: Hauler-only parts re-specced (redline 6,500, part limits 7,250 rpm,
10.4:1 pistons), SuperDuty cams back on the Haulers at stock lift, E750 0.35
bar; supercharger charge heating bypassed (`charger_intercooled` defaults to
1). Values in [patch](../../patch/README.md#haulers-superduty-engine-re-spec-417).

Validation: every patched engine source (39) rebuilds its stock class; probe:
SuperDuty 500 479 PS / 645 Nm, Extra 750 593 PS / 830 Nm, MC GTLE 459 PS
(shares the cams), GTK 154 PS (heating bypass); the other 20 cars unchanged;
`port_flow` 6/6. Not checked in game.

Left: the Extra 750 plateaus near 590 PS (knock on its 10.4:1 pistons, shared
with the 500, then belt slip above ~0.6 bar).

## Pacejka tyre with camber (#418)

Report: the Baiern CoupeSport GT III (255/25 and 295/20 R19 soft compound)
felt like soap sideways.

- Cause 1, the low-speed caps. `tire_forces` capped the lateral force at
  0.95 x load per m/s of lateral speed (0.85 longitudinal) at every speed.
  In slip-angle terms that bounds the cornering stiffness to 0.95 x road
  speed per radian: 16 at 60 km/h, below the stock tyre's 17.7 and the GT
  III's 40. Stiff tyres were mushy at small slip angles. The cap gain now
  grows with road speed above 2 m/s.
- Cause 2, the curve. `Tyre.java` gives every low-profile tyre its
  stiffness clamp of 40, so the #383 curve peaked at 4.3 degrees and fell
  off. The slope above the stock 17.68 now enters as a square root (40 acts
  as 26.6; GT III rear peak 1.56 at 6.4 degrees).
- No camber effect existed: camber only scaled the load by cos^2. Static
  camber does reach the hub (`Suspension.setSuspensionGeometry`, e.g. GT III
  front -1 degree), and body roll adds to it.
- New model: MF 5.2-style pure lateral/longitudinal curves, friction falling
  with load (-0.12 per dfz at load smoothing 0.4), saturating cornering
  stiffness, camber thrust (shift + force shift) and camber grip loss,
  combined slip by normalised slip. See
  [driving-model.md](driving-model.md#intended-driving-model-revision-8ac2ba9).
- Physics Lab, 1000 Hz, before -> after:
  - saloon: skidpad 0.985 -> 0.977 g, 100-0 43.0 -> 40.4 m, step-steer yaw
    peak 0.62 -> 0.69 s, burnout wheel speed 11.8 -> 4.4 m/s (the stock tyre's
    peak at 3500 N rises from 1.07 to 1.14 now the arctangent load
    efficiency is gone, so 235 Nm hardly breaks the rear loose);
  - CoupeSport sport suspension: skidpad 0.994 -> 1.018 g;
  - GT III: skidpad 1.245 -> 1.317 g, 100-0 34.3 -> 31.6 m, step-steer yaw
    peak 0.50 -> 0.28 s, power-oversteer rear slip 8.9 -> 6.4 degrees.
- Tests: `vehicle_dynamics` 45/45 (golden value, stiffness ratio, unrated
  load, heavy-car feedback share and reverse-sliding wheelspin margin
  updated; new camber test). `physics_recovery` 9/9. `race_vehicle` still
  fails its first ROC lap (max error 11.0 m against 9), as on `main` (12.2 m);
  the second lap improves from 7.5 m in 131.7 s to 3.5 m in 108.4 s.
- `src/physics/sl2_pacejka.cpp` (recovered SL2 formula, boot self-check
  only) is unchanged and still unused by the solver.
- Not validated in game yet.

## Stock anti-roll bars: per-car rates (#419)

- All 48 stock bar classes had `force = 25000`, `damping = 2500`. With the
  host couple a bar adds 2k per wheel in roll, so on 44 kN/m springs
  (SunStrip 1.5, Einvagen 110 GT, Focer 200) it more than doubled the roll
  stiffness.
- New rates come from the real bar diameters (OEM part listings, bushing
  sizes, forums; many low-confidence, see the table in
  [patch](../../patch/README.md#per-car-bar-rates-419)) through the solid
  torsion-bar formula with one assumed geometry per suspension type.
  No published wheel rate or roll stiffness was found except Nismo's R34
  catalogue. Race cars (GT III, Racing SunStrip, GT II), the GC8 front, the
  Golf III rear (torsion beam, equivalent bar), the R34 stock bars and the
  Prime DLH 500 are assumptions.
- Compile: `build/swaybar-compile-root` with the patched `Swaybar.class`,
  sources in `parts/running_gear/scripts/sways/src`,
  `compile_game_sources.exe <root> parts/running_gear/scripts/sways`, with
  the stock bar classes in place so `--float-mode auto` (#420) picks their
  float conversion. The unmodified `sources/` bars then rebuild all 48 stock
  classes byte for byte, and the patched ones differ only in force and
  damping. Before #420, the MC GT-B/GT-LE bars (`brand_prestige_factor` 1.90,
  2.65) came out one float ulp off and the Focer 200 pair one byte off.
- Tests: `swaybars` patched-car case passes with the new values. Run from a
  view whose `game` is stock (`build/stock-view` still has bars on its stock
  cars, so tests 1-2 fail there, and tests 3, 4, 6, 8 fail on the two-ticks
  expectation that predates the 1000 Hz solver, as on `main`).
- Not validated in game yet.

## Tyre pressure (#424)

Cause: the pressure reached the physics, but only through `setStiffness`.
The installed MWM `Tyre.class` has the same `SetInflation` as `sources/`
(it recompiles byte for byte), so the script was not at fault. In the
original law (nicolas-found42/sl-remastered, `evaluateWheelCurve`) the curve has
no peak (C = sliction = 0.81, D = 1.77 Fz), so a lower stiffness lowers the
force at every slip the car uses: about 30 % less at 8 degrees when the stock
tyre goes from 17.7 to 8. The #383/#418 curve has a peak of 1.15 that does
not depend on the stiffness: a softer tyre reaches the same grip a few
degrees later, so a deflated tyre felt the same. The stock script also caps
the pressure at its optimum and lowers the rolling resistance when the tyre
is under-inflated.

A host law tying grip to stiffness does not work: every stock tyre has at
least the stock 17.7 at its optimal pressure (195/50 21.1, 245/40 29.3,
295/20 40), so a law referenced to the stock stiffness would either miss the
low-profile tyres or change every car's grip at the optimal pressure. The
pressure is judged against each tyre's own optimum, which only the script
knows.

Fix: patched `Tyre.SetInflation`
(`patch/parts/scripts/rgearpart/reciprocatingrgearpart/src/Tyre.java`) with
p = pressure / optimum:

- pressure from 0.5 bar up to 1.5 times the optimum (the tuning slider too);
- friction times 0.5 + 0.5 m below the optimum (m is the stock misinflation,
  0.6 p³ + 0.4), times 1 - 0.3 (1 - 1/p) above it (smaller contact patch);
- stiffness: the stock formula below the optimum, its pressure factor
  1 + 0.2 (1 - 1/p) above it;
- rolling resistance min_rollres / sqrt(p) (was m × min_rollres);
- relaxation length 0.3 / sqrt(p) m through a new native
  `WheelRef.setRelaxation(F)V` (patched `WheelRef.java`), read by
  `VehicleDynamics` per wheel instead of the fixed 0.3 m.

Stock 205/55 (optimum 2.0 bar):

| Pressure | Friction | Stiffness | Rolling res. | Relaxation |
| --- | --- | --- | --- | --- |
| 0.5 bar | ×0.705 | 8.0 | ×2.00 | 0.60 m |
| 1.0 bar | ×0.738 | 8.3 | ×1.41 | 0.42 m |
| 1.5 bar | ×0.827 | 11.5 | ×1.15 | 0.35 m |
| 2.0 bar | ×1 | 17.7 | ×1 | 0.30 m |
| 2.5 bar | ×0.940 | 19.1 | ×0.89 | 0.27 m |
| 3.0 bar | ×0.900 | 20.1 | ×0.82 | 0.25 m |

At the optimal pressure every tyre keeps its previous values, so cars at
their default pressure drive as before.

Validation:

- Compilation: `slrr_host` Release builds; `Tyre.java` and `WheelRef.java`
  from `sources/` first recompiled byte for byte to the MWM classes (float
  mode D for Tyre, header bytes 9-11 aside), then the patched sources
  compiled (18,536 and 1,850 bytes).
- In game: a Baiern bench drive in Valo City on a scratch game copy with the
  two classes called `setRelaxation(0.3)` on the four wheels, without
  errors. Driving at another pressure has not been tried yet.

Limitations: the coefficients are OpenSLRR choices, not measured tyre data;
the pressure does not change the rolling radius, the contact patch length
or the longitudinal relaxation; without the patched classes the host keeps
0.3 m and the stock behaviour.

## A sideways slide swung back like a spring (#426)

Cause: the low-speed lateral cap (0.95 × load per m/s of sliding) used the
relaxed lateral speed, which lags the patch's real sliding by relaxation /
1.5 m/s (0.2 s) once the car has stopped rolling. A car that came to rest
in a slide kept being pushed by its tyres after it stopped, the other way
and back: a lagged damper, about 1 Hz. It was reported on the Baiern GT III.

Fix (`src/physics/tire_model.cpp:tire_forces`): `VehicleDynamics` also
passes the patch's actual lateral speed, which bounds the low-speed force;
below 3 m/s (the camber thrust's fade) a force pushing along the slide is
scaled down to zero at rest. Above 3 m/s nothing changes.

Validation:

- `tests/test_vehicle_dynamics.cpp:a_sideways_slide_stops_without_swinging_back`
  (braked car let go at 4 m/s sideways): the contact patches swung back at
  0.787 m/s before, 0.004 m/s after; 46 tests pass. What remains at the
  centre of mass (0.2 m/s) is the body rolling back on its springs.
- Physics Lab at 1000 Hz: every scenario unchanged except the side kerb
  bump, 1.11 g against 1.12 g.
- In game: a GT III bench drive in Valo City runs without errors. The stop
  itself has not been judged in game.

## A lowered car rode on its exhaust (#426)

Cause: `rebuild_body_support` took the lowest mesh vertex of every installed
part except running gear and hinged panels. On the Baiern GT III the stock
exhaust pipe (under the front floor) and `muffler_12` (behind the rear axle)
hang 3-4 cm below the side skirts and touched the road at rest: they carried
up to 1.5 kN, about 13 % of the car's weight, and slid on it at the body's
0.4 friction. Under power the rear tyres got 2,517 N each instead of about
3,000 N, so the rear broke loose with a little throttle and the car went
sideways at full throttle; it also bounced on the contacts, which felt like
springs without dampers.

Fix (`src/resources/part_runtime.cpp:rebuild_body_support`): parts that are
a `java.game.parts.EnginePart` (exhaust pipes, tips and mufflers, the engine
itself) are no longer body supports. The GT III's supports are now its side
skirts, bumper and anti-roll bar; at rest and under power the four tyres
carry the whole weight (rear 3,018 N each while accelerating).

Validation: `slrr_host` Release; GT III bench drive in Valo City with a
temporary log of the support points and tyre loads. The handling has not
been judged in game yet.

Limitations: an exhaust or sump hanging lower than the body now passes
through kerbs and the ground instead of scraping.

## Weight distribution and centre of gravity (#430)

Cause: the assembly's mass properties sum each installed part's CFG `body`
lines (`src/resources/part_runtime.cpp:subtree_mass_properties`). The stock
data, not the host model, puts the centre of gravity too far back and too
high: summing the same bodies as point masses (the stock game's method) gives
the same result within 1-2 cm and 1-2.5 points of front share. On every
racer the chassis CFG holds 450-1,300 kg in one `_phys_chassis_frame` body
whose centre lies behind mid-wheelbase, so the chassis alone has 31-43 % of
its weight on the front axle. 101 part CFGs had no `body` at all (springs,
dampers, struts, trailing arms, brakes, anti-roll bars, batteries, NOS
bottles, exhausts, mufflers, radiators, intercoolers, aftermarket seats,
steering wheels, wings, two camshafts) and the 29 tyres weighed 0.06-0.13 kg.
Measured parked (each hub at its loaded spring length): front shares
43.8-54.7 % on front-engined cars (Duhen SunStrip 51.4 %, real del Sol about
61 %) and heights of 0.53-0.59 m on the compacts; the MC (0.47 m) was the
only low car, hence its better stability. Static rollover threshold
track / 2h: Duhen 1.17 g, Focer 1.18 g, MC 1.55 g.

Fix (patch data only, no engine change): every massless part gets one
`body ... sphere 0.1` + `nocollision` at its origin, the tyres get a mass from
their size, and the 24 chassis CFGs are rebalanced at constant total car mass
(the chassis gives up what its parts now carry, then frame mass moves to a
`nocollision` box ahead, and cockpit/roof/trunk mass to a `nocollision` floor
box). Values and per-car results: [patch README](../../patch/README.md#weight-distribution-and-centre-of-gravity-430).
The ballast boxes go just before `# total mass`, after the chassis bodies'
`noclick` lines, which apply to the body line above them.

Validation: `slrr_host` Release with a temporary report in `dynamics_configure`
(host and point-mass centre, parked ground from `part_parked_spring_length`
and `wheel_pose`), all 24 racers spawned with `--spawn-car <pack>.rpk:<id>`
on a copy of the game with the new CFGs; total masses unchanged, front
shares on target, heights within 0.01 m except the Nonus Street GT
(0.517 m: the solver keeps 40 % of the high bodies). Bench drives of the
Duhen, Enula and SuperDuty: no NaN or suspended body. Not yet driven in game.

Limitations: the real-car targets are typical published figures, not
measurements, and the Prime has no real counterpart (52 %, 0.47 m chosen).
Part masses sit at the part origin, so a part whose origin is far from its
own centre (an exhaust pipe) is slightly misplaced. Unsprung mass is still
not simulated apart from the body: tyres and hubs only add to the rigid body.
The stock game exe still treats these parts as massless.

## Engine inertia was 2-3x a real engine's (#432)

Cause: `configure_drive` set the crank inertia to `0.28 + 0.01 ×
engine_inertia`. Stock `Chassis.engine_inertia` (the parts' sum, 0.09 per kg)
is 5.6–7.6, so every engine got 0.34–0.36 kg·m², mostly from the fixed 0.28.
That is a truck engine: road-car crank + flywheel + clutch is roughly
0.10–0.22 kg·m². In first gear (17:1 overall on the Baiern CoupeSport) it
added about 1,080 kg of equivalent mass to a 1,490 kg car, the engine rose
from idle to redline in about 1.3 s in neutral, and a flywheel swap changed
almost nothing. SLRR_rev uses the same formula.

The original game is lighter still. From SLRR_rev's original-game telemetry
(`data/telemetry/original-game/2026-11-05`, neutral full-throttle rise against
each capture's torque curve, 10 Hz samples): Baiern 7.63 → ~0.07 kg·m², Duhen
SunStrip 5.61 → ~0.052, Einvagen 110 GTK 5.97 → ~0.05–0.057, i.e. about
0.009 × `engine_inertia`, like a race engine with a light flywheel.

Fix: `max(0.10, 0.02 × engine_inertia)`, a road-car range of 0.11–0.15
kg·m² on stock engines (owner's choice: a realistic feel, not the original's).
The installed parts now set the inertia; 0.10 is the smallest inertia the
stability fixture (`idle_and_limiter_stay_bounded_for_every_flywheel`) covers.

Limitations: the real-car figures are typical values, not measurements of
these engines. SLRR torque curves are below the real cars' (Baiern peak 178
N·m against about 245 N·m for its counterpart), so the cars stay slower than
their counterparts. Wheel inertia (1.6 kg·m² per wheel) is unchanged.

## A wheel had a dead zone near the centre at speed (#434)

Cause: `host_input_update_controller` reclassified steering every tick, and a
binding only counts as active past 1 % of its travel
(`input_mapping::steering_kind`, `steering_is_analog`). Below that the steering
was flagged digital, so `steering_filter` gave a wheel within 4.5 degrees of
centre (1 % of 900 degrees) the keyboard treatment: the speed window, the
steering help's curvature limit and the 2 rad/s return. At speed this scaled
the small angles down, a dead zone with the STEERING DEAD ZONE slider at 0,
then the full analog response came back at once past 1 %: a notch. Parked,
nothing narrows the input, so it only showed while driving.

Fix: when no binding is active, the analog flag and the steering class keep
the class that steered last; a wheel, stick or key now keeps its own filter
through the centre.

Limitations: a wheel touched first after a key is still treated as a key until
it passes 1 % of its travel. Not validated in game with a wheel yet.

## Brakes as hardware, ABS and ASR (#446)

Cause:

- ABS capped the service brake each substep so a wheel never turned slower
  than 85 % of the true road speed at its patch: no modulator, no pressure
  cycles, and a ground speed no controller can measure (SLRR_rev's
  `absBrakeStep` is the same law).
- ASR was a launch control only: a throttle cut in first or reverse below
  30 km/h, armed by a full-throttle start from rest.
- Stock brake torques: `DiscBrake` used a pad "friction" of 1.0-1.1 (real
  pads 0.35-0.5) with `BRAKE_FORCE_FINE_TUNE = 2`, then the host divided it
  by the engine CFG's `brake_inertia` (28-75, unrelated to brakes): the Focer
  RC braked 2.4 times harder than the SuperDuty, the heaviest car. Front
  discs gave 51-59 % of the torque (the MC 41-44 %), which #400 hid by
  redistributing to 70/30. Pads and rotors changed the torque by 5-15 % and
  nothing heated.

Fix:

- Patched `Brake`/`DiscBrake` ([patch](../../patch/README.md#brakes-as-hardware-446)):
  torque = 2 pad faces x pad friction (street 0.40, fast road 0.45, race
  0.50) x 100 bar x piston area (pistons per side x piston diameter) x
  caliper stiffness (1, 1.05, 1.10) x (rotor radius - 25 mm), times the stock
  wear factor. The balance acts like a valve (0.5 leaves both axles their
  torque). The 48 stock brakes are re-sized from the real cars of the
  [#419 table](../../patch/README.md#per-car-bar-rates-419). A new native
  `WheelRef.setBrakeSpec(pad, rotor, diameter)` sends the hardware; a wheel
  with a spec switches the host to physical brakes: no `brake_inertia` gain,
  no `distribute_brakes`, handbrake gain 2.
- `src/physics/brake_system.cpp`: rotor temperature per wheel (heat = the
  friction brakes' work, `0.5 I (w0^2 - w1^2)` of each application; cooling
  `h = 12 + 18 v^0.8` W/m^2K over the faces, plus 1.8 faces of vanes, plus
  radiation; capacity from the ring's equivalent thickness, iron or
  carbon-ceramic), pad friction curves against temperature (street works
  from cold and fades past about 400 deg C, race is weak cold and holds to
  650, carbon rotors move the fade up).
- Without ABS, a rear proportioning valve (full pressure to 30 % pedal, then
  0.3 of the rise), so the fronts lock first.
- ABS (`AbsModulator`): per-wheel follow/dump/hold/rebuild against a
  reference speed (`ReferenceSpeed`: the accelerometer, saturated at 1.5 g,
  integrated between the wheels; braking pulls it to the fastest wheel, a
  0.4 s stall without any wheel spinning up resets it), dump at 16 % slip
  (26 % at the lowest slider setting) or on a hard wheel deceleration, rear
  channels at 0.7 of that (electronic distribution, which replaces the
  valve), select-low rear axle, a front yaw-moment build-up delay of 0.8 s,
  off below 2 m/s.
- ASR (`TractionControl`): engine torque cut on the driven axles' mean slip
  past 8 % or 0.8 m/s (20 % or 2 m/s at the lowest setting), PI with a fast
  cut and a 0.12 s recovery, in any gear and at any speed, plus brake
  pressure on a single spinning driven wheel below 80 km/h. It replaces the
  launch control.
- Debug overlay (Vehicle tab): ABS/ASR state, reference speed, rotor
  temperature and caliper pressure per wheel. Physics Lab: `--abs`, `--asr`,
  `--respec` (the re-specced CoupeSport brakes) and the `fade` and
  `split_mu` scenarios.

Validation:

- Compilation: `slrr_host` and `physics_lab` Release. The stock `Brake`,
  `DiscBrake`, `DrumBrake`, `Wheel` and 48 brake classes recompile from
  `sources/` byte for byte (header bytes 9-11 aside), and the #424
  `WheelRef.java` rebuilds its class; then the patched sources compile in
  `build/brake-compile-root` (compile `parts/scripts` twice: `DiscBrake`
  needs the new `Brake` fields).
- `vehicle_dynamics` 54/54, eight of them new (ABS shorter than locked, cycles,
  select-low and yaw delay, off at walking pace, reference speed, ASR in
  second gear, ASR brake on one wheel, pad curves, a stop heating the
  rotors); `abs_releases_a_locked_wheel_while_the_road_moves` now locks the
  wheels after the car was seen rolling (an ABS cannot know a speed it never
  measured).
- Physics Lab, 1300 kg saloon, 1000 Hz (at 250 Hz the ABS stops are 3-4 %
  shorter, the rest within 2 %):

| | 2000 Nm, no spec | Re-specced CoupeSport |
| --- | --- | --- |
| 100-0, no ABS | 40.4 m, wheels locked | 49.1 m, wheels locked |
| 100-0, ABS 100 | 39.1 m | 39.9 m (rotors 62 / 74 deg C) |
| Ten stops 150-30, ABS | 81.0 m each | 81.9 to 82.5 m, front rotors 443 deg C |
| Split friction 100-0, ABS, driver steering | 84.4 m, 6.4 deg | 84.1 m, 6.2 deg (without ABS: 37 deg) |
| Power oversteer, rear slip angle | 28.8 deg; ASR 100: 7.3 deg | |
| 0-100 km/h | 7.31 s; ASR 100: 7.46 s | |

- In game (stock MWM copy with the whole patch, Baiern CoupeSport bench in
  Valo City, temporary env-gated hooks since removed): the wheels receive
  2409 / 1252 Nm with their specs (street pads, vented front, solid rear);
  car mass 1524 kg. Full brake from 43 km/h: without ABS the rear pressure
  stays at the valve's 0.51; with ABS the channels cycle between 0.03 and 1
  and the reference speed follows the car within 1 km/h, back to the pedal
  below 7 km/h. ASR did not intervene in the stock launch (no wheelspin).
  Not driven by hand, not tried on the other 23 cars.

### HUD lamps, temperatures and the first limitations (#446)

- HUD lamps: build 606 reads `osd_abs` / `osd_asr` (handlers 0x445520 /
  0x4455B0) and lights `led_ABS` (cars 0x61) and `led_ASR` (0x62) while the
  aid intervenes: car flags 0x1000000 / 0x2000000, set where the ABS / ASR
  factor drops (0x44E0E1, 0x44E290) and read at 0x452746 / 0x452779. The
  four lamps (`led_HB` 0x63 and `led_ER` 0x64 too) take the gauge plate's
  anchor (newosd 0x55, 3.24, -2.4, -3.59 at 0x446261; the lamps 3.24, -2.4,
  -3.58), each mesh placing its lamp on the plate. The host table had the
  handbrake lamp at -0.227, 0.925, by eye, so it never showed on the plate;
  all three now share the plate's anchor. `HudSlot::Abs` / `Asr` parse the
  CFG lines like the other slots.
- Tyre temperature (`src/physics/tyre_thermal.cpp`): a tread lump (8 % of
  the tyre's mass, the #430 tread and sidewall densities) heated by half the
  sliding work at the patch, a carcass lump heated by the rolling resistance
  and by the rotor through the rim (1.5 W/K), 25 W/K between them; convection
  growing with speed, and the road under the tread. The patched `Tyre` sends
  its compound (`WheelRef.setTyreSpec`); the grip follows the tread
  temperature on a curve per compound (street full from 40 to 90 deg C, the
  soft racing compound from 80 to 115, all weaker cold and overheated).
- Brake fluid: follows 45 % of the rotor's rise with a 40 s lag; past
  230 deg C (DOT 4) the pressure at the pads falls, to 0.4 at 290 deg C.
- OSD panel (host option 32, Options > Game > TEMPERATURES, on by default):
  a top view above the damage gauge, each corner a tyre bar and two
  readouts in the stock LCD font, the tread above and the rotor below a
  brake mark; blue cold, green in the window, yellow to red past it, a red
  mark while the fluid boils.
- ABS feedback: a 14 Hz force feedback vibration on a wheel, and a light
  rumble on the controller the brake is bound to.
- The race AI gets ABS on cars with physical brakes. A wheel without a
  spec on such a car keeps the stock law's gain.
- The ABS and ASR sliders describe what they do instead of "percent".

Validation:

- `vehicle_dynamics` 56/56 (two more: tyre heat and grip, fluid boiling);
  `options_retry` passes with option 32 in its every-id round trip (33 is
  now the unknown id). `Tyre`, `WheelRef`, `OptionsDialog` and `OsdxOptions`
  compiled in the compile root; HEAD's `OptionsDialog` rebuilt byte for
  byte, HEAD's `OsdxOptions` came out 24 bytes larger than the shipped
  class with the same strings (constant-pool order of a two-pass build).
- Physics Lab (`--respec` adds a sport tyre's heat): ten minutes at
  100 km/h, treads 34-35 deg C, carcasses 45-48; the skidpad ramp leaves
  the outer front at 88 deg C and the outer rear at 71; a burnout heats the
  rear tread by 17 deg C in 4 s.
- In game (Baiern bench, ABS forced on by a temporary hook): with every lamp
  forced on, ABS, ASR and the handbrake light on their plate spots; in a real
  ABS stop only the ABS lamp lights. The panel shows the four corners, clear
  of the damage gauge.

Limitations: the pad, tyre and cooling curves are typical published shapes,
not measured parts; the reference cars and some calipers are assumptions
(no published data for the race cars and the Prime). The sizing assumed
real-car masses; the game's CoupeSport weighs 1524 kg, not 1400. The tyre
temperature does not change the pressure, wear or the tyre's stiffness, and
there is no tyre smoke. Pads do not wear faster when hot. The ABS pulse is
a vibration, not a pedal kick (no device has one). The race AI has no ASR.
The emergency lamp (`led_ER`, `osd_emergency`) is not lit; what lit it in
build 606 is not traced. Braking hard with the automatic gearbox shifted
down to first at about 44 km/h with the auto clutch closed, and the engine's
reflected inertia pushed the rear wheels: fixed by #448 (next section).

## Hard braking in first: the engine pushed the rear wheels (#448)

Cause: with the automatic box and the auto clutch, a hard stop downshifted to
first (about 44 km/h on the Physics Lab car) with the clutch closed. A closed
clutch adds the engine's inertia times the ratio squared to the driven
wheels: 0.38 × 12.9² ≈ 63 kg·m² in first, against 1.6 kg·m² per wheel. The
rear brakes then mostly slowed the engine, which pushed the rear wheels: with
the real ABS of #446 (`physics_lab --scenario brake --respec --abs 1`) the
rear tyre force fell from about 4.7 to 2.7–3.4 kN (both wheels) below 37 km/h.
The `stall_risk` opening in `step_drive` only acts below 12 km/h with the
accelerator released, and `SLRR_rev:src/engine/driving-helping.ts:brakingClutchHelp` only near stall speed.

The same downshift rule made the engine braking odd when the accelerator was
released (Lab `coast`, lift-off at 100 km/h): the box downshifted at 2940 rpm,
as under power, back up to 4000–4650 rpm. The engine braking faded in third
(0.084 g at 90 km/h, 0.059 g at 62 km/h), then grew at every downshift, up to
0.16 g in first at 43 km/h, with a jolt each time.

Fix (`VehicleDynamics::step_drive`, `update_gear`):
- The auto clutch opens with the brake pedal while the accelerator is
  released (`braking_clutch_`: 0 at a brake of 0.5, fully open at 0.8), in
  every gear and at any rpm, since the reflected inertia loads the wheels in
  any gear. It opens in 40 ms and takes up again over 250 ms on release. A
  manual clutch (`auto_clutch` false) is unchanged.
- While it is open, the engine follows the road speed in the gear (between
  idle and 90 % of the limiter), as the shift blip does. The clutch takes up
  without a jolt when the brake is released at speed, and the engine idles at
  rest.
- Accelerator released (throttle ≤ 0.05), the automatic box keeps its gear,
  so the engine braking fades with the speed, and downshifts when the road
  speed's rpm falls below 1.6 × idle and the lower gear stays below 42 % of
  the limiter. It upshifts only to protect the engine, as under power. The
  road speed picks the gear, so a stop with locked wheels or the clutch open
  still ends in first. Under power the rules are unchanged.

Validation:
- `vehicle_dynamics` suite: 49 passed. Three new tests fail before the fix and
  pass after it:
  `hard_braking_declutches_so_the_engine_never_pushes_the_rear_wheels`
  (rear brakes too weak to lock stand in for the ABS; rear force 3.8 → 1.4 kN
  before, 4.3 / 4.2 kN after), `coasting_engine_braking_fades_with_the_speed`
  (0.085 g at 90 km/h and 0.130 g at 40 km/h, 4668 rpm after the downshifts
  before; 0.085 / 0.037 g and 2024 rpm after) and
  `releasing_a_hard_brake_at_speed_takes_up_without_a_jolt` (after 0.3 s,
  0.151 g in first before, 0.061 g in third after).
- Physics Lab at 1000 Hz on the #446 branch (`e35ff7b`) with
  `--respec --abs 1`: the rear force holds about 5 kN down to 10 km/h, and the
  100-0 goes from 40.76 m / 3.113 s to 39.79 m / 2.938 s. Launch unchanged.
- Physics Lab on main (no real ABS, the rear wheels lock): the 100-0 goes from
  40.43 m to 41.15 m, because the closed clutch had kept the locked rear
  wheels turning. The new `coast` scenario gives 0.084 g at 90 km/h (third,
  4274 rpm), 0.057 g at 60, 0.039 g at 40 (third, 1907 rpm) and 0.039 g at
  20 km/h (first). The other scenarios are unchanged.

Limitations: hard braking has no engine braking; it is small next to the
brakes. The rev match while declutched is a controller, not combustion
torque. Coasting at low speed still steps up a little at each downshift
(0.029 → 0.044 g at 25 km/h, 0.036 → 0.057 g at 17 km/h). The engine braking
itself (`pumping`, `engine_friction_torque`) is unchanged and not compared with
measured cars. The `race_vehicle` and `race_brain` suites need the game
folder and were not run; the race AI also coasts with these rules. Not
validated in game.

build 606 is not traced. Unchanged and outside this issue: braking hard with
the automatic gearbox, it shifts down to first at about 44 km/h with the
auto clutch closed, and the engine's reflected inertia then pushes the rear
wheels (the rear force fell from 1.8 to 0.5 kN in the Lab).

## Tyres and suspension geometry (#450)

Cause, suspension: the 96 stock `*_McPherson_strut` / `*_trailing_arm`
classes carry one geometry copied from the Baiern (strut lower arm 0.296 m
pivoted 3 cm above the CFG wheel centre; rear arm 0.42 m about an axis swept
50 degrees, pivot 0.182 m up). The host evaluates it at the spring's static
length, 16-23 cm above the CFG wheel centre (anchor 0.39 m, static length
about the springs' authored `restlength`), where:

- the front lower arm points 30-40 degrees up: +0.5 to +1.4 deg of camber
  per 25 mm of bump;
- the rear arm rotation is the absolute angle from horizontal times 0.77
  (the axis' longitudinal share): -2.9 deg per 25 mm, static camber +2.1 deg
  (MC GT) to -6.0 deg (Racing SunStrip); unloaded (0.265 m, a parked
  CoupeSport) it reached +5.3 deg;
- `setYpr` toe of -1 to -2.5 deg is toe-in (negative toe turns the left
  wheel's forward axis to +x, inboard), 1-2.5 deg per rear wheel;
- in strut mode (`setHub` mode 0) `solve_corner_geometry` replaces the hub
  rotation with the strut fit, so `setYpr` camber and toe never reach a
  strut wheel.

Fix: patched sources in `patch/parts/running_gear/scripts/*_susp/src`,
generated per car at the static length (spring free length minus the #430
corner weight / rate). Fronts: strut fit with the stock ball joint, kingpin
axis and arm length, the strut top 0.58 m up the axis, the hub tilted to the
static camber, the lower-arm slope solved for the camber gain. Independent
rears: arm with a longitudinal axis, pivot level with the wheel centre at
rest, length 0.025 m / gain (1.59 m for -0.9 deg/25 mm, 7.2 m for the Golf's
-0.2), factory camber and toe in `setYpr`. Live axles (MC, SuperDuty): arm
length 0, so `part_runtime` keeps no arm and the hub follows the body. Table
in [patch/README.md](../../patch/README.md#tyres-and-suspension-geometry-450).
`VICx/VICy/LICy/LICz` are unchanged (the solver does not read them).

Cause, tyres: `Tyre.SetInflation` multiplied the compound friction by
0.5 + 0.5 x width / 275 (mu 1.13 for a 195 sport, 2.05 for the 405 drag
radial), set every profile at or below about 45 to the stiffness clamp of
40, rolling resistance 0.0036 (sport) and a nominal load unrelated to the
size (23.7 kN for the 405). `setFrictn_x` and `setLoadSmooth` were sent as
1.0 and 0.4 whatever the compound. Fix in the patched `Tyre.java`: friction
per compound, nominal load from the width, stiffness from the aspect ratio
and carcass, real rolling resistance, relaxation length from the sidewall,
and the drag radial's longitudinal ratio through `setFrictn_x`. Values in
the patch README. `Math.pow(FF)F` is a new host native (`natives_lang.cpp`)
declared by the patched `system/Scripts/lang/Math.java`.

Validation:

- Compilation: `slrr_host` Release. The 96 stock suspension classes and
  `Math.class` first recompiled from `sources/` byte for byte (header bytes
  9-11 aside, float mode D auto), then the patched sources compiled in
  `build/brake-compile-root`.
- Geometry: the generated arguments, fed to the host's `wheel_pose` in a
  standalone probe at each car's static length, give the target camber, toe
  and gain on all 96 corners (left and right mirror).
- In game (bench drive in Valo City, patch copied into `build/needle-game`,
  temporary log): CoupeSport front -0.4 to -0.6 deg, rear -1.8 to -2.5 deg
  with 0.15 deg toe-in; MC GT front -0.1 to +0.2 deg while driving, rear
  0 (no arm); no script error, `Math.pow` resolves.
- Not validated: the handling itself, Physics Lab (its cars do not use the
  scripts), the other 22 cars in game.

Limitations: one hinge per corner cannot model an SLA's progressive gain
(the strut fit flattens past 2 cm of bump and reaches +3.8 deg at full
droop), anti-dive/anti-squat or bump steer; a live axle's camber follows the
body instead of the road; front toe is 0 (strut mode drops `setYpr`); the
static lengths are computed, not measured per car; the tyre coefficients are
typical values, not a measured tyre; stock tyre sizes are unchanged.

## Ground surface friction and rolling resistance (#452)

Cause: every ground surface gripped and rolled like asphalt. The map SCM
named by the ground native was never read, and the host skipped the first
word of each PHYS collision face.

Findings:

- The ground native's `params` line is `render id, six numbers, SCM path,
  TRC path` (`params 0x00000010,0,0,0,0,0,0,maps\city\meshes\city.scm,
  maps\city\ai\city.trc`).
- A PHYS face is 28 bytes: the SCM material index, three vertex indices and
  the face normal. Dead_Island's faces use indices 2-4 of its 5 materials.
- SCM material i is the SCY material i (the modders' own note in
  Dead_Island's SCM), so the SCY texture names the material too.
- Text SCMs (city, city2, the Track Wizard addons) list `material i name`
  and optional keys; compiled ones (MP1, Physic_Lab, Caynon) are a count, a
  record size (0x24) and per material a texture id and a 32-byte name, no
  values.
- In the exe the keyword table at 0x610DC0 sends each key to a handler that
  writes every selected material's 0xAC-byte physics shape record:
  `friction` × `Config.ground_friction` at +0x50 (handler 0x462B00),
  `rollres` at +0x54, `collcoeff` at +0x58 (0x462BF0), the shape's bounce
  (default 0.2), then `skid_PS`, `skid_SM`, `skid_SMcolor`, `skid_SFX`.
  sl-remastered (`ground_config.cpp`, `physics_contact_geometry.cpp`,
  `wheel.cpp`): an unset material has friction `ground_friction` (1 when
  not positive) and rollres 0.001; `params name` selects every material of
  that name; a body contact takes the lower friction of its two shapes (a
  body's is 0.6) and their mean bounce; the tyre multiplies its D by the
  surface friction and subtracts `load × v × (surface + tyre rollres) ×
  0.01` from the longitudinal force.
- Only Dead_Island (Rock 0.8; Sand2 0.6, rollres 3, collcoeff 15, a grey
  mark) and Top_Gear_Track (grass 0.8 / 2, a grey mark) set values. In the
  stock city the lawns use `maps\textures\area.dds` (materials `Area`,
  `autopalyaszele`). No stock map sets `skid_SFX`, and no stock object CFG
  sets `friction`.

Fix:

- `ground_surface.cpp` reads both SCM forms into `GroundSurface`: friction
  factor, rolling coefficient, bounce, mark colour and slide sound.
  `friction` × `Config.ground_friction` (read by the ground native through
  `scene_instances_set_ground_friction`); `rollres` becomes a coefficient ×
  0.05 on the host's physical rolling law (chosen over the original's
  speed-proportional drag, which would brake 0.9 g on Dead_Island's sand at
  108 km/h). Unset keys fall back to a class from the material name, then
  the SCY texture (stock maps set no values): grass, sand, gravel, dirt,
  each with a mark colour. A paved name (road, asfalt, beton, sidwalk,
  kerb...) stays road; `area` and `fu` count only as the whole file name.
  `skid_SFX` ids resolve against the ground pack's dependency slots.
- `append_ground_collision` keeps each face's material index
  (`ScxGeometry::triangle_surfaces`), `attach_ground_surfaces` resolves it.
  `StaticMesh` keeps the table and, since its floor cells store vertex
  triples, a parallel list of surface ids per cell; `floor_test` returns the
  winning triangle's surface in `SceneFloorHit`, the car's floor patch keeps
  it per triangle; `GroundHit` carries it to the solver.
- `solve_linked` averages friction and rolling over the 13 tread samples
  (centre line and tread edges, not the sidewall), so a tyre half on the
  grass loses part of its grip; the rest comes from the centre sample. The
  single-point contact of unlinked wheels takes its one sample.
- `step_drive` multiplies the tyre friction (force law, sticking limits,
  metal and parked contacts) and adds the rolling coefficient to the
  wheel's rolling torque. A body contact uses min(0.6, ground friction) and
  scales its restitution by the mean of 0.2 and `collcoeff` over 0.2,
  capped at 0.5 (Dead_Island's 15 would throw a car).
- Tyre marks take the material's colour (`SkidMarkVertex::color`, alpha
  scaling the 0.62 opacity); the slide loop plays the material's `skid_SFX`
  at its scale over the default 0.6 and restarts when the ground changes.

Validation:

- Compilation: `slrr_host` and `physics_lab` Release; standalone test
  `tests/test_ground_surface.cpp` (6 tests: text and compiled SCM, `params`,
  name classes, values against fallback, effect keys and ground_friction)
  passes.
- Load logs (`build/Release/perf-roots/saved`): Dead_Island 2:Rock 0.80/0,
  3:Sand2 0.60/0.150 bounce 15 mark ff737373, 4:Sand 0.60/0.150 (fallback);
  Top_Gear_Track 2:Material #38 (Top_Gear_Grass.dds) 0.80/0.100 mark
  ff737373; city 3 lawns and city2 2 lawns at 0.65/0.080; garage and car
  lot shells all road.
- Bench drive on Top_Gear_Track's asphalt, 1/2/3 s at full throttle:
  14.7/38.3/62.1 km/h before and after. With the surface forced by a
  temporary hook: friction 0.6 10.6/24.6/39.0; rolling 0.15 9.4/23.8/43.4;
  both 7.5/17.1/28.4.
- Dead_Island bench start (temporary wheel log): the car rests on two
  diagonal wheels (4460 N and 3612 N against about 1600 N); the rear-left
  wheel lifts to full droop and spins at 241 rad/s through the open
  differential while the sand's drag holds the car. Without surfaces it
  already slowed there.
- Not seen: a coloured mark or a `skid_SFX` loop in game (the benches lay
  no mark on grass or sand, and no stock map sets `skid_SFX`).

Limitations: `skid_PS` (dust and smoke) needs a particle system, and
`skid_SM` (the mark's texture and skid config) textured marks, neither of
which the host has; objects stay road, as in the original (their faces have
no SCM; the object CFG `friction` key, used by no stock object, is not
read); the fallback values and mark colours are typical figures, not
measured; the race AI and traffic do not adapt to grass or sand.

## Tyre smoke, ground dust and textured skid marks (#454)

Cause: the host drew no tyre smoke or ground dust (raised since #381), and its
marks were flat, untextured strips. #452 read the map materials' friction but
left their `skid_PS` and `skid_SM` effects unread.

Facts:

- Stock ids. `particles.rpk` maps 2 and 0x101 to `particle_clouds/asphalt.cfg`
  (texture 0x15, asphalt.tga), 0x102 to grass, 0x103 to pebbles, 0x104 to sand
  (Dead_Island's `0x00030104 Sand smoke`) and 0x107 to red pebbles. It maps
  0xA and 0x201 to `skids/sliding/asphalt.cfg` (texture 9) and 0x202 to
  `grass.cfg` (texture 0xD). MW_Mod 0x47 is `MW_Mod\scripts\Skid_Grass.cfg`
  with `Skid_Grass.png`. The defaults 2 and 0xA are sl-remastered's
  `ground_config.cpp`, which also writes the default mark colour as
  0x80000000.
- Cloud format. The cfgs follow McAllister's Particle System API.
  - asphalt.cfg: 20 particles/s, 0.3 m growing to 2-5.25 m, 1.5-5 s, colour
    0.9 fading to 0.25-0.55 grey, gravity +2, damping 0.01, velocity
    `sphere 1 1 1`.
  - sand.cfg: 40/s, 3-6 m puffs living 4-6 s.
  - grass.cfg: `type 1` debris, 300/s, 1-4 cm, gravity -9.81, `velscale 2`,
    `sphere 0 0 0 0.1 2.5`.
  - Every `bounce` line is commented out. grass.cfg writes `move m0 0 0` for
    blades that still fly, so `move` is not read.
- Skid format. `maxcount 100`, `width min max`, `height` (metres per texture
  tile), `uv_end` and `uv_normal` (u0 v0 u1 v1), `fade`. Every texture is
  RGBA with a transparent background.

Fix:

- `ground_surface.cpp` reads `skid_PS` and `skid_SM`. The name fallbacks
  bring a stock cloud and the stock grass mark, and no longer a colour:
  grass gets 0x102, sand and dirt 0x104, gravel 0x103, all with mark 0x202.
  `attach_ground_surfaces` resolves the ids against the ground pack's slots,
  and a fallback's `0xffff....` against particles.rpk.
- `surface_effects.cpp` reads the RPK entry and its cfg once, then registers
  the cloud or the mark style.
- `particle_cloud.cpp` parses the clouds and runs the emitters and the shared
  pool. The emitter is shaped like the stock natives.
  - A three-number sphere is an ellipsoid of radii around the origin, not a
    fixed velocity. With five numbers it is a centre and a shell between the
    two radii.
  - Damping relaxes toward a 1.2 m/s breeze rather than toward rest. Clouds
    get slow eddies. A puff stays 0.3 radius above its emission ground, and
    debris lands and slides to rest.
- `emit_tyre_dust` and `tyre_dust_strength` (`part_runtime.cpp`) set the
  thresholds and rates described in
  [driving-model.md](driving-model.md#tyre-smoke-ground-dust-and-skid-marks-454).
  The emitters run in `update_skid_marks`, once per host frame;
  `particles_update` runs every frame in `finish_physics_frame`. Map unload
  clears both.
- `fs_particle.sc` and `draw_particles` draw the volumetric impostors.
  - Density is the sphere's thickness eroded by two fbm layers that roll
    upward over time. The edges are softened so neighbouring puffs merge.
  - Lighting: an optical depth toward the sun (`exp(-1.5 * across * ...)`),
    HG g = 0.6 rims, sky ambient lighter from above, plus 0.1 × sun of
    multiple scattering. It is capped at a white surface facing the sun.
  - The colour is lifted 25 % toward white, so a grey cfg colour reads as
    tyre smoke rather than oil smoke.
  - The cfg's alpha ramp is eased (t^1.5), with a 0.15 s fade-in.
  - The AO prepass depth gives soft intersections. Without it, the ground
    plane still softens the base, and puffs within 1.5 m of the camera fade.
- `draw_skid_marks` draws one batch per style through the particle program
  in mark mode. Textured tracks split at their tiles (`add_mark`), and the
  finish re-caps the last tile with `uv_end` mirrored (`set_uv`).

Validation:

- Compilation: `slrr_host` Release builds. The standalone tests pass:
  `tests/test_tyre_effects.cpp` (6 tests: stock asphalt and grass cfgs,
  domain sampling, emission and lifetime, debris landing, skid cfg, textured
  tiling and the end cap) and `tests/test_ground_surface.cpp` (7 tests,
  including the new `skid_effect_ids`).
- Load logs (`build/needle-game`):
  - ValoCity registers asphalt smoke (00470002), the asphalt mark (0047000a),
    grass debris and the grass mark.
  - Dead_Island resolves Sand2 to `[ps 00470104 sm 00450047]`, which is
    sand.cfg and MW_Mod's Skid_Grass (`width 0.10-2.00 tile 0.70`). Sand
    falls back to `[ps 00470104 sm 00470202]`.
- In game, with temporary env-gated hooks since removed:
  - ValoCity, real slide: a bench drive with handbrake and full lock from
    5 s slid the car. The pool grew to 1100 particles with 454 mark quads,
    textured marks on the road, and dirt marks and clippings on the lawn.
  - ValoCity, forced rear-wheel smoke for the screenshots: a lit grey cloud
    behind the car, merged and soft at the ground.
  - Dead_Island bench (no hook): the spinning rear wheel raises a rolling
    sand column and lays the Skid_Grass mark.
  - GPU cost (`--perf-log`, 2560×1440): `gpu_world_ms` 5.6 and 6.4 against
    3.6 and 3.9 while about 300 puffs filled the screen near the camera.
    The frame stayed near 140 FPS.
- Not seen: night lighting (the smoke takes no headlight or lamp light), the
  pebbles and red-pebbles clouds, and a map that sets `skid_PS` to another
  pack.

Second pass (the first pass's limits):

- Lamps and headlights. `draw_particles` sums the scenery lamps and car
  lights (`scenery_lamps_`, up to 64, headlights included) at each particle
  on the CPU and passes the sum in the vertex normal. It uses
  `scene_shadow.sh`'s lamp law (window, attenuation, spot cone, low-beam cut)
  without the facing term, distance floored at 1 m, weighted by
  Henyey-Greenstein g = 0.55 toward the camera (capped at 3×) × 0.6, the sum
  capped at 1.2. A per-pixel `lampLighting` under dozens of overlapping
  puffs was the cost to avoid. The first night run, before the caps, filled
  the screen with lamp-lit beige smoke.
- Sun shadow. `vs_particle.sc` takes four taps of the sun cascades per
  vertex (`shadow2D` is a level-zero compare, valid in a vertex shader),
  10 cm bias, interpolated across the puff; `bind_scene_shadow(true, 3)`
  binds them. Direct sun and its rims are scaled by it, and the ambient's
  share of the sun drops from 0.1 to 0.04 in shadow.
- Depth without AO. `update_ambient_occlusion` runs the depth prepass
  (view 140) while particles live even with AO off (`scene_depth_active_`)
  and skips the AO passes; `RenderDebugOverrides::soft_particles` turns it
  off.
- `maxcount`. Each skid style keeps its own ring of `maxcount` × 14 quads
  (`skid_mark_capacity`, at most 16000 per style and 24000 in all): a style
  past its cap reuses its own oldest quad.
- Natives. `ParticleSystem.init` gives the script object its `id` but no
  render type, so the entry's SMOKE.SCX mesh is not drawn as an object.
  `setSource` and `setDirectSource` map pos/rmin/rmax and vel/vmin/vmax to
  two spheres (the cfg's `velscale` still applies). `setFreq` multiplies the
  rates, `stop` stops the sources, `delAction` removes one action, and
  `setCounter`/`getCounter` count the cloud's particles in a sphere.
  `ResourceRef.destroy` drops the system; its particles finish their lives.
  Script particles have no ground plane (zero normal).
- Traffic. `update_skid_marks(rec, dt, 60)` runs for native traffic: marks
  and smoke within 60 m.
- Cost. The noise drops to 3 + 2 octaves (from 4 + 4). Puffs whose centre is
  nearer than 0.35 m are not drawn: their billboard is at zero alpha. The
  near fade lengthens with the puff's radius.

Validation (second pass):

- Compilation: `slrr_host` Release builds. `tests/test_tyre_effects.cpp`
  passes 8/8 (new: per-style rings, script domains without a floor).
  `tests/test_ground_surface.cpp` passes 7/7.
  `scripts/render_native_coverage.py --check` reports 367 bodies, 3 no-ops,
  6 missing.
- In game (temporary forced-smoke hook, since removed), all on
  `build/needle-game` unless stated:
  - ValoCity at 22:00: the smoke takes the street lamps' warm light and some
    red near the tail lights, no longer saturated.
  - `perf-roots/noao` (AO off): `gpu_ao_prepass_ms` is 0 without particles
    and about 0.2 ms with smoke, which stays soft.
  - Worst second with the camera inside the cloud, 2560×1440:
    `gpu_world_ms` 10.6 ms before the octave cut, 4.8–5.6 ms after. Other
    builds were running on the machine, so the figures are noisy.
- Not seen in game: a script-driven `ParticleSystem` (no stock caller
  outside commented-out `Osd` code), traffic sliding near the camera, and
  smoke under a building's sun shadow.

Third pass (the second pass's limits):

- Lamp shadows. The lamp sum moves from the CPU to `vs_particle.sc`. It
  reads the light data texture and, per shadowed lamp, its scenery and
  car-only tiles in `s_fixtureShadow`, or a headlight's tile in
  `s_headlightShadow`: four taps each, a 10 cm offset toward the lamp, no
  receiver gradient. The gain drops to 0.4, and the per-vertex sum is
  capped at 0.8: from inside a lamp-lit cloud at night, the first try still
  gave a bright beige wall.
- Indoor sun. With no cascades, `sunVisibility` uses the fitted field
  (`u_staticShadowMtx`) and its distance fade.
- Smoke shadows. `update_smoke_shadow` (`bgfx_shadows.cpp`) runs right after
  the car depth map, with its view and projection: sun-facing discs of every
  cloud particle within 20 m.
  - Output: RGB 1 − 0.75 × alpha × density, blended by DST_COLOR, ZERO;
    alpha the light depth of the puff's front, `gl_FragCoord.z` − radius ×
    z / 48, blended by MIN.
  - `bind_baked_light` binds the map at stage 14 and sets
    `u_carDepthParams.w`.
  - `fs_mesh.sc` takes `mix(1, rgb, smoothstep(front ± 0.01, depth))`. It
    scales the dynamic sun, or the baked light through `mix(0.55, 1, ·)`
    like the car shadow.
- `move`. PAPI `pMove(velocity, rotation)`: `move m0 0 0` in grass.cfg is
  read as "the engine moves this type 1 debris itself". A cloud with
  velocity 0 stays put; debris always integrates.
- `bone`. `character_bone_world` (`character.cpp`) returns a bone's pose
  (centimetres to metres). The source natives resolve the name with
  `character_find_bone` on the parent's render, and the action follows that
  bone (`script_particles.cpp`).
- `modePermanent`. Its handler 0x47F150 sets or clears bit 0x20000000 of
  the node flags at +0x6C. The node updates (0x520D60 and its two siblings)
  clear 0x80000000, let the children set it, then set the idle bit
  0x40000000 unless 0x80000000 or 0x20000000 is set; 0x451525 and 0x451876
  clear the idle bit again. Read as "permanent nodes never go idle": the
  host keeps permanent systems emitting at any distance, and lets the
  others idle past 200 m of the camera.
- Traffic slide loop. `update_slide_audio` runs for native traffic within
  40 m, with at most two traffic voices at once.

Validation (third pass):

- Compilation: `slrr_host` Release builds; `tyre_effects` 8/8 (now also
  checking the move flags), `ground_surface` 7/7. Native coverage and
  knowledge refs check clean.
- In game, ValoCity at 13:00 with forced smoke and temporary hooks (since
  removed):
  - With the smoke itself hidden, a debug tint in `fs_mesh` showed the
    smoke shadow's footprint confined to the ground behind the car.
  - Without the tint, that footprint measured 92 against 125-127 (±1
    between runs) with the shadow map off.
  - An earlier whole-frame darkening between runs was run-to-run sky
    variance, not the shadow.
- ValoCity at 22:00: lamp-lit smoke from outside; from inside the cloud, a
  dim fog with the scene visible through it.
- Not seen in game: lamp shadow tiles cutting the light in smoke, a bone
  source and the idle distance (no stock caller), a traffic slide loop,
  indoor smoke.

Fourth pass (the smoke shadow's reach):

- The map no longer rides on the car depth map. `update_smoke_shadow(video)`
  runs every frame after it, with its own orthographic box from the held sun
  direction, fitted to every cloud particle (light-space bounds plus 1 m,
  sides clamped to 2-200 m, depth the smoke's span plus 2 m). It is 1024 px,
  has its own matrix (`u_smokeShadowMtx`) and a half-metre depth tolerance
  (`u_smokeShadowParams.y`). It needs the shadows on and the sun above the
  horizon.
- `smokeShadowTransmit` in `scene_shadow.sh`, bound by `bind_scene_shadow`
  at stage 14, is read by `fs_mesh.sc` (as before) and by `fs_vehicle.sc`,
  where it scales the sun's diffuse and specular.
- Validation: in game, ValoCity at 13:00, forced smoke hidden and a debug
  tint since removed (red in `fs_mesh`, blue in `fs_vehicle`). The
  footprint reached the start burnout more than 30 m behind the car and
  tinted a pedestrian standing in it. The car's rear, inside its own smoke,
  turned blue.

Fifth pass (from the exe instead of host choices):

- The wheel's effects (0x453680, per wheel with load and a ground shape):
  - The ground record keeps `skid_PS` as a link at +0x5C (handler 0x462C60),
    `skid_SFX` at +0x6C, `skid_SM` at +0x7C (0x462E00) and `skid_SMcolor` at
    +0x8C (0x462FA0).
  - σ ≤ 0.05 or σ − 0.4 ≤ 0 releases both of the wheel's systems.
  - Above that, the wheel creates the `psws` particle system from the
    `skid_PS` link, with callback 0x451280, and the `smws` mark from the
    `skid_SM` link, with callback 0x451540.
  - σ is the wheel's +0x154, written by the tyre force (0x4561B2,
    0x456426) as √(sin² a + (cos a × k × scale)²). sl-remastered's
    `calculateTireSlipForce` has the same magnitude.
- `psws` (0x451280):
  - frequency (+0xC) = max(1, (σ − 0.4) × 0.2); a positive value clears the
    idle bit.
  - The source sits on the tread, ±0.15 of its width at random, lifted to
    0.1 m above the ground along the normal.
- `smws` (0x451540):
  - The point is the contact point plus 0.04 × the normal.
  - Its alpha is A(`skid_SMcolor`) × min(load / wheel+0x214, 1) × the same
    max(1, ...) factor.
  - 0x521620 keeps a ring of `maxcount` points per wheel's system.
- `updateWheelSlip` constants (0x60E02C-0x60E048): limits 1 and 1,
  relaxations 0.3 and 0.5, transition 0.1, threshold 0.1 m/s, damping 0.1
  and 0.2. Its relaxed branch's steady state is slip / 15 and lateral / 50,
  about 20 ms to settle, so `tyre_combined_slip` takes it once per frame.
- Host changes:
  - `tyre_combined_slip`, `tyre_effect_frequency` and
    `tyre_effect_intensity` (`skid_marks.cpp`) replace the SLRR_rev and
    OpenSLRR thresholds and the soft ground's rolling dust (the exe has
    none).
  - Marks take the exe's gate and alpha (the load fraction, against half the
    tyre's `maximum_load`, 4000 N when unset). Their width follows the exe's
    intensity min(σ − 0.4, 1).
  - The extra density is option 33, `tyre_smoke_density`.
- `modePermanent`, the idle trigger traced:
  - The source actions (0x51C050 and siblings) set the active bit
    0x80000000 when they run.
  - On an idle system, a source action is marked done and dropped
    (0x51C061). An idle system with no particle left is finished (0x51B3B0).
  - `script_particles_update` now drops an idle system's sources and wakes
    it on a positive `setFreq` or a new source. The 200 m rule is gone.
- Validation: `tyre_effects` 9/9, with `original_effect_law`: a standing
  burnout gives σ 1; a 22° drift without wheelspin gives 0.37, under the
  gate, and a little wheelspin crosses it. In game, a ValoCity handbrake
  slide (temporary hook) raised up to 769 particles and 370 mark quads, and
  the Dead_Island bench (no hook) raised sand dust and marks.

Sixth pass (Elias's test: "the smoke makes little balls"; asked for an
amount from the tyre's state and smoke from the wheel arches, as in CSP):

- Amount: `emit_tyre_dust` now takes the friction power at the patch,
  |Fx slip| + |Fy lateral| (the thermal model's sliding heat). Rubber smoke
  gets the cfg's rate per 15 kW, times 0.08 + 0.92 × smoothstep of the tread
  surface temperature from 110 to 230 °C (Elias's burnout read 318 °C on
  the HUD). Loose ground and debris get the rate per 30 kW, with no
  temperature. The exe's emission gate (σ > 0.4) is dropped for smoke and
  kept for marks; `tyre_smoke_density` defaults to 100 % again.
- Place: `ParticleSystem::due` and `emit` place each particle. Smoke is born
  round the tyre, at angle 1.7 × u^1.6 from the patch up the side the tread
  runs to, ±half the tread width across, and at least 1.6 tyre radii wide.
- Motion: each particle leaves with 0.6 × the car's speed, 0.3 × the slip
  speed (capped at 18 m/s) along the tread, 0.4-1.8 m/s outward from the
  arch and a little up. It swirls about the axle at 0.12 × the wheel's
  spin, capped at 10 rad/s, fading in 0.35 s. The pool grows to 6000.
- Look:
  - Each puff's own normal bends 55 % toward the sky, and the self-shadow
    and diffuse contrast are lower, so a cloud shades as one body.
  - The rims soften from r² 0.1, and alpha × 0.55 (from 0.85) lets more
    puffs overlap.
  - A grey cfg colour is lifted 60 % toward white, scaled down by its
    saturation, so sand keeps its colour.
  - The shadow map keeps at most 40 % of the sun per puff.
- Validation: `tyre_effects` 10/10 (with `placed_emission_swirls`);
  `options_retry` passes. In game (temporary hooks, since removed):
  - The handbrake drift heated the tread to only 40-75 °C, so it made
    little smoke, as the law intends.
  - With 80 kW forced on the rear wheels, smoke rose from the rear tyre and
    its arch as a white continuous cloud, a whiteout around the camera as
    in Elias's CSP reference.

Seventh pass (Elias's test: too little smoke, "small drops pulled up far too
fast"):

- Tyre smoke and dust feel 15 % of their cfg's gravity
  (`Particle::buoyancy`): asphalt.cfg's +2 m/s² with damping 0.01 lifted
  puffs at about 2 m/s.
- `particle_width` grows a puff by the square root of its age fraction, so
  it swells within its first second.
- Smoke gives the cfg's rate per 8 kW, a quarter of it cold and all of it
  from 160 °C (ramp from 60 °C); puff alpha × 0.7.
- `patch/particles/scripts/particle_clouds/asphalt.cfg` (Elias ships the
  patch):
  - puffs born 0.6 m wide that swell to 4-7.5 m and live 4-9 s;
  - lighter greys, gravity 0.6, damping 0.15, 30 per second.

Eighth pass (Elias: "top", but a slide under 80 km/h filled the track):
smoke gives the cfg's rate per 20 kW (from 8), a tenth of it cold, all of it
from 200 °C (ramp from 80 °C): about a third of the smoke for the same
slide.

Ninth pass (Elias: no sparks or water, sand raised almost nothing, "an
orange smoke"):

- Sand and loose ground:
  - Dust gives the cfg's rate per 8 kW of sliding power (from 30), plus up
    to 0.8 × that rate from a wheel rolling faster than 3 m/s (0.3 for
    debris).
  - `attach_ground_surfaces` decodes each loose ground's SCY texture on the
    CPU (`ground_texture_color`, `paint_image_from_texture`, cached by path)
    into `GroundSurface::dust_color`. The dust takes 80 % of it (debris
    50 %) and is born 1.2 tyre radii wide.
  - Dead_Island's bench raised an orange column.
- Sparks:
  - The stock spark.cfg (particles:0x106) has a new `emissive` flag (set for
    a cfg named spark*).
  - `draw_particles` adds sparks after the smoke as streaks along their
    screen motion (25 ms of travel, at least 2 cm wide), fs_particle.sc mode
    4, blended ONE + ONE.
  - A rim on the road sprays them at 0.3 × min(speed / 15, 2) of the cfg's
    rate, with 0.85 × the car's speed and up to 3.5 m/s up the normal.
  - A hit faster than 4 m/s (`emit_impact_sparks`, beside
    `play_impact_audio`) bursts (speed − 4) × 5, at most 60, along the
    contact normal.
- Water:
  - The three missing GroundRef natives are bound (`natives_particles.cpp`
    to `water.cpp`). `setWater(level, ...)` and `setWater(point, normal,
    ...)` set the surface plane (and forget the last limits);
    `addWaterLimit` removes water on the side its normal points to.
  - The water belongs to the GroundRef that set it and goes when that
    object does.
  - A wheel whose bottom is under the water throws water.cfg spray
    (particles:0x105) at the surface, more with speed and depth.
  - Native coverage: 370 bodies, 3 explicit no-ops, 3 missing (the welder
    leftovers).
- Validation:
  - `tyre_effects` 12/12 (new: `water_plane_and_limits`, `tinted_dust`).
  - In game, with temporary hooks since removed: sparks forced on the rear
    rims showed yellow streaks under the car at night; the Dead_Island bench
    raised orange dust.
  - Not seen in game: water spray (Valocity's sea needs a car off the
    quay), and impact sparks (the test crash rolled on grass, under 4 m/s
    on a hard surface).

Tenth pass (Elias: twice too much dust on sand; the smoke still looks like
little drops instead of an irregular shape):

- Dust: the cfg's rate per 16 kW and up to 0.4 × it from a fast rolling
  wheel (0.15 for debris), half the ninth pass.
- Shape:
  - `fs_particle.sc` reads its noise in world space, at a point inside the
    puff (0.32/m body, 1.15/m detail), drifting slowly up. Neighbouring puffs
    share the field and read as one irregular volume.
  - The same field eats each puff's outline (no round rim).
  - Elias kept the first world-noise shape and asked for it less
    opaque. The squared density of a second try left almost no smoke in
    his 250 °C burnout. Each puff's alpha is × 0.4 (from 0.7), and grey is
    lifted 60 % toward white.
- Smoke shadow: each puff keeps 20 % of the sun (from 40 %), and a texel
  lets at least 60 % through; the first try left near-black patches on the
  road.
- In game (forced smoke, temporary hook since removed): irregular
  translucent wisps over the road, light ground shading.

Options screens (Elias: put the density in the Options menu):

- `OptionsDialog.CMD_TYRE_SMOKE` (537) sets option 33 from a TYRE SMOKE
  slider (0-400 %, steps of 50) on both screens: the Graphics tab's WORLD
  column (`OsdxOptions`) and Advanced Video Options (`OptionsDialog`). It is
  not a preset setting, so moving it keeps the preset.
- Compiled with `compile_game_sources` in `build/opts454-root` (a real copy
  of `sl` and `system` with the patch's classes, the other folders as
  junctions to a stock root). HEAD's four `sl/Scripts/game` sources first
  rebuilt the shipped classes byte for byte. `OsdxOptions` needs a second
  run once the new `OptionsDialog.class` carries the constant.
- Not checked in game yet.

Limitations:

- Water has no physics: no buoyancy or drag from its density and viscosity.
- The tyre's longitudinal scale in σ (wheel+0x218) is taken as 1; its
  source field is not traced. The nominal load of the mark alpha
  (wheel+0x214) is taken as half the tyre's maximum load.
- The marks keep their own rings (`maxcount` × 14 quads per style, not
  `maxcount` points per wheel) and a 1 cm lift instead of the exe's 4 cm,
  which would float on this renderer's flat quads.
- The mark width's driver in the exe is not traced.
- Rendering constants of the volumetric model have no original
  counterpart: the colour lift, the lamp gain and caps, and the 0.8 m width
  cap.
