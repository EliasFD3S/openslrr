# Cooperative scripts and GameType events

This implements priorities 1 and 3 of the native inventory. The loading-reset
and loader-priority natives are unchanged.

## Threads

`Thread.init`, `start`, `stop`, `isAlive`, `sleep`, `suspend` and `resume` now
manage real cooperative executions. Windows fibers retain the C++ interpreter
and native-call stacks, allowing nested Java calls to resume at the exact
instruction after a sleep or wait. All VM execution stays on the host OS
thread; there are no concurrent Java worker threads.

- `start` queues `target.run()` for a host frame. It does not execute it inline.
- `isAlive` covers runnable, sleeping, waiting and suspended threads. It becomes
  false on completion or a stop request.
- `sleep` uses milliseconds and a steady-clock deadline. Zero yields until a
  later scheduling pass. Outside a script thread it waits through the existing
  host pump when available.
- `suspend` prevents scheduling without losing the stack; `resume` restores
  eligibility without overriding an outstanding sleep deadline.
- `stop` requests cancellation and wakes a suspended/sleeping thread. The next
  resume/checkpoint unwinds interpreter scopes before releasing the fiber.
- TREE checks its time slice every 2,048 operations. Each resumption gets about
  1-16 ms according to `Thread.setPriority` (-10 to 10; nonpositive priorities
  retain a 1 ms slice). The scheduling pass has an 8 ms soft budget, raised
  to 16 ms while a MAX_PRIORITY thread is ready, and rotates eligible threads.
  Ordinary native C++ work cannot be preempted mid-call.

`Object.wait` yields within a script fiber and uses the existing modal pump on
the host context. Waiters are tracked separately: `notify` wakes one waiting
execution, `notifyAll` wakes all. MessagePort waits return immediately if a
message is already queued. Waking a message port never clears dialog modal
flags; dialog completion removes only its own modal shield. VM/GC depth counters are switched with each stack;
scoped roots retain suspended locals, operands, arguments and construction
state. See [VM_GC.md](VM_GC.md).

The mechanic/time-display compatibility ticks are skipped while their Java
threads run. The showroom retains host camera selection, but the pose comes
from `CarSelect.run` when its thread is alive. Sections with a running message
thread no longer also drain that port through the host fallback. Tuning slider
events are queued to a live part thread, with synchronous delivery retained
only when no script thread consumes its port.

## Notifications and callbacks

Both `addNotification` overloads, `remNotification`, `registerCallback`,
`unregisterCallback` and `unregisterCallbacks` now have implementations.

Notifications identify their source by its canonical GameRef instance, so
aliases match. Adding the same listener/source/type replaces the old watch;
removal matches the exact type. Dispatch supports event-mask filtering,
low-28-bit event aliases, custom messages (`0x10000000`), custom method names,
skip-related (`0x40000000`) and one-shot (`0x80000000`) flags. One-shot watches
are removed before invoking script code, including on a mask miss. Named
methods take the stock GameRef/event/int or GameRef/event/String signatures;
the default selects `handleEventAsync` when the listener enables async mode.

The event emitter is connected to existing timer and mechanic part-pointer
events, and to trigger volumes: `scene_triggers_tick` emits
`EVENT_TRIGGER_ON` (0x20) and `EVENT_TRIGGER_OFF` (0x40) from a
`GameRef.RID_TRIGGER` source when a car enters or leaves it, with the car's
`id()` as the String param that stock handlers read with `token(0)`. Missing
collision and traffic producers must still emit their events when
implemented. Legacy UI input paths remain.

Callback modes 8, 9 and 28 correspond to `control(float)`, `drive()` and
`animate()`. They have separate ordered lists, idempotent registration and
per-object unregistration. Snapshot iteration plus registration IDs permits
callbacks to remove/re-register entries without invoking stale registrations
later in the same pass. Nested modal pumps do not re-enter callback dispatch.
The host's existing frame delta is passed to control; this is not a port of
the original engine's complete simulation/substep pipeline.

Live thread objects, notification endpoints and callback receivers are GC
roots. Resource destruction removes associated watches/hooks; VM teardown
cancels suspended executions before freeing their stacks and registries.

## Section presentation

`GameLogic.changeActiveSection` and GameState `exit` hold world presentation
while their cooperative stacks are suspended. The last complete frame stays
visible until the transition finishes; explicit loading overlays can still
be presented. This prevents a destroyed CarLot camera from exposing the free
camera and partially detached cars. The scope unwinds on cancellation as well.

## Validation

Temporary headless diagnostics exercised:

- A real TREE `GameType.run` message loop, sleep, suspension, resumption,
  cancellation and collections while its frames are suspended.
- Finite-thread completion, zero-duration yields, separate waiters and a TREE
  loop without sleep that remains preemptible, collectable and stoppable.
- Callback order, duplicate registration and removal during dispatch;
  notification aliases, custom messages, replacement, one-shot/removal and
  native registration entry points.
- Stock Init followed by cooperative frames and repeated collections.
- The actual `Mechanic.handleEvent` tuning path with stock styles and
  rectangle synchronization: slider clicks reach the part, and only explicit
  OK closes the dialog. Assignment paths such as `dlg.osd.globalHandler = part`
  evaluate the intermediate instance receiver before writing the field.
- Cold CarLot navigation using the six local saved vehicles: the first
  transition dropped from about 1.7 s to 0.84 s in the headless probe. This
  excludes GPU uploads and does not imply instantaneous cold loading.
- Fuel-rail pointer hit-testing and dragging through a live part message
  thread while a modal dialog stays open until explicit OK.

These checks do not launch a game window. Full interactive garage/showroom
behavior and compatibility with mods still require in-game verification.

## References

`repos/slrr-reverse/engine/Runtime/System/GameType.cpp` documents watch flags,
replacement/removal and callback modes. `System_natives.cpp` documents the
budgeted cooperative pump and sleeping VM threads. The source declarations and
actual callers in SLRR_rev/the installed TUFA classes supply the script
contracts. The fiber scheduler is the host implementation, not a literal
copy of the original VM's internal stack layout.

## Boot section adoption (#76)

The stock Init/GameLogic chain already enters SplashScreen in Java. After Init,
boot uses `mainloop_adopt_section`, as the frame loop does, to synchronize the
host without invoking enter again. Replaying enter replaces the OSD reference
and schedules a second timer; adoption preserves the existing presentation.

Windowed startup still removes/re-arms only the splash timer after startup
work. GameRef timers use steady wall-clock deadlines, so loading may otherwise
consume the three-second display interval even with one enter. The initial
splash frame is still presented immediately after adoption; FMV/loading-frame
handling is unchanged. No Java or native-signature change is required.

`run_save_io_tests.ps1 -Suite splash_boot` runs real shipped Init/Splash code
with the Null renderer and checks one pending timer and stable OSD identity
across repeated adoption. Requires local `game` assets; it opens no window
and does not request a new career. With `-BaselineRef a5184d2`, the same test
fails with two timers; the corrected build passes with one. Existing
`inventory_transition` tests pass 4/4. Windowed FMV/splash visual continuity
still requires user validation. See [evidence](../assets/validation/splash-boot/results.txt).

### Track presentation without a menu plate

A driving HUD normally has no fullscreen menu background. The renderer's
missing-menu backdrop must therefore be suppressed when
`fullscreen_script_camera` selects an active world camera. Without that check,
Select Track can display a solid white mask above the map but below the HUD;
opening a pause menu supplies a background and temporarily removes the mask.
The fallback's dark tint must also be passed to `draw_osd_quad`, which writes
its own color uniform; setting that uniform before the call is overwritten.

Release build and `map_views`, `ui_lifetime`, `splash_boot` checks pass. A local
headless TestTrack investigation confirmed an active scene camera and no
remaining loading overlay; it did not validate pixels. Test normal Select Track
entry, Escape/Continue, another track, Valo, and FMV/menu backgrounds in game.

### Menu input debounce

Menu-toggle and cancel hotkey presses share a 250 ms cooldown across dialog
commands: virtual axes 36/35 and physical Escape use the same gate. Select
(axis 34, physical Enter/numpad Enter) has a separate gate. A suppressed
press still updates held state, so it is not delivered later when the timeout
expires; release and a new press are required. Release events and driving axes
are not delayed. The existing command gate for directional UI navigation stays
unchanged. This is a UI-action guard, not a global keyboard or simulation delay.

`run_save_io_tests.ps1 -Suite menu_debounce` exercises the actual hotkey
dispatcher through a compiled Java native handler. Baseline `c6e563f` fails
when MENU is followed immediately by CANCEL; the corrected build passes held
input, cross-command cancellation, physical Escape, release, independent
accept and driving checks. Repressing after 275 ms succeeds. `pad_fallback`
also passes. In-game pause/Continue behavior still needs user validation.

### UI rectangles have one render owner

RenderRef.setMatrix projects script Rectangle instances into the OSD registry.
Once `_rect` and `_osd_wi` identify that handoff, scene collection must not also
submit the unscaled template mesh through a cursor or OSD preview camera. That
second submission produced a large black tooltip panel behind the compact
2D tooltip. Hidden/retired UI rectangles stay excluded while their resource
instance survives; ordinary 3D meshes are unaffected. No texture-ID or
particular-tooltip special case is used.

`run_save_io_tests.ps1 -Suite tooltip_render` uses the real frontend rectangle
mesh beneath a cursor world. Baseline `d988359` fails the one-owner assertion;
the corrected build passes visible/hidden/retired rectangle suppression and
ordinary-mesh preservation. `ui_lifetime` and `map_views` also pass. Requires
read-only local frontend assets. Pixel validation remains an in-game check.

### Catalog vehicle preview versus fullscreen showroom

Catalog's vehicle page uses an inset CarViewport and CarScene, while the
main-menu CarSelect has a fullscreen camera. An inset CarScene stays out of
the primary world selection: the normal preview pass draws it over the page,
clears depth without clearing its background color, and uses its script
look/move pose. Promoting it to the primary world produced a brown rectangle
and applied main-world orbit logic to the preview framing. Fullscreen CarScene
showrooms continue to supply the main world.

The fullscreen MouseCursor camera is UI-only and cannot supply the main 3D
camera, including when a tooltip appears. It remains registered for UI use.
This avoids a cursor preview replacing the selected world/camera on hover.

`run_save_io_tests.ps1 -Suite car_select_view` covers catalog inset selection,
script pose preservation, repeated cursor-hover synchronization, fullscreen
showroom preservation and cursor-only UI. Baseline `a2095e8`: 0 passed /
2 failed; corrected: 2 passed / 0 failed. `map_views`, `tooltip_render` and
`ui_lifetime` also pass. No Java changes or native signature changes.
These are host selection/collection tests; rendered framing and long-hover
visibility still require in-game validation.

Tooltip visibility also changes the set of OSD layers: the script's cursor
rectangle/text can introduce layer 0 below the catalog page's layer 10.
`draw_osd` must select its under-preview page plate from visible, owned,
nonmodal background widgets (`osd_page_background_layer`), not from the
minimum of all widget/text layers. Otherwise the page is moved above the car
when the tooltip appears. Modal plates remain above previews. The additional
`car_select_view` regression models that layer-0 arrival and repeated hiding,
keeps page layer 10 stable, excludes modal plates, and retires a hidden page.
The suite now has three cases; the original 0/2 baseline describes only the
two earlier camera-selection cases.


## Trigger cues and physical bodies (#130)

A RenderRef is visual unless it has explicit collision activation, a physics
shape, or a registered ground/collision mesh. Physics preparation must not
silently turn its display mesh into an obstacle. Trigger volumes themselves
are sensors even when a mesh or physical shape is attached. Ground shells,
CFG collision bodies and explicit PhysicsRef objects retain their physics.
This separates the MWM Test Track marker/frame from the actual challenge
weights; the latter must still hit the car. No production Java is changed.

The frontend finishObject resource uses flags 640 (0x280). The host now
recognizes that combined cue profile on RenderRefs and applies rotation and
vertical bobbing to the collected render matrix, leaving the script pose and
sensor fixed. Untextured SCX3 groups retain their authored diffuse colour;
the cue uses the transparent pass. User comparison with original MWM confirms
both movements. The 1 rad/s turn, 0.5 m bob at 2 rad/s and opacity 0.65 are
host compatibility defaults, not recovered original constants or proven
independent meanings for the two flag bits. The other matching frontend
resource (paintcan) shares this profile. Compare its presentation in game too.
MWM's explicit script animation of the weight/frame remains authoritative.

Research consulted nedobylskiy/SLRR_rev at
4ca11c265e0b9b3dad09a7f23d4a2af7740d626b and EliasFD3S/slrr-reverse at
93588c4c6f72ddcb231e1c9ca46429e7db5991b6. Their rendering/animation code does
not establish those flag bits' original timing or opacity. An offline read of
the installed MWM executable establishes the resource flags parser only;
this is ordinary offline analysis, not REA Evidence.

The trigger_markers suite uses the local read-only frontend/DestructionTools
assets and verifies visibility, lack of marker collision, rotation, bobbing,
opacity, unchanged script pose, and preservation of a physical box. A sensor
with an attached physical box stays traversable. A synthetic native car drives
outside/inside/outside a sphere and delivers exactly one ON/OFF callback with
its chassis live ID. This checks native event dispatch, not the entire MWM
challenge dialog. trigger_volume, garage_shell, map_props and physics_recovery
also pass. In-game marker appearance and challenge activation await validation.
Evidence: assets/validation/trigger-markers/validation.txt.


## Batched OSD text (#82)

Text native signatures, registries, ownership and font decoding are unchanged.
The registry, tooltip-line and HUD-string renderer paths share draw_osd_line.
Each line retains its original layer/view, hover colour, alignment, byte charset,
authored quad corners/UVs, advance and straight-alpha blending. There is no
sorting across text lines or across the background/icon/text stages.

OsdTextRun emits indexed quads with 20-byte position/UV vertices, bounded by
available transient vertices/indices and 16-bit local indices (16,383 glyphs per
chunk). Chunks retain pen position; missing glyphs/spaces advance without drawing.
Reusable scratch vectors avoid per-glyph allocation. Empty or exhausted buffers
do not submit partial quads. vs_osd_text/fs_osd_text bind only atlas stage 0 and
colour; world lightmaps, shadows, fog and sun uniforms are absent. Both shader
profiles (DXBC and GLSL) build; the runtime link check includes the text program.

Icon eligibility/pass is classified once per widget per frame, then traversed
in the same registry order within layer and highlight/icon/knob pass. This removes
repeated lower-casing and layer-by-layer triple classification. Widget/background,
modal and preview ownership rules remain in the existing OSD pipeline.

Validation against c63a8cd: the real Direct3D 11 backend with read-only slii24 font
assets renders 20 lines / 1,000 glyphs in 20 text draws instead of 1,000 (-98%).
Total bgfx stats are 23 versus 1,003, including three constant touched views.
Clearing the text returns to three. The 1280x800 before/after TGA pixel payloads
are byte-identical, including alternating opaque blue and half-alpha gold lines.
This is a controlled renderer scene, not an FPS measurement in a running game.
The existing present/hiz link warnings also occur in the baseline and are outside
this text change; the text program links and renders successfully.

run_save_io_tests.ps1 suites: osd_text_batch (4 layout/bounds/chunk tests),
osd_text_gpu (opt-in Windows/GPU/assets integration), car_select_view,
tooltip_render and ui_lifetime pass. The GPU suite opens its own test window
off-screen and captures it; it does not launch the game or modify assets/saves.
-BaselineRef c63a8cd on osd_text_gpu reports 1,000 draws and intentionally fails
the new 20-draw assertion. Evidence and screenshots:
assets/validation/osd-text-batch. Full menu/catalog/HUD gameplay checks remain
for user validation; no OpenGL runtime or general FPS improvement is claimed.


## Shared road, traffic and race runtime (#179 / #60 / #61 / #62)

`road_routes.cpp` reads the TRC declared by the selected GroundRef resource.
Crossing/lane links establish directed streets; Hermite samples, endpoint
projection and Dijkstra produce routes in script metres. A 64 m segment grid
accelerates projection. Heading breaks ties between opposite road directions
for traffic. There is no fallback to Valo for another map. `race_runtime.cpp`
caches by ground incarnation, resource generation, root and type. A failed query
clears the cached route; destruction clears it. Alignment returns independent
position/direction vectors. Route position/distance queries use metres along
the last route; these contracts are host interpretations. plotRoute creates a
navigator-owned coloured ribbon, without sampling its texture argument.

Traffic streaming checks a safe lane position before constructing the Part/CFG
assembly. Rejected positions allocate no vehicles or Java objects; a population
cap is not permission to construct and discard cars until it is full. Existing
rivals keep their persistent assemblies. Search radii, spacing and retry bounds
remain unchanged. The unavailable-spawn regression checks heap stability without
GC and fails on cc0f0db; `traffic_runtime -BaselineRef` substitutes both the road
and streaming implementations for comparison.

Native vehicle ownership is recorded by the Part factory (native-only or
unscripted car), independently of whether its wheel meshes are inline or mounted
stock parts. Like SLRR_rev's TrafficVehicle template path, these cars use native
wheel springs and tyre drive forces; they must not require Java transmission
ratios. Scripted mechanical vehicles retain their Java drivetrain path.

Traffic registrations retain the canonical map and its incarnation, as in
SLRR_rev `TrafficNative.Registration` (v221 88130b3). Driver route and signal
lookups use that binding for registered cars rather than assuming their parent
has the host `GroundRef` enum. Real native maps may be generic GameTypes. Losing
this binding leaves the driver without a route and the chassis parked; tests
must exercise that real representation for both traffic and individual racers.

`traffic_runtime.cpp` keeps model weights, stable individual IDs and temporary
road/crossing restrictions per ground. Model declarations may be id-only Java
resource references, rather than live instances. Positioned reinsertion is a
finite request, separate from permanent model weights. Density selects up to 100
active cars (20 at the default .2). Registered Java cars each have catalog weight
one; their visible share is rounded from that weight against the summed model
weights. They no longer reserve half the population. For 100 registered rivals
and an ordinary model weight of 1,000, the test produces 2 rivals/18 ordinary at
.2, 1/9 at .1 and none at zero. Those are fixture counts, not fixed production
quotas: actual Java model weights and registered rivals determine the split.
The density-to-cap scale remains a host policy, not recovered native timing.
The full 0..1 range is effective: the old cap at .5 is removed. Spawn candidates
are kept 45..160 m from the player, with candidates in front rejected below 80 m;
ordinary cars recycle outside 220 m. This concentrates the population locally
instead of spreading a small cap over the old 260/380 m spawn/retention ranges.

Four-wheel native cars and native-only racer doubles now use the existing rigid
body, floor collision and suspension solver while visible. The inline CFG wheels
are valid physical corners even without scripted rim slots. Adapted from
SLRR_rev traffic bea8ce0 traffic-vehicle.ts, the native driver supplies load-limited
longitudinal/lateral tyre forces (5 m/s2 throttle, 8 m/s2 braking, friction .9),
with four .25 m springs, .1 m static sag and damping ratio .65. Scripted cars
retain their engine/dyno drivetrain; no fictional Java engine is created for
native-only cars. The host additionally limits braking to avoid sign oscillation
near zero and compensates the forward gravity component while holding brakes.

Inline render meshes use the same hub, compression, steering and wheel rotation
as physical contacts. Previously, inline wheels were disabled by the mounted-rim
check and the visuals stayed at their authored pose while placement used the body
mesh bottom. Traffic placement now uses loaded tyre contacts through park_chassis;
it does not raise the body by a generic support-mesh bounding box. Static native
wheels therefore settle down onto the road as upstream's native cars do. Vehicles
without a supported four-wheel chassis retain the external-motion fallback.
Hidden cars remain excluded from drawing, physics and triggers. Hidden rivals
receive coarse five-second estimates. The shared signal/STOP clock advances by elapsed time with VM pause/time-warp
applied, independently of the bounded physical catch-up. Thus low FPS does not
stretch each signal slot; pausing still freezes it. Mainloop traffic catch-up uses the same
four-tick budget as car physics.

The traffic graph is separate from the street-centre route graph. Following
SLRR_rev traffic bea8ce0 road-traffic.ts, RoadOther segments run in reverse stored
order. Explicit node grey curves connect matching incoming/outgoing endpoints
within .03 m; joins are never invented by nearest street centres. The installed
City data supplies 1,572 lane curves and 3,854 crossing curves. Physical drivers
receive multiple connected curves ahead and steer, accelerate and brake through
the same Part solver as opponents. The external fallback carries arc-length
progress through lane -> crossing -> outgoing lane. The external/coarse path chooses a destination branch before one of its lanes;
the physical brain currently rebuilds lookahead using the first connected exit.
Native traffic uses an urban cruise target; rural speed groups, physical forward
peer following and the complete upstream traffic controller are not ported.

GameRef.create_native on a native car builds its CFG-backed native assembly
and aliases the Java handle to it, without running a chassis-specific constructor.
This covers Valocity's fake_racers doubles created at (0,-10000,0), followed by
addTrafficCar(dummycar,null). Registration owns insertion/visibility; removing it
preserves the Java-owned car. Explicit insertion positions are projected onto
actual lanes and published on the road before becoming visible. There are no
Valocity-specific VM exceptions.

Traffic projection uses a separate immutable 64m spatial grid for lane and grey
crossing segments. Query rings expand until a conservative distance/heading bound
proves that no unvisited segment can beat the result. Equal scores preserve the
original edge/segment ordering. Far-away or incompletely indexed/extreme geometry
falls back to exhaustive projection; non-finite query inputs fail safely.
Construction uses jobs_parallel_for from #58: each worker reads one immutable
edge and fills its own fragment; the host merges/publishes after the join. The
VM, GameRefs, physical integration and scene publication stay on the host thread.
Read-only queries are safe concurrently once the graph is published.

This removes a complete-map search from each physical driver's repeated halt
and route queries. On the local City fixture, 1,000 identical queries took
1548ms at f42f1b6 and 5.3ms with the grid, with the same checksum. A 3,600-tick
native-car integration took 5793ms before and 94-96ms after. A 20-car fleet took
35ms for 60 physical updates. These are CPU fixture timings, not a promised full
game framerate. Tests compare 150 indexed projections against an exhaustive
oracle, including crossing/lane filters, non-unit headings, high/far points and
cell boundaries, plus serial/parallel results. Reproduce with road_routes and
its -BaselineRef f42f1b6 option; traffic_runtime also supports that baseline.

Notifications emit balanced car_add <live-id> <registration-id> / car_rem
<live-id> strings to original Java after placement. Java owns opponent lists,
markers, challenges, countdowns, results and transitions. Removing a traffic
registration preserves a Java-owned rival; native-owned cars are destroyed.

Signal fixtures come from actual ICFG trlight placements, including exterior
maps. The shared crossing/approach binding follows SLRR_rev traffic branch
bea8ce06 and its traffic-control.ts: nearest crossing, then nearest flagged
incoming endpoint, grouped by its owning street. Heads and traffic consume the
same 10 s green / 3 s yellow branch cycle. Red/yellow prevent new entry; green
passage latches clearance until the next road. These timings and the 2 m offset before the incoming lane end are host policy,
not recovered original timing/stop lines.
Signal lens emission is grouped into one cached mesh per ground shell (at most
three colour groups), rebuilt on phase/registration changes. No separate draw
per signal head is added. A regulated approach without a matching lamp is a distinct timed STOP state,
not a permanent red. Registered drivers stop there for five seconds, then latch
clearance. Physical projections include the grey crossing curves, preventing a
car already crossing from being rebound to another incoming red approach.
The native flare billboard/halo, explicit STOP/YIELD sign discovery and complete
original junction producer remain outside this port.

`race_brain.cpp` interprets controllable/AI commands and supplies throttle,
brake and steering to configure_drive in the existing Part solver. Player
Controller commands keep their owner. BeginRace/stop/suspend hold; race/GoToTarget
plan a route; GoToTraffic follows roads and restrictions; follow periodically
replans toward its target. The latest v221 NightRace finish policy preserves
the destination and coasts across the finish plane when the radius-based Java
stop event arrives early, then applies brakes.

Traffic refreshes its route every two seconds, including projections onto
crossing curves. Adapted from SLRR_rev traffic bot.ts, a driver requesting throttle
without progress for two seconds attempts up to 3.5 seconds of reverse recovery,
only while the rear corridor is clear, then replans. A red light/timed STOP resets
the blocked timer and never triggers that manoeuvre. AI_stop cancels recovery.
Reverse requests are supported by both the native force driver and the scripted
mechanical gearbox. These recovery thresholds remain host policies from upstream.

MWM ROC uses per-car ROC_cars/*.spl, not just maps/ROC_Track/race.spl. The loader
samples the complete curve, projects the actual car position onto a segment,
chooses direction from the car's heading, and rotates a closed loop at that
point. Selecting a nearest key and following file order was sending the ROC
opponent the wrong way from the scripted grid. Closed loops must advance before
the shared start/finish can count as finished. Steering includes recovery from
a target behind the car and braking preview before curves. Resource cue flags
640 never rotate/bob a vehicle mesh or an installed Part render; they continue
to animate standalone trigger indicators.

Spline races no longer inherit the generic 108 km/h curvature-floor ceiling on
straight sections: their ceiling is 55 m/s times AI level, with curvature-limited
corner speed and braking preview out to 192 m. This is host tuning; AI_params
and per-key spline speed metadata are not yet interpreted. The unchanged ordered
ROC gate test now finishes in 96/104 seconds with the same 1.8 m maximum sampled
deviation, rather than the previous 122/126 seconds.

Limitations: behaviour flags are stored without tuning; trailing model
parameters, spline width/speed metadata, AI_params and opponent IDs do not tune
the controller. Follow offsets are world-space. Complete pursuit, traffic lane changes,
the full traffic impact/recovery policy and opponent driver seating
remain incomplete. Native/mechanical chassis now share 120 Hz contact substeps
under the unchanged 60 Hz host/control tick. Each active-world OBB pair receives
one finite-mass response, with a bounded translational sweep and all corrected
body/wheel frames published together. Hidden ancestor chains, other worlds and
stale incarnations are excluded. Parked bodies can wake on contact while Java
retains car/driver ownership. Promoted props and free bodies query all active
physical cars; pair damage carries actual impulse and pre-hit motion rather than
reusing the wall-relative speed assumption. See [the #197 audit](TRAFFIC_RACE_AUDIT.md)
for limits. AI tuning, population and streaming radii are host models, not
original-engine parity. Native bindings are not race completion.

Validation uses the Release host objects and read-only local assets. road_routes
covers directed graphs, native return formats, navigation ribbons and shared
signal phases (1,146 heads across the installed City/exterior data). traffic_runtime
covers real Taxi assemblies, native day-racer staging and physical ticks,
Wagon/Teg rendered tyre-to-road contact, weighted density scaling,
id-only declarations, balanced lifecycle events,
red-light stop/pause/green release, and cached signal emission meshes. The
trigger_markers test retains marker animation and excludes vehicle animation.
The race_vehicle integration executes original Bot methods on real City and ROC
collision terrain with stock Baiern/Enula assemblies. From the scripted ROC grid,
the per-car spline passes all three Round checkpoints in order and reaches its
finish gate in 96–104 simulated seconds; maximum sampled deviation is 1.8 m.
This is an automated physics integration, not a running career UI recording.
Day challenge/finish dialogs, the full night meeting and result/garage transitions
still need in-game validation. Evidence: assets/validation/shared-races.txt.
