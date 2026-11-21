# Rendering overhaul (#201)

The active tracker is https://github.com/EliasFD3S/openslrr/issues/201.
This document records delivered increments separately from planned systems.
HDR/exposure/tonemapping and image effects are tracked in #193.

## First increment: coherent shadow reception

Previously vehicles received sunlight without sampling the map shadow, while
world reflection-capture views explicitly skipped shadow reception. Scenery and
vehicles now share one visibility function and CPU binding contract. Captured
world meshes and prop batches receive the same field as the main camera.
The existing garage shadow field identifies its local source; its occlusion
attenuates that lamp, not unrelated sunlight. Ambient/environment illumination
is preserved. Preview and emissive vehicle draws opt out; vehicle receivers do
not sample the separate car-depth map containing themselves.

The GPU regression uses a controlled depth field to isolate receiver behavior.
It verifies actual production shader output, including view 72 used for
reflection captures, not only program linkage. The city smoke exercises the
normal game path but is not a complete before/after visual or FPS comparison.

## Second increment: radial cascades and continuous sun updates

Outdoor shadows now use four camera-centred radii: 32, 100, 320 and 1,000 m.
A 4096-square D32F atlas contains four 2048-square tiles (64 MiB depth payload,
compared with 16 MiB for the prior outdoor map). The last 15% of each inner
radius crossfades to the next, and the outer shadow fades over 900-1,000 m.
Receiver filtering samples the selected level and its neighbour; bias is
expressed as 2 cm along the light depth plus the receiver-plane correction.

The light-space basis is independent of camera orientation. Its X/Y translation
snaps to tile texels, and the sun projection is refreshed every frame, replacing
the old eight-metre/angular cache gates. This removes those particular temporal
thresholds; complete motion/shimmer validation is still required in game.
These are radial distance cascades, not tightly fitted camera-frustum splits.

Shadow frusta collect/upload off-camera scenery casters. Their mesh cache entries
survive into the following frame to avoid upload churn and are released after
leaving shadow/main-view usage (with one frame of retirement overlap). Vehicle
casters still use the separate nearby vehicle-depth path; this increment does
not add distant cars or skinned pedestrians to the scenery atlas. Garage local
shadows retain their single map. The later scene-intent increment removes the
stock-ground format requirement for scripted exterior scenes. A 1 km receiver
range does not guarantee geometry streaming at that distance.

GPU tests cover distinct atlas tiles, radial selection at 10/70/220/700 m,
blend values, final fade and sub-texel camera stability/small sun changes.
A city smoke and captures exercise production submission. Four refreshed passes
increase CPU submission/collection work; see the validation evidence for measured
cost. They must not be described as a free quality/performance improvement.

## Third increment: CPU work and texture filtering

The cascade implementation now collects outer candidates once, skips unused
character palette completion and filters render-space bounds for each level.
Cached depth groups merge contiguous opaque index ranges across material
changes; cutout settings and transparent holes retain their boundaries. None of
these optimizations lowers the four cascade radii or their update frequency.

The comparison footprint is now a symmetric nine-tap tent to reduce thin-caster
aliasing. GPU tests include a one-texel stripe shifted across the footprint;
this does not establish that every reported temporal shimmer is resolved.

Anisotropic world-texture filtering is exposed in the patched video menu and
debug overlay, off/hardware maximum, default on, persisted through the existing
host options file. It is separate from shadow comparison filtering. See
HOST_NATIVE_UTILITIES.md for index 6 and assets/validation/shadow-performance-filtering.txt
for tests and measurements. Previous timing numbers remain historical evidence,
not the current performance claim.

## Stationary moving-sun edge follow-up

The user confirmed that stopping the clock stops the remaining shimmer. A
stationary far-origin numeric sequence reproduced almost a one-texel grid jump
from tiny sun changes. A nearby floating origin reduces that discontinuity;
caster model transforms and receiver coordinates now use the same origin. A
separate depth vertex shader avoids combined-MVP cancellation of large world
positions. Camera travel can rebase the origin on a common multiple of all
cascade texel sizes, preserving grid phase.

Comparison sampling also replaces raw interpolated-depth thresholding for the
nearby vehicle shadow. Exact unchanged-state atlas reuse avoids redundant draws
when matrices, origin and caster records agree, while any small sun/caster change
invalidates it. Real-map and pixel/projection regressions cover these contracts.
The user confirmed the fix on the reported stationary view. This is not a
complete moving-camera/asset acceptance. See
assets/validation/shadow-origin-stability.txt for evidence.

## Scene intent instead of stock terrain detection

The host forwards the existing Scene.java `internalScene` field to the scene
layer. Exterior sunlight, procedural sky and cascades no longer require a
registered stock ground-prop package. The shadow pass reads the root-scoped scene
light directly, so an ordinary SCX-only addon or a dealer is eligible even when
no ground is visible in the main view. Hidden/inventory/unrelated light roots
remain excluded by the existing light traversal.

An interior remains interior even when its script creates a skydome. Car previews
opt out of outdoor lighting. Callers with no section metadata retain the prior
ground-prop detection fallback; resources/scene transitions reset that metadata.
No Java patch or resource-format change is required. This increment does not
replace the garage's approximate neon emitter or add real interior/headlight
emitters. Actual dealer/addon assets still need visual acceptance.

## Dealer and garage follow-up after the scene-intent retest

Real CarMarket inherits an unassigned `internalScene` int, absent from the sparse
host field map. It does create `sun` through Scene.addSceneElements. The host now
recognizes that live sun parented to the selected world when explicit interior
metadata is absent; explicit `internalScene=1` still wins. This closes the gap
missed by the earlier fixture which explicitly assigned zero.

Garage.java deliberately disables the sun and instances a directional neon.
The garage map was active, but its visibility only attenuated a small added
point-light approximation; the fallback floor lighting remained unshadowed.
Garage world meshes now use their authored directional light, and its map
attenuates that direct source. The old duplicate point contribution is removed
from garage world/vehicle draws. Non-terrain furniture also casts; the room shell
stays a receiver so its ceiling does not blanket the room.

This remains one approximate 10 m orthographic bay map anchored above the
origin, not a complete real interior-emitter system. Multiple fixtures, complete
room geometry occlusion and generic interior metadata remain outstanding.

Validation now uses an isolated physical directory tree containing all resource
packs. The older junction-based benchmark root skipped nested packs during
open_all; its city runs were useful smoke checks but did not validate garage or
dealer light creation. Record this limitation when interpreting old evidence.

## Addon cascade coverage: decoded ground bounds

Drag Track's assembled POLY mesh had 71,008 vertices but an all-zero bounding
box. The cascade culler treated the entire map as a point at its origin, so
camera travel could remove it from a shadow level. This looked like a short
shadow draw distance despite the 1 km outer radius.

Compute the ground render bounds from its decoded metre-space positions once
at loading time. This fixes the shared POLY path without changing cascade range,
resolution or culling policy. A real Drag Track regression checks the bounds
against every vertex and inclusion in all four volumes at the reported map's
start region. The addon case is skipped when those optional assets are absent.
In-game acceptance at the user's reported views remains separate from those
asset/GPU checks. Large monolithic addon meshes can now correctly participate
in more passes; this is a correctness fix, not a measured performance gain.

## Real garage caster/receiver follow-up

A subsequent user retest still appeared to show only baked shadows. The previous
pixel tests proved visibility reception but not the complete real room. The bay
pass excluded every ground-POLY instance (including built-in interior geometry)
and used a fixed 20 m footprint, while the first stock room spans about 37 m.
Its legacy distance/UV fade also relit edges inside that footprint.

Fit the orthographic footprint to transformed room bounds plus a margin. Include
room POLY geometry: the existing near plane rejects ceiling geometry above the
emitter without removing legitimate room casters. Disable receiver distance/UV
fading for this fitted interior field (zero distance), keeping projection/tap
bounds. Directional neon ambient remains authored; this does not implement
multiple physical interior fixtures or replace baked detail in source textures.

New regression uses actual garage shell, furniture and floor triangles. With
textures held fixed, it compares shadow-enabled/disabled floor pixels and then
moves the furniture mesh, proving dynamic coverage. A wider real-script on/off
capture confirms floor shadows beneath furniture. Temporary camera/capture
instrumentation is removed from the production executable.

## First real positional source: interior point light

The user validated floor shadows but reported missing wall shadows, then requested
starting real light sources. A production vehicle-depth test under a deliberately
chosen sun angle shadows a vertical wall (4,456 pixels). This rejects a general
floor-only receiver restriction; it does not certify the user's exact outdoor
parking/sun configuration. The legacy garage neon pointed straight down, leaving
vertical walls on ambient fill with no direct source for their shadows to occlude.

`PointLight` now provides a common renderer record: owner, render-space position,
colour, intensity, range, ambient and a shadow flag. The first adapter converts
the existing garage neon into one punctual source. It retains its owner/colour,
uses a local 3.3 m anchor, intensity 18, range 32 m and 15% of the authored ambient.
This is a deliberate compatibility policy; actual ceiling fixture placement and
native/script authoring for new point/spot lights are not implemented yet.

Six 512-square perspective faces share a 1536x1024 D32F atlas (6 MiB versus the
previous interior map's 16 MiB). Light-relative caster transforms, dominant-face
comparison and four filtered taps support walls/floors/ceilings. View 64 is
reserved/reordered before world views so the sixth face is available in-frame.
The source has inverse-square falloff and a smooth radial cutoff. Its direct
light and vehicle highlights share visibility with captured scenery. The old
fixed/directional contribution is suppressed on the migrated interior path;
ambient is explicit and source-texture baked detail remains.

Cache reuse needs unchanged projection/caster state. Moving a light/caster or
changing geometry/range updates depth. Colour/intensity changes only update
shading. Scope/lifetime, option toggles and invalid-light values are tested.
Same-frame GPU tests cover every face, lateral blocker removal, a reflection
view, colour/intensity/range and exact depth reuse. Real garage geometry tests
continue to move furniture over actual floor materials. Game captures with a
source moved 3 m show the lighting/shadow/highlight response; the first source
leaves distant parts of the room darker. More lamps and visual tuning remain.

## Interior directional daylight and alpha windows

A separate directional sun now coexists with the first interior point source.
It follows the continuous clock, adds direct diffuse/specular light and keeps
ambient separate from the sky. A 1024-square depth field covers the room and is
shared by scenery, vehicles and captured scenery. The legacy `ablak` window
section supplies actual triangles/UVs and its alpha mask; frames/opaque shell,
roof and interior objects remain occluders. Coplanar shell masking is limited to
8 cm, one texture and 32 window triangles. This is alpha coverage, not physical
glass transmission or automatic support for arbitrary addon window materials.

GPU tests using the real garage show sunlight changes when only the clock
changes; replacing window alpha with an opaque mask blocks the corresponding
light. Flare ghosts/rays use camera-to-sun visibility from the same field; tests
cover visible glass versus blocked roof/frame. See
[interior-directional-sun.txt](../assets/validation/interior-directional-sun.txt).
Stock g1 fixture placement and explicit white tube emission are implemented below.

## Stock garage ceiling fixtures

The g1 garage's nine paired fittings now drive nine neutral-white broad downward
sources. Positions are recovered from connected tube geometry, transformed with
the instance, rather than a central guessed anchor. Only the white tube faces
become self-illuminated; the existing texture and UVs stay intact. A bounded
12-tile, 12 MiB atlas uses one projection per fitting, cached while geometry and
sources are unchanged. Direct light/specular and captured scenery share the
same fixture visibility. Daylight remains a separate source/field.

Real-asset GPU tests cover all nine sources individually, occlusion, moved source,
exact depth reuse, reflection-view equality, rooted lifetime/option gating and
white tube pixels with lighting disabled. An isolated game capture was inspected:
white paired tubes align with the fittings and light reaches the surrounding
walls/floor. See [neon-fixture-placement.txt](../assets/validation/neon-fixture-placement.txt).
A follow-up corrects grazing-angle fixture shadow acne: four texel-aligned
comparisons use the receiver's projected plane depth rather than one shared
reference for a hardware PCF footprint. A vehicle-panel regression goes from
9,724 affected pixels to zero; actual object occlusion remains covered. See
[fixture-shadow-receiver-plane.txt](../assets/validation/fixture-shadow-receiver-plane.txt).
Other garage models still need explicit emitter metadata/adapters; this does not
claim a generic SCX authoring format, calibrated photometry or area-light softness.

## Interior seams, wall reception and probe occupants

Interior sunlight now requires projected alpha-window coverage as well as depth
visibility. The aperture field stores window depth/transmission, rejecting
windows behind a receiver and preventing shell seams from admitting sunlight.
The sun also uses aligned receiver-plane comparisons with a small residual bias.
Physical world positions are separate from the legacy normal-offset shadow
coordinates. Isolated captures confirm the highlighted corner/roof lines disappear
with direct window light and neon lighting retained.

Environment captures exclude vehicle-owned/attached subtrees, including seated
occupants, while independent pedestrians remain eligible. The main-view occupant
is preserved. A dedicated fixture-light test projects a vehicle shadow onto a
vertical wall and removes it by moving the blocker away. With high ceiling lamps,
a car farther from a wall may physically cast only onto the floor; no global
wall decal is substituted. See
[interior-seams-occupants.txt](../assets/validation/interior-seams-occupants.txt).

## Ambient occlusion and quality controls (#193 / #201)

Main-view HBAO and GTAO are selectable in Video Options and debug Renderer,
with low/medium/high sampling tiers, strength and radius. A camera-matched
single-sample depth prepass drives stable spatial horizon sampling and bilateral
filtering/upsampling; AO attenuates indirect diffuse only. Off is the default.
This increment does not implement HDR, temporal AO, bent normals or the remaining
post-processing backlog. Reflections/previews/UI do not reuse the main AO field.

Shadow quality selects depth resolutions; sharp/soft/very-soft edge filters are
independent. Resizing invalidates cached depths; changing filtering reuses them.
The patched Java source and rebuilt TUFA container expose and persist all controls.
GPU tests verify flat planes, contact occlusion, method differences, all tiers,
indirect-only application, resize/off cleanup, changed shadow pixels and cache reuse.
Isolated garage captures and the Options layout were inspected; a representative
hardware/FPS comparison and the reported deformed-body artifacts still need broader
in-game validation. See [ao-shadow-options.txt](../assets/validation/ao-shadow-options.txt).

## Still outstanding

- Matched baseline scenes and diagnostic views for lighting, normals, shadow
  coverage and cubemap faces; dealer, garage and addon scene validation.
- Broader cascade motion/quality validation: alpha foliage, moving casters,
  sunset/low angles, split transitions and bias on actual assets; reduce CPU
  collection/submission cost without reintroducing visible temporal jumps.
- Generic point/spot authoring and real headlight/street lights, including
  controls, damage and budgets; the stock g1 fixture adapter and bounded
  multi-emitter submission are implemented above. Other garage assets need adapters.
- Linear-light PBR material representation, legacy SCX adaptation, clearcoat,
  roughness filtering and a specified optional material-format extension.
- Sky geometry independence, continuous Scene.java inputs and cloud quality.
- Complete cubemap/scene-light consistency and capture update performance;
  the first increment fixes receiver shading but does not certify every face,
  probe, material or interior. Reflection resolution and far clip are unchanged.
- Retire legacy simulated beams/shadows only as their replacements work.

Scene.java was inspected: it selects discrete sky/light/fog/environment presets
and skips exterior sunlight setup for internalScene. No Java workaround is
needed for the receiver defects fixed here; the OptionsDialog patch ships the new graphics controls.

## Validation contract

Use copies of game data/saves for diagnostic runs. Report build, pixel tests,
manual visuals and timing separately. Do not close #201 on the strength of one
increment or turn an unvalidated screenshot into a compatibility claim.


## Continuous shadow edges

Fixture/interior sunlight Sharp retains anti-aliasing instead of exposing one
nearest depth texel. Very Soft now follows the fractional texel position with
a 4x4 tent, retaining receiver-plane depth correction and atlas isolation.
The regression previously had only two coverage values and an RGB jump of 78
when moving an edge in 1/32-texel increments; the corrected Sharp/Soft/Very Soft
paths have 11/14/7 values and maximum jumps 6/3/3 respectively. Contact shadows,
vertical receivers, AO isolation and cached depths remain covered. This changes
comparison filtering, not emitter positions or simulated volumetric beams.


## Outdoor banding and distant AO receivers

The reported large road stripes reproduce with AO disabled and disappear with
map shadows disabled. Outdoor PCF had one depth reference across a wide
footprint and a slope estimate with an absolute determinant threshold; this
created self-shadow moire on roads. Aligned, receiver-plane comparisons now
cover the full filter footprint, with continuous fractional weights and atlas
isolation. Only the blend region samples an adjacent cascade. Shadow coverage
and distance settings are unchanged. A production GPU test shades an inclined
receiver that is its own only caster: the old Soft filter darkens RGB sum
534 to 384; all corrected filters retain 534.

A separate AO test extends a plane to 1.5 km with a 5 cm camera near plane.
GTAO previously falsely darkened 1,396 of 6,640 visible pixels. Reversed-Z
camera depth and tangent-hemisphere rejection reduce that to 10 pixels in high
quality; medium/low have zero. HBAO has 37/6,640 at high and zero at lower tiers.
Contact occlusion and instanced props remain covered. These are numerical
regressions, not a claim of full GI or temporally accumulated AO. See
[ao-road-banding/README.md](../assets/validation/ao-road-banding/README.md).


## Pedestrian sun shadows (#230)

Skinned characters were excluded from every shadow pass, so pedestrians had no
ground contact apart from AO. The outdoor cascades now draw visible characters
within 100 m with the existing skinned depth program, using the same origin
rebase as other casters. The atlas cache includes a bone-palette hash, so
animation redraws it. Off-screen characters, point-light and interior-daylight
fields, and the separate nearby car-depth map are unchanged. Characters now
also receive their own shadow; receiver-plane bias is expected to keep that
free of acne, which needs in-game confirmation.
