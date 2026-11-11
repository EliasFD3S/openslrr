# Native audit and roadmap

Snapshot from 9 November 2026. The Release x64 registry was compared with the
376 entries in the stock table at `docs/stubs.txt` in the research workspace.
Matching uses owner, method and parameter types, including overloads. This
measures bindings. Behavioral status below comes from reviewing the C++
implementations and following selected calls to their consumers.

| Status | Stock signatures |
| --- | ---: |
| Registered with a body | 307 |
| Registered with the explicit `native_noop` handler | 6 |
| Absent from the registry | 63 |
| **Total** | **376** |

The first category includes constant responses, inline empty bodies and
partial implementations. The mock table calls out known behavioral gaps in
that category. It is not a compatibility score or a complete runtime trace.

## Explicit no-ops

| Owner | Native signatures | Missing effect |
| --- | --- | --- |
| `System` | `isLoadingReset()` | Seed the original resource streaming history. |
| `System` | `analyze(Object)`, `analyze(float)`, `analyze(int)` | The original discards the arguments. |
| `GameRef` | `setActiveCollision()` | Enable collision for a generic world object. |
| `RenderRef` | `setMatrix(int, ResourceRef, Vector3, Ypr)` | Transform a bone in a referenced space. The other `setMatrix` overload has a body. |

## Known mocks and partial implementations

| Native or group | Current behavior |
| --- | --- |
| `System.isLoading`, `setLdPriority` | Report in-progress RPK scans and choose their cooperative work cadence. Mesh and texture streaming have no corresponding queue. |
| `System.info` | Selector 5 returns the live host VM heap count. Other selectors return an empty string. |
| `System.netHost`, `netJoin`, `netLeave` | Compatibility calls return zero. |
| `Thread.setDaemon` | Sets and clears the per-thread flag. Shutdown still cancels every fiber. |
| `System.simTime`, `timeWarp` | The simulation clock follows the warp factor, and setting it returns the previous factor. Physics uses the scaled elapsed time. UI clocks, sleeps and host timer dispatch use real time. |
| `System.setMeasure`, `getConfigOptions` | The measure setting controls digital vehicle instruments. The options refresh reads gameplay settings and schedules a save when they change. |
| `RenderRef.setFlare`, `Navigator.updateNavigator`, `GroundRef.getNearestCross` | Directional flares use the scene's texture and an approximate sprite layout. The GPS camera and markers follow scene objects. The crossing query returns null until the road index is available. |
| `GfxEngine.numDisplayModes`, `currDisplayMode`, `displayModeName`, `changeVideoMode` | Enumerates host display modes and calls the live video-mode path. Mode switching, resize behavior and persistence still need interactive validation. |
| `RenderRef.getBoneId`, `plotRoute` | Always return zero. Bone lookup and route line construction are missing. |
| `RenderRef.lineCreate`, `lineAdd` | Build renderable ribbons from positions, normals and widths. Color is applied per segment. |
| `Chassis.setCooling`, `setSteerWheelRadius`, `setSteerWheel`, `setBuck`, `setNitroSFX` | Store configuration. No temperature simulation, cockpit steering animation, bucking effect or nitro audio consumes these values yet. |
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

## Missing signatures by system

| System | Count | Methods |
| --- | ---: | --- |
| `GroundRef` | 24 | `setWater` (2 overloads), `addWaterLimit`, `addTrafficCar`, `remTrafficCar`, `notifyTrafficCar`, `addTrafficN`, `addTrafficP`, `delTraffic`, `setTrafficCarBehaviour`, `haltTrafficCross`, `haltTrafficPath`, `getStartDirection`, `findRoute`, `getRouteLength` (2 overloads), `getRoutePos`, `getRouteDist`, `alignToRoad`, `setPedestrianDensityN`, `addPedestrianType`, `remPedestrianType`, `removePedestrianType`, `pedestrianDistance` |
| `PhysicsRef` | 8 | `create`, `createSphere`, `setStatic`, `getPos`, `getVel` (2 overloads), `getAngVel`, `getOri` |
| `Animation` | 9 | Constructor, `finalize`, `setSpeed`, `setFade`, `play`, `loopPlay`, `pause`, `seek`, `getPos` |
| `ParticleSystem` | 9 | `init`, `stop`, `setFreq`, `setDirectSource`, `setSource`, `setCounter`, `getCounter`, `delAction`, `modePermanent` |
| `System` | 1 | `compileAll` |
| `GameRef`, `RenderRef` | 6 | `GameRef.setState`, `getDetail`, `RenderRef.getDetail`, `addPoints`, `weld`, `progress` |
| `Camera` | 1 | `setFog` |
| `Painter`, `Vehicle` | 5 | `Painter.doPaint`, `paintPart`, `xPaint`, `Vehicle.getHorn`, `hasCrime` |

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
   Add traffic and pedestrians after routes and collision are stable.
4. **Complete garage actions and effects.** Implement the `Painter` natives,
   validate resource-created sound end to end, then animation, particles, flares and fog.
   Verify paint and installed-part state through a career save and reload.
5. **Complete platform and optional systems.** Enumerate display modes,
   complete controller options and loader state, then assess networking and
   script compilation/debug natives when needed by the supported workflows.

The stock driving, suspension and game-control pages were exercised with
synthetic keyboard input in the Release x64 host. Each test applied a new
binding, read the logical axis and saved and reloaded an isolated profile.
Physical gamepad input, driving on track and the race-only handlers still
need in-game validation.
