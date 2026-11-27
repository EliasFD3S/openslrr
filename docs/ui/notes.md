# Interface, OSD, cameras and input: issue notes

Verified notes on OSD drawing, viewports and cameras, the garage paint
booth and input, moved from the knowledge base. Script-side section and
dialog behaviour is in [../scripting/runtime.md](../scripting/runtime.md).

## Three-decimal elapsed time (#131)

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

## Issues #127 / #101 / #77: OSD state and resource lifetime

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

## File texture replacement and hidden preview viewports

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
Java was modified. scripting/natives.md now records file-backed texture propagation,
viewport lifetime eligibility and remaining dynamic-texture-ID limits; its
stock registration totals are synchronized with the Python checker (339
bodies, 3 explicit no-ops, 34 missing, 376 total; 31 active missing plus 3
leftovers). ENGINE_CONTRACT.md states the generic material/camera rules.

## Garage paint booth (#64)

Body paint, spray and decals were inert: the three `Painter` natives were
unbound, and no world-pointer event ever reached `Painter.handleEvent`, because
only the Mechanic received part clicks.

Events. `MouseCursor.addHandler` registers `EVENT_CURSOR` on the cursor
GameRef. `src/boot/mainloop.cpp:dispatch_world_pointer` now emits `EC_HOVER`,
`EC_LEAVE`, `EC_LDOWN`, `EC_LUP` and `EC_LCLICK` from that GameRef whenever
the Mechanic thread is not running (see scripting/runtime.md). The
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

## Per-axis gamepad fallback (#96)

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

## Rebind listener on a sim rig (four peripherals)

Report: on a rig with four peripherals (wheel base, pedals, shifter, button
box) one control could not be assigned; the button stayed on "PRESS A KEY...".

Cause, from the code (not reproduced on that rig): both Options screens
(`OsdxOptions.captureTick`, `OptionsDialog.animate`) first waited until
`Input.activeAxis` returned -1 on every device, then took the first device in
list order with an active channel. A channel that never rests blocked the
first step forever: an H-shifter in gear, a latched toggle, a rotary encoder
position held as a button, or an axis whose SDL initial value differs from
its rest. Even past that step, a lower device id with any active channel won
over the one the player touched. The old screen also scanned only
`controlSet.nDevices`, the devices present when the set was made. Separately,
a wheel with an SDL gamepad mapping could become the primary pad, whose raw
channels the listener never reads, so its extra buttons and pedals could not
be bound.

Fix: `InputCapture` (`src/platform/host_input_capture.hpp`) watches every
channel of every device, snapshots the rest once nothing moves, ignores what
stays held, takes the farthest move and waits for its release; each screen
shows the steps in a dialog (`OsdxOptions.buildCapture`). Only an SDL device
of gamepad type can be the primary pad. `InputDevices.applyCapture` derives the
range from the rest and the direction instead of the old "above 0.95"
guess. Device classes (`InputKind`: keyboard, mouse, pad, wheel) are exposed to
the host filters and to Java; `drive_steer_kind` reports the steering class.

Validation: Release build; `sdl_input` 2/2 (adds `capture_on_a_rig`: a stuck
shifter, an end-stop and a reversed pedal, a centred wheel, a toggle, a noisy
axis, a late device, the cancel click; and a gamepad-mapped wheel that must
not take the pad alias); `pad_fallback` passes. Both patched screens compiled
with `compile_game_sources` (the compile root first rebuilt HEAD's three
classes byte for byte). Not run in game, not checked on a real rig.

Not done: per-class filter settings (separate dead zone and gamma for pad and
wheel); today the Options sliders still apply to every class.

## Car controls per device class (#405)

Request: the input classes split by #393 (keyboard, pad, wheel) should each
have their own car controls in the menu, and the force feedback settings
should go further.

Change: the patched `ControlSet` has 118 controls; `carSlot(class, column,
c)` gives each class a primary and a secondary slot per car control (stock
0-9 and 15-24 stay the keyboard and mouse ones). In OSD v2, CAR CONTROLS has
a tab per class (`OsdxOptions.buildCarKeys`, one sub-page each, a `TabBar`)
and opens on the wheel tab when a wheel is connected, else the pad tab. The
listener takes only the tab's class (`InputDevices.captureBeginKinds`), so a
wheel at rest cannot steal a keyboard binding. On opening, `sortCarControls`
moves a binding that sits in another class's slot (an older file, or the
classic screen) to a free slot of its class. The steering halves pair inside
any column (`ControlSet.carControl`). The force feedback settings moved from
the RESPONSE column to their own sub-page (see the vehicles notes, #405).
Game and suspension controls are unchanged.

Validation: the io and game classes compiled with `compile_game_sources` (the
stock `ControlSet.java` first rebuilt the MWM class with only 1-ULP float
constant differences); `sdl_input` 2/2. Not run in game.

Limitations: the classic Options screen still shows only slots 0-9 and
15-24, so pad and wheel bindings sorted into their slots are not shown there
(they still work). The stock game reads the file but drops the pad and wheel
slots. Gamma and dead zone stay shared by every class.

## Live input, analog handbrake, transmission and clutch, framerate option

Requests: make the handbrake an axis; show the transmission and the clutch
as two choices; remove FRAMERATE (useless since the 1000 Hz physics); a
player saw no LIVE INPUT meters with his device until he rebound a key, while
a pad worked and driving was fine.

LIVE INPUT cause: the meters read `Input.getInput`, the player's controller.
`Dialog.show` resets that controller to the menu group alone
(`ControlSet.MENUSET`), and the car controls are group 1 (`DRIVERSET`),
activated only by the garage and the track. So in Options no keyboard or
wheel car binding existed on the controller; only the pad's built-in
fallback (`input_mapping::pad_fallback`: steer, throttle, brake, handbrake)
moved the meters. A rebind calls `setcontrol(cs)`, which adds every group,
hence "fixed after a remap". Fix: `OsdxOptions.buildControls` calls
`setcontrol` with the loaded set, as a rebind does; `OptionsDialog`'s
`Dialog.hide` restores the previous groups.

Handbrake: `HostInput::drive_handbrake` is a 0..1 float (was `axis > .5`);
`drive.handbrake` is `0.3 x pull` (a key still gives the old 0.3), the slide
correction stops past 5 % pull, the HUD lamp lights past 5 %. A lever bound
through CAR CONTROLS already maps as an analog half axis
(`InputDevices.applyCapture`). LIVE INPUT shows it; RESPONSE has a HANDBRAKE
DEAD ZONE (no gamma: the handbrake has no smoothing entry, and the stock
`vasp` order is fixed). The dead zone handler wrote only the keyboard slots
(`c`, `c + 15`), so since #405 pad and wheel bindings ignored every dead zone
slider; it now writes all six slots of the control (`ControlSet.carSlot`),
and the steering one both halves.

Transmission: still one `Config.player_transmission` (0 manual, 1 auto,
2 manual + clutch pedal, 5 semi-auto; `read_options` accepts only these), so
the GAME page shows TRANSMISSION (AUTO, SEMI-AUTO, MANUAL) and CLUTCH (AUTO,
MANUAL) and `OsdxOptions.setTransmission` combines them. A manual clutch
exists only with the manual gearbox: choosing it moves the gearbox to MANUAL,
another gearbox shows the clutch as AUTO and remembers the choice for the
session.

Framerate: removed from both Options screens; the host always blends car
poses (see [physics-tick.md §2](../vehicles/physics-tick.md)).

Validation: Release build; `options_retry`, `pad_fallback`, `menu_debounce`
pass. `OptionsDialog` and `OsdxOptions` compiled with `compile_game_sources`
in a copy of the game root (HEAD's sources first rebuilt both shipped classes
byte for byte). Not run in game; the LIVE INPUT fix is not checked with a
wheel or keyboard yet.

Limitations: while Options are open every group is on the controller, as
after any rebind (look keys included). The classic OptionsDialog screen keeps
its combined TRANSMISSION list. No handbrake gamma.

## Pointer gestures and race cameras: review notes on PR #308 (#295-#299)

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

## Change-view key (#470)

- In the patched MWM `Track`, V cycles follow → chase → cockpit cameras →
  follow, skipping the mirror and look-back cameras. A car without cockpit
  cameras returns to follow from chase. While LOOK BACK is held in the
  cockpit, CHANGE VIEW does not advance the index that release restores.
- Options → Controls → Game controls exposes CHANGE VIEW (slot 35). On load,
  only the stock keyboard F1/F2 pair in slots 35/36 migrates to V. If slot 35
  was rebound or cleared, it stays that way, but stock F2 in the hidden slot
  36 is still cleared: both slots drive the same virtual axis. A later F1
  binding with slot 36 empty is preserved. The hidden stock F5 look-back
  binding in slot 37 is also cleared; the visible LOOK BACK defaults to B.

Validation (Windows, Release): in Valo City with the Baiern and a legacy
profile edited to V/F2, F2 left the camera in follow and V selected chase
then cockpit. Holding B, pressing V and releasing B restored the steering-wheel
view; the next V displayed “Camera: cockpit 2”. The Game controls page showed
CHANGE VIEW bound to V; interactive rebinding and save/reload were not tested.

The NFSU2 camera moves to a mod (#486, #487, #488).
