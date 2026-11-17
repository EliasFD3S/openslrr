# OpenSLRR Enhanced

Copy the `sl` folder from this patch into the game root and accept the replacement of `OptionsDialog.class` and `Garage.class`. The OptionsDialog Java source is shipped with its compiled class. Use the Enhanced v3 Release.

The **Video Options** menu applies the resolution and the Enhanced settings in game. Exterior scenery range is 5 km. The scenery shadow map and the vehicle shadow map each use a resolution of 2048 × 2048. **F12 → Renderer** also sets how many reflection faces are computed per frame.

## Java natives

| Class and signature | Behavior |
| --- | --- |
| `java.game.OptionsDialog.getRendererOption(I)I` | Reads the active value for the requested identifier. An unknown identifier returns 0. |
| `java.game.OptionsDialog.setRendererOption(II)V` | Applies the value to the session. Unknown identifiers are ignored. |

| ID | Option | Values |
| --- | --- | --- |
| 0 | Anti-aliasing | 0 off, 1 MSAA 2×, 2 MSAA 4×, 3 MSAA 8×. Other values are clamped to this range. |
| 1 | Dynamic reflections | 0 off, any non-zero value on |
| 2 | Volumetric clouds | 0 off, any non-zero value on |
| 3 | Scenery sunlight | 0 off, any non-zero value on |
| 4 | Local lights | 0 off, any non-zero value on |
| 5 | Framerate adaptive behaviour | 0 Classic (60 Hz), any non-zero value Adaptive |

The menu and ImGui share options 0–4. They reset on the next launch. Resolution, the global shadow toggle, and flares use the game's existing configuration.

## Framerate adaptive behaviour

**Advanced Game Options → Framerate Adaptive Behaviour** selects how cars are drawn between the game's fixed 60 Hz physics steps:

- **Classic (60 Hz)** draws each car at its last physics step, as before. When the frame rate is not exactly 60 FPS, car motion and turns appear stepped.
- **Adaptive**, the default, draws each car between its last two physics steps, so it moves smoothly at any frame rate. The drawn car is up to one physics step (16.7 ms) behind the simulation.

The physics is the same in both modes. The choice is saved in `save/game/openslrr_options`.

## Pedestrians

The **Pedestrian density** slider is gone. The crowd stays at the game's original maximum (1.5). Day and night keep the map coefficient.

## Garage inventory saves (#59)

`Garage.class` fixes the MWM main-menu command: leave the garage first, then
save the career. `Garage.exit` returns the mechanic strip to `player.parts`
before `GameLogic.save` serializes it. No new native or save-format change is needed.

The shipped MWM binary calls `autoSaveQuiet` before `changeActiveSection`,
although `sources/sl/Scripts/game/src/Garage.java` already has the correct order.
The patch changes only those two complete TREE statements, preserving the
constant pool, source lines, all other methods, jump destinations outside the
pair, and the additional classes concatenated in the same TUFA container.

Reproduce with `python scripts/patch_garage_save_order.py INPUT_CLASS OUTPUT_CLASS`.
The builder accepts only the verified MWM input SHA-256 and refuses other versions.
For another mod, put `changeActiveSection(parentState)` before `autoSaveQuiet()`
in its own `CMD_MAINMENU` Java handler and use that mod's compiler.

In-game check: buy several parts, return to the main menu from the garage,
reload the career, and confirm every purchased part remains in the strip.
Repeat after a second purchase and also check normal Exit Game saving.
