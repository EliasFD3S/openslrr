# Native audit and roadmap

Baseline snapshot from 9 November 2026, incrementally updated 12 November.
`Camera.setFog` is registered. Earlier increments covered the resource and
vehicle bindings, the Language work described in
[SCRIPT_COMPILER.md](SCRIPT_COMPILER.md), and the Leftovers classification.
The Release x64 registry was compared with the
376 entries in the stock table at `docs/stubs.txt` in the research workspace.
Matching uses owner, method and parameter types, including overloads. This
measures bindings. Behavioral status below comes from reviewing the C++
implementations and following selected calls to their consumers.

| Status | Stock signatures |
| --- | ---: |
| Registered with a body | 329 |
| Registered with the explicit `native_noop` handler | 4 |
| Absent from the registry | 43 |
| **Total** | **376** |

The first category includes constant responses, inline empty bodies and
partial implementations. The mock table calls out known behavioral gaps in
that category. It is not a compatibility score or a complete runtime trace.

## Explicit no-ops

| Owner | Native signatures | Missing effect |
| --- | --- | --- |
| `System` | `analyze(Object)`, `analyze(float)`, `analyze(int)` | The original discards the arguments. |
| `GameRef` | `setActiveCollision()` | Enable collision for a generic world object. |

## Known mocks and partial implementations

| Native or group | Current behavior |
| --- | --- |
| `GameRef.setState` | Sets pose and linear/angular velocities. Cars receive the state in the live solver. Generic GameRefs retain the state but have no general dynamic-body solver yet. |
| `GameRef.getDetail`, `RenderRef.getDetail` | Read the canonical instance's detail bias; the default is zero. No distance-dependent LOD bias producer is implemented yet. |
| `Vehicle.getHorn` | Checks the selected horn SFX row on the canonical chassis, using live player horn input or the `sethorn` command. |
| `Vehicle.hasCrime` | Returns the stored zone speed limit in m/s, or -1 without a zone. Automatic zone enter/leave producers are still missing. |
| `System.isLoading`, `isLoadingReset`, `setLdPriority` | Track active RPK scans and measured pack-work history; reset seeds the 32-sample ring without clearing ongoing scans. Priority changes scan cadence. Mesh/texture streaming has no corresponding queue. |
| `System.compileAll` | Embedded C++ compiler writes validated TUFA containers for source units, including multiple classes per file. Existing loaded classes require a restart. See SCRIPT_COMPILER.md. |
| `System.info` | Selector 5 returns the live host VM heap count. Other selectors return an empty string. |
| `Animation` | Binds a `.matrixdata` clip to the render named by the constructor. `play` / `loopPlay` / `pause` / `seek` / `getPos` / `setSpeed` sample it on the skeleton declared by that render. `setFade` blends the clip in from the bind pose. Each render keeps its own pose. |
| `RenderRef.getBoneId` | Case-insensitive lookup in that skeleton. An unknown name, or a render with no skeleton, returns 0. |
| `RenderRef.setMatrix(int, ResourceRef, Vector3, Ypr)` | Writes that bone's local pose. With another render as `bone_ref`, this render is parented on that bone. Script positions are metres. |
| `Thread.setDaemon` | Sets and clears the per-thread flag. Shutdown still cancels every fiber. |
| `System.simTime`, `timeWarp` | The simulation clock follows the warp factor, and setting it returns the previous factor. Physics uses the scaled elapsed time. UI clocks, sleeps and host timer dispatch use real time. |
| `System.setMeasure`, `getConfigOptions` | The measure setting controls digital vehicle instruments. The options refresh reads gameplay settings and schedules a save when they change. |
| `RenderRef.setFlare`, `Navigator.updateNavigator`, `GroundRef.getNearestCross` | Directional flares use the scene's texture and an approximate sprite layout. The GPS camera and markers follow scene objects. The crossing query returns null until the road index is available. |
| `GfxEngine.numDisplayModes`, `currDisplayMode`, `displayModeName`, `changeVideoMode` | One fixed mode is exposed, 800×600×32 at index zero. The change call is empty. Startup window settings are a separate path. |
| `Camera.setFog` | Stores the color and the near and far distances on the driving camera. The fade uses those distances. The color argument is replaced by the day and twilight sky color before the shader reads it. |
| `GroundRef.setFog` | Stores a global color and near and far distances. Nothing in the renderer reads them. |
| `RenderRef.plotRoute` | Always returns zero. Route line construction is missing. |
| `RenderRef.lineCreate`, `lineAdd` | Build renderable ribbons from positions, normals and widths. Color is applied per segment. |
| `Chassis.setCooling`, `setSteerWheelRadius`, `setSteerWheel`, `setBuck`, `setNitroSFX` | Store configuration. No temperature simulation, bucking effect or nitro audio consumes these values yet. The installed wheel on slot type 11 turns about the cfg `steering` locator. |
| `GroundRef.setPedestrianDensityN`, `addPedestrianType`, `remPedestrianType` | Density, after the script's Config factor, scales a walking crowd around the driven car, capped at 12. Each type's `native ped` cfg supplies the render and the looped walk clip. They do not follow a road graph. `pedestrianDistance` is still missing. |
| `PhysicsRef.getPos` | Returns the Vector3 stored by `setMatrix`. Without one, it returns a new Vector3 from the object's pose. |
| `Part.setSfxLoopParams` | Stores the two parameters without applying them to a voice. |
| `Chassis.setHornSFX` | Stores all indexed sounds. The current player input plays the index 1 sample once per press. |
| `GameRef.getInfo`, `queueEvent` | Implement selected queries and commands. Unhandled queries return zero. Unhandled command paths still need coverage as new sections are exercised. |
| `GameType.registerCallback`, notifications | Control, drive and animate callbacks run. Notifications have event routing and GC roots. Other callback modes and event producers still need verification. |
| `Input.getAxis`, `activeAxis`, `Controller.user_SetAxisSmooth` | Keyboard, mouse and one logical XInput pad are wired. Pointer movement is excluded from rebinding. Negative mouse axis requests reset deltas, while their sensitivity argument remains unused. Smoothing uses a host time-based approximation. |
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
| `GroundRef` | 21 | `setWater` (2 overloads), `addWaterLimit`, `addTrafficCar`, `remTrafficCar`, `notifyTrafficCar`, `addTrafficN`, `addTrafficP`, `delTraffic`, `setTrafficCarBehaviour`, `haltTrafficCross`, `haltTrafficPath`, `getStartDirection`, `findRoute`, `getRouteLength` (2 overloads), `getRoutePos`, `getRouteDist`, `alignToRoad`, `removePedestrianType`, `pedestrianDistance` |
| `PhysicsRef` | 7 | `create`, `createSphere`, `setStatic`, `getVel` (2 overloads), `getAngVel`, `getOri` |
| `ParticleSystem` | 9 | `init`, `stop`, `setFreq`, `setDirectSource`, `setSource`, `setCounter`, `getCounter`, `delAction`, `modePermanent` |
| `Painter` | 3 | `doPaint`, `paintPart`, `xPaint` |

There are 40 missing active signatures and 3 missing leftovers, for 43 total.

## Roadmap

1. **Finish a playable drive on a test track.** Validate steering, brake,
   clutch, shifts and the input profile through a relaunch. Connect the remaining
   driver actions to gameplay, especially nitro activation and consumption.
   Compare acceleration, stopping and suspension against stock telemetry at
   different render rates.
2. **Unify world collision and damage.** Connect `PhysicsRef`,
   `GameRef.setActiveCollision` and `GameRef.setState` to the live world.
   Validate track barriers and contact with props. Connect impact deformation
   and wear to the parts and the regional HUD display.
3. **Complete the navigation and race loop.** Read road and route data,
   implement the `GroundRef` route queries, `Navigator.updateNavigator`
   and route rendering. Verify map, rear view and cruise control in a race.
   Add traffic after routes and collision are stable. Pedestrians already walk near the player; they do not follow the road graph.
4. **Complete garage actions and effects.** Implement the `Painter` natives
   and resource-created sound, then animation, particles, flares and fog.
   Verify paint and installed-part state through a career save and reload.
5. **Complete platform and optional systems.** Enumerate display modes,
   complete controller options and loader state, then assess networking and
   additional debug behavior when needed by the supported workflows.

The stock driving, suspension and game-control pages were exercised with
synthetic keyboard input in the Release x64 host. Each test applied a new
binding, read the logical axis and saved and reloaded an isolated profile.
Physical gamepad input, driving on track and the race-only handlers still
need in-game validation.
