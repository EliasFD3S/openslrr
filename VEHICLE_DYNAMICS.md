# Dynamic vehicle support — first port stage

Reference: [SLRR_rev a2ee10de](https://github.com/nedobylskiy/SLRR_rev/tree/a2ee10def17b8838184651ca0b8d174d198e5aed), `rigid-body-3d.ts`, `suspension-3d.ts`, `native-vehicle-physics.ts`, `vehicle-3d.ts` and `runtime/fixed-clock.ts`. [PHYSICS_PORT_AUDIT.md](PHYSICS_PORT_AUDIT.md) records the branch comparison and upstream limits.

## What runs

`vehicle_dynamics` is a standalone C++ kernel in script-space metres, kilograms and seconds. It integrates a six-degree-of-freedom rigid chassis using centre-of-mass position, quaternion orientation, world angular momentum, gravity, forces and torques. Box inertia is the upstream approximation, not recovered compound inertia from the original executable.

The clock runs at 240 Hz, processes at most eight ticks per frame, and retains its time debt. Rendering does not choose the integration timestep. Four wheel rays use the configured radii, rest/min/max lengths, spring rates and bump/rebound damping. The recovered suspension equation doubles projected velocity and defaults rebound damping to 0.7 times bump damping. There is no tensile spring force without contact. The upstream per-wheel force cap of three chassis weights and bump-stop stiffness multiplier of eight remain explicit safety adaptations.

Forces act at their chassis anchors, so unequal loads produce rotation. Ground contacts use triangle normals and impulses with friction. Initial overlap recovery belongs to the chassis contact solve: a wheel at minimum travel is a unilateral hard-stop contact, preventing a buried tyre's penalty force from launching the car. This is an explicit host stability adaptation. One penetration correction per patch/iteration prevents duplicate box corners from overcorrecting the position. Parked tyre friction is a bounded dissipative approximation; native SL2 combined slip is not connected yet.

## Native and render boundary

`part_physics_advance` builds its mass/centre from the installed CFG bodies and its spring parameters from the WheelRef state populated by Java `forceUpdate`. Only cars in the active world are simulated; inventory preview worlds remain isolated. Script `suspend` pauses a vehicle and `wakeup` resumes it. Script pose changes/reset reinitialise its frame once. The result updates the canonical GameRef matrix, position/orientation and velocity.

Spring reference positions come from CFG `wheel` coordinates, or an explicit native WheelRef `setPos` override. They are separate from hardware attachment poses and final tyre mesh centres. Native `WheelRef.getPos` reads template X/Z, so the Duhen Car Information screenshot (1366/1320 mm track) cannot validate the final visual track. The earlier regression incorrectly forced that equivalence. The current adapter uses recovered arm-circle and strut/trailing-hub pose arithmetic, then applies the authored hardware mounting pose once. Both Duhen variants are checked with template and rendered tyre positions measured separately. See [NATIVE_SUSPENSION_GEOMETRY.md](NATIVE_SUSPENSION_GEOMETRY.md) for binary identity, addresses, provenance and unsupported cases.

The dynamic hardpoint uses CFG `base + travel`, independently of tuned spring rest length. Native edits to a part's own attachment slot refresh its parent attachment too. Supported native geometry supplies separate arm and hub/wheel frames; unsupported configurations retain the previous travel/alignment approximation. The ordered linker is refreshed from those frames. Native WheelRef input positions are separate from output positions, preventing accumulated travel offsets.

The original script command `setsteer` now applies bounded steering input using CFG `maxsteer`, Ackermann length and each wheel's steering factor (native `setSteer` can override that factor). Wheel and hub carriers rotate around the wheel centre; one-anchor arm renders omit steering, while two-anchor shafts follow the carrier. Clearing steering restores the native alignment without accumulating rotations. This is parked visual steering; engine thrust and a complete tyre-force steering model remain separate work.

The bgfx renderer projects existing vehicle mesh buffers onto a sampled ground plane along the scene light. Depth testing keeps receivers behind visible geometry; a stencil union prevents overlapping triangles from multiplying shadow darkness. This planar silhouette is a host approximation, not a reconstruction of the original shadow renderer. It supports the sampled slope but does not conform to steps, multiple receiver levels or self-shadowing. Inventory previews do not receive world shadows. No per-frame CPU mesh projection or extra shadow textures are needed. Numerical tests cover negative floor heights and renderer Z reflection; GPU appearance still requires in-game review.


`settle_parked` and `part_plant_parked` no longer set vehicle height. Rendering and scripts observe the same dynamic pose. Physics never applies the renderer's Z reflection.

The terrain sampler reports absence of contact instead of an implicit y=0 plane. It sees preloaded garage triangles and active world geometry, returning the highest surface below its ceiling and its normal. The garage preload is disabled in other maps. Static triangles use a 2 m XZ index built at load time; oversized triangles use a fallback list. RenderRef terrain (including dealer POLY overrides) is transformed and indexed once per geometry/pose change. A frame snapshot refreshes the active set once, so wheel/contact queries no longer traverse all render nodes or all terrain triangles. Removal, reparenting and moving terrain are covered by tests. Running gear is positioned first, followed by one ordered-linker pass per vehicle update, replacing a complete assembly relink after each corner part.

## Evidence

Garage entry after the dealer issues `reset`, `setsteer`, `stop`, `wakeup`, then `suspend` before the first host physics tick. A requested reset now retains pending ground placement across that immediate suspension and across delayed active-world selection. The normal fixed-step contact solver resolves the initial overlap. Once linear/angular motion is quiet and spring support balances weight for 60 ticks, the suspended body freezes and stops accruing physics ticks. A later suspended lift teleport without a new reset stays at its scripted pose. The regression reproduces this command ordering for both Duhen variants on real garage terrain; placement does not require installing a part or inventing a floor/height offset.

Standalone tests check free fall, off-centre impulse, COM frame roundtrip, upstream bump/rebound numerical witnesses, spring force balance, no-floor free fall, negative-height terrain, slopes, 30/144 FPS trajectory equality and retained clock debt.

The asset integration test constructs stock vehicles through TUFA and the native bindings, without opening a game window:

- Baiern: approximately 1523.64 kg, four supported wheels, approximately 14946.87 N of total support.
- Duhen SunStrip 1.8 DVC: approximately 1242.30 kg, four supported wheels, approximately 12187.00 N of total support.
- Duhen template track regression: front 1.366 m, rear 1.320 m; chassis origin approximately 0.798 m on the fixture floor. Final tyre mesh positions are checked independently through the native arm/hub geometry, together with steering and restoration. These are numerical geometry checks, not visual confirmation.
- Both settle on the real Green Slip garage triangles. Checks also cover rendered/script pose agreement, teleport/reset, suspend/resume and inventory exclusion. The original zero-height garage reset is checked separately, including initial tyre/floor overlap recovery. Duhen also settles with only RenderRef terrain enabled, checking that the real terrain indices remain unchanged across 480 frames.

These are headless numerical/integration checks. Visual comparison in the user's session remains necessary; no game window was launched for this port.

## Remaining work

This stage does not supply engine thrust, clutch/transmission coupling, native tyre slip, anti-roll-bar forces, side-wall collision, independent unsprung rigid bodies or the complete native arm/hub/instant-centre constraint solver. XPBD deformation, damage and detached-body coupling also remain separate port stages. They must use the same physical state, not a second owner of the chassis transform.
