# Native audit and roadmap

Baseline snapshot from 9 November 2026, incrementally updated 18 November 2026.
The three `Painter` natives are registered (#64). `Camera.setFog` is registered. The remaining `PhysicsRef` signatures are
bound, and `GameRef.setActiveCollision` is no longer an explicit no-op.
Earlier increments covered the resource and vehicle bindings, the Language
work described in [SCRIPT_COMPILER.md](SCRIPT_COMPILER.md), and the Leftovers classification.
The Release x64 registry was compared with the
376 entries in the stock table at `docs/stubs.txt` in the research workspace.
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
| Registered with a body | 358 |
| Registered with the explicit `native_noop` handler | 3 |
| Absent from the registry | 15 |
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
| `FindFile` and File path operations | Share canonical physical confinement (#84). Enumeration alone can address the root. Leading separators are game-relative; external junctions are rejected and internal junctions allowed. Masks and dotted names remain supported. See [SAVE_IO_FIXES.md](SAVE_IO_FIXES.md). |
| `GameRef.setState` | Sets pose and linear/angular velocities. Cars receive the state in the live solver. Generic GameRefs retain the state but have no general dynamic-body solver yet. |
| `GameRef.setActiveCollision` | Queues an idempotent activation request. The next physics preparation enables the generic object's CFG-defined collision and clears the queued flag. Existing vehicle and legacy RenderRef collision remain independent. |
| `PhysicsRef` | `create`, `createBox`, `createSphere`, `setMatrix`, `setStatic`, `getPos`, `getOri`, `getVel` (both overloads) and `getAngVel` are bound. World boxes, spheres and resource CFG bodies join scene collision; OSD click boxes remain separate. `getVel(Vector3)` returns point velocity. `setStatic(0)` hands the body to the host free-body solver (gravity, scene and car contacts; a primitive gets a 500 kg/m³ density); `setStatic(1)` fixes it again. Free bodies do not collide with each other. |
| `GameRef.setMatrix`, `WheelRef` setters | Non-finite pose/scalar/vector inputs are rejected before mutation. Per-body solver failures suspend a chassis, remove a failed loose body, or disable a free PhysicsRef; valid simulation continues for other bodies. Existing signatures/counts are unchanged (#75). |
| `GameRef.getDetail`, `RenderRef.getDetail` | Read the canonical instance's detail bias; the default is zero. No distance-dependent LOD bias producer is implemented yet. |
| `Vehicle.getHorn` | Checks the selected horn SFX row on the canonical chassis, using live player horn input or the `sethorn` command. |
| `Vehicle.hasCrime` | Returns the stored zone speed limit in m/s, or -1 without a zone. Automatic zone enter/leave producers are still missing. |
| `System.isLoading`, `isLoadingReset`, `setLdPriority` | Track active RPK scans and measured pack-work history; reset seeds the 32-sample ring without clearing ongoing scans. Priority changes scan cadence. Mesh/texture streaming has no corresponding queue. |
| `System.compileAll` | Embedded C++ compiler writes validated TUFA containers for source units, including multiple classes per file. Existing loaded classes require a restart. See SCRIPT_COMPILER.md. |
| `System.info` | Selector 5 returns the live host VM heap count. Other selectors return an empty string. |
| `ResourceRef.makeTexture` | Resolves the filename under the game root and exposes a file-backed texture path. `RenderRef.changeResource` consumes it for per-instance materials; standalone dynamic texture ID queries are still incomplete. |
| `ResourceRef.duplicate`, `RenderRef.create` | Copy both RPK and file-backed texture replacements by value. Later skin selections do not mutate previously created render instances or shared geometry. |
| `RenderRef.changeResource` | Supports RPK texture replacements and file paths from `makeTexture`, matching the original texture resource to the mesh material slot. A new selection replaces the previous file override for that source; an RPK replacement clears it. |
| `Viewport.activate`, `deactivate`, `destroy`; camera selection | Active cameras with a viewport are eligible only while that viewport exists and is active. Hiding an OSD stops its preview without requiring scripts to destroy all scene objects. Reactivation restores eligibility; null-viewport camera behavior is unchanged. With a selected scene, camera ownership follows world ancestry rather than viewport size: a full-screen Navigator local world stays separate from the resized driving view. Renderer passes respect that rectangle and viewport priority (#65); visual validation is pending. |
| `Animation` | The constructor is registered with the dotted TUFA descriptor; the earlier slash form never matched, so script `Animation`s stayed unbound. It binds a `.matrixdata` clip to the render named by the constructor. `play` / `loopPlay` / `pause` / `seek` / `getPos` / `setSpeed` sample it on the skeleton declared by that render. `setFade` blends the clip in from the bind pose. Each render keeps its own pose. |
| `RenderRef.getBoneId` | Case-insensitive lookup in that skeleton. An unknown name, or a render with no skeleton, returns 0. |
| `RenderRef.setMatrix(int, ResourceRef, Vector3, Ypr)` | Writes that bone's local pose. With another render as `bone_ref`, this render is parented on that bone. Script positions are metres. |
| `Thread.setDaemon` | Sets and clears the per-thread flag. Shutdown still cancels every fiber. |
| `System.simTime`, `timeWarp` | The simulation clock follows the warp factor, and setting it returns the previous factor. Physics uses the scaled elapsed time. UI clocks, sleeps and host timer dispatch use real time. |
| `System.setMeasure`, `getConfigOptions` | The measure setting controls digital vehicle instruments. The options refresh reads gameplay settings and schedules a debounced save when they change. Failed saves suspend automatic retries until another change; forced shutdown saves can retry. Enhanced graphics are stored separately in save/game/openslrr_options. |
| `RenderRef.setFlare`, `Navigator.updateNavigator`, `GroundRef.getNearestCross` | Directional flares use the scene's texture and an approximate sprite layout. The GPS camera and markers follow scene objects. The crossing query now reads the selected ground TRC and honours the desired crossing distance (v221 host semantics). |
| `GfxEngine.numDisplayModes`, `currDisplayMode`, `displayModeName`, `changeVideoMode` | Enumerate available Win32 32-bit display modes with an 800×600 fallback, identify the current resolution, and apply validated mode changes through the host options path. |
| `Camera.setFog` | Stores the color and the near and far distances on the driving camera. The fade uses those distances. The color argument is replaced by the day and twilight sky color before the shader reads it. |
| `GroundRef.setFog` | Stores a global color and near and far distances. Nothing in the renderer reads them. |
| `RenderRef.plotRoute` | Builds a navigator-owned coloured ribbon from the last queried route. Invalid/missing routes return zero. Texture input is not yet sampled. |
| `GroundRef` road/route natives | Selected-ground directed TRC routes, independent alignment position/direction vectors, cached length/metre position/projection queries, timed crossing halts and path exclusions. Host route-query semantics beyond the v221 port are not recovered original implementations. |
| `GroundRef` traffic natives | Explicit TRC lane/crossing-curve traversal, physical native wheel/suspension/tyre forces with matching render hubs, native-only racer placement from off-map staging, model-weight-based rival share and density scaling, finite positioned reinsertion, stable individual IDs, balanced deferred Java notifications and teardown. setTrafficCarBehaviour stores its flag only; trailing model parameters remain uninterpreted. Signal fixtures share a host crossing cycle with cached lens emission and red/yellow entry restrictions, five-second missing-lamp STOP and rear-checked blocked-driver recovery. Original signal timing, explicit STOP/YIELD signs, full pursuit and traffic contact response remain incomplete. |
| Brain AI commands (not separate native signatures) | Shared physical driver for AI_race, GoToTarget, GoToTraffic, follow and MWM ROC splines. BeginRace/stop/suspend hold; NightRace retains the race destination. Host tuning, basic world-space follow offsets; renderinstance seating and spline metadata remain incomplete. Original Bot bytecode, real City/ROC terrain, ordered ROC gates and the finish gate pass physics integration tests; full career UI/result flows remain pending gameplay validation. |
| `RenderRef.lineCreate`, `lineAdd` | Build renderable ribbons from positions, normals and widths. Color is applied per segment. |
| `Chassis.setCooling`, `setSteerWheelRadius`, `setSteerWheel`, `setBuck`, `setNitroSFX` | Store configuration. No temperature simulation, bucking effect or nitro audio consumes these values yet. The installed wheel on slot type 11 turns about the cfg `steering` locator. |
| `GroundRef.setPedestrianDensityN`, `addPedestrianType`, `remPedestrianType`, `removePedestrianType`, `pedestrianDistance` | Density, after the script's Config factor, sets people per square metre of `city.walk` row area around the driven car. Each type's `native ped` cfg supplies the render and the walk clip. Walkers follow the original engine model: free agents inside a row, pushed back by its walls and both sidewalk runs, leaving only through gates (see `src/resources/pedestrian_lines.cpp`). `removePedestrianType` is the original native table's name for `remPedestrianType`. `pedestrianDistance(pos, type)` returns the distance to the nearest live walker of that type id, or of any type for 0, and -1 when there is none. |
| `PhysicsRef.getPos` | Returns a new Vector3 copying the one stored by `setMatrix`. Without one, it returns a new Vector3 from the object's pose. The copy matters: `Osd` keyboard navigation calls `gad.getHotSpot().sub(cur)`, and returning the stored vector let the arrow keys move the button's own Rectangle (`Rectangle.pos` is the same vector the button passed to `setMatrix`). |
| `Part.setSfxLoopParams` | Stores the two parameters without applying them to a voice. |
| `Chassis.setHornSFX` | Stores all indexed sounds. The current player input plays the index 1 sample once per press. |
| `GameRef.create_native` on native cars | Builds the CFG-backed native assembly, skips a chassis-specific Java constructor and binds the Java reference to its live chassis. Tested with the real fake_racers staging/traffic flow; other native resource kinds retain their existing path. |
| `Painter.paintPart`, `doPaint`, `xPaint` | Pick the installed part under the pointer and draw into that part's own runtime texture (`paint_texture.hpp`), shown in its cfg `texture` paint slot. `paintPart` fills it with one colour; `doPaint` projects the spray brush or a decal as a square `size` metres wide onto the paint-slot triangles that face it; a temporary (`temp`) decal preview is replaced by the next one and removed by `xPaint`, by a committed stroke or two host frames after the script stops refreshing it. The executable queues `paint`/`bigdecal` part commands instead; the projection, orientation and preview expiry are host approximations. `File.write(ResourceRef)` saves such a texture as a PNG in the SDAT trailer; `readResID` restores it, or reads the JPEG the stock game wrote beside the save as `<file>.<ordinal>`. See KNOWLEDGE_BASE.md, "Garage paint booth (#64)". |
| `GameRef.getInfo`, `queueEvent` | Implement selected queries and commands. `repair` performs the native per-part deformation, damage-associated slot/wheel and CFG mount/base-mesh reset; stock Java owns the installed-tree recursion and wear/tear caps, with a native fallback only for detached assembly children omitted from `attachedParts`. Unhandled queries return zero. Other command paths still need coverage as new sections are exercised. |
| `GameType.registerCallback`, notifications | Control, drive and animate callbacks run. Notifications have event routing and GC roots. Other callback modes and event producers still need verification. |
| `Input.getAxis`, `activeAxis`, `Controller.user_SetAxisSmooth` | Keyboard, mouse and one logical XInput pad are wired. Pointer movement is excluded from rebinding. Negative mouse axis requests reset deltas, while their sensitivity argument remains unused. Default pad driving controls are retained per axis unless explicitly overridden or their physical channel is assigned elsewhere. Steering fallback and analog classification share that policy. Smoothing uses a host time-based approximation. Menu/cancel hotkey presses share a 250 ms UI debounce across command changes; select has a separate gate. Releases and driving axes remain immediate. |
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

| System | Count | Methods |
| --- | ---: | --- |
| `GroundRef` | 3 | `setWater` (2 overloads), `addWaterLimit` |
| `ParticleSystem` | 9 | `init`, `stop`, `setFreq`, `setDirectSource`, `setSource`, `setCounter`, `getCounter`, `delAction`, `modePermanent` |

There are 12 missing active signatures and 3 missing leftovers, for 15 total.
No PhysicsRef signature remains absent from the stock table.

## Roadmap

1. **Finish a playable drive on a test track.** Validate steering, brake,
   clutch, shifts and the input profile through a relaunch. Connect the remaining
   driver actions to gameplay, especially nitro activation and consumption.
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
assets/validation/driver-selector; see KNOWLEDGE_BASE.md for implementation
entry points and remaining limits.


Issue #130 changes scene collision/render collection, not native registrations.
RenderRefs no longer acquire collision merely from their visible mesh; trigger
sensors are excluded, while explicit physics and ground collision survive.
The trigger_markers suite exercises native-car ON/OFF notification delivery
and the visual/physics separation with MWM resources. See SCRIPT_RUNTIME.md,
"Trigger cues and physical bodies", for reconstructed cue settings and the
remaining in-game validation. Registration counts do not imply that the full
MWM challenge is validated.


Issue #82 batches the renderer output of existing Text/OSD natives. It changes
no registration or Java body. Existing glyph/font/colour/layer semantics are
retained; controlled D3D11 render evidence and limits are documented under
"Batched OSD text" in SCRIPT_RUNTIME.md.
