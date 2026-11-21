# Gameplay, camera, input and nitrous triage (#204)

**Incomplete spike, not a gameplay-fix release.** Audited unmodified main
`7a430457a52e2ea4b44b671a24d4b0ed2099bab8` against the installed SLRR 2.2.1 MWM
scripts/data. This records all ten checklist items in
[the request](<https://github.com/EliasFD3S/openslrr/issues/204>) and the
[additional cockpit/driver reports](<https://github.com/EliasFD3S/openslrr/issues/204#issuecomment-7025864360>).
No engine, Java, asset or rendering behavior is changed by this PR.

A source-confirmed omission is distinguished from a reproduced playtest and
from a hypothesis. In particular, neither every reported failure nor complete
nitrous/flame gameplay has been reproduced. Missing reproduction fixtures and
board access are listed below; do not mark the original acceptance checklist
complete from this report.

## Findings and estimates

Estimates are indicative engineering effort including focused validation, not
measured implementation times. P1 is destructive input, P2 compatibility work,
and P3 optional/presentation work. These are proposed priorities, **not confirmed
project-board fields**.

| # | Report | Current evidence and bounded next step | Estimate / follow-up |
| --- | --- | --- | --- |
| 1 | Loading clicks remove parts | Unsafe loading/transition click path confirmed in source; exact destructive sequence not reproduced. Discard/rearm world gestures at boundaries and gate producer plus Mechanic consumer. | 1–3 days, P1 [#295](<https://github.com/EliasFD3S/openslrr/issues/295>) |
| 2 | Camera cursor in main menu | **Reproduced**: main-menu RMB enters camera pointer `M`, release restores `J`. Drag begins even without an eligible camera. | Menu-only gating 0.5–2 days; shared ownership work 2–4 days, P2 [#296](<https://github.com/EliasFD3S/openslrr/issues/296>) |
| 3 | Bare-chassis click has no dialog | Normal assembly mode explicitly ignores chassis in stock MWM. Tuning is a separate positive case, conditional on `isTuneable`. No universal chassis dialog should be invented as a bug fix. | Expectation check first; positive tuning-path diagnosis 1–2 days if it fails; see reproduction matrix below |
| 4 | Garage engine-test camera hotkeys | Raw arrow reads bypass controller reset/group bindings in two places. The exact preset-key report is not reproduced; stock default zoom is not all disabled in drive mode. | 1–3 days within P2 [#296](<https://github.com/EliasFD3S/openslrr/issues/296>) |
| 5 | Driving cursor and look return | OS cursor visibility does not honor script cursor enable state. Follow-camera mouse offsets persist without release decay. Original return timing/damping is not established by scripts. | Visibility in [#296](<https://github.com/EliasFD3S/openslrr/issues/296>); follow-only return 1–2 days after reference comparison, P2 [#297](<https://github.com/EliasFD3S/openslrr/issues/297>) |
| 6 | ROC/day entry cameras | ROC's timed `angle` command is absent. Day entry instead uses a staging `setMatrix`; bound follow-pose precedence can overwrite it. Full career sequences not reproduced. | 3–6 days, P2 [#298](<https://github.com/EliasFD3S/openslrr/issues/298>) |
| 7 | Custom gauge texture/font mismatch | No failing pack/CFG or expected image supplied. First-texture and glyph-layout assumptions are candidates, not a proven cause. Stock LCD font selection is not itself a generic-font bug. | 0.5–1 day reproduction, then 1–3 days for a demonstrated local fix, P2 [#299](<https://github.com/EliasFD3S/openslrr/issues/299>) |
| 8 | Flaps use interpolation, not physics | **Outdated as a blanket claim**: current code integrates one-DOF hinged plates with gravity, carrier/wind loads, stops, latch and static-world contacts. It is not a full reciprocal rigid-body joint solver. | No missing-hinge rewrite justified; compare remaining model limits with [physics #221](<https://github.com/EliasFD3S/openslrr/issues/221>) |
| 9 | Catalog `typeID` misclassification | Not reproduced. Stock MWM explicitly puts Replacement Parts and Neon Lights under Body. No demonstrated truncation/misparse explains the report; preserve data/parity. | Named offending part/RID/page required; 0.5–1 day diagnosis, then 0.5–2 days if a local parser bug is proved, P2 [#300](<https://github.com/EliasFD3S/openslrr/issues/300>) |
| 10 | Nitrous and flames unverified | Missing driving activation/consumption chain is source-confirmed; dry torque is always requested. Authored stock NOS flame meshes are always rejected by light-state gating. Generic ParticleSystem is a separate missing feature. See diagnostic evidence below. | Gameplay 3–6 days, P2 [#303](<https://github.com/EliasFD3S/openslrr/issues/303>); Stock mesh/light fixture 1–2 days and indicative integration 2–4 days, P3 [#304](<https://github.com/EliasFD3S/openslrr/issues/304>) |
| 11 | Interior gauge needles missing | Stock `cockpit_speed`/`cockpit_rpm` CFG declarations have no parser/storage/render-update consumer. Different feature from overlay `osd_*` pins. | 2–4 days, P2 [#301](<https://github.com/EliasFD3S/openslrr/issues/301>) |
| 12 | Mitch's hands miss steering wheel | Seated-driver arm IK exists. Fixed grip/wrist targets and lack of a palm-contact constraint are limitations; exact reported offset is not measured. | 0.5–1 day measurement, then 1–3 days correction, P2 [#302](<https://github.com/EliasFD3S/openslrr/issues/302>) |
| 13 | No gear-shift animation | Gearbox shifts, but driver pose has no accepted-shift/lever gesture phase and keeps both hands on the wheel. Stock gesture asset/timing not identified. | 0.5–1 day reference study, then 3–5 days integration, P3 [#302](<https://github.com/EliasFD3S/openslrr/issues/302>) |

## Input, chassis and camera evidence

### Loading is not just a debounce problem

The [outer loading call](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/jvm/jvm.cpp#L943-L957>) presents
before executing the Java loading-screen method. The
[presenter](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/boot/boot.cpp#L287-L298>) pumps
window messages while the overlay is visible but before that Java modal exists.
[Press/release handling](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/render/bgfx_lib_backend.cpp#L2488-L2557>) checks
modal/FMV state, not the loading overlay or transition. The
[Mechanic dispatcher](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/boot/mainloop.cpp#L295-L366>) lacks
a matching suppression guard. Stock assembly click handling then normally moves
a non-chassis part into inventory.

There is [one pending part-click latch and gesture edges](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/include/slrr/host_input.hpp#L42-L57>),
not an unlimited native click FIFO. The source establishes unsafe windows; an
instrumented load/return reproduction must establish which window causes the
reported removal. A modal-only guard would miss the early overlay window and a
stale click delivered after the modal closes.

Validate spamming the future part location during career load and catalog/dealer
return, plus holding through dismissal. Installed/inventory counts must stay
unchanged until a fresh eligible press/release. Preserve loading-dialog
click-to-continue. Existing inventory-transition checks cover lifecycle/save
ordering, not this pointer isolation.

### Cursor and mapped-input ownership

[RMB down](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/render/bgfx_lib_backend.cpp#L2473-L2486>) starts
camera drag unconditionally; only clipping is eligibility-gated. The
[cursor helper](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/platform/host_cursor.cpp#L151-L161>) changes
to pointer `M`. This reproduces independently of the older fix excluding cursor
cameras from main-world selection.

Stock Garage resets into DRIVERSET during engine test. Both
[host look axes](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/platform/host_input.cpp#L247-L253>) and
[orbit input](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/gameref.cpp#L1009-L1024>) instead
read raw arrow states. The controller's `camera 0`
[path](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L1917-L1927>) does
not retain/clear a camera ownership binding. Fix mapped look eligibility, not the
stock control-set scripts. Exact F-key/preset leakage still needs a named key
and reproduction; PageUp/wheel zoom also has stock DEFAULTSET bindings.

The OS [cursor handler](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/render/bgfx_lib_backend.cpp#L2507-L2510>) always
installs a visible shape. Generic non-camera cursor enable/disable commands have
no visibility owner. Follow-camera
[pose calculation](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/gameref.cpp#L779-L850>) uses
persistent mouse offsets; smoothing toward that offset target is not spring
return. Keep driving transient look separate from intentionally persistent garage
orbit and explicit free camera. Test mode/section/modal/focus transitions.

The optional persistent orbital toggle with controller look remains a separate
product/design scope, not a prerequisite for restoring follow behavior. Existing
bindings are reusable, but camera yaw/pitch currently bypass mapped look axes;
merely mapping the right stick does not complete that enhancement. Peripheral
integration is already being changed in [PR #227](<https://github.com/EliasFD3S/openslrr/pull/227>).

### Chassis expectation and race sequences

In the pristine MWM mechanic handler, ordinary assembly mode deliberately skips
chassis clicks. Tuning mode opens a dialog only for `isTuneable`; the chassis
implementation returns `brake_balance_can_be_set`, normally zero. The Baiern
CoupeSport GT III sets it to one. Required comparison:

- Normal chassis, assembly: no dialog is stock behavior.
- Non-tuneable chassis, tuning: no tuning dialog is expected.
- GT III chassis, tuning: brake-balance dialog should open.
- Known tuneable non-chassis part: control case for picking/dialog delivery.

If the positive case fails, trace picked render owner → script class → mode →
`isTuneable` → dialog. The report does not authorize a new universal chassis UI.

ROC's real career entry sends `angle 0 5.0 0.4853` after its race dialog, then
changes the command during countdown. The
[camera command parser](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/gameref.cpp#L483-L585>) has
no `angle` implementation (`torque` is also accepted without effect). ROC
`testMode` skips initial race-dialog/angle work, so track-test entry is not a
valid reproduction of the career sequence.

Day entry instead stages a camera matrix and starts the countdown. A
[script matrix update](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/gameref.cpp#L244-L260>) does
not release/invalidate a bound follow pose; the driving pose calculation wins
before generic/script orbit. That is a conditional explanation of lost day
staging, not proof of the whole day sequence failure. Recover generic timed
rotation semantics and separately define script/controller pose precedence;
validate actual challenge → RaceSetup → start and ROC career dialog/countdown,
including cancel/retry. Do not hardcode a race movie. Existing route/gate tests
are not camera-sequence acceptance.

## Gauges, catalog and driver evidence

The [HUD parser](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_config.cpp#L175-L199>) supports
eleven overlay commands. The
[HUD selection](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L6024-L6028>) uses
the controlled chassis layout, not every installed part's layout. The
[renderer](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/render/bgfx_osd.cpp#L482-L489>) binds
the first texture; digital glyphs
[assume ASCII-indexed quads](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/render/bgfx_osd.cpp#L694-L701>).
Defined custom font overrides are loaded directly. Stock `cars:58` resolves to
the LCD font, its SCX and digital atlas. None of these observations proves which
assumption the unavailable custom pack violates. Loose `--vehicle-osd` preview
has special resolution paths, so preview success is not installed-pack parity.
Trace raw/qualified IDs, mesh materials, every texture path and glyph mapping
with the actual pack before changing resource or font primitives.

Stock cockpit speed/RPM declarations reference a 3D pin resource and authored
pose/angle mapping. They are not overlay pins. Active
[PartConfig](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/include/slrr/part_config.hpp#L98-L133>) has
no cockpit instrument collection and the parser has no `cockpit_speed` or
`cockpit_rpm` branch. Implement generic parsed instrument data and parented
renders with live RPM/km/h angle/clamp updates and teardown, not a car-specific
Java workaround.

Stock catalog Body entries explicitly select `parts:F24F` (Replacement Parts)
and `parts:F24B` (Neon Lights). The inspected parts pack has both as children of
Body; the two aliases both say “Replacement”, and F24B contains neon scripts.
[TOC parsing](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/rpk_index.cpp#L188-L212>) reads
32-bit IDs; [child traversal](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/rpk_index.cpp#L795-L805>) resolves
dependency-qualified parents. Examined IDs fit the local16 convention. Duplicate
`D282` entries (orange neon and V6 folder) are a real data ambiguity with
first-wins lookup, but not a demonstrated cause of the report. The stated
Engine-neon quirk is not corroborated by this installed MWM catalog mapping;
another revision may differ. Do not relocate categories or change ID semantics
without the offending resource and original-game comparison.

[Driver posing](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L4936-L5074>) already
parents the character to the seat and solves arms. Both hands target wheel
points using inferred radius and fixed grip/wrist offsets; palm orientation and
finger contact are not constrained. Measure target/wrist/palm residuals and
reach-clamping before blaming rig scale, frames or offsets. No gear-dependent
phase is present in that pose routine, although
[gearbox updates](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/physics/vehicle_dynamics.cpp#L458-L505>) exist.
A wheel → lever → wheel gesture needs accepted-shift timing/reference and
pause/exit/driver-swap cancellation. No stock shift clip has been established.

## Hinges and nitrous

The existing [hinge contract](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/docs/ENGINE_CONTRACT.md#L528-L575>) describes
a physical one-DOF panel, not linear interpolation. The live runtime uses that
solver. Limitations remain: panel contacts do not push the carrier back; there
are no panel contacts with other cars, loose bodies, other panels or floor;
garage-suspended panels do not contact the world; hinge state is not saved.
The existing twelve hinge-kernel checks passed in this ticket, not a new
full-vehicle visual parity pass. No coordinated general joint redesign is
needed to triage the outdated report.

Nitrous Java kit parameters reach DynoData, but
[drive torque](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L2977>) always
samples nitro `0`. The [controller update](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/platform/host_input.cpp#L281-L302>) carries
no nitrous driving action. [HUD state](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L6033>) is
only `tank_nitro > 0`; the stock chassis sums canister **capacity**, not remaining
`actual`. `setNitroSFX` only stores its sound/pitch. The
stock NOS flame is **an authored mesh/light**, not necessarily a ParticleSystem
emitter. The inspected muffler's `particles:1E` render has flags `256` and resolves
to `MW_Mod:28`, the NOS flame mesh, with its texture. The exhaust also authors a
flag-256 fakespot light. [Part rendering](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L695-L744>) retains
the flag, but [scene collection](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/scene_instances.cpp#L2065-L2067>) gates
it through [part_lamp_lit](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/src/resources/part_runtime.cpp#L1631-L1647>), which
rejects flags without brake/night bits. Pure flag-256 NOS meshes are always
hidden, and the NOS light has no active-state owner. The
[missing ParticleSystem API](<https://github.com/EliasFD3S/openslrr/blob/7a430457a52e2ea4b44b671a24d4b0ed2099bab8/docs/NATIVE_AUDIT.md#L105-L112>) is
a separate gap, **not a prerequisite for activating this stock mesh**. Verify
purge semantics for the target version rather than assuming a stock purge action.

A one-off headless diagnostic was compiled/linked against this ticket's Release
host objects. It copied original TUFA classes into an isolated fixture, spawned
the stock Baiern, installed the compatible six-channel NOS rail and 12 lb bottle,
set injection to its authored maximum, and invoked the real Java `forceUpdate`.
The fuel rail remains installed in its distinct slot; a normal stock spawn alone
has no NOS kit. The resulting Java values were `nitro_H=1.362304`,
`nitro_cooling=0.637696`, and `consumption_nitro=1.811520`.

| Measurement | Observed result |
| --- | --- |
| Dyno at 3500 RPM, dry / half / wet | 198.132523 / 329.860504 / 461.588470 in the current dyno output units |
| Ten seconds, full throttle, N released (`axis31=0`) | 130.680413 km/h, 5491.134434 RPM, gear 3; bottle `actual` 5.454545 → 5.454545 |
| Reset, ten seconds, full throttle, N held (`axis31=1`) | 130.680413 km/h, 5491.134491 RPM, gear 3; bottle `actual` 5.454545 → 5.454545 |
| Synthetic `actual=0`, then Java refresh | `tank_nitro=5.454545`, HUD fill 1.0; this is an empty-bottle injection, **not observed depletion** |
| Stock `reload` Part command | Unhandled (`false`); this alone is not a complete compatibility verdict |
| Installed NOS mesh nodes | Two flag-256 mesh nodes created; zero eligible through the production light predicate; direct flag-256 check also false. This is not a rendered-pixel test. |

The dry/wet dyno banks demonstrably differ, yet the controlled physics run has
no nitrous response or consumption despite a valid logical action. This is a
reported missing end-to-end chain, not a successful nitrous acceptance test.
The pad/controller setup is synthetic; original RPM/throttle eligibility,
refill/save lifecycle, sound, purge and flame pixels remain unvalidated. The
probe is a local report-only fixture, not a committed passing gameplay regression.
Coordinate future dyno work with [PR #215](<https://github.com/EliasFD3S/openslrr/pull/215>)
rather than duplicating its torque-bank/math changes.

Read-only upstream comparison pinned SLRR_rev `v221` at
`2663d6864e5219e55b7a5744bb3238dae42f130f`:
[dyno bindings](<https://github.com/nedobylskiy/SLRR_rev/blob/2663d6864e5219e55b7a5744bb3238dae42f130f/src/runtime/dyno-native.ts#L42-L60>) ignore
the nitro argument, and [mechanical driving](<https://github.com/nedobylskiy/SLRR_rev/blob/2663d6864e5219e55b7a5744bb3238dae42f130f/src/engine/mechanical-system.ts#L137-L143>) samples
the primary bank. No ready activation/depletion/purge port was found in those
inspected owners. Newer upstream [flap motion](<https://github.com/nedobylskiy/SLRR_rev/blob/2663d6864e5219e55b7a5744bb3238dae42f130f/src/engine/part-flap.ts#L6-L26>) is
half-second smoothstep interpolation, not a physical replacement for this
engine's existing hinge solver. Historical local documentation saying upstream
has no flap native must not be treated as a claim about that newer revision.

## Verification performed here

- Windows x64 MSVC Release host build of unmodified `7a43045`: passed.
- Existing standalone suites: `menu_debounce` 1 passed; `car_select_view` 3
  passed; `ui_lifetime` 3 passed; `driver_mask` 1 passed; `flap_hinge` 12 passed.
  These cover their existing assertions, not all issue scenarios.
- Host `--probe-dyno`: passed (`437rpm torque=82.0873 W=4781.29`).
- Reset private-root Baiern garage launch: rendered garage/car inspected.
- Reset private-root garage → main menu launch, then one synthetic Win32 RMB
  press/release: main menu inspected; log order was `adopt MainMenu`, pointer
  `M`, pointer `J`, normal exit `0`. This confirms cursor leakage, not visible
  movement of a menu-background camera.
- No fresh full career loading-click, tuneable-chassis, garage engine-test,
  original follow-return, race-sequence, custom-gauge, catalog-misclassification,
  cockpit-needle or driver-contact/gesture playtest is claimed.

Representative commands (from the ticket root; all game roots private):

```powershell
powershell -File C:\openslrr-superagent\tools\Build-SlrrTicket.ps1 -TicketDir <ticket> -GameRoot
powershell -File scripts/run_save_io_tests.ps1 -Suite menu_debounce
powershell -File scripts/run_save_io_tests.ps1 -Suite car_select_view
powershell -File scripts/run_save_io_tests.ps1 -Suite ui_lifetime
powershell -File scripts/run_save_io_tests.ps1 -Suite driver_mask
# Existing hinge kernel compiled separately with MSVC, tests/test_flap_hinge.cpp
# + src/physics/flap_hinge.cpp, /std:c++20 /EHsc /MD /O2 /Iinclude.
build\Release\slrr_host.exe --probe-dyno
build\Release\slrr_host.exe --game --window --root data\game --spawn-car cars/racers/Baiern.rpk:0x6
build\Release\slrr_host.exe --game --window --root data\game --spawn-car cars/racers/Baiern.rpk:0x6 --auto-cmd mainmenu
# On the last run, send one WM_RBUTTONDOWN / WM_RBUTTONUP over the menu.
```

Baseline audit capture, `7a43045`, 640×480 client, windowed, reset private root.
This is evidence of the inspected scene, **not** a before/after rendering change:

![Baseline main-menu scenario](<https://raw.githubusercontent.com/EliasFD3S/openslrr/2387b5722ab19bb8cfecbeea7478cfb27df6e8f4/gameplay-triage-f4bc2dbea25c/audit-base-menu.png>)

## Reference provenance and remaining prerequisites

Pristine local MWM references (read-only; original assets/source are not shipped):

```text
C:\openslrr\open-SL\sources\system\Scripts\render\src\LoadingScreen.java:144–162
C:\openslrr\open-SL\sources\system\Scripts\io\src\MouseCursor.java:46–65,91–121
C:\openslrr\open-SL\sources\system\Scripts\io\src\ControlSet.java:265–268,312–314,337–341
C:\openslrr\open-SL\sources\sl\Scripts\game\src\Mechanic.java:220–279
C:\openslrr\open-SL\sources\sl\Scripts\game\src\Garage.java:938–949,981–991
C:\openslrr\open-SL\sources\sl\Scripts\game\src\ROCTrack.java:245–249,580–605
C:\openslrr\open-SL\sources\sl\Scripts\game\src\City.java:937–947,2969–2994
C:\openslrr\open-SL\sources\sl\Scripts\game\src\RaceSetup.java:192–218
C:\openslrr\open-SL\sources\sl\Scripts\game\src\Catalog.java:287–335,1472–1500
C:\openslrr\open-SL\sources\parts\scripts\bodypart\SRC\Chassis.java:56–57,582–621,811–823
C:\openslrr\open-SL\sources\cars\racers\Baiern_data\scripts\src\Baiern_CoupeSport_GT_III.java:25–26
C:\openslrr\open-SL\game\cars\racers\Baiern_data\scripts\_main_CoupeSport.cfg:54–61,289–365
C:\openslrr\open-SL\game\parts\mufflers\scripts\muffler_12.cfg:9–10
C:\openslrr\open-SL\game\cars\racers\Baiern_data\scripts\stock_exhaust_pipe.cfg:15–19
C:\openslrr\open-SL\game\parts.rpk
C:\openslrr\open-SL\game\cars.rpk
C:\openslrr\open-SL\game\particles.rpk (1E mesh resource, 1F fakespot)
C:\openslrr\open-SL\game\MW_Mod.rpk (28 mesh, 27 texture)
```

Follow-ups are created separately so small input/cursor fixes do not become a
hinge/particle/animation redesign. No production fix was folded into the spike.
Unmodified build/inspection is not a claim that all acceptance criteria passed.

Remaining prerequisites:

1. Exact failing custom-gauge pack/CFG/install and expected/original appearance;
   named misclassified part/RID/catalog page; expected chassis variant/mode and
   exact leaked preset keys. Without these, a generic parser/font/dialog fix
   would be speculative. The follow-up issues preserve these evidence gaps.
2. Original-game comparison for follow-return timing, timed camera command
   semantics and shift/lever gesture; actual career ROC/day sequence runs and
   nitrous/purge/flame visual acceptance still outstanding.
3. Project-board access: `gh` GraphQL `projectsV2` returned
   `INSUFFICIENT_SCOPES`; the configured token lacks `read:project`/project write
   access. The classic repository-project endpoint returned 404. Issues
   [#295–#304](<https://github.com/EliasFD3S/openslrr/issues?q=is%3Aissue+295+296+297+298+299+300+301+302+303+304>) exist
   with proposed priorities above, but **none has been added/prioritized on the
   board by this ticket**. No token/permission configuration was changed.
