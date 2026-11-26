# Rendering notes: materials and meshes

Alpha, colour mixing, culling, emissive overlays and lamp glass.

## Issue #129: foliage alpha-to-coverage erosion

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

## Two-sided cards under the sun (Track Wizard foliage)

Track Wizard addons (Côte d'Arbroz) build grass, bushes and trees from
crossed SCX4 cards in a 1-bit DXT1 atlas, flags 0x22000000 (alpha test and
two-sided), reference 128, no lightmap. Each card has one normal per quad,
bent about 45 degrees up toward the card's authored front. With the dynamic
sun, `fs_mesh` used `max(dot(n, sun), 0)`: a card whose front faced away from
a low sun got almost no direct light while the crossing card stayed lit, so
every clump showed dark, angular half-planes. The legacy fixed light already
used `abs(n.l)`; an `abs` alone does not help here because n.l is near zero,
not negative.

Authored two-sided SCX4 surfaces (flag 0x02000000 on map placements, single
and batched) now add 4 to `u_alphaTest.z`. When the sun is on the other side
of the card than the normal, `fs_mesh` mirrors the normal through the card's
geometric plane (screen-space derivatives of the world position) for the sun
term only. Single-sided surfaces, lightmapped maps, local lights, lamps and
fixtures are unchanged; sun shadows between crossing cards remain.

Validation: in game on a copy of `game/` with the addon (bench drive, 13:00,
MSAA on), the dark half-planes are lit; clumps in tree shadow stay dark.
A/B runs showed the darkness persisted without AO and without sun cascades,
and an unlit texture-only frame was uniform, which ruled out the atlas, AO
and shadows. Valo City and other two-sided cutouts were not rechecked.
Captures: `assets/validation/two-sided-foliage/`.

## Issues #132 / #116: vehicle material backface culling

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

## Emissive overlays sit flush (#281)

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

## Lamp glass snapped onto the lens (#284)

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
