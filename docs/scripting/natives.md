# Native audit and roadmap

Baseline snapshot from 9 November 2026, incrementally updated 18 November 2026.
The three `Painter` natives are registered (#64). `Camera.setFog` is registered. The remaining `PhysicsRef` signatures are
bound, and `GameRef.setActiveCollision` is no longer an explicit no-op.
Earlier increments covered the resource and vehicle bindings, the Language
work described in [scripting/compiler.md](compiler.md), and the Leftovers classification.
The Release x64 registry was compared with the
376 entries in the stock table `stubs.txt` (under `docs/` in the research workspace).
The same table is pinned in `assets/progress/stock-natives.json`, and
`python scripts/render_native_coverage.py` recomputes the counts below and the
README chart from the registrations in `src/`.
Matching uses owner, method and the full descriptor, including the return
type derived from the pinned typed signature. Only an omitted void return is
accepted as a runtime alias. Slash-form descriptors and registered parameter
matches with no expected return descriptor fail the audit. This
measures bindings. Behavioral status below comes from reviewing the C++
implementations and following selected calls to their consumers.

| Status | Stock signatures |
| --- | ---: |
| Registered with a body | 370 |
| Registered with the explicit `native_noop` handler | 3 |
| Absent from the registry | 3 |
| **Total** | **376** |

The first category includes constant responses, inline empty bodies and
partial implementations. The mock table calls out known behavioral gaps in
that category. It is not a compatibility score or a complete runtime trace.

## Explicit no-ops

| Owner | Native signatures | Missing effect |
| --- | --- | --- |
| `System` | `analyze(Object)`, `analyze(float)`, `analyze(int)` | The original discards the arguments. |

## Known mocks and partial implementations

| Native or group | Current behavior |
| --- | --- |
| `FindFile` and File path operations | Share canonical physical confinement (#84). Enumeration alone can address the root. Leading separators are game-relative; external junctions are rejected and internal junctions allowed. Masks and dotted names remain supported. See [saves/notes.md](../saves/notes.md#filesave-io-fixes--issues-68-70-and-71). |
| `GameRef.setState` | Sets pose and linear/angular velocities. Cars receive the state in the live solver. Generic GameRefs retain the state but have no general dynamic-body solver yet. |
| `GameRef.setActiveCollision` | Queues an idempotent activation request. The next physics preparation enables the generic object's CFG-defined collision and clears the queued flag. Existing vehicle and legacy RenderRef collision remain independent. |
| `PhysicsRef` | `create`, `createBox`, `createSphere`, `setMatrix`, `setStatic`, `getPos`, `getOri`, `getVel` (both overloads) and `getAngVel` are bound. World boxes, spheres and resource CFG bodies join scene collision; OSD click boxes remain separate. `getVel(Vector3)` returns point velocity. `setStatic(0)` hands the body to the host free-body solver (gravity, scene and car contacts; a primitive gets a 500 kg/m³ density); `setStatic(1)` fixes it again. Free bodies do not collide with each other. |
| `GameRef.setMatrix`, `WheelRef` setters | Non-finite pose/scalar/vector inputs are rejected before mutation. Per-body solver failures suspend a chassis, remove a failed loose body, or disable a free PhysicsRef; valid simulation continues for other bodies. Existing signatures/counts are unchanged (#75). |
| `GameRef.getDetail`, `RenderRef.getDetail` | Read the canonical instance's detail bias; the default is zero. No distance-dependent LOD bias producer is implemented yet. |
| `Vehicle.getHorn` | Checks the selected horn SFX row on the canonical chassis, using live player horn input or the `sethorn` command. |
| `Vehicle.hasCrime` | Returns a stored crime_speed limit in m/s, else the host speed zone (SLRR_rev road-speed-zones.ts): 35 km/h near an urban lane, 100 km/h near a rural one (nearest traffic curve within 30 m), -1 off the roads. The original zone producers are not recovered. |
| `System.isLoading`, `isLoadingReset`, `setLdPriority` | Track active RPK scans and measured pack-work history; reset seeds the 32-sample ring without clearing ongoing scans. Priority changes scan cadence. Mesh/texture streaming has no corresponding queue. |
| `System.compileAll` | Embedded C++ compiler writes validated TUFA containers for source units, including multiple classes per file. Existing loaded classes require a restart. See scripting/compiler.md. |
| `System.info` | Selector 5 returns the live host VM heap count. Other selectors return an empty string. |
| `ResourceRef.makeTexture` | Resolves the filename and gives the file-backed texture a live native ID and type 7. Decal passes that ID to Painter; OSD/material replacement resolves its path too. `duplicate` gives a file texture an independent handle, and aliases retain its type. Saving embeds its pixels as a type-7 PNG blob. |
| `ResourceRef.duplicate`, `RenderRef.create` | Copy both RPK and file-backed texture replacements by value. Later skin selections do not mutate previously created render instances or shared geometry. A duplicated `RenderRef` type gets a live id, so creating a render from `copy.id()` keeps its replacements (Painter's decal slots). |
| `RenderRef.changeResource` | Supports RPK texture replacements and file paths from `makeTexture`, matching the original texture resource to the mesh material slot. A new selection replaces the previous file override for that source; an RPK replacement clears it. |
| `Viewport.activate`, `deactivate`, `destroy`; camera selection | Active cameras with a viewport are eligible only while that viewport exists and is active. Hiding an OSD stops its preview without requiring scripts to destroy all scene objects. Reactivation restores eligibility; null-viewport camera behavior is unchanged. With a selected scene, camera ownership follows world ancestry rather than viewport size: a full-screen Navigator local world stays separate from the resized driving view. Renderer passes respect that rectangle and viewport priority (#65); visual validation is pending. |
| `Animation` | The constructor is registered with the dotted TUFA descriptor; the earlier slash form never matched, so script `Animation`s stayed unbound. It binds a `.matrixdata` clip to the render named by the constructor. `play` / `loopPlay` / `pause` / `seek` / `getPos` / `setSpeed` sample it on the skeleton declared by that render. `setFade` blends the clip in from the bind pose. Each render keeps its own pose. |
| `RenderRef.getBoneId` | Case-insensitive lookup in that skeleton. An unknown name, or a render with no skeleton, returns 0. |
| `RenderRef.setMatrix(int, ResourceRef, Vector3, Ypr)` | Writes that bone's local pose. With another render as `bone_ref`, this render is parented on that bone. Script positions are metres. |
| `Thread.setDaemon` | Sets and clears the per-thread flag. Shutdown still cancels every fiber. |
| `System.simTime`, `timeWarp` | The simulation clock follows the warp factor, and setting it returns the previous factor. Physics uses the scaled elapsed time. UI clocks, sleeps and host timer dispatch use real time. |
| `System.setMeasure`, `getConfigOptions` | The measure setting controls digital vehicle instruments. The options refresh reads gameplay settings and schedules a debounced save when they change. Failed saves suspend automatic retries until another change; forced shutdown saves can retry. Enhanced graphics and the other host-only options are stored separately in save/game/openslrr_options.json. |
| `RenderRef.setFlare`, `Navigator.updateNavigator`, `GroundRef.getNearestCross` | Directional flares use the scene's texture and an approximate sprite layout. The GPS camera and markers follow scene objects. The crossing query now reads the selected ground TRC and honours the desired crossing distance (v221 host semantics). |
| `GfxEngine.numDisplayModes`, `currDisplayMode`, `displayModeName`, `changeVideoMode` | Enumerate available Win32 32-bit display modes with an 800×600 fallback, identify the current resolution, and apply validated mode changes through the host options path. |
| `Camera.setFog` | Stores the color and the near and far distances on the driving camera. The fade uses those distances. The color argument is replaced by the day and twilight sky color before the shader reads it. |
| `GroundRef.setFog` | Stores a global color and near and far distances. Nothing in the renderer reads them. |
| `RenderRef.plotRoute` | Builds a navigator-owned coloured ribbon from the last queried route. Invalid/missing routes return zero. Texture input is not yet sampled. |
| `GroundRef` road/route natives | Selected-ground directed TRC routes, independent alignment position/direction vectors, cached length/metre position/projection queries, timed crossing halts and path exclusions. Host route-query semantics beyond the v221 port are not recovered original implementations. |
| `GroundRef` traffic natives | Explicit TRC lane/crossing-curve traversal, physical native wheel/suspension/tyre forces with matching render hubs, native-only racer placement from off-map staging, model-weight-based rival share and density scaling, finite positioned reinsertion, stable individual IDs, balanced deferred Java notifications and teardown. setTrafficCarBehaviour stores its flag only; trailing model parameters remain uninterpreted. Signal fixtures share a host crossing cycle with cached lens emission and red/yellow entry restrictions, five-second missing-lamp STOP, eased stops before the stop point, physical lead-car following along the planned path, branch-first exit choice with a kept plan, spawning outside the camera's view (with a budgeted building-occlusion ray) and recycling outside its cone, and rear-checked blocked-driver recovery. remTrafficCar keeps a streamed car so a woken police scout can chase with AI_follow, also off the road; a Bot returning with AI_GoToTraffic is registered again with its old ID and told ai_info_entertraffic. Car-to-car impacts reach scripts as EVENT_COLLISION. Released cars retain static-world response even without a driver/engine. Shared120Hz chassis OBB pairs exchange finite-mass impulses; movable props/free bodies query all active physical cars. Pair damage/detachment uses actual impulse and pre-hit motion, with bounded translational rather than exact rotational CCD. Traffic also changes lanes, cruises in rural/urban speed bands, reacts after queues, settles after impacts and pushes on after a minute stuck (bot.ts ports). Original signal timing, explicit STOP/YIELD signs and upstream's oriented-hull following remain incomplete. See [the #197 audit](../world/traffic-races.md). |
| Brain AI commands (not separate native signatures) | Shared physical driver for AI_race, GoToTarget, GoToTraffic, follow and MWM ROC splines. BeginRace/stop/suspend hold; NightRace retains the race destination. Host tuning, basic world-space follow offsets; renderinstance seating and spline metadata remain incomplete. Original Bot bytecode, real City/ROC terrain, ordered ROC gates and the finish gate pass physics integration tests; full career UI/result flows remain pending gameplay validation. |
| `RenderRef.lineCreate`, `lineAdd` | Build renderable ribbons from positions, normals and widths. Color is applied per segment. |
| `LightSource` family (OpenSLRR, #235) | New `java.render` classes, not stock natives. Point, spot and rect lights attached to a GameRef; consumed by the garage renderer only. Rect lights render as spots, inner cone and per-spot shadows are not honoured. See [rendering/contract.md "Script light sources"](../rendering/contract.md#script-light-sources-235). |
| `Chassis.setCooling`, `setSteerWheelRadius`, `setSteerWheel`, `setBuck` | Store configuration. No temperature simulation or bucking effect consumes these values yet. The installed wheel on slot type 11 turns about the cfg `steering` locator. |
| `Chassis.setNitroSFX` | Configures the press-triggered nitrous one-shot. Mapped driving input selects the wet dyno bank and depletes a native aggregate; explicit `reload` fills it. The HUD uses remaining/capacity. Stock flag-256 exhaust meshes and central fake-light glow share simulation-clock flicker; original flare sizing/rays/ghosts remain approximate or absent. See [vehicles/driving-model.md "Nitrous driving and exhaust"](../vehicles/driving-model.md#nitrous-driving-and-exhaust-303-304). |
| `GroundRef.setPedestrianDensityN`, `addPedestrianType`, `remPedestrianType`, `removePedestrianType`, `pedestrianDistance` | Density, after the script's Config factor, sets people per square metre of `city.walk` row area around the driven car. Each type's `native ped` cfg supplies the render and the walk clip. Walkers follow the original engine model: free agents inside a row, pushed back by its walls and both sidewalk runs, leaving only through gates (see `src/resources/pedestrian_lines.cpp`). `removePedestrianType` is the original native table's name for `remPedestrianType`. `pedestrianDistance(pos, type)` returns the distance to the nearest live walker of that type id, or of any type for 0, and -1 when there is none. |
| `PhysicsRef.getPos` | Returns a new Vector3 copying the one stored by `setMatrix`. Without one, it returns a new Vector3 from the object's pose. The copy matters: `Osd` keyboard navigation calls `gad.getHotSpot().sub(cur)`, and returning the stored vector let the arrow keys move the button's own Rectangle (`Rectangle.pos` is the same vector the button passed to `setMatrix`). |
| `Part.setSfxLoopParams` | Stores the two parameters without applying them to a voice. Native car CFG sfx_loop is kept in `PartConfig::sfx_loop` and played by `update_traffic_engine_audio`; mechanical AI already updates its SfxTables. Script engine loops have no listener-distance input; native traffic loops use distance gain and stereo pan. |
| `Chassis.getMass`, `Part.getMass` | `Chassis.getMass` returns the mass of the assembled car, the same sum over the installed tree the physics body is built from, as the executable does (0x0043CAA0 reads the vehicle body's mass). `CarInfo` (mass, kg/HP, kg/Nm), the Garage, the catalog delivery price and MultiPlayer read it. `Part.getMass`, including on a chassis called as a `Part`, stays the part's own CFG mass. See [notes.md "Chassis.getMass is the assembled car (#413)"](notes.md#chassisgetmass-is-the-assembled-car-413). |
| `Chassis.setHornSFX` | Stores all indexed sounds. The current player input plays the index 1 sample once per press. |
| `GameRef.create_native` on native cars | Builds the CFG-backed native assembly, skips a chassis-specific Java constructor and binds the Java reference to its live chassis. Tested with the real fake_racers staging/traffic flow; other native resource kinds retain their existing path. |
| `Painter.paintPart`, `doPaint`, `xPaint` | Pick the installed part under the pointer and draw into that part's own runtime texture (`paint_texture.hpp`), shown in its cfg `texture` paint slot. `paintPart` fills it with one colour; `doPaint` projects the spray brush or a decal as a square `size` metres wide onto the paint-slot triangles that face it; a temporary (`temp`) decal preview is replaced by the next one and removed by `xPaint`, by a committed stroke or two host frames after the script stops refreshing it. The executable queues `paint`/`bigdecal` part commands instead; the projection, orientation and preview expiry are host approximations. `File.write(ResourceRef)` saves such a texture as a PNG in its save blob (SDAT trailer or JSON `blobs`); `readResID` restores it, or reads the JPEG the stock game wrote beside the save as `<file>.<ordinal>`. See [ui/notes.md, "Garage paint booth (#64)"](../ui/notes.md#garage-paint-booth-64). |
| `GameRef.getInfo`, `queueEvent` | Implement selected queries and commands. `repair` performs the native per-part deformation, damage-associated slot/wheel and CFG mount/base-mesh reset; stock Java owns the installed-tree recursion and wear/tear caps, with a native fallback only for detached assembly children omitted from `attachedParts`. Unhandled queries return zero. Other command paths still need coverage as new sections are exercised. |
| `GameType.registerCallback`, notifications | Control, drive and animate callbacks run. Notifications have event routing and GC roots. Other callback modes and event producers still need verification. |
| `Input.getAxis`, `activeAxis`, `Controller.user_SetAxisSmooth` | Win32 keyboard/mouse and simultaneous SDL3 peripherals are wired. The primary pad retains the legacy profile alias; other peripherals expose signed axes, buttons and hats. Rebinding raw axes is relative to their initial position; reopen controls after adding a device. Hardware rig calibration is not validated; host force feedback (#395) is not checked on hardware. Pointer movement is excluded from rebinding. Negative mouse axis requests reset deltas, while their sensitivity argument remains unused. Default pad driving controls are retained per axis unless explicitly overridden or their physical channel is assigned elsewhere. Steering fallback and analog classification share that policy. Smoothing uses a host time-based approximation. Menu/cancel hotkey presses share a 250 ms UI debounce across command changes; select has a separate gate. Releases and driving axes remain immediate. |
| `Controller.user_SetAxisSpeed`, `user_SetAxisForce` | Scale software smoothing and override the logical axis value. The force coefficient and hardware feedback are unimplemented. |
| `Sound.has3DHardware`, `hasMixHardware`, `enableDebugDump` | Fixed capability responses. Zero reports the lack of legacy hardware mixing support and keeps the legacy debug overlay disabled. |
| `System.runFinalization`, `WheelRef.finalize` | Compatibility bodies. VM finalizers run during collection. Wheel references borrow their state from the chassis. |

`GfxEngine.flush`, `forceRendering`, `printScreen`, engine SFX tables and
the cooperative thread scheduler have working paths in the host. Their
presence in the registry does not imply that every use in a race is covered.

## Leftovers

This category is excluded from the active implementation priorities but stays
in the 376-signature total. It describes current usefulness, not a proven
historical origin.

| Owner | Signatures | Binding status | Reason |
| --- | --- | --- | --- |
| `System` | `netHost`, `netJoin`, `netLeave` | 3 compatibility bodies returning zero | Retained networking API; no networking implementation planned here. |
| `Welder` (listed under RenderRef in the research native table) | `addPoints`, `weld`, `progress` | 3 missing | Specialized welding API declared by `Welder extends RenderRef`; no callers found in the local stock Java source trees. A Monster Garage origin is plausible but unverified. The slrr-reverse implementation explicitly describes these as soft stand-ins, not recovered PE behavior. |

The chart moves the network bodies out of Language and the three Welder
signatures out of Resources. Missing leftovers remain missing, rather than
being replaced by success-returning mocks.

## Missing signatures by system (active priorities)

None: every active stock signature has a binding.

The `GroundRef` water natives are bound since #454 too (`water.hpp`): the
surface plane and its limits feed the wheels' water spray; no buoyancy or
drag reads them yet. Only the three Welder leftovers remain missing.
The nine `ParticleSystem` natives are bound since #454
(`src/natives/natives_particles.cpp`, `src/resources/script_particles.cpp`):
`init` resolves the RenderRef type to a stock `particle <cfg>` cloud, the
source actions emit into the tyre effects' particle pool in the parent's
frame, `getCounter` counts the cloud's particles in the counter's sphere, and
a source's `bone` follows that bone of the parent's skeleton.
`modePermanent` follows the exe's flag 0x20000000: a system that is not
permanent goes idle after an update with no source emitting, drops its
sources, and wakes on a positive `setFreq`; see
[vehicles/notes.md](../vehicles/notes.md#tyre-smoke-ground-dust-and-textured-skid-marks-454).
A parent outside the active world (the commented-out `Osd` cursor effects,
the only stock callers) emits nothing.
No PhysicsRef signature remains absent from the stock table.

## Roadmap

1. **Finish a playable drive on a test track.** Validate steering, brake,
   clutch, shifts and the input profile through a relaunch. Connect the remaining
   driver actions to gameplay; compare the implemented nitrous chain against
   original gameplay beyond its targeted regressions.
   Compare acceleration, stopping and suspension against stock telemetry at
   different render rates.
2. **Unify world collision and damage.** Native CFG placements and
   `PhysicsRef` primitives now share scene collision, and
   `GameRef.setActiveCollision` activates generic object collision at the
   physics boundary. Continue with standalone dynamic bodies and validate
   track barriers and prop contact in game. Connect impact deformation and
   wear to the parts and the regional HUD display.
3. **Validate the navigation and race loop.** Shared road queries, route
   ribbons, traffic lifecycle and physical bot inputs are now implemented
   for #179. Verify daytime challenges, the night meeting, ROC spline finish,
   timing/results and repeat entry in game. Complete the documented traffic
   behaviour, pursuit and contact limitations. Pedestrians already walk the `city.walk` sidewalk rows near the player; they do not use the road graph.
4. **Complete garage actions and effects.** The `Painter` natives are bound
   (#64); continue with resource-created sound, then animation, particles,
   flares and fog. Verify paint and installed-part state through a career
   save and reload.
5. **Complete platform and optional systems.** Enumerate display modes,
   complete controller options and loader state, then assess networking and
   additional debug behavior when needed by the supported workflows.

The stock driving, suspension and game-control pages were exercised with
synthetic keyboard input in the Release x64 host. Each test applied a new
binding, read the logical axis and saved and reloaded an isolated profile.
Physical gamepad input, driving on track and the race-only handlers still
need in-game validation.


## Verification on 17 November 2026

`python scripts/render_native_coverage.py` and its `--check` mode confirm the
stock-manifest counts above and the generated JSON/SVG/README outputs. These
counts describe registration coverage, not full compatibility. The #115
check now preserves exact return types and dotted object names, permits only
the omitted-void alias, and keeps exact registrations ahead of aliases.
Intentional additional return overloads remain allowed when the expected
descriptor is also registered. Run `python -m unittest discover -s tests -p
test_native_coverage.py` for the nine descriptor/scanner/CLI regressions.

The driver fixes introduce no new native signature and no production Java
change. `driver_mask` exercises cache invalidation, visibility restoration and
large scene traversal. `driver_selector` follows the registered
makeTexture/changeResource/duplicate/create chain into a collected humans.rpk
material slot and checks viewport deactivate/reactivate/destroy. Its synthetic
paths do not validate image decoding. The user additionally validated seated
driver behavior, skin changes and return to the menu in game for PR #155
(#112 and #156). Evidence logs live in assets/validation/driver-mask and
assets/validation/driver-selector; see [vehicles/notes.md "Driver duplicate visibility (#112)"](../vehicles/notes.md#driver-duplicate-visibility-112)
and [ui/notes.md "File texture replacement and hidden preview viewports"](../ui/notes.md#file-texture-replacement-and-hidden-preview-viewports) for implementation
entry points and remaining limits.


Issue #130 changes scene collision/render collection, not native registrations.
RenderRefs no longer acquire collision merely from their visible mesh; trigger
sensors are excluded, while explicit physics and ground collision survive.
The trigger_markers suite exercises native-car ON/OFF notification delivery
and the visual/physics separation with MWM resources. See scripting/runtime.md,
"Trigger cues and physical bodies", for reconstructed cue settings and the
remaining in-game validation. Registration counts do not imply that the full
MWM challenge is validated.


Issue #82 batches the renderer output of existing Text/OSD natives. It changes
no registration or Java body. Existing glyph/font/colour/layer semantics are
retained; controlled D3D11 render evidence and limits are documented under
"Batched OSD text" in scripting/runtime.md.

## Exact native coverage descriptors (#115)

`scripts/render_native_coverage.py:host_registrations` retains full descriptors
instead of normalizing slashes and dropping returns. Source diagnostics reject
slash-form and malformed descriptors with file/line context. Stock return types
come from the pinned typed_signature via stock_runtime_descriptor; the pinned
manifest itself is unchanged. classify_rows tries the exact expected descriptor
first, then only an omitted void return. If the same owner/method/parameters
exist with no matching return, the audit fails and lists expected/registered
descriptors. Additional return overloads are allowed when an expected signature
also exists. Bodies and native_noop remain distinct per exact registration.

Validation: nine standard-library Python tests cover primitive/object/array
returns, omitted V, exact priority, overloads, missing bindings, slash and syntax
errors, source diagnostic locations and CLI --check failure before output
writes. The full source audit and regenerated-output --check pass; counts were
then 339 bodies / 3 explicit no-ops / 34 missing (376 total; 358 / 3 / 15 since
#180, see the table at the top). Generated coverage JSON
now states the exact-descriptor scope; README and scripting/natives.md agree. No
runtime source, executable, stock manifest or production Java changed, so no
new game compilation or in-game validation is needed for this audit-only fix.

## Rendering, sound and cursor native utilities

Implemented against the research workspace's `stubs.txt`, the original script callers, and the
contracts documented in `repos/slrr-reverse/engine/Runtime/Render/RenderNatives.cpp`,
`Audio/Sound.cpp` and `Cars/Cars.cpp`.

| Native | Host behavior |
| --- | --- |
| `GfxEngine.forceRendering`, `flush` | Seven presentations, guarded against recursion. The loader is synchronous; bgfx frames retire deferred GPU resource releases. No Java callbacks or VM collection are entered by this presentation hook. |
| `GfxEngine.printScreen(String)` | Queues a framebuffer readback through bgfx's built-in TGA writer. Relative paths use the game root. Parent directories are created. The script's `printScreenIndexed` uses `.tga`, which is preserved without doubling the suffix. Requires the bgfx renderer. |
| `Text.setDefaultColor`, `getDefaultColor` | Complete 32-bit ARGB, initially `0xffffffff`. Applied on text creation; existing texts retain their colors. |
| `MouseCursor.getPos` | New Vector3 containing normalized screen x/y and zero z. A missing inner cursor returns null. |
| `MouseCursor.getPickedPos` | New Vector3 containing the world-space scene hit, using the same camera/ray convention as mechanic picking. Keeps the last hit on a miss, initially zero. |
| `Sound.changeMusicSet` | Selects Garage_Shop, Roam_ride, Race_chase or Main_menu under the game's music directory. Invalid set/NONE stops music. Re-selecting the current set does not restart it. |
| `Sound.nextTrack`, `prevTrack` | Wrap around the active playlist and actually switch the streamed track. |
| `Sound.getMode`, `setMode` | Per-playlist mode state (initially 3). Accept 0..3, preserve the old mode for invalid values. No active set: get returns -1, set returns its input, matching the reference native. Specific shuffle/repeat policies for these mode values have not been established; the host currently advances sequentially at EOF. |
| `Sound.has3DHardware`, `hasMixHardware` | Zero: the software mixer does not provide the legacy DirectSound hardware-buffer capabilities. This does not mean sound is unavailable. |

Effects, music and stock Cinepak FMV PCM each have their own SDL3_mixer
mixer, so their volumes are independent. Music streams from the file rather
than loading complete songs, retaining the MP3/WAV playlist scan, sequential
order and bounded bad-track recovery (WMA is not decoded; the stock game
ships none). SDL mixes on its own audio thread, including during modal script
pumps; the music pump only advances the playlist at EOF. Startup logs list
scan counts and track starts; SDL_mixer failures include the operation and
SDL's error text. The Cinepak audio position drives video timing, falling
back continuously to wall time on EOF or a stopped track. Other VLC/Windows
codec fallbacks retain their own audio outputs.

`SfxRef.nplay(pos, radius, pitch, volume, flags, instance)` places the sound
when `pos` is a Vector3 (#390): the resource's `mindist`/`maxdist` give the
distance model (SLRR_rev's defaults 2.5 m and 60 m when absent) and a positive
`radius` caps the audible range. A null `pos` plays an unplaced (menu) sound.
The listener is the world camera; see ENGINE_CONTRACT §14. The stock
OptionsDialog `MIXING` choice (DirectSound software/hardware 2D/3D) is
replaced by 3D POSITIONING, DOPPLER and REVERB toggles (host options 22-24)
in both OptionsDialog and the OSD v2 Options screen.

### Audio options

The VM now keeps `Input.createHotkey` on Input rather than rewriting it to
`Osd.createHotkey`. The existing Java Osd overloads can therefore activate
hotkeys without the missing-method error. This does not add a general keyboard
hotkey dispatcher to the host.

Slider hit boxes (knob or track Button.parent) now route through the Slider's
range/tick-aware `setValue`. The host copies the result to `osd.sliderValue`
before dispatching its command. Async GameType handlers receive an Event with
F_GAD_CHANGE and the Slider as gadget, through handleMessage(Event); synchronous
dialog handlers retain osdCommand. This keeps tuning controls separate from
the dialog OK/Cancel buttons. Reference casts in TREE no longer turn subsequent
instance assignments into static writes. Mouse capture keeps dragging active outside
the track; release, cancellation and hidden controls are handled.

The engine-owned Config instance is loaded before Init and retained as a VM
static. Audio edits queue a save after 500 ms of inactivity and flush on normal
shutdown. Persistence uses the original Java `Config.saveConfig/loadConfig`
and `save/game/options` (written as JSON, legacy SDAT still read), including all three channel volumes and
the other Config fields. No original Java files are patched. The engine volume
value is saved/restored; engine-loop audio is driven by
`update_engine_audio` (`src/resources/part_runtime.cpp`).

A temporary headless diagnostic exercised the actual OptionsDialog sliders,
verified 20/40/60% through save/reload in an isolated directory, and started all
four music sets with volume zero. Audible playback and visual mouse interaction
remain user checks; no game window was launched.

`SLRR_rev` remains a resource/script reference; no directly portable audio
device or screenshot implementation was taken from it.

### Repeated effects and UI bursts

Transient effects are grouped by normalized WAV path. Requests closer
than 35 ms are coalesced before reading/allocating another voice. At most two
copies of one transient cue may overlap; their gains are divided by the copy
count so correlated waveforms cannot multiply that cue's peak amplitude.
Coalesced requests return the already playing voice handle.

Menu hover/select/back share a three-voice budget and may repeat rapidly
without the transient 35 ms coalescing window. Select/back remove
lingering hover voices, and hover cannot obscure an active select/back sound.
Looping sounds are excluded from grouping. The existing overall 32-voice
budget evicts a transient first; it no longer interrupts a loop to admit a
new voice when all slots are occupied by loops.

`host_sfx_stats()` exposes the active voice and coalesced-request counts.
A temporary muted XAudio2 diagnostic confirmed 1,000 hover requests -> one
voice, 1,000 wrench requests -> one voice, two spaced wrench requests -> two
voices, click priority, and preservation of independent looping voices.

A temporary fuel-rail tuning diagnostic verified that dragging updates the
mixture ratio to 17:1 while keeping the dialog shown and its result untouched.
The audio-slider/save-reload diagnostic also passes after this correction.

The tuning pointer regression also traverses `osd_hit_index` before changing
values: slider track Buttons are `nofocus` because their Slider parent owns
focus, not because they should swallow mouse input. Their hit volumes now
remain clickable. An active modal OSD also prevents unmatched mouse clicks
from becoming mechanic/world clicks. A headless wide-dialog display/wait
probe drags the slider, remains waiting, and closes only on an explicit OK.

### Map ambient sounds

Map placements (`gametype ... params`) can point at object CFGs with ambient
sound lines. In the stock Valo City (`objects.rpk`) these are the
`ambience_placeholders` (city-centre traffic from 8 to 18 h, the island sea
loop), the `fa*` trees (birds, 4-21 h), the `kuka_*` bins (flies) and the
`krthaz*` houses (dogs in the morning, grasshoppers at night): about 13,700
emitters across `city` and `city2`. `scene_ambience` registers them when
`spawn_ground_shell` creates a ground shell and updates them once per frame
with simulated seconds. The rules below come from `StreetLegal_Redline.exe`
(SLRR 2.2.1):

| Rule | Original |
| --- | --- |
| `sfx_loop <sound> <radius> [from_hour=0] [to_hour=24]`. One loop per object; a later line replaces an earlier one, so the city centre plays only its second traffic loop. | parser 0x465340 |
| `sfx_random <sound> <radius> [min_s=1] [max_s=1] [from_hour=0] [to_hour=24]`, at most 16 per object. | parser 0x465410 |
| Hours become truncated seconds of the day. The window is `[from, to)` and wraps at midnight (`18 8` is night). | 0x4670a0 |
| One timer per object. The first delay uses entry 0's min/max; each firing plays a uniformly drawn entry and schedules the next draw after that entry's own min + random range. Outside its hours an entry retries 30-90 s later. | 0x4660f7, 0x467983 |
| A sound starts only when `distance² - radius² <= 250²` (`distance <= 250 m` when the radius is 0). | 0x550560 |
| Gain is the sound resource's `volume` times DirectSound 3D rolloff: full inside `mindist`, `mindist / distance` up to `maxdist`, constant beyond. The original sets these distances on the 3D buffer. | 0x55b6a0 |

The listener is the world camera. Loops follow the listener every frame and
stop when they leave their range or hours, when their shell leaves the
active world, and when the game root changes. They pause with
`System.timeWarp(0)` like the engine loops. Random one-shots use the normal
transient-effect path, so the 32-voice budget and repeat coalescing above
still apply.

Limitations: there is no stereo panning or Doppler; voices are not positioned
in 3D, only attenuated by distance. The original's `instances` and `flags`
sound properties are not interpreted. Many trees request the same bird cue,
so the transient coalescing keeps at most two copies of it playing.
`SLRR_AMBIENCE_LOG=1` logs each random request. Loop starts and stops are
always logged as `[ambience]`.


### Renderer options: anisotropic filtering

Existing natives `java.game.OptionsDialog.getRendererOption(I)I` and
`setRendererOption(II)V` use option index **6** for anisotropic filtering.
Getter returns 0/1. Setter treats zero as off and nonzero as on and saves via
the existing host graphics persistence path. Default is on; malformed/missing
keys retain that default. `save/game/openslrr_options.json` stores
`"anisotropic_filtering": true|false` under `video`; stock Config's binary layout is unchanged.

The patched OptionsDialog video menu and debug Renderer checkbox share this
value. The setting takes effect on subsequent rendered frames, enables bgfx's
maximum anisotropy reset flag and opts world colour/detail texture samplers into
anisotropic min/mag filtering. The actual maximum is backend/hardware-defined;
this is not an API for selecting individual sample counts. Shadow comparison,
cubemap and UI samplers retain their own filtering contracts. Existing texture
mip availability still limits the result. Source and compiled TUFA container
are both shipped in patch/sl/Scripts/game; no new native signature was added.


### AO and shadow controls

The same OptionsDialog getters/setters add integer IDs 7–12: AO method
(0 off, 1 HBAO, 2 GTAO), AO quality, shadow quality, shadow smoothing (each 0–2),
AO strength (0–200 percent), and AO radius (25–300 cm). Setters clamp values and
save immediately through host graphics persistence. Missing/malformed keys retain
defaults; stock Config binary layout is unchanged. Menu source and compiled TUFA
container ship together. Defaults are AO off, medium qualities, soft shadows,
100 percent strength and 100 cm radius. Debug Renderer controls share these values.

### Controller devices (#345)

`java.io.InputDevices` (OpenSLRR class, `patch/system/Scripts/io`) reads the
SDL3 device slots for Options > Controls: `count`, `name`, `type`, `power`,
`connected`, `axes`, `buttons`, `hats`, `battery`, `canRumble`, `activity`,
`rumble(i, strength, ms)`, `calibrateBegin`, `calibrateEnd`, `calibrateReset`,
`calibrated`, `calibrating`. Index i is ControlSet device i + 3. Calibrated
ranges change the raw axis values every binding sees
(`src/platform/host_input_devices.cpp`). Rumble is only a test pulse.
`canForceFeedback(i)` says whether a device takes a constant force on its
steering axis, and `testForceFeedback(i)` turns it left then right for 0.6 s.
Driving force feedback comes from the host (#395), not from scripts:
`Controller.user_SetAxisForce` still does not drive SDL haptics.

`kind(device)` takes a ControlSet device id (not an index) and returns its
class: 0 keyboard, 1 mouse, 2 pad, 3 wheel/sim peripheral, -1 none. The rebind
listener is `captureBegin`, `captureState` (0 idle, 1 releasing, 2 listening,
3 holding, 4 done, 5 cancelled), `captureDevice`, `captureAxis`,
`captureAnalog`, `captureRest`, `capturePeak`, `captureHeld`,
`captureProgress` and `captureCancel`, over `InputCapture`
(`src/platform/host_input_capture.hpp`). `captureBeginKinds(mask)` (#405)
listens only to the device kinds in the mask (`1 << kind`); the Esc key and
the left mouse button always listen, to clear and to cancel. The Java helpers
`applyCapture(cs, i)`, `ensureDevice(cs, device)`, `kindName(device)`,
`classKinds(cls)`, `classCount(cls)` and `forceFeedbackIndex()` live in the
same class; both Options screens use the first three.

### Advanced video controls (#377)

IDs 13–20 back the Advanced Video Options page: VSync, view distance (500–3000 m),
prop detail (0–3, a LOD distance multiplier of 2, 1.4, 1 or 0.7 on top of the
debug LOD bias), fog, car lights, headlight shadows, street lamps and the number
of shadowed street lamps (0–6). They persist in `save/game/openslrr_options.json` like
IDs 0–12. Each one only lowers what the matching F12 Renderer switch allows: the
renderer checks both (`host_enhanced_graphics()` and `render_debug()`). The
video page's Preset choice is script-side: `OptionsDialog.applyPreset` calls the
setters and moves the controls with `MultiChoice.setValue` / `Slider.setValue`,
which do not send their command.

### Suspension anti shares (#469)

`WheelRef.setAnti(braking, driving)`, declared by the patched
`parts/scripts/src/WheelRef.java`: the share of the axle's longitudinal load
transfer the suspension's arms carry (0 to 1.5), braking (front anti-dive,
rear anti-lift) and driving (front anti-lift, rear anti-squat). The 96
patched stock corners call it; without it the host uses typical shares for
the suspension type ([notes](../vehicles/notes.md#anti-dive-and-anti-squat-469)).

### Math functions (#463)

The patched `system/Scripts/lang/Math.java` declares, besides the stock
`random`, `sqrt`, `randomize`, `setrandseed` and the #450 `pow`:

- Constants `PI`, `HALF_PI`, `TWO_PI`, `DEG2RAD`, `RAD2DEG` and `E`
  (32-bit floats).
- Host natives in radians (`src/natives/natives_lang.cpp`): `sin`, `cos`,
  `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `exp`, `log` and
  `wrapAngle` (the same angle in [-PI, PI], through `std::remainder`).
  Like `pow`, a NaN or infinite result returns 0; `asin` and `acos` clamp
  their input to [-1, 1] and `log` returns 0 for a <= 0. They exist only in
  OpenSLRR: a script calling them does not run in the original game.
- Script helpers compiled into the class: `abs`, `sign`, `min`, `max`,
  `clamp(v, lo, hi)`, `lerp(a, b, t)`, `randomRange(lo, hi)` and
  `randomInt(n)` (an index in [0, n), 0 when n <= 0). Floats only, no
  overloads. `Part.clampTo` stays for the existing part scripts and mods.

`tests/test_script_safety.cpp` calls the natives and runs a script compiled
against the shipped `Math.class` that reads `Math.PI` and calls the helpers.
