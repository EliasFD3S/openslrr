# OSD v2: a host that runs any Java OSD

Status: the first stage is in place. The CRC 2005 OSD runs as
`java.render.osdx` and drives the new full-screen Options screen. The stock
`java.render.Osd` screens still use the legacy path (see "Migration").

## Why

The legacy host does not run the Java OSD; it rebuilds it in C++ from class
names and field writes:

- `src/ui/osd_registry.cpp:osd_on_field_write` watches writes to `phy`, `cmd`,
  `handler`, `rect`, `hr`, `tooltip`, `inputString`, `disabled`, `vp`, `ins`.
- `src/ui/osd_script.cpp` drags sliders in C++ (only `java.render.osd.Slider`)
  and fakes focus by writing `pointedGadget`, `controlMode` and
  `lastUserActivityTimeStamp`; `src/boot/boot.cpp` clicks only
  `java.render.osd.MultiChoice` through Java.
- Rectangles become "widgets" whose role (plate, icon, knob, darken) is guessed
  from the texture ID (0x0018, 0x0091, 0x012B, 0x013C, 0x00A2).

A new gadget, or another OSD library, got none of it. Yet the stock
`java.render.Group` and the CRC `Group` both already implement the engine's
own protocol: `handleEvent(GameRef, int, String)` with `EC_HOVER`,
`EC_LEAVE`, `EC_LDOWN`, `EC_LUP` and `EC_LCLICK` for the hit boxes they
parent. The legacy C++ duplicates what that Java does.

## Contract

The host provides primitives and cursor events; Java owns behaviour.

1. **Virtual dispatch.** OSD builders (`createRectangle`, `createMenu`…),
   `Menu.addItem` and hotkey callbacks run on the receiver's class. The
   stock-only receiver repairs still apply when the receiver is unknown, but
   they no longer force `java.render.Osd` / `java.render.osd.Menu` onto an
   object of another package (`src/jvm/tree_eval.cpp`,
   `src/jvm/jvm.cpp:invoke_by_name`, `src/platform/host_input.cpp:host_input_check_hotkeys`).
2. **Attach.** `java.render.osdx.OsdHost.attach(osd)` (native,
   `src/natives/natives_osd.cpp:register_osd_natives`) marks an Osd as v2
   (`src/ui/osd_registry.cpp:osd_cursor_attach`). `osdx.Osd` calls it in its
   constructor. The widgets of an attached Osd are `plain`.
3. **Primitives draw as written.** A plain rectangle draws its texture at its
   matrix, back to front by its z (`Osd` priority × `LAYER_FACTOR`), with its
   tint (`OsdHost.tint(RenderRef, argb)`, `src/render/bgfx_osd.cpp:draw_osd`).
   The empty texture (`frontend:0x00AF`) with a tint is a solid colour quad. No
   role is guessed from IDs, and sizes are taken as written: the legacy
   "wide thin mesh is the dialog shield" stretch in
   `src/natives/natives_boot.cpp:osd_mesh_halves` skips attached Osds.
4. **Groups.** Any GameRef whose `osd` field is set to an Osd gets a host group
   (`osd_on_field_write`), and `WORLDTREELEAF` hides or shows it
   (`osd_on_group_leaf`), whatever its class.
5. **Cursor events.** For a plain hit box (a `PhysicsRef.createBox` under a
   group) the host sends the stock event to the box's parent GameRef when its
   event mask has `EVENT_CURSOR`: `handleEvent(phy, EVENT_CURSOR, param)` with
   `"15 x y id 1"` (hover), `"16 x y id"` (leave), `"1 x y id"` (left down),
   `"2 x y id"` (left up) and `"5 0 id"` (click, after an up on the box that
   took the down). `src/ui/osd_registry.cpp:osd_cursor_move` and
   `osd_cursor_button`, called first by the window procedure
   (`src/render/bgfx_lib_backend.cpp`); when they take the input the legacy
   hover, click and slider code does not run. Focus, press, click, slider drag
   (`Osd.animate` reads `Input.cursor`) are the Java code of the OSD.
6. **Keys.** Hotkeys keep the native path (`createHotkey` →
   `handleEvent(Hotkey)` on the owner's class).
7. **Classpath.** Every folder under `system/Scripts` is its `java.*` package
   (`src/jvm/jvm.cpp:bootstrap_classpath`): `system/Scripts/render/osdx` holds
   `java.render.osdx`.

## The osdx package (`patch/system/Scripts/render/osdx`)

The CRC 2005 OSD (`Osd`, `Group`, `Gadget`, `Menu`, `Style`, `Button`,
`TextButton`, `MenuButton`, `HelpMenuButton`, `MultiChoice`,
`MultiChoiceArrow(_Header)`, `MultiChoiceNumeric(Arrow)`,
`MultiChoiceScroller`, `Slider`, `ProgressBar`, `Ledz`, `StartLamp`,
`FlashText`, `StringInput(_Header)`, `TextBox`, `ScrollGroup(2)`, `Event`,
`EventReceiver`, `Label`) and its dialogs (`osdx/dialog`), with these
changes for SLRR:

- packages renamed `java.render.osd(.dialog)` → `java.render.osdx(.dialog)`;
  the dialogs import `java.render.osdx.Osd` by name.
- CRC-only engine API replaced: `Frontend.greyFont` → `smallFont`,
  `GfxEngine.videoMode` / `Config.videoMode` → `Config.video_x/y`, the CRC
  `Camera` and `GfxEngine.RF_*` flags → the SLRR `Camera` and
  `Viewport.RENDERFLAG_*`; `FullscreenFade`, `Hotkey.negCommand`,
  `Hotkey.useNegative` and `Hotkey.setPriority` dropped (SLRR's `Hotkey` has
  none of them; a chained `setPriority` returned nothing and lost the
  gadget's hotkey references).
- frontend IDs remapped to `frontend.rpk` (CRC → SLRR): slider knob 0x62 →
  0x91, arrow up 0x20 → 0x4C, highlighted arrows 0xA5/0xA4 → 0x4C/0x84,
  tooltip 0x5D → 0xCB, none 0x14 → 0x18, white alpha 0x55 → darken 0xA2,
  highlight 0x01 → 0x12B, dialog buttons 0x120/0x116 → 0xD8 (highlight 0x12B),
  headers 0x116 → 0x28, loading 0x8B → 0x0E. CRC textures with no SLRR
  counterpart (LEDs 0x4C/0x4D/0xF5, start lamp 0x110/0x111) use 0x12B / 0xA2.
- `Osd.animate()` (no argument) is added: the SLRR animate hook passes no
  frame time, and the CRC `animate(dt)` holds slider dragging and the gadget
  label slide-in. `Gadget` records the label's end position without the CRC
  `renderinst.id()` check (the host keeps text without that instance, so every
  label slid to the screen centre). `Gadget.restartAnimate` and
  `Osd.animateGroupIn` replay the slide-in when a page is shown again.
- `Rectangle` and `RectangleTemplate` subclasses add the CRC colour API
  (`setColor`, coloured templates) on top of `OsdHost.tint`.

OpenSLRR additions, all plain Java on the contract above:

| Class | What it is |
| --- | --- |
| `OsdHost` | The natives: `attach`, `tint`, `fade` (alpha factor), `resize` (half width/height without a new RenderRef) |
| `Animated` | Interface for objects the Osd moves every frame (`Osd.addAnimated`, `tick(dt)` from `Osd.animate`) |
| `OsdAnimator` | UI animations: slide in from left/right/top/bottom, fade, ease-out cubic, per-element delay; `slideGroup` moves a whole page (texts, rectangles, gadget plates, slider knobs) in a top-to-bottom cascade. Visuals only: hit boxes stay put |
| `LevelBar` | Smooth level bar: one fill rectangle resized in place and eased every frame, a bright cap, optional centred mode |
| `TabBar` | A row of tab buttons (command `baseCmd + i`) with an accent bar under the active one |
| `Toggle` | An OFF/ON `MultiChoice` with a green/grey status lamp; `Menu.addToggle` |
| `SectionHeader` | Accent title with a thin rule |
| `InfoPanel` | Framed help panel; it is the Osd's tooltip box, so focus fills it with `Gadget.tooltip` |
| `Toast` | Timed notification (a `GameType` timer), built once and hidden by colour |
| `HintBar` | Key legend ("ESC BACK") |
| `AxisMeter` | Segmented level bar for a live value (pedal, steering; centred mode), tinted rather than rebuilt so it can update every frame |

## The Options screen (`patch/sl/Scripts/game/src/OsdxOptions.java`)

`OptionsDialog.osdCommand(CMD_OPTIONS)` opens it, so OPTIONS in the main menu
(`MainMenuDialog`) and in the in-game menu (`IngameMenu`) both reach it. Five
tabs: Video (resolution, display mode, VSync, preset, anti-aliasing,
anisotropic, reflections, clouds, sun flare), Graphics (AO, shadows, lights,
prop detail, fog, car lights, street lamps, view distance, lamp shadows),
Sound (volumes, mixing), Controls, Devices, Game (transmission, difficulty,
assists, units, GPS, traffic, pedestrians, head movement, frame-rate mode).

**Controls** (#345): live `LevelBar`s for throttle, brake, steering and clutch
(`Input.getInput` every frame), mouse sensitivity, the response of the loaded
control set (gamma and dead zone per axis, force feedback and emulated force
feedback strength) and four sub-pages:

- CAR / GAME / SUSPENSION CONTROLS: every binding as a button; a click
  captures the next key, axis or button (the classic `OptionsDialog.animate`
  remapper: wait for release, take the first active input, direction from
  the axis value, a shared axis split in halves, ESC clears). While it
  captures, `OsdxOptions.capturingInput` makes the host hold back hotkeys
  (`src/platform/host_input.cpp:captures_binding`), so ESC does not close the
  screen.
- CONTROL FILES: the control sets in `save/controls` (`ControlSet.fileCheck`),
  LOAD, SAVE over the selected one, SAVE AS NEW (`CONTROLS n`). No text entry.
- RESET TO DEFAULTS refreshes the bindings and the sliders.

The control set is loaded when the screen opens and saved when it closes.
Pages slide in from the side of the clicked tab (`OsdAnimator`), sub-pages from
the right, the frame fades in when the screen opens, the toast drops from the
top.

**Devices** (#345): up to four connected controllers from SDL3
(`java.io.InputDevices`, `patch/system/Scripts/io`): name, type (wheel, PS5
pad...), axes/buttons/hats, power and battery, rumble support, a live activity
meter, and three buttons: RUMBLE (0.6 strength, 0.6 s), CALIBRATE / DONE and
RESET. Calibration records each axis's raw minimum and maximum while active
and then maps that range onto -1..1 for that device
(`src/platform/host_input_devices.cpp:calibrated`); it is saved per device
name in `save/game/openslrr_axis_calibration`. The list follows plugging and
unplugging live; REFRESH asks SDL again at once (`InputDevices.refresh`).

Each control carries the `OptionsDialog` command of its setting; a change
copies the value into `owner.osd.multiValue` / `sliderValue` and calls
`owner.osdCommand`, so the existing code applies and saves it. A preset
re-reads the renderer options afterwards. Key redefinition, axes/force
feedback and the control-file requesters stay on the classic pages: those
buttons close the screen and open the classic page.

## Script pitfalls found on the way (compiler/VM, not fixed here)

- `new T(..)` as a bare statement (compiler warning `W_UNVERIFIED: new T(..)
  as a statement`) makes the VM store the next assignment elsewhere: the
  field written next stayed null. Assign the object instead.
- `cond ? a : b` passed as a method argument always produced `b`. Use
  if/else.
- Building a colour with `>>`, `&` and `<<` in a script gave a transparent
  result (the fades left every plate invisible). `OsdHost.fade` scales the
  alpha on the host instead.
- An interface with methods (`Animated`) is `W_UNVERIFIED` for the compiler;
  calls through it work in game.
- `compile_game_sources` resolves another class from the first class file of
  that name in its index (directory order), so a backup folder such as
  `_openslrr_backup_235/` in the game root shadows the current
  `OptionsDialog.class`. Compile in a root without stale copies.

## Migration

1. Done: osdx uses v2; stock screens are unchanged.
2. Attach the stock `java.render.Osd` screen by screen (main menu, options
   dialogs, garage, catalog, dealers, paint booth, race setup) with a short
   in-game check each. The stock `Group.handleEvent` is already the right
   receiver.
3. Delete the legacy pieces once no screen uses them: the `osd_on_field_write`
   gadget hooks, `osd_script.cpp`, the ID-based roles, the builder
   redirection.

## Validation (stage 1)

Release host builds; the 49 osdx classes and the game classes compile. In
game, on a copy of the game root, a temporary probe (removed) opened Options
through `MainMenuDialog.osdCommand(CMD_OPTIONS)`, clicked each tab, applied
the LOW preset (values and toast updated), dragged the EFFECTS slider
(20 → 72), saw the rows slide in after a tab change, and pressed BACK (back on
the stock main menu). The screen has no picture of its own: prime.avi (or the
garage) shows through a full-screen veil. Not checked in game: keyboard
navigation, APPLY resolution, the hand-off to the key pages, the in-game menu.
The Controls and Devices tabs were seen in game: the smooth bars, the car key
page with the real bindings, a capture cleared with ESC (the screen stayed
open), SAVE AS NEW listing `CONTROLS 1`, a page sliding in, and an Xbox 360
pad listed after REFRESH (type, 6 axes, 11 buttons, 1 hat, charging 100 %,
rumble). Not checked: capturing a real key or axis, LOAD, rumble and
calibration on that pad.
`run_save_io_tests.ps1 -Suite ui_lifetime` crashes (0xC0000409) and
`-Suite tree_numbers` fails `neq expected=0 actual=1` with and without these
changes.
