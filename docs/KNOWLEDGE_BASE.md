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
- Asset test suites (`car_lights`, `swaybars`, `race_vehicle`, `nitrous`,
  `stock_engine_power`)
  read the stock install from `game/` (else `data/game`) relative to the repo
  root. If `patch/` has been copied into that `game/`, they fail: patched
  racer classes install sway bars and the headlight checks differ. To run
  them on stock, start the compiled `build/<suite>-tests/<suite>-after.exe`
  from a short folder (MAX_PATH) holding a `game` junction to a stock install
  and a `patch` junction to the repo's `patch/`. On 2026-11-23 the merged
  #215 branch passed them this way. `stock_engine_power` installs `patch/`
  over the game itself, like the patched swaybars case. `port_flow` instead
  needs `data/game` = stock with `patch/parts` (minus `src/`) installed over it.
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
  changes live in `patch/` with their source (`patch/sl/Scripts/game/src/OptionsDialog.java`,
  `patch/sl/Scripts/game/src/Garage.java`, `patch/system/Scripts/render/src/*.java`).
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
  limit), `--auto-cmd mainmenu|street|escape|N`, `--vehicle-osd PATH`,
  `--wind M/S[,HEADING]` (ENGINE_CONTRACT §11), a positional player name,
  and the headless probes `--probe`, `--probe-gc`,
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
- Runtime environment variables: `SLRR_GAME_ROOT` (`src/main.cpp`),
  `SLRR_VLC_DIR` (`src/platform/video_fmv_vlc.cpp`) and `SLRR_AMBIENCE_LOG`
  (`src/resources/scene_ambience.cpp`, debug logging of random ambience
  requests). `SLRR_FMOD_SDK_DIR` and `SLRR_TOOLS_CACHE_DIR` are build-time
  only (see README.md).
- Automated checks on `main`: the only CTest target is `audio_recovery`
  (`-DBUILD_TESTING=ON`, `CMakeLists.txt`). Every other regression is a
  standalone `tests/test_<name>.cpp` built and run by
  `scripts/run_save_io_tests.ps1 -Suite <name>` (suite list in its
  `ValidateSet`); `tests/*.py` run with Python. The older CTest suite from
  PR #3 (`8637eae`) was deleted by `b907b6b`, and no CI workflow exists in
  the repository. The probes `--probe` and `--probe-gc` need a game root;
  `--probe-dyno` and `--probe-pacejka` do not.
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
| Platform | `src/platform/host_input.cpp`, `src/platform/host_audio.cpp`, `src/platform/host_sfx.cpp`, `src/platform/host_music.cpp`, `src/platform/video_fmv.cpp`, `src/platform/video_cinepak.cpp`, `src/platform/host_game_options.cpp` | Input, audio, FMV (built-in Cinepak AVI decoder), options |
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
traffic, `scene_ambience_tick`, triggers, camera follow, mechanic and world
pointers, `part_physics_begin`, `render.flush()`, `part_physics_end` and the
quit check. Do not move an event delivery or add a second pump for the same
port.

Physics (#203): `src/resources/part_runtime.cpp:part_physics_begin` counts
the 60 Hz host ticks owed, runs `scene_instances_prepare_physics` and
`src/resources/part_runtime.cpp:prepare_batch` on the host, then starts the
batch on the physics thread (`src/resources/part_runtime.cpp:simulate_batch`).
The frame is drawn meanwhile, from the poses the previous frame published.
`src/resources/part_runtime.cpp:part_physics_end` waits for the batch,
publishes it (`src/resources/part_runtime.cpp:commit_batch`), then runs
engine audio, hinged panels (`src/resources/part_runtime.cpp:advance_flaps`),
render poses, steering wheels, driver pose and pedestrians. Scripts of the
next frame therefore see the ticked state, as before; the drawn frame is one
frame behind it. `part_physics_advance` does both halves at once (tests).

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
3. Each 60 Hz host tick configures the dynamics and calls
   `VehicleDynamics::advance` for two 120 Hz substeps
   (`include/slrr/vehicle_dynamics.hpp:VehicleDynamics`). At most four host
   ticks run per frame; excess time is dropped. The ticks of a frame form one
   batch on the physics thread: `prepare_batch` configures each car for the
   first tick (`configure_drive`) and copies its body; the later ticks of the
   batch refresh only the steering (`src/resources/part_runtime.cpp:sim_refresh_drive`).
4. Wall contact is `collide_chassis`: four 0.18 m sphere probes plus oriented
   chassis-box contacts against the static index. A released car enters it
   when `drive.torque`, `engine_lost` or `drive.native_traffic` is set.
   `configure_drive` handles player and autonomous mechanical cars, an
   engine-lost car's zero-torque neutral drive, and the distinct native
   traffic force path. Wall collision, impact damage and mileage are no
   longer player-only; damage options still distinguish the player
   (`src/resources/part_runtime.cpp:sim_step_car`,
   `src/resources/part_runtime.cpp:configure_drive`). On the physics thread
   impacts are only recorded; `commit_batch` applies them with
   `damage_chassis` in solver order, with the chassis as it was when hit.
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
   (`src/physics/flap_hinge.cpp:flap_step`). `Part.flap`/`flap_toggle` push
   it with build 606's spring (#327, below), `damage_chassis` hands each impact to
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
11. Aerodynamics are real-life `½ρCAv²` forces: body drag, side force and
   lift over the chassis bounds, `Chassis.C_drag` at `drag_center` (read by
   `src/resources/part_runtime.cpp:configure_drive`), and wing faces from the
   installed parts' CFG `wing` lines
   (`src/resources/part_config.cpp:parse_part_config`,
   `src/resources/part_config.cpp:part_wing_area`), collected in chassis
   coordinates by `src/resources/part_runtime.cpp:rebuild_aerodynamics`. All
   act in `src/physics/vehicle_dynamics.cpp:VehicleDynamics::apply_aero`, on
   the car's velocity relative to the wind that
   `src/resources/part_runtime.cpp:sim_step_car` sets on released cars from
   `src/physics/vehicle_dynamics.cpp:wind_velocity`, with the wind and clock
   `src/resources/part_runtime.cpp:prepare_batch` copies into the batch
   (`--wind`, `src/resources/part_runtime.cpp:part_set_wind`).
   ENGINE_CONTRACT §11 has the coefficients and the original formulas.

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
  (`src/resources/part_runtime.cpp:sim_free_bodies`), which reuses the
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
  with a heading, 1 m radius and 1 m/s. Walls and both runs repel them (soft
  steering within 3 m) and push them back to 0.5 m, an OpenSLRR choice: many
  stock corridors and gates are only 2.2 m wide, and a 1 m clearance left
  walkers pinned on gates (#272); the four strongest influences steer; they
  leave a row only through a gate. Stock gate links form closed block rings, so no
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
- `osd_rpm/speed/gear/mileage` spell their value only when the render's RSD
  `flags` has bit 4 (stock lcdfont/osdfont/slider renders: `flags 4.000`);
  build 606 picks its font batch from that bit (0x0052CF2C). Any other render
  is drawn whole like `osd_gauge`: Easy Forza OSD puts its gauge plate
  (`cars:0xBAA4`, `flags 1152.000`, a one-quad SCX) on `osd_rpm`
  (`include/slrr/vehicle_hud_damage.hpp:hud_render_draws_text`). A loose
  `--vehicle-osd` cfg qualifies dependency slot 2 against the installed
  `cars.rpk`, exactly as an installed car cfg does; it has no per-pack file
  fallback (`src/render/bgfx_osd.cpp:draw_vehicle_hud`).
- CFG mesh overrides (`osd_gauge`, needles, non-font readouts) use the Osd
  camera of `Osd.java`: `CAM_OFFSET` 5.48, 30 degree vertical view, so the
  z = 0 plane spans +-1.469 (`SCALE_3D`) vertically and +-1.469 x aspect
  horizontally, scaled by 5.48 / (5.48 - z). Font readout anchors are screen
  NDC, y down. The stock table keeps its own 16:9 calibration.
- HUD values come from `src/resources/part_runtime.cpp:part_vehicle_hud`:
  RPM, km/h, mileage, nitro, turbo, gear, and the damage gauge's parts.
  RPM and km/h come from `src/resources/part_runtime.cpp:gauge_rpm` and
  `src/resources/part_runtime.cpp:gauge_speed`, which the dashboard pins
  (`cockpit_rpm` / `cockpit_speed`, see "Dashboard pins" at the end) share.
- Damage gauge (`osd_damage`, stock font `cars\meshes\osdfont.scx`): glyph
  `A` is the car outline, `E`/`L`/`T` the drivetrain, `O h l r p q t u v` the
  body and `a b c d` the wheels, in one coordinate system with the front up.
  `src/resources/part_runtime.cpp:hud_damage_parts` lists the installed
  engine, body and wheel parts with their rest-installed top-down centre,
  normalised to chassis renders, and their condition `wear × tear`
  (`GAME:parts/scripts/src/Part.java:getConditionNoAttach`). Closed CFG mount
  frames retain deliberate `setSlotPos`, not flap/damage offsets; removing a
  panel does not change valid chassis-only normalisation bounds.
  `src/render/bgfx_osd.cpp:draw_vehicle_hud` delegates region ownership to
  `include/slrr/vehicle_hud_damage.hpp:hud_assign_damage`: for the stock font,
  only script `Quarterpanel` instances (including subclasses) own `q/p/u/v`, selected by
  left/right and front/rear. Accessories cannot colour or resurrect a missing
  fender. Other regions retain smallest-containing-quad aggregation (wheels
  by corner). This is host mapping, not recovered original native behaviour.
  The stock `q` UVs reverse the longitudinal axis of the asymmetric front
  fender tile, placing its bright edge beside the engine. For the stock
  cars damage font and exact stock UV layout only,
  `include/slrr/vehicle_hud_damage.hpp:hud_stock_left_fender_uv` uses a
  horizontal mirror of `p`; shared SCX/PNG assets and custom UVs stay untouched.
  Custom fonts keep the previous spatial region mapping.
  Regression target: `vehicle_hud_damage` (`BUILD_TESTING=ON`); optionally pass
  the original `cars/meshes/osdfont.scx` path for a real-asset integration check.

### 5.9 Options and saves

- Options mirror `java.util.Config` and save to `save/game/options` through
  the stock Java `Config.saveConfig` after a 500 ms debounce, plus a final
  save at shutdown (`src/platform/host_game_options.cpp:host_game_options_update`).
- Enhanced options (MSAA, reflections, clouds, sunlight, local lights) are
  session-only and exposed through `OptionsDialog.get/setRendererOption`
  (`src/natives/natives_script.cpp:register_script_natives`).
- File saves write version 2 JSON at the existing paths (#291): named career,
  car (part tree, per-class tuning data) and skin layouts, and a typed
  `values` list for other files. Schema: [SAVE_JSON_SCHEMA.md](SAVE_JSON_SCHEMA.md).
  Version 1 JSON, SDAT type 1/type 2 and raw streams are still read; a version
  1 or SDAT file under `save/` read to its last byte is rewritten as version 2
  on close. Resource paths remount independently of session pack order;
  scripts keep binary cross-type read semantics. See ENGINE_CONTRACT §15
  (`src/natives/save_json.cpp:encode`, `src/natives/save_json.cpp:decode`,
  `src/natives/natives_io.cpp:json_frame`,
  `src/natives/natives_io.cpp:json2_take_payload`,
  `src/natives/natives_io.cpp:json_take_payload`,
  `src/natives/natives_io.cpp:sdat_take_payload`).
- Lessons from #291. Before it, cars looked stuck in SDAT: a `PlayerCarN` is
  rewritten only when that car is saved (`CarLot.saveCar` on the current car),
  so untouched cars never left the legacy format. Conversion therefore happens
  on a complete read, which the car lot does through `save/temp/` when a car is
  shown; the career folder gets the JSON copy on the next save
  (`GAME:sl/Scripts/game/src/GameLogic.java:save` copies `PlayerCar*`).
  The flat stream cannot name part data on its own: subclasses (`Tyre`,
  `Block`, mod parts) append versioned fields after `super.save`, so the host
  records which script frame wrote each value
  (`src/jvm/jvm.cpp:set_script_frame_hooks`,
  `src/natives/natives_io.cpp:save_frame_enter`). `GameLogic.save` writes the
  racer list without a count; the encoder recognises a racer by its
  int, int, float prefix followed by a `VehicleDescriptor` frame.

### 5.10 Threads and the worker pool

Read this before adding parallel work. The contract is
[ENGINE_CONTRACT.md §6](ENGINE_CONTRACT.md#host-threads-and-the-worker-pool);
this section is the working guide.

**Threads that exist.**

| Thread | Owner | Runs |
| --- | --- | --- |
| Host thread | `src/boot/boot.cpp:boot_game` | VM and script fibers, frame pump, physics preparation and publication, scene collection, bgfx submission, every GameRef and cache mutation |
| Physics thread | `src/resources/part_runtime.cpp:PhysicsWorker` | The bodies of one frame's physics batch (#203), while the host draws the frame |
| bgfx render thread | bgfx (`src/render/bgfx_lib_backend.cpp:open`) | Graphics API calls for the frame the host submitted |
| Worker pool, one per logical CPU minus one | `src/platform/job_system.cpp:jobs_parallel_for` | Data-parallel batches started by the host thread |
| Exterior map parser | `src/natives/natives_resref.cpp:spawn_ground_shell` (`std::async`) | `scene_instances_ground_map` for the next chained map file |
| Pedestrian stepper | `src/resources/part_runtime.cpp:PedSim` | Walker steering on copies of the walker records |
| Audio and video | FMOD Core (effects/music/Cinepak PCM), optional VLC/Windows codecs | Their own decode and output threads |

**Physics thread (#203).** `part_physics_begin` prepares a batch on the host
and starts it; `part_physics_end`, after `render.flush()`, waits for it and
publishes it. A batch (`SimBatch`) holds copies of every car (`SimCar`),
loose body (`SimLoose`) and free PhysicsRef body (`SimFree`) it steps, with
the inputs they need: drive and steering set up by `configure_drive`, input
and options, the crush cage's shift of the wall probes, nitrous capacity and
rate, car shells for loose-body contacts, and dyno curves as immutable copies
(`include/slrr/dyno_model.hpp:dyno_torque_function`). On the physics thread
it reads only those and the scene collision `scene_instances_prepare_physics`
built for it. It never touches the VM, GameRef, `records`, audio or bgfx; it
records impacts, props to wake, loose bodies to settle or remove, and
diagnostics, which `commit_batch` applies in solver order.

Rules for host code:

- Between `part_physics_begin` and `part_physics_end` only rendering runs.
  Code that may run there and changes what a batch reads must call
  `part_physics_join()` first. The scene collision mutators
  (`scene_instances_prepare_physics`, `scene_instances_select`,
  `scene_instances_add_static_mesh`, `scene_instances_add_ground_props`, ...),
  `part_destroy` and the tyre tuning in the debug overlay do.
- `part_physics_join()` is a no-op on the physics thread and without a batch.
- A batch is committed before the next one starts, before a new world
  (`register_part_natives`) and in `part_physics_advance`.
- `--physics-inline` runs the same batch on the host thread. From the same
  starting state the fixed-step benchmark gives the same states in both
  modes. The start itself can differ by a fraction of a millimetre between
  runs: the frames before the benchmark starts use wall-clock time.

What changed against the single-threaded tick: the drawn frame shows the state
before this frame's ticks (one frame of display latency); damage, crush cages,
detaching, waking props and loose-body settling apply at the end of the batch
instead of between its substeps; a crush cage moves the wall probes as it was
when the batch started; race AI decides once per frame; a parked car woken
during a batch keeps its drive until the next frame configures it. With one
tick per frame (60 FPS) only the display latency and the cage timing differ.

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

**Per-frame work on the pool (#266).**
`src/resources/scene_instances.cpp:scene_instances_collect_batches` lists the
visible cells on the host thread, then runs the placement tests (flags, LOD,
frustum, far-size filter, reflection size) over contiguous runs of cells, a
few runs per thread. Each run fills its own list of (batch slot, placement),
and the host thread appends the runs in order, so every batch lists its
placements as the serial loop did. The far-size filter
(`src/render/bgfx_lib_backend.cpp:screen_size_visible`) only reads backend
state that the batch does not change. The LOD level and epoch of a placement
are the only values written during the batch, from any job, through
`std::atomic_ref` in `src/resources/scene_instances.cpp:prop_lod_visible`;
`scene_prop_lod_level` is idempotent, so a job that reads a level another
job has just stored gets the same answer. Fewer than 16 visible cells, or
fewer than 4 pool threads, keep the loop on the host thread (on two cores the
pool was slower than the serial loop). A new per-frame user must follow the
same rules: lazy caches and bgfx stay on the host thread, results are
appended in serial order. Walker palettes (#268) follow it too:
`src/resources/scene_instances.cpp:scene_instances_collect` finds or loads
each character on the host thread (`character_skin_prepare`), the jobs run
`character_skin_palette_prepared` for different characters
(`src/resources/character.cpp:rebuild` touches only its own character and
reads its immutable skeleton and clip), and the host thread publishes the
palettes into the frame cache in order.

**Worker bgfx encoders (#270).** On by default (renderer debug option
"Worker encoders", `render_debug().worker_encoders`). When on,
`src/render/bgfx_lib_backend.cpp:parallel_submission_ready` is true with 3 or
more pool threads; `queue_or_run` then queues the AO prepass
(`src/render/bgfx_ao.cpp`), each car lamp tile (`src/render/bgfx_shadows.cpp`)
and each reflection face draw instead of submitting it, and
`src/render/bgfx_lib_backend.cpp:run_worker_submits` runs the queue on the
pool while the host thread draws the sky and the world on encoder 0. Rules:

- One job per Sequential view, so the order inside a view is the serial one.
  Each job calls `bgfx::begin(true)` and sets the thread-local `t_encoder_`
  and `t_bind_`; every draw helper goes through `enc()` and `bind()`. The host
  caches encoder 0 in `host_encoder_` (reset at `bgfx::shutdown`): one
  `bgfx::begin()` or TLS lookup per draw call doubled world submission.
- Binding caches (#246 light arrays, #262 uniforms and textures) live in a
  `BindState`; each worker owns one (`worker_binds_`), the host uses
  `main_bind_`. The harness reads `main_bind_` counters.
- No shared writes from jobs: `draw_prop_batches` takes the shadow-local
  choice as a parameter instead of toggling `static_shadow_local_`;
  `snapshot_textures` resolves every texture of the queued objects into
  `resolved_textures_` first (lookups that would insert run on the host);
  `upload_light_data` runs in frame setup and is skipped inside the window;
  profiler zones are off on worker threads (`perf::worker_thread`), whose time
  is summed in `worker_submit`, with the host's wait in `submit_wait`.
- `draw_prop_batches` allocates one instance buffer per list: bgfx takes its
  resource lock per transient allocation, and a granted count can be smaller
  than asked when another encoder allocates in between.
- Fewer than 3 threads, or the option off: the queued draws run on the host
  thread in submission order.

Harness (`tests/test_scene_shadow_gpu.cpp`, crowd city 1280x720 with lamp
tiles, GTAO and a reflection probe; lamp fades use a fixed step so the runs
compare): threaded and one-thread images are identical at 24, 2 and 1 cores.
At 24 threads host submission was 1.54 ms threaded against 1.61 ms on one
thread, worker submission 0.50 ms, host wait 0.002 ms; bgfx's shared submit
state (sort keys, atomics, transient buffers) took back most of what the host
saved, so the option first shipped off. In game (user, 21 November 2026,
Valo City at night, 2560x1440, MSAA 4x, HBAO high, 40 lamps, 6 shadowed,
12 car tiles; `build/Release/perf/renderer_debug_20261121_031919.txt` off,
`_031932.txt` on) the host gained: renderer zones 5.80 → 5.13 ms, frame
8.15 → 7.35 ms, world submission 1.96 → 1.88 ms, worker submission 0.85 ms
summed, host wait 0.002 ms, render thread unchanged (4.15 / 4.19 ms). The
user then made it the default; the two views were close but not identical
(1032 / 1014 instances).

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
  the results. Steady per-frame work stays on the host thread, except the
  placement loop of `scene_instances_collect_batches` (#266). §5.10 is the
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
- An unassigned reference field or static reads as Void and compares equal to
  null (`src/jvm/tree_eval.cpp:values_equal`). RacerTalkDialog builds its
  static reply texts only `if (Text_honkin == null)`; before #226 every racer
  reply except the literal lines was blank.
- Timers belong to their GameType. A section change neither removes nor drops
  them; EVENT_TIME is gated only by the event mask
  (`src/boot/section_machine.cpp:change_active_section`,
  `src/boot/mainloop.cpp:dispatch_due_timers`). Stock sections clear their own
  timers or mask in exit(). City's one-second tick (police, racer challenges,
  night races, the garage trigger) keeps running through RaceSetup and checks
  actualState itself; before #226 it stopped for the rest of the visit after a
  day race was set up.

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
- The intended vehicle solver is maintainer revision `8ac2ba9`, not the
  earlier #48 curve. `src/physics/tire_model.cpp:tire_forces` scales reference
  Magic Formula curves by clamped ratios of script friction, stiffness and
  sliction, with progressive load efficiency, lateral load smoothing and a
  combined friction ellipse. Stiffness is a B multiplier, not an exact slope.
  `setCPatch` tolerance and `setInstantCenter` data are copied/stored but have
  no active solver effect; there is no jacking or shared lateral impulse
  bound. The approved brake fix consumes one impulse budget before tyre
  force and holds the reaction with the unused budget; raised launch take-up
  still requires the service brake below 0.05
  (`src/physics/vehicle_dynamics.cpp:VehicleDynamics::step_drive`).
  See ENGINE_CONTRACT §11 and [the migration plan](VEHICLE_DYNAMICS_PLAN.md).
- Installed body low points are cached from real mesh vertices, not the
  chassis AABB's empty underside corners
  (`src/resources/part_runtime.cpp:rebuild_body_support`). Hidden meshes keep
  their supports; corner gear and hinged panels have separate contact paths.
  Slot poses, deformation, repair and assembly changes invalidate the cache.
- Missing-spring fixed-travel support is solved implicitly with body contacts
  in the current substep, before the tyre law consumes the load
  (`src/physics/vehicle_dynamics.cpp:solve_fixed_support`). A compression limit
  missing from scripts uses CFG `base - travel`. Rim-less hub hardware has a
  metal contact, not the previous tyre's grip. Static friction is projected
  after forces/gravity and before pose advance
  (`src/physics/vehicle_dynamics.cpp:solve_rest_contacts`). Rubber sticking
  also respects the active tyre model's anisotropic zero-slip capacities
  (`src/physics/tire_model.cpp:tire_friction_limits`), so low grip or zero
  longitudinal grip cannot borrow a stronger parking coefficient. Controlled
  longitudinal brake holding is additionally limited by its unspent brake
  impulse, independently of lateral grip. This adds no new chassis
  sleep/wake state machine.
- A CFG `body` line is mass and collision; it is not a crush zone. Do not size
  a deformation cage from the phys SCX bodies. The assembly mass properties
  integrate the closed phys hulls (815 of 818 stock racer hulls in the earlier
  audit), with a 30%-of-render-box moment floor for chassis configuration.
  The generic rigid-body fallback now uses 0.75 times the bounds dimensions
  (`src/physics/vehicle_dynamics.cpp:RigidBody::configure`); a supplied inertia
  vector replaces it only when every component is positive and finite.
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
| Tyre reference scaling (`8ac2ba9`) | friction / 1.76863, stiffness / 17.6839, sliction / 0.814, each clamped; nominal lateral peak multiplier 1.42 before load/slip factors | `src/physics/tire_model.cpp:tire_forces` |
| Hinged panel step / latch release / tear-off / script spring | 240 Hz / 3.5 m/s local impact speed / 4.5 m/s contact speed or 0.15 m blocked overlap / 60 s⁻² toward 95% open (build 606), damping 2.3 s⁻¹ | `src/resources/part_runtime.cpp:kFlapStep`, `include/slrr/flap_hinge.hpp:FlapTuning` |
| Prop cell | 64 m | `src/resources/scene_instances.cpp:kPropCellSize` |
| Shadow maps | 2048² | `src/render/bgfx_lib_backend.hpp:kShadowResolution` |
| Pedestrian radius / density scale | 400 m / ×4 | `src/resources/part_runtime.cpp:tick_pedestrians` |
| Walker body radius / speed | 1 m / 1 m/s (original) | `src/resources/pedestrian_lines.cpp:kRadius` |
| Walker hard wall clearance | 0.5 m (OpenSLRR) | `src/resources/pedestrian_lines.cpp:kWallClearance` |
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
`GameRef.setActiveCollision` has a body since PR #22 (§12). One host-only
no-op outside the stock table: `LoadingScreen.<init>`. `Player.<init>` runs
its Java body (#226): without it the boot player had no `parts`, `carlot` or
`osd`, so `--spawn-car` sessions left the game from Car Lot.

Partial bodies that matter for upcoming work: `GameRef.setState` applies to
cars only, `GroundRef.getNearestCross` returns null,
`RenderRef.plotRoute` returns 0, `GroundRef.setFog` stores values nobody
reads, and `Part.setSfxLoopParams` stores values without applying them.

## 9. Where the documentation disagrees with the code

| Statement | Code reality |
| --- | --- |
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
| Released cars retain static-world contacts without a driver/engine. Shared chassis OBB pairs and all-car movable-body contacts now use impulse-aware damage/detachment; full Bot recovery/audio/seating remain partial. See [traffic/race audit](TRAFFIC_RACE_AUDIT.md). | `src/resources/part_runtime.cpp:simulate_batch`, `src/resources/part_runtime.cpp:sim_loose_bodies`, `src/resources/part_runtime.cpp:sim_free_bodies` |
| Gamepad fallback is now per driving axis and unclaimed physical channel; hardware validation remains separate | `src/platform/host_input.cpp`, `src/platform/host_input_mapping.hpp` |
| The unused offscreen scene target, HiZ pyramid/readback and legacy projected car shadows are removed (#105). World rendering uses the swap chain; the retained projected-size filter does not perform occlusion. Shader creation releases caller handles on success and failure. | `src/render/bgfx_lib_backend.cpp:LibBgfxBackend::open`, `src/render/bgfx_program.hpp:create_owned_program` |

Fixed on `main`: `System.arraycopy` overflow, PTX bounds, dyno session
lifetime and sampling, and the `Vehicle.getHorn` / `hasCrime` registrations.

## 11. Open issues: verified starting points

These are entry points, not fixes. Each needs its own reproduction.

- **#15 HUD too small** (closed; PR #20 merged). Every stock HUD element comes from the
  table in `src/render/bgfx_osd.cpp:draw_vehicle_hud`. Speed, gear and
  mileage are drawn left-aligned at their anchor, so centring needs the
  string width. CFG `osd_*` overrides take separate branches.
- **#16 car drives through trees** (closed; PR #22 merged). `ground_prop_config` now keeps
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
- **#203 physics-clock/engine-math spike.** The default remains 60 Hz host
  ticks with 120 Hz shared-world substeps and interpolated drawing. Fixed
  stepping is not independent wall-clock scheduling: excess frame time is
  discarded. The integer shared-world loop would lose 4% of simulated time
  with a bare 1000 Hz override. Rate experiments, throttle integration boundaries,
  adaptive-setting references and dyno/Java compression couplings are recorded
  in [PHYSICS_TICK_SPIKE.md](PHYSICS_TICK_SPIKE.md). The owner permits changed
  engine outputs: cycle torque is now SI net crank Nm, sampled power is T·ω,
  and driving consumes that curve without the legacy ×2 gain or duplicate WOT
  loss. The shipped Java head/Inline/Vee patch normalizes capped curtain area by
  a declared 1-ms ideal-air reference exchange volume, replacing X²/V. Native
  DynoData fields remain unchanged; full pressure-dependent flow, fuel calibration
  and independent-thread ownership remain open. The actual-solver throttle
  experiment covers digital/analog plate response, cut hysteresis and latched
  BOV events without enabling new production controls. `interpolation_ms`
  measures pose publication inside Physics, not downstream renderer caches.
- **#352 stock engine power.** After #215 stock maxHP fell to 36-100% (65% on
  average over the 24 stock racers). The fuel-heat patch now keeps the stock
  heat (handbook value commented out, so mods keep their scale), the port-flow
  law stays, and `include/slrr/dyno_model.hpp:engine_stock_calibration` (2.8)
  scales indicated torque so the racers keep their previous maxHP on average
  (45%-152% individually). `tests/test_stock_engine_power.cpp` pins each racer.
  Without the port-flow law, exactly 2 reproduces the legacy power formula.
  Details: [PHYSICS_TICK_SPIKE.md](PHYSICS_TICK_SPIKE.md#stock-calibration-352).

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
    not `sl2_pacejka.cpp`. Revision `8ac2ba9` scales reference curves by script
    friction, stiffness, sliction, load capacity/smoothing and `frictn_x`;
    `configure_drive` also copies the patch tolerance, but the solver does
    not use that field (`src/resources/part_runtime.cpp:configure_drive`).
    Native traffic has its own load-limited force path within the same
    `VehicleDynamics` owner, not this mechanical-tyre curve.
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
night detail vertex path used a 0.002 m normal offset to reduce coplanar depth
fighting, without depth writes; since #281 it is a relative depth bias instead
(see "Emissive overlays sit flush" below). Onset remains 18:30. Signs only emit through their authored masks as well.

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
The overlay depth handling (now #281's depth bias) and 18:30 onset remain unchanged.

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

`src/resources/rpk_index.cpp:RpkIndex::read_payload` reuses at most 64 lazily
opened pack streams in an LRU under a mutex, reopening evicted packs on demand.
Text/ICFG descriptors are cached by canonical full RID; large binary map
payloads are not retained as another resident copy. `payload_props` parses all text keys once under its own lock
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

`src/resources/rpk_index.cpp:RpkIndex::parse_rpak` reads only the header,
dependencies and TOC, leaving payloads on disk until first use. It checks file
sizing, 64-bit dependency-count sums, dependency/table spans, record count versus
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

FMOD decoded sample sounds are shared between voices and held in a
16 MiB decoded-PCM / 64-entry LRU cache. Samples are prepared for looping at
load time; each channel selects loop or one-shot playback independently.
Playing voices retain their sound even after cache eviction; root reset
stops voices and clears cached data/counters. `host_audio` owns the shared
FMOD mixer with independent effects/music groups; music streams are updated
through the existing frame pump even when only effects are playing.
Sounds above the cache limit can play up to the 64 MiB per-effect bound,
checked with an open-only metadata probe before full decode. `wave_reads`
counts uncached load attempts (probe plus decode), not OS file reads. Loop pitch
and gain updates now reject non-finite inputs via the same finite defaults
as play_file. HostSfxStats exposes rejection/read/hit/cache diagnostics.

`src/platform/host_music.cpp` tries at most one playlist pass when opening
a track fails, preserving next/previous direction. A stream failure advances
through the same bounded recovery path. A wholly unreadable set remains
stopped; update does not continuously retry it. Explicit change/next can
retry later. HostMusicStats exposes attempts/failures/current track for
diagnostics. No Java native or music-set/mode contract is added.

Pre-FMOD validation: Release host compiles. audio_recovery creates only digital-silence
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

Video without VLC (#216): 64-bit Windows ships no Cinepak codec (only the
32-bit `SysWOW64\iccvid.dll`), so without LibVLC both the AVIFile and the
DirectShow paths failed and every intro and `prime.avi` was skipped.
`video_fmv_open` now tries `src/platform/video_cinepak.cpp` first: it indexes
the AVI, decodes Cinepak frames in order into BGRA and plays PCM audio through
a dedicated FMOD channel/group, whose position drives the frame clock.
The FMV pump updates the shared mixer during boot intros, and EOF/channel
failure falls back to wall time from the last valid position without jumping
the picture. PCM is staged in a WAV memory sample (unsigned 8-bit or signed
16-bit, mono/stereo) and released before its shared mixer owner. Looping
replays the same sound with a fresh channel. Files it rejects
(another codec, or a first frame it cannot decode) still go to LibVLC, then
AVIFile/DirectShow. The video_safety suite checks the decoder and a generated
Cinepak AVI with PCM audio.


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

`prepare_batch` resolves the controlled chassis and validates its script
frame once before the batch. Loose panels can have different owner IDs;
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


### Driver head in the cockpit camera (#320)

- `src/resources/part_runtime.cpp:pose_driver` aims the seated driver's neck
  at the car's first cfg camera, 7.2 cm below it, so camera 0 sits in the
  head. The cockpit camera also leans with acceleration (`cockpit_offset`, in
  `part_camera_pose`), and the face then filled the view (seen in a bench
  drive: the driver's face from the front at full throttle).
- While this car's `_hostCockpitCamera` is camera 0 and is the scene camera
  (`fullscreen_script_camera`), `pose_driver` shrinks `Bip01 Head` to 1/1000
  about its joint with `src/resources/character.cpp:character_scale_bone`.
  Hair and face follow the head bone; neck, body and hands stay. The pose is
  rebuilt from bind every frame, so the head is back in every other view,
  for other cars and for the other internal cameras.
- Not checked: whether the original exe hides the head the same way (the
  view flag `+0x3C` of #301 is a candidate). The head is also missing from
  the car's shadow and mirrors while in the cockpit view.
- In game (`--bench-drive`, ValoCity, Baiern, F1 with #318): before, the face
  in front of the camera on six shots at full throttle; after, none. F3 back to
  the chase view runs unchanged.

### Driver hands on the wheel (#302, P2)

- `src/resources/part_runtime.cpp:pose_driver` aims each hand at the rim at
  ±1 rad from the top (radius = rim radius − 1.5 cm). It used to put the wrist
  7.8 cm behind that point along the seat's +Z and leave the hand on the
  forearm's bind orientation.
- Measured on stock Baiern and Enula, wheel centred and at both locks (bench
  drive, temporary log; the "palm" is the wrist plus 8 cm along the hand
  bone's X axis): the wrist target was always reached (reach 44–52 cm for a
  62 cm arm, no clamping), but the palm sat 2–3 cm inside the rim, 0.5 cm at
  Enula's right lock. On a raked column the seat's +Z is not the wheel axis,
  so the wrist landed 4.1 cm (Baiern) or 6.5 cm (Enula) out of the wheel
  plane instead of 7.8 cm.
- First fix (06610f4): the wrist on the elbow-to-palm line and the hand's X
  axis pointed at the rim. The palm point then sat within 1.2 mm of the rim,
  but in game the fists only rested on the wheel: the stock driver
  (`humans\drivers\vin.SCX`, 20 bones) has a single `Bip01 L/R Hand` bone and
  no finger bones; the fist is modelled closed.
- Grip: in hand space (bind pose, vertices weighted to the hand bone) the
  fist spans x 1.7–12.1 cm, palm toward -Z, and its grip hole is at about
  (9.5, 0, -1.2) cm, open along the hand's Y axis (both hands; the left one
  is mirrored in Y). `pose_driver` now puts that hole on the rim's centreline
  with Y along the rim, the palm (-Z) toward the wheel's centre and X as
  close to the forearm as that allows, solves the arm to the resulting wrist
  (two passes) and turns the hand with two `character_align_axis` calls (X,
  then Y, which is a twist about X).
- In game (cockpit view, Baiern, with #318 and #320 applied): the rim now
  runs into both fists. Not checked: steering locks in game, other drivers
  (the grip-hole numbers are for `vin.SCX`), P3 (gear-shift gesture).

### Gear-shift gesture (#302, P3)

- Build 606 (read-only disassembly of `StreetLegal_Redline.exe`): the chassis
  cfg line `shifter x y z [yaw pitch roll]` (handler 0x00444990) stores the
  lever knob at car+0x9AC. No stock racer has the line (only
  `cars\traffic\Wreck_data`), so the knob stays at the chassis origin.
- On a gear change (0x0044D0C0, gear at +0x1DF8, 0 = N, 1-6, 7 = R) the exe
  plays `sfx_gearchange`, sets two timers to now + 0.3 s and now + 0.7 s,
  and re-targets the console-side hand (right when the seat x < 0, otherwise
  left) to the knob plus an H-pattern offset per gear (table 0x0063C6B0,
  metres): N 0, 1 (-0.05, 0, -0.05), 2 (-0.05, 0, 0.05), 3 (0, 0, -0.05),
  4 (0, 0, 0.05), 5 (0.05, 0, -0.05), 6 (0.05, 0, 0.05), R (-0.09, 0, -0.05).
  When both timers have run out, 0x0044D260 puts the hand back on the wheel.
  The driver setup (0x00449200) reads the bone names `lfbone`, `rfbone`,
  `lhbone`, `rhbone`, `sthbone` and `gsbone` from the human, with Biped
  defaults; no stock human overrides them.
- OpenSLRR: `PartConfig::shifter` (parsed in
  `src/resources/part_config.cpp:parse_part_config`), and
  `src/resources/part_runtime.cpp:pose_driver` keeps the last gear per
  `DriverSeat`. A change starts a 0.7 s gesture counted in physics ticks (a
  pause holds it; releasing the seat cancels it). The console-side fist moves
  to the knob over 0.15 s, closes across the car with the palm down, and
  eases back over the last 0.15 s. The exe switches the hand at once.
- Tests: `cockpit_needles` case `parse_shifter_line`. In game (bench drive,
  Baiern, cockpit view): on the 1 to 2 shift the right hand leaves the rim for
  about 0.7 s and returns. With the knob at the chassis origin it goes below
  the view, as the stock data implies.

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
axes and the Y/reverse fallback are unchanged. SDL3 peripheral polling (#188)
now lives in `src/platform/host_input_devices.cpp`; device 2 preserves the legacy
pad schema while slots 3+ expose simultaneous signed joystick axes/buttons/hats.
Explicit wheel/pedal bindings replace the corresponding pad default, and analog
classification uses actual peripheral axis metadata. Unavailable signed bindings
are skipped to avoid a false midpoint value on focus loss or disconnect.
The `sdl_input` suite exercises these paths with SDL virtual devices.

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
Since #235 the shipped class is compiled from `patch/sl/Scripts/game/src/Garage.java`
instead, which keeps this order; the binary builder is kept as the #59 reference.

Validation: inventory_transition executes the actual shipped GameLogic transition,
Mechanic.flushInventory and Garage.osdCommand/exit with isolated presentation
callbacks. The original records zero transferred items at save and three after
exit; the patched handler records all three before save, with one exit/enter.
This verifies call ordering, not a complete career file round-trip or gameplay.

## Car Lot save ordering (#315)

MWM saves before entering the Car Lot: in the installed `Garage.class`,
`GameLogic.autoSaveQuiet()` is at source line 1520 and
`changeActiveSection(player.carlot)` at line 1521. The decompiled `sources/` Garage
has them the other way round, and since #235 the shipped class is compiled from
that order. The save then ran after `CarLot.enter` had parked the current car.
`GameLogic.save` brackets its car save with `carlot.lockPlayerCar()` and
`releasePlayerCar()`. The lock found no `player.car` and did nothing, and the
release handed the parked car back to the player (`setParent(player)`, outside
the lot's world) and cleared `carSlots[curcar]`. The lot drew every other car,
and the header still named the current one. Choosing another car then ran
`saveCar(old_curcar)` on a slot marked empty, which deletes
`save/temp/PlayerCar<n>` without writing it.

The fix restores the MWM order for this command only, and first flushes the
mechanic strip (`mechanic.flushInventory()`), so the save still holds what the
#59/#235 order protects. `Garage.exit` flushes again and finds nothing. This is
a script fix. The host's `setParent` keeps its meaning: a car parented to a
`Player` belongs to no world and is not drawn.

Lessons:

- A decompiled `sources/` class is not proof of stock call order. Before
  reordering or relying on statement order in a patched class, read the
  installed class's TREE. The source line of each node gives the order
  (`scripts/patch_garage_save_order.py` shows the TREE layout). MWM saves
  before the transition for the main menu, Choose Track, both dealers and Car Lot.
- A blank Car Lot slot whose header still names a car means the car left the
  lot's world after `CarLot.enter`. Look for a `releasePlayerCar` between enter
  and the first frame, not for a load failure.
- It was not caused by the JSON saves (#291): it reproduces at `5b002e4` with an
  SDAT career, and the stock MWM `Garage.class` shows the car on the current host.

Validation: `shipped_carlot_save_order` in `tests/test_inventory_transition.cpp`
runs the shipped `osdCommand(CMD_CARLOT)` with the real `GameLogic` transition
and `Mechanic.flushInventory`: three items are moved before the save, with one
exit and one enter. The recording probe expects `flush,save,change`. In game
(OpenSLRR host, scripted `--bench-career ... --auto-cmd 111`, not interactive):
a one-car JSON career and a four-car SDAT career both show the current car in
its slot. Before/after captures are in `assets/validation/carlot-current-car/`.
Picking another car in the lot afterwards was not played through.
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

### Cockpit camera as the scene camera (#318)

- Stock `Track.changeCamInternal` (F1) hides the follow camera on the OSD
  viewport, then calls `car.command("render <viewport> <controller> <cam>")`.
  The host answers with a `_hostCockpitCamera` per chassis
  (`src/resources/part_runtime.cpp:part_command`, verb `render`) that has
  `cockpit_index >= 0`, follows the car and has no parent.
- The ancestry check above then rejected it: the camera is not under the
  selected world, so no world camera remained and only the clear colour and
  the OSD were drawn. `src/resources/gameref.cpp:fullscreen_script_camera` now
  checks the followed car's ancestry for a cockpit camera, so a car stored in a
  local world (inventory, preview) still never supplies the scene camera.
- Tests: `map_views` case `cockpit_camera_is_scene_camera` fails on `main`
  (84517db) and passes with the fix. In game (`--bench-drive`, ValoCity,
  Baiern, F1 posted as WM_KEYDOWN): before, an empty world; after, the city
  seen from the car's first cfg camera.

### OSD plates over preview cameras (#348)

- `src/render/bgfx_osd.cpp` draws OSD plates either on `kOsdUnder` (view 7,
  below the preview cameras that start at view 8) or on `kOsdView` (70, above
  everything). A non-fullscreen plate that overlapped a preview used to go
  under it unconditionally, for the garage parts bar whose slot cameras live
  in the same viewport (InventoryPanel, priority 15).
- That put the driving GPS frame (`Player` Osd, priority 18, `frontend:0x0083`)
  under the navigator map (`Navigator.show`, viewport priority 12): the map
  covered the bezel's top and left edges and its glass. A plate now goes
  under a preview only when the preview's viewport priority is >= the plate's
  layer (its Osd viewport priority). A plate above a lower-priority preview
  stays on `kOsdView`, even when it is the lowest page-background layer.
- Catalog (Osd 10 / CarViewport 12) and MultiPlayer (Osd 10 / car viewports
  12) keep their plates under the cars.
- Validation: bench drive in ValoCity (Baiern, `--bench-shots`). Before, the
  map covered the frame; after, the frame is drawn over it. No automated test
  covers the OSD view choice.

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
A brain outside traffic finds its ground through the car's parents
(`src/resources/race_brain.cpp:car_ground`): nothing tags a script GroundRef or
native map with the GroundRef kind at run time, so before #226 day, quick and
night race opponents and pursuing police got no route and never moved.
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


### Shared shadow receivers (#201, first increment)

- `src/render/bgfx_lighting.cpp:LibBgfxBackend::bind_scene_shadow` binds the
  same field for scenery, vehicles and world reflection captures. Vehicle stage
  5 avoids the cubemap at stage 4. The source tag distinguishes the sunlight
  field from the existing garage-lamp field.
- `shaders/scene_shadow.sh` shares filtering/bias/fade across the mesh and
  vehicle shaders; only the associated direct light is attenuated. Ambient,
  environment reflection, emissive lamps and previews retain their own policy.
- `tests/test_scene_shadow_gpu.cpp` measures real D3D11 pixels: shadowed/lit
  receivers, disabled field, preview/emission isolation, capture-view parity and
  local-shadow isolation from sunlight. It does not validate full cubemap
  composition, cascades, addon light discovery or complete garage behavior.
- See RENDERING_ROADMAP.md and assets/validation/scene-shadow-foundation.txt
  for the current scope, evidence and remaining #201 work.


### Distance cascades (#201, second increment)

- `src/render/shadow_cascades.hpp:shadow_projection` builds camera-centred
  radial projections with fixed light-space basis and texel-snapped X/Y.
- `src/render/bgfx_shadows.cpp:LibBgfxBackend::update_static_shadow` renders
  four outdoor atlas tiles each frame, keeping garage local shadows separate.
  Radial splits are 32/100/320/1,000 m; off-camera scenery participates.
- `shaders/scene_shadow.sh` selects adjacent levels and crossfades their
  visibility. See RENDERING_ROADMAP.md for memory/CPU cost, cache retirement,
  the separate short-range vehicle-caster path and scene-detection limits.
- `tests/test_scene_shadow_gpu.cpp` checks distinct atlas clears/selection,
  blends and the 1 km fade through actual shaders, plus projection stability.


### Cascade CPU cost, filtering and anisotropy (#201)

- The outer shadow candidates are collected once per frame; the four levels
  cull their render-space bounds without rebuilding all scene records. Shadow
  collection opts out of character palette completion: these passes exclude
  skinned casters. Main/reflection character collection keeps the default.
- `src/render/scene_mesh_data.hpp:shadow_draw_groups` caches contiguous depth
  ranges across opaque material changes while preserving blend holes and
  cutout texture/channel/reference boundaries. Revision uploads rebuild them.
- Tent-weighted nine-tap bilinear comparison replaces the four-tap footprint.
  A thin-stripe GPU regression checks fractional response, but reported moving
  pole-shadow scintillation still needs manual validation; do not mark it fixed.
- `OptionsDialog.get/setRendererOption` index 6 controls anisotropic filtering
  (off/hardware maximum, default on), saved in openslrr_options and shared with
  the debug checkbox. World colour/detail samplers opt in, comparison maps and
  UI do not. Patched OptionsDialog source/container are shipped and installed.
- See assets/validation/shadow-performance-filtering.txt for measured changes;
  the previous cascade cost in scene-shadow-cascades.txt is historical.


### Stationary moving-sun shimmer follow-up (#201)

- User reports edge shimmer stopping when clock speed is zero. The numeric
  regression reproduces global-origin light-lattice jumps with a stationary
  receiver: see `tests/test_scene_shadow_gpu.cpp`. World-space UV thresholds
  and a wider PCF kernel alone did not address that projection defect.
- Cascades now rotate around a nearby shared origin, with caster model
  translations and receiver coordinates rebased consistently. The depth vertex
  shader separates model and view-projection transforms to avoid large-coordinate
  cancellation after rotation. Long-drive origin shifts align to all tile grids.
- An atlas is reused only when the sample matrices, origin and ordered caster
  stamps (geometry identity/revision, buffer handles, transforms) match exactly.
  Small sun changes and changed/moved casters invalidate it; no angular gate.
- Car shadows use comparison sampling before interpolation, not interpolation
  of raw depths followed by a binary step. GPU tests cover fractional edges,
  nonzero origin reception, origin rebase phase and real-map reuse/invalidation.
  See assets/validation/shadow-origin-stability.txt; manual user validation of
  the reported camera remains outstanding.

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

### AO silhouette streaks around cars and pedestrians (#201)

Reported at `905dee4`: with HBAO or GTAO on, dark lines spread on the
ground beside cars (rear wheel, bumper) and pedestrians' legs; AO Off removes
them. Both methods in `shaders/fs_ao.sc` sampled every receiver at the same
radial distances (`(k+0.75)/steps`), and only varied the direction rotation
with interleaved gradient noise. That hash is built for temporal accumulation,
which this AO does not have; on its own it varies slowly along diagonals. The
5-tap Gaussian filter could not remove either pattern, so the occluder's
outline appeared as shifted copies on the ground.

The main pass now takes rotation and step offset from a 4x4 tile
(`shaders/fs_ao.sc:hbao`, `shaders/fs_ao.sc:gtao`), and the separable filter
uses [1,2,2,2,1]/8, one full tile period. This follows the HBAO+ approach
(per-texel rotation and jitter from a small tile, then a matched blur).
Assetto Corsa's Custom Shaders Patch, which the user named as a reference,
documents SSAO or NVIDIA HBAO+ over a pre-pass with real scene normals and
uses baked vertex AO for cars and tracks; it publishes no further algorithm
details. This host still reconstructs normals from depth and has no baked AO.

Validated at `b92f6c0`: Release build and `scene_shadow_gpu` (flat planes
0 darkened pixels, contact AO kept), `render_programs_gpu` and
`scene_mesh_cache` pass. The user retested the reported car and pedestrian
views in game and confirmed the streaks are gone. A numerical model of a box
on a plane with ideal normals had not reproduced them, so in-game checks are
the reliable test for this kind of pattern artifact. Pedestrians cast no sun shadow at all (#230); the
contact darkening under them comes only from AO.

### Pedestrian sun shadows (#230)

Every shadow pass skipped `gpu_skin` instances: the scenery depth program has
no bone palette, and the outer cascade collection calls
`scene_instances_collect` with palette completion off to save CPU. Pedestrians
therefore cast nothing; only AO darkened the ground under them.

`src/render/bgfx_shadows.cpp:update_static_shadow` now takes skinned casters
from the main-view `instances_`, which are already palette-complete, within
`kSkinnedShadowRange` (100 m). It draws them in each cascade whose radius
covers them, with the skinned depth program the AO prepass already uses
(`vs_skin` + `fs_static_shadow`), and XORs a palette hash into their caster
stamp so animation invalidates the cached atlas
(`src/render/bgfx_shadows.cpp:skin_palette_hash`).

Pitfalls: rest-pose bounds of skinned meshes do not cover animated limbs, so
cull by distance, not `shadow_bounds_visible`. `u_skinBones` reads 48 matrices;
only pass a palette of at least 48x16 floats, as the main draw does.
Off-screen characters, point lights, interior daylight and the car-depth map
still ignore skinned casters.

Validated in game by the user together with the AO tile fix: pedestrians cast
sun shadows in ValoCity and no AO streaks appear around them or the car. A
build without the AO fix showed the streaks again, which is a quick way to tell
the two problems apart. No skinned-caster GPU regression or `--perf-log`
measurement exists yet.

### Rendering foundation review findings (#233)

Fixes from the `antomys` review of PR #202:

- Batched props (trees, fences, street furniture) occlude in the AO prepass, so
  they must also receive it. The prop path in
  `src/render/bgfx_lighting.cpp:bind_ambient_occlusion` callers now binds AO
  with `view_id == ao_world_view_`; before, `fs_mesh` read whatever
  `u_aoSampling` the previous draw left.
- `src/render/bgfx_lib_backend.cpp:update_local_light` is gated on
  `local_lights` only. Gating it on `dynamic_lighting` (Map sunlight) removed the
  garage fixtures and their point shadow with sunlight off, unlike `main`.
- `shaders/scene_shadow.sh:clampReceiverGradient` caps the outdoor cascade
  receiver-plane gradient. The old `receiverBias` capped it at 0.004; without a
  cap, quads straddling two surfaces can produce thin leaks or dark fringes.
  The limit is a 20-unit world slope times `extent/(extent+768)`, matching the
  ortho depth range in `src/render/shadow_cascades.hpp:shadow_projection`.
- `src/render/scene_mesh_data.hpp:same_material` compares `self_illuminated`,
  so emissive and non-emissive parts are not merged.
- Some docs held raw 0x96 bytes (Windows-1252 en dash), which is invalid UTF-8.
  Check with `grep -P '\x96'` before committing.

Validated in game by the user at `bbcb39c` (20 November 2026), after checking
the garage with Map sunlight off, prop AO and shadow edges near poles and kerbs.
No `--perf-log` measurement exists for these changes.

### Stock light resources and placement (#235)

Survey of the stock game plus the `DestructionTools` and `MW_Mod` RPKs
(20 November 2026, read-only scan of the game root). The game already has a
complete authored light model; an explicit light API should consume it before
inventing new Java classes.

- Light resources are RPK type 13 (`RESTYPE_RENDER_LIGHT` in the game's
  `ResourceRef.java`), text payloads. 89 entries: 35 `spot`, 27 `fakespot`,
  15 `directional`, 5 `fakepoint`, 2 `point`, 1 `fake point` (with a space),
  4 base classes without a `type` line.
- Common keywords: `type`, `diffuse a r g b`, `specular`, `ambient`,
  `position x y z`, `direction x y z`, `range f`, usually `attenuation c l q`
  (`1 .03 0` or `0 0 0`). Spot and fake lights add `theta`/`phi` cone angles
  (radians, e.g. 1.896/2.02), `lit_dynamic 1`, `attH_texture`/`attV_texture`
  (0x80000000 = none), and a flare set `lf_*` (note the `lenght` spelling).
- Parser pitfalls: inline `//` comments (`type spot // thatis`), commented-out
  lines, non-zero `position` on directional lights (garage `0 3 0`).
- `src/natives/natives_renderref.cpp:apply_directional_light` only reads
  directional lights (diffuse/ambient/specular/direction). Range, attenuation,
  cone, `lit_dynamic`, `fake*` and `lf_*` are not parsed anywhere yet.
- Placement: lights are render children in part/car/scenery cfgs,
  `render <light id> <pos xyz> <ypr>`, then `flags <group>` and optionally
  `colors 1 0xAARRGGBB`. 99 car/part cfgs do this. Group bits from cfg comments:
  1 brake, 2 reversing, 4 city (headlights/streetlights), 8 police/siren,
  16/32 indicators, 256 nitro; combinations exist.
- Scenery: `objects.rpk` lamps (`0x18C` kislampa, `0x18E` lamp_base) place the
  `0x1C7` street point light (range 500); `0x18F`/`0x191` use `0x16`
  `fake point`. `0x1D7` trafficlamp is an ICFG with a separate
  `light <id> ...` + `color` syntax.
- Java: only directional lights are created from scripts (Garage.java duplicates
  `misc.garage:0x001D` as "neon", CarLot uses `0x0025`, Scene.java the skydome
  sun with `setLight`/`setFlare`). No Java creates headlights; lamp state is
  engine-driven (time of day, car state).
- The `fake*` types look like flare/glow-only lights that do not light the
  scene. Not confirmed against the original engine.

### Script light sources and the g1 garage (#235, phase 1)

- API: `patch/system/Scripts/render/src/LightSource.java` with `PointLight`,
  `SpotLight` (`setCone`) and `RectLight` (`setSize`). Package `java.render`,
  new classes, no stock equivalent. Contract in ENGINE_CONTRACT §12
  "Script light sources".
- Natives: `src/natives/natives_lights.cpp:register_light_natives`, called from
  `src/natives/natives_boot.cpp:register_boot_natives`. Natives are registered
  on the declaring class only: `Jvm::invoke` resolves the declaring owner first
  (`src/jvm/jvm.cpp:find_method_inherited`), so `SpotLight.setPosition` reaches
  `java.render.LightSource.setPosition`. Registrations use literal class names
  because `scripts/render_native_coverage.py` reads them statically.
- Registry: `src/resources/script_lights.cpp:script_lights_create`, keyed by the
  Java object, parent stored as the canonical GameRef key (`gameref_get`).
  `src/resources/gameref.cpp:gameref_remove` calls
  `script_lights_forget_parent`; `gameref_clear` and `register_light_natives`
  clear it. `script_lights_visit_gc_roots` roots lights and parents.
- Parent type: `ResourceRef`, not `GameRef`. `RenderRef` extends `ResourceRef`
  directly (`GAME:system/Scripts/util/resource/src/RenderRef.java`), and the
  garage parents its lights to `stuff`, a `RenderRef`.
- Space: parent-local script metres, the coordinates of `RenderRef.setMatrix`.
  `src/resources/scene_instances.cpp:chain_matrix` documents the S·M·S instance
  transform; `src/render/bgfx_lib_backend.cpp:update_local_light` uses
  `scene_instances_render_matrix` (the drawn pose, as the instance transform)
  and flips Z. Mesh-local SCX coordinates equal script-local coordinates, so
  the adapter's anchors can be pasted into Java unchanged.
- g1 anchors: `include/slrr/legacy_neon.hpp:adapt_legacy_neon_fixtures` finds
  nine fittings, all in `misc/garage/g1/garazs_01_targyak.scx` (the 0x42 stuff
  mesh), none in the room mesh; y = 6.43138, x/z in `Garage.java:createFixtures`.
- All garages use the same fitting: two 3.00 × 0.11 × 0.09–0.12 m tubes about
  0.185 m apart. Anchors follow the adapter's rule (a tube is a connected
  component 2–4 m long and under 0.25 m wide and tall; tubes pair within 0.4 m
  horizontally and 0.1 m vertically; anchor = pair midpoint, 8 cm below the
  lower tube's bottom). Run on the g1 group it reproduces the adapter exactly.
  - g2 (`misc/garage/g2/garazs2_targyak.scx`, stuff 0x18): group `neon`
    (texture `g2cucc.dds`, SCX4 emissive colour 0.53), 20 tubes → 10 fittings
    along X, all at y = 5.42426, x = −13.633/−7.046/0.027/7.122/14.095,
    z = −3.267/4.322. Ceiling 7.79 m.
  - g3 (`misc/garage/g3/garazs3targyak.scx`, stuff 0x3c): group
    `Material #11345` (texture `g3cucc.dds`, emissive colour 0.82), 24 tubes →
    12 fittings, three along Z and nine along X, at y 5.52–6.43 (stepped
    ceiling up to 8.80 m). Exactly the 12-fixture budget.
  - ROC (`misc/garage/g4`, ground 0x2f, stuff 0x51): an open-air scene
    (sand ground, tents, containers, stands); no ceiling fittings and no
    emissive lamp group. No lights are created there; since #330 the sun
    lights it (see "ROC garage: tent canvas and sunlight").
  - Dark tubes in g2/g3 (user test at `bc2f8aa`): only
    `include/slrr/legacy_neon.hpp:adapt_legacy_neon_fixtures` turns tube faces
    into `self_illuminated` groups, and it matched only g1's `garazs1.dds`
    swatch. Script lights never touched it (g1 kept its white tubes). Now
    `legacy_neon_candidate_group` also accepts groups with an SCX4 emissive
    colour and no emission mask, and the same tube-shape and pairing rules pick
    the faces. Its anchors equal the Garage.java tables in all three garages, so
    without the patch g2 and g3 get the same fallback lights as g1.
  - Brightness: Garage.java sets intensity 30 and range 26 m per garage
    (`createFixtures`). History, all user feedback in game: 24 / 20 m too dim,
    48 / 26 m (`c07d99c`) too bright, 36 / 26 m (`1e0c3cd`), then 30 at the
    user's request. In the GPU test the floor that was dim at 24 / 20 m (gain
    under 60 in an 8-bit capture) brightens about 1.5× (g1 47→72, g2 54→77,
    g3 55→81; 36 / 26 m gave 87, 93, 91 and 48 / 26 m 112, 123, 105).
    Brighter areas clip in the capture. The tube glow does not depend on
    these values.
- Renderer: registry lights replace the adapter whenever one has a drawn parent,
  even when disabled. Limits: one point light, 12 fixtures, inner cone fixed at
  65° by `shaders/scene_shadow.sh`, fixture shadows all-or-nothing, rect drawn as
  spot, garage only (`garage_`).
- Garage source: `sources/sl/Scripts/game/src/Garage.java` is the only copy on
  disk and is the MWM version (jack, `Mirans_Mod_protection`). Every
  non-comment string literal appears in the original MWM class; the known code
  difference was the #59 save order. The patch copy adds `createFixtures`
  (one table per garage index 0–2), `createFixture` and `destroyFixtures`
  (called in `exit` before `stuff.destroy`).
- Validation: `tests/test_script_lights.cpp` (constructors and inherited natives
  through the shipped classes, input validation, destroy/parent/GC/reset
  lifetime); `tests/test_scene_shadow_gpu.cpp` checks that nine script spots at
  the Garage.java anchors give the adapter's positions, directions and cones,
  that disabled lights darken the garage and that destroying them restores the
  adapter. Its `garage_fixtures` case loads g2 and g3, checks each Garage.java
  anchor sits 8 cm under its lamp group's tubes, that the registry places all
  10/12 spots there, and that they light the real floor (15,207 and 20,437 of
  65,536 pixels), that only lamp-group faces emit and stay white without any
  light (g1 120, g2 193, g3 168 pixels), and the brightness above; and that the
  neon point record stays ambient-only when every script light is disabled.
  `tests/test_inventory_transition.cpp` runs the recompiled
  `patch/sl/Scripts/game/Garage.class` main-menu path (save after exit), and
  `garage_rejected_commands_do_not_save` runs `osdCommand` with a recording
  probe.
- Refused commands (review of PR #236): the MWM source saved after Choose
  Track and the dealers even when they were refused (no car, undriveable car,
  dealer closed). The shipped #59 class does not save there; it saves before the
  transition when they are accepted (`save,change`). The patch saves inside the
  success branch after `changeActiveSection` (`change,save`), so a refused
  command never saves a garage whose mechanic strip was not flushed. Car Lot
  is the exception since #315: it flushes the strip, saves, then changes
  section (`flush,save,change`), see "Car Lot save ordering (#315)". With a local
  `build/garage-59.class` (`git cat-file blob 6171aa2:patch/sl/Scripts/game/Garage.class`) the test also compares all 43 `CMD_*` commands: none
  differs from the #59 class in whether it saves.
  Validated in game by the user in the g1 garage at `df7117f` (20 November
  2026): the nine script fittings light the garage as before. g1, g2 and g3
  validated in game by the user at `2f904ac` (20 November 2026): glowing tubes
  and intensity 30 / range 26 m accepted (24 was too dim, 48 and 36 too bright).
- Phase 2: street lamps and scenery lights, see "Scenery lights and street
  lamps (#237)" below. Still open: per-light spot shadows and inner cone.

### Scenery lights and street lamps (#237)

Phase 2 of #235: type-13 light resources placed by map objects now light the
city at night.

- Parser: `src/resources/light_resource.cpp:parse_light_resource` reads
  `type` (`point`, `spot`, `directional`, `fakepoint`/`fake point`,
  `fakespot`; none for base classes), `diffuse/specular/ambient a r g b`
  (alpha dropped), `position`, `direction`, `range`, `attenuation c l q`,
  `theta`, `phi`, `lit_dynamic`, and notes any `lf_*` flare keyword. Inline
  `//` and `;` comments are stripped; malformed or negative values keep the
  previous value. `src/resources/light_resource.cpp:load_light_resource`
  parses only RPK entries whose `typeof` is 13
  (`src/resources/light_resource.cpp:is_light_resource`); the light is never
  guessed from keywords or aliases. All 89 stock payloads parse (2 point,
  35 spot, 6 fake point, 27 fake spot, 15 directional, 4 untyped).
- Placement: `src/resources/scene_instances.cpp:scene_instances_add_ground_props`
  keeps every non-variant `render` child whose resource is typeof 13 before
  the mesh filter drops it, composes `render` pose, resource `position` and
  placement pose, and stores the shell-local list in `GroundPropMap::lights`
  (`src/resources/scene_instances.cpp:scene_instances_ground_lights`). The cfg
  `flags` after the render line is the light group. A dynamic placement's light
  stays at its authored pose when the body is knocked over.
- Ownership: `src/natives/natives_resref.cpp:spawn_ground_shell` attaches the
  list to each new ground shell with
  `src/resources/script_lights.cpp:scenery_lights_attach`. The registry keys it
  by the shell's canonical GameRef; `script_lights_forget_parent` (called by
  `gameref_remove`) and `script_lights_clear` drop it. Scenery lights hold no
  Java object and are not GC roots; the list is shared with the cached map, so
  revisiting a city attaches the same data again.
- Valo City (`maps/city.rpk` id 1, two shells, 25,821 placements): 3,736
  light children, 2,841 real (`objects.rpk` 0x1C7 street, group 4) and 895
  glow-only (0x16 `fake point`). Drag Track has 318.
- Rendering: `src/render/bgfx_lib_backend.cpp:update_scenery_lights`, called
  by `update_local_light` outside garages when Local lights (option 4) is on.
  The city group follows the night factor of
  `src/resources/scene_instances.cpp:scene_instances_sun_state` since #271
  (lamp power scaled by it, off below 0.01); night facades keep
  the `scene_sun_is_night` window (before 06:00 or from 18:30). Other groups (car
  brake/indicator bits) stay off for scenery; car lights are their own path
  (#279, below); `fake*` lights never light the scene.
  `src/resources/script_lights.cpp:scenery_lights_select` keeps lamps whose
  45 m light sphere meets the view frustum (a lamp outside it lights no visible
  pixel), then the nearest 64 within 500 m. Each fades to 0 over
  min(150 m, 40 % of the cut distance, at least 20 m) before the first excluded
  lamp or the 500 m limit, so lamps enter and leave the set with no power and
  near lamps stay at full power. On a straight street (lamps every 30 m on
  both sides) the whole visible street fits: 36 lamps ahead out to 480 m, the
  last at 0.04 of full power.
- History: the first version (`1df1452`) reused the 12 garage fixture slots,
  nearest 12 within 250 m, with no frustum test. In game (user, 20 November
  2026) lamps visibly appeared only a few lamps ahead while driving.
- Shader budget: a separate lamp array, `kSceneryLamps` = 64
  (`src/render/bgfx_lib_backend.hpp`), `u_lampParams` plus three vec4[64]
  arrays (position + range, colour × intensity, attenuation), 193 vec4 of the
  fragment constants, bound with the fixtures in
  `src/render/bgfx_lighting.cpp:bind_fixture_lights` (same enable: previews
  and lamp glass get none). `shaders/scene_shadow.sh:lampLighting` runs in
  `fs_mesh` (instanced props included) and `fs_vehicle`: unshadowed, no cone,
  stock falloff 1/(c + l·d + q·d²) inside the window saturate(1 − (d²/r²)²)²,
  and an early `continue` when the pixel is outside a lamp's range. Garage
  fixtures (12, shadowed) are unchanged and never hold scenery lamps.
- Brightness: accepted in game by the user at `7c9d05f` (20 November 2026,
  "l'intensité top"); keep these values. Intensity 0.8 ×
  authored diffuse, range capped at 45 m (authored street range is 500 m)
  (`kSceneryLightIntensity`, `kSceneryLightMaxRange` in
  `include/slrr/script_lights.hpp`). In the GPU test a lamp at 7.8 m over a
  road with 0.08 ambient adds 161/255 red at its foot and 39 at a corner 30 m
  away.
- Cost: selection over all 3,736 Valo City lights takes about 8.5 µs per frame
  (`tests/test_scenery_lights.cpp`, Release). GPU, `scene_shadow_gpu` case
  `lamp_cost` on this machine: 1920×1080, a street-level road over the lower
  half drawn as 8 layers (about 8 M shaded pixels), GPU timer per frame: no
  lamps 0.21 ms, 12 street lamps 0.35 ms, 64 street lamps 0.83 ms; 12 and 64
  lamps all in range of every pixel 0.46 and 1.50 ms. Per million shaded
  pixels that is about 0.08 ms for 64 street lamps and 0.16 ms worst case. Not
  measured in game because the bench writes to the installed game root.
- Shadows (after the user noted at `7c9d05f` that lamps cast none):
  `src/render/bgfx_shadows.cpp:update_lamp_shadows`. Budget
  `kLampShadows` = 6 wanted lamps in `kLampShadowSlots` = 8 slots (two spare
  so a new lamp fades in while the one it replaces fades out), each one tile
  of the fixture atlas (4 × 3 tiles of `local_shadow_resolution`, 256/512/1024
  by Shadow quality; outside garages it is otherwise unused) with view ids
  128–135.
  - Selection (after the user saw the car's shadow "turn around the car" at
    `cb90023`): the first version took the 6 nearest lamps of the
    frustum-selected set by camera distance, so orbiting the chase camera
    around a parked car changed the shadowed set (4 changes in two orbits in
    `lamp_shadow_stability`) and its shadows cross-faded between directions.
    Now `src/resources/script_lights.cpp:scenery_lights_near` lists lamps
    within 55 m of the focus, the followed car (`set_lamp_shadow_focus`, from
    the same car matrix as `update_car_depth`), else the camera position,
    with no frustum. Held lamps keep their slot while within reach; the
    farthest held lamp is replaced only by a candidate at most 75 % as far and
    4 m nearer, after 1 s of hold, one swap per frame; free slots take the
    nearest candidates. A shadowed lamp outside the frustum-selected lit set
    is added to it at full power (the lit selection keeps 8 of its 64 entries
    free for this). Projection: one downward perspective, 150° field, near 0.3 m, far
  = the 45 m range. A lamp lights mostly downward; at the tile rim (3.7 × the
  lamp height, 29 m for the 7.8 m `lamp_base`) its ground light is about a
  tenth of the peak, and the shader fades the shadow to none over the outer
  part of the tile, so one depth pass per lamp replaces a five- or six-face
  cube. A paraboloid would need tessellated casters (large ground and prop
  triangles warp), so it was not used.
  - Casters: static scenery (props, instanced batches, trees, poles, world
    meshes) is collected once when a lamp gets a slot, with the lamp's frustum
    (`scene_instances_collect`, `scene_instances_collect_batches`). Excluded:
    the ground POLY (city-wide; it also holds buildings, which therefore cast
    no lamp shadow; its streets only receive), and meshes whose box holds the
    lamp (its own housing and pole, which would black it out). Dynamic casters
    come from the main-view instances each frame: vehicles including the
    player car (when vehicle shadows are on) and skinned characters
    (`ao_skin_depth_program_`, culled by distance like the cascades). A tile
    is redrawn only when its lamp is newly assigned or a dynamic caster stamp
    (geometry, revision, buffers, transform, skin palette hash) changes; static
    and dynamic casters are redrawn together because D3D11 cannot copy part
    of a depth texture.
  - Car shadows (after the user still saw the car's shadow move around it at
    `d17976f` and asked to always show it): every lamp whose light reaches a
    6 m sphere around the player car gets a car-only tile, up to
    `kCarShadowTiles` = 16, the half-size quarters of atlas tiles 8–11 (128,
    256 or 512 px by Shadow quality), drawn in one view (136) with a per-draw
    clip remap and scissor. Projection: a narrow perspective from the lamp onto
    the sphere, near = distance − 6 m, far = the lamp range, so every receiver
    whose ray from the lamp crosses the car falls in the tile. Casters: the
    vehicles in the sphere (the player car, nearby traffic); the scenery tiles
    then skip vehicles in the sphere, so they stay cached while the car moves.
    Strength fades over 10 m before the lamp's reach (or the 17th lamp), so a
    car shadow appears where that lamp's light on the car is near zero. Focus:
    the car followed by the script camera, else
    `src/resources/part_runtime.cpp:part_controlled_vehicle`; no car tiles
    without a car. The shader multiplies the scenery and car visibilities per
    lamp (`shaders/scene_shadow.sh:lampTileVisibility`); special lamps
    (scenery or car tile) are the first 24 lamp entries and the frustum
    selection keeps 40 of 64.
  - Diagnosis of the moving car shadow (no in-game capture available here):
    the shadow projections are fixed in world space (lamp position, straight
    down; no camera term, re-centring or texel snapping), the lamp views
    128–143 are ordered before the world view in `draw_previews` (no frame
    lag), each receiver uses the matrix and tile of its own slot, the sun car
    depth map is off at night (the night sun points upward and
    `update_car_depth` returns), and the stock `shd_*` decal is not drawn. In
    the harness a parked car seen from a fixed top-down camera keeps exactly
    the same shadow pixels while the game camera orbits it. What changed with
    the camera or the car was the 6-lamp budget: on the test street 7 lamps
    reach the car and on crossings more, so at least one lit it unshadowed,
    and while driving the 6 slots were reassigned every lamp spacing (19
    assignments over 300 m), each one fading a car shadow out on one side and
    in on another over 0.8 s, which reads as the shadow moving around the car.
    With car tiles every lamp that lights the car shadows it; a lamp's shadow
    still swings around the car as it is passed (correct for a point light).
  - No pop: a slot's weight fades 0 → 1 in 0.8 s (real time) when assigned
    and back to 0 when it is released (out of reach or replaced); the shader
    mixes visibility by that weight.
  - Sampling: `shaders/scene_shadow.sh:lampTileVisibility`, the fixture
    filtering (receiver-plane depth gradient, aligned 2×2 taps, 4×4 tent for
    the very-soft Shadow smoothing), 3 cm normal offset toward the lamp.
    Shadowed lamps are the first `u_lampShadowParams.x` lamp entries; their
    loop has no per-pixel early-out before the lookup because the gradient
    needs uniform control flow.
  - Gates: Local lights, the global shadow toggle (`VideoOptions::shadows`),
    Map shadows in the debug panel, night, outside garages.
  - Cost (`scene_shadow_gpu` case `lamp_shadows`, this machine, 1024 tiles,
    1920×1080 with 8 road layers ≈ 8 M shaded pixels, 10 lamps): unshadowed
    0.37 ms, 6 shadowed lamps with cached tiles 0.68 ms, with all 6 tiles
    redrawn every frame (moving car) 0.79 ms. The test scene has few casters;
    redraw cost grows with the casters in each lamp's cone.
- Validation: `tests/test_scenery_lights.cpp` (parser, every stock payload,
  Valo City registration count against an independent ICFG scan, kislampa
  pose, shell removal and revisit, selection budget/groups/day/fade);
  `tests/test_scene_shadow_gpu.cpp` case `scenery_lamps` (night lights the road
  through `fs_mesh`, noon equals the unlit image, option 4 off, glow-only,
  non-city and behind-camera lights select nothing, a zero-power or
  out-of-range lamp leaves the image unchanged) and `lamp_cost` (timing).
  `selection_frustum_and_distance` checks frustum culling, the 500 m street
  and that a lamp entering far ahead has no power. `lamp_shadows` checks that
  a static block and a car under a lamp darken the road at night (304
  darkened pixels of 19,200), that the ground POLY does not cast, that
  shadows off and daytime give no lamp shadows, the 6-lamp budget goes to the
  nearest lamps, a newly assigned lamp starts at zero strength (image equal
  to unshadowed) and fades in, and that unchanged casters cause no redraw
  while a moved car causes one. `lamp_shadow_stability`: a parked car with
  the camera orbiting it twice keeps the same 6 lamps (0 changes, 0
  assignments); driving 300 m along a street with lamps every 15 m assigns
  19 lamps, each exactly once. `car_lamp_shadows`: a parked car on that
  street gets 7 car tiles (all 7 lamps reaching it); seen from a fixed
  top-down observer its shadow (2,112 pixels) is identical over two orbits of
  the game camera, with no car tile redraw; driving 30 m at 3 m/s changes
  car-tile strength by at most 0.0073 per frame, tiles enter and leave at
  strength 0, and scenery tiles are redrawn only on new assignments. Cost
  (1920×1080, 8 road layers, 50 lamps, car moving every frame, noisy between
  runs): no lamp shadows 0.60–1.00 ms, 6 scenery tiles 1.06–1.12 ms, plus
  7 car tiles 1.57–1.77 ms. In-game: `1df1452` worked but lamps popped
  in; `7c9d05f` accepted (intensity, no pop) without shadows; `cb90023` cast
  shadows that moved around the car when the camera turned; `d17976f`
  (car-focused selection) still showed it; `cd1e963` (car tiles from every
  lamp reaching the car) accepted by the user in game (20 November 2026).
- Not done: lens flares/glow for scenery lights (`lf_*`), spot cones for
  scenery spots (drawn as omni), lights following knocked-over lamps,
  `0x1D7` trafficlamp `light` syntax. Car head- and brake lights: #279 below.

### Car lights: headlight beams and brake lights (#279)

- What the stock data says (read-only scan of every car cfg and RPK under
  `game/cars` and `game/parts`). Lights are not marked by a part class,
  name or typeof of the part: a light part is any part whose cfg has a
  `render <id>` line resolving to a typeof-13 light resource, followed by
  `flags <group>` (1 brake, 2 reverse, 4 city, 16 indicator; see the
  scenery-light notes above). Example, Baiern `L_headlights.cfg`:
  `render 0x000000E4 ... ; CITY / flags 4` (typeof 14 lamp-glass mesh
  `L_headlights_CITY`, texture `Baiern_light_glow.dds`) and
  `render 0x00030008 0 0 0 0.087 0 0 ; CITY / flags 4` (racers.rpk 0x08,
  `type spot`, diffuse 0.2 grey, range 400, attenuation 1 .03 0, theta
  1.896, phi 2.02, `attH_texture`/`attV_texture` = `light_beams/attH.dds`,
  `attVV.png`, flare `lf_*`). `L_taillights.cfg` has `render 0x0003000F ...
  ; BRAKE / flags 1`: `fakespot`, diffuse 1 0 0, range 5. Over the stock
  cfgs, 34 headlight parts carry a real `spot` in group 4 (range 250–400),
  some other parts too (pop-up or bumper lamps); taillight city lamps are
  `fakespot` (range 0.001 or 5); every brake child is a `fakespot`.
- The "fake beam": the original lit the road with the headlight spot itself,
  shaped by the horizontal/vertical attenuation textures `attH`/`attV`
  (D3D projected light). No beam quad or ground mesh exists in the data, and
  this engine never drew those textures, so nothing doubled up and nothing
  had to be hidden. The `_CITY`/`_BRAKE` meshes are the lamp glass, shown by
  `src/resources/scene_instances.cpp` through `part_lamp_lit`; they stay.
- Scripts: no Java class drives headlights or brake lights (no lights
  on/off or braking method in the car scripts); the engine decides.
- Ownership: `src/resources/part_runtime.cpp:collect_lamps` keeps, per part
  record, the first group-4 `spot` light (headlight beam) and the first
  group-1 light (brake light), with the render pose composed with the
  resource position and direction. `part_car_lights` reports them in render
  space for installed parts only: `car(part)` must reach a chassis (a
  removed part has no record; a torn-off loose part, `detach_panel`, has no
  parent), and lit ones only when `part_lamp_lit` is true for their group.
- Gates (`part_lamp_lit`, shared with the lamp glass): engine running;
  headlights before 06:00 and from 17:00 (#313; the stock night starts at
  21:00, moved earlier because dusk is already dark), on the
  `GameLogic.time` clock); brake lights while `dynamics.drive.brake` >
  0.05 (`kBrakeLampMin`), day or night, never in the showroom preview.
  Wear: condition c = wear × tear (the `getConditionNoAttach` product of
  `Part.java`; 1 = new). `part_lamp_power`: 0 at c ≤ 0.25
  (`kLampBrokenCondition`: no beam, no lit glass), 40 % rising linearly to
  full at c = 0.5 (`kLampFullCondition`). No flicker: it would make frames
  depend on time and break the capture tests. `part_tear_off` tears a part
  off as a collision does (used by the test).
- Rendering: `src/render/bgfx_lib_backend.cpp:update_car_lights`, before
  `update_scenery_lights` outside garages when Local lights (option 4) and
  the debug option "Car lights" are on. At most `kCarLights` = 16 within
  150 m, nearest to the followed car first; the street-lamp budget shrinks
  by that count so the array still holds 64. Car lights are unshadowed and
  come right after the shadowed lamps. Hue from the resource diffuse
  normalised to its peak; power and cone are constants because the authored
  0.2 diffuse and 108–116° cone were shaped by the textures above: headlight
  intensity 3 × part power, range min(authored, 60 m), authored attenuation,
  half-angles 14°/30°; brake light intensity 1.5, range 6 m, attenuation
  1 0 0.3, half-angles 55°/85° around its authored backward direction.
  Shader: `shaders/scene_shadow.sh:lampCone`, direction and outer cosine at
  light-data column 192+i of row 0, inner cosine in the attenuation `.w`;
  street lamps store −2 (omnidirectional), so their images are unchanged.
  Stock headlight poses yaw each beam 5° outward (`0.087` is yaw) and do not
  pitch it down.
- Tests: `tests/test_car_lights.cpp` (new suite `car_lights`, stock Baiern
  on Valo City): 2 headlights and 2 brake lights from 4 different parts;
  beams 2.07 m ahead of the car centre, forward, level; nothing lit parked
  at noon; driving at night 2 headlights, no brake; braking 2 brake lights;
  removing the left headlight leaves only the right beam; wear 0.2 gives no
  beam and no lit glass, 0.4 gives 0.76, 1.0 full; a taillight torn to 0.1
  loses its brake light; the remaining headlight torn off and lying loose
  gives no light. `scene_shadow_gpu` case `car_lights` (night road, 25-pixel
  patches, sum of r+g+b): 10 m ahead 1500 unlit, 9510 with two headlights,
  5520 with one; 24 m ahead 1500 → 3600; 9 m behind unchanged; brake red 3.5 m
  behind 500 → 2271 with green unchanged; toggle off or option 4 off equals
  the unlit image.
- Not done: headlight shadows came in #283 (below); the `attH`/`attV` beam pattern (a round
  cone instead), lens flares, reverse and indicator lights, native traffic
  cars without part records, not validated in game yet.

### Headlight shadows (#283)

- `src/render/bgfx_shadows.cpp:update_headlight_shadows`, after
  `update_lamp_shadows` in the frame (so it sees the final lamp order). Its
  own D32F atlas, 3 × 2 tiles of half the local shadow resolution (128, 256
  or 512 px by Shadow quality), views 145–150, ordered before the world with
  the other shadow views (`draw_previews`).
- Candidates are the lit headlight beams in the lamp array, so the #279 rules
  (installed part, running engine, condition above 0.25) already apply. The
  followed car's (`set_headlight_focus`, the same car as the lamp shadow
  focus) go first, then the others in the array's nearest-first order, up to
  `kHeadlightShadows` = 6 (debug budget "Headlight shadow tiles").
- One perspective frustum per headlight part: from the lamp along its beam,
  2 × the outer half-angle + 4° wide, near 0.15 m, far = the beam range
  (60 m). Casters are the main view's instances (world meshes except the
  ground polygon, vehicles except the light's own car through
  `part_render_owner`/`part_chassis`, characters within reach) and the
  instanced props whose bounds meet the frustum. A caster outside the camera
  view casts nothing, a known gap; the beams point ahead of the followed car,
  mostly inside the view. Tiles are redrawn every frame (the beams move).
- Each tile view is one `queue_or_run` job (#270), so it is submitted on a
  worker encoder when worker encoders are on.
- Shader: a lamp entry's colour `.w` holds its tile + 1;
  `shaders/scene_shadow.sh:headlightVisibility` samples `s_headlightShadow`
  (stage 12) with four compare taps, 3 cm normal offset toward the lamp,
  matrices and tile rects in row 2 of the light data (columns 96+4t and
  120+t). The unshadowed lamp loop now also skips pixels outside a spot cone
  before shading.
- Gates: Local lights, Car lights, the shadow option (`VideoOptions::shadows`),
  the debug option "Headlight shadows", outside garages.
- Tests (`scene_shadow_gpu` case `headlight_shadows`): a 1.5 m box 8 m ahead
  of two headlights darkens the road behind it (3798 → 1866, r+g+b over a
  patch) and leaves the road beside and in front unchanged; with ten
  headlights the followed car's two (the farthest) get tiles and six in all;
  shadow option off gives none. Cost (`city_submission` workers case, crowd
  city 1280x720, the followed car's 2 headlights + 4 of traffic, 256 px
  tiles), two runs: GPU +0.15 / +0.26 ms (the headlight views 0.234 / 0.247
  ms), render thread +0.17 / +0.36 ms, host submission +0.05 / +0.19 ms, 386
  more draws.
- Not done: casters outside the camera view, brake light shadows, a
  per-headlight shadow fade on budget changes (a tile switches on or off at
  once), not validated in game.

### Lamp glass snapped onto the lens (#284)

- The stock lamp-glass meshes (`light_flags` renders such as `_CITY`,
  `_BRAKE`) are authored in front of their lamp body: Baiern 3.85–10.6 mm over
  the lens (`L_taillights_CITY` 10 mm on every vertex). Lit, the glass floated
  off the car.
- `src/resources/part_runtime.cpp:snap_lamp_glass`, at the end of
  `render_part`: each glass vertex moves onto the nearest lens triangle of
  the same part's other renders, 0.5 mm (`kGlassSnapGap`) in front of it. A
  lens triangle faces within 60° of the glass normal (`kGlassLensAlignment`;
  housing walls are not lenses), lies within 3 cm (`kGlassSnapReach`) and
  holds the vertex in its plane projection. Vertices with no lens under them
  take the mean move, then two more passes settle those the mean brought over
  a lens. The copy is the render node's `deformed_geometry`; game files and
  the shared mesh cache are untouched.
- `restore_deformation` no longer clears lamp nodes: Java `setTear(1)` during
  assembly ran it and dropped the snap. The deformation pass already skipped
  lamp nodes, so lamp glass still does not follow body dents.
- The vehicle path draws lamp glass with `BGFX_STATE_DEPTH_TEST_GEQUAL`, so
  the 0.5 mm gap never loses a depth tie with the lens.
- Test (`tests/test_car_lights.cpp`, stock Baiern): 14 glass meshes, 78
  vertices over a lens, all exactly 0.50 mm in front after loading, none
  behind, at most 10.1 mm moved; before the change 3.85–10.6 mm.

### Renderer debug menu (#243)

- Overrides and statistics: `src/render/render_debug.hpp:render_debug`
  (`RenderDebugOverrides`, defaults = normal renderer, never saved) and
  `src/render/render_debug.hpp:render_debug_stats`, filled each frame by
  `src/render/bgfx_lib_backend.cpp:publish_debug_stats` just before the overlay
  draws. That function also turns the bgfx profiler (`BGFX_DEBUG_PROFILER`) on
  only while the overlay is open; per-view GPU times need it.
- Menu: `src/render/debug_overlay.cpp:draw_render` with sections Timings
  (`draw_render_timings`, rolling 120 frames per view, views missing in a
  frame count as 0, sorted by GPU time; dump via `render_timing_dump`),
  Passes (`draw_render_passes`), Budgets (`draw_render_budgets`), Buffers
  (`draw_render_views`, `ImGui::Image` of the D32F atlases and the R8 AO
  buffer), Session and host, Options (saved). View names come from
  `src/render/render_debug.hpp:render_debug_view_name`; views 128–135 are the
  fixture or lamp tiles, 136 the car lamp tiles, 140–144 AO (prepass,
  geometry since #256, pass 1, two filters).
- Clipboard and log: bgfx's Dear ImGui build defines
  `IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS` (bgfx `imconfig.h`), so
  `ImGui::SetClipboardText` only filled an in-process buffer and Copy never
  reached Windows (user report on PR #244). `src/platform/host_clipboard.cpp:host_clipboard_set_text`
  writes CF_UNICODETEXT; `DebugOverlay::draw` installs it, with
  `host_clipboard_get_text`, as the ImGui platform clipboard after
  `imguiCreate`, and the Copy button calls it directly
  (`tests/test_host_clipboard.cpp` round-trips UTF-8 and restores the previous
  text). Log prints to stdout and appends to
  `perf/renderer_debug_YYYYMMDD_HHMMSS.txt` beside the executable
  (`src/render/debug_overlay.cpp:write_render_dump`); the menu shows the path.
- Snapshot header: resolution, VSync (there is no frame cap besides VSync),
  then `overrides:` with one line per debug override that differs from its
  default (`src/render/render_debug.hpp:render_debug_overrides_text`, plus the
  overlay's map/vehicle shadows, OSD, flare and reflection faces), or `none`.
  Saved Enhanced options are not listed.
- CPU zones added to `include/slrr/frame_profiler.hpp` (also new `--perf-log`
  CSV columns): `CollectBatches` (inside Collect), `SunShadows`, `CarDepth`,
  `LampShadows` (inside Shadows), `FixtureShadows`, `LampSelect`,
  `AmbientOcclusion`.
- Where each override acts: AO in `update_ambient_occlusion` (targets
  destroyed); cascades in the `update_static_shadow` cascade loop (an off
  cascade is cleared, i.e. lit); shadow caching in the sun, point, fixture,
  lamp and car-tile cache checks; character casters in the cascade and lamp
  tile caster loops; fixture shadows in `update_fixture_shadows`; lamp lighting
  in `update_scenery_lights`; lamp shadows and car tiles in
  `update_lamp_shadows`; reflections in `update_reflections`; clouds and fog at
  their uniforms; MSAA and anisotropy in the reset flags and `world_sampler`;
  map props, vehicles, characters and sky by removing their records right
  after collection in `sync_instances` (so draw, shadows and AO all lose them;
  `SceneInstance::ground_prop` marks single props). Budgets: lit lamps and
  distance (`scenery_lights_select` got a distance parameter), shadowed lamps
  (a lower budget now also releases the farthest held slots), car tiles,
  `local_shadow_resolution` / `scenery_shadow_resolution` and
  `src/render/shadow_cascades.hpp:cascade_radii` (radius scale), AO buffer
  scale, reflection faces (0 freezes the cube).
- Validation: `tests/test_scene_shadow_gpu.cpp` case `render_debug_overrides`
  checks the defaults, that cached lamp tiles are not resubmitted, that lamp
  shadows, car tiles, AO and lamp lighting toggles remove their views (0 ms)
  and restore them, and the budgets. First harness look (1280×720, 50 lamps,
  caching off, HBAO, one road and one car): world 0.13 ms, AO 0.10 ms
  (pass 1 0.07), six lamp tiles 0.014 ms, car tiles 0.005 ms. The harness
  scene is tiny; real costs need the menu in game.

### Per-view light uniforms (#246)

- bgfx uniform semantics (checked in the vendored source,
  `build/_deps/bgfx-src/src/bgfx.cpp` `Encoder::submit` and
  `rendererUpdateUniforms`): `setUniform` calls between two submits form a
  range attached to the next draw only. The renderer replays each draw's range
  into a value cache that persists across draws, views and frames, in render
  order. Render order equals submit order only in a `ViewMode::Sequential`
  view; sorted views reorder draws, so a value set on one draw can be missing
  on a draw rendered before it.
- Hence `src/render/bgfx_lighting.cpp:bind_scene_shadow` uploads the
  view-invariant arrays (fixtures, lamps, lamp and car-tile shadow matrices and
  tiles, sun cascades, point faces) only when
  `LibBgfxBackend::light_arrays_current` is false: the draw's view (set by
  `draw_instance` and `draw_prop_batches`) is marked Sequential through
  `set_view_sequential`, and the view, frame (`frames_`) and light revision
  (bumped by the `update_*` light/shadow functions) match the last upload.
  Per-draw counts and flags (`u_lampParams`, `u_lampShadowParams`,
  `u_fixtureParams`, shadow params) and textures are still set per draw, so
  unlit draws (previews, lamp glass) read no lamp. Draws from other paths (sky,
  terrain parts, OSD) and views not marked Sequential upload every time.
- #262 extends the rule to the single-value light uniforms and to texture
  stages; see "Per-view uniform values, kept texture stages and residency
  skip (#262)" below.
- Measured (`scene_shadow_gpu` case `per_view_light_uniforms`, 2,000 lit draws
  in one view): main-thread submission 1.45 → 1.08 ms, bgfx render thread
  unchanged (≈3.7 ms). The D3D11 renderer re-uploads the program's whole
  fragment constant buffer whenever any uniform of a draw changed, and every
  draw still sets per-draw material uniforms; the fragment cbuffer of
  `fs_mesh` is about 600 vec4 (64-lamp arrays, 24 lamp-shadow and 24
  car-tile matrices, fixtures, cascades). Cutting render-thread and GPU upload
  cost needs a smaller cbuffer, e.g. lamp and shadow arrays in a texture.

### High AO at half resolution (#248)

- `src/render/bgfx_ao.cpp:update_ambient_occlusion` now sizes the AO buffers
  at half resolution for every quality; the debug "AO resolution scale"
  multiplies that half size (0.25–2, 2 = full resolution, the old High).
- Samples per pixel (`shaders/fs_ao.sc`, quality Low / Medium / High):
  HBAO directions × steps 4×3 / 6×4 / 10×6 (High was 8×6 at full res);
  GTAO slices × steps (× 2 sides) 2×4 / 3×6 / 5×8 (High was 4×8 at full res).
  High therefore keeps about 1.25× the old per-pixel samples on a quarter of
  the pixels, and 2.2× Medium's GTAO samples.
- Upsample unchanged: `shaders/ambient_occlusion.sh:ambientVisibility`, 2×2
  bilinear taps weighted by view-depth similarity. In the AO test (160×120,
  blocker on a floor) half-res High differs from full-res High by more than 12
  levels on 0.6 % (HBAO) and 0.15 % (GTAO) of pixels; the contact shading
  widens by about one half-res texel (HBAO 376 vs 246 contact pixels, GTAO
  135 vs 94) and does not cross the blocker silhouette.
- Harness timing at 2560×1440 (small scene, noisy between runs): GTAO High
  pass 1 0.26–0.29 ms vs 0.61–0.80 ms full-res (−55 to −65 %), HBAO High
  0.25–0.38 vs 0.55–0.79 ms. Applied to the in-game 2.6 ms that is roughly
  −1.5 to −1.7 ms; confirm in game with the debug menu.

### Street lamp shadow early-out and capped kernel (#250)

- `shaders/scene_shadow.sh:lampLighting` takes `dFdx/dFdy(world)` once per
  pixel, in uniform control flow, then skips each shadowed lamp whose range
  does not reach the pixel or that faces away (`dot(n, delta) <= 0`); both
  give zero radiance, so the skip is exact. The surviving lamps call
  `shaders/scene_shadow.sh:lampTileVisibilityFast`, which rejects pixels
  outside the tile before any tap and derives the receiver depth gradient
  from the world derivatives through the tile matrix (quotient rule on
  clip.xyz / clip.w, `receiverGradientFrom`) instead of `dFdx(p)`, so it can
  run after a per-pixel branch.
- Kernel: Sharp and Soft keep their 2x2 taps; Very soft uses the same 2x2
  taps on a two-texel grid (4 taps) instead of the 4x4 tent (16). Sun
  cascades and garage fixtures are unchanged.
- The #237 loop stays as a reference path behind
  `render_debug().lamp_shadow_reference` (u_lampShadowParams.w = 1; menu
  Passes, "Lamp shadow reference path").
- `scene_shadow_gpu` case `per_view_light_uniforms` compares both paths on the
  lamp, car-tile and block scene: Sharp and Soft are byte-identical (max
  channel difference 0); Very soft differs on shadow edges (max 35 levels,
  1.5 % of channels). Harness timing, 1920×1080, 8 road layers, 50 lamps,
  6 scenery + 7 car tiles redrawn every frame (noisy): lamp shadow cost over
  "no lamp shadows" Very soft 1.10 → 0.61 ms, Soft 0.69 → 0.65 ms, Sharp
  about 1.1 → 0.86 ms. The harness lamps all reach the visible road, so the
  range early-out saves more in the city, where most pixels are out of range
  of most shadowed lamps.

### Map prop LOD and far size cull (#252)

- Stock data (read-only scan of the game root, `tests/test_prop_lod.cpp`):
  `lod first last` follows a `render` line and marks the levels it draws at,
  `lods N s0 s1` is one object-wide line with ascending size ratios
  (thresholds for levels 1..N). In practice N = 2 and every chain is a pair:
  the high mesh `lod 2 2` and a `_lod` mesh `lod 0 1` (e.g. `lamp_base`
  0x118 / 0x13, tree `fa03a` 0xd8 / 0x5d5 `lod_fa3.scx`). Two threshold sets
  exist: `0.01 0.025` (trees, lamp posts, some buildings) and
  `0.001 0.0025` (most building fronts). 32 object types in the stock packs
  have chains. The ratio's definition is not documented; it is implemented as
  placement bounding radius / distance (not confirmed against the original
  engine): a fa03a tree switches to its low mesh at about 150 m going out and
  back at 122 m.
- Code: `src/resources/part_config.cpp:parse_part_config` reads `lods` into
  `PartConfig::lod_sizes`. `scene_instances_add_ground_props` keeps the whole
  chain for types with `lods` (others keep only the highest LOD, as before),
  tags each prop with its level range and one `PlacementLod` per placement
  (shell-local centre, radius covering all its LOD renders, thresholds, level).
  `src/resources/scene_instances.cpp:prop_lod_visible` picks the level with
  `src/resources/scene_instances.cpp:scene_prop_lod_level` (10 % hysteresis)
  in both collection paths. The renderer sets `ScenePropLodView` in
  `sync_instances`: only the main view's collection stores levels; AO, sun
  cascades, reflections and lamp tiles reuse them. Low meshes are resident
  with the map (`resources`), a small memory cost.
- Far size cull: `screen_size_visible` drops props beyond 40 m whose
  projected box is under `render_debug().far_cull_px` (default 4, was 2).
- Debug overrides: `lod_bias` (distance multiplier), `force_highest_lod`
  (pre-#252 behaviour) and `far_cull_px`, all in the snapshot header.
- Valo City (whole city, no frustum): 16,602 placements with a chain; prop
  triangles 3.37 M with the highest LOD → 1.94 M with LOD (−42 %) from the
  first shell origin. With a 2560×1440 projection model the far cull gives
  1.69 M triangles / 26,728 records at 2 px and 1.47 M / 23,152 at 4 px (of
  31,075). Draw calls are not reduced by LOD itself (a low mesh replaces the
  high one); the size cull removes records.

### Light data texture (#254)

- The fixture, street lamp, lamp-tile and car-tile arrays moved out of the
  fragment constants into a 256 × 3 RGBA32F texture (`s_lightData`, stage 11)
  read with `texelFetch` (`shaders/scene_shadow.sh:lightData`,
  `shaders/scene_shadow.sh:lightMatrix`; layout documented there). Matrices
  are stored by column and rebuilt with `mtxFromCols`, which reproduces the
  uniform `mat4` exactly.
- `src/render/bgfx_lighting.cpp:upload_light_data` builds the contents where
  the #246 per-view arrays were uploaded (first lit draw of a Sequential view
  and frame, every draw in other views) and calls `updateTexture2D` only when
  they changed. A bgfx texture update applies to the whole frame, which is
  valid because light data changes only in the `update_*` functions, before
  any draw. Per-draw counts and flags stay uniforms.
- Fragment uniforms (from the embedded D3D11 shaders' uniform tables):
  `fs_mesh` 595 → 79 vec4, `fs_vehicle` 593 → 77 vec4. Harness, 2,000 lit
  draws in one view: bgfx render thread 6.5–7.1 ms → 4.7–5.0 ms (about −30 %),
  main-thread submission unchanged at about 1.1 ms with the per-view path.
  Images are byte-identical to the uniform version (cross-build capture of the
  lamp/car-tile scene, and every scene_shadow_gpu pixel count unchanged).
- Investigations noted while measuring (not fixed):
  - Half-res GTAO pass 1 still reads the full-resolution D32F prepass:
    `surfaceNormal` takes 9 depth taps, and each slice sample calls
    `sampleDelta`, which reads the depth twice (`depthAt`, then `positionAt`).
    High is 5 slices × 8 steps × 2 sides, so about 170 full-res depth fetches
    per AO pixel, spread over a screen radius that grows with the world radius
    (Elias uses 3 m, the harness 1 m, which is why the harness predicted more
    savings). Filter passes 2 and 3 each rebuild the position and the 9-tap
    normal again. Likely wins: one fetch per sample, a half-resolution linear
    depth (R16F/R32F) for all three passes, and a depth mip chain chosen by
    sample distance (as in XeGTAO); storing the normal once would remove the
    9-tap reconstruction from passes 2 and 3.
  - Prop LOD (#252) keeps both meshes of a chain as props, so the collection
    loops visit about twice as many props for the 16,602 chained placements.
    Each runs the frustum test before the LOD test, and `prop_lod_visible`
    recomputes the transformed centre and distance for both of a placement's
    props. That matches the +0.3 ms of collection seen in game. Testing the
    LOD before the frustum test, caching the level per placement and frame, or
    keeping per-LOD lists would remove it.

### LOD selection cost in collection (#257)

- `src/resources/scene_instances.cpp:prop_lod_visible` now computes a
  placement's level once per updating LOD view: `scene_instances_set_lod_view`
  bumps `g_lod_epoch` when `update` is set, `PlacementLod::epoch` records the
  epoch of the stored level, and the second chain member and the other
  collection path reuse it. The batch path tests the LOD before the frustum
  sphere, so the non-selected chain member costs one lookup. Levels are now
  also refreshed for placements in visible cells outside the view frustum,
  which keeps off-screen casters consistent.
- Harness (`tests/test_prop_lod.cpp`, Valo City, street-level 60° frustum,
  no size filter, 60 frames): collection 0.56–0.58 ms with the highest LOD
  only; with LOD 0.60–0.63 ms before this change and 0.59–0.61 ms after
  (noisy, ±0.2 ms on a loaded machine). LOD selection itself costs about
  0.03–0.05 ms here, so the +0.3 ms of collection seen in game at #252 is
  mostly not LOD selection; it is within frame-to-frame noise of that zone.

### AO geometry pass and depth fetches (#256)

- New view 141 (`src/render/bgfx_ao.cpp:update_ambient_occlusion`, shader
  pass 3 of `shaders/fs_ao.sc`) writes, at AO resolution, the linear view
  depth (R32F, 0 = no surface) and the 9-tap reconstructed normal (RGBA32F,
  w = valid) of the full-resolution depth texel under each AO texel centre.
  AO pass 1 and both filters read the centre position and normal from it
  (2 fetches instead of 10), and the filter taps read the linear depth once
  per tap; positions are rebuilt at the same full-resolution texel centres,
  so the filters and the centre terms are exact. AO passes moved to views
  142–144; the views ordered before the world now run 128–144
  (`draw_previews`).
- Pass 1 samples the AO-resolution linear depth (one fetch per sample, a
  quarter of the texels over the same radius) instead of reading the
  full-resolution D32F twice per sample. `render_debug().ao_reference`
  ("AO reference sampling") keeps the full-resolution depth, now with one
  fetch per sample, for comparison. Depth mips chosen by sample distance were
  not added (the half-resolution depth already removed most of the cost).
- AO test, reference vs new on the blocker scene: GTAO max 2–7 levels and no
  pixel off by more than 8 at every quality; HBAO max 48–55 levels on
  0.7–0.8 % of pixels (its few radial samples move to the coarser grid),
  contact area within ±10 %.
- Harness timing, GTAO High, 2560×1440, 3 m radius, a street scene filling
  the screen (400 m ground, 400 boxes): pass 1 1.61–1.93 ms → 0.57–0.74 ms;
  the new geometry pass costs 0.02–0.03 ms and the filters 0.09–0.15 ms.
  The old numbers match the 1.8 ms Elias measured in game.

### Measured in game after the renderer performance stack (#246–#257)

Elias, 2560×1440, night city, his settings (MSAA 8x, GTAO High, 3 m AO
radius, shadow quality High, Very soft smoothing), debug snapshot:

| | Before the stack | After the stack |
| --- | --- | --- |
| GPU | 13.34 ms | 7.63 ms |
| bgfx render thread | 12.11 ms | 5.62 ms |
| World view | 9.31 ms | 5.40 ms |
| AO pass 1 | 2.60 ms | 0.62 ms |

The frame is still about 11.2 ms with present wait 0.03 ms: the main thread
is now the bottleneck (scene collection + residency 3.87 ms, of which
instanced prop batches 1.56 ms; world submission 2.91 ms; OSD + debug 0.62;
reflections 0.5; AO 0.25; shadows 0.2). The renderer snapshot does not list
the script, traffic, physics and GC zones, which make up most of the
remaining ~2.8 ms (they are in the menu's "Session and host" section).

### Static prop world cache and persistent batches (#260)

- `src/resources/scene_instances.cpp:refresh_world_cache` stores, per map
  prop, its script-space world centre and its render-space instance matrix
  (S·shell·pose·S), plus each cell's world centre, for
  `GroundPropMap::cached_shell`. Both collection paths call it with the
  shell's current matrix and it recomputes only when that matrix changes
  (ground shells are static), instead of a matrix product, two
  `transform_point` and a sign-flip loop per placement and frame.
- `scene_instances_collect_batches` keeps a persistent layout: at load each
  map gets one slot per batchable mesh (`GroundPropMap::batch_ids`,
  `batch_geometry`, `GroundPropBatch::slot`); the output vector holds one
  entry per slot and shell, keeps its instance vectors' capacity from frame
  to frame and is only rebuilt when the shells or slots change. The
  per-frame `batch_of` hash map is gone. Entries can be empty; every consumer
  already skips empty batches (the debug stats now count non-empty ones).
- The world list copy in `scene_instances_collect` was kept: in the harness
  taking the frame list instead of copying it saved nothing measurable, and
  it would force reflection and shadow collections to rebuild the list later
  in the frame.
- The renderer snapshot (`src/render/debug_overlay.cpp:render_timing_dump`)
  now lists Scripts, Traffic, Physics (vehicles, pedestrians, collision), GC
  and the render flush, plus "other main thread" (frame minus scripts,
  traffic, physics, flush and GC), so the main-thread budget adds up.
- Harness (`tests/test_prop_lod.cpp`, Valo City, street frustum, 60 frames,
  frame cache on, batch vector kept across frames as in the renderer):
  main-view collection 0.60–0.70 ms → 0.17–0.24 ms; identical output
  (same records and transform checksums with and without LOD).

### Per-view uniform values, kept texture stages and residency skip (#262)

- Same bgfx rule as #246, extended to every single-value light uniform:
  `LibBgfxBackend::set_uniform` (`src/render/bgfx_lighting.cpp`) skips a
  value equal to the one already set in the same Sequential view and frame
  (sun, fog, sky and cloud wind, shadow filter/params/origin/ranges/eye and
  matrix, interior sun, point and local light, car depth, AO, fixture/lamp
  counts, lightmap and mesh UV transforms). The view comes from
  `LibBgfxBackend::BindingScope`, opened by `draw_instance` and
  `draw_prop_batches`; a new view or frame (`frames_`) starts a new
  generation. Every set of these handles in the renderer goes through
  `set_uniform`: outside a Sequential scope it forgets that handle, so sky,
  terrain parts, sun flare, AO passes and shadow passes that set the same
  uniforms stay correct. Matrices pass 16 floats; a new handle routed through
  `set_uniform` must pass its real size.
- Two bgfx cases where a set value never reaches the draw the cache expects
  (`build/_deps/bgfx-src/src/bgfx.cpp` `EncoderImpl::submit`, `bgfx_p.h`
  `discard`): a dropped submit (no indices, or more than
  `BGFX_CONFIG_MAX_DRAW_CALLS`) discards its pending uniforms, and values set
  after the last submit of a scope attach to whatever submit comes next.
  `submit_bound` forgets everything after a submit without indices, and the
  scope destructor forgets everything when values are still pending (this
  also covers the #246 arrays). The draw-call limit is not detected: past it
  the frame is already missing draws.
- Texture stages (`set_texture`, `submit_bound`): in this bgfx (fe9bbdec6)
  `setTexture` compares the binding (`setBind`) and writes a sampler uniform,
  and the encoder hashes the binding set at a submit when it changed;
  `BGFX_DISCARD_BINDINGS` resets the bindings after every submit. Inside a
  Sequential scope, submits now use `BGFX_DISCARD_ALL & ~BGFX_DISCARD_BINDINGS`
  and only stages whose sampler, texture or flags changed are set again. A
  draw that leaves a kept stage unset (vehicle stage 4 without a live
  reflection cube, mesh and vehicle samplers on the same stage) is submitted
  with the full discard, so no draw samples a texture it did not ask for. The
  scope ends with `bgfx::discard(BGFX_DISCARD_BINDINGS)`, so sky, terrain,
  blended, OSD and shadow draws outside it start with no bindings, as before.
  Every `setTexture` and `submit` inside the two scoped functions must use
  the wrappers, or the kept state no longer matches the encoder.
- Instanced map props: `kPropInstanceChunk` (`src/render/bgfx_lib_backend.hpp`)
  raised from 128 to 1024 placements per submit. Instance data comes from the
  per-frame transient vertex buffer (6 MB default, 80 bytes per placement)
  whatever the chunk; `getAvailInstanceDataBuffer` still clamps a chunk when
  the buffer runs low. Draw order inside a batch changes (each group now
  covers all placements at once); the city capture is byte-identical.
- `sync_instances` (`src/render/bgfx_lib_backend.cpp`) lists the meshes it
  keeps (shadow meshes, lamp casters, resident props, instances, previews)
  with revision, owner and deformed flag. When that list and
  `instance_meshes_.size()` match the previous frame, `cache_scene_mesh` and
  the sweep would change nothing, so the used map, prefetch, cache loop and
  sweep are skipped (`residency_skips_`); skin uploads still run.
  `clear_scene` resets the stored list.
- Harness (`tests/test_scene_shadow_gpu.cpp` `city_submission`, Valo City at
  22 h, street view, 640×360, world view 5 Sequential, two runs): caches off
  then on in the same process, captures byte-identical to each other and to
  the pre-#262 build. sync_instances 0.38–0.39 → 0.32 ms, world submission
  0.84–0.87 → 0.68–0.69 ms, bgfx render thread 1.28–1.34 → 0.92–0.94 ms.
  Per frame: uniform sets 39,590 → 186, texture sets 15,063 → 1,995, draws
  1,368 → 1,367. The pre-#262 build measured 0.39–0.46 / 0.79–0.99 /
  1.34–1.62 ms in the same harness.
- In game after #260 and #262 (Elias, 2560×1440, night city, about the same
  spot and time as the #246–#257 snapshot, 6,090 draws, 625 batches / 8,946
  placements): frame 11.2 → 10.2 ms, GPU 7.63 → 7.36 ms, bgfx render thread
  5.62 → 4.70 ms, world view GPU 5.40 → 5.25 ms, scene collection +
  residency 3.87 → 3.24 ms, world submission 2.91 → 2.68 ms. The main thread
  is still the limit (render flush 7.9 ms of a 10.1 ms frame; scripts 0.37,
  physics 0.98, other 0.88). Open question: the "instanced prop batches"
  zone (main-view `scene_instances_collect_batches` only) stayed at 1.55 ms
  although the #260 harness measured 0.6–0.7 → 0.17–0.24 ms for it, so the
  in-game cost lies in something the harness does not reproduce (scene,
  LOD/size filter, frame cache state).

### Emissive overlays sit flush (#281)

- Symptom (user, 21 November 2026): lamp glass floated about 2 mm off its
  body: car head- and taillights, traffic lights.
- Cause in code: `shaders/vs_mesh.sc` and `shaders/vs_mesh_inst.sc` pushed
  every emissive overlay 2 mm along its normal (`1f886b5` for night facade
  masks, `8f2775d` for the instanced copy). The flag is
  `SceneInstance::emissive_overlay` (`u_alphaTest.y` = 2, or the instance
  colour's w = 2): night-only props, night facade meshes and the traffic
  signal glass (`src/resources/scene_instances.cpp:signal_geometry`). The
  push moved the glass visibly and showed glass authored just behind its
  bezel.
- Fix: no geometry offset; clip z is scaled by 1 + 1e-4, which with
  reverse Z brings the overlay 1e-4 × its distance nearer in depth only
  (0.5 mm at 5 m, 5 mm at 50 m); x and y are unchanged, so nothing moves on
  screen. 1e-5 lost depth ties on 250 of 4,200 pixels of a slanted wall in the
  harness. Overlays keep `BGFX_STATE_DEPTH_TEST_GEQUAL` and additive blend
  without depth writes.
- Car lamp glass is not an emissive overlay: the `_CITY`/`_BRAKE` meshes
  (`light_flags`) are drawn by the vehicle program, which has no offset. In
  the stock data they are authored in front of the lamp body: Baiern
  `L_taillights_CITY` exactly 1.000 cm along the lens normal for all 12
  vertices, `L_taillights_BRAKE` 0.94–1.0 cm, `L_headlights_CITY` 0.4–1.0 cm
  (measured with `load_scx_geometry`, distance to the body triangles). That
  gap is the asset's, not the renderer's; #284 snaps that glass onto the lens
  at load (see "Lamp glass snapped onto the lens (#284)").
- Test: `scene_shadow_gpu` case `emissive_overlay_flush`: a 4 × 4 m wall seen
  at a slant, glass as an 8 × 8 grid with the other diagonal; level glass
  shows on all 4,200 interior pixels, 1.5 mm in front too, 1.5 mm behind on
  none. With the old 2 mm push the glass 1.5 mm behind showed on every pixel.

### Instanced night overlays and blended props, world list zones (#264)

- Profiler: `perf::Zone::WorldList` wraps the frame's world list build in
  `src/resources/scene_instances.cpp:world_instances` (the first collection of
  the frame builds it, which is the main view's
  `scene_instances_collect_batches`, so it is nested in "instanced prop
  batches"), and `perf::Zone::WorldCopy` wraps the copy of that list in
  `scene_instances_collect`, summed over every collection of the frame. The
  snapshot and the menu's view table now show draws per view:
  `LibBgfxBackend::submit_counted` (`src/render/bgfx_lib_backend.hpp`) counts
  every renderer submit into `view_draws_`, reset at the start of the frame
  and published with the debug stats.
- Night overlays: night-only props (`GroundProp::night_only`, the authored
  night facades) get a persistent slot per mesh (`GroundPropMap::night_ids`,
  `night_geometry`, `GroundProp::night_slot`). When `scene_instances_collect`
  receives a `night_batches` vector (only `sync_instances` passes
  `night_batches_`), visible night props go there instead of one
  `SceneInstance` each; other callers still get SceneInstances with
  `emissive_overlay`, which every shadow, AO and reflection pass already
  skips. `draw_dynamic_world` draws them in pass 1 through
  `LibBgfxBackend::draw_props_instanced` (`src/render/bgfx_lighting.cpp`).
- Props with a blended section (`LibBgfxBackend::blended_prop_instance`: a map
  placement whose SCX3/4 mesh has a blended group, no ground polygons) keep
  their SceneInstance, because shadows, AO and reflections use it. Only the
  world view draws them instanced: `draw_dynamic_world` groups them per mesh
  and lighting in first-appearance order and draws each group in both passes
  through `draw_props_instanced` instead of `draw_instance`.
- `draw_props_instanced` is draw_instance's group lambda for a map placement
  (cull_backfaces, no preview, sky, skin or texture replacement, opacity 1)
  with `inst_program_`. Each group gets its own instance data, because the
  tint draw_instance computes from the placement colour (diffuse, ColorMixTex,
  blend modulation, emission colour) is computed per placement in the same
  order and sent in the instance colour, with `u_color.rgb` = 1 and the alpha
  (which never depends on the placement) in `u_color.a`. fs_mesh multiplies
  `tint * placement`, so the product is the same float.
- `shaders/vs_mesh_inst.sc` applies vs_mesh's overlay depth bias (a 2 mm
  normal offset before #281) when the instance colour's w is 2 (1 otherwise, including the shadow programs).
  Pitfall: the first version read `u_alphaTest.y` in this vertex shader, like
  vs_mesh does; the bgfx render thread then took ~45 ms per frame in the
  harness, and the main thread got slower too (bisected: same code without
  that shader change ran at normal speed). Keep that uniform out of
  vs_mesh_inst.
- Order: pass 1 is now the instance loop (world list, signal glow), then the
  night overlays, then the blended props; groups are drawn group-major over
  their placements. Overlays add light (SRC_ALPHA, ONE), which an 8-bit
  target accumulates in any order; the captures below show no change.
- Harness (`tests/test_scene_shadow_gpu.cpp` `city_submission`, now run at
  640×360 and at 2560×1440; the size also drives the far-size filter, so
  2560 keeps the props the game keeps; Valo City 22 h, world view only,
  caches on). Both captures are byte-identical to the pre-#264 build
  (`build/city_frame_reference_<w>x<h>.bin`, written by the earlier build).
  At 2560×1440: world-view draws 2,555 → 1,843, scene instances 789 → 116
  (673 overlay placements in 65 meshes, 114 blended placements),
  sync_instances 0.50–0.67 → 0.36–0.41 ms, world submission
  1.55–1.64 → 0.97–1.06 ms, bgfx render thread 1.87–2.12 → 1.29 ms (two
  runs each).

### Parallel prop batch collection and collection sub-zones (#266)

- `scene_instances_collect_batches` runs its placement loop on the job pool
  (§5.10 "Per-frame work on the pool"). A first version chose the LOD in a
  separate pass (jobs list the LOD placements, the host thread chooses) and
  was slower than serial in the harness (0.36 ms warm against 0.28 ms): two
  pool dispatches and a serial pass over every LOD placement. The shipped
  version is one dispatch with the LOD chosen in place through
  `std::atomic_ref`.
- Equivalence: `city_submission` compares the pooled loop with the same
  loop run inside a job (where a batch runs serially) on the same frame
  state: identical batches and placement order. The 640x360 and 2560x1440
  captures match the pre-#266 build on 24 threads and with the test process
  pinned to one and to two cores. Windows has no ThreadSanitizer here (MSVC
  only, no g++/clang); the review covers what the jobs read and write.
- New profiler zones in sync_instances: `CollectInstances` (the main view's
  `scene_instances_collect`), `CollectSkins` (walker palettes, every
  collection), `CollectPreviews` (script viewport previews) and `Residency`
  (`src/render/bgfx_lib_backend.cpp:sync_instances`), shown under "Scene
  collection + residency" in the snapshot.
- Harness (`city_submission`, 2560x1440, world view, 24 pool threads; "cold"
  overwrites 96 MB before every frame): batches zone 0.28-0.32 ms warm,
  0.50 ms cold before; 0.17-0.23 ms warm, 0.26-0.31 ms cold after.
  sync_instances cold 0.72-0.73 -> 0.46-0.54 ms. Main-view
  `scene_instances_collect` (still serial) takes 0.11-0.15 ms warm and
  0.16-0.21 ms cold.
- Known harness flake, present before #266 (seen on the #264 build with
  no source change): about one run in three, the 2560x1440 capture differs
  from another run's in 17 scattered pixels of the lamp-lit road by 1 in one
  channel, between two runs in the same process too (warm against cold).
  The cause is not found; it is not thread related (it happens with the
  serial #264 code). `city_submission` prints the warm/cold comparison and
  requires only caches on/off and pooled/serial equality.

### Walker palettes, camera list and world list costs (#268)

- Walker palettes were already built once per frame and shared by every
  collection (`g_frame_characters`); the cost is the pose rebuild. New ones
  now run on the job pool (8 or more, with 4 or more pool threads), see §5.10.
  Walkers outside a view's frustum still get no palette; walkers that are
  merely far keep theirs, because the renderer skips a walker without one and
  dropping them would change the image.
- `src/resources/gameref.cpp:gameref_active_cameras` read every GameRef of
  the scene, and `fullscreen_script_camera`, `sync_instances` and
  `draw_previews` call it each frame. It now reads `g_camera_refs`, the keys
  given kind Camera (`gameref_upsert`, `gameref_bind_scene_camera`), dropping
  the ones that are gone. Ties in `cam_pri` are now broken by `cam_seq`
  (activation order) instead of the node table's hash order. Harness: a full
  scan of 6,000 extra nodes takes 34.5 us warm; the registry ~0.
- `src/resources/character.cpp:character_posed` checks for skin weights
  before the character lookup, which used to insert a `Character` entry for
  every render node of the world list.
- The world list (`collect_root`) costs about 1 us per node, spread over the
  transform chain, paint and texture matching, and moving the large
  `SceneInstance` into the list. The mesh side of the texture matching is now
  cached per mesh and revision (`mesh_texture_keys`), and the list is
  reserved once. No per-node revision exists (`g_scene_revision` is global
  and texture and paint fields change without it), so caching whole nodes or
  whole previews across frames would need change tracking first. Memoising
  parent matrices would change the float association of `chain_matrix`, so
  the captures would no longer match.
- Harness: `city_submission` has a third case, `crowd` (2560x1440): 80
  walkers of the Valo City types (humans 0x57-0x5B, 60 ahead of the camera),
  a car-like subtree of 30 cars.rpk renders whose nodes carry a paint id
  (approximation: the paint id matches no texture, so the matching loops run
  but nothing is repainted), and the same subtree in an inset preview drawn
  by `draw_previews`. The walker clock ends at the same value in every run,
  because a step computed at a different absolute time rounds differently.
  Before -> after: walker palettes 0.30-0.32 -> 0.15-0.17 ms, previews
  0.05-0.06 -> 0.04 ms, world list 0.12-0.13 ms (unchanged within noise),
  sync_instances 0.95-1.0 -> 0.92-0.97 ms. The crowd frame's bgfx render
  thread is 9-11 ms before and after (not investigated: 80 skinned walkers
  and 30 overlapping car meshes at 2560x1440).
- The 2560x1440 flake noted for #266 also hit the caches on/off comparison;
  `city_submission` now accepts up to 64 differing bytes at most 1 apart and
  fails on anything larger.

### AO darkens large walls seen at a grazing angle (#274)

- Symptom (Elias, Valo City suburb, daytime): a house facade very dark with
  black blotches at wall bases and corners; normal with AO off or with "AO
  reference sampling" on, so the #259 AO-resolution path.
- Cause: every AO texel stands for one full-resolution depth texel, but at
  half resolution the AO texel centre lies exactly on the boundary between
  two full texels. The geometry pass picked the texel from its interpolated
  uv, the AO pass and its filters from integer AO texels
  (`fullTexelCentre(aoTexelCentre(t))`), and they did not always agree. The
  stored linear depth then belonged to one texel while the position was
  rebuilt at its neighbour. On a frontal wall the two depths are equal; at a
  grazing angle they differ a lot, so samples left the wall plane, became
  occluders and darkened open walls in bands.
- Fix: `shaders/fs_ao.sc` `aoFullCentre` picks the full texel with a
  quarter-texel bias (texel 2t at half resolution, t at full resolution) and
  every pass uses it: the geometry pass (depth, position, normal), the
  receiver and samples of the AO pass and the filter weights.
  `shaders/ambient_occlusion.sh` fetches the depth of the same texel for the
  depth-aware upsample weights.
- Test: `tests/test_scene_shadow_gpu.cpp` `ambient_occlusion`, a 30 m x 8 m
  wall alone and on a floor, frontal and grazing, near and far, AO High,
  geometry path against reference sampling. Before: open wall grazing near
  GTAO 2,817 of 8,257 pixels darkened (reference 0), HBAO 7,439. After: 0
  for both; every case within 2 % of the reference. The house itself was not
  located in the harness; the synthetic wall reproduces the fault.
### Street lamps lit while the sun is up (#271)

- Symptom (Elias, Valo City, evening): a walker had its sun shadow plus a
  softer second shadow pointing away from a street lamp, and the lamp post
  had two diagonal shadows. Turning "street lamp lighting" off removed the
  second shadow. Cause: street lamps switched on at full power on a clock
  window (`scene_sun_is_night`, 18:30-06:00) independent of the light that
  lit the scene, so a lamp's shadow tile (#237) added a second shadow beside
  the sun's. Not a regression of the #244-#269 stack: the clock gate and
  the lamp tiles come from #237/#238, and on the 2a4abf1 merge the harness
  at 13:30 shows 0 lamps lit and single shadows (no bisect was needed).
- Fix, second version (the first used the day-cycle clock and ramped from
  17:37, 68 % at 18:00 while the low sun still cast long shadows; Elias saw
  the doubled shadows again at dusk):
  `src/resources/scene_instances.cpp:scene_instances_sun_state` measures the
  sun the renderer lights with: the world's light after the day cycle
  (`scene_instances_light`), its diffuse luminance against the same light at
  noon (`scene_sun_apply_day_cycle_at`), its elevation, and whether it still
  casts sun shadows (direction y < 0.02, as in `update_static_shadow`).
  `src/resources/scene_sun.cpp:scene_sun_night_factor` maps the direct
  fraction: 0 down to 20 % of noon, smoothstep to 1 at 0.
  `src/render/bgfx_lib_backend.cpp:update_scenery_lights` scales lamp power
  by it (none below 0.01); `src/render/bgfx_shadows.cpp:update_lamp_shadows`
  draws no tile below 0.3 or while the sun casts shadows, and scales the
  lamps it adds itself. A lamp's shadow is the absence of its light, so its
  darkening fades with it. The renderer snapshot header prints the sun
  elevation, diffuse, noon diffuse, direct fraction, sun shadows and factor.
- By hour on the stock day cycle (elevation, direct, sun shadows, factor):
  05:54 -1.3 deg 0.09 off 0.60; 06:00 0.0 deg 0.15 on 0.14; 06:15 3.2 deg
  0.35 on 0; 12:00 0; 17:45 3.2 deg 0.35 on 0; 17:54 1.3 deg 0.22 on 0;
  18:00 0.0 deg 0.15 on 0.14 (lamps faint, no tiles); 18:06 -1.3 deg 0.09
  off 0.60; 18:15 -3.2 deg 0 off 1; night 1. The old clock gate was off
  until 18:30 and on before 06:00.
- Lamp geometry: the light sits at the type-13 light child (the lamp head);
  `update_lamp_shadows` keeps out of a lamp's own tile any caster whose
  bounds contain the light position (the pole and housing, `holds_lamp`).
- Test: `tests/test_scene_shadow_gpu.cpp` `city_submission(..., day=true)`
  (1280x720, crowd, sun passes and GTAO High 3 m, plus `update_lamp_shadows`)
  renders noon, 18:00 and 22:00 with street lamp lighting on and off. Noon:
  no lamp lit, no shadow slot, identical frames. 18:00 (sun shadows on):
  40 lamps at most 0.11 (factor 0.14), no shadow tile, lamp contribution
  11.7 M (sum of channel differences) over 496 k pixels. 22:00: unchanged,
  lamps 0.80, 6 shadowed, 80.7 M over 500 k pixels. The
  night captures at 640x360, 2560x1440 and crowd match the pre-#271 build.

### Baked vertex light on ground POLY under the dynamic sun (#275)

- Symptom (Elias): the Nevada Desert mountains very dark, blotchy and high
  in contrast by day. Nevada Desert is MW_Mod's `Nevada_Track` script, which
  starts `maps\ROC_track.rpk`; the terrain is `maps/roc_track/meshes/roc.scY`,
  one SCX3 ground POLY (61,038 vertices, normals present).
- Cause: POLY vertices carry a colour (byte 24 of each 56-byte vertex,
  `src/resources/scene_instances.cpp` POLY parser). On this terrain it is
  grey with alpha equal to the grey, mostly 0 or 1 (rock `sziklafal2`: 43 %
  of vertices near 0; sand `sivatag`: 49 %), brighter on slopes facing the
  original's sun: baked light. The sections do not overlap (no coinciding
  triangles), so it is not a layer blend weight. Since 5ca91e7 (2026-11-10)
  outdoor ground POLY drew with `u_alphaTest.w = -3`, where `fs_mesh`
  multiplies the texture by it, then our sun, ambient, shadows and AO lit it
  again: baked-dark vertices stayed black whatever the sun did. Not AO and
  not the cascades: the same look with AO off and map shadows off.
- Fix: `src/render/bgfx_lib_backend.hpp:baked_vertex_light` keeps the
  vertex colour only for outdoor ground POLY without dynamic lighting (where
  it is the scene's light); under the dynamic sun it is ignored, as the
  stock lightmap path already does for lightmapped surfaces.
- Test: `tests/test_scene_shadow_gpu.cpp` `desert_capture`, the desert at
  noon (sun passes, GTAO High 3 m), the mountain band behind the start
  gantry: 72.4 % of pixels under luminance 40 (mean 54) before, 0.1 % (mean
  136) after; it also checks that the data still bakes dark rock vertices.
- Side effect: Valo City's ground POLY loses its baked vertex light too. The
  night city captures differ by 3.5 on average per changed pixel (max 69):
  slightly lighter ground, same picture. `build/city_frame_reference_*.bin`
  from earlier builds therefore no longer match byte for byte.
- Not checked: SLRR_rev does not establish the original's use of this
  colour (its docs call vertex-colour blending unconfirmed and drop alpha).

### Nitrous driving and exhaust (#303, #304)

- Ownership: `src/resources/part_runtime.cpp:configure_drive` takes mapped axis
  31 from the controlled mechanical car. Native remaining starts at zero;
  `reload` fills the script capacity, while refresh only clamps. `Canister.actual`
  remains unchanged script save data. Stock Track reloads on entering driving;
  arbitrary save/load does not preserve the native aggregate. HUD fill is the
  remaining/capacity ratio, only for positive injector consumption.
- Physics: `src/physics/vehicle_dynamics.cpp:VehicleDynamics::step_drive` keeps
  dry torque and accepts an optional wet dyno callback/input, blending after the
  predictive limiter. No #215 dyno or torque-gain correction is copied. The Part
  owner consumes `RPM * .0002 * input * limiter * dt * consumption_nitro` once per
  fixed solver step. The stock injector's nominal RPM/throttle fields are not
  exported. Audio uses the configured one-shot or default `sound:25` once per
  press; release, pause, reset and teardown do not create extra triggers.
- Evidence: direct read-only MWM executable disassembly, SHA-256
  `b284a7a53909d0b9ce59d59ddc11e14df40a7eb74d86fa76efb06305aadc2b03`,
  plus MWM Chassis, Canister, NOSInjectorSystem, Track and ControlSet sources.
  Addresses, version distinctions and upstream checks are in EXTERNAL_RESEARCH §7.
- Actual authored asset: mufflers resolve `particles:1E` through `MW_Mod:28`
  (`NOS_Flames.SCX`, SCX3 flags 0x101, 48 vertices/16 triangles) and `MW_Mod:27`
  (`NOS_Flames.png`, RGBA8 425x554). Independent SCX parsing and full PNG
  CRC/zlib/filter decode establish alpha 0..209, not an opaque card. PNG uploads
  are premultiplied; only flag-256 transparent vehicle sections use the matching
  blend and no depth write. Flame geometry is excluded from lamp-glass snapping.
- Flag-256 mesh/fake-light visibility uses eligible injection, >1000 RPM,
  throttle >.01 and a shared deterministic 30% simulation-clock pulse. This is
  explicitly not the original per-render-child RNG cadence. Collection and
  cameras do not spend fuel or resample it; paused physics freezes it.
- Baiern's pipe carries two separate `particles:1F` FakeSpot children. All
  placements survive collection. `src/render/bgfx_lighting.cpp:LibBgfxBackend::draw_nitro_flares`
  draws their authored central atlas glow with source depth; they never enter
  physical road-light arrays. The Flares option gates this view-only draw.
  Sizing follows the existing sun approximation, with mode-2 atlas/cm-offset
  evidence from SLRR_rev `2663d686`. Original size units, directional convention
  and ray/ghost fidelity remain unverified; no arbitrary backward rotation,
  floodlight, replacement texture or particle emitter is introduced.
- Regression entry points: `tests/test_nitrous.cpp`, `tests/test_sdl_input.cpp`,
  `tests/test_vehicle_dynamics.cpp`, `tests/test_script_lights.cpp` and the
  `scene_shadow_gpu` nitrous case. Compilation and actual gameplay captures
  must still be reported separately; these sources alone are not pixel acceptance.

### Anti-roll bars: stock data behind the catalog branch (#300, PR #307)

Read-only scan of `game/parts.rpk` (RPAK TOC; the 16-byte TOC header sits after
the dependency table, as in `src/resources/rpk_index.cpp:RpkIndex::parse_rpak`):

- The stock RPK already holds 48 bar resources. They hang under
  `0x0220 "anti-roll"` → `0x021F "_hidden_"` → `0x0019 "runinggear"`, in nine
  brand folders `0x00DC..0x00E4` (Baiern … Shimutshibu). The stock Catalog only
  browses `0xF229` (Suspensions), so the hidden branch never showed. PR #307
  adds `parts:0x0220r` as a second root of the Running Gear/Suspension page;
  no RPK byte changes. `0xF232 "anti-roll"` under Shocks is an empty folder.
- The brand folders live in parts.rpk, so the Catalog's car-pack filter
  (`id() >> 16` against car packs) never hides them: every car sees all 48 bars,
  as it already does for the Suspensions brand folders.
- Each bar RSD is `script parts\running_gear\scripts\sways\swaybar_*.class` +
  `native part parts\running_gear\scripts\swaybar.cfg`. That CFG has
  `compatible 0x00004000 1`; `parts:0x4000` is the base class node
  `Swaybar` (`script java.game.parts.rgearpart.Swaybar`). Car mounts use
  `attach 0x00044000 1`, dependency 4 = parts.rpk. Fit is therefore generic:
  any bar fits any 701 (front) / 702 (rear) mount; the front/rear in the name
  is not a restriction.
- Only ten stock chassis CFGs author mounts: Duhen ×4 (701+702), Focer ×3
  (701+702), Einvagen ×3 (701 only). Other cars can buy a bar but cannot fit it.
- All 48 scripts set `force = 25000`, `damping = 2500`: every bar is
  functionally identical. Stock data also gives the six Ishima bars and the
  Duhen 1.8/2.0/2.2 bars `name_prefix = "Duhen SunStrip 1.5 DVC"`, which the
  patched `Swaybar.calcStuffs` now shows in their names.
- The Java `updatevariables` still has its `addrollbar` commands commented
  out; the host reads `force`/`damping` from the installed part instead
  (`src/resources/part_runtime.cpp:configure_anti_roll`).
- Every stock racer script lists its bars in commented-out
  `stock_parts_list_RGear_sways` lines (Focer RC 300 also `stg_1_`). `Chassis`
  already pushes that list with the other running-gear lists, and
  `src/resources/part_runtime.cpp:add_part` destroys a part no slot accepts, so
  a listed bar without a mount adds nothing. #311 adds the missing mounts to
  the 17 other racer chassis CFGs (Einvagen gets 702 only) and uncomments all
  24 lists. Stock lists that named the wrong bar are corrected: Duhen 1.5 rear
  `0x41FF` to `0x017C`, Ishima Enula WRZ `0x41FF` to `0x0195/0x0196` (stage 1
  `0x0197/0x0198`), WR SuperTurizmo `0x41FF` to `0x0197/0x0198`.
- **The installed MWM 2.2.1 car classes are not built from `sources/`.** MWM
  added stock neons to the NOS branches: five `addPart( parts:0xD2xxr, "<color>" )`
  after the canisters of each branch (`parts:0xD273` Red, `0xD285` Green,
  `0xD288` Blue, `0xD291` Cyan, `0xD294` Aqua, `0xD279` Pink, `0xD276` Purple,
  `0xD282` Orange, `0xD267` Yellow, `0xD270` White; neon RSDs under
  `parts:0xF24B`). The count per branch varies (5, 7 or 8). The SuperDuty top
  branch replaces the four canisters with two bottles (`parts:0xD215` Botle,
  `0xD339` Double Botle), ten neons and `cars.racers.SuperDuty:0xD1` rollbar
  (the RID form must match the `stg_` list one exactly, or the constant pool
  differs). No MWM car
  source ships, and recompiling `sources/` would drop them. The bundled compiler
  reproduces original classes byte for byte (only header bytes 9-11 differ;
  source line numbers are encoded in the tree), so a reconstruction is proven
  by an identical output. The reconstructed sources are in
  `patch/cars/racers/*_data/scripts/src/`; see SCRIPT_COMPILER.md.
- **The local `game/` may hold an installed patch.** On 2026-11-22 the patch
  (sl, parts and ten car classes) was copied into `game/` for in-game tests, so
  tests that expect stock classes failed there. To test against stock, build a
  hard-linked view whose changed files come from the untouched MWM 2.2.1 copy
  and run the test executable from it; `tests/test_swaybars.cpp` builds its
  patched game view the same way (patched files replace their link, never
  written through).

### Dashboard pins: CFG `cockpit_rpm` / `cockpit_speed` (#301)

- Format, from the MWM executable (SHA-256 `b284a7a5…2b03`, read-only
  disassembly): `cockpit_rpm` (0x00444B60) and `cockpit_speed` (0x00444CB0)
  scan `x y z yaw pitch roll render offset factor low high` with
  `%f %f %f %f %f %f %d %f %f %f %f`. The game's own scanner (0x00553890)
  reads `0x` hex for `%d`, so stock ids parse. Defaults (constructor
  0x004467D1): offset 2.2, factor -0.0005 rpm / -0.02 speed, low -2.2,
  high 2.2. The render is resolved as type 14 against the owning pack.
- Update (0x0043C0A0, mode 1): `angle = value * factor + offset`; below `low`
  it becomes `low`, otherwise above `high` it becomes `high` (no swap). The
  pin is posed at `(yaw, pitch, roll + angle)`, which with
  `src/resources/part_config.cpp:part_pose_matrix` (yaw, pitch, then roll) is
  a turn about the posed local Z axis. Values (0x0045283C): rpm is
  `|engine rad/s| * 9.5493` (0x005F0C14), speed `|velocity| * 3.6`
  (0x005F0C04), the same sources as `osd_rpmpin` / `osd_spdpin`.
- The modding tool's `needles.ts` description (field order, defaults, Z spin,
  km/h) matches the binary. Its "SLRR units, pi = 3.147" remark has no
  counterpart in the engine: the angle goes straight into the roll in radians.
- Stock data: 17 racer cfgs in `patch/` and 24 in `game/` carry both lines,
  all with pin `0x00020037` = `cars:0x37` (`cars\meshes\pin_small.SCX`).
  Enula, Nonus, SuperDuty pitch their pins; Einvagen rests at its `high` 1.5
  because its offset 2.2 is above it.
- OpenSLRR: `include/slrr/part_config.hpp:CockpitNeedle`, parsed in
  `src/resources/part_config.cpp:parse_part_config` (a line without pose and
  render, or with a bad number, goes to `unknown` and does not invalidate the
  cfg), angle in `src/resources/part_config.cpp:cockpit_needle_angle`. The
  chassis (`native_kind == "car"`) gets one RenderRef child per defined pin in
  `src/resources/part_runtime.cpp:render_part`, kept in `Record::needles`, not
  in `renders` (deformation, lamp snapping and HUD bounds must not see a part
  that turns). `src/resources/part_runtime.cpp:pose_cockpit_needles` sets
  their matrix every `part_physics_advance` after the steering column;
  `part_destroy` removes them and `part_visit_gc_roots` marks them.
- Build 606 only updates the pins in a view whose flag `+0x3C` is set and
  destroys them otherwise (0x00452836, view record `car + idx * 0x2B8 +
  0x11C`); the flag is not decoded (probably the internal camera). OpenSLRR
  keeps them on every car with the lines, in every view.
- Tests: `tests/test_cockpit_needles.cpp` (suite `cockpit_needles`): parse
  defaults and limits, stock Baiern (flat) and Enula (pitched) at rest and
  driving, pivot and axis fixed, pins removed with the car.
- Pin material: `cars\meshes\pin_small.SCX` is SCX3 with one group of flags
  `0x101` (SCX3 blend) on `cars	extures\Pin.dds`, a DXT5 card that is clear
  except for the needle. The vehicle path
  (`src/render/bgfx_lighting.cpp:draw_instance`) only sent the SCX4
  transparent bit `0x40000000` and flag-256 flames to its blended pass, so the
  card was drawn opaque: a black square behind each needle. SCX3 blend now
  blends on any vehicle mesh without `light_flags`. In the MWM `cars/` and
  `parts/`, the other SCX3 blend groups are lamp glows (`lampglow.SCX`,
  Focer and Enula light meshes), drawn with `light_flags`, so unchanged.
- In game (`--bench-drive`, ValoCity, Baiern, F1 with the #318 fix): both pins
  show on the dashboard dials at rest, thin needles with no card. The chase
  view and the overlay pins are unchanged.

### Pointer gestures and race cameras: review notes on PR #308 (#295-#299)

Verified on the PR branch (f422ebb), by reading the code; not run in game.

- The loading dialog's click/Enter to continue does not use window messages:
  `src/boot/mainloop.cpp:mainloop_frame` polls `GetAsyncKeyState`
  (VK_LBUTTON, RETURN, SPACE, ESCAPE) with its own armed/held edge. Gating
  `WM_LBUTTONDOWN` under the plate therefore cannot break the skip.
- Stock `ControlSet.java` binds look keys only in group 3 (`CAMTURNSET`):
  arrows at 0.005 (yaw) and 0.003 (pitch), the same constants the host had
  hard-coded. No group binds axes 7/8 on a gamepad, so routing the look
  through `host_input_camera_axis(controller, axis)` keeps stock speed.
- `angle offset gain rate` is used beyond ROC: `City.java` night races
  (start orbit around the bot, finish orbit) call `angle 0 4.0 0.7853` then
  `dist 5.5 6.5`, and `angle 0 0` with `dist 8.0 16.0` at the start. These
  paths were not run in game by the PR.
- A held follow camera (`follow_hold`, set by `setMatrix` on the bound
  follow camera or by a positive `angle` gain) is clamped to the `dist`
  range every frame and released only when gain is 0 and the eye is more
  than `dist max + 0.5` from the car, or on a right-drag. A staging point
  beyond `dist max` is pulled in at once; City day races stage at about
  7.2 m with the default `dist 1 10`, so they hold.
- `zoom` keeps `aov_goal`; the camera constructor (`natives_osd.cpp`) and
  the cockpit camera rebind (`part_runtime.cpp`) set `half_aov` directly but
  not `aov_goal`, so a reused camera eases back to an older zoom goal.
- `host_input_cancel_pointer_gestures` turns a held world button into a
  `world_release`, which `dispatch_world_pointer` emits as cursor event 2
  in the frame after the boundary, without a matching press in that scene.

### Doors and hoods open with build 606's spring (#327, from #204)

- Before: a script open/close (`Part.flap`, `flap_toggle`) drove the hinge
  with a critically damped servo (9 rad/s): the panel glided to its stop like
  an interpolation. In the original game it visibly shakes.
- Build 606 (read-only disassembly): `flap_toggle` (0x00474B00, switch on the
  command) only sets flag 2 (open) or 4 (close) at joint+0x88, bit 1 being
  the latch (0x0043F280 releases it). The flap solver (0x0043F850; the drive
  at 0x0044030D) adds `gain * (target - edge) * dt` to the free edge's
  velocity, with no damping term there: open targets the edge at
  `0.05 lo + 0.95 hi` of the CFG travel and close the `lo` pose, both with
  gain 60 (0x42700000); otherwise the gain is a per-joint value at +0x34,
  used only above 0.1. Gravity acts on the same edge, so a heavy hood sags
  about 9.81 / 60 m below the target. Not decoded: the term scaled by
  joint+0x7C at 0x0043FF05 (likely a damping), and the CFG field order of
  +0x24 / +0x28 / +0x34.
- OpenSLRR (`src/physics/flap_hinge.cpp:flap_step`, `FlapTuning`): the open
  hold is `script_spring * (0.95 upper - angle) - script_damping * rate`
  (60 s⁻², 2.3 s⁻¹, a host damping ratio near 0.15 in place of the undecoded
  term), the close drive the same spring aimed 0.35 rad through the closed
  pose. The opening counts as finished once the panel has been free for 1 s
  and moves under 0.05 rad/s; the weaker door-check limit applies only then.
- Simulated (Baiern door, 1.134 rad travel): it reaches the stop at 0.25 s,
  falls back to 0.95, returns to the stop at 0.8 s and rests near 1.05-1.08
  by about 1.2 s. The Baiern hood rests near 0.66 of its 0.873 travel. A close
  latches in 0.2 s. Tests: `tests/test_flap_hinge.cpp` (12/12), including the
  overshoot onto the stop and the fall back below the target.

### ROC garage: tent canvas and sunlight (#330)

- Canvas. The ROC stuff mesh (`misc/garage/g4/roc_g_egyeb.SCX`, stuff 0x51)
  is a script `RenderRef`, so `instance.cull_backfaces` is false and the map
  rules for authored SCX4 coverage do not apply. That path cut every SCX4
  section at half alpha. The canvas section `satorteto` is ColorMixTex with
  specular (type 7, flags 0, diffuse 0.70/0.06/0.06); its
  `maps/textures/satortetoanyag.dds` (DXT5) has alpha 0.13-0.33 on the canvas
  and 255 on the letters, so only the letters stayed. Now
  `include/slrr/scx_mesh.hpp:scx_legacy_cutout` keeps the half-alpha cutout
  for plain Tex sections and drops it for ColorMixTex families
  (`scx_color_mix_function`, the ten kinds the vehicle path already treated as
  colour mix) unless the alpha-test flag 0x20000000 is set. The canvas is
  drawn opaque with its texture colour, like the map object
  `objects/meshes/sator/sator.scx` (same section, flags 0x80000000).
- Why not authored coverage for all script RenderRefs: a runtime log of the
  SCX4 sections drawn by script RenderRefs with neither coverage flag (G1,
  G2 club garage, ROC, city) found 38 sections; only two have texels under
  half alpha: the canvas, and a plain Tex brace in the ROC (`1 - Default`,
  `tabmatte1_alphas.dds`, 70 % transparent, a diagonal strip) that needs the
  cutout. ColorMixTex colour mixing (diffuse red under the canvas alpha) is
  not applied to script RenderRefs or to type 7 map sections; the stock look of
  that red is unverified.
- Sunlight. Stock `Garage` sets `internalScene = 1` in its constructor, so
  `Scene.addSceneElements` creates no sun, and it instances the
  `misc.garage:0x001D` neon, which the host turns into a point light 3.3 m above
  the map origin (`update_local_light`). In the open-air ROC that lit almost
  nothing. Worse, a valid neon point record puts `draw_instance` in point-light
  mode (`u_sunDir.w = -1`), so a sun alone would not have lit it either. The
  patched `Garage.enter` sets `internalScene = 0` when `roc` is set (1
  otherwise) before `addSceneElements`, and creates no neon in the ROC
  (`exit` destroys it only if present). `exit` and the one-second `refresh`
  reuse the value set by `enter`. Effects in the ROC: the day-cycle sun,
  procedural sky, sun shadows and the time-of-day fog and envmap of
  `Scene.addSceneElements` (the envmap replaces `garageEnvMaps[3]` at the first
  refresh). At night it is as dark as an outdoor map, with no lamp.
- Validation: `map_alpha` 5/5 (new `roc_tent_canvas_is_not_cut`),
  `inventory_transition` 6/6 and `script_lights` 5/5 with the shipped class.
  In game (`--bench-career ... --auto-cmd 128`, a temporary env hook setting
  the hour and taking renderer screenshots, removed): canvas and valance
  visible, sunlit sand and shadows at 13:00; club garage unchanged;
  `osdCommand(CMD_ROCINFO)` leaves the ROC garage without error. At 20:00 the
  lower car body looks lit from below; not investigated, it may be the general
  dusk sun or reflection rather than this change.
- Rebuild of the shipped class: `build/issue315-compile-root` (a game copy)
  reproduced the previous `patch/sl/Scripts/game/Garage.class` byte for byte
  before the edit; see SCRIPT_COMPILER.md "Shipped patch classes (#235)".
