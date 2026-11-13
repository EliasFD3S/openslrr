# External research: SLRR_rev and draft PR #3

Collected 12 November 2026 for OpenSLRR `main` at `b907b6b`.

This file summarizes knowledge from two sources that OpenSLRR agents keep
being pointed at:

- [nedobylskiy/SLRR_rev](https://github.com/nedobylskiy/SLRR_rev) at
  `77b0fb6`, a TypeScript/WebGL remaster that reads the original assets and
  interprets the game's Java sources.
- The Markdown documents of draft
  [PR #3](https://github.com/EliasFD3S/openslrr/pull/3) (branch
  `antomys/main`, head `cde988a`).

SLRR_rev has no license file. Everything below is a summary in our own
words with links; consult the linked document for detail and do not copy its
text or code. References are written `SLRR_rev:docs/<file>` and
`PR3:docs/<file>`; `python scripts/check_knowledge_refs.py --external
SLRR_rev=<checkout> --external PR3=<checkout>` checks them. OpenSLRR code
references use the same `path:symbol` form as
[KNOWLEDGE_BASE.md](KNOWLEDGE_BASE.md).

Confidence tags:

| Tag | Meaning |
| --- | --- |
| **corpus** | A parser consumed every stock file of that kind. Structure is proven, field meaning is not |
| **source** | Visible in the stock Java. Native behaviour does not follow from it |
| **exe** | Read from a disassembled EXE. Valid for that binary only |
| **obs** | Observed in the running original game |
| **ext** | Third-party reconstruction (slrr-reverse, sl-remastered, street-legal) |
| **design** | A SLRR_rev or OpenSLRR choice, not original behaviour |

## 1. Sources and how far to trust them

- **Different builds.** SLRR_rev studies a modified Steam install whose
  `System.buildNumber()` is 940 (EXE SHA-256 `f44fb88a…6e44fefc2`).
  OpenSLRR targets 2.2.1 MWM, build 606. SLRR_rev documents three EXE hashes
  (Steam, an intermediate build, the Elias archive); the MWM binary is a
  fourth. Addresses never transfer between them
  (`SLRR_rev:docs/legacy-extensions.md`, `SLRR_rev:docs/elias-workspace-archive.md`). **corpus**
- **Not independent.** SLRR_rev's 2026-11-05 audit says much of OpenSLRR's
  rigid-body and suspension dynamics derives from SLRR_rev revision
  `a2ee10de`, and OpenSLRR's SCX skinning cites SLRR_rev
  (`src/resources/scx_mesh.cpp:v4_weight_bytes`). SLRR_rev's PTX reader in turn
  cites OpenSLRR. Agreement between the two is not confirmation
  (`SLRR_rev:docs/physics-upstream-audit-2026-11-05.md`). **ext**
- **Native census.** SLRR_rev finds 418 native-flagged declarations across
  3,047 TUFA files, including mods; OpenSLRR's stock table has 376. Natives
  such as `Chassis.getRPM`, `getAIparam_*`, `Steam.*`, `runDLL`/`runMethod`
  and `Math.atan2/exp/ln/log10` belong to build 940 or mods. The MWM sources
  declare no `getRPM` or `getAIparam` native (`SLRR_rev:docs/native-status.md`). **corpus**
- **Music sets.** Build 940 has six music folders; MWM has four, which is
  what OpenSLRR implements (`SLRR_rev:docs/music.md`). **corpus**
- **Community references** (`SLRR_rev:docs/sources.md`):

  | Source | Use | Caution |
  | --- | --- | --- |
  | EliasFD3S/slrr-reverse (archived, MIT) | RPAK, TUFA, TREE op widths, SDAT, native contracts | Heuristic and stub paths; its physics integrator is a placeholder. The vendored compiler comes from TrentonCh/slrr-reverse; its relationship to this repository is not stated (`third_party/slrrc/README.openslrr.md`) |
  | nicolas-found42/sl-remastered | Recovered scalar physics: dyno, tyre curve, suspension, clutch, heat | Qualified against the Steam EXE, not MWM |
  | nicolas-found42/street-legal (Rust) | Native `forceUpdate` ordering, suspension slot linker | Targets build 606, the same family as OpenSLRR; active-script path incomplete |
  | Elias working archive (IDA database) | Native ABI, `Physics_Step` structure, wheel table stride | Its EXE hash differs from both Steam and MWM |
  | nonamesex/io_invo_scx | SCX v3/v4 layout | Unlicensed; cross-check only |
  | imagecode-git/slrr-javas | Published game Java sources | Not a native engine |

## 2. File formats

All little-endian. Each item notes OpenSLRR's status.

### RPK / RDB (`SLRR_rev:docs/formats/rpk.md`) **corpus**

- Header `RPAK`, version `0x200`, two dependency counts, 64-byte dependency
  records (ID, 56-byte path, trailer). Then a table size, a record count and
  two service words; the table end must match exactly.
- Record: parent ID, type ID, `typeof` byte, flags byte, an f32 at +10 that
  is `isparentcompatible` (not padding), payload offset and size, alias, and
  a 12-float transform when `flags & 1`. OpenSLRR reads the transform
  (`src/resources/rpk_index.cpp:parse_rpak`); the +10 float is unused.
- In a payload, `high16 = 0` means the same pack and any other value goes
  through that pack's dependency table. OpenSLRR implements this as
  `resolve_embedded_id`.
- `typeof`: 5 mesh, 7 texture, 8 game type, 9 physics body, 13 light, 14
  render object.
- `Catalog.java` compares `id() >> 16` with the vehicle type and relies on
  the low word being the original local ID. OpenSLRR's runtime ID keeps the
  low word by design.

### SCX (INVO) v3 / v4 (`SLRR_rev:docs/formats/scx.md`) **corpus**

- v3 material blocks end at size 0. Texture indices start at +44; layer 2 is
  at +52; extended materials hold UV channels at +70/+72. The signed index at
  0x38 is **self-illumination** (window light masks), not a lightmap; city
  facades have zeroed second UVs (`SLRR_rev:docs/baked-lightmaps.md`).
  OpenSLRR agrees (`src/resources/scx_mesh.cpp:scx_resolve_baked_lightmaps`).
- v3 flag `0x1` enables base-map alpha; `0x40` blends layer 2 by its alpha.
  Enable transparency from the flag, never from the file having alpha.
- v4 chunk sizes include their 8-byte header. Chunks: 0 material, 1 hard
  surface, 2 sparse bones, 3 bones, 4 vertices, 5 indices. The index chunk
  counts indices, not triangles. Skin weight groups store one weight fewer
  than influences. `0x06000001` without a primary map is the colour source;
  `0x06000007` is the lightmap.
- Geometry is in centimetres; world units are metres (×0.01, no axis swap).

### TUFA / TREE (`SLRR_rev:docs/formats/tufa.md`) **corpus**

- Sections `TUFA`, `CONS`, `FILD`, `MTHD`, `CLSS`, `TREE`; a file may hold
  several classes. Every CONS entry starts with an explicit kind; there is
  no padding after kind 4. Strings are Windows-1252.
- Method flags: native `0x40`, static `0x8`. An empty TREE does not mean
  native. Descriptors are JVM-like with dotted names and may omit `V`.
- TREE nodes are `op u8, source line u16, [u32 operand]`; the list of wide
  ops is in the doc and matches OpenSLRR's table (`src/jvm/tufa.cpp:parse_trees`).
- The `.java` dialect has resource literals and integer truthiness, so plain
  `javac` cannot compile it.

### CFG (`SLRR_rev:docs/mounts-and-driving.md`, `SLRR_rev:docs/engine-world.md`) **corpus**

- `slot x y z yaw pitch roll id`, angles applied in YXZ order. `attach` is
  an allowed connection; `compatible` is an alternative identity of the
  current slot, not a connection.
- `slottype` chooses the parent frame: 0 the part root, 11 the `steering`
  line, 101+ the wheel with index `type − 101`.
- The CFG stockpart tree is not the car Java builds: engine, suspension,
  brakes, wheels and tyres come from `addStockParts`
  (`SLRR_rev:docs/gameplay.md`). **source**
- `body x y z yaw pitch roll mass <shape>`: shapes `ball`, `sphere`, `box`,
  `rodx/y/z`, or a mesh file. `box` values are half-extents. `nocollision`
  excludes the preceding body (`SLRR_rev:docs/map-physics-investigation.md`). **exe**
- The `spring a b c` triple before each wheel is unproven. SLRR_rev reads
  length / compression / rebound; OpenSLRR reads base / travel / damping
  (`include/slrr/part_config.hpp:PartWheelSpring`). **design**
- Some stock lamp CFGs contain a merged token `-1.570.000000`; both projects
  repair it.

### SDAT saves (`SLRR_rev:docs/vehicle-serialization.md`) **corpus**

- `SDAT`, version `0x0100`, type 1 or 2, flags; payload from +8. Type 2 ends
  with a trailer of unsaved resources (kind 7 = image bytes) and the pack
  list. Saved RIDs use the file-local pack ordinal in the high word; `0` or
  `0xffff` means transient. OpenSLRR implements the container
  (`src/natives/natives_io.cpp:sdat_frame`).
- `Part.save` writes subclass tails of varying length, so only each class's
  own `load` can read a part; a flat parser fails. **source**

### Road network `city.trc` (`SLRR_rev:docs/road-graph-debug.md`, `SLRR_rev:docs/traffic-upstream-audit-2026-11-08.md`) **corpus**

- Version 1, then four group counts: 53 and 218 crossing records (pools A
  and B), 484 path records, 1,572 road records. References are one-based
  global record IDs across all groups.
- Splines are 57-byte cubic Hermite segments in **metres**. Traffic drives
  against the stored road order (**obs**). Pool A crossings mark
  out-of-town stretches where traffic reaches about 100 km/h (**obs**).
- OpenSLRR has a reader (`src/formats/trc.cpp:trc_read`) that nothing calls.

### Sidewalks `city.walk` and `city.rest` (`SLRR_rev:docs/character-skeleton-research.md`) **corpus**

- 1,799 text rows `bounds id;payload`; the payload is binary encoded as
  letter pairs A–P. Record kinds: 21 junction wall quad, 22 gate quad with
  links, 23 sidewalk run. Junction quads are relative to the row origin.
- Observed in the original game (**obs**): paired runs bound a zone that
  people walk along; gates are goals; they leave a zone only through a
  junction transition; a nearby car makes them hurry while staying inside
  the zone.
- `city.rest` has 755 boxes in areas with 484, 53 and 218 entries, exactly
  the TRC path and pool counts. That it indexes the road network is an
  unverified inference worth testing for issue #19.

### Skeletons and clips (`SLRR_rev:docs/character-skeleton-research.md`) **corpus**

- `.bon`/`.skl` are 9-line records with file-order parents that may point
  forward. Use the skeleton the RPK render declares, not a same-named file.
- Scale conventions stack easily (reader ×0.1, skinning ×10, SCX ×0.01):
  pick one metre-space convention.

## 3. Script and native contracts

- Hierarchy `Native → ResourceRef → GameRef → GameType`. `ResourceRef.set`
  rebinds without changing identity (`SLRR_rev:docs/java-game-objects.md`). **ext**
- `GII_ANIMATE` (28) callbacks go to every registered receiver, including
  OSDs, or modal editors never consume keys. OpenSLRR dispatches mode 28. **source**
- A `Dummy` with the world-tree-root flag is an isolated render world for
  inventory and catalog previews (`SLRR_rev:docs/part-previews.md`). **source**
- `Part.repair()` ends with the native command `"repair"`; the Java
  mesh-replacement alternative is commented out (`SLRR_rev:docs/deformable-physics.md`).
  Confirmed in the MWM source (`GAME:parts/scripts/src/Part.java:repair`). OpenSLRR has no
  such verb. **source**
- `GroundRef.getNearestCross(pos, dist)` searches both crossing pools for the
  node minimising `| |node − pos| − dist |`, so `dist` is not a cutoff; ties
  keep file order; the result is a new `Vector3`
  (`SLRR_rev:docs/traffic-upstream-audit-2026-11-08.md`). OpenSLRR returns
  null. **ext + exe**
- Traffic lights (`trlight` ICFG) bind once at creation: nearest crossing
  first, then a kind check, then the nearest flagged link. A lamp only reads
  the crossing's phase. Observed phases at one junction: 20 s green, 5 s
  amber (`SLRR_rev:docs/traffic-signals-research.md`). **exe + obs**
- `Valocity.java` registers traffic models with relative weights and police
  separately (`SLRR_rev:docs/java-traffic-models.md`). It also registers six
  pedestrian types; the MWM source sets density 0.003 by day and 0.0005 at
  night, and `GroundRef.setPedestrianDensity` multiplies it by
  `Config.pedestrianDensity` (`SLRR_rev:docs/character-skeleton-research.md`,
  `GAME:sl/Scripts/game/src/Valocity.java:setPedestrianDensity`). **source**
- Math: integer `sin`/`cos` take degrees, float versions radians; `random`
  follows an MSVC-style LCG (`SLRR_rev:docs/java-math.md`). **corpus**
- `WheelRef.setLoadSmooth` writes coefficient 14; `setDamping(f)` sets
  rebound to 0.7 × bump. OpenSLRR matches (`SLRR_rev:docs/physics-reverse-port.md`). **ext**
- Both projects still lack all `Painter` and `ParticleSystem` natives, the
  `GroundRef` route queries, and `PhysicsRef.create`/`createSphere`.

## 4. Game mechanics and telemetry

- **Original-game telemetry exists.** SLRR_rev's
  `data/telemetry/original-game/2026-11-05` holds 6 captures (16 sessions,
  5,373 samples) of RPM, speed, gear channel, raw pedals and native dyno
  curves for the Shimutshibu ST9, Baiern CoupeSport 2.5, Emer Nonus,
  Einvagen 110 GTK (NA and turbo) and Duhen SunStrip. There is no wheel-speed
  channel, so clutch and tyre slip cannot be separated. The data came from
  build 940 (`SLRR_rev:docs/stock-physics-telemetry.md`). **obs**
- **Baiern targets** from the original UI: 1,482 kg, RWD, 0–100 km/h
  6.109 s, quarter mile 14.542 s, mile 39.763 s. The UI's derived figures are
  inconsistent; do not fit physics to them (`SLRR_rev:docs/original-mechanics.md`). **obs**
- **Dyno disagreement.** SLRR_rev's port of sl-remastered excludes the final
  table endpoint and ignores the nitro argument of `getTorque`/`getHP`.
  OpenSLRR keeps the last sample reachable and blends the nitro bank
  (`src/physics/dyno_model.cpp:dyno_sample`). The nitro blend only affects
  the `getTorque`/`getHP` natives; driving always samples with nitro 0. One
  side is wrong for the target EXE; settle it against the binary. Both
  reproduce the witness value for default DynoData, about 82.087 N·m at the
  437 RPM step (`src/physics/dyno_model.cpp:dyno_self_check`). **ext**
- **Clutch pitfall.** `ClutchF` is not a torque cap in N·m, and both sides of
  the clutch exchange must share one wheel inertia, or energy is created
  (`SLRR_rev:docs/engine-telemetry-calibration.md`). **obs + design**
- High-RPM launch plateaus are genuine original behaviour. **obs**
- **Damage.** No deformation solver exists in the Java, and node-beam is not
  proven for SLRR. SLRR_rev's XPBD cages, thresholds and condition formula are
  invented, as are OpenSLRR's (`src/physics/deformation_cage.cpp`). **ext**
- Button steering slews at 2 rad/s in the original. **source**

## 5. Maps, collision, traffic and pedestrians

- **Placements.** Placed objects are `gametype 0x<pack:id> params
  x,y,z,yaw,pitch,roll[,seed]`; the per-instance seed chooses
  `rendertypes` and `colors` variants. Test Track's compiled RPK lacks its
  placements, which only its text RDB holds (`SLRR_rev:docs/maps.md`). **exe + corpus**
- **Static collision** (`SLRR_rev:docs/map-physics-investigation.md`):
  collision comes from CFG `body` lines, not render meshes. Of 4,855 stock
  `body` lines, 3,419 reference `.scx` meshes and most others are spheres or
  boxes. Tree `fa06a` has a trunk box of half-extents 0.05 × 3 × 0.05 m;
  `fa12a` 0.05 × 6 × 0.05; `fa03a` has no body. **corpus**
- **Thin-trunk pitfall.** SLRR_rev first tested eight chassis corners against
  boxes and cars still passed through 0.1 m trunks; a full oriented-box
  (separating axis) test fixed it. **design, measured**
- Ground collision: SLRR_rev did not decode the `PHYS` chunk that OpenSLRR
  already uses (`src/resources/scene_instances.cpp:append_ground_collision`).
  Worth sharing upstream.
- Ground triangles can lie below Y = −20 m; do not clip them. Where ground
  layers overlap, use the highest surface below the probe. **design**
- **Traffic bot recipe** (`SLRR_rev:docs/traffic-test.md`): kinematic
  puppets far away, simple rigid bodies within 30 m, spawning off-screen at
  80 m or more, pure-pursuit lane following and time-gap following. All
  **design**; known bugs include visible spawns.
- **Pedestrian controller** (`SLRR_rev:docs/character-skeleton-research.md`):
  corridors from two runs and two gates, lateral positions 0.25–0.75 with
  0.3 m clearance, transitions only through linked gates and inside the
  junction contour, never joining unlinked geometry. OpenSLRR's
  `src/resources/pedestrian_lines.cpp` ports this and adds its own inner-edge
  guess for wide corridors. **design**

## 6. Rendering performance lessons

SLRR_rev WebGL measurements, not native frame rates
(`SLRR_rev:docs/map-render-performance.md`, `SLRR_rev:docs/graphics-debug.md`,
`SLRR_rev:docs/map-geometry-cutoff.md`):

- The city at an 850 m range is about 1,600 draws and 530,000 triangles; at
  500 m about 600–660 draws. OpenSLRR reports about 20,000 draws (issue #18).
- Draw count, not triangle count, drives CPU submission. The large wins were
  one GPU texture per DDS (1,430 → 136), pooling identical opaque materials
  (3,049 → 575) and merging building parts into texture arrays. Paint-masked
  facade materials must stay unshared.
- Splitting ground surfaces larger than 128 m into a 64 m grid halved the
  submitted triangles at a 200 m radius.
- Hi-Z occlusion only paid off when occluders were limited to terrain and
  structures (never trunks or props), with float depth for the 0.01 m near
  plane and a tiny bias. Never reset visibility on a timer.
- Night render variants are duplicate meshes: exclude them by alias, not by
  `flags = 4`. OpenSLRR already does
  (`src/resources/scene_instances.cpp:ground_prop_is_mesh`).
- Crash-frame spikes came from collision BVH rebuilds and soft-body solves,
  not rendering: refit instead of rebuilding
  (`SLRR_rev:docs/reports/2026-11-07-collision-cpu-investigation.md`).
- Physics catch-up can snowball; OpenSLRR caps it at four ticks per frame.

## 7. Migrating mechanics from SLRR_rev

Maintainer guidance (issue #21): SLRR_rev implements most game mechanics.
Before implementing anything in OpenSLRR, first check whether SLRR_rev has
it and try to migrate it.

How to look:

1. Search `SLRR_rev:docs/native-status.md` for the native's class and name.
   It lists implemented handlers, temporary mocks and missing methods per
   overload.
2. Search the SLRR_rev source tree for the native name or the mechanic. Its
   natives are registered with `jvm.natives.register(owner, name, args, …)`
   in `SLRR_rev:src/runtime/*-native.ts`; mechanics live in
   `SLRR_rev:src/engine/`, rendering in `SLRR_rev:src/render/` and formats
   in `SLRR_rev:src/formats/`.
3. Read the matching SLRR_rev document for the evidence level and the known
   limits, and its tests (`SLRR_rev:tests/`) for the cases to keep.

How to migrate:

- Port the behaviour into the OpenSLRR layer that owns it
  ([ENGINE_CONTRACT.md §2](ENGINE_CONTRACT.md#2-compatibility-contract)),
  in C++. Do not carry over the Three.js, WebGL or async structure.
- Keep original behaviour apart from SLRR_rev design. Many SLRR_rev values
  are tuning or invented models (damage cages, traffic driver, pedestrian
  speeds); label them as such in the code and docs.
- Check against the MWM scripts and data. SLRR_rev runs build 940, and some
  of its natives, music sets and addresses do not exist in MWM (§1).
- Record the SLRR_rev file and revision in the commit and in the
  documentation, as ENGINE_CONTRACT §18 requires for ported algorithms.
- SLRR_rev has no license file; both READMEs state that the two projects
  exchange code. Keep attribution in the notes of each port.

Where OpenSLRR gaps already have an SLRR_rev implementation (revision
`77b0fb6`):

| OpenSLRR gap | SLRR_rev source | State in SLRR_rev |
| --- | --- | --- |
| Map objects without collision (#16) | `SLRR_rev:src/engine/map-collision.ts:MapCollision`, `SLRR_rev:src/engine/rigid-body-3d.ts:orientedBoxContact`, placements in `SLRR_rev:src/engine/map-objects.ts` | CFG `box` bodies as oriented boxes in an 8 m grid, full box-versus-box test, support from top faces. Mesh bodies of map objects are not imported |
| `GameRef.setActiveCollision` no-op | `SLRR_rev:src/runtime/game-type-native.ts:setActiveCollision`, `SLRR_rev:src/runtime/game-object.ts:requestActiveCollision` | Queues a one-time collision activation that the physics step consumes |
| `PhysicsRef` world objects | `SLRR_rev:src/runtime/physics-ref-native.ts:PhysicsRefNative` | Script-created static boxes; `setStatic(0)` throws, `getVel`/`getAngVel` return zero. `create` and `createSphere` are absent |
| Native `repair` command (#17) | `SLRR_rev:src/runtime/part-native.ts:repairDamage` | Implemented: clears deformation, slot damage and wheel damage for the installed subtree |
| `GroundRef.getNearestCross` | `SLRR_rev:src/runtime/ground-road-native.ts:GroundRoadNative`, `SLRR_rev:src/engine/road-nearest.ts:nearestRoadCrossing`, `SLRR_rev:src/formats/trc.ts` | Implemented with the documented radius semantics |
| Traffic model natives | `SLRR_rev:src/runtime/traffic-native.ts` | `addTrafficN` and `delTraffic` keep per-ground model weights. `addTrafficCar`, `setWater` and `Navigator.updateNavigator` are mocks in `SLRR_rev:src/runtime/game-boot.ts` |
| Traffic simulation and signals | `SLRR_rev:src/engine/road-traffic.ts`, `SLRR_rev:src/engine/traffic-stream.ts`, `SLRR_rev:src/engine/traffic-control.ts`, `SLRR_rev:src/engine/bot.ts`, `SLRR_rev:src/render/traffic-lamps.ts` | Host design driven by the TRC graph; not tied to the stock traffic natives yet |
| Pedestrians on sidewalks (#19) | `SLRR_rev:src/formats/pedestrian.ts`, `SLRR_rev:src/engine/pedestrian-walking.ts`, `SLRR_rev:src/engine/pedestrian-transition.ts`, `SLRR_rev:src/render/pedestrian-crowd.ts`, `SLRR_rev:src/render/character-ragdoll.ts` | OpenSLRR already ported the corridors; compare its transitions and its inner-edge guess with these. The ragdoll is not ported |
| Engine and drivetrain physics | `SLRR_rev:src/engine/combustion-model.ts:CombustionModel`, `SLRR_rev:src/engine/native-vehicle-physics.ts`, `SLRR_rev:src/engine/mechanical-system.ts` | Recovered from sl-remastered plus fitted host responses; settle the dyno endpoint disagreement first (§4) |
| City rendering cost (#18) | `SLRR_rev:src/render/map-material-pool.ts`, `SLRR_rev:src/render/building-material-batch.ts`, `SLRR_rev:src/render/map-object-stream.ts`, `SLRR_rev:src/render/map-geometry-chunks.ts`, `SLRR_rev:src/render/map-gpu-occlusion.ts`, `SLRR_rev:src/render/static-map-shadows.ts` | Measured techniques (§6); port the idea, not the WebGL code |
| CFG `osd_*` gauges | `SLRR_rev:src/osd/java-gauges.ts` | Implemented |
| Telemetry export for comparison | `SLRR_rev:src/engine/drive-telemetry.ts` | Same record format as the original-game captures |
| Car damage | `SLRR_rev:src/engine/deformable-panel.ts`, `SLRR_rev:src/engine/vehicle-damage.ts` | SLRR_rev's own model, not original behaviour |

Missing in SLRR_rev too, so these need original research: all `Painter`
and `ParticleSystem` natives, the `GroundRef` route queries (`findRoute`,
`getRouteLength`, `getRoutePos`, `getRouteDist`, `alignToRoad`,
`getStartDirection`), `RenderRef.plotRoute`, `PhysicsRef.create` and
`createSphere`, dynamic standalone `PhysicsRef` bodies,
`pedestrianDistance`, `removePedestrianType` and `haltTrafficCross`.

## 8. Draft PR #3: what it contains and what applies to main

PR #3 documents were written against the merge base `77d19e7`. PRs #6
(instrumentation) and #11 (review of PR #3) were merged **into the PR #3
branch**, not into `main`. `main` took only the `System.arraycopy`, PTX and
dyno fixes from it, with their tests, in `8637eae`, then deleted those tests
in `b907b6b`.

| PR #3 document | Status relative to main |
| --- | --- |
| `PR3:docs/ARCHITECTURE.md` | Subsystem map still valid; omits the compiler, pedestrians, characters and `src/formats`. Size figures are stale |
| `PR3:docs/SOURCE_REVIEW.md` | Most risks still open on main; condensed in [KNOWLEDGE_BASE.md §10](KNOWLEDGE_BASE.md#10-open-risk-register) |
| `PR3:docs/ROADMAP.md` | Its milestone 0 (tests, CI) regressed on main; folded into [MILESTONES.md](MILESTONES.md) |
| `PR3:docs/INSTRUMENTATION.md` | Trace, metrics, native report, physics telemetry and replay, fault injection, fuzzing, ASan: none of it is on main |
| `PR3:docs/CRASH_LOGGING.md` | `--log-all` crash logging and minidumps: not on main |
| `PR3:docs/MULTIAGENT.md` | Worktree-per-agent workflow; its scripts are not on main |
| `PR3:docs/COMMUNITY_REFERENCES.md` | Community notes on RPK IDs, `typeof`, suspension tutorials; treat as hypotheses |
| `PR3:docs/ENGINE_CONTRACT.md` | The older French version; main's English contract supersedes it |
| `PR3:docs/NATIVE_AUDIT.md` | Older counts (307/6/63). Newer than main only for reproducible audit tooling and the `GfxEngine` display-mode row |

Tooling on the PR #3 branch that main could adopt, subject to the
maintainers' decision after `b907b6b` removed tests:

- A pinned stock-native manifest with a generator and `--check` mode, which
  would make the README's coverage figures reproducible.
- Crash logging with minidumps and a run manifest.
- Physics telemetry with a deterministic probe and replay comparison.
- Native call reporting (registered, called, missing at run time).
- RPK and TUFA count hardening against malformed files.
