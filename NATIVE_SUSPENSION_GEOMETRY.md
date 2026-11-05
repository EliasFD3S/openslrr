# Native suspension pose geometry

## Evidence and boundary

- [EliasFD3S/slrr-reverse, 5c42f3b](https://github.com/EliasFD3S/slrr-reverse/tree/5c42f3b139aef4c7b9e0b6420c5704e22c0e6cd7), especially `engine/Runtime/Parts/WheelRef.cpp` and `Part.cpp`.
- [SLRR_rev linker contract, a2ee10de](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/docs/suspension-slot-linker-experiment.md).
- Local original executable SHA-256: `b284a7a53909d0b9ce59d59ddc11e14df40a7eb74d86fa76efb06305aadc2b03`.
- Native `WheelRef.getPos` at `0x441B60`, inspected with REA/IDA. X and Z come from the chassis template. They do **not** measure the final rendered tyre centres. Therefore Car Information's 1366/1320 mm track is insufficient evidence for forcing the Duhen's tyre meshes onto those coordinates.
- Native wheel pose update at `0x44FA70`, then read directly through the existing IDA MCP server because REA's full function request passed unsupported `offset`/`max_lines` parameters. The read-only IDA request used the server's advertised schema, on an owned copy of the executable. The owned headless database was closed without saving; no game or IDA GUI was launched. Raw observations remain in ignored `out/ida-wheel-*` files and are not bundled game source.

## Implemented subset

`suspension_geometry.cpp` translates pose arithmetic from the following native paths:

- `setArm` (`0x441770`) establishes a point relative to the wheel anchor and normalizes its hinge axis. In the host's chassis frame, the configured pivot is `reference + raw arm point`; the spring base/travel Y bias cancels at this boundary.
- `0x45018F` through `0x45031A`: retain displacement along the hinge axis and project the perpendicular component onto the configured arm circle. Arm orientation follows the signed hinge angle.
- `setHub` (`0x441880`) normalizes the strut axis and, in mode zero, stores the adjusted upper point `reference + pc - p1`.
- Hub mode `0` (`0x4508F0` through `0x450D14`): intersect the strut line and lower-attachment sphere, select the native ray root, fit the hub orientation and subtract the rotated lower attachment from the arm point.
- Hub mode `-1`: retain the arm-derived origin/orientation path, composing native alignment.

These produce separate arm and hub/wheel frames. CFG hardware attachment poses are then applied once, instead of cancelling the complete mounted wheel position back onto the template centre. Slot families 101/201/301 consume their respective frames; inherited corner lookup and ordered `linked` rules remain separate from this pose solve. `WheelRef.getPos` and `Chassis.getWheelPos` retain template X/Z for scripts.

The chassis still uses the existing fixed-step rigid body and ray/spring contact model. This is **not** a recovered unsprung-body force/joint solver. Positive-length hubs, opposite-wheel/live-axle constraints, general bone skinning and exact native steering/spin coupling remain incomplete and use the pre-existing pose fallback. Native getPos Y still uses the host's current spring/pose representation; only its X/Z template contract is corrected here.

## Validation

Unit checks cover the arm-circle radius and preserved hinge coordinate, mirrored struts, closure of lower and upper attachments, travel-dependent lateral motion, and explicit rejection of an unported hub mode. Integration constructs both original Duhen 1.5 and 1.8 DVC assemblies through Java, checks that all four supported corner geometries resolve, and measures actual tyre meshes separately from template coordinates. Native position getters stay stable during steering. Existing garage entry, suspension, inventory and force-balance checks remain enabled.

Before/after on the headless 1.5 DVC fixture: rear tyre centre about 0.657 to 0.711 m from the centreline; front about 0.683 to 0.715 m. These values emerge from installed geometry and loaded travel, not placement constants. They are numerical observations; pixel matching and FPS must still be checked in the user's game session.
