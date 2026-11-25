# OpenSLRR Enhanced

Copy the `sl`, `system`, `parts` and `cars` folders from this patch into the game root and accept the replacement of `OptionsDialog.class`, `Garage.class`, `Catalog.class`, the `parts` classes (the base `Swaybar.class`, the three port-flow classes, the re-specced stock engine parts (#411), `SuperCharger` and `Pulley` (#415) and the four fuel classes: `AirFuelDeliverySystem`, `FuelInjectorSystem`, `DynoData` and the SL Tuners methanol fuel rail, and the 48 stock anti-roll bars (#419)), 24 racer classes and 17 racer chassis CFGs (`_main_*.cfg`). `system` adds four new classes in `system/Scripts/render` (`LightSource`, `PointLight`, `SpotLight`, `RectLight`) and the `system/Scripts/render/osdx` package (the CRC 2005 OSD ported to SLRR, with its `dialog` folder); they replace nothing. `sl/Scripts/game` also adds `OsdxOptions.class`, and `system/Scripts/io` adds `InputDevices.class` (the SDL3 controller list for the Devices tab). Install the three `parts` port-flow classes together: the block classes call the updated CylinderHead helper. The fuel classes add fuel heat, ethanol blends and knock; install them together. The Java sources are shipped next to their compiled classes, under `src/`. Use the Enhanced v3 Release.

**Options** (main menu and in-game menu) opens a full-screen screen built on `java.render.osdx` ([docs/ui/osd-v2.md](../docs/ui/osd-v2.md)): Video, Graphics, Sound, Controls (live axis meters, gamma, dead zones, steering speed sensitivity, force feedback), Devices (connected controllers, battery, rumble test, axis calibration) and Game tabs, an info panel that explains the setting under the pointer, and a notification when a preset is applied. Key redefinition, axes and the control files open the classic pages. The classic pages below are still in `OptionsDialog`. The **Video Options** menu applies the resolution and the Enhanced settings in game. A **Preset** choice (Low, Medium, High, Ultra) sets them all at once; changing one setting afterwards shows Custom. **Advanced** opens a second page: VSync, prop detail, fog, car lights, headlight shadows, street lamps, view distance (500–3,000 m, 3 km by default) and the number of street lamps that cast shadows. **Advanced Game Options** has the pedestrian density slider again (0.5–1.5; an old saved value below 0.5 returns to 1.5). The scenery shadow map and the vehicle shadow map each use a resolution of 2048 × 2048. **Ctrl+F12 → Renderer** also sets how many reflection faces are computed per frame.

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

### Per-car bar rates (#419)

All 48 stock bars had `force = 25000` N/m and `damping = 2500` N s/m. The
patched classes in `parts/running_gear/scripts/sways` (sources in `src/`) set
each bar's rate at the wheel from the real car's bar diameter and no damping
(a real bar is a spring). Rate = G π d⁴ / (32 L a²) × MR², with G = 79.3 GPa,
an active length L = 1.0 m, a lever a = 0.22 m and a link-to-wheel ratio MR of
0.9 (strut), 0.65 (double wishbone), 0.7 (multilink), 0.8 (live/solid axle)
or 0.9 (Golf torsion beam, as an equivalent bar). The share column is the
bar's part of that axle's roll stiffness with the stock springs,
2k / (spring + 2k).

| Bars | Real car | Front mm, N/m, share | Rear mm, N/m, share | Source |
| --- | --- | --- | --- | --- |
| Baiern CS | E36 325i | 24, 43,200, 55 % | 18, 8,300, 18 % | OEM parts |
| Baiern DS | E36 M3 | 26.5, 64,300, 62 % | 19, 10,300, 21 % | retailer listings, low |
| Baiern GT | race E36 M3 | 26.5, 64,300, 51 % | 19, 10,300, 14 % | no data: M3 bars |
| SunStrip 15 | del Sol S | 21, 13,200, 37 % | 15, 3,400, 13 % | OEM parts |
| SunStrip 18 | del Sol Si | 22, 15,900, 35 % | 15, 3,400, 11 % | OEM parts |
| SunStrip 22 | del Sol VTi | 24, 22,500, 43 % | 15, 3,400, 10 % | OEM parts |
| SunStrip 20 | race del Sol | 24, 22,500, 30 % | 22, 15,900, 24 % | street-race bars, low |
| Einvagen GT/GTK/GTA | Golf III | 20, 20,800, 37–48 % | beam as 18, 13,700, 28–38 % | front OEM; rear assumed |
| Ishima WRY/WRZ/WRS | Impreza GC8 | 20, 20,800, 35–45 % | 17, 10,900, 24–31 % | front assumed; rear derived, low |
| Focer 200/300/WRC | Lancer Evo VI | 23, 36,500, 51–61 % | 21, 15,300, 31–42 % | OEM-fit bushes |
| MC GT/GTB | Camaro Z28 | 30, 55,000, 67–68 % | 19, 13,400, 38–39 % | forums (solid assumed) |
| MC GTLE | Camaro SS | 32, 71,300, 70 % | 19, 13,400, 31 % | forums |
| Nonus SGT | R34 GT-R | 23, 22,100, 38 % | 22, 18,500, 33 % | no stock data: assumed |
| Nonus GT2 | race R34 | 23, 22,100, 27 % | 27.2, 43,100, 42 % | Nismo catalogue |
| Prime | none | 42,000, 55 % | 17,900, 30 % | share-based (no real car) |
| SuperDuty 500 | Ram 1500 | 30, 55,000, 61 % | none, 0 | bushing kits, low |
| SuperDuty 750 | Ram 2500 | 32, 107,900, 72 % | none, 0 | bushing kits, low |

Before, a 25 kN/m bar more than doubled the roll stiffness of the soft cars
(SunStrip 1.5, Einvagen 110 GT, Focer 200: about 44 kN/m springs) and added
40 % on the GT III. With `--float-mode auto` (#420) the sources compile byte
for byte to the stock classes apart from the force and damping constants;
compile them over the stock classes so auto mode picks the stock float
conversion. Cars already
bought keep their installed bar objects, which take the new class values
when they load.

## Tyre pressure (#424)

Copy `parts` with the patched `Tyre.class` and `WheelRef.class` (sources in
`src/`); install them together, since `Tyre` calls the new
`WheelRef.setRelaxation`. The **Inflation** tuning slider now goes up to 1.5
times the tyre's optimal pressure. Away from the optimum the tyre loses
grip: under-inflated it is also softer, rolls with more resistance and takes
longer to build its cornering force; over-inflated it is a little stiffer
and rolls more freely. At the optimal pressure nothing changes. Details in
[docs/vehicles/notes.md](../docs/vehicles/notes.md#tyre-pressure-424).

## Java natives

| Class and signature | Behavior |
| --- | --- |
| `java.game.parts.WheelRef.setRelaxation(F)V` | Sets the tyre's lateral relaxation length in metres (clamped to 0.05–2; 0.3 by default). |
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
| 13 | VSync | 0 off, non-zero on (default on) |
| 14 | View distance | 500–3,000 metres (default 3,000) |
| 15 | Prop detail | 0 low, 1 medium, 2 high (default), 3 ultra |
| 16 | Fog | 0 off, non-zero on (default on) |
| 17 | Car lights | 0 off, non-zero on (default on): headlight beams and brake lights |
| 18 | Headlight shadows | 0 off, non-zero on (default on) |
| 19 | Street lamps | 0 off, non-zero on (default on) |
| 20 | Shadowed street lamps | 0–6 (default 6) |
| 21 | Steering speed sensitivity | 0–100 percent (default 50): how much an analog wheel or stick's full lock narrows with speed; 0 keeps full lock at any speed, 100 narrows it like keyboard steering |

The menu and ImGui share all of these options. They are saved in `save/game/openslrr_options.json` and persist across launches. Local lights (4) are independent of scenery sunlight (3): turning sunlight off keeps the garage lamps and their shadows. Resolution, the global shadow toggle, and flares use the game's existing configuration.

AO affects indirect diffuse light in the main world view. It leaves direct sunlight,
lamps, emission, reflections and UI unchanged. All AO qualities run at half
resolution; high takes more samples per pixel. AO is off by default to preserve the existing frame cost.
Higher shadow quality increases depth resolution and memory; smoothing changes
comparison filtering independently. Start with medium quality and soft edges.

## Framerate adaptive behaviour

**Advanced Game Options → Framerate Adaptive Behaviour** selects how cars are drawn between the game's fixed 60 Hz physics steps:

- **Classic (60 Hz)** draws each car at its last physics step, as before. When the frame rate is not exactly 60 FPS, car motion and turns appear stepped.
- **Adaptive**, the default, draws each car between its last two physics steps, so it moves smoothly at any frame rate. The drawn car is up to one physics step (16.7 ms) behind the simulation.

The physics is the same in both modes. The choice is saved in `save/game/openslrr_options.json`.

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
closed lifts now imply area-based leakage too. Outputs and tuning behavior
change. The only cams with one, eight SuperDuty cams, are re-specced to a
seated valve (#409, below).
See [derivation, provenance and numeric examples](../docs/vehicles/physics-tick.md#derived-correction-now-shipped).

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

## SuperDuty camshafts: seated closed valves (#409)

`parts/engines/MC_Prime_SuperDuty/scripts` ships eight camshafts:
`MC_Prime_SuperDuty_{L,R}_DOHC_{intake,exhaust}_camshaft` and
`SuperDuty_{L,R}_{intake,exhaust}_camshaft`. They declared a closed-valve lift
of 1.27 mm (0.050 in, the lift at which cam duration is measured); every
other stock cam declares 0. The stock X²/V law made that leak about 0.5 % of
the open flow, the #203 curtain law about 12 %, and the Hauler's SuperDuty 500,
SuperDuty Extra 750, Prime DLH 500 and MC GTLE lost all torque below
2,000–2,500 rpm (Hauler 500: −101 Nm at 1,000 rpm). The cams now declare 0, a
seated valve; no coefficient changes. Closed lift only feeds the head's
minimum flow and is not saved, so cars already bought pick it up.

Stock-car dyno, net torque at 1,000 / 2,000 / 4,000 rpm and peak power
(#203 patch, host dyno):

| Car | Before | After |
| --- | --- | --- |
| Hauler's SuperDuty 500 | −101 / −56 / 181 Nm, 128 PS | 267 / 398 / 561 Nm, 342 PS |
| Hauler's SuperDuty Extra 750 | −120 / −76 / 254 Nm, 180 PS | 296 / 546 / 880 Nm, 503 PS |
| Prime DLH 500 | −140 / −49 / 170 Nm, 223 PS | 247 / 392 / 444 Nm, 298 PS |
| MC GTLE | −177 / −28 / 253 Nm, 273 PS | 385 / 571 / 581 Nm, 361 PS |

The curves now have the stock data's shape (positive from 1,000 rpm, same
peak rpm region). Their level is above the real engines these cars stand for;
that is the engine re-spec's job, not this fix. The eight sources rebuild the
stock classes with only the known one-ULP float differences before the edit.
To rebuild, copy the stock game into a private root, put the sources in
`parts/engines/MC_Prime_SuperDuty/scripts/src` and run
`compile_game_sources.exe <root> parts/engines/MC_Prime_SuperDuty/scripts/src`.

## Hauler's SuperDuty engine re-spec (#417)

After #411 the Haulers made 246 PS / 397 Nm (SuperDuty 500) and 300 PS /
478 Nm (Extra 750) for a 2.4-ton truck; the owner asked for at least double.
Their own engine parts held them back, so these Hauler-only parts are
re-specced (no coefficient):

| Part | Change |
| --- | --- |
| `SuperDuty_5_4_block` | redline 4,500 → 6,500 rpm |
| `SuperDuty_5_4_crankshaft`, `SuperDuty_500_connecting_rods` | maximum 5,500 / 5,250 → 7,250 rpm |
| `SuperDuty_500_transmission`, `SuperDuty_500_flywheel` | maximum 6,000 / 7,000 → 7,250 rpm |
| `SuperDuty_500_pistons` | crown 63 → 90 cc: compression 7.85 → 10.4:1 (95's knock limit 10.5) |
| `SuperDuty_500_H4L/H4R_cylinder_head` | the Haulers get their SuperDuty cams back (stock lists; #411 had given them the MC cams) |
| `SuperDuty_{L,R}_{intake,exhaust}_camshaft` | stock lift again (19 / 13.46 mm) |
| `SuperDuty_E750_supercharger` | 0.35 bar at full speed |

| Car | Before (#411/#415) | After |
| --- | --- | --- |
| Hauler's SuperDuty 500 | 246 PS / 397 Nm | 479 PS @6,500 / 645 Nm @4,000 |
| Hauler's SuperDuty Extra 750 | 300 PS / 478 Nm | 593 PS @6,500 / 830 Nm @4,000 |
| MC GTLE (shares the SuperDuty cams) | 346 PS / 599 Nm | 459 PS @6,250 / 681 Nm @3,750 |

The Extra 750 plateaus near 590 PS: more boost runs its 10.4:1 pistons
(shared with the 500) past 95's knock limit, and above about 0.6 bar the
60 mm belt slips.

## Superchargers: positive displacement, pulleys and belt (#415)

Superchargers used the turbocharger law, a bell curve of boost around an
"optimal" speed: the Hauler's SuperDuty Extra 750 blower added +10 Nm at
1,000 rpm, peaked near 3,300 rpm and cost no drive power. They now follow a
positive-displacement (Roots, scroll) law in the host dyno, with no power
coefficient:

- **Delivery** follows the blower's speed, engine rpm × drive ratio. At full
  speed it reaches 1 + `P_turbo_max` times the inlet pressure (the stock value
  at the stock ratio); internal leakage returns a fixed flow, so the delivered
  share is 1 − `leak_rpm` / blower rpm (300 rpm by default). The bypass caps
  the boost at `P_turbo_waste`.
- **Heating**: T2 = T1 (1 + (PR^0.286 − 1) / `adiabatic_efficiency`); Roots
  0.55 by default, the GTK's scroll blower 0.65. It sets the drive power. The
  game has no intercooler part, so the charge itself is bypassed back to the
  intake temperature (`charger_intercooled`, read by the host, defaults to 1;
  0 would keep the outlet heating). Knock uses the intake temperature, as for
  turbos.
- **Drive power**: the crank pays mdot × cp × (T2 − T1) for every dyno row.
- **Pulleys**: the drive ratio is the crank pulley's `surface_diameter` over
  the blower's `pulley_diameter`. `Pulley.fillDynoData` reports the crank
  pulley to the dyno; the Vee-8 supercharger driver pulley is 90 mm (stock
  ratio 1.5 with the E750's 60 mm pulley), and the SL Tuners Roots blower's
  pulley is 36 mm (its stock 2.5). A blower without a driver pulley part keeps
  its stock ratio.
- **Belt**: as wide as the blower pulley, it carries 27 N per mm of width
  (8 mm pitch timing belt) times its condition (`getWear`). When the drive
  needs more power than the belt carries at the pulley's rim speed, the belt
  slips: the dyno row is computed again with the blower speed the belt holds.

Java: `DynoData` gains `charger_type`, `charger_leak_rpm`,
`charger_efficiency`, `charger_belt_force`, `charger_pulley_radius` and
`crank_pulley_diameter` (copied by `clone`, kept from the bank that carries
the blower in `beAverageOf`, reset by `clear`). `SuperCharger` and `Pulley`
ship patched (`parts/scripts/enginepart/airfueldeliverysystem/charger` and
`.../reciprocatingenginepart`); turbochargers keep the old law.

Stock data: the E750 blower gives 0.35 bar at full speed (the stock ratio, #417);
the GTK blower 0.8 bar with efficiency 0.65 (its stock 3.1 bar, clamped by a
0.62 bar bypass, was a turbo-law workaround).

| Car | Torque at 1,000 / 2,000 / 4,000 rpm, peak | Naturally aspirated twin |
| --- | --- | --- |
| Einvagen 110 GTK | 89 / 173 / 247 Nm, 154 PS | 110 GT: 65 / 111 / 148 Nm, 91 PS |
| Hauler's SuperDuty Extra 750 | 279 / 495 / 830 Nm, 593 PS | SuperDuty 500: 262 / 414 / 645 Nm, 479 PS (#417) |

Rebuild order: `parts/scripts/src` (DynoData), then
`.../reciprocatingenginepart/src` (Pulley, with CylinderHead), then
`.../airfueldeliverysystem/charger/src` (SuperCharger), then the engine
families.

## Stock engine re-spec (#411)

The stock racers get realistic dyno curves with the current engine
calculations (#203 port-flow law and fuel heat, #407 fuel blends and knock,
#409 cams). There is no power coefficient anywhere: only engine part data
changes. Targets are each car's real counterpart, and the base game's
hierarchy for the race versions; weights are unchanged.

Bore, stroke, slots and part fitments are unchanged, so every catalog part
still fits. The levers are the head breathing factors the stock data already
uses (`intake/exhaust_efficiency_tuning`, stock 1.0–1.25, now 0.96–1.56),
boost (wastegate settings at realistic values: 0.8–1.5 bar), cam lift, one
piston crown and one redline. #411 gave the Hauler's SuperDuty 500 and Extra
750 the MC cams; #417 (above) re-specs the Hauler engine and gives them their
SuperDuty cams back. Wastegate and boost
defaults only reach newly assembled cars: an owned turbo keeps its saved
setting, while head breathing and cam data apply at once. The cam swap also
reaches only new Haulers: an owned one keeps its SuperDuty cams and makes
335 PS / 550 Nm (Extra 750: 398 PS / 680 Nm) instead of 246 / 397.

Stock-car dyno (host dyno, net crank power, ch = PS):

| Car | Real counterpart | Before: #407 + #409 (PS / Nm) | After (PS @ rpm / Nm @ rpm) | Target (PS / Nm) |
| --- | --- | --- | --- | --- |
| Baiern CoupeSport | BMW 325i (E36) | 195 / 240 | 195 @6,250 / 240 @4,750 (unchanged) | 192 / 245 |
| Baiern DevilSport | BMW M3 3.2 (E36) | 271 / 329 | 318 @6,250 / 371 @5,250 | 321 / 350 |
| Baiern CoupeSport GT III | race E36 M3 | 340 / 390 | 393 @7,250 / 416 @5,250 | ~360 / 390 |
| Duhen SunStrip 1.5 DVC | CRX del Sol 1.5 | 107 / 138 | 107 @7,000 / 138 @3,000 (unchanged) | 103 / 133 |
| Duhen SunStrip 1.8 DVC | Honda 1.8 (B18B class) | 117 / 151 | 140 @6,750 / 170 @4,750 | 140 / 168 |
| Duhen SunStrip 2.2 DVC | Honda 2.2 VTEC (H22A class) | 125 / 219 | 185 @6,250 / 252 @4,000 | 185 / 206 |
| Duhen Racing SunStrip 2.0 CDVC | race turbo | 165 / 163 | 260 @8,000 / 241 @7,250 | ~260 / 260 |
| Einvagen 110 GT | Golf III 1.8 | 96 / 157 | 91 @4,750 / 150 @3,500 | 90 / 145 |
| Einvagen 110 GTK | Golf G60 class (supercharged 1.8) | 126 / 205 | 154 @5,000 / 248 @3,750 (#415) | 160 / 225 |
| Einvagen 140 GTA | Golf III VR6 power | 146 / 227 | 173 @5,250 / 256 @4,250 | 174 / 235 |
| Ishima Enula WRY | Impreza 2.0 | 109 / 180 | 124 @6,000 / 188 @3,500 | 125 / 180 |
| Ishima Enula WRZ | Impreza WRX | 139 / 219 | 195 @5,500 / 265 @4,750 | 218 / 290 |
| Ishima Enula WR SuperTurizmo | Impreza 22B STi | 205 / 265 | 230 @6,750 / 292 @5,250 | 280 / 363 |
| Shimutshibu Focer RC 200 | Lancer 2.0 NA (4G63 class) | 125 / 194 | 151 @6,250 / 212 @3,750 | 150 / 180 |
| Shimutshibu Focer RC 300 | Lancer Evo VI | 172 / 238 | 283 @7,000 / 327 @5,250 | 280 / 373 |
| Shimutshibu Focer WRC | Evo WRC | 245 / 375 | 298 @6,000 / 420 @3,750 | ~300 / 500 |
| MC GT | Camaro Z28 LT1 | 235 / 366 | 293 @6,250 / 402 @3,750 | 279 / 441 |
| MC GTB | Camaro Z28 LS1 | 249 / 374 | 308 @6,250 / 410 @4,000 | 309 / 454 |
| MC GTLE | Camaro SS / 8.1 big block | 360 / 611 | 459 @6,250 / 681 @3,750 (#417) | 345 / 600 |
| Emer Nonus Street GT | Skyline R34 (NA, 25GT class) | 171 / 228 | 212 @6,500 / 265 @4,750 | 210 / 255 |
| Emer MotorSport Nonus GT II | JGTC R34 | 407 / 457 | 485 @8,750 / 491 @5,000 | ~480 / 480 |
| Prime DLH 500 | big-block muscle car | 285 / 433 | 338 @6,250 / 465 @3,750 | ~340 / 480 |
| Hauler's SuperDuty 500 | Dodge Ram 1500 5.9 | 341 / 558 | 479 @6,500 / 645 @4,000 (#417) | ~500 (#417) |
| Hauler's SuperDuty Extra 750 | Ram 2500 V10 power | 501 / 898 | 593 @6,500 / 830 @4,000 (#417) | ~650 (#417) |

Changed parts (`parts/engines/*/scripts`, sources under `src/`):

| Part | Change |
| --- | --- |
| `Baiern_Devils_6SFi_3_6_block` | redline 7,250 → 7,600 rpm (M3 3.2) |
| `Baiern_Devils_6SFi_cylinder_head` | breathing 1.25 → 1.44 |
| `Duhen_88er_cylinder_head`, `Duhen_93er_cylinder_head` | breathing 1.14, 1.36 (declared 1.0, 1.0) |
| `Einvagen_110_GT_cylinder_head` | breathing 0.96 (none declared: 1.0) |
| `Einvagen_PerPro_GTA_cylinder_head` | breathing 1.27 |
| `Einvagen_110_GTK_pistons` | crown −25.61 → −20 cc: compression 7.5 → 8.0:1 |
| `Einvagen_GTK_9psi_supercharger` | boost 0.62 → 0.80 bar (description updated) |
| `Ishima_WR_head_cylinder_head` | breathing 1.10 |
| `Ishima_WRZ_turbocharger` | wastegate 0.4 → 1.0 bar, adjustable up to 1.0 (description updated) |
| `Shimutshibu_RC_DDSA1C/DDSB3C/DDSB8C_cylinder_head` | breathing 1.16 / 1.38 / 1.16 |
| `Shimutshibu_RC_M3FCR15W7_turbocharger`, `..._MWFCR22W12_turbocharger` | wastegate 0.65 → 1.0 bar, 1.25 → 1.5 bar |
| `SuperDuty_500_H4L/H4R_cylinder_head` | breathing 1.38 in / 1.50 ex (the MC cam lists for the Haulers were reverted by #417) |
| `SuperDuty_{L,R}_{intake,exhaust}_camshaft` | stock lift again in #417 (#411 had 8.8 / 6.2 mm) |
| `SuperDuty_E750_supercharger` | boost ceiling 1.15 → 0.42 bar |
| `MC_Prime_SuperDuty_{L,R}_DOHC_cylinder_head` | breathing 1.32 in / 1.43 ex |
| `Emer_StreetGT_cylinder_head`, `Emer_MotorSport_GT2_cylinder_head` | breathing 1.18, 1.56 |

At realistic boost the WRZ (−10 %), the SuperTurizmo (−18 %) and the GTK
(−17 %) stay below their real counterparts: their heads are shared with the
naturally aspirated versions, and the knock model has no intercooler. The
torque peaks of several engines sit higher in the rev range than the real
ones (stock cam timing is unchanged).

To rebuild, copy the stock game into a private root, put the sources in each
family's `scripts/src` and run `compile_game_sources.exe <root>
parts/engines/<family>/scripts/src`. Every edited source rebuilt its stock
class before the edit, with only the known one-ULP float constants differing.

## Fuel heat, blends and knock (#203, #407)

Four `parts` classes carry the fuel model; install them together:
`parts/scripts/enginepart/AirFuelDeliverySystem.class`,
`parts/scripts/enginepart/airfueldeliverysystem/FuelInjectorSystem.class`,
`parts/scripts/DynoData.class` and
`parts/engines/Einvagen_Duhen_Ishima_Focer/scripts/SL_Tuners_methanol_fuel_rail.class`.

Each fuel releases its lower heating value divided by its stoichiometric
air:fuel ratio per kg of charge (#203). Pump 95 is E5 and pump 98 is E10
(ethanol by volume), so their ratio and heating value follow the blend; 100
octane race fuel has no ethanol (#407):

| Fuel | Lower heating value | Stoichiometric ratio | Best power | Heat | Knock limit | Allowed compression |
| --- | --- | --- | --- | --- | --- | --- |
| 95 (E5) | 42.52 MJ/kg | 14.40:1 | 12.38:1 | 2.953 MJ/kg | 10.5:1 | 6.5–16.0:1 |
| 98 (E10) | 41.65 MJ/kg | 14.10:1 | 12.13:1 | 2.954 MJ/kg | 11.5:1 | 6.5–17.0:1 |
| 100 (E0) | 43.40 MJ/kg | 14.70:1 | 12.64:1 | 2.952 MJ/kg | 12.5:1 | 6.5–18.0:1 |
| Methanol | 19.90 MJ/kg | 6.47:1 | 5.05:1 | 3.076 MJ/kg | 15.0:1 | 8.0–20.5:1 |
| Diesel | 43.40 MJ/kg | 14.50:1 | 18.13:1 | 2.993 MJ/kg | none | 14.0–24.0:1 |

Best power is lambda 0.86 for gasolines, 0.78 for methanol and 1.25 (lean,
smoke limit) for diesel. The stock mixture-match factor (0.8 to 1 away from
the best ratio) still applies. Per kg of air the gasolines release almost the
same heat, as real fuels do: octane acts through knock.

The knock limit is the highest static compression a naturally aspirated 20 °C
charge runs at full spark advance. `FuelInjectorSystem` passes it to the dyno
as the new `DynoData.knockCompression` field; the host retards the spark above
it (about 2.5% of the heat per compression point, down to 60%), boost and
intercooling included ([driving model](../docs/vehicles/driving-model.md)).
The allowed compression only refuses engines that cannot run on the fuel: it
no longer moves with the mixture, and its maximum is the knock limit + 5.5.
The tuning slider and the mixture clamp go down to 4:1, so methanol reaches its
best mixture. The SL Tuners methanol rail now defaults to 5.5:1 with a fuel
flow limit of 0.0655 (stock 0.02 at 18:1, the same air flow). A saved methanol
injection above 10:1 is scaled by 5.05/16 on load, keeping its richness
relative to the old 16:1 optimum.

`DynoData.class` gains one field and recompiles from `sources/` with the known
one-ULP differences in its float defaults. To rebuild, copy the stock game into
a private root (never the installed game or a hard-linked view), put the four
sources in its `src` folders and compile, in this order:

```powershell
.\build\Release\compile_game_sources.exe <root> parts/scripts/src
.\build\Release\compile_game_sources.exe <root> parts/scripts/enginepart/src
.\build\Release\compile_game_sources.exe <root> parts/scripts/enginepart/airfueldeliverysystem/src
.\build\Release\compile_game_sources.exe <root> parts/engines/Einvagen_Duhen_Ishima_Focer/scripts/src
powershell -File scripts/run_save_io_tests.ps1 -Suite port_flow
```

The suite runs `updatevariables` in the VM for every fuel at four mixtures and
the host dyno with and without knock, boost and intercooling.
