# OpenSLRR Enhanced

Copy the `sl`, `system`, `parts` and `cars` folders from this patch into the game root and accept the replacement of `OptionsDialog.class`, `Garage.class`, `Catalog.class`, the `parts` classes (the base `Swaybar.class`, the three port-flow classes and `AirFuelDeliverySystem.class`), 24 racer classes and 17 racer chassis CFGs (`_main_*.cfg`). `system` adds four new classes in `system/Scripts/render` (`LightSource`, `PointLight`, `SpotLight`, `RectLight`); they replace nothing. Install the three `parts` port-flow classes together: the block classes call the updated CylinderHead helper. `AirFuelDeliverySystem.class` keeps the stock fuel heat (#352). The Java sources are shipped next to their compiled classes, under `src/`. Use the Enhanced v3 Release.

The **Video Options** menu applies the resolution and the Enhanced settings in game. Exterior scenery range is 3 km. The scenery shadow map and the vehicle shadow map each use a resolution of 2048 × 2048. **Ctrl+F12 → Renderer** also sets how many reflection faces are computed per frame.

## Anti-roll bars (#300)

**Running Gear → Suspension** now includes the existing anti-roll bars. Copy
`parts` as well as `sl`: the base Swaybar class adds the authored series prefix
to its name. The stock variants retain their original IDs, tuning and generic
attachment compatibility; a nominal front/rear name is not a fit restriction.

Fit a purchased bar to a chassis mount: 701 selects the front axle, 702 the
rear. Installed bars apply condition-scaled spring/damping inputs through the
host's bounded anti-roll approximation, including while coasting with no engine
or controller. Removing or moving a bar replaces its contribution rather than
accumulating another command. Cars without a bar retain their prior physics.
This deliberately enables a dormant stock feature; it is not recovered MWM
anti-roll physics.

### Stock bars on every racer (#311)

Copy `cars` too. Every stock racer now has both mounts and comes with its own
bars when it is newly assembled (dealer, used cars, opponents):

- The 17 chassis CFGs (`_main_*.cfg`) that lacked a mount get one: Baiern,
  Enula, MC, Nonus, Prime and SuperDuty get 701 and 702, Einvagen gets 702.
  Positions follow the authored Duhen mounts relative to the wheels (front bar
  0.224 m ahead of the front axle, rear bar 0.225 m ahead of the rear axle). The
  bar mesh is not resized to the car's track.
- The 24 car scripts uncomment their stock bar lists. They are the MWM 2.2.1
  scripts (stock neons, NOS bottles and rollbars included), rebuilt byte for
  byte before the edit. Stock lists that named a Duhen bar or the front bar
  twice now name the car's own bars: Duhen SunStrip 1.5 DVC rear, Ishima Enula
  WRZ (and its stage-1 WRS list) and WR SuperTurizmo.
- Cars already bought or saved keep their parts.

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
| 6 | Anisotropic filtering | 0 off, non-zero on |
| 7 | Ambient occlusion | 0 off (default), 1 HBAO, 2 GTAO |
| 8 | AO quality | 0 low, 1 medium (default), 2 high |
| 9 | Shadow quality | 0 low, 1 medium (default), 2 high |
| 10 | Shadow smoothing | 0 sharp, 1 soft (default), 2 very soft |
| 11 | AO strength | 0–200 percent (default 100) |
| 12 | AO radius | 25–300 centimetres (default 100) |

The menu and ImGui share all of these options. They are saved in `save/game/openslrr_options` and persist across launches. Local lights (4) are independent of scenery sunlight (3): turning sunlight off keeps the garage lamps and their shadows. Resolution, the global shadow toggle, and flares use the game's existing configuration.

AO affects indirect diffuse light in the main world view. It leaves direct sunlight,
lamps, emission, reflections and UI unchanged. All AO qualities run at half
resolution; high takes more samples per pixel. AO is off by default to preserve the existing frame cost.
Higher shadow quality increases depth resolution and memory; smoothing changes
comparison filtering independently. Start with medium quality and soft edges.

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

Since #235 the shipped `Garage.class` is compiled from
`sl/Scripts/game/src/Garage.java` in this patch, which keeps this order. The
binary builder `python scripts/patch_garage_save_order.py INPUT_CLASS OUTPUT_CLASS`
still reproduces the #59-only class from the verified MWM input (it refuses
other versions), but its output is no longer the file shipped here.
For another mod, put `changeActiveSection(parentState)` before `autoSaveQuiet()`
in its own `CMD_MAINMENU` Java handler and use that mod's compiler.

In-game check: buy several parts, return to the main menu from the garage,
reload the career, and confirm every purchased part remains in the strip.
Repeat after a second purchase and also check normal Exit Game saving.

## Garage lights (#235)

`Garage.java` creates one `java.render.SpotLight` per twin-tube ceiling fitting,
parented to the garage stuff mesh, and destroys them when leaving the garage:
9 lights in the first garage, 10 in the second and 12 in the third. The ROC
garage is outdoors and gets none. The engine draws them with the Enhanced
local-light path, so **Local lights** must be on. Without the patched classes the
engine places the same fittings itself in the three garages. The tube faces
glow in every garage. Brightness is set per garage in `createFixtures`
(intensity 30, range 26 m by default; the first version used 24 and 20 m).

| Class | Use |
| --- | --- |
| `PointLight(ResourceRef parent)` | Omnidirectional light. One is drawn per interior. |
| `SpotLight(ResourceRef parent)` | Cone light; `setCone(innerDeg, outerDeg)`. |
| `RectLight(ResourceRef parent)` | Area light drawn as a spot for now; `setSize(w, h)`. |

All three inherit `setPosition`, `setDirection` (parent-local, metres),
`setColor(0xRRGGBB)`, `setIntensity`, `setRange`, `setShadows`, `setEnabled` and
`destroy` from `LightSource`. Lights follow their parent and disappear with it.
Car lights stay engine-driven.

`Garage.java` also saves only after a successful Choose Track or car dealer
transition. A refused command (no car, undriveable car, dealer closed)
stays in the garage and does not save. The Car Lot saves before it opens, as
in MWM, after moving the parts bought in the garage to your inventory. Saving
after it opened took the current car out of its slot (#315). Street lamps and other scenery lights do not use
this API: the engine reads them from the map objects (#237).

## Street lamps (#237)

At night (18:30 to 06:00) the street lamps placed in the maps light the road
around them. The engine reads the lamps' own light resources, so no class is
patched. Up to 64 lamps in view, out to 500 m, are lit and fade in with distance; lamps
that only glow in the stock game (`fake point`) add no light. They need
**Local lights**; switching it off turns them off. With shadows on, the six
lamps nearest your car cast shadows of cars, people, trees and poles, and
every lamp that lights your car casts its shadow; turning the camera does not
change them (Shadow quality
sets their resolution); buildings cast no lamp shadow.

In-game check: in Valo City, set the clock to night and drive under lamp posts;
the road should be lit around them. At noon they add no light. Switch Local
lights off and on at night.

In-game check: with Local lights on, enter each club garage (first, second and
third) and check that every ceiling fitting lights the floor under it; leave and
re-enter, then switch Local lights off and on.

## Port-flow normalization (#203)

Install the complete `parts` subtree, including `CylinderHead.class`,
`Block_Inline.class` and `Block_Vee.class`, then restart. The shared head helper
replaces squared disk-area/lift volume with capped valve-curtain area times a
nominal ideal-air flow speed and the existing 1-ms reference interval, divided
by cylinder swept volume. Both Vee banks use it before averaging. Native DynoData
field meanings and fuel/save schemas are unchanged.

The reference speed is derived from NASA's M=1 nozzle capacity at 293.15 K,
not fitted to stock horsepower. The stock 0.9 breather factor remains empirical.
This is a dimensional correction inside the legacy pressure-relaxation model,
not a complete pressure-dependent compressible valve solver. Existing nonzero
closed lifts now imply area-based leakage too; SuperDuty's authored 1.27-mm
minimum therefore changes noticeably. Outputs and tuning behavior change.
See [derivation, provenance and numeric examples](../docs/PHYSICS_TICK_SPIKE.md#derived-correction-now-shipped).

To reproduce the included TUFA with the embedded Windows compiler, first follow
the [build recipe](../README.md#build) and build target `compile_game_sources`.
Copy the patch's `parts` subtree into a **private** game copy, then run:

```powershell
.\build\Release\compile_game_sources.exe data\game parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart
.\build\Release\compile_game_sources.exe data\game parts/scripts/enginepart/block
powershell -File scripts/run_save_io_tests.ps1 -Suite port_flow
```

The compiler reports one head class and two block classes. The tests execute the
shipped helper in the VM and cover SI/script-unit agreement, low-lift linearity,
throat saturation, volume/valve-count/wear/efficiency scaling and zero geometry.

## Fuel heat (#203, reverted in #352)

`AirFuelDeliverySystem.class` uses the stock heat values (about 1.1 MJ/kg
times 1.55) again, so engine mods tuned for the stock scale keep working. The
host's stock calibration (`engine_stock_calibration`, #352) keeps the 24 stock
racers at their pre-#215 maxHP on average instead. The #203 handbook heat
stays in the source, commented out: lower heating value divided by the
stoichiometric air:fuel ratio, per kg of charge:

| Fuel | Lower heating value | Stoichiometric ratio | Heat at the best mixture |
| --- | --- | --- | --- |
| 95, 98, 100 octane | 43.4 MJ/kg | 14.7:1 | 2.952 MJ/kg (98 and 100 keep their stock 0.05% and 0.07% more) |
| Methanol | 19.9 MJ/kg | 6.47:1 | 3.076 MJ/kg |
| Diesel | 43.4 MJ/kg | 14.5:1 | 2.993 MJ/kg |

The stock mixture-match factor (0.8 to 1 away from a fuel's best ratio) and
compression window apply as in the stock game. Install it with the rest of
`parts`; it adds no field and changes no method signature. To rebuild it, copy
`parts` into a private game copy and run:

```powershell
.\build\Release\compile_game_sources.exe data\game parts/scripts/enginepart/src
powershell -File scripts/run_save_io_tests.ps1 -Suite port_flow
```

The suite runs `updatevariables` in the VM for every fuel at three mixtures.

## Stock engine power (#352)

With the SI torque law and the port-flow law, the host scales indicated torque
by 2.8 so the 24 stock racers keep their pre-#215 maxHP on average. The
port-flow law still moves engines apart: from 45% (SuperDuty 500) to 152%
(Baiern CoupeSport) of their previous maxHP. `stock_engine_power` pins every
racer's figure with this patch installed.
