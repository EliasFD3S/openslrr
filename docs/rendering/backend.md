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

## bgfx backend selection and the Vulkan spike (#186)

A spike, not a port: Direct3D 11 stays the default and the only backend
validated in game. Results: #478. The numbers below come from one laptop
(i9-13905H, RTX 4060 Laptop GPU, Windows 11, 1600x900), not from the #358
machine.

What the build and the host do now:

- `CMakeLists.txt` compiles every shader for `s_5_0`, `s_6_0`, `430` and
  `spirv`; `src/render/bgfx_lib_backend.cpp` embeds DXBC for Direct3D 11,
  DXIL for Direct3D 12 and SPIR-V for Vulkan (bgfx's `BGFX_EMBEDDED_SHADER`
  maps Direct3D 12 to DXIL, not DXBC). All 14 shaders compile for the two new
  profiles with `WERROR` and no source change. The executable grows by
  0.49 MB (166 KB of DXIL, 324 KB of SPIR-V). Compiling the 14 shaders one
  after another takes 18.8 s for `s_5_0`, 1.8 s for `430`, 2.5 s for
  `s_6_0` and 2.1 s for `spirv`: the two new profiles add 22 %
  (`shader-build-time.txt`).
- `--renderer auto|d3d11|d3d12|vulkan` (`bgfx_request_renderer`) sets
  `bgfx::Init::type`; `auto` leaves the choice to bgfx, which takes
  Direct3D 11. `SLRR_RENDERER` does the same for the GPU test executables.
  When bgfx starts another backend than the one asked for, `open` fails: a
  run must not measure a backend it did not ask for.
- `SLRR_GPU_VENDOR=nvidia|amd|intel` sets `bgfx::Init::vendorId`. With no
  vendor, DXGI takes adapter 0 and bgfx's Vulkan backend takes the first
  discrete GPU, so on a machine with two GPUs the backends can differ. The
  `[bgfx] open` line prints the adapter (`10de:28e0` in every run here).
- `render_programs_gpu` and `scene_shadow_gpu` pick the embedded shader of
  the running backend instead of requiring Direct3D 11.
  `SLRR_GPU_KEEP_GOING=1` makes `scene_shadow_gpu` run every case and list
  the failing ones.
- `scripts/perf_baseline.ps1 -Renderers d3d11,d3d12,vulkan` adds the backends
  to the benchmark matrix; `-Shots` saves a capture every 10 s of each run.

GPU suites (`assets/validation/vulkan-spike-186/gpu-tests.txt`):

| Suite | Direct3D 11 | Direct3D 12 | Vulkan |
| --- | --- | --- | --- |
| `render_programs_gpu` | pass | pass | pass |
| `osd_text_gpu` | pass | pass, capture identical | pass, capture identical |
| `scene_mesh_cache` | pass | pass | pass |
| `scene_shadow_gpu` | pass (28 cases) | pass (28 cases) | 27 of 28: `ambient_occlusion` fails |

- Vulkan AO: an open wall gets occlusion it does not get on Direct3D
  ("frontal near", GTAO: 75 pixels, 0 on Direct3D; "frontal far on floor",
  HBAO: 566 pixels, 0 on Direct3D, which fails `differ<=visible/50`).
  Reading the AO targets back (`ao-attribution.txt`): the linear depth and
  the normals of the AO geometry pass are identical on Vulkan and
  Direct3D 12 (0 of 4,800 texels), the capture without AO is identical, and
  the AO value differs after the AO passes, on one texel row next to a
  vertical depth step or the top border (255 on Direct3D, 232 to 245 on
  Vulkan). Direct3D 11 and Direct3D 12 are identical at every stage. The
  Intel Iris Xe gives the same Vulkan result as the RTX 4060, so it is not
  one driver, and shaderc's `-O 0` gives the same SPIR-V size and result.
  The difference is therefore in the SPIR-V build of the first AO pass
  (`shaders/fs_ao.sc`: `hbao`, `gtao`, `sampleDelta`) with identical
  inputs. The cause inside that pass is not found.
- `ambient_occlusion` run alone fails on Direct3D 11 and 12 too (GTAO contact
  pixels 0, other counts than in the full run): the case depends on state
  left by the cases before it.
- City captures against the Direct3D 11 reference of the same suite: the
  640x360 and 2560x1440 night frames differ in at most 12 pixels, by 1/255,
  on Direct3D 12 and on Vulkan. The 1280x720 crowd frames differ in 182
  pixels (day) and 28 (night, worker encoders) on Direct3D 12, and on Vulkan
  in 197,311 of 921,600 (mean 1.11/255, largest 73) and 120,870 (mean 0.32,
  largest 19). The suite printed `max 0` for all of them until this change:
  `compare` and the variable it fills were arguments of one `printf`.
- Direct3D 12 and Vulkan agree with each other and not with Direct3D 11 on
  the garage sunlight counts (566 lit pixels against 545, 1,316 against
  1,264). Not classified.
- The per-view uniform rule of #246 holds on the three backends
  (`per_view_light_uniforms`: `per-draw uploads=4 per-view uploads=1`,
  images identical), and the caches of #262 cut world submission on each (640x360 city
  frame, off then on: 7.76 to 4.49 ms on Direct3D 11, 5.44 to 3.81 on
  Direct3D 12, 4.42 to 2.23 on Vulkan). That gain is in the encoder, before
  any backend. Render-thread times of this harness are not comparable between
  backends on this laptop (45 to 107 ms on Direct3D 11 with a hidden window,
  3 to 14 ms on the others).

In game (`assets/validation/vulkan-spike-186/`, first 45 s of the #358
replay, `--no-vsync`, MSAA 8x, AO method 1 High, shadows High, smoothing 2):

- The captures show the same scene on the three backends by day and by
  night. On the standing start, Direct3D 12 differs from Direct3D 11 by
  0.12/255 on average; Vulkan differs by 1.93/255 by day and 1.05/255 at
  night, along texture detail, foliage and geometry edges
  (`day-difference-d3d11-vulkan-x8.png`). Single-setting captures
  (`edge-classification.txt`): with anisotropic filtering off the
  difference falls from 1.94 to 0.55, with AO off to 1.69, with MSAA off it
  does not move, and with MSAA, anisotropic filtering, AO, reflections and
  clouds all off it is 0.006 (0.3 % of the pixels). Two runs of one backend
  differ by 0.11 on Direct3D 11 (clouds, reflection). So about 1.4 is
  anisotropic filtering, for which bgfx asks 16x on both backends, and about
  0.25 the AO difference above.
- Runtime checks on the three backends (`runtime-checks.txt`): the garage
  with the debug menu, the intro video and the main menu draw on Vulkan
  (`vulkan-garage-debug-menu.jpg`, `vulkan-intro-video.jpg`,
  `vulkan-main-menu.jpg`); a resize to 1100x700, a minimise and a restore
  during the replay leave every backend running, and the next captures have
  the new size (1084x661).
- Other builds and another game instance ran on the same machine during
  the measurements. A run counts as clean when no other `slrr_host` or compiler
  ran and the CPU was under 35 % before and after it: 16 of 30 runs. The
  laptop GPU's clock also moves by itself (210 to 2,505 MHz sampled between
  runs; a later 5 s check of the same scene ran at 31 to 39 ms of GPU time).
  Clean averages, ms:

| Run | Clean runs | Frame | Physics | Flush | Render thread | GPU | Draws |
| --- | --- | --- | --- | --- | --- | --- | --- |
| day Direct3D 11 | 3 | 19.62 | 9.62 | 8.67 | 6.80 | 8.92 | 4242 |
| day Direct3D 12 | 3 | 19.47 | 9.88 | 8.20 | 5.66 | 10.86 | 4240 |
| day Vulkan | 2 | 17.50 | 8.93 | 7.38 | 6.81 | 6.20 | 4239 |
| night Direct3D 11 | 3 | 18.73 | 10.43 | 6.98 | 6.01 | 8.28 | 3731 |
| night Direct3D 12 | 2 | 16.13 | 8.87 | 6.11 | 4.17 | 9.93 | 3737 |
| night Vulkan | 3 | 17.17 | 9.38 | 6.61 | 6.28 | 6.44 | 3736 |

- The same backend varies by up to 8.7 ms of frame time between clean runs
  (day Direct3D 12: 15.62 to 24.34 ms), more than the backends differ, so the
  frame column shows no difference. Every host time moves with the laptop's
  CPU state. Divided by `collect_ms` of the same run, which no backend
  touches, the render thread is 2.45 (day) and 2.27 (night) on Direct3D 11,
  2.73 and 2.54 on Vulkan (+12 %), 2.00 and 1.80 on Direct3D 12 (-18 %,
  -21 %); the runs of two backends do not overlap (day: 2.33 to 2.53, 2.72
  to 2.75, 1.89 to 2.12). Flush divided the same way is the same on the
  three (2.63 to 2.68 at night).
- The frame is bound by the host thread on each backend (physics wait about
  9 ms, flush 6 to 9 ms, GPU below the frame), so a faster or slower render
  thread does not change it here.
- GPU: the per-pass times add up to about the same on the three backends
  (quietest round, day: 6.16 ms on Direct3D 11, 5.75 on Direct3D 12, 6.10 on
  Vulkan; night: 6.84, 6.35, 6.02). The frame totals bgfx reports differ
  (day 7.08, 9.99, 6.44) only by the time outside any view (0.9, 4.2 and
  0.3 ms: MSAA resolve, present, or where each backend puts its frame
  queries), so they are not a like-for-like comparison and show no GPU gain.
- First frame of the drive: the host's `present` zone (its `bgfx::frame`
  call) takes 1 to 4 ms on Direct3D 11, 182 to 234 ms on Vulkan and 276 to
  621 ms on Direct3D 12, in every clean run. bgfx creates each pipeline on the render thread at its first
  draw and keeps nothing between runs unless the application supplies
  `bgfx::CallbackI::cacheRead`/`cacheWrite`, which the host does not
  (`renderer_vk.cpp:getPipeline`, `bgfx.cpp:CallbackStub`). After the
  two warm-up seconds the worst frame is similar (55 to 162 ms on
  Direct3D 11, 58 to 90 on Vulkan, 54 to 106 on Direct3D 12).

Read in the pinned bgfx (fe9bbdec6), not measured:

- Direct3D 12 and Vulkan copy a program's whole vertex and fragment uniform
  block to a mapped buffer for every draw whose program has a predefined
  uniform (`u_model`, `u_viewProj`; every vertex shader here has one),
  where Direct3D 11 calls `UpdateSubresource` per changed stage. The
  uniform skips of #246 and #262 save encoder work on every backend but not
  that copy; `u_skinBones[48]` is 3 KB per skinned draw.
- Vulkan keys descriptor sets and pipelines by a 32-bit hash without a
  collision check, and allows 65,536 descriptor sets per frame behind an
  assertion only.
- Without vsync Vulkan uses `VK_PRESENT_MODE_IMMEDIATE_KHR` and silently
  falls back to FIFO when the surface has none, which would cap a benchmark.
- With the bgfx profiler on (`--perf-log`, debug menu), Vulkan ends the
  render pass at every view; per-view GPU times include that.
- A lost device aborts on every backend (`BGFX_FATAL`), as today.
- Neither `homogeneousDepth` nor `originBottomLeft` is set on Direct3D 12
  or Vulkan (Vulkan flips the viewport), so the caps-driven projection and
  shadow matrices take the Direct3D 11 path unchanged.

Not done: the comparison against the #358 baseline needs its machine
(i7-13700KF, RTX 4080, 2560x1440) and a quiet session; the cause of the
Vulkan AO difference inside `fs_ao.sc`, the anisotropic filtering
difference and the garage sunlight counts are not explained; AMD GPUs,
borderless fullscreen, a video mode change from the options, career and
races were not exercised on Direct3D 12 or Vulkan, and the Intel GPU only
ran `render_programs_gpu` and the AO case;
`python scripts/check_knowledge_refs.py` was not run (no Python on the
machine).
