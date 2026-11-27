# OpenSLRR knowledge base for coding agents

Verified against `main` at `b907b6b` (12 November 2026). Every code reference
below is written as `path` or `path:symbol` and can be rechecked with
`python scripts/check_knowledge_refs.py`. Symbols are used instead of line
numbers so the references survive edits.

This file condenses what an agent needs before touching the engine. It does
not replace [ENGINE_CONTRACT.md](ENGINE_CONTRACT.md), which remains the
authoritative contract. Knowledge taken from the sibling project SLRR_rev
and from draft PR #3 is in [EXTERNAL_RESEARCH.md](../research/EXTERNAL_RESEARCH.md).
The proposed roadmap is in [MILESTONES.md](../planning/MILESTONES.md).
Verified notes on individual issues live with their subsystem (§14); the
[documentation index](../README.md) lists every file.

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
14. [Subsystem notes](#14-subsystem-notes)

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
  (see [EXTERNAL_RESEARCH.md](../research/EXTERNAL_RESEARCH.md#1-sources-and-how-far-to-trust-them)).

**SLRR_rev first.** SLRR_rev implements most game mechanics. Before
implementing a missing native or mechanic, check whether SLRR_rev already
has it and migrate it into the owning OpenSLRR layer, recording the source
file and revision. Verify the result against the MWM scripts, and keep
SLRR_rev's own design choices labelled as such. The procedure and a map of
current gaps to SLRR_rev code are in
[EXTERNAL_RESEARCH.md §7](../research/EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev).

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
- Asset test suites (`car_lights`, `swaybars`, `race_vehicle`, `nitrous`)
  read the stock install from `game/` (else `data/game`) relative to the repo
  root. If `patch/` has been copied into that `game/`, they fail: patched
  racer classes install sway bars and the headlight checks differ. To run
  them on stock, start the compiled `build/<suite>-tests/<suite>-after.exe`
  from a short folder (MAX_PATH) holding a `game` junction to a stock install
  and a `patch` junction to the repo's `patch/`. `port_flow` instead needs
  `data/game` = stock with `patch/parts` (minus `src/`) installed over it.
  On 2026-11-23 the merged #215 branch passed all of them this way.
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

- Build requirements and commands are in [README.md](../../README.md#build).
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
  `--float-mode A|B|C|D|auto` comes first; the default `auto` keeps each
  unit's float literals as the class it replaces has them, while
  `System.compileAll` stays on B (#420, [scripting/notes.md](../scripting/notes.md)).
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
  (`src/boot/boot.cpp:boot_game`). The benchmark car shifts automatically
  whatever the saved transmission (`src/resources/part_runtime.cpp:configure_drive`).
  That straight drive leaves the road after about 12 s, so use a recorded
  drive for measurements (#358): `--record-path FILE` writes the player
  car's published local matrix every frame of normal play, and
  `--replay-path FILE` makes the benchmark car follow it in external motion
  (`src/boot/drive_path.cpp`, `src/resources/part_runtime.cpp:part_set_external_pose`);
  the drive then lasts the recording. `--seed N` fixes the script RNG
  (`Math.randomize`, `src/natives/natives_lang.cpp:natives_fix_random_seed`).
  `--auto-cmd demo` runs the main menu's DEMO race (measured like the
  benchmark with `--bench-drive`), but two runs diverge within 10 s, so it is
  not an A/B scene. `scripts/perf_baseline.ps1` runs the replay over day and
  night, CPU affinity profiles and graphics presets; the reference drive and
  numbers are in `assets/validation/perf-baseline/` and
  [rendering/performance.md](../rendering/performance.md#baseline-on-a-recorded-drive-358).
- Runtime environment variables: `SLRR_GAME_ROOT` (`src/main.cpp`),
  `SLRR_VLC_DIR` (`src/platform/video_fmv_vlc.cpp`) and `SLRR_AMBIENCE_LOG`
  (`src/resources/scene_ambience.cpp`, debug logging of random ambience
  requests). `SLRR_TOOLS_CACHE_DIR` is build-time only (see README.md).
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
  OSD builder names to the class of the dialog's Osd (`java.render.Osd`, or
  `java.render.osdx.Osd`) (`src/jvm/jvm.cpp:invoke_by_name`). OSD v2 and the
  osdx package: [ui/osd-v2.md](../ui/osd-v2.md).
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
10. After the ticks of a frame, `publish_render_poses` always blends each car
   from its pose before the last tick to the published one, by the
   unsimulated fraction of a tick
   (`src/resources/part_runtime.cpp:publish_render_poses`). The Classic
   framerate mode (renderer option 5) is gone.
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
  (`gametype 0x<pack:id> params x,y,z,yaw,pitch,roll[,seed]`; a placement
  with no `params` line sits at the map origin), `EXTP`
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
  [scripting/natives.md](../scripting/natives.md#map-ambient-sounds).
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
- The four lamps (`led_ABS`/`led_ASR`/`led_HB`/`led_ER`, cars 0x61-0x64)
  share the gauge plate's anchor in build 606; their meshes place them on
  the plate. ABS and ASR light while the aid intervenes, the handbrake lamp
  with the lever (#446). Host option 32 adds a brake and tyre temperature
  panel above the damage gauge (`src/render/bgfx_osd.cpp:draw_vehicle_hud`).
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
  `values` list for other files. Schema: [saves/json-schema.md](../saves/json-schema.md).
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
| Audio and video | SDL3_mixer (effects/music/Cinepak PCM), optional VLC/Windows codecs | Their own decode and output threads |

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
(`src/render/bgfx_ao.cpp`), each car lamp tile (`src/render/bgfx_shadows.cpp`),
each sun cascade (#359, `src/render/bgfx_shadows.cpp:LibBgfxBackend::update_static_shadow_inner`:
collection, mesh caching, cache stamps and the view setup stay on the host;
each cascade's culling and draws are one job sharing the collected casters)
and each reflection face draw instead of submitting it, and
`src/render/bgfx_lib_backend.cpp:run_worker_submits` runs the queue on the
pool while the host thread draws the world on encoder 0. The world itself is
split too (#359, `src/render/bgfx_lib_backend.cpp:LibBgfxBackend::submit_world`):
the opaque part goes to four consecutive Sequential views
(`kWorldOpaqueView` 151–154, ordered just before `kWorldView` by
`apply_base_view_order` and the inset order in `draw_previews`), sky and
prop batches first, then pass 0 over three equal runs of `instances_`; the
first view takes the world view's clear, and the host submits only the
blended part (pass 1, night overlays) to `kWorldView`. AO reaches those views
through `ao_receiver_view`. bgfx starts with 16 encoders
(`init.limits.maxEncoders`): with the default eight, a day frame's jobs
outnumbered the free encoders and the rest ran serially on the host after
the world. Rules:

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

**Where to build next.** [MILESTONES.md M7](../planning/MILESTONES.md#m7--multicore-frame)
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
  earlier #48 curve, with the #383 tyre: `src/physics/tire_model.cpp:tire_forces`
  is one combined-slip Magic Formula curve (C from sliction, B from
  stiffness, E -1, peak friction 1.15 × script friction ratio, the original's
  load smoothing), fed a relaxed slip angle
  ([driving model](../vehicles/driving-model.md)).
  `setCPatch` tolerance and `setInstantCenter` data are copied/stored but have
  no active solver effect; there is no shared lateral impulse bound. Anti-dive
  and anti-squat use typical shares by suspension type instead
  (`WheelState::jacking`, #469, [notes](../vehicles/notes.md#anti-dive-and-anti-squat-469)). The approved brake fix consumes one impulse budget before tyre
  force and holds the reaction with the unused budget; raised launch take-up
  still requires the service brake below 0.05
  (`src/physics/vehicle_dynamics.cpp:VehicleDynamics::step_drive`).
  See ENGINE_CONTRACT §11 and [the migration plan](../vehicles/dynamics-plan.md).
- Installed body low points are cached from real mesh vertices, not the
  chassis AABB's empty underside corners
  (`src/resources/part_runtime.cpp:rebuild_body_support`). Hidden meshes keep
  their supports; corner gear and hinged panels have separate contact paths,
  and engine parts (`EnginePart`, exhaust included) are not supports.
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
| Tyre curve (#383) | peak friction 1.15 × friction / 1.76863; C from sliction (slide/grip ratio); B = stiffness / (C × peak); E -1; lateral relaxation 0.3 m | `src/physics/tire_model.cpp:tire_forces` |
| Hinged panel step / latch release / tear-off / script spring | 240 Hz / 3.5 m/s local impact speed / 4.5 m/s contact speed or 0.15 m blocked overlap / 60 s⁻² toward 95% open (build 606), damping 2.3 s⁻¹ | `src/resources/part_runtime.cpp:kFlapStep`, `include/slrr/flap_hinge.hpp:FlapTuning` |
| Prop cell | 64 m | `src/resources/scene_instances.cpp:kPropCellSize` |
| Shadow maps | 2048² | `src/render/bgfx_lib_backend.hpp:kShadowResolution` |
| Pedestrian radius / density scale | 400 m / ×4 | `src/resources/part_runtime.cpp:tick_pedestrians` |
| Walker body radius / speed | 1 m / 1 m/s (original) | `src/resources/pedestrian_lines.cpp:kRadius` |
| Walker hard wall clearance | 0.5 m (OpenSLRR) | `src/resources/pedestrian_lines.cpp:kWallClearance` |
| Live GameRef IDs | from `0x70000000` | `src/resources/gameref.cpp:g_next_live` |

## 8. Native coverage at a glance

From [scripting/natives.md](../scripting/natives.md) and
`assets/progress/native-coverage.json` (26 November 2026): of 376 stock
signatures, 370 have a body, 3 are explicit no-ops and 3 are missing.
`scripts/render_native_coverage.py` recomputes these counts from `src/`;
`--list missing` prints the missing signatures.

| Missing family | Count | Blocks |
| --- | ---: | --- |
| Leftovers (`RenderRef.addPoints`, `weld`, `progress`: the welder) | 3 | No stock caller found |

The `GroundRef` route and traffic natives that this table listed are bound
since #180.

The three `Painter` natives that this table listed are bound since #64;
see [ui/notes.md "Garage paint booth (#64)"](../ui/notes.md#garage-paint-booth-64).

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

No known disagreement since the 24 November 2026 recheck of #44 (the native
counts in §8 and two stale code comments were corrected). Add a row here
when a statement and the code part:

| Statement | Code reality |
| --- | --- |
| None known | — |

## 10. Open risk register

Risks found by the PR #3 source review (baseline `60d7d71`) and rechecked on
`main`. "Open" means the code still has the pattern; none was reproduced at
run time.

| Risk | Where |
| --- | --- |
| DirectShow dynamic media-format renegotiation is not supported; malformed/short RGB32 frames are rejected | `src/platform/video_fmv.cpp` (`grab_frame`) |
| `FindFile` and File share canonical physical-root confinement (#84). Enumeration alone may address the root; leading separators remain game-relative, parent traversal and external junctions are rejected, and internal junctions are allowed. File.delete remains nonrecursive (#68, #71). Invalid SDAT writes preserve alignment (#70); see [saves/notes.md](../saves/notes.md#filesave-io-fixes--issues-68-70-and-71) | `src/natives/natives_boot.cpp:under_game`, `src/natives/natives_boot.cpp:findfile_start`, `src/natives/natives_io.cpp:register_io_natives` |
| Boot adopts the section already entered by Java Init without replaying lifecycle (#76). Windowed startup retains a timer-only reset so startup work cannot consume the splash display interval. | `src/boot/boot.cpp:boot_game`, `src/boot/mainloop.cpp:mainloop_adopt_section` |
| Released cars retain static-world contacts without a driver/engine. Shared chassis OBB pairs and all-car movable-body contacts now use impulse-aware damage/detachment; full Bot recovery/audio/seating remain partial. See [traffic/race audit](../world/traffic-races.md). | `src/resources/part_runtime.cpp:simulate_batch`, `src/resources/part_runtime.cpp:sim_loose_bodies`, `src/resources/part_runtime.cpp:sim_free_bodies` |
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
  in [vehicles/physics-tick.md](../vehicles/physics-tick.md). The owner permits changed
  engine outputs: cycle torque is now SI net crank Nm, sampled power is T·ω,
  and driving consumes that curve without the legacy ×2 gain or duplicate WOT
  loss. The shipped Java head/Inline/Vee patch normalizes capped curtain area by
  a declared 1-ms ideal-air reference exchange volume, replacing X²/V. The
  added DynoData fields are the fuel's `knockCompression` (#407) and the
  supercharger's `charger_*`/`crank_pulley_diameter` (#415), read by name by the
  host dyno; full pressure-dependent flow and independent-thread
  ownership remain open. The actual-solver throttle
  experiment covers digital/analog plate response, cut hysteresis and latched
  BOV events without enabling new production controls. `interpolation_ms`
  measures pose publication inside Physics, not downstream renderer caches.

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
   partial or unvalidated; check [scripting/natives.md](../scripting/natives.md) (§8).
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
    not `sl2_pacejka.cpp`. Since #418 a Magic Formula (MF 5.2 structure:
    pure lateral/longitudinal curves, load sensitivity, camber, normalised
    combined slip) shaped by script friction, stiffness, sliction, load
    capacity/smoothing and `frictn_x`, with the hub's camber against the road;
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
- Write a new issue's verified notes in its subsystem's notes file (§14), not
  here. This file keeps only cross-cutting knowledge: evidence rules, maps,
  data flows, invariants, constants, risks and open issues.

## 14. Subsystem notes

Per-issue findings (what was wrong, the fix, how it was verified) are grouped
by subsystem. Each file is a set of independent `## <title> (#issue)` sections.

| Subsystem | Reference | Issue notes |
| --- | --- | --- |
| VM, scripts, natives | [runtime](../scripting/runtime.md), [compiler](../scripting/compiler.md), [GC](../scripting/vm-gc.md), [natives](../scripting/natives.md) | [scripting/notes.md](../scripting/notes.md) |
| Rendering | [contract](../rendering/contract.md), [roadmap](../rendering/roadmap.md) | [lighting](../rendering/lighting.md), [shadows and AO](../rendering/shadows-ao.md), [performance](../rendering/performance.md), [materials](../rendering/materials.md), [backend](../rendering/backend.md) |
| Vehicles, physics, driver | [driving model](../vehicles/driving-model.md), [physics tick](../vehicles/physics-tick.md), [dynamics plan](../vehicles/dynamics-plan.md) | [vehicles/notes.md](../vehicles/notes.md) |
| World, maps, traffic, races | [traffic and races audit](../world/traffic-races.md) | [world/notes.md](../world/notes.md) |
| Interface, OSD, cameras, input | [script runtime](../scripting/runtime.md) | [ui/notes.md](../ui/notes.md) |
| Saves, files, options | [JSON schema](../saves/json-schema.md) | [saves/notes.md](../saves/notes.md) |
| Resources, audio, video | §5.4 | [resources/notes.md](../resources/notes.md) |
| Build system | §3 | [platform/build.md](../platform/build.md) |
