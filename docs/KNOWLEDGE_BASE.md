| `FindFile` and File share canonical physical-root confinement (#84). Enumeration alone may address the root; leading separators remain game-relative, parent traversal and external junctions are rejected, and internal junctions are allowed. File.delete remains nonrecursive (#68, #71). Invalid SDAT writes preserve alignment (#70); see [SAVE_IO_FIXES.md](SAVE_IO_FIXES.md) | `src/natives/natives_boot.cpp:under_game`, `src/natives/natives_boot.cpp:findfile_start`, `src/natives/natives_io.cpp:register_io_natives` |

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
- `compile_game_sources` is built from the tracked
  `tools/compile_game_sources.cpp`, explicitly allowed by `.gitignore`.
  The target was added in `af0dc9c` without its source, not deleted later;
  the older `/tools/` ignore rule hid it. The restored CLI delegates to
  `script_compile_all` rather than using the former local no-op stubs.
  Build with `cmake --build build --config Release --target compile_game_sources`,
  then run `build/Release/compile_game_sources.exe <game-root> <directory>`.
  The directory is relative to the root. Use a copied game root: compilation
  writes class files and retains first originals as `.openslrr.bak`.
  Exit codes: 0 for successful compilation, 1 for failures or no source units,
  2 for incorrect arguments.
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
- Performance flags: `--perf-log CSV|-` prints one `[perf]` line per second
  (frame time, scripts, physics, scene collection, shadows, reflections,
  world and OSD submission, present wait, bgfx render-thread and GPU time,
  draws) and appends CSV rows (`src/platform/frame_profiler.cpp`).
  `--no-vsync` and `--debug-overlay` set the Ctrl+F12 panel at startup.
  The repeatable city run is
  `--spawn-car cars/racers/Baiern.rpk:0x6 --bench-map ValoCity --bench-drive 40 --bench-time 13`:
  it starts the MapBox track from the garage, settles the car for two
  simulated seconds, then holds full throttle with the wheel straight at a
  fixed 1/60 s step per frame, so every build covers the same street frame
  by frame and each CSV row is one simulated second. `--bench-shots DIR`
  writes window captures every 10 simulated seconds
  (`src/boot/boot.cpp:boot_game`).
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
| Native registry | `src/natives/native_registry.cpp:alternate_descriptor`, `src/natives/natives_internal.hpp:native_noop` | `class#method#descriptor` table |
| Natives | `src/natives/natives_boot.cpp:register_boot_natives` and the `natives_*.cpp` files | Script API adapters |
| Resources | `src/resources/rpk_index.cpp:resolve_embedded_id`, `src/resources/rpk_index.cpp:model_for_resource` | RPK packs, RIDs, RSD properties |
| Meshes and textures | `src/resources/scx_mesh.cpp:load_scx_geometry`, `src/resources/ptx_image.cpp:load_texture_file`, `src/resources/dds_image.cpp` | SCX v3/v4, PTX, DDS, PNG/TGA |
| Scene | `src/resources/scene_instances.cpp:scene_instances_collect`, `src/resources/scene_instances.cpp:scene_instances_ground_map` | Render instances, maps, placements, collision index |
| Part CFG | `src/resources/part_config.cpp:parse_part_config` | Slots, wheels, renders, bodies, HUD `osd_*` lines |
| Parts and cars | `src/resources/part_runtime.cpp:part_create`, `src/resources/part_runtime.cpp:part_command`, `src/resources/part_runtime.cpp:part_physics_advance` | Part/Chassis/Vehicle natives, assembly, damage, pedestrians, HUD state (about 6,500 lines) |
| Physics | `src/physics/vehicle_dynamics.cpp`, `src/physics/tire_model.cpp`, `src/physics/dyno_model.cpp`, `src/physics/deformation_cage.cpp`, `src/physics/mass_properties.cpp`, `src/physics/wheel_mount.cpp` | Rigid body, tyres, dyno, deformation, assembled mass properties, wheel mount failure |
| Pedestrians | `src/resources/pedestrian_lines.cpp`, `src/formats/walk.cpp` | Walkers in the `city.walk` sidewalk rows, after the original engine |
| Characters | `src/resources/character.cpp`, `src/resources/skeleton.cpp`, `src/formats/matrixdata.cpp` | Skeletons, clips, skinning, Animation |
| Renderer | `src/render/bgfx_lib_backend.cpp:flush`, `src/render/bgfx_lighting.cpp:draw_instance`, `src/render/bgfx_shadows.cpp`, `src/render/bgfx_osd.cpp:draw_osd` | Frame, materials, shadows, OSD and HUD |
| UI | `src/ui/osd_registry.cpp:osd_add_widget`, `src/ui/osd_registry.cpp:osd_on_field_write` | OSD widgets, hit-testing, modal state |
| Map ambience | `src/resources/scene_ambience.cpp:scene_ambience_attach`, `src/resources/scene_ambience.cpp:scene_ambience_tick` | `sfx_loop`/`sfx_random` lines of map placements |
| Platform | `src/platform/host_input.cpp`, `src/platform/host_sfx.cpp`, `src/platform/host_music.cpp`, `src/platform/video_fmv.cpp`, `src/platform/host_game_options.cpp` | Input, audio, FMV, options |
| Worker pool | `src/platform/job_system.cpp:jobs_parallel_for` | One worker per logical processor for loading work off the host thread (§6) |

Code that exists but is not used:

- `src/formats/trc.cpp` (road network) and `src/formats/rest.cpp` parse
  their formats but have no caller.
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
   `.rpk` under the root (`src/resources/rpk_index.cpp:open_all`, which reads
   the packs on the worker pool and numbers them in directory order), checks
   the build number, opens the window and plays the intros.
3. `src/boot/boot.cpp:start_init` runs `Init.<init>(I)` with halting on the
   first unbound native.
4. The main loop repeats: window pump, section render sync,
   `mainloop_frame`, GC safe point, then a `SwitchToThread` yield (a
   `Sleep(1)` cost about 1.5 ms per frame).

### 5.2 Frame order

`src/boot/mainloop.cpp:mainloop_frame` runs, in order: resource loading pump,
music, options update (Config re-read every 100 ms, debounced save), sun time,
`script_threads_tick`, section adoption, async message pump (only when the
GameType thread is not alive), controller and hotkeys,
`script_callbacks_tick`, world camera, `dispatch_due_timers`, credits and
loading-screen special cases, the fallback clock, mechanic inventory,
`part_physics_advance`, `scene_ambience_tick`, camera follow, mechanic
pointer, `render.flush()` and the quit check. Do not move an event delivery
or add a second pump for the same port.

Inside `src/resources/part_runtime.cpp:part_physics_advance`:
`scene_instances_prepare_physics`, then N chassis ticks, engine audio, hinged
panels (`src/resources/part_runtime.cpp:advance_flaps`), steering wheels,
driver pose and pedestrians.

Inside `src/render/bgfx_lib_backend.cpp:flush`: camera, `sync_instances`,
static shadow, car depth, reflections, sky, dynamic world, garage shell,
flare, FMV, OSD and HUD, previews, ImGui, `bgfx::frame`.

### 5.3 Native call resolution

- Natives are registered as `register_method("java.pkg.Class", "name",
  "(args)Ret", fn)`. Descriptors use dotted class names. A duplicate
  registration throws. `descriptor_match` treats a missing return type as
  `V` (`src/natives/native_registry.cpp:alternate_descriptor`).
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
   index. It runs only for a released chassis whose `drive.torque` is set
   or whose engine flew out (`engine_lost`), and `configure_drive` sets the
   torque only for the player-controlled car with gear ratios, a dyno and a
   positive maximum RPM. Other cars get no wall
   collision, impact damage or mileage
   (`src/resources/part_runtime.cpp:advance_chassis_tick`,
   `src/resources/part_runtime.cpp:configure_drive`).
5. Impacts feed `damage_chassis` and the XPBD cage
   (`src/physics/deformation_cage.cpp`). `dynamics_sync` publishes poses.
6. Mass, centre of mass and inertia come from the installed parts' CFG bodies,
   each spread over its own box, sphere or closed phys hull
   (`src/resources/part_runtime.cpp:assembly_mass_properties`,
   `src/physics/mass_properties.cpp:mesh_mass`). Crush cages are generated over
   the render shell instead (`src/resources/part_runtime.cpp:panel_contact`), so
   the phys bodies are mass and collision, not deformation zones.
7. A corner whose mount condition reaches zero loses its wheel slots' parts as
   loose bodies: impact energy at once
   (`src/resources/part_runtime.cpp:damage_wheel_mounts`), hardpoint travel once
   the chassis cage has settled (`src/resources/part_runtime.cpp:settle_wheel_mounts`),
   both detached by `src/resources/part_runtime.cpp:detach_panel`. The refreshed
   chassis rests that corner on its remaining hub hardware.
8. A big impact near the engine frees the `dynodata` child and everything on it
   as one loose body with its pre-impact velocity
   (`src/resources/part_runtime.cpp:damage_engine_mount`,
   `src/physics/wheel_mount.cpp:pre_contact_velocity`). Without a dyno the car
   has no drive torque; `engine_lost` keeps `collide_chassis` running for it.
9. A part on a CFG `flap` slot (doors, hood, trunk) is a latched hinged plate
   (`src/physics/flap_hinge.cpp:flap_step`). `Part.flap`/`flap_toggle` drive
   it, `damage_chassis` hands each impact to
   `src/resources/part_runtime.cpp:flap_impact`, which can release the latch
   and swing it, and `advance_flaps` integrates it at 240 Hz on frame time
   with gravity, the carrier's velocity change and wind, then re-places it
   through `place_child`. On a moving car an unlatched panel's box meets the
   static scene and native objects
   (`src/resources/part_runtime.cpp:flap_world_contact`, response in
   `src/physics/flap_hinge.cpp:flap_contact`): it is swung out of the way or,
   from 4.5 m/s, torn off through `detach_panel` after the loop.
   ENGINE_CONTRACT §10 has the contract.
10. After the ticks of a frame, in the Adaptive framerate mode
   (`src/platform/host_game_options.cpp:host_framerate_mode`), `publish_render_poses`
   blends each car from its pose before the last tick to the published one, by
   the unsimulated fraction of a tick
   (`src/resources/part_runtime.cpp:publish_render_poses`). Classic (60 Hz)
   draws the last tick.
   The renderer, chase and cockpit cameras and car shadows draw that pose;
   scripts and physics still read the ticked one
   (`src/resources/scene_instances.cpp:chain_matrix`).

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
- Placements (trees, lamps, buildings) become render props in 64 m cells
  (`src/resources/scene_instances.cpp:scene_instances_add_ground_props`).
  Every placement of a mesh without blended sections is instanced
  (`src/resources/scene_instances.cpp:prop_mesh_batchable`); `fs_mesh` mixes
  the per-instance colour into ColorMixTex (paint-masked) facades,
  and `scene_instances_collect_batches` merges one mesh's visible placements
  from all cells into one batch. The rest are drawn one by one.
- The GPU cache retains a shared source plus draw metadata, not a second set
  of CPU vertex/index/skin arrays. Ground/map-prop sections with identical
  materials use transient reordered upload indices
  (`src/render/scene_mesh_data.hpp:prepare_scene_mesh`); collision and picking
  keep the authoritative source's original sections and indices.
- During `flush`, collections of the active world share one snapshot
  (`src/resources/scene_instances.cpp:scene_instances_frame_cache`).
- `ground_prop_config` retains the full native CFG. Box, sphere and SCX bodies
  are cached per type, transformed by placement/body pose, merged into a
  spatially indexed collision mesh per map and added to the exact shape index.
  They are independent of render variant, LOD, visibility and culling.
- If every collision-enabled body has positive mass, the placement is drawn as
  an ordinary prop and recorded as dormant. `collide_chassis` queues dormant
  placements inside the padded chassis bounds
  (`src/resources/scene_instances.cpp:scene_instances_wake_dynamics`); after the
  vehicle loop each becomes a loose native body (`part_add_dynamic_native`)
  with its full collision mesh, authored mass and mass-weighted body-pose
  centre, and its static instances are hidden
  (`src/resources/scene_instances.cpp:scene_instances_promote_dynamics`). It
  sleeps until car contact, wakes for equal/opposite car impulses, and
  publishes every render slot from the ground-shell frame
  (`scene_instances_ground_frame`). Dynamic placements are excluded from both
  static ground caches and floor-mesh collection; mixed-mass placements stay
  static. Creating every such prop at load (about 4,000 in Valo City) cost
  about 400 ms of physics per frame. A promoted prop at rest more than 40 m
  beyond car reach becomes scenery again at its settled pose
  (`src/resources/scene_instances.cpp:scene_instances_settle_dynamic`), as
  does one whose world is left; a lost one (fallen off the map, still moving
  after 10 s out of reach, pushed beyond its 64 m cell) returns to its map
  pose. The GroundRef a prop is parented to comes from the shell that woke it:
  each city visit builds a new GroundRef while the prop map stays cached.
- `PhysicsRef.setStatic(0)` releases a script body to the free-body solver
  (`src/resources/part_runtime.cpp:advance_free_bodies`), which reuses the
  loose-body car and scene contacts and publishes pose and velocities.
- Trigger volumes (`GameRef.RID_TRIGGER`, `java.game.Trigger`) are parsed by
  `src/resources/trigger_volume.cpp:trigger_volume_parse` and raise
  `EVENT_TRIGGER_ON`/`OFF` with the car's id from
  `src/resources/scene_triggers.cpp:scene_triggers_tick`.
- Besides the four corner sphere sweeps, the chassis box is tested against
  native boxes/spheres and against the wall triangles of every static mesh
  (`src/resources/scene_instances.cpp:scene_instances_contact_walls`, kernel
  `src/physics/scene_collision.cpp:overlap_triangle`), so corners, fence ends
  and thin trunks between the probes still stop the car. Both use the chassis
  box above its lowest 30 %.
- `scene_instances_prepare_physics` rebuilds the collision index from GameRef
  render nodes once per physics frame. Floor and sweep queries are
  `scene_instances_sample_floor` and `scene_instances_sweep_sphere`.
- Placements whose CFG has `sfx_loop`/`sfx_random` lines become ambient
  emitters of their ground shell
  (`src/resources/scene_ambience.cpp:scene_ambience_attach`). The world camera
  is the listener; the shell must be in the active world
  (`src/resources/scene_ambience.cpp:scene_ambience_tick`). Rules and
  executable addresses are in
  [HOST_NATIVE_UTILITIES.md](HOST_NATIVE_UTILITIES.md#map-ambient-sounds).
- The per-instance seed and variant choice match the reverse-engineered
  rule: `trunc(x+y+z) & 0x7fffffff` when the seventh parameter is missing,
  then `rendertypes[seed % n]` and `colors[seed % n]`.

### 5.7 Pedestrians

- `city_walk` loads the hard-coded `maps/city/ai/city.walk`; walkers run on a
  worker thread (`src/resources/part_runtime.cpp:PedSim`,
  `src/resources/part_runtime.cpp:tick_pedestrians`).
- The row layout is the original reader's (0x00586580,
  `src/formats/walk.cpp:walk_read`): a header of origin, floor slopes and
  height, then edges. An edge with id -1 is a wall (kind 21) or a sidewalk
  run (kind 23, a Hermite spline); any other id is a gate into that row.
- Walkers follow the original model (tick 0x00589cb0,
  `src/resources/pedestrian_lines.cpp:PedestrianLines::step`): free agents
  with a heading, 1 m radius and 1 m/s. Walls and both runs repel them and
  push them back to 1 m; the four strongest influences steer; they leave a
  row only through a gate. Stock gate links form closed block rings, so no
  route crosses a carriageway.
- Spawns use the original formula per row (corridor: 10-90 % across the
  runs; other rows: a fan triangle). The headcount is `density × row area`
  inside 400 m, with no fixed cap. Height comes from the walk data.
- Pedestrians are skinned and animated through the same Animation code the
  natives use (`src/resources/character.cpp:character_bind_animation`); the
  clip advances with the distance walked, as in the original (0x00587820).

### 5.8 HUD and OSD

- OSD coordinates are −1..1 with Y down. `osd_set_framebuffer` rescales icon
  widths for the aspect ratio.
- `src/render/bgfx_osd.cpp:draw_vehicle_hud` draws the stock instrument
  layout from a hard-coded table, scaled by `stock_scale = 0.5625 / (h/w)`.
  CFG `osd_*` lines (`include/slrr/part_config.hpp:HudSlot`) override it.
- HUD values come from `src/resources/part_runtime.cpp:part_vehicle_hud`:
  RPM, km/h, mileage, nitro, turbo, gear, and the damage gauge's parts.
- Damage gauge (`osd_damage`, stock font `cars\meshes\osdfont.scx`): glyph
  `A` is the car outline, `E`/`L`/`T` the drivetrain, `O h l r p q t u v` the
  body and `a b c d` the wheels, in one coordinate system with the front up.
  `src/resources/part_runtime.cpp:hud_damage_parts` lists the installed
  engine, body and wheel parts with their top-down centre, normalised to the
  chassis renders, and their condition `wear × tear`
  (`GAME:parts/scripts/src/Part.java:getConditionNoAttach`). The renderer
  gives each part to the smallest blip quad of its kind holding that centre
  (wheels by corner) and draws a blip in its worst part's colour, or not at
  all when it holds no part (`src/render/bgfx_osd.cpp:draw_vehicle_hud`).

### 5.9 Options and saves

- Options mirror `java.util.Config` and save to `save/game/options` through
  the stock Java `Config.saveConfig` after a 500 ms debounce, plus a final
  save at shutdown (`src/platform/host_game_options.cpp:host_game_options_update`).
- Enhanced options (MSAA, reflections, clouds, sunlight, local lights) are
  session-only and exposed through `OptionsDialog.get/setRendererOption`
  (`src/natives/natives_script.cpp:register_script_natives`).
- File saves now write versioned typed JSON at the existing paths, with SDAT
  type 1/type 2 and raw-stream read fallback. Resource paths remount independently
  of session pack order; scripts keep binary cross-type read semantics. See
  ENGINE_CONTRACT §15 (`src/natives/natives_io.cpp:json_frame`,
  `src/natives/natives_io.cpp:json_take_payload`,
  `src/natives/natives_io.cpp:sdat_take_payload`).

### 5.10 Threads and the worker pool

Read this before adding parallel work. The contract is
[ENGINE_CONTRACT.md §6](ENGINE_CONTRACT.md#host-threads-and-the-worker-pool);
this section is the working guide.

**Threads that exist.**

| Thread | Owner | Runs |
| --- | --- | --- |
| Host thread | `src/boot/boot.cpp:boot_game` | VM and script fibers, frame pump, physics, scene collection, bgfx submission, every GameRef and cache mutation |
| bgfx render thread | bgfx (`src/render/bgfx_lib_backend.cpp:open`) | Graphics API calls for the frame the host submitted |
| Worker pool, one per logical CPU minus one | `src/platform/job_system.cpp:jobs_parallel_for` | Data-parallel batches started by the host thread |
| Exterior map parser | `src/natives/natives_resref.cpp:spawn_ground_shell` (`std::async`) | `scene_instances_ground_map` for the next chained map file |
| Pedestrian stepper | `src/resources/part_runtime.cpp:PedSim` | Walker steering on copies of the walker records |
| Audio and video | XAudio2, Media Foundation, VLC | Their own decode and output threads |

**API** (`include/slrr/job_system.hpp`).

- `jobs_parallel_for(count, body)` calls `body(i)` once per index and
  returns when all have finished. The calling thread takes part. The first
  exception is rethrown after every index has run.
- It runs serially, on the calling thread, when `count` is 1, inside a job,
  when the pool has no workers (one CPU, or after `jobs_shutdown`), or when
  another thread's batch holds the pool. Never wait for a batch from inside a
  job.
- `jobs_concurrency()` gives the thread count, caller included. It follows
  the process affinity mask.
- `jobs_inside()` is true inside a job. `index_floor` uses it to pick its
  serial path (`src/resources/scene_instances.cpp:index_floor`).
- `jobs_printf` holds a job's log lines and prints them in index order when
  the batch ends. `JobLogCapture` holds the lines of a task that runs
  outside the pool, such as a `std::async`, for its caller to print. Use one
  of them for any log line that can come from a job.

**Pattern.** Every user follows prepare, run, publish:

1. On the host thread, collect the inputs and size the results: one slot per
   index, or a cache entry reserved before the batch, as
   `src/resources/scene_instances.cpp:scene_instances_prepare_physics` does.
2. Run the batch. A body reads its inputs and writes only its own slot.
3. On the host thread, publish the slots in index order into caches,
   registries and logs. Pack ids, cache contents and log order then do not
   depend on the thread count.

When results must be appended, keep the serial order: split the work into
contiguous runs and append the runs in order
(`src/resources/scene_instances.cpp:index_floor`). The same applies to
offsets computed before a parallel copy
(`src/resources/scene_instances.cpp:merge_static_meshes`).

**What a job may touch.**

| Safe to call or read from a job | Host thread only |
| --- | --- |
| `RpkIndex` const methods: its payload and RSD caches lock their own mutexes (`src/resources/rpk_index.cpp:read_payload`) | The VM, `Object`, `Value`, `Jvm::RootScope`, natives |
| `load_scx_geometry`, `load_texture_file`, `parse_part_config`, `scene_instances_ground_map`, `src/resources/scene_instances.cpp:load_geometry`, `src/resources/scene_instances.cpp:build_native_collision` | GameRef state (`gameref_get`, matrices, parents) |
| Immutable data that nothing writes during the batch | Scene caches: `g_meshes` through `geometry()`, `g_native_collision`, floor and ground caches. Preload missing meshes with `preload_meshes` first; a lookup that misses inserts. |
| | bgfx: create textures and buffers after the batch. Decode on the pool, upload on the host (`src/render/bgfx_lib_backend.cpp:prefetch_part_textures`). |

Before calling a function from a job, check it for `static` locals, globals
and lazily filled caches. `RpkIndex` keeps its payload and RSD caches behind
mutexes: payload reads from several jobs are safe but take turns, which
limits the speed-up.

**Pitfalls already met.**

- `std::mutex` is not recursive. Do not hold a lock while running bodies that
  may start another batch. `jobs_parallel_for` takes its own lock last for
  this reason.
- A `std::future` from `std::async` blocks in its destructor. Keep the
  futures in the function that owns the data they read.
- Freeing per-job containers costs time too: the cell runs of a large
  `index_floor` take a few milliseconds to destroy.
- Do not grow a buffer by one element range at a time with `reserve` of the
  exact size. It reallocates every time and made `merge_static_meshes`
  quadratic: 12.5 s of a city load.
- A prefetched result that several consumers share needs one use per
  consumer. Otherwise a failed first consumer forces the others to redo the
  work serially (`src/render/bgfx_lib_backend.cpp:load_part_textures`).

**Checking a change.**

- Standalone tests (`tests/test_job_system.cpp`, built by hand) under g++
  with `-fsanitize=thread`, and with `-fsanitize=address,undefined`.
- Determinism: compare the `[rpk]`, `[scene]` and `[scx]` log lines of a
  run on all cores with a run restricted to one core. For example, set the
  parent PowerShell's `ProcessorAffinity` before `Start-Process`, or use
  `start /affinity 1`. The lines must match, apart from the randomly chosen
  sky dome.
- Equivalence: when a parallel algorithm replaces a serial one, compare
  their outputs once in a local build (not committed), as was done for
  `index_floor` on all six city collision meshes.
- Speed: time the stage on the city benchmark (§3) on all cores and with 1,
  2 and 4 cores. `--perf-log` covers the frame. Load stages need local
  timers.
- Assets: same pack, prop, collision, mesh and texture counts as `main`, and
  matching start-of-drive window captures (`--bench-shots`) when the same
  sky dome is chosen.

**Where to build next.** [MILESTONES.md M7](MILESTONES.md#m7--multicore-frame)
lists the planned steps for the frame loop: measure where the CPU limits,
cull scene collection in parallel, record draws with bgfx encoders, run
physics after PR #48, and optional pipelining.

## 6. Invariants and pitfalls

VM and lifetime:

- Script threads are cooperative fibers on the host thread, never parallel
  workers. Native C++ work inside a fiber is not preempted.
- Loading work that only reads files runs on the worker pool
  (`src/platform/job_system.cpp:jobs_parallel_for`): pack indexing, mesh
  loading (`src/resources/scene_instances.cpp:preload_meshes`, also called
  by `collect_root` for the meshes new in a frame), a map's object types
  (`src/resources/scene_instances.cpp:build_native_collision`), placement
  collision bakes and their merge, prop batching, static collision that
  appeared or moved
  (`src/resources/scene_instances.cpp:scene_instances_prepare_physics`), the
  floor index of large meshes (`src/resources/scene_instances.cpp:index_floor`)
  and texture decoding
  (`src/render/bgfx_lib_backend.cpp:prefetch_part_textures`). A job writes
  only its own slot of a result the caller prepared and never touches the
  VM, GameRef state, other cache entries or bgfx; the host thread publishes
  the results. Steady per-frame work stays on the host thread. §5.10 is the
  guide for adding parallel work.
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
- Float `!=` compares by value, the negation of `==`
  (`src/jvm/tree_eval.cpp:tree_eval`). It used to read both floats as
  integer 0, which made every float inequality false; `Tyre.SetInflation`
  then never applied a pressure and every tyre sat at its 0.5 bar floor. The
  MWM key checks (`Check(key) != Math.sqrt(key)+3.75` in `GameLogic`,
  `MainMenu` and others) compute the same float on both sides and still
  pass: boot to the main menu and the garage was checked in game.
- `System.arraycopy` grows the destination on purpose
  (`src/natives/natives_internal.hpp:native_arraycopy`).

Resources and data:

- Keep repeated RSD `texture` keys in order; never deduplicate them.
- The high bit of a RID can be set while scripts hold it as `int32_t`.
  Never serialize a native pointer into a RID or script integer.
- A CFG `box` body holds half-extents. `PhysicsRef.createBox` takes full
  sizes. Native CFG bodies are authoritative: an object without a `body`, or
  with `nocollision`, gets no collision from its render mesh. Ground shells
  and legacy RenderRefs keep their separate collision/override path
  (`src/resources/scene_instances.cpp:scene_instances_prepare_physics`).
- A `linked a [b] [parentSlot]` rule: the first matching rule wins; do not
  sort them. LOD renders are alternatives, not layers.
- A two-anchor linked render with a skinned bone00/bone01 rig is posed, not
  scaled: bone00 on anchor `a`, bone01 on anchor `b`
  (`src/resources/part_runtime.cpp:relink_one`,
  `src/resources/character.cpp:character_place_bone`). The stock spring and
  shock absorber renders declare no `skeleton`; their rig is the BON next to
  the SCX, taken only when it names bone00 and bone01
  (`src/resources/character.cpp:render_skeleton`). Only a render
  without that rig, such as a CV shaft, is stretched
  (`src/resources/part_runtime.cpp:span_between`).

Physics:

- Physics runs at fixed steps; a higher frame rate must not speed up
  simulated time. A missing ground contact means air, not a floor at Y = 0.
- The physics delta is clamped to 0.1 s and scaled by the time warp
  (`src/jvm/jvm.cpp:advance_sim_time`), and at most four host ticks run per
  frame, so below 15 FPS the simulation slows down.
- A CFG `body` line is mass and collision; it is not a crush zone. Do not size
  a deformation cage from the phys SCX bodies, and do not take inertia from the
  render bounds: 815 of the 818 stock racer phys hulls with mass are closed
  solids, and the mass properties integrate them.
- Repairs must restore deformation, damage-associated slot state and wheel
  damage together. The native `repair` command does this locally for each part
  (`src/resources/part_runtime.cpp:repair_part`); stock Java covers the normal
  tree walk, with a native fallback only for detached assembly children omitted
  from `attachedParts`. A standalone `setTear` still clears deformation only when
  Java sets tear to at least 1 (`src/resources/part_runtime.cpp:restore_deformation`).

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
| Ambient start range | 250 m beyond the CFG radius | `src/resources/scene_ambience.cpp:scene_ambience_audible` |
| Chassis wall probe | four spheres of 0.18 m | `src/resources/part_runtime.cpp:collide_chassis` |
| Tyre friction units | script friction / 1.68 = peak mu | `include/slrr/tire_model.hpp:kFrictionUnits` |
| Hinged panel step / latch release / tear-off | 240 Hz / 3.5 m/s local impact speed / 4.5 m/s contact speed or 0.15 m blocked overlap | `src/resources/part_runtime.cpp:kFlapStep`, `include/slrr/flap_hinge.hpp:FlapTuning` |
| Prop cell | 64 m | `src/resources/scene_instances.cpp:kPropCellSize` |
| Shadow maps | 2048² | `src/render/bgfx_lib_backend.hpp:kShadowResolution` |
| Pedestrian radius / density scale | 400 m / ×4 | `src/resources/part_runtime.cpp:tick_pedestrians` |
| Walker body radius / speed | 1 m / 1 m/s (original) | `src/resources/pedestrian_lines.cpp:kRadius` |
| Live GameRef IDs | from `0x70000000` | `src/resources/gameref.cpp:g_next_live` |

## 8. Native coverage at a glance

From [NATIVE_AUDIT.md](NATIVE_AUDIT.md) and
`assets/progress/native-coverage.json` (12 November 2026): of 376 stock
signatures, 339 have a body, 3 are explicit no-ops and 34 are missing.
`scripts/render_native_coverage.py` recomputes these counts from `src/`;
`--list missing` prints the missing signatures.

| Missing family | Count | Blocks |
| --- | ---: | --- |
| `GroundRef` routes, traffic, water | 19 | Navigation, traffic |
| `ParticleSystem` | 9 | Smoke, sparks and other effects |
| Leftovers (`Welder`) | 3 | No stock caller found |

The three `Painter` natives that this table listed are bound since #64;
see "Garage paint booth (#64)" below.

Explicit no-ops: `System.analyze` ×3 (faithful).
`GameRef.setActiveCollision` has a body since PR #22 (§12). Two host-only
no-ops outside the stock table: `Player.<init>` and `LoadingScreen.<init>`.

Partial bodies that matter for upcoming work: `GameRef.setState` applies to
cars only, `GroundRef.getNearestCross` returns null,
`RenderRef.plotRoute` returns 0, `GroundRef.setFog` stores values nobody
reads, and `Part.setSfxLoopParams` stores values without applying them.

## 9. Where the documentation disagrees with the code

| Statement | Code reality |
| --- | --- |
| HOST_NATIVE_UTILITIES: engine-loop audio not yet implemented | `update_engine_audio` runs every physics frame |
| Stale comments: "no green-thread budget yet" in `src/boot/mainloop.cpp`, "240 Hz tick" in `src/resources/scene_instances.cpp` | Fibers have budgets, physics is 120/60 Hz |

## 10. Open risk register

Risks found by the PR #3 source review (baseline `60d7d71`) and rechecked on
`main`. "Open" means the code still has the pattern; none was reproduced at
run time.

| Risk | Where |
| --- | --- |
| DirectShow dynamic media-format renegotiation is not supported; malformed/short RGB32 frames are rejected | `src/platform/video_fmv.cpp` (`grab_frame`) |
| `FindFile` still bypasses shared path confinement. The File native path validator now checks canonical components, rejects the root and accepts dots within names; File.delete is nonrecursive (#68, #71). Invalid SDAT resource writes preserve field alignment with a null placeholder (#70); see [SAVE_IO_FIXES.md](SAVE_IO_FIXES.md) | `src/natives/natives_boot.cpp:under_game`, `src/natives/natives_io.cpp:register_io_natives` |
| Boot adopts the section already entered by Java Init without replaying lifecycle (#76). Windowed startup retains a timer-only reset so startup work cannot consume the splash display interval. | `src/boot/boot.cpp:boot_game`, `src/boot/mainloop.cpp:mainloop_adopt_section` |
| Released cars retain static-world contacts without a driver/engine. Shared chassis OBB pairs and all-car movable-body contacts now use impulse-aware damage/detachment; full Bot recovery/audio/seating remain partial. See [traffic/race audit](TRAFFIC_RACE_AUDIT.md). | `src/resources/part_runtime.cpp:advance_chassis_tick`, `src/resources/part_runtime.cpp:advance_loose_panels`, `src/resources/part_runtime.cpp:advance_free_bodies` |
| Gamepad fallback is now per driving axis and unclaimed physical channel; hardware validation remains separate | `src/platform/host_input.cpp`, `src/platform/host_input_mapping.hpp` |
| The unused offscreen scene target, HiZ pyramid/readback and legacy projected car shadows are removed (#105). World rendering uses the swap chain; the retained projected-size filter does not perform occlusion. Shader creation releases caller handles on success and failure. | `src/render/bgfx_lib_backend.cpp:LibBgfxBackend::open`, `src/render/bgfx_program.hpp:create_owned_program` |

Fixed on `main`: `System.arraycopy` overflow, PTX bounds, dyno session
lifetime and sampling, and the `Vehicle.getHorn` / `hasCrime` registrations.

## 11. Open issues: verified starting points

These are entry points, not fixes. Each needs its own reproduction.

- **#15 HUD too small** (PR #20 open). Every stock HUD element comes from the
  table in `src/render/bgfx_osd.cpp:draw_vehicle_hud`. Speed, gear and
  mileage are drawn left-aligned at their anchor, so centring needs the
  string width. CFG `osd_*` overrides take separate branches.
- **#16 car drives through trees (PR #22).** `ground_prop_config` now keeps
  native CFG bodies. Placements use cached triangle and exact box/sphere
  collision paths, including a full chassis volume test for thin trunks.
  `PhysicsRef` has its seven missing signatures, and `setActiveCollision`
  queues activation at the physics boundary. The oriented-box behavior is
  based on `SLRR_rev:src/engine/map-collision.ts:MapCollision`. Entirely
  positive-mass placements also use the loose-body solver with mass-aware car
  impulses; they sleep until contact and are absent from static caches.
  `setStatic(0)` PhysicsRef bodies use the same solver, and trigger volumes
  raise `EVENT_TRIGGER_ON`/`OFF` for cars.
- **#17 "Repair car" does not repair.** Stock `Part.repair()` raises wear and
  tear only up to `repair_max_wear` / `repair_max_tear` (0.95 in the MWM
  `Part.java`), then sends the native command `"repair"`
  (`GAME:parts/scripts/src/Part.java:repair`). `part_command` now routes that
  one-shot command to `repair_part`, which restores local deformation and
  damage-associated slot/wheel state. Normal script-covered children are not
  traversed again; only detached assembly children missing from `attachedParts`
  receive the native fallback.
  SLRR_rev's corresponding implementation is
  `SLRR_rev:src/runtime/part-native.ts:repairDamage` (revision `77b0fb6`).
- **#18 city performance.** Measured with `--perf-log` and the city
  benchmark (§3) before the fix: pedestrian area sampling took about 4 ms
  per frame (`src/resources/pedestrian_lines.cpp:area_within`), the two
  Valo City ground meshes cost 1,372 draws in the camera view and again in
  the reflection face, every view re-walked the GameRef tree and re-posed
  every walker within 400 m (about 3 ms for the 600 walkers of the patched
  density), spawning tested each sample against every walker, trees were
  never instanced, and the main loop slept 1.5 ms per frame. All of these
  are addressed. Remaining per-frame costs: scene collection (about 1–2 ms),
  a 50–140 ms frame every few seconds (not yet isolated), `gameref_find_live`
  is a linear scan, `native_has_alias` runs on every Java invoke, and every
  instance-field store calls `osd_on_field_write`.
- **#19 pedestrians on the road.** Addressed by the port of the original
  walker model (§5.7). The `Animation` constructor descriptor used slashes
  and never matched, so script animations were unbound; it now uses the
  dotted form. Open: the car-flee side choice and exact accumulator roles in
  0x00586e40 are only partly decoded, and `city.rest` is still unused.
- **#38 matte cars at the car dealers.** Paint, chrome and glass read the
  dynamic cubemap only in a reflection scene. The dealers have a map and a
  script camera, but no garage flag and no followed car, so they used to
  render without any reflection. Every world camera is now a reflection
  scene, and without a followed car the probe sits on the car nearest the
  camera target (`src/render/bgfx_lib_backend.cpp:flush`). The garage
  capture also draws the preloaded garage shell; before, the paint and glass
  mirrored only the props and the clear colour
  (`src/render/bgfx_lib_backend.cpp:update_reflections`).
- **#56 single-threaded game.** Checked with the city benchmark (§3) and
  per-thread CPU sampling. Before, the host thread did nearly all the work:
  about 20 of 29 s of process CPU in a run, with at most four threads busy at
  once. A warm Valo City load took about 13 s on that thread, 12.5 s of it in
  `src/resources/scene_instances.cpp:merge_static_meshes`, which grew its
  buffer by one placement's mesh at a time. On a first launch over freshly
  copied files, serial texture decoding added about 14 s. Loading work now
  runs on the worker pool (§6) and the merge sizes its buffers once: the load takes
  about 1–2 s warm and 5 s on a first launch, with up to 20 threads busy on
  a 20-thread CPU.
  The VM, physics, scene collection and bgfx submission stay on the host
  thread by contract; in the drive they cost about 4–7 ms per frame. A
  follow-up overlapped the chained `city2` parse with `city`'s props, indexed
  large collision meshes on the pool and kept the log order serial: the warm
  load is now under 1 s.

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
7. *What caused the missing compiler-tool source error on fresh clones?*
   `af0dc9c` added the target without tracking its ignored source. The source
   is now tracked and the CLI uses the existing compiler adapter (§3).
8. *Why did cars pass through Valo City trees?* Placement bodies were dropped
   and props were not in the collision index. PR #22 retains and indexes those
   bodies and adds full-chassis contact coverage (§11, #16).
9. *How does garage repair clear dents without forcing 100% condition?* Stock
   Java applies its repair caps and recursively calls the native `repair`
   command; that command restores local deformation and slot/wheel damage
   without changing the script-selected wear/tear values (§11, #17).
10. *Where does the city's frame time go?* Unmeasured. Repeated scene walks
    with chain-matrix allocation are the first suspects (§11, #18).
11. *Do pedestrians follow a road graph?* No. They walk freely inside the
    `city.walk` sidewalk rows and change rows through gates; the TRC road
    network is parsed by unused code (§5.7).
12. *Is `GameRef.setActiveCollision` implemented?* Yes. PR #22 queues an
    idempotent request and consumes it at the physics boundary
    (`src/resources/gameref.cpp:gameref_process_collision_requests`).
13. *Which tyre model drives the car?* `src/physics/tire_model.cpp:tire_forces`,
    not `sl2_pacejka.cpp`. Its curve comes from the tyre script's `WheelRef`
    values (friction, stiffness, sliction, load capacity, `frictn_x`, patch
    angle); `configure_drive` copies them each tick
    (`src/resources/part_runtime.cpp:configure_drive`).
14. *Can SLRR_rev's EXE addresses be used for the MWM binary?* No. They
    belong to Steam build 940. SLRR_rev documents three EXE hashes, and the
    MWM binary is a fourth.
15. *Where does a shipped Java change go?* `patch/`, with its source and an
    explanation, compiled into the right game tree.
16. *How many music sets does MWM have?* Four; build 940's six do not apply.
17. *A script call does nothing and nothing crashes. Where do you look?*
    stderr, for `native/empty TREE without host binding`: outside Init an
    unbound native returns void instead of failing (§5.3).
18. *Can a worker-pool job look up a mesh with `geometry()`?* Only if the
    mesh is already loaded: a miss inserts into `g_meshes`. Preload the ids
    with `preload_meshes` on the host thread first, and publish job results
    on the host thread (§5.10).

## 13. Keeping this file true

- Run `python scripts/check_knowledge_refs.py` after changing code this file
  cites. With the stock sources available, add
  `--external GAME=<folder containing sl, system and parts>` to also check the
  stock-script references.
- When a contract changes, update [ENGINE_CONTRACT.md](ENGINE_CONTRACT.md)
  first, then this summary.
- Move items out of §9 and §10 when they are fixed, in the same change.


## 14. Garage lifetime correction (#122)

Boot no longer loads G1 meshes unconditionally into the renderer's permanent
ScenePart list and the global static collision cache. Garage.java selects its
GroundRef and stuff RenderRef from garageIndex; the normal resource/scene path
owns their lifetime. The boot copy had no world owner and was drawn whenever
the garage viewport was active, including G2/G3/ROC, and entered reflection
captures as well. Its collision triangles also survived garage changes.

The patch removes this duplicate preload rather than adding garage-name checks
or deleting cached resources still owned by a live scene. Resource decoding
continues through natives_resref and scene_instances. No Java changes.
Release x64 compiles and knowledge reference validation passes (219 references).
No in-game validation is claimed yet. Test initial G1, then use fastpromotion
for club garages and letmeroc for the ROC garage; inspect walls, floor/lift,
props and reflections for old G1 geometry. Confirm the car remains supported
by each active garage's collision geometry and that returning to G1 works.
Issue #93 / PR #121 is independent of the garage lifetime correction.


### Garage shell follow-up: obsolete exclusion removed

User testing of #122 exposed missing garage shells after removing boot G1.
spawn_ground_shell still explicitly skipped model paths containing "garage",
assuming the boot preload owned them. Remove that exclusion: the selected
GroundRef now owns its POLY/render shell and collision override, like other
maps. Keep the boot preload removed. The user reports improved city shadows
with that removal; the causal rendering mechanism has not been isolated.

Read-only integration test against local game/misc/garage.rpk: G1 (0x7a),
G2 (0x06), G3 (0xc7), ROC (0x2f) each attach and collect one shell; repeated
spawn does not duplicate it. Before a61b1ba: 0/4 pass; corrected: 4/4 pass.
Run `./scripts/run_save_io_tests.ps1 -Suite garage_shell` (requires game assets),
and add `-BaselineRef a61b1ba` for reproduction. No saves or game files changed.
Release x64 rebuilt successfully. Full visual/collision validation in G3 and
ROC remains pending; the test checks scene attachment, not GPU output.


## 14. Standalone map placements (#134)

spawn_ground_shell previously registered native placements only for secondary
grounds or primary grounds with exterior-map references. This silently excluded
standalone TestTrack and ROC, despite their placement tables being parsed.
Register placements for every attached ground shell; exterior references still
control traversal of additional maps. Existing render-id cache and attached-shell
checks prevent duplication. No Java edits or per-track special cases.

Read-only integration tests use local game assets and the production resource
index, ground decoder, shell attachment and scene collection. TestTrack reports
1413 placements / 1435 render instances; ROC 1791 / 1791. Before 933cf0a neither
registered its props (0/2 passing); corrected both register and supply resident
meshes (2/2 passing). Repeated attachment keeps the shell count unchanged.
Evidence: assets/validation/map-props. Run
`./scripts/run_save_io_tests.ps1 -Suite map_props`, optionally with
`-BaselineRef 933cf0a`. These tests require game assets; no saves are modified.
Release x64 compiled; knowledge references 219 checked, zero errors.
In-game validation remains pending: inspect poles/objects in TestTrack and ROC,
reenter the maps, and verify frame time and collisions now that props exist.
The scripted trigger/race progression issues are separate.


### Track structures casting shadows (follow-up to #134)

User confirms standalone native placements appear. The tower/stairs remain
unshadowed because they belong to the ground POLY itself, excluded wholesale
from the static shadow caster pass. Ground POLY now casts as well as receives;
vehicle, sky, skinned and light exclusions remain. Fragment receiver bias now
uses the depth gradient in shadow UV coordinates over the PCF footprint rather
than derivatives measured in screen pixels. This is intended to avoid terrain
self-shadow grids when including the ground; the cap is 0.004 normalized depth.
Potential tradeoff: excess bias can detach fine contact shadows, especially at
shallow angles. Visual validation is required before merge.

Release x64 and embedded DXBC/GLSL shaders compile. Knowledge references: 219
checked, zero errors. No GPU visual test was performed by the agent. Retest
TestTrack tower/stairs and adjacent apron for shadows/acne, then Valo buildings
and roads. These rendering checks are separate from placement integration tests.

## 14. Asset bounds checks (#91, #92)

SCX v3 validates all three face indices against the current material group's
vertex count before appending indices. Validation also runs for metadata reads;
an index referencing vertices in a later group is invalid even when the final
mesh would contain that index. Invalid geometry is marked not-ok with an error.

matrixdata_read checks declared keys against remaining bytes (eight floats per
key) before resizing times or track channels. This prevents allocation driven
by an unsupported key declaration. Existing quaternion/time/name checks remain.
These are parser corrections, not changes to deformation or animation behavior.

Release x64 build passes. Five deterministic fixtures exercise valid multi-group
SCX, an out-of-range local index, an index into a later group, truncated keys
with no channel allocation, and a valid named animation. Main 933cf0a: 2 pass /
3 fail; corrected: 5 pass / 0 fail. Evidence: assets/validation/asset-bounds.
Run `./scripts/run_save_io_tests.ps1 -Suite asset_bounds`; add
`-BaselineRef 933cf0a` to reproduce. Generated fixtures do not alter game assets
or saves. Knowledge-reference check: 219 references, zero errors.
In-game visual regression testing remains pending; these fixes are most directly
validated with malformed fixtures, not normal driving.


### Character animation pause (#128)

Character pose rebuilds used gameref_now (wall time), while pedestrians moved
with the JVM simulation clock. With timeWarp(0), their world position stopped
but skinning continued. The main loop now publishes jvm.sim_time to character
animation immediately after advance_sim_time; all six character pose/bone/palette
read paths sample that clock. Pause and time scaling therefore match physics,
without adding the paused wall interval on resume. UI timers/orbit clocks are
unchanged. No Java changes. This applies to character animations, not FMVs.

Release x64 compiled; source audit confirms no remaining gameref_now calls in
character.cpp. Knowledge references: 219 checked, zero errors. No automated
animation playback or agent in-game validation is claimed. Manual check: in
Valo face a walking pedestrian, pause for several seconds, confirm limbs are
frozen, resume without pose catch-up, then repeat. Also check mechanic/driver
animations during normal play. Save/section changes should remain functional.


### Pedestrian worker after pause (PR #136 follow-up)

User reports walkers blocking each other after pause, near the first garage.
Direct deterministic crossing checks pass without changing avoidance forces.
Inspection found the worker cleared posted before calculating a step, allowing
another pre-result snapshot to be queued. A result can also become ready between
the main thread's consume and submit locks. Keep posted through calculation,
clear it only on result publication, and defer submission while posted OR ready.
Zero-duration (pause) ticks consume results/update poses but submit no new job.
This prevents stale snapshots and zero-time jobs from rolling back movement.

Release x64 rebuilt. Twenty head-on crossings on real city.walk corridors pass;
these are avoidance regression checks, not proof of the worker race fix or the
specific first-garage encounter. No adjustment to avoidance weights, density or
freecam range. In-game retest remains necessary: watch first-garage pedestrians,
pause/resume repeatedly, then watch opposing pairs pass. Freecam behavior was
explicitly deferred by the user. Knowledge check: 219 references, zero errors.


### Initial garage steering (#125)

The setsteer command updated steering_input/request but placed attached parts
using the existing dynamic hub matrices. The steering wheel read the new scalar
while the wheels kept the prior pose until another operation recomputed it.
When dynamic_frames are already available, setsteer now calls pose_wheels,
which rebuilds steered/rolling frames from the current suspension state and
places/relinks the assembly. It does not integrate physics, change chassis pose,
repark the car or reset wheel spin. Before dynamic frame initialization the
existing rest placement path is preserved; later parking reads the saved steer.

Release x64 compiles; knowledge references 219 checked, zero errors. Source
trace: Garage.java sends setsteer -0.7 on entry and mechanic-mode changes.
No automated assembled-vehicle regression or agent in-game validation claimed.
Manual check: enter/load the garage and inspect wheel braquage before touching
any part; change mechanic modes and return from catalog/dealer, then confirm
normal driving steering still works. Notes stay in KNOWLEDGE_BASE.md.


### Three-decimal elapsed time (#131)

format_game_time used %g for seconds, producing variable decimal counts.
Displays with seconds now discard fractional milliseconds before splitting
hours/minutes and emit exactly three decimals. The issue's 1:19.8356 example
becomes 1:19.835 (truncation, not rounding up). Formats hiding seconds keep the
original minute boundary and US am/pm behavior. Physics and stored times are
unchanged; no Java edits. Existing separators remain as before.

Release x64 compiles. Twelve formatting tests cover the reported value, zero,
trailing zeros, minute/hour boundaries, no-seconds clocks, am/pm, no-minutes
and NaN input. Baseline 896e9bd: 4 pass / 8 fail; corrected: 12/12 pass.
Run `./scripts/run_save_io_tests.ps1 -Suite time_format`, optionally with
`-BaselineRef 896e9bd`. Evidence: assets/validation/time-format.
Knowledge reference check: 219 checked, zero errors. In-game check pending:
TestTrack/ROC timer should show exactly three fractional digits; garage clock
should retain its ordinary display.


### Native building night overlays (#133)

Ground placement loading previously rejected every standalone `night` word in
render aliases. Stock objects.rpk object 0x1a1 (krthaz02) includes a second render
0x4ba (krthaz02_night) in its ICFG, flagged 4; this is authored night geometry,
not a missing alternative texture filename. Retain the render, mark the placement
night-only by alias word boundaries, and filter at collection time in both
individual and instanced paths. Base facades stay present. All required meshes
stay resident so time changes need no map reload and reflection collection uses
the same visibility. Collision bodies are unchanged.

Enhanced policy currently shows night overlays 18:00 inclusive to 06:00
exclusive, from scene_sun_time with 24-hour wrapping. These thresholds are a
host policy, not a newly proven original-engine rule. The old blanket exclusion
advice in EXTERNAL_RESEARCH section 7 is superseded by this runtime filter;
flags=4 alone still cannot classify arbitrary daytime renders.

Release x64 compiles. Read-only stock-asset integration test instantiates
krthaz02 and checks collected props/batches at noon, 22h, 6h, 18h and noon again:
corrected totals 1/2/1/2/1, night counts 0/1/0/1/0; baseline c15a839 is always
1 with zero night overlays. Run `./scripts/run_save_io_tests.ps1 -Suite night_props`,
optionally `-BaselineRef c15a839`. Evidence: assets/validation/night-props.
Source provenance: actual objects.rpk ICFG/render aliases; current SLRR_rev
map-object-stream/category sources were checked for an applicable night policy
but none was adopted. Knowledge reference check: 219 checked, zero errors.
Visual validation pending: Valo facade windows at midday/night and after moving
time back; check no daytime overlays, duplicate flicker, or reflection mismatch.


### Night facade illumination follow-up (PR #139)

User reports registered overlays still look unlit, and requests lights start
at 18:30. Visibility now starts at 18.5h (off at 06:00 as before). Night meshes
under _krthaz_selfillum are marked emissive_overlay in SceneInstance, kept out
of opaque instance batches and drawn in the blended pass after facades. They
use reverse-Z GEQUAL, no depth write and SRC_ALPHA/ONE additive blending for
coplanar lit detail. The shader ignores sun/ambient/shadow attenuation for these
layers; ordinary geometry lighting is unchanged. Emissive overlays do not cast
solid shadows. Reflection draws reuse the same instance flags.

Release x64 and embedded shaders compile. The real-asset test now checks just
before 18:30, exactly 18:30, noon/night/dawn/noon again and verifies emissive
metadata on the collected night mesh. Checks pass; they do not prove GPU output.
User must retest illumination, coplanar flicker and overly bright wall regions.
This supersedes the 18:00 threshold documented above. Knowledge check: 219
references, zero errors. No simulation, Java or physical light-source changes.


### Night emission masks and coplanar signs (PR #139)

User confirms night detail appears but the entire house was too bright; signs
were correctly lit with slight depth fighting. Actual krthaz02_night.scx has
five material groups: one window material carries 0x06000005 (EmissionMaskMap),
four wall/roof/door sections do not. The property was ignored by the SCX reader.
CarBuilder scxWrite.ts documents selfIllumMap as TEX2D_EMISSIONMASK 0x06000005;
the matching property is present in the shipped SCX.

Parse and retain emission texture slot plus UV mapping; include both in material
merge equality. In a night overlay, skip sections without a mask and sample the
mask RGB to modulate the additive emissive contribution. Missing masks do not
fall back to white/full-bright. Preserve normal lighting on base facades. The
night detail vertex path uses a 0.002 m normal offset to reduce coplanar depth
fighting, without depth writes or distance-dependent clip bias. Onset remains
18:30. Signs only emit through their authored masks as well.

Release x64 and DXBC/GLSL shaders compile. Stock integration test verifies one
masked and four unmasked sections, valid mask slot and hour transitions. This
establishes parser/scene metadata only, not final pixel output. Retest windows,
wall/roof darkness, illuminated signs and flicker. Knowledge references: 219,
zero errors. No Java edits or physical-light changes.


### Night signs: SCX4 emission colour (PR #139 follow-up)

Stock `_uvw/night_reklam1naa.SCX` and `night_vreklam3aa.SCX` declare
COLOR_EMISSIVE (property 2) as white without TEX2D_EMISSIONMASK; their
daytime counterparts have no emission colour. ScxGroup now retains that
colour, and material merging compares it. Night overlay drawing accepts
either an authored mask (windows) or a nonzero emission colour (signs).
Colour-only signs use the white mask binding and multiply the base texture
by the emission colour; non-emissive facade groups remain excluded. Missing
authored mask textures still do not fall back to a full-bright facade.
The 2 mm overlay offset and 18:30 onset remain unchanged.

Validation: Release host build succeeded; `night_props` passed against game
assets, checking night/day sign emission and window mask presence. Actual
GPU appearance of signs still requires user validation; windows were
confirmed working by the user before this follow-up.


### Issue #129: foliage alpha-to-coverage erosion

Valo tree ICFGs use detailed `fa_tibi` renders at LOD 2; the prop selector
already chooses those over LOD 0–1 billboards. The leaf material `lomb`
uses SCX4 flags 0xa2000000 (alpha test and two-sided), reference 0x38,
and Lomb3/Lomb4 BC3 textures. Their authored mip chains are already
completed by the DDS loader; this change does not replace assets or LODs.

`shaders/fs_mesh.sc` previously discarded alpha below the cutoff and then
used a one-sided smoothstep from cutoff to cutoff + fwidth. This eroded
small leaves, increasingly with minification. MSAA alpha-to-coverage now
uses a linear pixel footprint centred on the authored cutoff, computes
derivatives before any clipping in that mode, and preserves zero coverage
for fully transparent texels. Non-MSAA cutout, shadow and reflection-face
thresholds are unchanged. `src/render/bgfx_lighting.cpp` enables this path
for authored two-sided SCX4 cutouts, in ordinary and batched draws, rather
than matching the `fa_tibi` filename. Other two-sided cutouts can benefit
as well; inspect fences along with trees when validating.

Validation: Release host and DXBC/GLSL mesh shaders compile.
`scripts/analyze_foliage_coverage.py` decodes stock BC3 alpha and estimates
quad derivatives on the texel grid; output is recorded in
`assets/validation/tree-foliage/coverage.csv`. For Lomb3 mip 0, hard-cutout
coverage is 14.4%, old estimated coverage 7.8%, centred coverage 13.5%;
at mip 5 those estimates are 20.3%, 2.2%, and 19.0%. These are CPU
diagnostics, not measured GPU output or in-game validation. Compare the
park trees near the first garage using MSAA 2x/4x/8x, then AA off/FXAA,
and check fences, tree shadows and reflections for regressions.
`SLRR_rev:src/formats/scx.ts` and `SLRR_rev:src/render/map-material-pool.ts` were
consulted; neither provides this bgfx alpha-to-coverage correction.


Issue #129 in-game follow-up: the user confirmed the foliage fix works on
16 November 2026 and supplied `assets/validation/tree-foliage/in-game-after.png`.
This confirms the reported Valo foliage appearance; separate validation of
every AA mode and other two-sided cutout assets has not been claimed.


### Issue #141: separated clouds and stronger daylight

Reference: the user supplied r72-29ee74f video frames and current screenshots,
kept in `assets/validation/clouds-daylight/`. Git comparison with `29ee74f`
showed the original density shape was still present; later wind moved the
field by about 100 km at noon and the increased sun energy also affected
cloud brightness. This is visual tuning toward the reference, not a byte-for-byte
rollback of the old shader or removal of the dynamic sky.

`shaders/fs_mesh.sc` now samples smaller puffs (noise scale 0.0019/0.0032/0.0019),
separates formations with a 0.52–0.72 density threshold, and reduces extinction
from 0.009 to 0.006. All 48 ray steps span the cloud layer instead of being
capped at 85 m and truncating oblique views. Cloud scattering bounds the
sun peak separately from world direct irradiance, so raising sun energy
does not turn formations into a saturated white blanket. Sky day/night
colours, stars, cloud movement and reflection capture remain procedural.
`src/render/bgfx_lighting.cpp` anchors wind to noon with a slower 1.5/0.55 m
per game-second drift; main views and cubemap faces share it. Time scrubbing
continues to move clouds; this does not change the game's time wrapping policy.

`src/resources/scene_sun.cpp` raises the daylight luminance target from 1.15
to 1.45 and its dark-palette gain cap from 2.8 to 4.0. Direct diffuse and
specular are stronger, not ambient exposure. Night ambient and the sunset
ramp are unchanged; garage lights do not use this outdoor day-cycle change.

Validation: Release host plus DXBC/GLSL mesh shaders compile. The new
`sun_cycle` suite in `scripts/run_save_io_tests.ps1` passes daylight energy,
noon direction, zero direct sun at night, unchanged night ambient, a warm
fading sunset, finite nonnegative values and 24-hour repetition. Running
the same suite with `-BaselineRef origin/main` fails only the stronger
daylight assertion, as expected. Logs are in the validation directory.
Cloud aesthetics, brightness and performance still require user testing
in game, including noon, late afternoon, sunset, night and car reflections.


### Issues #132 / #116: vehicle material backface culling

`src/render/bgfx_lighting.cpp` previously drew all vehicle sections without
culling, unlike the map SCX4 path. SCX4 material flag 0x02000000 explicitly
requests two-sided drawing; an unset flag should cull clockwise faces in
the renderer's reflected-Z vertex convention. The stock Enula trunk and
MC chassis materials do not request two-sided drawing. Their raw indices
and unit normals are consistent, but a top-view CPU depth diagnostic finds
frontmost downward faces in these meshes (1,069/52,397 covered pixels for
the trunk, 2,699/48,180 for the chassis at the diagnostic's 256-square grid).
This supports the backface explanation for the dark body-panel patches.
It does not by itself prove the exact cause of the under-car spike in #116.

Vehicle drawing now honours that material flag in the main view, previews
and reflection captures. SCX3 retains its existing behaviour. Glass that
now draws once uses the authored opacity/reflection once; the previous
half-reflection and two-face opacity compensation remain only for explicit
two-sided materials. The shared u_dynamicReflection.y component carries
this flag to `shaders/fs_vehicle.sc`; x retains its reflection mode.
No triangles are deleted, no game files edited and no deformation filtered.
Shadows retain their intentional two-sided caster handling.

Validation: Release host and DXBC/GLSL vehicle shaders compile.
`scripts/analyze_vehicle_backfaces.py` is a reproducible CPU geometry
diagnostic limited to rigid SCX4 position/normal/UV layouts; results and
original issue captures are in `assets/validation/vehicle-polygons/`. It is
not an in-game rendering test. SLRR_rev vehicle-material.ts/scx.ts were
consulted; the local CarBuilder scxWrite.ts documents the same 0x02000000
Backface-cull-off bit. User validation is required separately for the MC
roof/cabin, Enula trunk, and the #116 under-car spike, plus glass opacity
and reflections. Do not claim #116 confirmed from the trunk diagnostic.


### Issues #72 / #126: exact TREE integers and integer balances

`src/jvm/tree_eval.cpp:arithmetic` now handles integer operands without
converting through float. Addition, subtraction and multiplication wrap
explicitly at 32 bits; division handles INT_MIN / -1 without a host trap
and remainder handles that pair as zero. The existing divide/remainder
by zero -> zero policy is retained. Both expression dispatchers and
compound arithmetic use the same operation; unary negation and increment/
decrement also avoid signed overflow. Mixed operands still promote to float.
The deliberate MWM float-inequality behaviour remains unchanged.

`src/jvm/tree_eval.cpp:java_f2i` defines NaN -> zero, saturation at integer
limits and truncation otherwise for TREE conversions (locals, casts, fields,
array lengths and indices). Primitive casts clear the completed expression
and class carrier, avoiding the previous value-without-binop failure.
Integer field/local narrowing on mixed compound assignment is preserved.
These are general VM rules: no special case for Racer.money or the ROC.
Stock Racer.money and GameLogic.ROC_ENTRYFEE are both int; the fee is
100000. The tests confirm exact fee subtraction even for odd balances above
2^24, and integer storage after a fractional fee. They do not prove why a
particular historical ROC session went negative. A script spending more
than the balance still gets a negative integer; this change adds no clamp
or entry-eligibility rule. Keep that part of #126 subject to game validation.

Validation: Release host builds; `tree_numbers` compiles a Java fixture to
TUFA, loads it in the VM and checks exact arithmetic, compound operators,
wrapping, casts including infinities/NaN, fee storage and mixed floats.
Handcrafted TREE ordering additionally exercises the value dispatcher for
all five arithmetic tags. The INT_MIN divide/remainder cases run after
other checks pass. The same suite against pre-fix origin/main fails the
precision/fee/compound/cast cases; dangerous division cases are skipped
when earlier assertions fail. The existing save_io suite also passes. Logs
are in `assets/validation/tree-numbers/`. No production Java is modified;
the generated test Java lives only in an isolated temporary fixture.
SLRR_rev's source tree and slrr-reverse TREE evaluator sources were
consulted; no third-party numeric implementation was copied.

Previous vehicle follow-up: the user validated the #132/#116 correction and
authorized merging PR #143; it is merged and both issues are closed.


### Issues #127 / #101 / #77: OSD state and resource lifetime

`src/render/bgfx_lighting.cpp:LibBgfxBackend::bind_baked_light` resets mesh
base/mix UV transforms to identity for groupless draws, including OSD quads
and font glyphs. Previously these could inherit the final world material's
atlas transform and sample a transparent region of a UI texture. This is a
source-backed candidate for #127; the intermittent city HUD disappearance
still needs user confirmation in game. It does not change UI layering or
blend mode.

Widgets now have monotonic native handles independent of vector slots.
`osd_add_widget` returns the handle stored in _osd_wi; setter/binding APIs
resolve it through an index. Retiring a widget immediately clears its Java
pointers and marks it dead. A destroyed Viewport retires its owner's widgets
and removes its text states, while deactivate/hide only suppress visibility.
GC compacts retired entries before marking, remaps hover/string focus, and
uses forget hooks when sweeping. Registry clears never reuse old handles.
Hidden live pages remain rooted and can be reopened. The root snapshot makes
the widget sweep hook a constant-time miss for unrelated garbage objects.

Fonts share immutable CPU data by root plus normalized font name. ResourceRef
aliases are weak VM payloads; live texts and current frontend font slots
continue to root their font references. Sweeping an alias removes its mapping
and expires the last CPU asset owner. GPU font textures use the same asset
key instead of ResourceRef addresses; the next renderer flush destroys cached
font textures whose CPU assets no longer have owners. Alias cleanup only
checks its own asset, not the whole cache for every swept VM object.

Validation: Release host compiles. The ui_lifetime suite checks hidden-page
roots, immediate destruction release, stable IDs after compaction/clear,
hover and text-input focus across compaction, 200 retirement cycles without
vector growth, two refs sharing one real slii24 atlas, live-text retention,
collection of the unused alias, and final CPU asset expiration after owner
destruction. Log: `assets/validation/ui-lifetime/after.txt`. Existing
tree_numbers and save_io suites pass. GPU reclamation and the HUD visual
fix are compiled but not measured/validated in game. SLRR_rev
`SLRR_rev:src/runtime/osd-text-native.ts` and `SLRR_rev:src/osd/osd-layer.ts`
were consulted for text/layer disposal; their browser ownership model is not
a replacement for the VM's mark/sweep and no implementation was copied.

Before merging, test city driving, garage/city re-entry, repeated options and
catalog dialogs, hidden pages reopening, typing/slider focus, and a resolution
change that changes frontend font size. No production Java was modified.


### Issues #107 / #94: RPK reads and index-scoped cache lifetime

`src/resources/rpk_index.cpp:RpkIndex::read_payload` reuses a lazily opened
file stream per pack under a mutex. Text/ICFG descriptors are cached by
canonical full RID; large binary map payloads are not retained as another
resident copy. `payload_props` parses all text keys once under its own lock
and caches ordered repeated values, including negative key lookups. Empty
read failures are retried. Unscoped ids retain last-pack-wins resolution.
TOC lookups hash the low 16-bit local id directly (first matching TOC entry),
removing the linear fallback for misses. Dependency slots and host pack ids
remain separate. Cached descriptors are snapshots for the index lifetime;
editing a pack on disk requires rebuilding/rebinding the index.

`RpkIndex::set_root` clears packs, descriptor caches and open streams when
the root changes. A monotonic generation changes on root/pack registration
and distinguishes even index objects recreated at the same address.
`src/resources/script_cfg.cpp:frontend_cache` keys frontend pack/CFG state
by index identity and generation. `scene_instances_set_resources` also
clears shadow metadata, and `character_forget_all` clears skeleton/clip
caches as well as live characters. The active RPK pointer is cleared when
its owning index is destroyed; destroying another index does not clear it.
Root/index mutation must occur at a quiescent point after stopping preload
readers, followed by scene rebinding. Concurrent readers are supported;
concurrent root replacement and live-world RID migration are not added.

Validation: Release host compiles. The resource_cache suite creates two
isolated miniature RPAKs and frontend CFGs. Eight readers execute 1,600
property queries: instrumentation reports one payload disk read and one RSD
parse. Tests also verify key case/comments, repeated texture order, local/
qualified ids and misses, root-switch invalidation with reused RID values,
frontend cache rebuild with a different pack-id assignment, shadow metadata
rebinding, and active-index cleanup on destruction. Log:
`assets/validation/resource-cache/after.txt`. Existing map_props (Test Track
and ROC placements), night_props (window/sign data), ui_lifetime and save_io
suites pass. These checks do not measure full city load-time improvement.
SLRR_rev `SLRR_rev:docs/formats/rpk.md` and `SLRR_rev:src/formats/rpk.ts` were
consulted for RPAK structure and text/binary payload distinction; no parser
implementation was copied.

PR #145 was validated and merged at the user's request; #127/#101/#77 are
closed. For this resource wave, in-game menu/garage/city/track loading still
requires user confirmation before merge. No production Java is modified.


### Issues #88 / #89 / #74: reject malformed TUFA before execution

`src/jvm/tufa.cpp` bounds collection counts by remaining bytes before
reserving storage: constants >=8 bytes, field/method records 16/20, trees
4, nodes 3, interfaces 4. File sizing, allocation and reads are inside the
load exception boundary; failed tellg is rejected. Parser failure clears
all previously parsed blobs, so a malformed tail cannot publish a valid
prefix from a concatenated class container. Null nonempty input is rejected.

`parse_one_blob` validates pool links and the constant-bearing TREE opcodes
before the VM seeds static fields. UTF8, Class, Resource and reference
carriers must match the operation, while reference() still accepts the
existing bare UTF8/NameAndType compatibility forms. This prevents malformed
string/RID initializers and lazy RID fixups from encountering bad constants
after class publication. Static enrichment also catches TREE exceptions.
No unsupported constant kinds or new opcode semantics are introduced.

`kTufaMaxLocals` is 65536: method local counts and local load/declaration
slots are checked by the parser. TREE evaluation additionally bounds method
locals/arguments and declarations before allocating or indexing; size_t
arithmetic avoids imm+1 wrapping. These runtime checks also protect
in-memory TREE callers that bypass file parsing.

Validation: Release host compiles. The tufa_validation suite compiles a
valid two-class TUFA container, mutates collection counts, method-local
counts, three extreme declaration slots, static string/RID constants and
the concatenated tail, and checks clean parser/VM rejection. It also checks
the runtime declaration/local-count guards directly and executes the valid
fixture after a rejected file. No production Java is modified.

`-Suite tufa_validation -CorpusOnly` scans actual game classes; with
`-BaselineRef origin/main` it substitutes only the baseline parser. Both
parsers accept 1546 classes from the same 1501 files, rejecting only the
pre-existing `game/system/Scripts/render/osd/Dialog.class` (CONS kind 2).
This wave adds no new corpus rejection. Corpus comparison deliberately
does not run huge-count malformed fixtures against the unsafe parser.
Logs: `assets/validation/tufa-validation/`. Existing tree_numbers and
save_io suites pass. SLRR_rev `SLRR_rev:docs/formats/tufa.md` and
`SLRR_rev:src/formats/tufa.ts` were consulted for container/record contracts;
validation is implemented locally, without copied code.

PR #146 was validated and merged at the user's request; #107/#94 are closed.
For this wave, confirm menu/options, career loading, garage/city and catalog
behaviour in game before merge; parser corpus compatibility is separate
from gameplay validation. RPK TOC count validation remains a separate risk.


### Issues #80 / #95: RPAK ranges and bounded ground-node traversal

`src/resources/rpk_index.cpp:RpkIndex::parse_rpak` checks file sizing,
64-bit dependency-count sums, dependency/table spans, record count versus
minimum TOC record size, alias/bounds spans, and 64-bit offset+size against
the file snapshot before publishing entries. It parses into a temporary
pack: a bad record rejects the TOC instead of leaving a usable prefix.
File loading and allocations are inside an exception boundary. An open
pack with a rejected TOC retains its assigned id/path but has no entries.

`RpkPack::file_size` records the validated snapshot. Uncached read_payload
checks both that snapshot and the current stream length before allocating
the payload, and contains allocation/read exceptions for preload callers.
Descriptor cache hits remain immutable snapshots as described in #107;
this does not add hot pack editing. A file truncated after TOC parsing is
rejected by an uncached payload read before allocation.

`src/resources/scene_instances.cpp:walk_ground_node` visits each in-range
file offset once per GroundWalk. Cycles and repeated child pointers no
longer cause exponential recursion or duplicate placements/geometry. The
existing depth and byte-range guards remain; chunk iteration uses remaining
byte counts. Repeated node data is consumed once in first traversal order,
not interpreted as a new placement on each graph edge.

Validation: Release host compiles. rpk_validation covers huge TOC counts,
dependency addition overflow, huge payload sizes, offset+size overflow,
truncated aliases/bounds, oversized table ranges, a file truncated after
indexing, a two-edge self-cycle, and two children sharing a placement node.
All 80 actual RPK files / 11367 TOC entries are accepted. Actual Test Track
and ROC placement counts remain 1413 and 1791 respectively, matching the
previous wave's logs. Existing resource_cache suite passes. Evidence is in
`assets/validation/rpk-map-validation/`. No unsafe baseline cyclic walk or
huge reserve is deliberately executed. No game Java/data is modified.
SLRR_rev `SLRR_rev:docs/formats/rpk.md` and `SLRR_rev:src/formats/rpk.ts` were
consulted for RPAK spans and record contracts; no external code copied.

PR #147 was validated and merged at the user's request; #88/#89/#74 are
closed. In-game loading of garage, city and tracks remains the validation
step before merging this resource-validation wave.


### Issues #108 / #97: audio admission, PCM reuse and playlist recovery

`src/platform/host_sfx.cpp` admits requests before WAV reads/source creation.
At most 24 of 32 voices may be loops, leaving capacity for one-shots. At full
capacity an existing one-shot is selected for replacement before decoding,
but retired only when the replacement buffer is ready. Existing menu/burst
coalescing is retained. Rejected loop requests increment diagnostics without
reading their WAV. This is a common loop budget, not new engine-vs-ambient
priority; a 25th loop can still be denied until another loop stops.

Decoded immutable WAV format/PCM is shared between voices and held in a
16 MiB / 64-entry LRU cache. Playing voices retain their buffer even after
cache eviction; root reset stops voices and clears cached data/counters.
Oversized valid WAVs can play but are not added to that cache. Loop pitch
and gain updates now reject non-finite inputs via the same finite defaults
as play_file. HostSfxStats exposes rejection/read/hit/cache diagnostics.

`src/platform/host_music.cpp` tries at most one playlist pass when opening
a track fails, preserving next/previous direction. A fill() failure advances
through the same bounded recovery path. A wholly unreadable set remains
stopped; update does not continuously retry it. Explicit change/next can
retry later. HostMusicStats exposes attempts/failures/current track for
diagnostics. No Java native or music-set/mode contract is added.

Validation: Release host compiles. audio_recovery creates only digital-silence
PCM WAVs in an isolated fixture and mutes playback. Actual XAudio2/Media
Foundation checks pass: 24 loops share one disk read; the next loop is
rejected before I/O; nine distinct one-shots are admitted with 32 total
voices while existing loops remain; NaN loop updates do not interrupt the
voice; 65 new cues exercise the 64-entry limit while an evicted playing
buffer stays valid. Root reset releases voices/cache. An unreadable music
file is skipped to a valid WAV in both directions; an all-broken two-track
set makes two attempts then stays stopped through 100 updates. Mid-stream
fill-error recovery is implemented but not fault-injected by this fixture.
Log: `assets/validation/audio-recovery/after.txt`. resource_cache regression
also passes. These silent tests do not validate perceived sound balance or
engine/ambient priorities in gameplay; listen in menus, garage and city
before merging. SLRR_rev `SLRR_rev:docs/music.md` consulted for set contracts;
no external implementation copied. No production Java/game assets modified.

PR #148 was validated and merged at the user's request; #80/#95 are closed.


### VM lookup costs (#81, #111)

`NativeRegistry::has` and `NativeRegistry::invoke` perform an exact hash lookup,
then at most one alternate lookup for the omitted void return (`(...)` /
`(...)V`). They no longer scan all registered natives on a miss. Exact
registrations retain priority when both spellings are registered; duplicate
exact signatures still throw. Return types and class aliases are not widened.
Late registrations work without negative-cache invalidation.

`Jvm::seed_static_fields` records eligible short-tree RID initializer metadata
once per class load in `static_rids_`. `Jvm::get_static` consults this index
instead of rescanning fields and trees for every small integer. Values remain
mutable; an unresolved local RID is retried against the active resource index,
and loaded definitions and metadata stay immutable until restart (see #73/#66 below).

Validation: Release build; `vm_lookup` before/after compatibility tests cover
void aliases, exact priority, missing/duplicate natives, late registration,
mutable statics, unresolved-to-resolved RIDs and recompile/restart separation. The isolated
100,000-miss / 4,000-registration benchmark measured about 1.43 s before and
7 ms after on this machine; this is not an in-game FPS claim. `tree_numbers`
passes. TUFA corpus remains 1,501 files / 1,546 accepted classes, with the same
pre-existing Dialog.class kind-2 rejection. Evidence:
`assets/validation/vm-lookup/before.txt` and
`assets/validation/vm-lookup/after.txt`. No production Java or native API added.


### Script input safety (#87, #90)

Native adapters now use checked `native_arg_number`, `native_arg_integer` and
`native_arg_object` helpers (`src/natives/natives_internal.hpp`). Missing or
mistagged values use explicit fallbacks; numeric arguments accept Int/Float,
non-finite floats use the fallback, and float-to-int conversions saturate.
Options getter/setter, display-mode lookup/change, camera priority, time strings
and System.exit(String) no longer read unchecked argument slots/union members.
Incomplete or object-valued renderer-option setters leave settings unchanged.
Text.setPos and RenderRef.setMatrix accept integer coordinates as well as floats.
No native signatures or production Java changed.

TREE direct and compound array writes share `store_array_slot`
(`src/jvm/tree_eval.cpp:store_array_slot`). Existing auto-growth is preserved
below 1,048,576 slots, matching the new-array cap. Negative indices and indices
at or above that limit are ignored, leaving slots/length unchanged; the first
rejection is logged once per process. Compound growth now synchronizes length
as direct growth already did. This is a compatibility limit, not Java bounds
exception semantics. System.arraycopy keeps its separately documented policy;
this limit does not establish a total VM memory budget.

Validation: Release build and `script_safety`, `tree_numbers`, `save_io` suites
pass. The new suite compiles a Java fixture and exercises direct/compound
stores, the last allowed slot, negative/cap/INT32_MAX indices, mutable options,
short/mistagged native calls, Float camera priorities and time flags, Int text
and matrix coordinates, and System.exit with an Int argument. Unsafe old array
behavior was not executed with huge indices. Evidence:
`assets/validation/script-safety/after.txt`. In-game validation remains separate.


### Video frame bounds and shutdown (#78, #102)

The DirectShow Sample Grabber accepts only a complete VIDEOINFOHEADER describing
packed RGB32 (one plane, BI_RGB). Width/height validation rejects zero, negative
width, INT32_MIN height and byte counts beyond the signed 32-bit sample API.
`video_rgb32_bytes` and `video_copy_rgb32` in `include/slrr/video_frame.hpp`
validate the actual returned sample span before copying. The second
GetCurrentBuffer result, not scratch capacity or the initial query, determines
the valid bytes. Short samples never publish a new frame or copy stale rows.
Negative-height images retain top-down order; positive heights are flipped.
Dynamic media-format renegotiation is not implemented by this change.

WM_CLOSE only requests quit in the libbgfx backend. The main loop returns early
when quit is already requested and skips presentation if quit is requested
mid-frame. Backend close destroys/resets its FMV texture, dimensions and serial
before bgfx shutdown; HWND destruction remains after shutdown (this order was
already correct before this fix). Repeated close was verified.

Validation: Release build and `video_safety` pass. Synthetic pixel fixtures
cover both orientations, short/empty samples, extra trailing bytes and invalid
or overflowing dimensions. A real Direct3D 11 window test sends WM_CLOSE,
checks the HWND is still valid through mainloop_frame's quit return, then
checks it is destroyed by close and that close can be repeated. Test log:
`assets/validation/video-safety/after.txt`. Existing present/hiz shader-link
warnings (#105) were observed in that historical test; the backend still opened
and the test passed. The inactive passes and those programs are now removed;
this does not enable HiZ occlusion. Actual DirectShow decoder fallback, intro playback and FMV texture
allocation/release during playback remain manual validation; the synthetic
copy tests do not claim to cover a full decoder graph.


Intro cancellation follow-up (#102): `video_fmv_play_boot_intros` now takes a
boolean pump callback. False aborts the sequence, including ESC-release waits,
closes the active video and clears OSD blocking. Boot returns through renderer
cleanup before starting Init when the window was closed during intros. The
video_safety suite checks cancellation both before opening a decoder and after
opening a generated silent AVI; no later intro is opened. Immediate LibVLC
cancellation can log demux errors in this fixture; actual rendered frames are
not asserted. Release build and the suite pass.


### Options save failure policy (#109)

`host_game_options_update` keeps failed options dirty but suspends automatic
saveConfig retries after the first failure. `host_game_options_dirty` rearms
the normal 500 ms debounce on a new setting change. Forced updates (including
shutdown) can still retry; repeated failures without a change log only once.
Success clears dirty/failure state, and options load resets it for a new
session. Runtime settings continue to apply even when persistence fails.

Release build and `options_retry` pass. An isolated compiled Config fixture
binds a counted saveConfig adapter: checks debounce, failure followed by 100
updates after another debounce interval, rearming on a real GPS option change,
forced retry, recovery and no extra writes after success. This tests the save
result policy, not filesystem permissions; the existing save implementation is
unchanged. Evidence: `assets/validation/options-retry/after.txt`.


Enhanced settings persistence follow-up: MSAA, dynamic reflections, volumetric
clouds, sunlight and local lights now round-trip through
save/game/openslrr_options beside framerate_mode. The renderer-option native
saves after explicit changes; debug-menu changes/reset use
`host_save_graphics_options`. This does not extend the stock Config binary
format. Missing keys retain defaults; invalid MSAA/bool lines are ignored.
Loading resets defaults first so settings do not leak between roots/sessions.
The parent directory is created on an explicit host save. Save failures are
reported per user change, without a periodic retry loop.

Release build, options_retry and script_safety pass. The persistence test writes
and reloads every MSAA index (0=off, 1=2x, 2=4x, 3=8x), all enhanced booleans,
framerate coexistence, the debug-save entry point, legacy and invalid files.
These are real disk round-trips in an isolated fixture; the stock Config
saveConfig remains counted/injected for retry-policy tests.


### Live GameRef lookup (#106)

`gameref_find_live` now uses a live-ID hash index rather than scanning g_refs.
The index points at stable unordered_map elements and is erased before their
states are removed. ID allocation is monotonic until clear, without redundant
collision scans or signed overflow; exhaustion returns 0. IDs of entries that
become aliases remain dormant and are filtered at lookup, preserving wrapper
unbind semantics. Alias deletion, canonical deletion and clear remove indexed
entries. This index does not add Java GC roots or change ID ownership.

`advance_free_bodies` resolves the controlled chassis and validates its script
frame once before iterating bodies. Loose panels can have different owner IDs;
their per-panel lookups remain, now indexed. Seated-driver world traversal
(#112) is unchanged and stays open; caching its results requires reliable
invalidation of all relevant scene changes.

Release build, live_lookup, physics_position and ped_crossing pass. The same
lifetime/alias fixture passes with baseline a15ab7b and current code, covering
stable addresses after rehash, dormant aliases, alias/canonical removal, reused
keys and clear/reset. Isolated 100,000 lookups / 8,000 nodes measured about
2.71 s before and 0.81 ms after; not an in-game FPS measurement. Evidence:
`assets/validation/live-lookup/before.txt` and
`assets/validation/live-lookup/after.txt`. Gameplay validation is separate.


### Driver duplicate visibility (#112)

`DriverSeat` now owns a `GameRefRenderMask`. The duplicate-render mask is rebuilt
when the scene revision, root, kept render or resource type changes, rather
than undoing visibility and breadth-first walking the world on every pose.
Creation/upsert, removal, clear, alias binding, changed parenting and resource
type changes invalidate it. All production GameRef type assignments use
`gameref_set_type`; direct metadata mutation outside these entry points must
explicitly use upsert or the appropriate setter. Upsert conservatively
invalidates even an existing entry, as callers may initialize more metadata
afterwards. Ordinary transform changes and redundant parent assignments do not
invalidate. Frequent scene mutations can therefore still cause repeated walks;
there is no measured in-game frame-time claim.

Rebuild traversal has no 4,096-node cutoff and visits each canonical key once,
terminating even with aliases/cycles. Already-empty renders are left alone.
Release restores only entries hidden by this mask, checking their per-creation
incarnation so a recycled key or cleared world is never unhidden accidentally.
Incarnations continue across clear and do not alter live IDs or Java GC roots.

Release build and driver_mask, live_lookup, script_safety, ped_crossing pass.
The driver-mask fixture checks a duplicate after 5,000 nodes, 10,000 unchanged
cache hits, motion/no-op parenting, late creation, type/reparent/world changes,
alias/cycle traversal, original hidden visibility, removal/reused keys and
clear/release. This tests the shared production mask used by pose_driver, not
seated animation or a full garage transition. Evidence:
`assets/validation/driver-mask/after.txt`. No production Java/native API change.


### File texture replacement and hidden preview viewports

The MWM DriverSelect chain uses ResourceRef.makeTexture, duplicate,
RenderRef.changeResource and RenderRef.create. makeTexture supplies a file path
rather than an RPK texture ID. RenderRef now stores per-instance
texture_file_replacements alongside RPK replacements; duplicate and instance
creation copy the paths by value. Scene collection maps the original texture
resource to matching mesh slots and publishes the replacement path. Replacing
the same source updates its file entry; replacing it with an RPK texture clears
the file entry. Geometry/shared mesh textures are unchanged. This closes the
file-backed material path without changing production Java or inventing an
executable special case for DriverSelect. It does not implement all possible
dynamic texture resource queries such as standalone makeTexture IDs.

`gameref_active_cameras` excludes bound aliases and cameras whose non-null
viewport is inactive or missing. Cameras with no viewport retain their prior
eligibility. Thus Osd.hide / Viewport.deactivate stops the scene preview even
when a script retains its scene camera and render objects. Reactivating the
viewport makes its camera eligible again; no model is forcibly destroyed.

Reference checks: DriverSelect.java RefreshRender/exit and Osd.hide, SLRR_rev
src/runtime/viewport-native.ts deactivate, plus slrr-reverse native signatures
for makeTexture/changeResource. No external implementation copied.

Release build and driver_selector, driver_mask, physics_position pass. The
selector fixture uses actual humans.rpk / vin.SCX and verifies the file path
reaches a collected material slot through registered natives; subsequent
changes preserve prior copies. It also tests viewport deactivate/reactivate/
destroy against fullscreen camera selection. Synthetic paths do not assert
image decoding or a visual skin comparison; gameplay remains to validate.
Evidence: `assets/validation/driver-selector/after.txt`.


PR #155 gameplay validation (17 November 2026): the user confirmed seated
driver behavior and then the MWM DriverSelect skin/menu fixes. No production
Java was modified. NATIVE_AUDIT.md now records file-backed texture propagation,
viewport lifetime eligibility and remaining dynamic-texture-ID limits; its
stock registration totals are synchronized with the Python checker (339
bodies, 3 explicit no-ops, 34 missing, 376 total; 31 active missing plus 3
leftovers). ENGINE_CONTRACT.md states the generic material/camera rules.


### Exact native coverage descriptors (#115)

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
writes. The full source audit and regenerated-output --check pass; counts remain
339 bodies / 3 explicit no-ops / 34 missing (376 total). Generated coverage JSON
now states the exact-descriptor scope; README and NATIVE_AUDIT.md agree. No
runtime source, executable, stock manifest or production Java changed, so no
new game compilation or in-game validation is needed for this audit-only fix.


### Per-axis gamepad fallback (#96)

`src/platform/host_input_mapping.hpp` shares the production mapping and analog
steering policy between mapped_axis and host_input_update_controller. Defaults
(left stick steering, RT throttle, LT brake, B handbrake) remain available when
an unrelated pad action is added. An explicit pad binding for a driving axis
replaces that axis's default. A physical channel assigned elsewhere cannot
also drive its default action; the two steering halves are checked separately.
Keyboard steering retains priority and its digital classification. Explicit
analog steering still uses its configured transfer function. Smoothing, forced
axes, device polling and the Y/reverse fallback are unchanged.

Release build, pad_fallback and script_safety pass. The new fixture supplies
synthetic physical values to the same pure production mapping helpers, testing
extra horn/camera buttons, per-axis overrides, reassigned triggers/B, half-stick
claims, keyboard priority, explicit analog remaps and short input spans. No
hardware or full options-screen interaction is asserted. Evidence:
`assets/validation/pad-fallback/after.txt`. Native coverage --check remains
current; no native signatures or production Java changed.


### Immutable loaded classes and reference overloads (#73, #66)

`Jvm::load_class_file` caches successfully loaded absolute, lexically normalized
paths (case-folded on Windows). Failed/empty loads are not cached. A new file
may introduce new classes, including siblings concatenated with an existing
class; exact loaded class names are never overwritten or reinitialized. Class
storage is published before its literal/static enrichment, so recursive class
queries see the existing definition. Live methods, TREE vectors, statics and
RID initializer metadata remain valid for the VM lifetime. Changed files,
including new blobs appended to a previously loaded path, require restart.
A fresh VM sees the new disk contents. No live hot-reload API is introduced.

`find_method_by_name` scores null as a reference instead of tying primitive
arguments. Primitive arrays are reference parameters as well. This fixes the
ordinary this(null)/this(parent) overload chain without recognizing Dummy or
any particular mod class. Numeric conversion scoring is retained. Selection
among multiple equally scored reference overloads remains declaration-order;
this does not implement full source-language overload inference.

Validation: baseline dede4b5 fails all three class_lifetime scenarios; current
code passes them. Coverage includes repeated/normalized-path load with mutable
statics, reload inside an executing native callback/TREE frame, new concatenated
containers with an existing class and a new sibling, failed-load retry and an
already-loaded file later corrupted on disk. Constructor fixtures cover four
Dummy-shaped overloads, null/int selection and primitive-array parameters.
The actual game Dummy.class is also executed with production GameType natives;
all four constructors produce native roots and preserve parent/flags. This is
not a full career, catalog or suspended-fiber gameplay run. The minimal test
pack setup logs unrelated frontend RID misses. Evidence:
`assets/validation/class-lifetime/before.txt` and
`assets/validation/class-lifetime/after.txt`.

Release build, vm_lookup, tree_numbers, save_io and TUFA corpus checks pass.
The corpus is unchanged: 1,501 files, 1,546 accepted classes and the existing
Dialog.class CONS-kind-2 rejection. vm_lookup now checks immutable definitions
in the running VM and refreshed metadata in a fresh VM. No native signatures
or production Java changed. Static ++ behavior is covered separately by #63 below; the constructor
counter fixture uses explicit addition to isolate constructor dispatch.


### Static increment/decrement writeback and career days (#63)

TREE previously computed ++/-- but only wrote back locals, instance fields or
array elements. Static field reads now retain their actual owner/name through
both direct/quick field opcodes and resolved name paths. Statement and
expression ++/-- use a shared field-writeback helper; postfix retains the old
expression result and prefix exposes the stepped value. Instance reads clear
the static target so an earlier static access cannot redirect a later write.
Integer stepping keeps 32-bit wrap and floats remain floats. No GameLogic/day
special case, production Java change or new native is involved.

The actual shipped GameLogic.spendTime TREE now advances day when time crosses
its existing strict `> 24*3600` condition. The exact-midnight boundary is not
changed; a positive step beyond it advances the day. Multi-day steps increment
once per wrap and still notify timeRefresher and call System.syncGameTime.

Validation: static_increment has 0/2 passing on baseline 4de3d3c and 2/2 on the
fix. A compiled generic fixture covers statement/expression pre/post ++/--,
float fields, uninitialized statics, foreign owners, both 0x1b/0x1c encodings,
multiple increments in one expression, int overflow and local/instance
regressions. A TreeHost harness executes the actual GameLogic.class method,
checking exact midnight, single/multiple day crossings and expected notification/
clock-sync calls. It isolates the VM method from the rest of the career; full
career persistence and header display require in-game validation. Evidence:
`assets/validation/static-increment/before.txt` and
`assets/validation/static-increment/after.txt`.

Release build, tree_numbers, class_lifetime and save_io pass. Native coverage
--check remains current; stock registration counts are unchanged.

## Garage main-menu inventory save ordering (#59)

The verified MWM Garage.class main-menu command saves before changing section.
This differs from sources/sl/Scripts/game/src/Garage.java, which already changes
section first. A save taken before Garage.exit sees player.parts empty because
the purchased parts still belong to the mechanic's VisualInventory.

The distributed patch/sl/Scripts/game/Garage.class swaps the two standalone
TREE statements. GameLogic.changeActiveSection now runs Garage.exit and
Mechanic.flushInventory before autoSaveQuiet snapshots player.parts. The VM,
natives and save format are unchanged. scripts/patch_garage_save_order.py
reproduces the binary patch from one SHA-256 verified MWM input and rejects
other mods; do not apply binary offsets to an unknown Garage container.

Validation: inventory_transition executes the actual shipped GameLogic transition,
Mechanic.flushInventory and Garage.osdCommand/exit with isolated presentation
callbacks. The original records zero transferred items at save and three after
exit; the patched handler records all three before save, with one exit/enter.
This verifies call ordering, not a complete career file round-trip or gameplay.
The suite also checks generic interface dispatch; no class-specific VM workaround
is needed. In-game validation must buy parts, leave via Main Menu, reload and
check the inventory, then repeat with normal Exit Game saving.

## Full-screen navigator and scene viewport ownership (#65)

Navigator.show creates a small viewport at priority 12; ViewMap recreates the
same independent localroot camera with a 1x1 viewport at priority 5. Track's
map command resizes the driving OSD viewport to (.64, .64, .35, .35).
Previously sync_instances discarded every full-screen secondary camera and
fullscreen_script_camera could instead select the navigator as the main scene
camera, while the main world pass always occupied the entire framebuffer.

Camera ownership now follows the selected scene world via the existing
scene_instance_transform ancestry check. Independent Dummy roots stay separate,
even with an owning parent; resizing the driving viewport does not transfer
main-camera ownership. When no world is selected the legacy full-screen
fallback remains. The legacy fullscreen_script_camera function name is retained.

The renderer collects independent local-world views at full screen as well as
insets, uses the scene viewport rectangle and aspect for the world pass, and
orders inset-world/preview passes by viewport priority (map 5 before scene 10).
It resets the bgfx ordering on the next frame so closing M restores normal
composition. Interface views remain above the camera passes; shadow/reflection
views are outside the reordered range. Full-frame depth occlusion is skipped
for an inset world view. No Java change or new native is needed.

Validation: map_views fails on baseline 67cd581 and passes on the fix. The test
uses real camera activation, world isolation/ancestry and viewport lifetime,
including a local root owned by the scene, the GPS-to-full-screen transition,
a resized driving view and map closure. It does not render GPU pixels. Release
build and driver_selector regression pass; visual in-game validation is pending.
Check M in Valo: map tiles/roads and available markers, bottom-right driving
view, then M again; repeat, drive to move the GPS and check garage/catalog
previews. Evidence is in assets/validation/map-views/{before,after}.txt.

## Authored map alpha versus colour mixing (#50)

The opaque bridge and construction openings are not missing texture alpha:
their SCX4 ColorMixTex sections (type 3) carry explicit alpha-test flags. The
previous material_mix shortcut treated every type-3 surface as opaque paint,
turning texture alpha into a solid colour weight and bypassing discard in both
ordinary and instanced rendering and the static shadow pass. The dealer's
korlat panels instead use type 0 with explicit blending; the old glass test
required material_type bit 0x04, so their texture alpha was ignored too.

include/slrr/scx_mesh.hpp:scx_surface separates colour function from coverage.
SCX4 0x40000000 selects blending; otherwise 0x20000000 selects alpha testing.
ColorMixTex paint mixing applies only when neither flag is present. SCX3's
existing flags 1 (cutout), 0x101 (blend) and 0x100 (paint without alpha opacity)
keep their previous coverage classification. This policy is shared by map
materials in draw_instance, draw_prop_batches, update_static_shadow and prop
batch eligibility. Vehicle-specific shaders retain their own material path.
Blended props stay out of opaque batches and static shadow casting, as with
existing glass. This does not add transparent-depth sorting or blended shadows.

Verified assets: roadtest26_egyedi has five type-3 tested sections at reference
56 for the bridge grids/cables; area_200_egyedi and area_19_egyedi each have two
such sections (references 25 and 56 respectively). The dealer marketkulso
korlat is type 0 with flags 0xc0000000. No asset files or map names are used
as rendering exceptions. SLRR_rev's scxDiffuseUsesAlpha was consulted for its
SCX3 alphaOpacity distinction; the correction is based on the local materials
and the renderer's own flags, not copied code.

Validation: map_alpha passes three tests covering the real bridge, both worksite
meshes and dealer rail, plus opaque paint and legacy classification controls.
night_props still passes (including sign emission). Release builds and native
coverage --check pass. These tests check asset/material contracts, not GPU
pixels. Visual in-game validation is pending: inspect the bridge grids from
both sides, the worksite grids/cables and dealer rails; also check ordinary
facades, foliage and the resulting cutout shadows. Evidence is recorded under
assets/validation/map-alpha. No native was added or changed.

### Storefront colour/opacity follow-up

The maintainer validated the bridge, worksite and dealer fix in game, then
reported solid white city storefront panes. Real storefront glass meshes use
COLOR_DIFFUSE opacity around 0.247 or 0.498. The 06x5_shop_uveg and
12x6_uveges_uveg type-0x32/0x33 variants deliberately omit a diffuse texture;
other storefront Tex families have textures but black diffuse RGB. The generic
map draw previously used white fallback RGB and forced tint alpha to one.

scx_modulate_blend now carries authored opacity into the existing straight-alpha
mesh output for blended SCX4 surfaces and applies diffuse RGB when the texture
slot is deliberately absent. Textured families retain sampled RGB, so their
black diffuse colour does not black out the window. Absent diffuse maps use
straight-alpha blending; decoded premultiplied images also scale RGB by material
opacity. Opaque surfaces, legacy
SCX3, emissive overlays and the vehicle material path are unchanged. This adds
no new reflection implementation; it corrects the existing mesh pass modulation.

map_alpha now passes four tests, including five actual storefront glass meshes:
two without diffuse textures and three textured variants. The test checks tint,
material opacity and opaque/legacy controls. The original bridge/worksite/dealer
tests remain passing. Storefront visual verification is pending; the screenshot
provided by the maintainer is stored as storefront-before.png under the same
validation directory and is explicitly a before image.

## Version metadata before compilation on every generator (#103)

CMakeLists.txt now defines slrr_version as an always-run custom target whose
byproduct is generated/slrr_version.h. slrr_host depends on that target, and
src/boot/boot.cpp explicitly lists the header as OBJECT_DEPENDS.
The old TARGET PRE_BUILD command behaved as PRE_LINK outside Visual Studio,
so Ninja/Makefiles could compile or reuse objects with the previous version.
Configure-time generation no longer hides that ordering problem on first build.

cmake/write_version.cmake still writes only when the computed content changes:
revision count and short hash follow HEAD, and a dirty checkout adds +. The
version target is checked every build, but an unchanged header does not trigger
compilation/relinking. The window-title consumer rebuilds in the same invocation after a
Git-only metadata change. Paths passed to the generator are quoted.

Validation: tests/test_version_build.py extracts the actual production version
rule into independent Git fixtures with spaces in their paths and compiles a
small main plus a header-consuming boot function at the production source paths. It checks
initial clean build, metadata-only dirty state, new commit without C++ edits,
an unchanged rebuild (header and executable timestamps preserved), and clean
rebuild after generated outputs are removed. Ninja, NMake Makefiles and Visual
Studio 17 2022 all pass. Baseline 9df8920 fails the first dirty-metadata Ninja
rebuild, reporting the previous clean version from the boot consumer. These are
build-graph tests, not a game launch or a full Ninja build of the bgfx project.
The full game's Visual Studio Release target also builds. Native bindings and
runtime gameplay are unchanged. Evidence: assets/validation/version-order.

## Per-body physics failure containment (#75)

The rigid-body kernel deliberately throws on invalid mass/bounds/orientation,
non-finite frames and non-finite integration state. Keep those kernel checks:
the owning runtime now contains failures at each chassis, loose-part and free
PhysicsRef body rather than allowing a frame pump to unwind a Java dialog or
terminate the host. std::exception failures are reported and handled locally.

Chassis configuration is transactional (configure/set_frame on a candidate).
An invalid chassis is suspended with physics_pending_placement cleared; a step
failure restores its saved rigid body and the last finite local pose, resets
motion and interpolation and leaves other chassis ticking. Pose orientation is
reconstructed as in dynamics_sync. With no valid pose to restore, the node is
hidden. The normal start/wakeup path can retry simulation; unchanged bad data
will suspend it again. A chassis failure is logged once per record lifetime.

Loose-body creation also configures a candidate. Failed native map promotion
cleans partial nodes, keeps the ordinary scenery and suppresses repeated retries
for that resource until the part runtime is reset. A failed active loose body
is removed; a promoted map prop returns to its authored placement. A failed free
PhysicsRef restores its last published finite pose and is removed from simulated
bodies. Failed loose/free contact steps restore the collision partner's saved
rigid body too. These guards do not roll back arbitrary script/asset mutations
or promise recovery from allocation failure.

GameRef.setMatrix rejects non-finite position/rotation before mutation. WheelRef
setters reject non-finite scalar or Vector3/Ypr fields atomically; finite values
retain their existing semantics. Numeric overflow generated later in the solver
is caught before an invalid body frame is published. Free bodies now have an
explicit first-configuration flag (live_id can legitimately still be zero), and
free-body state plus failed-native-type suppression reset with part registration.
No native signature is added; the kernel and normal physics tick remain intact.

Validation: physics_recovery is 0/8 passing on f3638a1 and 8/8 on the fix. Its
isolated synthetic RPK/CFG/SCX fixtures cover NaN poses, malformed free-body
bounds, invalid free frames, zero-height chassis bounds, corrupted chassis
pose/orientation, wheel updates, extreme finite velocity overflowing the render
frame, and transactional invalid map-body creation. A native pump callback
returns after the numerical failure while a healthy chassis keeps ticking.
This is not an automated real Dialog.display test. Active detached-panel failure
and contact-partner rollback paths are implemented but not separately fault-
injected by this suite. map_props and physics_position regressions pass. The
Release build and native audit checks pass. In-game driving/collision/dialog
smoke validation is pending. Evidence: assets/validation/physics-recovery.

## Unique world-position varying (#100)

shaders/varying.def.sc keeps the initialized v_worldPos on TEXCOORD4 and removes
the duplicate TEXCOORD6 declaration. The bundled shaderc inserts declarations
by name and retains the first entry, so TEXCOORD4 was already the effective
semantic. No producer/consumer shader or remaining semantic is reassigned.
The duplicate is removed from the open risk register above.

Validation: the Release target regenerated DXBC and GLSL shaders successfully.
SHA-256 comparison of all 26 generated shader headers before/after found no
changed bytes. A declaration scan found no repeated attribute/varying names.
This is a declaration cleanup with unchanged compiled shader output; a new
in-game visual difference is not expected. Evidence:
assets/validation/worldpos-varying/validation.txt.

Track HUDs do not require a menu background: `src/resources/gameref.cpp:fullscreen_script_camera` suppresses the fallback veil while a world camera is active. See [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#track-presentation-without-a-menu-plate).

Menu hotkey presses use a shared 250 ms cancel/menu gate and a separate select gate, without delaying release or driving: `src/platform/host_input.cpp:host_input_check_hotkeys`. Validation and scope are in [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#menu-input-debounce).

UI rectangles handed to the OSD registry are excluded from 3D scene/preview collection, including cursor tooltip templates: `src/resources/scene_instances.cpp:collect_root`. See [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#ui-rectangles-have-one-render-owner).

Catalog vehicle insets stay in the preview pass, preserving the page background and script camera pose; fullscreen showrooms retain main-world ownership. Cursor cameras are UI-only: `src/boot/mainloop.cpp:mainloop_sync_section_render`, `src/resources/gameref.cpp:fullscreen_script_camera`. See [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#catalog-vehicle-preview-versus-fullscreen-showroom).

Tooltip layer 0 must not promote catalog page backgrounds above preview cameras. Background ordering uses owned nonmodal page plates rather than the lowest arbitrary widget/text layer: `src/ui/osd_registry.cpp:osd_page_background_layer`.

Trigger indicators are visual RenderRefs, while trigger volumes stay sensors; explicit physics and ground collision remain authoritative: `src/resources/scene_instances.cpp:scene_instances_prepare_physics`. Cue rendering and reconstructed motion defaults are documented in [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#trigger-cues-and-physical-bodies-130).

## 14. Validated issue fixes (2026-11-16)

### GameRef aliases: events and timers (#93)

`src/resources/gameref.cpp:gameref_set_event_mask` and
`src/resources/gameref.cpp:gameref_add_timer` resolve the canonical node before
creating a new GameType state. Alias writes now reach the same event mask and
timer queue used by reads, clear/remove operations and timer dispatch. Existing
canonical kinds are preserved; unregistered objects still create GameType state.
No Java changes or scene-specific exceptions. This fixes state routing, not the
scheduler itself. Stock paths mostly use direct objects, so the main impact is
aliased native instances and mod scripts.

Release x64 compilation succeeds. Three deterministic GameRef tests cover mask
set/clear and kind preservation, timer canonical destination/one-shot/removal,
and unregistered-object creation. Main 784a760: 1 pass / 2 fail; corrected:
3 pass / 0 fail. Run `./scripts/run_save_io_tests.ps1 -Suite gameref_alias`,
or add `-BaselineRef 784a760` to reproduce. In-game validation remains pending;
these tests exercise the actual state primitives but not Java event dispatch.

PR #121 integration update (2026-11-17): conflicts were limited to this
knowledge base and the test runner; current main's suites and fixes are kept.
Release x64 rebuild succeeds. Against main 4e4b782, gameref_alias still reports
1 pass / 2 fail; the integrated branch reports 3 pass / 0 fail. live_lookup and
trigger_markers pass too. The trigger animation regression now samples two
bob heights and both yaw basis components, avoiding equal-value samples at
animation extrema, and reports caught exceptions instead of aborting silently.
These remain automated host checks; no additional in-game validation is claimed.
Evidence: assets/validation/gameref-alias/integration.txt.

OSD text lines share one bounded glyph batcher and an unlit shader, avoiding per-glyph draws/world-light bindings: `src/render/bgfx_osd.cpp:draw_osd_line`. Measured draw counts and pixel comparison are in [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md#batched-osd-text-82).

## CMake dependency paths and toolchain guard (#114)

CMakeLists.txt reads the resolved BGFX_DIR/BIMG_DIR definitions from the
already-added bgfx.cmake directory scope. This follows its actual choice of
explicit paths, in-tree checkouts or FetchContent sources, including custom
FETCHCONTENT_BASE_DIR and FETCHCONTENT_SOURCE_DIR overrides. It does not infer
source paths from the host build directory or overwrite cache preferences.
ImGui sources, shader includes and bimg third-party headers now use the same
checkout as the corresponding libraries. A missing resolved directory fails
configuration with a direct diagnostic.

The guard now explicitly requires Windows x64 and an MSVC-compatible compiler,
matching the existing MSVC flags and documented Visual Studio workflow. MinGW
is rejected rather than allowed through to fail on /utf-8 or /fp:precise. x86,
ARM64 and non-Windows targets remain unsupported. Clang-cl is not separately
validated; the exercised compiler is Visual Studio 2022 MSVC.

Validation: tests/test_cmake_dependency_paths.py configures the real project
in isolated directories containing spaces, offline against existing dependency
sources. Five cases cover default/custom caches, source overrides, in-tree
checkouts and relative explicit directories; generated ImGui/shader/bimg paths
are checked. Five cases execute the actual guard with supported/unsupported
platform/compiler/architecture values. Current: 10/10 pass. Baseline b85dc44: 5/10 pass;
results recorded in assets/validation/cmake-dependency-paths. The guard cases
simulate variables, not installed MinGW/ARM compiler runs. Non-default cases
are configure/generate checks; the ordinary Release x64 host build succeeds.
No game runtime behavior or native registrations change.

Fixtures/logs remain under build/ for inspection. They reference the existing
sources through directory junctions without modifying dependency checkouts.
A tiny fixture-only compile_game_sources.cpp isolates the independent missing
utility file in HEAD; the pre-existing local optional-target edit stays excluded.

## Removed inactive renderer/video fallbacks (#113)

The supported Windows x64 build always uses libbgfx for game rendering.
The obsolete, non-compiling WGL branch has been removed from the renderer
factory. The implemented NullBackend remains in bgfx_backend.cpp for headless
probes/tests and the existing unwired D3D9 selection; only the unused three-line
null_backend.cpp stub was removed. This does not remove bgfx's own renderer
backends or add cross-platform support.

SLRR_HAS_LIBBGFX no longer gates the factory or PNG/PTX decoders, and CMake
no longer defines it. Font PNG and PTX/JPEG/PNG decoding always use the same
existing stb implementation, with no replacement decoder or asset changes.
The GPU regression runner no longer injects the obsolete define either.

The non-Win64 DirectShow soft-success branch was deleted: graph setup failure
now takes the existing error return after graph cleanup rather than inventing
a playing video without a decoder. The supported Win64 playback path already
used that error behavior. Other VLC/AVIFile/DirectShow selection and playback
lifetime code remain unchanged. No unsupported x86 build is claimed.

Validation/evidence: assets/validation/dead-fallbacks. This cleanup is checked
by building the supported host without the old define and exercising existing
renderer/video/font regressions. It does not establish non-Windows or x86
compatibility or reproduce the removed dormant branch as a supported runtime.

Issue #113 checks passed: Release x64 compile/link without the old renderer
macro; video_safety, ui_lifetime, splash_boot and osd_text_gpu. The D3D11 text
capture retains the #82 pixel hash and 20 text draws for 1,000 glyphs. Native
coverage remains current; knowledge references pass. These are supported-host
regressions, not execution of the removed x86/WGL branches.


### Shared race support (#179; #60, #61, #62)

Road ownership and cache: `src/resources/race_runtime.cpp:race_ground_roads`;
directed TRC solver: `src/resources/road_routes.cpp:RoadRoutes`;
traffic identities/streaming/halts: `src/resources/traffic_runtime.cpp:race_traffic_tick`;
brain commands: `src/resources/race_brain.cpp:race_brain_command` and
`race_drive_input`; mechanical input integration:
`src/resources/part_runtime.cpp:configure_drive`; route ribbon:
`src/natives/natives_renderref.cpp:plotRoute`.
Sources and host limits: EXTERNAL_RESEARCH v221 section and SCRIPT_RUNTIME shared
race section. Tests: road_routes, race_brain, traffic_runtime and race_vehicle.
A real assembled Baiern moves under original Bot.startRace commands in an
isolated fixture; this is not proof of finished career day/night/ROC sessions.
The original issues and #179 were closed for consolidation, not gameplay acceptance;
#197 retains the remaining work. The [current audit](TRAFFIC_RACE_AUDIT.md)
records the baseline's missing chassis pairs/player-only movable contacts, the
shared-contact implementation and remaining physical/kinematic, seating/audio
limits. Contact integration is not acceptance of the complete #197 checklist.


## Inactive renderer passes cleanup (#105)

The offscreen scene target, fullscreen present quad, HiZ pyramid/readback and
legacy projected-car-shadow path had no production caller. They and their six
exclusive shader sources/build inputs are removed. Active cached map/garage
shadows, vehicle depth, reflection captures, OSD and previews retain their paths.
`scene_instances_set_size_filter` is an optional projected-size filter, not an
occlusion query; the backend preserves its 40m near exemption and two-pixel
cutoff, and disables it for insets, reflection and shadow collection.

`create_owned_program` links without implicit shader transfer and releases both
caller references regardless of the result. bgfx retains its own references for
a successful program. This covers missing stages, interface mismatch and program
creation failure without depending on bgfx's early-return ownership behavior.

`render_programs_gpu` reproduces the old D3D11 rejected-link leak (shader count
27 -> 29), explicitly cleans it, then checks 32 successful/rejected/missing-stage
cycles return to 27 and that backend close/reopen preserves the baseline. The
existing GPU OSD test still produces 20 draws for 1000 glyphs; map view tests pass.
The test uses unique shader metadata hashes to prevent bgfx deduplication from
hiding leaked references. See assets/validation/render-pass-cleanup.txt.


## GPU mesh source ownership (#83)

`ScenePart` stores `SceneMeshData` (mesh info, groups, texture paths, scale and
vertex/index counts), a shared source and GPU handles. Uploads and skin uploads
read all vertex attributes directly from that source. Owned standalone meshes
move into one shared source; resident scene meshes reuse their existing source.
Material merging copies only its reordered index scratch, released after bgfx
has copied the upload bytes. Blended group ordering and source collision/picking
data are preserved. No renderer-owned vertex/index/skin payload remains resident.

Revision changes rebuild GPU buffers and metadata from the current source;
changed texture identities reload bindings. Replaced deformed sources retain
compatible texture bindings, as before. Skin buffers are recreated on demand.

The `scene_mesh_cache` D3D11 test covers shared-source ownership, material order,
blend exclusions, 16 position/normal/UV/skin/index revisions without GPU handle
growth, deformed-source texture transfer and full cleanup. Read-only City POLY,
Baiern chassis and area_205 assets account for 16,076,716 bytes of vertex/index/
skin arrays the old renderer would duplicate; this is a sample payload count,
not whole-process memory usage or an FPS gain. See
assets/validation/mesh-cache-ownership.txt.


## GC eligibility cache and mark epochs (#110)

`src/jvm/gc.cpp:needs_finalizer` caches class eligibility, skipping descriptors
and empty-owner searches. New class insertion, game-root/classpath changes and
append-only native registration invalidate cached results. Real finalizers still
run through ordinary VM dispatch and retain once-only/resurrection behavior.

The weak heap address index is updated at allocation/sweep, checked before any
native-address dereference, and never acts as a GC root. Per-object epochs
replace temporary managed/marked hash sets; each resurrection trace gets a new
epoch. Finalizer discovery walks a stable owning-vector prefix instead of
allocating another pointer snapshot. See docs/VM_GC.md for memory/CPU tradeoffs.

`gc_runtime` covers native/script finalizers, late class/native availability,
resurrection and allocations, invalid native pointers, roots/pinning/cycles and
locals/arguments on a suspended cooperative stack. The 60,000 mixed-object
fixture takes about 11ms to collect versus 168-171ms before; allocation costs
26-29ms versus 23ms before. The baseline/current logs and limits are recorded in
assets/validation/gc-lookup-epochs.txt. This is not a gameplay FPS measurement.

## Garage paint booth (#64)

Body paint, spray and decals were inert: the three `Painter` natives were
unbound, and no world-pointer event ever reached `Painter.handleEvent`, because
only the Mechanic received part clicks.

Events. `MouseCursor.addHandler` registers `EVENT_CURSOR` on the cursor
GameRef. `src/boot/mainloop.cpp:dispatch_world_pointer` now emits `EC_HOVER`,
`EC_LEAVE`, `EC_LDOWN`, `EC_LUP` and `EC_LCLICK` from that GameRef whenever
the Mechanic thread is not running (see SCRIPT_RUNTIME.md). The
press/release edges come from `HostInput::world_press` / `world_release`,
set only for a left press that missed every OSD widget. `GameRef.getInfo`
answers `GII_CATEGORY` (7) with `GIR_CAT_PART` (9) for parts, which the
Painter, CarMarket and garage drop handlers test. Vehicles are not yet
reported as `GIR_CAT_VEHICLE`.

Natives. `src/natives/natives_painter.cpp:register_painter_natives` binds
`paintPart`, `doPaint` and `xPaint`. Each picks the part under the pointer
with `src/resources/scene_instances.cpp:scene_instances_pick_surface`, which
also returns the hit mesh, its paint slot and the hit point and normal in the
mesh frame. The executable (slrr-reverse notes on 0x004827D0, 0x00482960,
0x00482C00) queues `paint 0 0 <color> 0 -1` and `bigdecal ...` commands on the
picked part; the host draws directly instead:

- `paintPart` fills the part's own texture with the can colour.
- `doPaint` projects the brush (`misc.garage:0x0102`, a soft dot) or a decal
  with `src/resources/paint_texture.cpp:paint_stamp_mesh`: a square `size`
  metres wide, along the hit normal, oriented to screen up (including
  mirrored part frames), turned by
  `rot` and mirrored by `flip`. Only paint-slot triangles facing the stamp
  inside a depth slab are painted; each texel blends once per stamp. Other
  panels whose bounds the stamp reaches
  (`src/resources/scene_instances.cpp:scene_instances_paint_targets`) and
  whose triangles it covers (`src/resources/paint_texture.cpp:paint_stamp_reaches`)
  are painted too, in their own frames, so a decal across a door seam lands
  on both parts. A preview can therefore span several textures.
  `temp = 1` (the decal preview) is drawn over a copy and replaced by the next
  preview.
- `xPaint` and `painter_tick` (two host frames without a refresh) take the preview off.
  A texture created only for a preview gives the part its previous texture
  back.

These orientation, size and expiry rules are host approximations.

Textures. `paint_texture.hpp` keeps Painter textures in host memory with ids
`0xFFFFxxxx`. `src/resources/part_runtime.cpp:assign_texture` shows them in
the part's paint slot like a pack colour (`Part.setTexture` accepts them) and
reclaims one after three host frames without a live part, resource wrapper
or open save reader using it. This also reclaims unused loaded-skin textures and preserves
ids through synchronous part rebuilds. A part draws on a copy when it
shares its texture. `scene_instances_texture` maps the id to a `paint:` path;
`src/render/bgfx_osd.cpp:paint_texture_handle` uploads it with a box-filtered
mip chain and again on each change. Images start from the part's current
paint (DXT/DDS/PNG/PTX decoded by `paint_image_from_texture`).

File-backed decals. Stock `Decal(String)` calls `ResourceRef.makeTexture`,
then Painter receives only `brush.id()`. Previously `makeTexture` stored a
filename but left that ID at zero. It now allocates a live native texture ID
and type 7. `scene_instances_texture` resolves its canonical wrapper's file;
file-texture duplication creates an independent handle, and aliases carry the
type so career saves can serialize purchased decal sets. The PNG serializer
also accepts these file resources. Loaded image resources stay alive while
native resource wrappers refer to them (`gameref_uses_resource`).

Saves. `Part.save` and `Vehicle.saveSkin` write the texture through
`File.write(ResourceRef)`: `ResourceRef.set` types it 7 and its save blob
(SDAT trailer or JSON `blobs` entry) carries a PNG (`paint_png_encode`,
fixed-Huffman deflate).
`src/natives/natives_io.cpp:sdat_load_texture_blob` restores it on
`readResID`; an empty stock blob reads the JPEG side file `<save>.<ordinal>`
that the original game writes. Stock MWM saves with painted parts therefore
load their paint. OpenSLRR saves are not meant to be read by the original
executable.

Paint-can preview follow-up. Paint cans share the resource flags (640) used
by the host's reconstructed world-cue animation. Applying its half-metre bob
and transparency in the tiny inventory cells moved the cans partly or entirely
offscreen. `src/resources/scene_instances.cpp:collect_root` now applies that
world-space animation only when collecting the active world, not the separate
local worlds of inventory previews. The stock script's preview camera remains
authoritative. The real-asset `trigger_markers` regression checks stationary,
opaque paint cans in a local preview and preserves the active-world marker's
rotation/bobbing and sensor behavior.

Decal picker. Painter is the only stock user of the legacy
`Osd.createButton(rid, ...)`: `Osd.createIcon` creates a `native button`
type (`misc.garage:0x008C`) in the Osd group and `Osd.event_handler` turns its
`EC_LCLICK` into `osdCommand(CMD_DECALBUTTONS + i)` by matching the object's
`id()`. `src/natives/natives_gameref.cpp:bind_icon_button` gives that GameRef
a live id and a hit-only OSD widget sized from the cfg render mesh; the click
handler in `src/boot/boot.cpp` emits `EC_LCLICK` from the icon instead of an
osdCommand. Before this the slots took no clicks, so only the first decal of
the shown page could be painted.

The garage builds the Mechanic's parts bin even when it reopens straight
into body paint. Its never-shown `InventoryPanel` Osds had visible hit boxes
at layer 15 over the paint-can and decal slots, so after a catalog visit those
clicks reached the parts bin. `osd_on_field_write("vp")` now keeps an Osd's
widgets hidden until its viewport is activated (ENGINE_CONTRACT.md).

Decal thumbnails. `setDecalButtons` duplicates the button render type,
swaps its texture for the decal and creates
`RenderRef(slot, copy.id(), "decal button")`. A duplicated render type is now
a live resource (`ResourceRef.duplicate` in `src/natives/natives_resref.cpp`),
so `copy.id()` aliases the copy and the instance inherits its replacement.
`RenderRef.create` (`src/natives/natives_renderref.cpp`) then draws that
texture as a flat picture over the slot's hit box, and destroying the instance
removes it. The button mesh itself is not drawn.
Projection/preview behavior remains the host approximation described above.
