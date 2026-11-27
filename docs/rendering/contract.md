# Rendering contract details

Reflections, mesh ownership, shadows, sky and light sources, and the
debug overrides, moved from
[ENGINE_CONTRACT.md §12](../core/ENGINE_CONTRACT.md#12-enhanced-rendering),
which keeps the backend overview. Issue notes are grouped in
[lighting.md](lighting.md), [shadows-ao.md](shadows-ao.md),
[performance.md](performance.md), [materials.md](materials.md) and
[backend.md](backend.md); the overhaul plan in [roadmap.md](roadmap.md).

## Reflections

The branch has double-buffered dynamic cubemaps, a capture framebuffer, and
a probe state with an origin and box-projection bounds.
An interior reflection depends on the probe position and the parallax
correction. Changing only the texture size does not fix a reflection that
looks stretched in the garage.

Capture, writing the faces, publishing a ready cubemap, and sampling it have
distinct lifetimes. Check the six orientations, the frame, world transitions,
the uniforms, and probe validity. Do not read a texture at the same time it
is the destination of its own capture.

Environment capture excludes vehicle/Part-owned subtrees, including seated
characters. Filtering only `vehicle_material` lets a seated actor be captured
from the probe inside the excluded car and reflected onto its external panels.
Use the render owner and bounded GameRef ancestry; independent pedestrians remain
visible in reflections. This does not hide occupants in the main camera.

Every world camera is a reflection scene: the garage, the dealers and the
city all use the cubemap. The garage probe sits on the car body and its box
is the garage bounds. Without a followed car, a world camera captures from
the car nearest its target, so a showroom car reflects the room around it.
A capture must draw what the main view draws; the garage shell is preloaded
SCX geometry outside the instance list and is drawn explicitly.

A checked "dynamic reflections" option does not prove that the car material
reads that cubemap. Follow the binding into the shader. Check fallbacks to
the old envmaps and the `vehicle_material` flags.

Map shadow visibility is shared by scenery and vehicle shaders through
`shaders/scene_shadow.sh`. Bind it at mesh stage 3 or vehicle stage 5; stage 4
is the vehicle cubemap and must not be overwritten. Captured world meshes and
prop batches receive the same world-space shadow field as the main view.
The field identifies its source (directional or added local light), so only that light's
direct diffuse/specular contribution is occluded. Ambient/environment light
is preserved. Vehicle previews and emissive lamp surfaces disable reception;
the separate car-depth map is not sampled by the same car as a receiver.
Scene.java's `internalScene` is forwarded as scene environment metadata.
Exterior lighting and shadow eligibility must not depend on stock ground POLY
or ground-prop registrations, nor on a terrain mesh being visible. Select the
root-scoped light directly. An interior may have a skydome; its presence alone
must not enable the sun. Preview scenes opt out; callers without section metadata
retain automatic ground-prop detection. Resource reset clears the scene intent.
An inherited unassigned `internalScene` may be absent from the sparse host object;
in that case a live Scene.sun parented to the current world also establishes
exterior intent. Explicit interior metadata takes priority.
The stock club garages derive paired ceiling emitters from their actual SCX
tube geometry (g1 9, g2 10, g3 12). `legacy_neon.hpp` is an explicit asset
adapter. Candidate faces are the `texturak` section of `garazs1.dds` that samples
a known white swatch (g1), or groups with an authored SCX4 emissive colour and no
emission mask (the g2 `neon` and g3 `Material #11345` lamp groups). Only
connected tube-shaped components (2–4 m long, under 0.25 m wide and tall, above
3.3 m) are paired into fittings, which keeps emissive signs out; local anchors sit 8 cm below the tube's
lowest face and follow the instance's transform. The loader splits only tube
triangles into `self_illuminated` groups, preserving their original texture/UVs.
These opaque surfaces remain white without dynamic light or sky ambient; the
rest of the fitting/material stays normally lit. This is runtime material
metadata, not a new serialized SCX version or arbitrary bright-pixel detection.

The live rooted neon owner scopes up to twelve `SpotLight` records: neutral white,
intensity 24 (engine units, not calibrated lumens), range 20 m, a downward hood
with a 65-degree inner and 80-degree outer half-angle. Captured scenery and
vehicle diffuse/specular use the same inverse-square, radial/cone falloff and
per-source shadow visibility. These are broad punctual approximations to the
fittings, not area lights. Local/dynamic-light options and world transitions
clear/reselect the records. Garage fixtures are gated by Local lights (option 4)
only; Map sunlight (option 3) must not switch them off (#233). The 15% legacy ambient remains an explicit fill;
when fixtures are present the point compatibility record supplies ambient only
(zero direct intensity), suppressing the old directional neon contribution.

For unadapted interiors, the previous `PointLight` fallback remains: a local
3.3 m anchor through the owner transform, intensity 18 and range 32 m, preserving
its owner/colour. Missing fixtures must not invent nine lights in other garages.
These fallback settings are compatibility defaults, not recovered positions.

One point-shadow atlas contains six 512-square D32F faces in a 1536x1024 2D
texture (6 MiB depth payload). Face views 0/1/2/3/4/64 precede the world;
`draw_previews` must reserve/reorder view 64 before world 5/6, including inset
sorting. Do not collide with previews 8..63, OSD 70 or reflections 72+.
Perspectives cover 92 degrees for overlap, near .05 m and the light's range.
Caster transforms and face matrices share a light-relative origin. Cutout
materials retain their alpha rules. Skinned casters remain excluded.

`u_pointShadowFaces[6]` and `u_pointShadowParams` select the dominant cube face
in the existing scenery/vehicle depth sampler. Four comparison taps stay in
that face. The radial receiver bias is .015 m. Light radiance uses inverse-square
attenuation with a smooth range window and a bounded near-source denominator.
`u_sunDir.w=-1` selects explicit positional-light ambient instead of the legacy
fixed/directional light. Direct local diffuse/specular is occluded; ambient and
reflections remain separate. Captured scenery samples the same light/shadow field.
Exact unchanged caster/projection state reuses the atlas; light position/range or
caster identity/revision/buffer/transform changes invalidate it. Colour/intensity
updates change shading without redrawing depth.

The fixture path instead allocates a 2048x1536 D32F atlas (12 MiB), at most
twelve 512-square tiles, one downward perspective per fitting. Reserved views
128..139 are ordered before world 5/6 and the sun view 65; reset the full order
0..139, preserving preview/inset/OSD/reflection order. Main/captured mesh and
vehicle programs use fixture sampler 7. Unchanged projection/caster state reuses
all tiles; position, direction, range or caster changes refresh depth, while
colour/intensity updates only affect shading. Fixture PCF aligns four comparisons
with depth texel centres and evaluates each reference depth on the local receiver
plane (derivatives taken before bounds branches). This avoids grazing-angle
self-shadow stripes without a large constant bias, disabling vehicle occlusion,
or increasing atlas resolution/tap count. Bilinear weights reconstruct visibility;
the small residual depth bias is .000005. The fallback cube atlas is inactive
when the paired fixtures are active. Close destroys the extra atlas/uniforms.

Generic fixture authoring, adapters for other garages, outdoor punctual sources,
projector headlights, PBR, HDR/tonemapping, area lights and removal of baked
detail in diffuse textures are still separate work.

Interior daylight uses a separate 1024-square D32F directional shadow map
(4 MiB), on reserved view 65 ordered before world 5/6, alongside the point atlas.
Its direction/radiance use the continuous day cycle; sky ambient never substitutes
for direct sunlight. Scenery, vehicle diffuse/specular and reflection captures
sample this independent field (mesh stage 5, vehicle stage 6). The existing
point-light ambient/falloff is retained. No extra ambient is added by daylight.

The stock garage adapter treats the blended SCX section `ablak` as a sunlight
aperture: actual transformed triangles, authored base UVs and alpha mask. Up to
32 triangles sharing one mask are supported. The mask cuts opaque shell fragments
only within 8 cm of the window plane, and the window surfaces themselves cast
alpha-tested shadows (threshold .5). Walls elsewhere, roof, frames and ordinary
objects remain casters. Unsupported masks/triangles do not open extra shell.
This is a legacy asset adapter, not automatic transmission for all blended glass.
Texture alpha controls binary coverage, not physical coloured-glass transmission.

A separate 1024-square RG32F aperture field stores window light-space depth and
transmission, with its own D32F depth attachment (12 MiB together). View 66 is
ordered after the sun field and before world 5/6, including inset views. Mesh and
vehicle sampler 8 reads it: only projected alpha windows in front of a receiver
admit direct interior sunlight. This prevents thin wall/roof seams from becoming
unintended openings. Opaque-frame/object occlusion still comes from the sun depth
field. Four texel-aligned, receiver-plane comparisons use a .0000005 residual
sun depth bias; physical lighting/reflection coordinates must stay unbiased.
The old 5 cm normal offset is confined to the legacy shadow varying, not
`v_worldPos`, in mesh, instanced and vehicle shaders. Sky draws explicitly reset
material mode; aperture capture mode cannot leak into the visible sky.

An unchanged projection/mask handle/caster stamp reuses both daylight fields.
Clock, geometry and caster motion update it; night, missing apertures or disabled
map shadows/dynamic lighting disable interior daylight safely. Skinned casters
remain excluded. The source and sky share time, but are distinct contributions.
Interior flare artwork resolves through the active root's skydome resource; all
rays/ghosts use camera-to-sun shadow visibility, including window alpha and frames.
It draws as a lens overlay only when that visibility permits it. Preview/emission
passes disable daylight reception, and backend close destroys the extra resources.
Stock g1 fixture placement and white tube emission are implemented above;
other interiors retain the point fallback until they have fixture metadata.

Outdoor receivers now use four camera-centred distance cascades (32/100/320/
1,000 m) in a 4096-square depth atlas, with 2048-square tiles. Projection
translation is snapped in light space; every frame uses the current sun rather
than an angular cache threshold. Blend the last 15% of each inner range into
the next and fade the final level over 900-1,000 m. Views 0/2/3/4 and vehicle
depth view 1 precede world view 5; ordering must keep all shadow passes before world
receivers. The garage instead uses the six-face point atlas described above. These
are radial distance cascades, not camera-frustum-fitted splits. Scenery casters
outside the main view must be uploaded and retained for their shadow submissions.
Collect outer shadow candidates once and cull their reflected-Z render bounds
for each cascade. Skip character palette completion in that collection; do not
change default completion for ordinary/reflection views. Depth draws may merge
adjacent opaque material sections, but must preserve alpha-cutout and blended
index boundaries, rebuilding their metadata with every geometry revision.
Outdoor shadow comparisons align to depth texel centres and evaluate each
reference depth on the receiver plane. Derivatives are evaluated for every
cascade before distance selection. Sharp uses four continuous comparisons,
Soft a 4x4 cubic/tent filter, and Very Soft a broader 4x4 fractional tent. A constant
reference across a wide footprint produces moire/acne on sloped receivers;
inflating a single slope bias is not an adequate replacement. Only sample an
adjacent cascade in its blend interval; clamp every tap inside its own tile.

Sun rotation is evaluated around a nearby shared floating origin. Rebase
caster model translations and receiver positions together; depth vertices must
apply model and view-projection separately, avoiding a precombined large-world
MVP cancellation. A long-drive origin shift must preserve every cascade's grid
phase. Frustum culling remains in absolute render space.
An unchanged atlas may be reused only with identical matrices, origin and
caster identity/revision/buffer/transform records. Never skip a changed sun or
caster through a time/angular threshold. The nearby car-depth receiver uses a
comparison sampler so depth is compared before bilinear filtering, matching
PCF rather than interpolating depth and thresholding afterwards.

This is not yet a complete real-light implementation. See rendering/roadmap.md
for #201 progress, performance cost and unvalidated scene cases.

## Mesh CPU/GPU ownership

Scene geometry remains the authoritative CPU source for collision, picking,
animation and re-upload. The GPU cache shares that source and retains only draw
metadata, texture bindings and GPU handles. Never deep-copy vertex/index/skin
payloads just to keep a resident draw entry. Reordered material indices are
upload scratch and may be freed once bgfx has copied them. Animated revisions
must read the current source, rebuild metadata/index counts and recreate skin
buffers as needed; deformed replacements preserve compatible texture bindings.

## Shadows

Map shadows and car shadows have passes and resources that must be inspected
separately. Enhanced aims at dynamic shadows, not at stacking the old SLRR
projected shadow on top of a new shadow map.

Resolution alone does not explain the pixels: coverage in metres, projection,
depth, bias, filtering, leaf alpha, and the choice of casters also count.
A building missing from the casters stays without a shadow even with a larger
shadow map.

Every shadow pass depends on a linked shader program. bgfx links a program
only when the fragment shader's `$input` list equals the vertex shader's
`$output` list; otherwise it returns an invalid handle and, in Release, prints
nothing. The passes check the handle and switch themselves off. A varying added
to `vs_mesh` must therefore be added to every fragment shader paired with it.
Startup reports each failed program as `[bgfx] program <name> failed to link`.

Ambient occlusion uses a separate camera-matched reversed-Z D32F prepass in view
140, an AO-resolution geometry pass (linear depth and normal) in view 141,
then R8 horizon evaluation and separable depth-aware filtering in views
142–144 before the main world draw. It does not replace the swapchain or require
a colour post-process chain. Every AO quality runs at half resolution (#248); high
uses more directions/slices (HBAO 10×6, GTAO 5×8) instead of full resolution.
HBAO and GTAO are independent horizon/slice implementations, not vendor SDKs.
Reconstruct sample positions at the exact depth texel centres, including the
centre used by normal reconstruction; otherwise flat receivers develop false AO.
Choose normal derivatives with two-sample projected-depth extrapolation on each
side of the receiver, rejecting unsupported discontinuities. Recover the plane
normal from projected-depth slopes rather than subtracting reconstructed world
positions: the latter loses precision at grazing angles. The separable filter
weights distance to the receiver plane, so tilted surfaces are denoised without
accepting foreground depth discontinuities.
Finite-slice GTAO is normalised against its unobstructed hemisphere. Reject
coplanar/behind-tangent samples: quantised screen samples may leave the nominal
slice and must not occlude a flat road. Clear AO depth to zero and use GREATER
for individual, skinned and instanced depth draws. Use the actual reversed
projection coefficients when reconstructing positions; do not reinterpret the
main-view field as normal-Z. AO radius stays in world metres; a fixed pixel
cap must not shrink it when approaching a receiver.
Per-texel sampling variation is a 4x4 tile: each texel of a block has its own
direction rotation and radial step offset, and the two filter passes use
[1,2,2,2,1]/8 weights, which average exactly one tile period on a flat
receiver. Do not reintroduce fixed step distances or a non-periodic per-pixel
hash (such as interleaved gradient noise) without temporal accumulation: both
leave occluder silhouettes stamped on the ground as parallel streaks.
Only main-view opaque indirect diffuse is attenuated. Direct sun/lamps, emission,
transparent glass, preview/inset views, cubemap captures and UI opt out. Off or
zero strength destroys the AO targets; resizing/quality changes recreate them.
Do not sample an earlier camera's field in another view. Samplers 9/10 are AO/depth.
No temporal accumulation, depth pyramid, bent normals or HDR is implied.

Host shadow quality selects local projection tiles 256/512/1024, interior sun
512/1024/2048, and vehicle depth/exterior cascade tiles 1024/2048/2048.
Resize invalidates the corresponding cached depths. Medium preserves the former
resolution. Smoothing is independent: sharp, soft, very soft comparison filters;
it does not invalidate caster depths or change lighting direction/distance.
All comparison taps preserve receiver-plane correction where present. For fixture
and interior sunlight, Sharp uses a narrow continuous four-tap edge (never raw
nearest-texel visibility), Soft uses bilinear four-tap coverage, and Very Soft
uses a fractional 4x4 tent. Do not snap the wider filter to the nearest depth
texel: this creates square coverage jumps. Fixture taps stay inside their atlas tile.
Outdoor cascade receiver-plane gradients are clamped per cascade to a 20-unit
world slope converted to normalized depth (`extent/(extent+768)`, #233). A pixel
quad that straddles two surfaces (pole on ground, kerb, wall/floor crease) must
not extrapolate wide filter taps against distant depths. Fixture and interior-sun
gradients are not clamped yet.
Options
persist alongside host graphics; OptionsDialog native IDs 7–12 and debug UI agree.

The active world renders to the swap chain. No offscreen scene-copy or HiZ depth
pyramid is wired into the frame path; the unused implementations are removed.
The optional scene size filter only rejects small projected bounds and does not
read occlusion depth. Keep it disabled for inset, shadow and reflection views.
Program creation owns and releases its input shader references on every outcome;
rejected shader pairs must not leak handles during opening or reopening.

Do not collect casters using only the visible camera frustum.
The list must respect the useful light volume. When the time of day changes,
sun-dependent matrices and caches must be invalidated.

Skinned characters (pedestrians, seated drivers) cast into the outdoor sun
cascades only. They come from the main-view records, whose bone palettes are
already complete; the outer shadow collection keeps skipping palette
completion, so a character outside the camera frustum or beyond 100 m casts
nothing. Rest-pose bounds do not describe animated limbs: cull them by the
cascade radius, not by mesh bounds. Their caster stamp hashes the bone palette,
so a pose change redraws the atlas. Point-light and interior-daylight fields
still exclude skinned casters.

The renderer factory always selects libbgfx for Bgfx in the supported build.
NullBackend is retained for headless probes and tests; there is no separate
WGL fallback. Image and font decoders do not depend on an optional renderer
macro. The Windows x64/MSVC toolchain requirement remains in CMake.

## Sky and lights

The Enhanced sky must be driven by game time and by parameters consistent
with the sun, the clouds, and the reflection. A night-time appearance must
not leave a noon sun in the materials or in the shadow.
Animated cloud parameters need a stable time, distinct from a count of
elapsed frames.

OSD text uses a dedicated position/UV layout and unlit shader pair. A text
line submits one indexed batch per available transient-buffer chunk, preserving
font, colour, layer and draw order. Do not route glyphs through world-lighting
bindings or merge labels across the plates/icons that separate them. Long lines
must rebase their 16-bit indices while retaining pen advance. See
`src/render/osd_text_batch.hpp` and `LibBgfxBackend::draw_osd_line`.

Font loading prefers `frontend/fonts/<stem>.ttf`, then `.otf`, with the legacy
SCX/atlas loader as fallback. `scripts/bake_slrr_fonts.py` converts user-owned
stock assets to outline TTFs, keeping the byte map, authored advances and top
origin (zero hhea ascent). Ordinary replacement fonts use their hhea ascent as
the baseline offset. Both outline formats rasterize to padded SDF atlases through
the pinned dependency's stb_truetype; one shader composes fill, outline and
shadow without extra glyph passes. Font assets/cache lifetime remain shared and
weakly indexed as before. `slrr_text_set_effects` overrides a line's bounded
outline width, shadow offsets and ARGB colours; effects do not affect measure,
alignment or advance. They are not persistent options or Java APIs. Legacy
atlases retain their baked effects and bypass SDF composition. The byte-based
text contract is unchanged; this does not add Unicode shaping or kerning.
Only trusted local font files are supported: SFNT table bounds are checked, but
stb's parser does not provide comprehensive hostile-font validation.

OSD surfaces have their own blending contract. Do not apply world lighting
to them, or a global correction meant for the body.
For trees and glass, check alpha test, blending, depth write, pass order,
and mipmaps before modifying the original textures.

## Script light sources (#235)

Scripts create lights with `java.render.LightSource` and its subclasses
`PointLight`, `SpotLight` and `RectLight`. These classes are new: they are
OpenSLRR additions shipped in `patch/system/Scripts/render`, not stock API.
Car head- and taillights stay engine-driven and do not use this API. Since
#279 they light the scene from the car data: each part owns the lights its
cfg places as `render <type-13 light>` children, one headlight beam (first
`flags 4` light of type `spot`) and one brake light (first `flags 1` light)
at most (`src/resources/part_runtime.cpp:part_car_lights`). A light exists
only while its part is installed on a car whose engine runs and its part
condition (wear × tear) is above 0.25; headlights follow the city-lamp night
window and the brake light the brake pedal, like the part's lamp glass
(`part_lamp_lit`). The renderer draws them as spot lamps ahead of the street
lamps in the lamp array; since #283 up to six headlight beams (the followed
car's first) cast shadows from their own atlas.

| Native (owner) | Descriptor | Effect |
| --- | --- | --- |
| `LightSource.create` | `(Ljava.util.resource.ResourceRef;I)V` | Registers the receiver as kind 0 point, 1 spot or 2 rect, attached to the parent's canonical GameRef. A parent without GameRef state, or another kind, creates nothing and logs `[light]`. A second call restarts the light with defaults. |
| `LightSource.setPosition` | `(Ljava.lang.Vector3;)V` | Parent-local position, metres, script axes (as `RenderRef.setMatrix`). Each axis is clamped to ±1e5. |
| `LightSource.setDirection` | `(Ljava.lang.Vector3;)V` | Parent-local direction of travel, normalised. A zero vector is ignored. |
| `LightSource.setColor` | `(I)V` | `0xRRGGBB`; alpha ignored. |
| `LightSource.setIntensity` | `(F)V` | Engine units, clamped to 0–1000. |
| `LightSource.setRange` | `(F)V` | Metres, clamped to 0.2–500. |
| `LightSource.setShadows` | `(I)V` | Stored for every kind; only the point light honours it today. |
| `LightSource.setEnabled` | `(I)V` | Enables or disables the light. |
| `LightSource.destroy` | `()V` | Removes the light; later calls on the object are ignored. |
| `SpotLight.setCone` | `(FF)V` | Inner/outer half-angles in degrees; outer clamped to 1–89, inner to 0–outer. |
| `RectLight.setSize` | `(FF)V` | Width and height in metres, clamped to 0–100. Stored only. |

Every setter ignores non-finite or mistagged arguments and keeps the previous
value. Defaults: position 0, direction (0,-1,0), white, intensity 24, range
20 m, cone 65/80°, shadows and enabled on.

Lifetime. The registry (`src/resources/script_lights.cpp`) is keyed by the Java
object and is a GC root provider: a registered light and its parent stay alive
until `destroy`, removal of the parent GameRef (`gameref_remove`), or a world
reset (`gameref_clear`, or a new VM registering the natives). Lights are not
saved.

Rendering. `LibBgfxBackend::update_local_light` reads the registry every frame,
only in the garage with Local lights (option 4) on (scenery lights outside
garages are described under "Scenery lights (#237)"). The parent's drawn
script matrix M gives the render-space position S·M·p (S flips Z), the same
convention as the instance transform S·M·S applied to mesh vertices. Spot and
rect lights become fixture `SpotLight`s, at most 12, in creation order; the
outer angle is clamped to 65.5–89° because the shader's inner edge is fixed at
65°, and every fixture shares the fixture shadow atlas. A rect light is drawn as
a spot. The first enabled point light becomes the point record and keeps the
neon's ambient fill. Any registered light whose parent is in the drawn world,
enabled or not, disables the stock g1 anchor adapter (`legacy_neon.hpp`), so a
script can turn the garage dark. Without registry lights the adapter path is
unchanged. While script lights own the scene, including when all of them are
disabled, the neon point record stays ambient-only (intensity 0, no shadows).
The patched Garage creates one spot per twin-tube fitting in garages 1–3 (9, 10
and 12 lights, intensity 30, range 26 m, set per garage in its fixture tables);
the open-air ROC garage has none, and since #330 it has no neon either: it is an
exterior scene (`internalScene = 0`) lit by the sun. The stock `misc.garage:0x001D` neon remains an ambient-only record
whenever fixtures exist.

## Scenery lights (#237)

Map objects place stock light resources as `render <id> <pos> [<ypr>]`
children followed by `flags <group>`. A resource is a light when its RPK entry
has typeof 13 (`RESTYPE_RENDER_LIGHT`); never infer it from names or
keywords. `parse_light_resource` reads the type-13 text payload. Point and
spot light the scene; `fakepoint`/`fake point`/`fakespot` are glow only and
never add scene lighting.

Each ground shell owns the light children of its map's placements in its local
script space (`scenery_lights_attach`). They leave the registry with the shell
(`gameref_remove`) or on a world reset, have no Java object and are not saved.

Outside garages, with Local lights (option 4) on, lights of the city group
(bit 4) light the scene at night (`scene_sun_is_night`); other groups stay
off. The renderer keeps lamps whose capped light sphere meets the view
frustum, the nearest 64 within 500 m, and fades each to zero before the cut,
so no lamp appears or disappears with power. They have their own unshadowed
lamp array (64 entries, separate from the 12 garage fixtures) with the stock
D3D attenuation 1/(c + l·d + q·d²), range capped at 45 m and intensity 0.8 ×
diffuse, evaluated in the mesh and vehicle shaders. Scenery spots are drawn as
omni lights, and flares are not drawn. Garages keep the fixture path of #235
unchanged.

With shadows on, the 6 lamps nearest the followed car (else the camera),
whatever the camera direction, cast shadows: one downward
150-degree perspective tile each in the fixture atlas (free outside garages),
sized by Shadow quality and filtered like the fixtures, faded out at the tile
rim. Static scenery casts from a list collected when the lamp gets its slot
(not the ground POLY, not the lamp's own housing); vehicles and characters
cast every frame, and a tile is redrawn only when its casters change. A lamp
keeps its shadow slot until a clearly nearer lamp (75 % of its distance and
4 m nearer) has waited 1 s; shadow strength fades in and out over 0.8 s, so
turning the camera never moves a car's shadows and no shadow pops. In
addition, every lamp whose light reaches the player car (up to 16) casts the
car's shadow from a small car-only tile holding just the vehicles within 6 m
of it, so the car always has a shadow from each lamp that lights it; that
shadow fades in over 10 m where the lamp's light on the car is near zero. The intensity values above were accepted in game by the user.

## Renderer debug overrides (#243)

The OpenSLRR Debug menu (Ctrl+F12, Renderer tab) can switch renderer passes
off and change budgets for performance work. These overrides live in
`render_debug()` (`src/render/render_debug.hpp`), default to the normal
renderer, are never saved and reset on restart. The renderer must read them
where the pass is decided, so that an override removes the pass's view (no GPU
time) rather than drawing it empty. Options that already exist (Enhanced
graphics such as AO method, shadow quality, MSAA, clouds; the overlay's map
and vehicle shadows, OSD, flare and reflection faces) are edited in place: the
Enhanced ones are saved as before, the overrides only further disable them.
A new pass should get a toggle, a named view in `render_debug_view_name` and a
profiler zone when it has measurable CPU cost.
