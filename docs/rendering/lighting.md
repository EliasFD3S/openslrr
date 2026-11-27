# Rendering notes: lights, night and daylight

Night overlays, script and scenery light sources, car lights, street lamps
and sky/daylight. Contract: [contract.md](contract.md).

## Native building night overlays (#133)

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
(see [materials.md "Emissive overlays sit flush"](materials.md#emissive-overlays-sit-flush-281)). Onset remains 18:30. Signs only emit through their authored masks as well.

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

## Stock light resources and placement (#235)

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

## Script light sources and the g1 garage (#235, phase 1)

- API: `patch/system/Scripts/render/src/LightSource.java` with `PointLight`,
  `SpotLight` (`setCone`) and `RectLight` (`setSize`). Package `java.render`,
  new classes, no stock equivalent. Contract in
  [contract.md "Script light sources"](contract.md#script-light-sources-235).
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
  section (`flush,save,change`), see [saves/notes.md "Car Lot save ordering (#315)"](../saves/notes.md#car-lot-save-ordering-315). With a local
  `build/garage-59.class` (`git cat-file blob 6171aa2:patch/sl/Scripts/game/Garage.class`) the test also compares all 43 `CMD_*` commands: none
  differs from the #59 class in whether it saves.
  Validated in game by the user in the g1 garage at `df7117f` (20 November
  2026): the nine script fittings light the garage as before. g1, g2 and g3
  validated in game by the user at `2f904ac` (20 November 2026): glowing tubes
  and intensity 30 / range 26 m accepted (24 was too dim, 48 and 36 too bright).
- Phase 2: street lamps and scenery lights, see "Scenery lights and street
  lamps (#237)" below. Still open: per-light spot shadows and inner cone.

## Scenery lights and street lamps (#237)

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

## Street lamps lit while the sun is up (#271)

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

## Baked vertex light on ground POLY under the dynamic sun (#275)

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

## Car lights: headlight beams and brake lights (#279)

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
  intensity 3.5 × part power, range min(authored, 60 m), attenuation
  1/(1 + (d/12 m)²) and a low-beam cutoff (see "Headlight beam shape" below),
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

## Headlight beam shape: trees lit like day at night

- Symptom (Elias, forest road at 04:55): trees and bushes up to 40–50 m
  ahead glow saturated green while the road stays dark; leaves look
  speckled. Diagnosed from the shader, not a material.
- Cause: the #279 headlights used the authored linear attenuation
  `1 .03 0` with intensity 3 and a round cone. The road is hit at a grazing
  angle (`n·L` ≈ 0.09 at 8 m), a trunk or a leaf facing the car at ≈ 1, so
  everything vertical got about ten times the road's light: 1.9× the
  texture per lamp at 20 m, still 1.4× at 40 m. Leaves with scattered normals
  then go from black to saturated pixel by pixel.
- Stock data: `racers/textures/light_beams/attV.ptx` (256², JPEG at offset
  0x24) is the vertical profile the original projected. Row means, centre
  row 128: the band below the centre peaks (rows 150–190), rows above fall
  to 15–40 % of it and fade to 0 at the top. Its angular mapping to the
  authored phi is not established, so only that shape is used.
- Fix: `update_car_lights` gives headlights the attenuation
  `{1, 0, 1/12²}` (`kHeadlightFalloff`) and intensity 3.5, which keeps the
  road 8 m ahead at its #279 level (0.21 per lamp) and gives a surface
  facing the car 0.94 at 20 m and 0.29 at 40 m. `LampLight::above_axis_cut`
  (headlights 0.75, `kHeadlightAboveAxis` = 0.25 kept) goes to light-data
  row 2 column 128+i; `shaders/scene_shadow.sh:lampCone` removes that share
  between +1.1° and +3.4° above the beam axis (sine difference 0.02–0.06).
  Street lamps write 0 there and are unchanged.
- Test: `scene_shadow_gpu` case `car_lights`: the test beams point 5° down,
  so the road 24 m ahead is above their axis; its check is now "lit, and
  less than a third of the 10 m patch's gain". Measured: 10 m ahead
  9510 → 9627 with two headlights (unchanged level), one 5520 → 5583, 24 m
  ahead 3600 → 1785; brake light and headlight shadow cases unchanged; the
  whole suite passes.
- Roadside bushes (Elias, in game after the above): trees were fixed, but
  grass and bush cards below the beam axis still glowed. Their normals face
  a side of the card, so a headlight at 0.7 m lit them at `n·L` ≈ 1 while
  the ground behind them got ≈ 0.03. `shaders/fs_mesh.sc` now passes the up
  normal to `lampLighting` for cutout surfaces (`u_alphaTest.x` set): lamps
  light grass, bushes and leaves as the ground they stand on. Sun, ambient
  and the other light paths keep the authored normal. Side effect: canopies
  above a headlight get nothing from it, and a cutout fence is lit like the
  road; street lamps above still light foliage from above.
- Not done: no texture lookup of `attV`/`attH` (a smooth analytic cutoff);
  bushes not yet re-checked in game.

## Headlight shadows (#283)

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
- Smoke shadow (#454): view 139, after the car depth map. It is a 1024 px
  RGBA16F transmittance map (RGB multiplied, alpha the MIN light depth of
  the smoke's front), fitted every frame to the smoke from the held sun
  direction (`u_smokeShadowMtx`). `bind_scene_shadow` binds it at stage 14
  for `fs_mesh.sc` and `fs_vehicle.sc` (`smokeShadowTransmit`). See
  [vehicles/driving-model.md](../vehicles/driving-model.md#tyre-smoke-ground-dust-and-skid-marks-454).
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

## Issue #141: separated clouds and stronger daylight

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

## ROC garage: tent canvas and sunlight (#330)

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
  before the edit; see scripting/compiler.md "Shipped patch classes (#235)".
