# OpenSLRR knowledge base for coding agents

Verified against `main` at `b907b6b` (12 November 2026). Every code reference
below is written as `path` or `path:symbol` and can be rechecked with
`python scripts/check_knowledge_refs.py`. Symbols are used instead of line
numbers so the references survive edits.

This file condenses what an agent needs before touching the engine. It does
not replace [ENGINE_CONTRACT.md](ENGINE_CONTRACT.md), which remains the
authoritative contract. Knowledge taken from the sibling project SLRR_rev
and from draft PR #3 is in [EXTERNAL_RESEARCH.md](EXTERNAL_RESEARCH.md).
The proposed roadmap is in [MILESTONES.md](MILESTONES.md).

## Contents

1. [Evidence rules](#1-evidence-rules)
2. [Project identity and game data](#2-project-identity-and-game-data)
3. [Build, run and diagnostics](#3-build-run-and-diagnostics)
4. [Subsystem map](#4-subsystem-map)
5. [Data flows](#5-data-flows)
6. [Invariants and pitfalls](#6-invariants-and-pitfalls)
7. [Constants worth knowing](#7-constants-worth-knowing)
8. [Native coverage at a glance](#8-native-coverage-at-a-glance)
9. [Where the documentation disagrees with the code](#9-where-the-documentation-disagrees-with-the-code)
10. [Open risk register](#10-open-risk-register)
11. [Open issues: verified starting points](#11-open-issues-verified-starting-points)
12. [Self-check](#12-self-check)
13. [Keeping this file true](#13-keeping-this-file-true)

## 1. Evidence rules

Rank evidence before acting on it. The order follows
[ENGINE_CONTRACT.md §2](ENGINE_CONTRACT.md#2-compatibility-contract):

| Evidence | What it proves |
| --- | --- |
| Native declaration in a stock class | The API exists: class, name and descriptor |
| Stock Java caller | What the script expects from the native |
| Disassembly of the reference EXE | Behaviour, for that exact binary only |
| Corpus parse of every stock file | File structure, not field meaning |
| Third-party port or reconstruction | A proposal, not proof |
| A similar function name | Nothing |

- The active code is the reference for the implementation. Comments and
  older audits can describe earlier states (§9 lists known cases).
- A native registered with a body is not proof of complete behaviour.
  Distinguish absent, explicit no-op, fixed value, partial, implemented and
  validated in a named scenario.
- Compilation and in-game validation are separate results. A headless probe
  is not a visual validation.
- Agreement between OpenSLRR and SLRR_rev on physics is not independent
  confirmation: parts of OpenSLRR's dynamics were derived from SLRR_rev
  (see [EXTERNAL_RESEARCH.md](EXTERNAL_RESEARCH.md#1-sources-and-how-far-to-trust-them)).

**SLRR_rev first.** SLRR_rev implements most game mechanics. Before
implementing a missing native or mechanic, check whether SLRR_rev already
has it and migrate it into the owning OpenSLRR layer, recording the source
file and revision. Verify the result against the MWM scripts, and keep
SLRR_rev's own design choices labelled as such. The procedure and a map of
current gaps to SLRR_rev code are in
[EXTERNAL_RESEARCH.md §7](EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev).

## 2. Project identity and game data

- OpenSLRR is a C++20 reimplementation of the Street Legal Racing: Redline
  engine for Windows x64, rendering through bgfx (Direct3D 11 in practice).
  It runs the game's own TUFA script classes and resources.
- The reference game is **SLRR 2.2.1 MWM** (build 606). OpenSLRR does not
  detect the installed build: `System.buildNumber` is a stock native whose
  host binding returns the constant 606
  (`src/natives/natives_io.cpp:register_io_natives`), so the boot check in
  `src/boot/boot.cpp:boot_game` only proves the binding exists. Other builds
  and mods are untested, not rejected. SLRR_rev studies a modified Steam
  build 940, so its counts and addresses do not transfer directly.
- A game root is any directory containing `system/Scripts/lang/System.class`.
  An explicit `--root`, else `SLRR_GAME_ROOT`, is used as given, with no
  fallback when it is invalid. Without either, the host tries `.`,
  `data/game`, `game`, `../game`, `../../game`,
  `Street Legal Racing - Redline` and `../Street Legal Racing - Redline`
  (`src/main.cpp:main`, `src/platform/game_root.cpp:find_game_root`).
- `data/` is gitignored and holds the local install and reference sources.
  Never commit game data.
- Stock Java sources, when present, follow this layout:

  | Layer | Path in the install or source tree |
  | --- | --- |
  | Core library (`java.lang`, `java.util`, `java.render`, `java.sound`, `java.io`) | `system/Scripts/<package>/src/*.java`, compiled TUFA beside it |
  | Career, garage, menus | `sl/Scripts/game/src/*.java` |
  | Part hierarchy | `parts/scripts/src/*.java` |
  | Cars | `cars/racers/*` |

- MWM ships four music sets: `Garage_Shop`, `Roam_ride`, `Race_chase`,
  `Main_menu` (`MUSIC_SET_GARAGE`..`MUSIC_SET_MENU` = 0..3 in `Sound.java`).
  The host matches this (`src/platform/host_music.cpp:host_music_change_set`).
  The extra `Misc_Menu` and `Anim_Scene` sets in SLRR_rev belong to build 940.
- Java changes are allowed only for specific authorized features. Shipped
  changes live in `patch/` with their source (`patch/sl/Scripts/game/src/OptionsDialog.java`).
  A file that exists only in the local game tree is not a patch.

## 3. Build, run and diagnostics

- Build requirements and commands are in [README.md](../README.md#build).
  The CMake guard rejects anything but Windows x64 (`CMakeLists.txt`).
  `bgfx.cmake` is cloned manually into the gitignored `third_party`
  folder at a pinned commit.
- **Known build blocker on a fresh clone:** `CMakeLists.txt` declares the
  `compile_game_sources` executable from `tools/compile_game_sources.cpp`,
  but `/tools/` is gitignored (`.gitignore`) and the file is not tracked.
  CMake stops with "Cannot find source file" unless the file exists locally.
  The README's coverage generator under `tools/` is missing for the same
  reason. This was inferred from standard CMake behaviour and `git ls-files`,
  not reproduced on Windows.
- The vendored compiler `third_party/slrrc` is a C++17 static library built
  with `/fp:precise`; its literal self-test must pass before any output is
  published (`src/jvm/script_compiler.cpp:script_compile_all`).
- Command-line flags are parsed in `src/main.cpp:main`: `--help`/`-h`,
  `--game`, `--window`, `--root DIR`, `--spawn-car pack.rpk:0xID` (implies
  `--auto-new`), `--auto-new`, `--no-wait` (`--auto-new` plus a three-frame
  limit), `--auto-cmd mainmenu|street|escape|N`, `--vehicle-osd PATH`, a
  positional player name, and the headless probes `--probe`, `--probe-gc`,
  `--probe-dyno`, `--probe-pacejka`. `escape` is accepted but missing from
  the usage text.
- Environment variables: `SLRR_GAME_ROOT` and `SLRR_VLC_DIR`
  (`src/platform/video_fmv_vlc.cpp`). There are no others.
- There is no test target on `main`. `main` had a CTest suite (added in
  `8637eae` with PR #3's `System.arraycopy`, PTX and dyno regressions) until
  `b907b6b` deleted it. A Windows CI workflow only ever existed on the PR #3
  branch. The probes are the only automated checks: `--probe` and
  `--probe-gc` need a game root, `--probe-dyno` and `--probe-pacejka` do not.
- Ctrl+F12 opens the ImGui debug panel (`src/render/debug_overlay.cpp:handle_message`).
  Its FPS readout is `1000 / cpuTimeFrame`, and VSync defaults to on
  (`src/render/debug_overlay.hpp:RenderOptions`).

## 4. Subsystem map

| Area | Entry points | Owns |
| --- | --- | --- |
| Startup | `src/main.cpp:main`, `src/boot/boot.cpp:boot_game`, `src/boot/boot.cpp:start_init` | Flags, game root, service order, Init, the main loop |
| Frame | `src/boot/mainloop.cpp:mainloop_frame` | The ordered per-frame pump (§5.2) |
| Sections | `src/boot/section_machine.cpp:change_active_section`, `src/boot/section_machine.cpp:adopt_actual` | `GameLogic.actualState`, enter/exit |
| Script events | `src/boot/script_events.cpp:script_event_emit`, `src/boot/script_events.cpp:script_callbacks_tick` | GameType notifications and callback modes 8/9/28 |
| Class files | `src/jvm/tufa.cpp:tufa_parse` | TUFA containers, constants, TREE node lists |
| VM | `src/jvm/jvm.cpp:invoke`, `src/jvm/jvm.cpp:invoke_by_name`, `src/jvm/tree_eval.cpp:tree_eval` | Classes, statics, method lookup, TREE interpretation |
| Threads | `src/jvm/script_threads.cpp:script_threads_tick` | Cooperative Windows fibers |
| GC | `src/jvm/gc.cpp:collect_garbage`, `src/jvm/gc.cpp:gc_safe_point` | Mark/sweep heap, root providers |
| Script compiler | `src/jvm/script_compiler.cpp:script_compile_all`, `third_party/slrrc` | `System.compileAll` |
| Native registry | `src/natives/native_registry.cpp:descriptor_match`, `src/natives/natives_internal.hpp:native_noop` | `class#method#descriptor` table |
| Natives | `src/natives/natives_boot.cpp:register_boot_natives` and the `natives_*.cpp` files | Script API adapters |
| Resources | `src/resources/rpk_index.cpp:resolve_embedded_id`, `src/resources/rpk_index.cpp:model_for_resource` | RPK packs, RIDs, RSD properties |
| Meshes and textures | `src/resources/scx_mesh.cpp:load_scx_geometry`, `src/resources/ptx_image.cpp:load_texture_file`, `src/resources/dds_image.cpp` | SCX v3/v4, PTX, DDS, PNG/TGA |
| Scene | `src/resources/scene_instances.cpp:scene_instances_collect`, `src/resources/scene_instances.cpp:scene_instances_ground_map` | Render instances, maps, placements, collision index |
| Part CFG | `src/resources/part_config.cpp:parse_part_config` | Slots, wheels, renders, bodies, HUD `osd_*` lines |
| Parts and cars | `src/resources/part_runtime.cpp:part_create`, `src/resources/part_runtime.cpp:part_command`, `src/resources/part_runtime.cpp:part_physics_advance` | Part/Chassis/Vehicle natives, assembly, damage, pedestrians, HUD state (about 4,900 lines) |
| Physics | `src/physics/vehicle_dynamics.cpp`, `src/physics/tire_model.cpp`, `src/physics/dyno_model.cpp`, `src/physics/deformation_cage.cpp` | Rigid body, tyres, dyno, deformation |
| Pedestrians | `src/resources/pedestrian_lines.cpp`, `src/formats/walk.cpp` | Sidewalk corridors from `city.walk` |
| Characters | `src/resources/character.cpp`, `src/resources/skeleton.cpp`, `src/formats/matrixdata.cpp` | Skeletons, clips, skinning, Animation |
| Renderer | `src/render/bgfx_lib_backend.cpp:flush`, `src/render/bgfx_lighting.cpp:draw_instance`, `src/render/bgfx_shadows.cpp`, `src/render/bgfx_osd.cpp:draw_osd` | Frame, materials, shadows, OSD and HUD |
| UI | `src/ui/osd_registry.cpp:osd_add_widget`, `src/ui/osd_registry.cpp:osd_on_field_write` | OSD widgets, hit-testing, modal state |
| Platform | `src/platform/host_input.cpp`, `src/platform/host_sfx.cpp`, `src/platform/host_music.cpp`, `src/platform/video_fmv.cpp`, `src/platform/host_game_options.cpp` | Input, audio, FMV, options |

Code that exists but is not used:

- `src/formats/trc.cpp` (road network) and `src/formats/rest.cpp` parse
  their formats but have no caller.
- `src/render/bgfx_shadows.cpp:draw_vehicle_shadows` (the legacy projected
  car shadow) is never called. Its two shader programs are still created at
  init but only used by that function.
- `src/render/null_backend.cpp` is not compiled. The WGL fallback in
  `src/render/bgfx_backend.cpp` is disabled because `SLRR_HAS_LIBBGFX` is
  always defined.
- `src/physics/sl2_pacejka.cpp` only serves `--probe-pacejka`. The active tyre
  model is `src/physics/tire_model.cpp:tire_forces`.

## 5. Data flows

### 5.1 Startup

1. `src/main.cpp:main` parses flags, resolves the game root, creates the
   `NativeRegistry` and `Jvm`, then calls `boot_game`.
2. `src/boot/boot.cpp:boot_game` bootstraps the classpath, registers every
   native family through `src/natives/natives_boot.cpp:register_boot_natives`,
   loads options through the engine-owned `java.util.Config`
   (`src/platform/host_game_options.cpp:host_game_options_load`), scans every
   `.rpk` under the root (`src/resources/rpk_index.cpp:open_all`), checks the
   build number, opens the window and plays the intros.
3. `src/boot/boot.cpp:start_init` runs `Init.<init>(I)` with halting on the
   first unbound native.
4. The main loop repeats: window pump, section render sync,
   `mainloop_frame`, GC safe point, `Sleep(1)`.

### 5.2 Frame order

`src/boot/mainloop.cpp:mainloop_frame` runs, in order: resource loading pump,
music, options update (Config re-read every 100 ms, debounced save), sun time,
`script_threads_tick`, section adoption, async message pump (only when the
GameType thread is not alive), controller and hotkeys,
`script_callbacks_tick`, world camera, `dispatch_due_timers`, credits and
loading-screen special cases, the fallback clock, mechanic inventory,
`part_physics_advance`, camera follow, mechanic pointer, `render.flush()` and
the quit check. Do not move an event delivery or add a second pump for the
same port.

Inside `src/resources/part_runtime.cpp:part_physics_advance`: hinges, then
`scene_instances_prepare_physics`, then N chassis ticks, engine audio,
steering wheels, driver pose and pedestrians.

Inside `src/render/bgfx_lib_backend.cpp:flush`: camera, `sync_instances`,
static shadow, car depth, reflections, sky, dynamic world, garage shell,
flare, FMV, OSD and HUD, previews, ImGui, `bgfx::frame`.

### 5.3 Native call resolution

- Natives are registered as `register_method("java.pkg.Class", "name",
  "(args)Ret", fn)`. Descriptors use dotted class names. A duplicate
  registration throws. `descriptor_match` treats a missing return type as
  `V` (`src/natives/native_registry.cpp:descriptor_match`).
- `Jvm::invoke` finds the most specific method up the class chain. For a
  native or empty method it looks up a host leaf with `native_has_alias`.
  A registered host leaf also overrides a Java body (`src/jvm/jvm.cpp:invoke`).
- `invoke_by_name` scores overloads by live argument tags and redirects some
  OSD builder names to `java.render.Osd` (`src/jvm/jvm.cpp:invoke_by_name`).
- An unbound native is **not** fatal outside Init. A script call logs
  `[jvm] native/empty TREE without host binding …` to stderr and returns
  void; a non-native empty method returns a default value. Only while Init
  runs (`halt_on_unbound`) does an unbound native halt the chain. The
  registry's own "Missing native" exception fires only for a direct host
  call with an unregistered key. Watch stderr for these lines when a
  feature silently does nothing.

### 5.4 Resources

- Three identifier spaces must stay apart: the local ID in a pack, the
  serialized reference that goes through the owning pack's dependency table,
  and the host runtime ID `(pack_id << 16) | local`
  (`include/slrr/rpk_index.hpp:make_full_id`).
- Resolve embedded references with the owning pack:
  `src/resources/rpk_index.cpp:resolve_embedded_id`. Resolving against the
  caller's pack gives a valid but wrong resource.
- `model_for_resource` follows `sourcefile`, `mesh` and `render`, keeps the
  repeated `texture` keys in order, and prefers a `.dds` sidecar.
- SCX geometry is in centimetres; loading applies 0.01
  (`include/slrr/scx_mesh.hpp`). `.scy` paths are swapped for `.scx`
  (`src/resources/scene_instances.cpp:load_geometry`).
- `RpkIndex::read_payload` reopens the pack file on every call
  (`src/resources/rpk_index.cpp:read_payload`).

### 5.5 Cars

1. A script creates a part; `part_create` loads the RSD `native car|part`
   CFG (`src/resources/part_config.cpp:load_part_config`) and creates a
   `Record`.
2. Installation goes through `part_install` and `place_child` with slot
   matrices from `src/resources/part_config.cpp:part_slot_matrix`.
3. Each 60 Hz host tick runs `advance_chassis_tick`, which configures the
   dynamics and calls `VehicleDynamics::advance` for two 120 Hz substeps
   (`include/slrr/vehicle_dynamics.hpp:VehicleDynamics`). At most four host
   ticks run per frame; excess time is dropped.
4. Wall contact is `collide_chassis`: four 0.18 m spheres against the static
   index. It runs only for a released chassis whose `drive.torque` is set,
   and `configure_drive` sets it only for the player-controlled car with
   gear ratios, a dyno and a positive maximum RPM. Other cars get no wall
   collision, impact damage or mileage
   (`src/resources/part_runtime.cpp:advance_chassis_tick`,
   `src/resources/part_runtime.cpp:configure_drive`).
5. Impacts feed `damage_chassis` and the XPBD cage
   (`src/physics/deformation_cage.cpp`). `dynamics_sync` publishes poses.

### 5.6 Maps and world

- `ResourceRef.set(I)` on a GroundRef calls
  `src/natives/natives_resref.cpp:spawn_ground_shell`, which builds
  `GroundMapData` through `scene_instances_ground_map`.
- Compiled ground data contains RSD placements
  (`gametype 0x<pack:id> params x,y,z,yaw,pitch,roll[,seed]`), `EXTP`
  exterior packs, `PHYS` collision triangles and `POLY` render sections
  (`src/resources/scene_instances.cpp:append_ground_placement`,
  `src/resources/scene_instances.cpp:append_ground_collision`).
  The `PHYS` chunk is used as map collision; SLRR_rev has not decoded it.
- Placements (trees, lamps, buildings) become instanced render props in 64 m
  cells (`src/resources/scene_instances.cpp:scene_instances_add_ground_props`).
  Only the `render` lines of their CFG are kept
  (`src/resources/scene_instances.cpp:ground_prop_renders`). They are not
  GameRef nodes and are **not** in the collision index.
- `scene_instances_prepare_physics` rebuilds the collision index from GameRef
  render nodes once per physics frame. Floor and sweep queries are
  `scene_instances_sample_floor` and `scene_instances_sweep_sphere`.
- The per-instance seed and variant choice match the reverse-engineered
  rule: `trunc(x+y+z) & 0x7fffffff` when the seventh parameter is missing,
  then `rendertypes[seed % n]` and `colors[seed % n]`.

### 5.7 Pedestrians

- `city_walk` loads the hard-coded `maps/city/ai/city.walk`; walkers run on a
  worker thread (`src/resources/part_runtime.cpp:PedSim`,
  `src/resources/part_runtime.cpp:tick_pedestrians`).
- Corridors are rows with exactly two kind-23 runs and two kind-22 gates
  (`src/resources/pedestrian_lines.cpp:build`). For corridors wider than
  5.2 m, `zone_point` keeps people near a guessed inner edge; that guess is
  an OpenSLRR-only heuristic.
- The headcount is `density × walkable area` inside 400 m, with no fixed
  cap. Height comes from the walk data, not from the floor.
- Pedestrians are skinned and animated through the same Animation code the
  natives use (`src/resources/character.cpp:character_bind_animation`).

### 5.8 HUD and OSD

- OSD coordinates are −1..1 with Y down. `osd_set_framebuffer` rescales icon
  widths for the aspect ratio.
- `src/render/bgfx_osd.cpp:draw_vehicle_hud` draws the stock instrument
  layout from a hard-coded table, scaled by `stock_scale = 0.5625 / (h/w)`.
  CFG `osd_*` lines (`include/slrr/part_config.hpp:HudSlot`) override it.
- HUD values come from `src/resources/part_runtime.cpp:part_vehicle_hud`:
  RPM, km/h, mileage, condition = min(wear, tear), nitro, turbo, gear.

### 5.9 Options and saves

- Options mirror `java.util.Config` and save to `save/game/options` through
  the stock Java `Config.saveConfig` after a 500 ms debounce, plus a final
  save at shutdown (`src/platform/host_game_options.cpp:host_game_options_update`).
- Enhanced options (MSAA, reflections, clouds, sunlight, local lights) are
  session-only and exposed through `OptionsDialog.get/setRendererOption`
  (`src/natives/natives_script.cpp:register_script_natives`).
- Car saves use the SDAT container (`src/natives/natives_io.cpp:sdat_frame`).

## 6. Invariants and pitfalls

VM and lifetime:

- Script threads are cooperative fibers on the host thread, never parallel
  workers. Native C++ work inside a fiber is not preempted.
- An `Object*` held in C++ is not a root. Use `Jvm::RootScope`
  (`include/slrr/jvm.hpp:RootScope`) across yields, or register a root
  provider for a native registry.
- GC runs only at the host safe point, never inside a call, construction,
  nested modal pump or another collection. `disableGC`/`enableGC` are
  idempotent flags, not counters.
- Outside a script fiber, a TREE loop stops jumping after 8,192 back-edges
  per target, silently, and the 4,000,000-step budget fails the method
  (`src/jvm/tree_eval.cpp:tree_eval`). Inside a fiber the step counter
  resets at each 2,048-operation time-slice checkpoint instead.
- Float `!=` is evaluated as an integer comparison on purpose (an MWM
  protection workaround). Do not "fix" it without tests.
- `System.arraycopy` grows the destination on purpose
  (`src/natives/natives_internal.hpp:native_arraycopy`).

Resources and data:

- Keep repeated RSD `texture` keys in order; never deduplicate them.
- The high bit of a RID can be set while scripts hold it as `int32_t`.
  Never serialize a native pointer into a RID or script integer.
- A CFG `box` body holds half-extents. `PhysicsRef.createBox` takes full
  sizes. Target rule for world collision: objects without a `body` must not
  get collision from their render mesh. Today the collision index is still
  built from GameRef render geometry unless a map supplies a collision
  override (`src/resources/scene_instances.cpp:scene_instances_prepare_physics`).
- A `linked a [b] [parentSlot]` rule: the first matching rule wins; do not
  sort them. LOD renders are alternatives, not layers.

Physics:

- Physics runs at fixed steps; a higher frame rate must not speed up
  simulated time. A missing ground contact means air, not a floor at Y = 0.
- The physics delta is clamped to 0.1 s and scaled by the time warp
  (`src/jvm/jvm.cpp:advance_sim_time`), and at most four host ticks run per
  frame, so below 15 FPS the simulation slows down.
- Repairs and resets must restore deformation, slot damage and wheel damage
  together. Today deformation is restored only when Java sets tear to at
  least 1 (`src/resources/part_runtime.cpp:restore_deformation`).

Rendering and UI:

- Loading, CPU residency, GPU upload, visibility, shadow casting and
  collision are separate decisions. Render culling must not remove ground.
- The world far plane is forced to 3,000 m in `flush`, whatever the Config
  view range says.
- The host keeps fallback paths for scripts that are not active. They must
  go quiet when a script thread already owns the port, or events double.
- A MessagePort wake does not close a dialog. Closing one dialog must not
  remove another's modal shield.

Process:

- Fix the general primitive. Do not add `if (class_name == ...)` exceptions
  to the VM. The code already contains many name-based special cases
  (OSD builder redirection, Splash, MainMenu, Garage, CarSelect, Mechanic,
  damage panel classification by CFG filename); do not add more.
- Preserve uncommitted work in the tree. Bring fixes over by hunks, not by
  restoring whole files.
- Document every new native: class, name, descriptor, effect, lifetime and
  persistence.

## 7. Constants worth knowing

| Constant | Value | Where |
| --- | --- | --- |
| Physics substep / host tick | 120 Hz / 60 Hz | `include/slrr/vehicle_dynamics.hpp:update_frequency` |
| Host ticks per frame | at most 4 | `src/resources/part_runtime.cpp:kMaxPhysicsTicksPerFrame` |
| Frame delta clamp | 0.1 s for input, callbacks and camera; 0.1 s for physics, then time-warp scaled | `src/boot/mainloop.cpp:kDtClamp`, `src/jvm/jvm.cpp:advance_sim_time` |
| GC threshold | 8,192 allocations, then grows with the heap | `include/slrr/jvm.hpp:gc_threshold_` |
| TREE time-slice check | every 2,048 operations | `src/jvm/tree_eval.cpp:tree_eval` |
| Script pass budget | 8 ms, 16 ms with a MAX_PRIORITY thread | `src/jvm/script_threads.cpp:script_threads_tick` |
| Fiber stack | 64 KiB commit, 1 MiB reserve | `src/jvm/script_threads.cpp:script_thread_start` |
| SFX voices | 32 total, 3 menu, 2 copies per cue, 35 ms coalescing | `src/platform/host_sfx.cpp:host_sfx_play_file` |
| Chassis wall probe | four spheres of 0.18 m | `src/resources/part_runtime.cpp:collide_chassis` |
| Prop cell | 64 m | `src/resources/scene_instances.cpp:kPropCellSize` |
| Shadow maps | 2048² | `src/render/bgfx_lib_backend.hpp:kShadowResolution` |
| Pedestrian radius / density scale | 400 m / ×4 | `src/resources/part_runtime.cpp:tick_pedestrians` |
| Live GameRef IDs | from `0x70000000` | `src/resources/gameref.cpp:g_next_live` |

## 8. Native coverage at a glance

From [NATIVE_AUDIT.md](NATIVE_AUDIT.md) and
`assets/progress/native-coverage.json` (12 November 2026): of 376 stock
signatures, 329 have a body, 4 are explicit no-ops and 43 are missing.
`scripts/render_native_coverage.py` recomputes these counts from `src/`;
`--list missing` prints the missing signatures.

| Missing family | Count | Blocks |
| --- | ---: | --- |
| `GroundRef` routes, traffic, water, pedestrians | 21 | Navigation, traffic, `pedestrianDistance`, `removePedestrianType` |
| `PhysicsRef` | 7 | `create`, `createSphere`, `setStatic`, `getVel` ×2, `getAngVel`, `getOri`: world physics objects |
| `ParticleSystem` | 9 | Smoke, sparks and other effects |
| `Painter` | 3 | Paint booth: `doPaint`, `paintPart`, `xPaint` |
| Leftovers (`Welder`) | 3 | No stock caller found |

Explicit no-ops: `System.analyze` ×3 (faithful) and
`GameRef.setActiveCollision` (a real gap). Two host-only no-ops outside the
stock table: `Player.<init>` and `LoadingScreen.<init>`.

Partial bodies that matter for upcoming work: `GameRef.setState` applies to
cars only, `GroundRef.getNearestCross` returns null,
`RenderRef.plotRoute` returns 0, `GroundRef.setFog` stores values nobody
reads, and `Part.setSfxLoopParams` stores values without applying them.

## 9. Where the documentation disagrees with the code

| Statement | Code reality |
| --- | --- |
| README lists "projected car shadows" | The projected pass `draw_vehicle_shadows` is never called. Car shadows come from the car depth map (`src/render/bgfx_shadows.cpp:update_car_depth`) |
| `patch/README.md`: exterior scenery range 5 km | `flush` forces 3,000 m (`src/render/bgfx_lib_backend.cpp:flush`) |
| `patch/README.md`: "F12 → Renderer" | The panel opens with Ctrl+F12 |
| NATIVE_AUDIT: pedestrian crowd capped at 12 and not following a road graph | No headcount cap; walkers follow `city.walk` sidewalk corridors (not the TRC road graph) |
| README: pedestrian density slider scales the crowd; `patch/README.md`: the slider is gone | The patched OptionsDialog drops the slider and forces `Config.pedestrianDensity = 1.5` (`patch/sl/Scripts/game/src/OptionsDialog.java`); the host still scales the crowd by that value (`src/resources/part_runtime.cpp:tick_pedestrians`) |
| NATIVE_AUDIT: `GfxEngine` exposes one fixed 800×600 mode and an empty change call | Modes are enumerated with `EnumDisplaySettingsW` and applied (`src/natives/natives_script.cpp:display_modes`) |
| HOST_NATIVE_UTILITIES: engine-loop audio not yet implemented | `update_engine_audio` runs every physics frame |
| Issue #19: Animation natives are not bound | They are registered and drive the host crowd (`src/natives/natives_renderref.cpp:register_renderref_natives`) |
| `.gitignore` last line `*.md !README.md` | A single malformed pattern that matches nothing; Markdown files are not ignored |
| Stale comments: "no green-thread budget yet" in `src/boot/mainloop.cpp`, "240 Hz tick" in `src/resources/scene_instances.cpp`, "WGL until libbgfx is linked" in `include/slrr/render_backend.hpp` | Fibers have budgets, physics is 120/60 Hz, libbgfx is always linked |

## 10. Open risk register

Risks found by the PR #3 source review (baseline `60d7d71`) and rechecked on
`main`. "Open" means the code still has the pattern; none was reproduced at
run time.

| Risk | Where |
| --- | --- |
| DirectShow frame copy trusts `width × height × 4` against the sample size | `src/platform/video_fmv.cpp` (`grab_frame`) |
| TUFA and RPK readers reserve declared counts before checking the remaining bytes | `src/jvm/tufa.cpp:parse_constants`, `src/resources/rpk_index.cpp:parse_rpak` |
| File confinement is a string-prefix check; `FindFile` bypasses it; `File.delete` can `remove_all` the root | `src/natives/natives_boot.cpp`, `src/natives/natives_io.cpp` |
| TREE integer overflow is undefined behaviour; the value dispatcher has no remainder case; `do_binop` computes integer arithmetic through float, losing precision above 2^24 | `src/jvm/tree_eval.cpp:tree_eval`, `src/jvm/tree_eval.cpp:do_binop` |
| Boot enters the Splash section again after Java already entered it | `src/boot/boot.cpp:boot_game` (`change_active_section` instead of `adopt_actual`) |
| Wall collision, impact damage and mileage run only for the player-controlled car with an engine | `src/resources/part_runtime.cpp:advance_chassis_tick` |
| Retired OSD widgets stay in the list and stay rooted | `src/ui/osd_registry.cpp:osd_visit_gc_roots` |
| Any pad binding disables every XInput fallback axis | `src/platform/host_input.cpp` |
| Root-scoped caches survive a game-root change | `src/resources/script_cfg.cpp:frontend_scripts`, `src/resources/scene_instances.cpp` |
| `shaders/varying.def.sc` declares `v_worldPos` twice | `shaders/varying.def.sc` |
| The `present` and `hiz` programs never link: `fs_present` and `fs_hiz` read only `v_texcoord0` but are paired with `vs_mesh`, so the offscreen scene target and the HiZ pyramid are skipped. Startup logs `[bgfx] program … failed to link` | `src/render/bgfx_lib_backend.cpp:LibBgfxBackend::open` |

Fixed on `main`: `System.arraycopy` overflow, PTX bounds, dyno session
lifetime and sampling, and the `Vehicle.getHorn` / `hasCrime` registrations.

## 11. Open issues: verified starting points

These are entry points, not fixes. Each needs its own reproduction.

- **#15 HUD too small** (PR #20 open). Every stock HUD element comes from the
  table in `src/render/bgfx_osd.cpp:draw_vehicle_hud`. Speed, gear and
  mileage are drawn left-aligned at their anchor, so centring needs the
  string width. CFG `osd_*` overrides take separate branches.
- **#16 car drives through trees.** Placement CFG `body` lines are parsed by
  `parse_part_config` but dropped by `ground_prop_renders`, and props never
  enter `scene_instances_prepare_physics`. `PartBody` keeps only `.scx`
  collision paths, so the `box` dimensions of stock tree trunks (half-extents
  about 0.05 × 3 × 0.05 m) are discarded (`include/slrr/part_config.hpp:PartBody`).
  A 0.1 m trunk can slip between the four chassis probe spheres, so a fix
  needs a volume test. The owner also points at the seven missing
  `PhysicsRef` natives and the `setActiveCollision` no-op. SLRR_rev
  implements box bodies with a full box-versus-box test
  (`SLRR_rev:src/engine/map-collision.ts:MapCollision`) and
  `setActiveCollision`.
- **#17 "Repair car" does not repair.** Stock `Part.repair()` raises wear and
  tear only up to `repair_max_wear` / `repair_max_tear` (0.95 in the MWM
  `Part.java`), then sends the native command `"repair"`
  (`GAME:parts/scripts/src/Part.java:repair`). `part_command` has no
  `repair` verb, and deformation is restored only when tear reaches 1, so
  nothing visual is reset (`src/resources/part_runtime.cpp:part_command`).
  SLRR_rev implements the verb (`SLRR_rev:src/runtime/part-native.ts:repairDamage`).
- **#18 city performance.** The reported profile is CPU-bound. Each frame
  collects instances for the main view plus `reflection_faces` cube faces
  (default 1); the map shadow re-collects when the camera moves at least
  8 m or the sun changes. Every collection walks the GameRef tree and
  recomputes each parent chain with an allocation
  (`src/resources/scene_instances.cpp:chain_matrix`). Alpha-cutout trees are
  never instanced (`prop_mesh_batchable`). Other per-frame costs:
  `gameref_find_live` is a linear scan, `native_has_alias` runs on every
  Java invoke, and every instance-field store calls `osd_on_field_write`.
  Measure before changing anything.
- **#19 pedestrians on the road.** The crowd already follows `city.walk`.
  Candidates: the OpenSLRR-only inner-edge guess in `zone_point`, junction
  transitions through polygons that include the carriageway
  (`src/resources/pedestrian_lines.cpp:constrained_transition`), and heights
  taken from the walk data. The road network parser exists but is unused.
  Compare the transitions with SLRR_rev's
  (`SLRR_rev:src/engine/pedestrian-transition.ts`).
  Placement through the stock natives needs the missing
  `pedestrianDistance` and `removePedestrianType`.

## 12. Self-check

An agent that has absorbed this file should answer these without guessing.
Each answer names its evidence.

1. *Which game build does OpenSLRR target, and is it enforced?* SLRR 2.2.1
   MWM. It is not detected: the host's `buildNumber` binding returns a
   constant 606, so the boot check only proves the binding exists (§2).
2. *A texture shows white on one car. What do you check before the decoder?*
   The owning pack, the dependency slot, `resolve_embedded_id` with the
   owner's pack, the texture order, then decode and upload (§5.4).
3. *How is a host runtime RID built?* `(pack_id << 16) | (local & 0xffff)`
   (`include/slrr/rpk_index.hpp:make_full_id`).
4. *A native body exists. Is the feature done?* No. It may be a fixed value,
   partial or unvalidated; check [NATIVE_AUDIT.md](NATIVE_AUDIT.md) (§8).
5. *Can a native keep an `Object*` across `Thread.sleep`?* Only under
   `Jvm::RootScope` or a registered root provider (§6).
6. *Does doubling the frame rate double the simulation speed?* No: fixed
   60 Hz host ticks with two 120 Hz substeps, at most four ticks per frame.
7. *Why does a fresh clone fail to configure?* `CMakeLists.txt` needs the
   untracked, gitignored `tools/compile_game_sources.cpp` (§3).
8. *Why do cars pass through Valo City trees?* Placement bodies are dropped
   and props are not in the collision index (§11, #16).
9. *Why does garage repair leave dents?* There is no native `repair` verb
   and stock repair caps tear at 0.95 (§11, #17).
10. *Where does the city's frame time go?* Unmeasured. Repeated scene walks
    with chain-matrix allocation are the first suspects (§11, #18).
11. *Do pedestrians follow a road graph?* They follow `city.walk` sidewalk
    corridors; the TRC road network is parsed by unused code (§5.7).
12. *Is `GameRef.setActiveCollision` implemented?* No, it is an explicit
    `native_noop` (`src/natives/natives_gameref.cpp:register_gameref_natives`).
13. *Which tyre model drives the car?* `src/physics/tire_model.cpp:tire_forces`,
    not `sl2_pacejka.cpp`.
14. *Can SLRR_rev's EXE addresses be used for the MWM binary?* No. They
    belong to Steam build 940. SLRR_rev documents three EXE hashes, and the
    MWM binary is a fourth.
15. *Where does a shipped Java change go?* `patch/`, with its source and an
    explanation, compiled into the right game tree.
16. *How many music sets does MWM have?* Four; build 940's six do not apply.
17. *A script call does nothing and nothing crashes. Where do you look?*
    stderr, for `native/empty TREE without host binding`: outside Init an
    unbound native returns void instead of failing (§5.3).

## 13. Keeping this file true

- Run `python scripts/check_knowledge_refs.py` after changing code this file
  cites. With the stock sources available, add
  `--external GAME=<folder containing sl, system and parts>` to also check the
  stock-script references.
- When a contract changes, update [ENGINE_CONTRACT.md](ENGINE_CONTRACT.md)
  first, then this summary.
- Move items out of §9 and §10 when they are fixed, in the same change.
