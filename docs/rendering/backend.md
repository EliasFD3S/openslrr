# Rendering notes: backend and GPU resources

Mesh ownership and skinning, shader varyings, removed passes and
fallbacks, review findings and the debug menu.

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

Track HUDs do not require a menu background: `src/resources/gameref.cpp:fullscreen_script_camera` suppresses the fallback veil while a world camera is active. See [scripting/runtime.md](../scripting/runtime.md#track-presentation-without-a-menu-plate).

Menu hotkey presses use a shared 250 ms cancel/menu gate and a separate select gate, without delaying release or driving: `src/platform/host_input.cpp:host_input_check_hotkeys`. Validation and scope are in [scripting/runtime.md](../scripting/runtime.md#menu-input-debounce).

UI rectangles handed to the OSD registry are excluded from 3D scene/preview collection, including cursor tooltip templates: `src/resources/scene_instances.cpp:collect_root`. See [scripting/runtime.md](../scripting/runtime.md#ui-rectangles-have-one-render-owner).

Catalog vehicle insets stay in the preview pass, preserving the page background and script camera pose; fullscreen showrooms retain main-world ownership. Cursor cameras are UI-only: `src/boot/mainloop.cpp:mainloop_sync_section_render`, `src/resources/gameref.cpp:fullscreen_script_camera`. See [scripting/runtime.md](../scripting/runtime.md#catalog-vehicle-preview-versus-fullscreen-showroom).

Tooltip layer 0 must not promote catalog page backgrounds above preview cameras. Background ordering uses owned nonmodal page plates rather than the lowest arbitrary widget/text layer: `src/ui/osd_registry.cpp:osd_page_background_layer`.

Trigger indicators are visual RenderRefs, while trigger volumes stay sensors; explicit physics and ground collision remain authoritative: `src/resources/scene_instances.cpp:scene_instances_prepare_physics`. Cue rendering and reconstructed motion defaults are documented in [scripting/runtime.md](../scripting/runtime.md#trigger-cues-and-physical-bodies-130).

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

## Rendering foundation review findings (#233)

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

## Renderer debug menu (#243)

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

## Mesh skinning and deformed texture handoff (#79, #99)

SCX v4 skin arrays remain empty when backfilling zero preceding vertices.
Previously their emptiness also prevented appending the first skinned vertex,
so the next vertex backfilled zeros for it. Append influences when the current
vertex is skinned OR earlier vertices established skin arrays. Unskinned-only
meshes still leave both arrays empty; mixed meshes retain parallel arrays.

The deformed-mesh cache now transfers texture handles and shared texture paths
under the same guarded condition: a previous mesh entry exists and its texture
list and SCX path match. Previously the missing braces moved shared paths
unconditionally, including dereferencing an end iterator when the previous
entry was missing. Incompatible meshes keep their own texture load path.
This is a correction to existing loader/cache logic, not a new mechanic or a
claim of parity with a separate engine.

### Verification

Release x64 build passes. Four generated SCX fixtures run through the real
loader: skinned-first, unskinned then skinned, unskinned-only, and skinned then
unskinned. Before (main 1de56f0): cases 0 and 3 fail; after: all four pass.
The fixture files are temporary and no game assets are modified.

```powershell
./scripts/run_save_io_tests.ps1 -Suite scx_skin
./scripts/run_save_io_tests.ps1 -Suite scx_skin -BaselineRef 1de56f0
python scripts/check_knowledge_refs.py
```

[Before](../../assets/validation/mesh-skin/before.txt) /
[after](../../assets/validation/mesh-skin/after.txt). Knowledge references: 219
checked, zero errors. The texture guard is checked by source review and full
renderer compilation, not by a GPU integration test. In-game validation and
screenshots remain pending: inspect articulated parts and replace/damage a
body panel, confirming texture stability and absence of stray vertices.
