# Rendering notes: performance

Uniform and texture-stage costs, prop LOD and culling, world-list
collection and batching, with in-game measurements.

## Per-view light uniforms (#246)

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

## Map prop LOD and far size cull (#252)

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

## Light data texture (#254)

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
  - (Resolved by #256, see "GPU time per pass" below for the current AO
    cost.) The AO pass used to read the full-resolution D32F prepass twice
    per sample and rebuild the 9-tap normal in every pass. Since #256 the
    geometry pass stores the AO-resolution linear depth and the normal once,
    and `sampleDelta` reads that linear depth (one fetch per sample) except in
    the reference mode.
  - Prop LOD (#252) keeps both meshes of a chain as props, so the collection
    loops visit about twice as many props for the 16,602 chained placements.
    Each runs the frustum test before the LOD test, and `prop_lod_visible`
    recomputes the transformed centre and distance for both of a placement's
    props. That matches the +0.3 ms of collection seen in game. Testing the
    LOD before the frustum test, caching the level per placement and frame, or
    keeping per-LOD lists would remove it.

## LOD selection cost in collection (#257)

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

## Measured in game after the renderer performance stack (#246–#257)

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

## Static prop world cache and persistent batches (#260)

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

## Per-view uniform values, kept texture stages and residency skip (#262)

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

## Instanced night overlays and blended props, world list zones (#264)

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

## Parallel prop batch collection and collection sub-zones (#266)

- `scene_instances_collect_batches` runs its placement loop on the job pool
  ([knowledge base §5.10](../core/KNOWLEDGE_BASE.md#510-threads-and-the-worker-pool) "Per-frame work on the pool"). A first version chose the LOD in a
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

## Walker palettes, camera list and world list costs (#268)

- Walker palettes were already built once per frame and shared by every
  collection (`g_frame_characters`); the cost is the pose rebuild. New ones
  now run on the job pool (8 or more, with 4 or more pool threads), see
  [knowledge base §5.10](../core/KNOWLEDGE_BASE.md#510-threads-and-the-worker-pool).
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

## Baseline on a recorded drive (#358)

The straight `--bench-drive` run used for earlier numbers leaves the road
after about 12 s and sits against a fence for the rest of the run (and, with
a manual-transmission save, never left neutral; the benchmark now forces the
automatic gearbox). The baseline therefore replays a 135 s drive Elias
recorded in Valo City (`--record-path` / `--replay-path`, KNOWLEDGE_BASE §3),
through downtown with pedestrians, residential streets, the expressway and
the underpass. Files and machine: `assets/validation/perf-baseline/`.

`scripts/perf_baseline.ps1` runs it at 13:00 and 22:00, with the process
restricted to every logical processor (`all`), four P-cores with
Hyper-Threading (`p4`, logical CPUs 0-7) or four E-cores (`e4`, 16-19), and
with Elias's settings (2560x1440, MSAA 8x, GTAO High, shadows High / Very
soft, reflections and clouds on) or a `low` preset (1920x1080, no MSAA, no
AO, low shadows, no dynamic reflections or volumetric clouds). Averages over
the drive, ms (i7-13700KF, RTX 4080, DDR5 at 4000 MT/s):

| Run | Frame | p95 | Flush | Shadows | Collect | Prop batches | World | Physics | Render thread | GPU | Draws |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| day all saved | 6.33 | 7.01 | 5.06 | 2.56 | 1.00 | 0.50 | 0.81 | 0.59 | 2.47 | 4.19 | 2998 |
| day p4 saved | 6.79 | 7.65 | 5.28 | 2.63 | 1.15 | 0.61 | 0.82 | 0.64 | 2.54 | 4.48 | 2994 |
| day e4 saved | 10.32 | 11.13 | 8.75 | 4.50 | 2.09 | 1.07 | 1.23 | 0.67 | 3.84 | 4.85 | 3013 |
| night all saved | 3.57 | 4.15 | 2.57 | 0.08 | 0.89 | 0.44 | 0.83 | 0.46 | 1.82 | 3.29 | 2215 |
| night e4 saved | 5.53 | 6.35 | 4.18 | 0.14 | 1.88 | 0.96 | 1.28 | 0.57 | 2.88 | 4.35 | 2233 |
| day all low | 6.12 | 6.73 | 4.81 | 2.54 | 1.03 | 0.51 | 0.78 | 0.61 | 1.89 | 2.36 | 2402 |
| day e4 low | 9.89 | 10.62 | 8.29 | 4.33 | 2.09 | 1.09 | 1.21 | 0.69 | 2.97 | 3.78 | 2414 |
| night all low | 3.06 | 3.61 | 2.15 | 0.08 | 0.87 | 0.42 | 0.80 | 0.41 | 1.30 | 1.68 | 1620 |

The full table, with `p4` night runs, is `replay-expo-off/summary.txt`.

Findings:

- The frame is CPU-bound in every run (present wait at most 0.15 ms, GPU
  below the frame time).
- By day, shadow submission on the host is the largest single cost, almost
  all of it the sun cascades (`sun_shadows_ms` 2.54 of 2.56): 2.5 ms of a
  6.3 ms frame on all cores, 4.5 ms of 10.3 ms on four E-cores.
  At night it is gone and the frame is almost twice as fast. The sun cascades
  are not on worker encoders (#270 only moved the AO prepass, car tiles and
  reflection faces), so they are the first target of #359.
- The `low` preset barely changes the CPU frame (6.33 → 6.12 ms by day): low
  shadow quality still collects and submits every cascade. It only lowers GPU
  time (4.19 → 2.36 ms). A weaker machine has no setting that cuts the host
  cost of sun shadows.
- The 1.55 ms "instanced prop batches" zone of the #260/#262 snapshot does
  not reproduce on this drive: 0.44–0.51 ms on all cores, about 1 ms on four
  E-cores. It predates the parallel batch collection of #266.
- The heaviest stretch is downtown by day: about 9 ms per second on all
  cores, 15 ms on four E-cores, with 4,300–5,800 draws. No frame went above
  33 ms in any run.

## Sun cascades on worker encoders (#359, first increment)

- `LibBgfxBackend::update_static_shadow_inner` queues one job per sun cascade
  through `queue_or_run`, like the car lamp tiles (#270). The host keeps the
  outer-frustum collection (`scene_instances_collect`,
  `scene_instances_collect_batches`), `cache_scene_mesh`, the cache stamps
  and every `bgfx::setView*` call. Each job tests each caster against its
  cascade frustum, filters the prop batches and submits on its own encoder.
  The collected casters live in a `shared_ptr` the jobs share; they read the
  mesh cache only after the host has filled it, pass the shadow-local flag to
  `draw_prop_batches` explicitly, and use a copied shadow origin and cascade
  radius. A cascade turned off in the debug menu is touched on the host and
  queues nothing.
- Harness (`scene_shadow_gpu`, new `city_submission` day + workers case,
  crowd street at noon, 1280x720, 24 threads): threaded and one-thread images
  are byte-identical over 40 frames; host shadow time 6.23 → 3.23 ms, worker
  submission 3.93 ms, host wait 0.31 ms.
- Replay of the #358 drive by day (`assets/validation/perf-baseline/issue359-cascades/`):

| Run | Frame before → after | Host shadows | Worker submission |
| --- | --- | --- | --- |
| all, saved | 6.33 → 5.27 ms (−17 %) | 2.56 → 1.09 ms | 0.40 → 2.33 ms |
| p4, saved | 6.79 → 5.68 ms (−16 %) | 2.63 → 1.18 ms | 0.37 → 2.06 ms |
| e4, saved | 10.32 → 8.56 ms (−17 %) | 4.50 → 2.05 ms | 0.55 → 3.31 ms |
| all, low | 6.12 → 4.99 ms (−18 %) | 2.54 → 1.07 ms | 0 → 1.85 ms |
| e4, low | 9.89 → 8.16 ms (−17 %) | 4.33 → 1.95 ms | 0 → 2.73 ms |

  GPU time and draws are unchanged. World submission rises by 0.05–0.15 ms,
  the cost of sharing bgfx's submit state with the cascade encoders.
- Still on the host: about 1.1 ms (2 ms on four E-cores) of cascade
  preparation, mostly the outer-frustum collection, plus the world view and
  the serial part of collection (the rest of #359).

## Sun caster collection and the parallel batch merge (#359, second increment)

- New profiler zone `sun_collect` (`perf::Zone::SunCollect`, inside
  `sun_shadows`): the outer-frustum collection of the sun cascades. After the
  first increment it was 0.94 of the 1.02 ms left in `sun_shadows` (1.87 of
  2.01 ms on four E-cores), and three quarters of it was
  `scene_instances_collect_batches` over the outer frustum.
- Probing every `scene_instances_collect_batches` call of a frame (temporary
  zones, removed): serial setup 0.39 ms, parallel placement loop 0.36 ms,
  serial merge 0.51 ms (0.73 / 1.06 / 0.88 ms on four E-cores). The merge
  copied each placement's matrix and colour into its batch one by one.
- `src/resources/scene_instances.cpp:scene_instances_collect_batches` now
  numbers every hit within its batch in run order (one counter per slot),
  sizes each batch once, and copies the instances on the pool. Each batch
  lists its placements in exactly the serial order, so the output is
  unchanged: the `scene_shadow_gpu` reference-capture differences are the
  same as on `main` and threaded against one-thread stays byte-identical.
- Replay of the #358 drive by day (`assets/validation/perf-baseline/issue359-batch-merge/`),
  frame time #358 → first increment → this change:

| Run | Frame | p95 | `sun_collect` |
| --- | --- | --- | --- |
| all, saved | 6.33 → 5.27 → 4.96 ms | 6.13 → 5.67 ms | 0.94 → 0.70 ms |
| p4, saved | 6.79 → 5.68 → 5.44 ms | 6.69 → 6.41 ms | → 0.80 ms |
| e4, saved | 10.32 → 8.56 → 8.13 ms | 9.65 → 8.99 ms | 1.87 → 1.40 ms |
| all, low | 6.12 → 4.99 → 4.84 ms | 5.75 → 5.69 ms | → 0.72 ms |
| e4, low | 9.89 → 8.16 → 7.22 ms | 9.04 → 7.95 ms | → 1.20 ms |

  The main view's `collect_batches` is unchanged within noise (0.52 →
  0.57 ms): it merges far fewer placements than the outer frustum.

## Opaque world on worker encoders (#359, third increment)

- Run-to-run noise of the #358 replay, same build twice in a row: ±0.03 ms
  on all cores, ±0.2–0.3 ms on four E-cores. Between sessions the same build
  drifted by about 6 % (#366's day all-cores 4.96 ms read 5.25 ms later), so
  every comparison below is before/after in the same session.
- `scene_instances_collect_batches` merges on the pool only from 8,192
  placements (`kParallelMergeMin`). Logged in game: the main view merges
  about 3,000, reflection faces about 200, the outer sun frustum about
  21,000. The always-parallel merge of #366 had made the main view slower
  (`collect_batches` 0.44 → 0.53 ms at night, all cores); with the threshold
  it is back to 0.45 ms and `sun_collect` keeps 0.72 ms.
- `LibBgfxBackend::submit_world` cuts the opaque world into four consecutive
  views drawn before `kWorldView` (KNOWLEDGE_BASE §5.10): sky and prop
  batches, then pass 0 over three runs of `instances_`, each a worker job.
  The host keeps the blended part. One opaque view was not enough: the host
  finished its small blended part and waited for the single worker
  (harness host wait 1.0–1.6 ms). AO is decided per view
  (`view_id == ao_world_view_`), so the opaque views got none until
  `ao_receiver_view` included them; the harness caught it (129,738 bytes
  differ, max 17).
- With four opaque jobs a day frame queues more jobs than bgfx's default
  eight encoders. Jobs without an encoder ran serially on the host after the
  world, and day frames got slower (all cores 4.91 → 5.30 ms). bgfx now
  starts with 16 encoders.
- Tried and dropped: the host claiming queued jobs once its own world work
  is done. Host wait fell (e4 day 1.44 → 0.66 ms) but frame time did not
  change; the jobs' total work is the critical path.
- Harness (`scene_shadow_gpu`, night and day workers cases): threaded and
  one-thread images byte-identical; reference-capture differences unchanged.
- Replay of the #358 drive, same session
  (`assets/validation/perf-baseline/issue359-world-split/`, before = threshold
  only, after = this increment):

| Run | Frame | p95 | Host wait |
| --- | --- | --- | --- |
| day, all | 4.91 → 4.79 ms | 5.56 → 5.38 ms | 0.22 → 0.88 ms |
| day, e4 | 7.97 → 7.82 ms | 8.69 → 8.40 ms | 0.38 → 1.44 ms |
| night, all | 3.72 → 3.60 ms | 4.43 → 4.23 ms | 0 → 0.48 ms |
| night, e4 | 5.44 → 5.00 ms | 6.15 → 5.64 ms | 0 → 0.68 ms |

  The gains are small: the four opaque jobs together take longer than the
  host did alone (worker submission 2.28 → 3.16 ms by day), the cost of
  bgfx's shared submit state that #270 already measured.

## Far sun cascades on alternate frames (#361, first increment)

- Per-view draws on the #358 replay (600-frame averages, dense start of
  the drive → open road): opaque prop batches (view 151) 1,100 → 230;
  opaque instances (152–154) 580 → 270; sun cascades (0, 2–4) 1,500 → 500,
  800 of them in the 1,000 m cascade alone; AO prepass (140) 610 → 210;
  lamp tiles (136, night) 630 → 330; reflections (72) about 200.
- Tried and dropped: merging map sections on the fields `fs_mesh` reads
  (ignoring specular, shininess, reflection and lightmap slots in
  `same_material`) removed 0.5 % of prop-batch draws; the remaining sections
  differ by texture. About 40 % of prop batches hold one placement.
- Also measured for #360 and set aside: routing `u_color` and `u_alphaTest`
  through the uniform cache. bgfx re-uploads the whole vertex and fragment
  constant buffers for any draw that sets a uniform, and 69 % of world draws
  change none, but the replay showed no change (render thread 2.54 ms by
  day before and after): the world view is about 290 of 3,000 draws.
- `update_static_shadow_inner` now redraws cascades 2 and 3 on alternate
  frames while the camera moves (shadows-ao.md). Harness
  (`scene_shadow_gpu`): the still-camera captures are byte-identical to
  main; a new case checks the alternation, the invalidation after a gap and
  the option. In game (day replay, 2560x1440), the start frame, where two
  runs without alternation differ by 55 pixels, differs by 486,000 pixels
  with it: 98.5 % by at most 2/255, 1,001 by more than 16/255, scattered
  over distant foliage. Later frames differ between runs anyway (traffic).
- Day replay, same session
  (`assets/validation/perf-baseline/issue361-cascade-alternation/`):

| Run | Draws | Frame | p95 | Worker submission | Render thread |
| --- | --- | --- | --- | --- | --- |
| day, all | 3,002 → 2,515 | 4.60 → 4.29 ms | 5.16 → 4.86 ms | 3.12 → 2.45 ms | 2.46 → 2.09 ms |
| day, e4 | 3,012 → 2,523 | 7.89 → 7.10 ms | 8.79 → 7.83 ms | 4.48 → 3.42 ms | 4.02 → 3.38 ms |

  The p4 runs were unstable in this session for the unchanged build (5.46,
  6.51 and 7.47 ms on three runs), so they are not reported; their worker
  submission and render thread fell like the others (2.74 → 2.24 ms,
  2.74 → 2.15 ms). Night has no sun cascades and is unaffected.

## CPU occlusion culling of map props (#361, parked)

Prototyped and parked (not merged): correct, but it
costs more CPU than it saves. Read this before retrying occlusion culling.

- Potential, measured by testing the main view's prop placements against
  the AO prepass depth (day replay): in the dense start of the drive about
  75 % of prop-batch submits and 88 % of placements are fully hidden; the
  large instances (ground sections) almost never are.
- GPU depth readback is not usable as is: bgfx's D3D11 `readTexture` copies
  and maps the staging texture at once (`renderer_d3d11.cpp`), so the render
  thread waits for the GPU frame. An asynchronous readback would need a
  bgfx patch, works only with AO on (the low preset has no depth prepass)
  and lags 2–3 frames.
- The prototype rasterised large opaque polygons (4 m² and more, walls only,
  triangle pairs merged into quads) into a 256 x 144 view-depth buffer on
  the job pool, with inner coverage and the farthest depth per texel, then
  removed hidden placements from the opaque world and the AO prepass only
  (lamp tiles keep them: a hidden prop still casts).
- Lessons for a retry:
  - Most Valo City buildings are batched map props, not instances: the
    instances in view are mostly traffic car parts and walkers. Occluders
    must include the prop placements.
  - Many SCX4 sections carry the alpha-test flag on fully opaque textures;
    without reading the texture's smallest alpha (DXT1 3-colour blocks,
    DXT3/5 alpha) almost nothing occludes.
  - The two Valo City ground meshes hold 450,000 triangles, about 26,000
    polygons after the filter.
- Correctness: the harness city captures and the in-game start frame (about
  50 % of placements culled) were byte-identical with culling on and off.
- Cost against saving (same session,
  `assets/validation/perf-baseline/issue361-occlusion/`): culling took 0.8 to
  1.1 ms a frame (mesh cache lookup 0.1–0.25, raster 0.4–0.7, tests
  0.1–0.25) and saved about 0.2 ms of render thread and 0.2–0.3 ms of worker
  submission. A draw goes only when a whole batch is hidden, so draws fell
  about 10 %:

| Run | Frame | Draws |
| --- | --- | --- |
| day, all | 5.86 → 6.37 ms | 2,515 → 2,271 |
| day, e4 | 8.25 → 9.21 ms | 2,524 → 2,285 |
| night, all, low | 3.73 → 4.62 ms | 1,623 → 1,443 |
| night, e4, low | 5.27 → 6.32 ms | 1,622 → 1,449 |

- Spatial clusters for the ground meshes changed nothing. A 128-texel
  buffer, three jobs and occluders within 200 m cut the cost to about
  0.47 ms but also the saving (2,350 draws). Paying off would need a cost
  near 0.1–0.2 ms: a SIMD rasteriser and precomputed simplified occluders.

## GPU time per pass (#362)

- `--perf-log` now writes `gpu_<pass>_ms` columns: map shadows, car depth,
  interior sun, world (views 5 and 151–154), AO prepass, AO, reflections,
  lamp tiles, headlight tiles, previews, OSD and other. The bgfx profiler
  supplies per-view GPU times; it is on while the debug menu is open or
  `--perf-log` is active (`LibBgfxBackend::flush`; it used to switch only
  while the menu was drawn). `render_debug.hpp:render_gpu_pass` maps a view to
  its pass and `render_debug_view_name` now names the opaque world views.
- Profiler cost (same session): render thread 2.08 → 2.11 ms by day, frame
  4.37 → 4.33 ms; within the noise.
- GPU times swing between two levels (about 3.8 and 5.8 ms for the same
  build) when another 3D application runs. Measure with nothing else
  rendering.
- Day replay, 2560x1440, saved settings (MSAA 8x, AO method 1 High, shadow
  quality High, smoothing 2), RTX 4080, all cores
  (`assets/validation/perf-baseline/issue362-gpu-passes/`):

| Pass | Day | Night |
| --- | --- | --- |
| World | 1.80 ms | 2.17 ms |
| Map shadows (sun cascades) | 0.76 ms | — |
| AO prepass | 0.20 ms | 0.17 ms |
| AO (geometry, AO, two filters) | 0.67 ms | 0.63 ms |
| Reflections | 0.12 ms | 0.12 ms |
| Lamp tiles | — | 0.14 ms |
| Total GPU | 3.89 ms | 3.30 ms |

- The GPU is below the CPU frame on this machine (day 3.9 ms against a
  4.7 ms frame), so it does not limit the frame. Against #358's baseline
  (another session) the day GPU frame fell from 4.19 to 3.6–3.9 ms, mostly
  from the far-cascade alternation (#361).
- Single-setting variants by day (GPU total, world / AO with its prepass /
  shadows):
  saved 3.81 (1.79 / 0.91 / 0.79), MSAA 4x 3.69 (1.75 / 0.91 / 0.77),
  AO off 2.77 (1.64 / 0 / 0.84), smoothing 0 3.71 (1.70 / 0.87 / 0.83),
  reflections off 3.66 (1.76 / 0.92 / 0.81).
- Findings:
  - MSAA 8x costs about the same as 4x at 1440p on the RTX 4080
    (0.04 ms of world time); keep 8x as the saved default here.
  - AO is the largest optional pass: 0.87–1.0 ms with the prepass, a quarter
    of the GPU frame. Its screen-space passes (0.67 ms) are the candidate for
    a GPU optimisation, as the open note above says (pass 1 reads the
    full-resolution D32F prepass).
  - The Very soft kernel costs about 0.1 ms; reflections 0.12 ms. Neither is
    an outlier.
  - Weaker GPUs are not measured: the same columns and variants should be
    run there before changing any default.

## AO pass cost (#362)

- Per AO view by day (replay, saved settings with AO on: HBAO High, 3 m,
  2560x1440 so the AO targets are 1280x720, RTX 4080, idle GPU otherwise):
  depth prepass (140) 0.20 ms, geometry (141) 0.02 ms, AO (142) 0.57 ms,
  filters (143, 144) 0.06 and 0.02 ms. The AO pass is the cost.
- Diagnostics on that pass (one build each, same session):

| Change (not kept) | AO pass |
| --- | --- |
| none (HBAO High: 10 directions × 6 steps) | 0.574 ms |
| `sampleDelta` without `aoFullCentre` (per-sample math) | 0.616 ms |
| 3 steps instead of 6 | 0.533 ms |
| 5 directions instead of 10 | 0.461 ms |
| return after reading depth and normal | 0.010 ms |
| far samples from 1/2, 1/4, 1/8 linear-depth mips (Scalable AO rule) | 0.716 ms |
| the same, level chosen per step, not per jittered texel | 0.683 ms |

- Reading: the cost is the sampling loop, but it does not scale with the
  number of samples (half the samples saves 7–20 %), with the per-sample
  arithmetic, or with cache misses on far samples: depth mips made it slower,
  even with a level that neighbouring texels share. No change that keeps
  High's image made the pass cheaper, so nothing was kept. The real lever is
  the sample count, which the Medium (6 × 4) and Low (4 × 3) qualities already
  offer as user choices.
- What is left to try, with a GPU profiler (Nsight) to see occupancy and
  stalls first: register pressure of the unrolled loops, interleaved
  (deinterleaved, HBAO+-style) sampling, which changes the sample positions
  and so the image slightly, and a compute version of the pass.

## Sun map props reused across frames (#359, fourth increment)

- On 4 E-cores by day, `sun_collect` (the sun's outer-frustum collection)
  was 1.2 ms of the main thread: about 0.4 ms for the instances and 0.65 to
  1.3 ms for 15,000 to 21,000 map-prop placements in batches.
- `LibBgfxBackend::sun_prop_batches` collects those batches over the outer
  box widened by 40 m and reuses them while the camera stays within 20 m of
  where they were collected, the held sun direction (#371), the world and the
  shadow origin are unchanged, for at most 60 frames (prop LOD follows the
  main camera). Map props do not move, and every cascade still culls them
  against its own frustum, so the draws are those of a fresh collection. The
  instances (dynamic objects included) are still collected every frame.
  Renderer debug option: "Reuse the sun's map props across frames".
- Harness (`scene_shadow_gpu`, references regenerated from `main`): city
  captures byte-identical, threaded and one-thread images identical.
- Day replay, same session
  (`assets/validation/perf-baseline/issue359-sun-cache/`); draws unchanged
  (2,242 by day, all cores):

| Run | Frame | p95 | `sun_collect` | Shadows (host) |
| --- | --- | --- | --- | --- |
| day, all, saved | 4.67 → 3.96 ms | 5.39 → 4.83 ms | 0.71 → 0.27 ms | 0.82 → 0.37 ms |
| day, e4, saved | 7.47 → 6.30 ms | 8.86 → 7.20 ms | 1.24 → 0.40 ms | 1.42 → 0.56 ms |
| day, all, low | 4.21 → 3.79 ms | 4.80 → 4.55 ms | — | 0.77 → 0.37 ms |
| day, e4, low | 6.94 → 5.91 ms | 7.83 → 7.08 ms | — | 1.36 → 0.54 ms |

  Worker submission rose by 0.05 to 0.16 ms: each cascade culls the slightly
  larger cached set. A first measurement with another game running on the
  GPU read all-core frames as slower; it was discarded.
