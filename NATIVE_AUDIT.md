# Native audit and roadmap

Snapshot from 9 November 2026. The Release x64 registry was compared with the
376 entries in the stock table at `docs/stubs.txt` in the research workspace.
Matching uses owner, method and parameter types, including overloads. This
measures bindings. Behavioral status below comes from reviewing the C++
implementations and following selected calls to their consumers.

| Status | Stock signatures |
| --- | ---: |
| Registered with a body | 297 |
| Registered with the explicit `native_noop` handler | 8 |
| Absent from the registry | 71 |
| **Total** | **376** |

The first category includes constant responses, inline empty bodies and
partial implementations. The mock table calls out known behavioral gaps in
that category. It is not a compatibility score or a complete runtime trace.

## Explicit no-ops

| Owner | Native signatures | Missing effect |
| --- | --- | --- |
| `Controller` | `user_SetAxisSpeed(int, float)`, `user_SetAxisForce(int, float, float)` | Override the axis rate and apply an axis force. Profile smoothing uses the separate `user_SetAxisSmooth` binding. |
| `System` | `isLoadingReset()`, `setLdPriority(int)` | Reset loader state and set a loader priority. |
| `GameRef` | `setActiveCollision()` | Enable collision for a generic world object. |
| `RenderRef` | `setMatrix(int, ResourceRef, Vector3, Ypr)`, `lineAdd(Vector3, Vector3, int, float)` | Transform in a referenced space and append line geometry. The other `setMatrix` overload has a body. |
| `ResourceRef` | `makeSound(ResourceRef, String)` | Create a sound resource. Playing existing WAV resources uses a separate binding. |

## Known mocks and partial implementations

| Native or group | Current behavior |
| --- | --- |
| `System.isLoading` | Always returns zero. The visible loading screen is managed separately by the host. |
| `System.simTime`, `timeWarp`, `setMeasure`, `getConfigOptions` | Sim time reads the host clock. Time warp echoes its argument. The measure setting controls digital vehicle instruments. The options refresh reads gameplay settings and schedules a save when they change. |
| `RenderRef.setFlare`, `Navigator.updateNavigator`, `GroundRef.getNearestCross` | Directional flares use the scene's texture and an approximate sprite layout. The GPS camera and markers follow scene objects. The crossing query returns null until the road index is available. |
| `GfxEngine.numDisplayModes`, `currDisplayMode`, `displayModeName`, `changeVideoMode` | One fixed mode is exposed, 800×600×32 at index zero. The change call is empty. Startup window settings are a separate path. |
| `RenderRef.getBoneId`, `lineCreate`, `plotRoute` | Always return zero. Bone lookup and route line construction are missing. |
| `Chassis.setCooling`, `setSteerWheelRadius`, `setSteerWheel`, `setBuck`, `setNitroSFX` | Store configuration. No temperature simulation, cockpit steering animation, bucking effect or nitro audio consumes these values yet. |
| `Part.setSfxLoopParams` | Stores the two parameters without applying them to a voice. |
| `Chassis.setHornSFX` | Stores all indexed sounds. The current player input plays the index 1 sample once per press. |
| `GameRef.getInfo`, `queueEvent` | Implement selected queries and commands. Unhandled queries return zero. Unhandled command paths still need coverage as new sections are exercised. |
| `GameType.registerCallback`, notifications | Control, drive and animate callbacks run. Notifications have event routing and GC roots. Other callback modes and event producers still need verification. |
| `Input.getAxis`, `activeAxis`, `Controller.user_SetAxisSmooth` | Keyboard, mouse and one logical XInput pad are wired. Pointer movement is excluded from rebinding. Negative mouse axis requests reset deltas, while their sensitivity argument remains unused. Smoothing uses a host time-based approximation. |
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
| `System`, `Thread` | 9 | `System.info`, `compileAll`, `analyze` (3 overloads), `netHost`, `netJoin`, `netLeave`, `Thread.setDaemon` |
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
4. **Complete garage actions and effects.** Implement the `Painter` natives
   and resource-created sound, then animation, particles, flares and fog.
   Verify paint and installed-part state through a career save and reload.
5. **Complete platform and optional systems.** Enumerate display modes,
   complete controller options and loader state, then assess networking and
   script compilation/debug natives when needed by the supported workflows.

The stock driving, suspension and game-control pages were exercised with
synthetic keyboard input in the Release x64 host. Each test applied a new
binding, read the logical axis and saved and reloaded an isolated profile.
Physical gamepad input, driving on track and the race-only handlers still
need in-game validation.
