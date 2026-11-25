# Rendering notes: shadows and ambient occlusion

Shadow cascades and receivers, pedestrian and lamp shadows, and AO.
Contract: [contract.md](contract.md#shadows).

## Shared shadow receivers (#201, first increment)

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
- See rendering/roadmap.md and assets/validation/scene-shadow-foundation.txt
  for the current scope, evidence and remaining #201 work.

## Distance cascades (#201, second increment)

- `src/render/shadow_cascades.hpp:shadow_projection` builds camera-centred
  radial projections with fixed light-space basis and texel-snapped X/Y.
- `src/render/bgfx_shadows.cpp:LibBgfxBackend::update_static_shadow` renders
  four outdoor atlas tiles each frame, keeping garage local shadows separate.
  Radial splits are 64/160/400/1,000 m since #371 (32/100/320/1,000 before);
  off-camera scenery participates.
- `shaders/scene_shadow.sh` selects adjacent levels and crossfades their
  visibility. See rendering/roadmap.md for memory/CPU cost, cache retirement,
  the separate short-range vehicle-caster path and scene-detection limits.
- `tests/test_scene_shadow_gpu.cpp` checks distinct atlas clears/selection,
  blends and the 1 km fade through actual shaders, plus projection stability.
- Far cascades alternate while the camera moves (#361, renderer debug option
  "Far cascades on alternate frames while moving",
  `render_debug().cascade_alternation`). When the projections changed this
  frame, cascade 2 (320 m) redraws on even frames and cascade 3 (1,000 m) on
  odd ones. The skipped tile keeps its depth and its
  `shadow_cascade_matrices_` entry, so receivers sample it with the matrix
  it was drawn with; the 10 % extent margin in `cascadeVisibility` covers
  one frame of camera motion. A still camera draws all four (the caster
  cache needs it), and `cascade_tile_valid_` drops every tile after an
  atlas re-creation, an origin rebase or a frame that did not reach the sun
  cascades (no sun, point light in the same atlas, shadows off). Consequence
  for later work: a far tile may hold casters one frame old.

## Sun steps, crossfade and cascade radii (#371)

- Flicker: the game clock moves the sun every frame (`GameLogic.time` x
  `timeFactor`). The cascade light basis then rotated a little every frame;
  texel snapping absorbs translation, not rotation, so caster edges flipped
  texels everywhere. With the clock speed at 0 the flicker stopped.
- `LibBgfxBackend::shadow_sun_direction` holds the shadow light direction
  until the sun has moved `render_debug().shadow_sun_step` degrees (0.05 by
  default, about every 3 s at `timeFactor` 4). The cascades and the car depth
  map use it; scene lighting keeps the live sun. A still camera can reuse
  the cached atlas again. Step 0 follows the sun every frame.
- Crossfade (`update_shadow_crossfade`): on a step, the atlas of the previous
  direction stays as drawn, with the matrices of its tiles
  (`shadow_cascade_prev_matrices_`); the two atlases swap and the new one is
  drawn in full. Receivers fade from the previous atlas to the new one over
  `render_debug().shadow_crossfade` seconds (0.75 by default): the cascade
  sampling functions live in `shaders/scene_shadow_cascade.sh`, included
  twice by `scene_shadow.sh`, the second time on `s_staticShadowPrev`
  (stage 13 in `fs_mesh` and `fs_vehicle`) with `u_shadowCascadesPrev`. Where
  the previous atlas does not cover a receiver (the camera moved), the new
  atlas alone is used. A crossfade starts only from a complete atlas of the
  previous frame and stops if the shadow origin rebases. It costs a second
  atlas of the same size (64 MB at High) and a second cascade lookup only
  during the fade; no extra shadow rendering.
- Radii: the first cascade reached 32 m, beyond which thin shadows faded
  out as one texel grew from 3.4 to 10.7 cm (reported as shadows appearing
  and disappearing while driving). The radii are now 64/160/400/1,000 m
  (texels at 2048: 6.9, 17, 43 and 107 cm); each divides the 8,000 m common
  step of `shadow_rebase_origin`.
- The GPU harness runs with the crossfade off (captures are compared at
  once); `real_world_cache` tests the held direction, the step, step 0 and
  the crossfade swap and fade.

## Cascade CPU cost, filtering and anisotropy (#201)

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
  (off/hardware maximum, default on), saved in openslrr_options.json and shared with
  the debug checkbox. World colour/detail samplers opt in, comparison maps and
  UI do not. Patched OptionsDialog source/container are shipped and installed.
- See assets/validation/shadow-performance-filtering.txt for measured changes;
  the previous cascade cost in scene-shadow-cascades.txt is historical.

## Stationary moving-sun shimmer follow-up (#201)

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

## Pedestrian sun shadows (#230)

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

## AO silhouette streaks around cars and pedestrians (#201)

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

## High AO at half resolution (#248)

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

## Street lamp shadow early-out and capped kernel (#250)

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

## AO geometry pass and depth fetches (#256)

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

## AO darkens large walls seen at a grazing angle (#274)

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
