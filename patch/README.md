# OpenSLRR Enhanced

Copy the `sl`, `system`, `parts`, `cars`, `particles` and `MW_Mod` folders from this patch into the game root and accept the replacement of `OptionsDialog.class`, `Track.class` (#470), `Garage.class`, `Catalog.class`, `TestTrack.class`, `City.class`, `CarLot.class`, `ROCTrack.class`, `MainMenu.class`, `MW_Mod/Scripts/DealerTrack.class` (#465), the `parts` classes (the base `Swaybar.class`, the three port-flow classes, the re-specced stock engine parts (#411), `SuperCharger` and `Pulley` (#415) and the four fuel classes: `AirFuelDeliverySystem`, `FuelInjectorSystem`, `DynoData` and the SL Tuners methanol fuel rail, the 48 stock anti-roll bars (#419), and `Brake`, `DiscBrake` and the 48 stock brakes (#446), the 96 stock suspension corners (#450)), `system/Scripts/lang/Math.class` (#450, #463), 24 racer classes, the 24 racer chassis CFGs (`_main_*.cfg`) and the part CFGs that now carry a mass (tyres, running gear, accessories, exhausts, mufflers, seats, steering wheels, wings). `system` adds four new classes in `system/Scripts/render` (`LightSource`, `PointLight`, `SpotLight`, `RectLight`) the `system/Scripts/render/osdx` package (the CRC 2005 OSD ported to SLRR, with its `dialog` folder) and the `system/Scripts/openslrr` mod API (#487, [docs/scripting/mod-api.md](../docs/scripting/mod-api.md)); they replace nothing. `sl/Scripts/game` also adds `OsdxOptions.class`, and `system/Scripts/io` adds `InputDevices.class` (the SDL3 controller list for the Devices tab). Install the three `parts` port-flow classes together: the block classes call the updated CylinderHead helper. The fuel classes add fuel heat, ethanol blends and knock; install them together. The Java sources are shipped next to their compiled classes, under `src/`. Use the Enhanced v3 Release.

**Options** (main menu and in-game menu) opens a full-screen screen built on `java.render.osdx` ([docs/ui/osd-v2.md](../docs/ui/osd-v2.md)): Video, Graphics, Sound, Controls (live axis meters, gamma, dead zones, steering speed sensitivity, force feedback), Devices (connected controllers, battery, rumble test, axis calibration) and Game tabs, an info panel that explains the setting under the pointer, and a notification when a preset is applied. Key redefinition, axes and the control files open the classic pages. The classic pages below are still in `OptionsDialog`. The **Video Options** menu applies the resolution and the Enhanced settings in game. A **Preset** choice (Low, Medium, High, Ultra) sets them all at once; changing one setting afterwards shows Custom. **Advanced** opens a second page: VSync, prop detail, fog, car lights, headlight shadows, street lamps, view distance (500–3,000 m, 3 km by default), the number of street lamps that cast shadows and the tyre smoke density (0–400 % of the default, also in **Graphics → World**). **Advanced Game Options** has the pedestrian density slider again (0.5–1.5; an old saved value below 0.5 returns to 1.5). The scenery shadow map and the vehicle shadow map each use a resolution of 2048 × 2048. **Ctrl+F12 → Renderer** also sets how many reflection faces are computed per frame.

## Scripts use the Math functions (#465)

Copy `sl`, `MW_Mod` and `parts` with `system/Scripts/lang/Math.class`: the
classes below read `Math.PI`, `Math.HALF_PI` or `Math.DEG2RAD` and call
`Math.wrapAngle`, `Math.clamp`, `Math.randomInt` or `Math.randomRange`
(#463), so they need the patched `Math` class and do not run in the
original game.

- `TestTrack`, `City`, `CarLot`, `ROCTrack`: half turns and start headings
  use `Math.PI` instead of 3.14 / 3.140 / 3.142 (and -1.571 is
  `-Math.HALF_PI`); `TestTrack` compares the car's heading with the start's
  through `Math.wrapAngle` instead of a hand-made wrap with the wrong sign.
- `MainMenu`: the random driver face in the player setup can be the last of
  the 60 (`Math.randomInt`).
- `MW_Mod/Scripts/DealerTrack`: the car that leaves a full dealer can be
  the last one, and the parked cars' random steering is -0.5 to 0.5 as its
  comment says (`Math.randomRange`; the stock expression mixed a string and
  a number).
- `Tyre` (degrees to radians, the mirrored tyre's half turn, the
  inflation clamp), `DiscBrake`, `CylinderHead`, `Block_Inline` and
  `Block_Vee` (pi was 3.14159265 or 3.1416).

These stock sources were first rebuilt against their MWM classes: the same
bytes apart from the compile stamp (also in each inner class) and the case
of two resource paths (`misc` / `Misc`, `prime` / `Prime`). `Track` and
`CarSelect` rebuild to other sizes (MWM changed them) and keep their hand
written values; so does `Suspension`'s own `DEG2RAD`, which is the same
number.

## Tyre flat spots (#461)

Copy `parts`: the patched `Tyre.class` (source in `src/`) keeps the flat spot
a locked wheel grinds into its tread (`flat_spot`, `flat_spot_angle`, which
the host updates), saves it (Tyre save version 2; older saves load without
one) and clears it on a repair in the garage. Tyre pressure, wear,
punctures and debeading are host-only and need no file.

## Tyre smoke (#454)

Copy `particles` too. `particles/scripts/particle_clouds/asphalt.cfg` is the
stock tyre smoke cloud retuned for the volumetric smoke: puffs born 0.6 m wide
that swell to 4-7.5 m, live 4-9 s, lighter grey, rising at 0.6 m/s² instead of
2 m/s² and damped less, 30 per second instead of 20. The host still decides
how much smoke each tyre makes (its sliding power and tread temperature) and
lets it hang at 15 % of the cfg gravity.

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

## Weight distribution and centre of gravity (#430)

Copy `cars` and `parts`. Stock racers had their weight almost evenly split
front to rear (Duhen SunStrip 51/49 instead of about 61/39), and most sat
5–9 cm too high, so they lifted two wheels easily and became unstable under
hard braking. The stock data is the cause: the chassis CFGs put most of their
mass in one rear-biased `_phys_chassis_frame` body, and 101 parts weighed
nothing (springs, dampers, struts, arms, brakes, anti-roll bars, batteries,
exhausts, mufflers, radiators, seats, steering wheels, wings), with tyres at
0.06–0.13 kg.

- **Parts**: each gets one `body` sphere with `nocollision` at its origin
  (brake 8 kg, strut 12, trailing arm 10, damper 2.5, spring 2, bar 4,
  battery 14, exhaust 6–9, muffler 5–7, radiator 7, bucket seat 10, steering
  wheel 1.5, wing 5). Tyres get a mass from their size (tread 15 kg/m²,
  sidewalls 6 kg/m²): 7.4 kg (195/50 R16) to 16.8 kg (405/40 R15).
- **Chassis** (all 24 `_main_*.cfg`): each car keeps its stock total mass. The
  chassis gives up the mass now carried by its parts, then mass moves from the
  frame body to a front box (front structure, drivetrain, fluids: 0.30 m above
  the ground, 0.20 m behind the front axle) and, where the car sat too high, from
  the cockpit, roof and trunk bodies to a floor box (0.25 m above the ground).
  Both boxes have `nocollision`; collisions and crush zones are unchanged.

Targets come from the real cars (front share, parked height of the centre of
gravity); heights are only lowered:

| Car | Front share | COG height (m) |
| --- | --- | --- |
| Baiern CoupeSport / DevilSport / GT III | 47.6–49.0 → 51.0–51.9 % | 0.526 / 0.511 / 0.434 → 0.500 / 0.500 / 0.435 |
| Duhen SunStrip 1.5 / 1.8 / 2.2 / Racing | 50.2–52.0 → 61.0 % | 0.583 / 0.582 / 0.574 / 0.480 → 0.508 / 0.507 / 0.500 / 0.469 |
| Einvagen 110 GT / GTK / 140 GTA | 54.0–54.7 → 62.0 % | 0.547 / 0.540 / 0.529 → 0.532 / 0.526 / 0.517 |
| Ishima Enula WRY / WRZ / WRS | 43.8–45.3 → 58.0 % | 0.553 / 0.545 / 0.540 → 0.520 / 0.524 / 0.521 |
| Focer RC 200 / RC 300 / WRC | 46.8–48.0 → 59.0 % | 0.578 / 0.573 / 0.574 → 0.520 |
| MC GT / GTB / GTLE | 47.2–47.6 → 56.0 % | 0.472–0.473 → 0.470 |
| Nonus Street GT / GT II | 48.2 / 49.4 → 55.0 % | 0.589 / 0.496 → 0.517 / 0.491 |
| Prime DLH 500 | 46.5 → 52.0 % | 0.492 → 0.470 |
| SuperDuty 500 / Extra 750 | 47.9 / 47.7 → 57.0 % | 0.610 / 0.614 → 0.609 / 0.612 (unchanged) |

Cars already bought keep their chassis object but read the new CFG masses when
they load. Details in
[docs/vehicles/notes.md](../docs/vehicles/notes.md#weight-distribution-and-centre-of-gravity-430).

## Brakes as hardware (#446)

Copy `parts`: the patched `Brake.class`, `DiscBrake.class`,
`WheelRef.class` and the 48 brakes in `parts/running_gear/scripts/brakes`
(sources in `src/`). Install them together: `DiscBrake` uses the new `Brake`
fields and `Brake` calls the new `WheelRef.setBrakeSpec`.

A disc brake's torque now comes from its hardware: 2 pad faces x the pad
friction (street "conventional" 0.40, "SL Tuners Green" 0.45, "SL Tuners
Black" 0.50) x 100 bar at full pedal x the piston area of one side of the
caliper x the caliper's stiffness (street 1, SL Tuners Street 1.05, Race
1.10) x the radius of the pad's centre (25 mm inside the rotor's edge), times
the stock wear factor. A new `piston_mm` field gives the piston size (0 keeps
54, 40 or 34 mm for 1, 2 or 3 pistons per side, which mod brakes such as
MW-Tunners get). The brake balance acts like a valve: 0.5 leaves both axles
their torque, an adjuster moved away cuts one of them. The host then heats
each rotor and fades its pads: street pads work from cold and fade past
about 400 deg C, race pads are weak cold and hold to about 650, vented and
carbon rotors store and shed more heat, and carbon moves the fade up.

The stock brakes are re-sized from the real cars of the [anti-roll bar
table](#per-car-bar-rates-419), so the fronts lock first, at about 60-75 % of
the pedal on street cars and 45-55 % on race cars with warm pads (about a
tenth more pedal with cold ones). Race pads are kept for the
race versions only. The part names now carry the new disc size, and the
descriptions the piston size and torque.

| Brakes | Real car | Front: disc, pistons per side, pads | Nm | Rear | Nm | Stock script Nm F / R |
| --- | --- | --- | --- | --- | --- | --- |
| Baiern CS | E36 325i | 286 vented, 1x57 mm, street | 2409 | 276 solid, 1x42 mm, street | 1252 | 3718 / 2928 |
| Baiern DS | E36 M3 | 315 vented, 1x54 mm, fast road | 2731 | 312 vented, 1x38 mm, fast road | 1337 | 5221 / 3978 |
| Baiern GT | E36 M3 GT race | 355 carbon, 3x34 mm, race | 4361 | 330 carbon, 1x36 mm, race | 1496 | 4822 / 4276 |
| SunStrip 15 | del Sol S | 242 vented, 1x54 mm, street | 1759 | 239 solid, 1x30 mm, street | 534 | 3356 / 2945 |
| SunStrip 18 | del Sol Si | 262 vented, 1x54 mm, fast road | 2185 | 239 solid, 1x28 mm, fast road | 524 | 3493 / 3219 |
| SunStrip 22 | del Sol VTi | 262 vented, 1x54 mm, fast road | 2185 | 260 solid, 1x28 mm, fast road | 582 | 3659 / 3493 |
| SunStrip 20 | race del Sol | 282 carbon, 2x42 mm, race | 3214 | 239 carbon, 1x26 mm, race | 502 | 4125 / 3525 |
| Einvagen GT | Golf III 1.8 | 256 vented, 1x54 mm, street | 1887 | 226 solid, 1x30 mm, street | 498 | 3168 / 2919 |
| Einvagen GTK | Golf III GTI 16V | 280 vented, 1x54 mm, fast road | 2370 | 232 solid, 1x30 mm, fast road | 579 | 3493 / 3219 |
| Einvagen GTA | Golf III VR6 | 288 vented, 1x57 mm, fast road | 2870 | 232 solid, 1x32 mm, fast road | 692 | 4403 / 3178 |
| Ishima WRY | Impreza GC8 2.0 | 260 vented, 1x57 mm, street | 2143 | 266 solid, 1x34 mm, street | 784 | 3416 / 3168 |
| Ishima WRZ | Impreza WRX | 277 vented, 1x57 mm, fast road | 2607 | 266 vented, 1x34 mm, fast road | 882 | 3946 / 3659 |
| Ishima WRS | Impreza STi | 294 carbon, 2x42 mm, fast road | 3195 | 290 carbon, 1x32 mm, fast road | 912 | 4822 / 4276 |
| Focer 200 | Evo VI RS | 294 vented, 1x54 mm, fast road | 2515 | 284 solid, 1x30 mm, fast road | 744 | 3895 / 3485 |
| Focer 300 | Evo VI GSR | 320 vented, 2x42 mm, fast road | 3367 | 300 vented, 1x30 mm, fast road | 795 | 5111 / 4355 |
| Focer WRC | Evo WRC tarmac | 355 vented, 3x36 mm, race | 4890 | 300 vented, 1x28 mm, race | 808 | 5503 / 4735 |
| MC GT | Camaro Z28 | 302 vented, 1x54 mm, fast road | 2727 | 305 vented, 1x36 mm, fast road | 1226 | 4070 / 5233 |
| MC GTB | Camaro Z28 SS | 330 vented, 2x40 mm, fast road | 3325 | 305 vented, 1x38 mm, fast road | 1366 | 4237 / 5566 |
| MC GTLE | Camaro SS | 345 vented, 3x34 mm, fast road | 3797 | 330 vented, 1x38 mm, fast road | 1500 | 4569 / 6541 |
| Nonus SGT | R34 GT-R | 324 vented, 2x42 mm, fast road | 3417 | 300 vented, 1x40 mm, fast road | 1414 | 4483 / 3718 |
| Nonus GT2 | R34 race | 355 carbon, 3x36 mm, race | 5122 | 330 carbon, 1x34 mm, race | 1398 | 4725 / 4275 |
| Prime | none (V8 coupe) | 330 vented, 2x40 mm, fast road | 3325 | 310 vented, 1x46 mm, street | 1815 | 5233 / 3700 |
| SuperDuty 500 | Ram 1500 | 330 vented, 1x68 mm, street | 4067 | 330 solid, 1x44 mm, street | 1703 | 5734 / 5298 |
| SuperDuty 750 | Ram 2500 | 345 vented, 1x72 mm, fast road | 5405 | 350 vented, 1x48 mm, fast road | 2443 | 6545 / 4714 |

Cars already bought keep their brake objects, which take the new values
when they load. Without these classes the host keeps the stock torques (#400)
and no heat.

The patched `Tyre.class` (with the #424 pressure changes) also sends its
compound through `WheelRef.setTyreSpec`, so each tyre has a temperature that
sets its grip: a street tyre works from cold, a soft racing tyre needs to be
warmed. **Options → Game → TEMPERATURES** (on by default, `OptionsDialog` and
`OsdxOptions`) shows each tyre's and brake's temperature above the gauges.
The HUD lights its stock ABS and ASR lamps while they work. Details in
[docs/vehicles/notes.md](../docs/vehicles/notes.md#brakes-as-hardware-abs-and-asr-446).

## Tyres and suspension geometry (#450)

Copy `parts` and `system`: the 96 stock suspension classes in
`parts/running_gear/scripts/{FL,FR,RL,RR}_susp`, the patched `Tyre.class`
and `system/Scripts/lang/Math.class` (sources in `src/`). Install them
together: `Tyre` calls the new `Math.pow`, which the host provides
(as in SLRR 2.3.1).

**Suspension.** Every stock car had the Baiern's geometry: a strut whose
lower arm sat past horizontal at the ride height (the wheel gained positive
camber in bump), a semi-trailing arm swept 50 degrees at the rear (-2.9 deg
of camber per 25 mm, static camber from +2.1 to -6.0 deg depending on the
ride height) and 1 to 2.5 deg of toe-in per rear wheel. Each corner is now
laid out at the car's static ride height (from its spring and the #430
weights) after the real car:

- Fronts keep the strut fit: the lower arm is near horizontal at rest, the
  strut top sits 0.58 m up the kingpin axis, and the hub is tilted to the
  static camber (the host ignores `setYpr` in strut mode). Double-wishbone
  and multilink fronts use the same fit with their steeper camber gain.
- Independent rears are a front-view swing arm (arm length = 1 / camber
  gain) with the factory camber and toe in `setYpr`.
- Live rear axles (MC, SuperDuty) have a 25 m arm (0.06 deg per 25 mm): the
  wheels stay square to the axle, so camber follows the body in roll.
- Every wheel keeps the stock geometry's position at rest (#456): fronts 9 to
  11 cm inboard of the CFG wheel centre, rears 2.8 cm outboard, so the track
  widths and the fitment are unchanged. Front wheels sit 0.4 to 1.6 cm higher
  in the arch than with the stock arm.

| Cars | Real car | Front camber, gain /25 mm | Rear camber, toe-in, gain /25 mm |
| --- | --- | --- | --- |
| Baiern CS / DS / GT III | E36 325i / M3 / race | -0.5 / -0.9 / -3.0, -0.4 | -1.8 / -1.6 / -2.0, 0.15 / 0.15 / 0.10, -0.9 |
| SunStrip 1.5 / 1.8 / 2.2 / Racing | del Sol / race | 0 (race -2.5), -0.6 | -0.7 (race -1.5), 0.10, -0.7 |
| Einvagen 110 GT / GTK / 140 GTA | Golf III (torsion beam) | -0.5, -0.4 | -1.5, 0.17, -0.2 |
| Enula WRY / WRZ / WRS | Impreza GC8 | -0.3 / -0.3 / -0.5, -0.4 | -1.0 / -1.0 / -1.2, 0.10, -0.4 |
| Focer RC 200 / 300 / WRC | Lancer Evo VI / WRC | -0.3 (WRC -2.0), -0.4 | -1.3 (WRC -1.5), 0.10, -0.7 |
| MC GT / GTB / GTLE | Camaro Z28 / SS | 0, -0.6 | live axle: 0, 0 |
| Nonus Street GT / GT II | R34 GT-R / JGTC | -1.1 / -3.0, -0.6 | -1.5 / -2.0, 0.10, -0.7 |
| Prime DLH 500 | none | -1.0, -0.6 | -1.5, 0.10, -0.7 |
| SuperDuty 500 / 750 | Ram 1500 / 2500 | +0.3, -0.5 | live axle: 0, 0 |

**Caster (#458).** The stock struts had none: their strut axis and top lay
in the front-view plane. Each front strut now leans back by its real car's
factory figure where one was found (E36 325i +3°50', M3 +6°38', Golf III
+1°45', Impreza 3°, Lancer Evolution VIII 3°55', 4th-generation Camaro +3°,
Ram 1500 +3.4°), a typical one otherwise (del Sol, R34, race cars, Prime). The host steers the hub about
that kingpin (ball joint to strut top), so the outer wheel's top leans into
the turn, and force feedback takes the caster trail from it. The hub frame's
strut line is solved with the hub's pitch so that camber, toe, gain and the
wheel's position at rest are unchanged (host `wheel_pose`: within 0.04 deg
and 1 mm on all 48 fronts).

| Cars | Caster (deg) | Trail (cm) | Camber at 20 deg of lock, outer / inner |
| --- | --- | --- | --- |
| Baiern CS / DS / GT III | 3.85 / 6.6 / 6.6 | 2.2 / 3.5 / 3.6 | -0.8 / +1.8, -2.1 / +2.4, -4.0 / +0.4 |
| SunStrip 1.5 / 1.8 / 2.2 / Racing | 2.0 (race 3.5) | 1.3 (race 2.1) | +0.3 / +1.7 (race -2.5 / -0.2) |
| Einvagen 110 GT / GTK / 140 GTA | 1.75 | 1.2 | -0.1 / +1.1 |
| Enula WRY / WRZ / WRS | 3.0 | 1.8 | -0.3 / +1.7 (WRS -0.5 / +1.5) |
| Focer RC 200 / 300 / WRC | 3.9 (WRC 5.0) | 2.2 (WRC 2.8) | -0.6 / +2.0 (WRC -2.6 / +0.8) |
| MC GT / GTB / GTLE | 3.0 | 1.8 | 0.0 / +2.0 |
| Nonus Street GT / GT II | 5.5 / 7.0 | 3.0 / 3.8 | -1.9 / +1.8, -4.2 / +0.5 |
| Prime DLH 500 | 5.0 | 2.8 | -1.7 / +1.8 |
| SuperDuty 500 / 750 | 3.4 | 2.0 | +0.1 / +2.4 |

**Tyres.** The friction was the compound times 0.5 + 0.5 x width / 275, so
width alone added 32 % between a 195 and a 345; low-profile tyres sat at the
stiffness clamp of 40; rolling resistance was 0.004. Now:

- peak friction per compound, warm, at the nominal load: street 1.00, sport
  1.10, soft racing 1.30, drag radial 1.05 sideways and 1.42 lengthwise;
- nominal load 4000 N x width / 205 mm (x pressure^0.25): a wider tyre loses
  less grip to load sensitivity instead of having more friction;
- cornering stiffness 17.68 x (0.45 / aspect ratio)^0.25 x carcass (street
  0.95, sport 1, soft 1.15, drag radial 0.75) x rim fit;
- rolling resistance 0.010 / 0.011 / 0.013 / 0.015 / sqrt(pressure ratio);
- relaxation length 0.3 m x sqrt(sidewall / 113 mm / pressure ratio).

| Tyre | Friction | Nominal load (N) | Stiffness | Relaxation |
| --- | --- | --- | --- | --- |
| 195/50 R16 sport | 1.13 -> 1.10 | 3146 -> 3805 | 21.1 -> 16.8 | 0.28 m |
| 205/55 R17 sport | 1.15 -> 1.10 | 3496 -> 4000 | 17.7 -> 16.7 | 0.30 m |
| 245/45 R17 sport | 1.25 -> 1.10 | 4600 -> 4780 | 25.3 -> 17.4 | 0.30 m |
| 255/25 R19 soft | 1.37 -> 1.30 | 5218 -> 4976 | 40 -> 23.6 | 0.23 m |
| 295/20 R19 soft | 1.48 -> 1.30 | 6206 -> 5756 | 40 -> 24.9 | 0.22 m |
| 405/40 R15 drag radial | 2.05 -> 1.05 (x1.35 lengthwise) | 23728 -> 7902 | 27.2 -> 12.8 | 0.36 m |

Stock tyre sizes are unchanged. Cars already bought keep their parts, which
take the new values when they load. Details in
[docs/vehicles/notes.md](../docs/vehicles/notes.md#tyres-and-suspension-geometry-450).

## Mod settings and keys (#488)

Install `system/Scripts/openslrr` and the updated `ControlSet`, `OsdxOptions`
and `Track` classes together. **Options → Mods** lists settings registered by
mods; **Controls → Game controls** lists their rebindable actions. The **Game**
page offers Classic and the installed camera providers. Mod settings are saved
in `save/game/openslrr_options.json`, separate from the stock binary options.
See the [scripting API](../docs/scripting/mod-api-keys-settings.md) for examples.

## Java natives

| Class and signature | Behavior |
| --- | --- |
| `java.lang.Math.pow(FF)F` | a to the power b, declared by the patched `Math` class (SLRR 2.3.1 has it); 0 when the result is undefined or infinite (#450). |
| `java.lang.Math.sin/cos/tan/asin/acos/atan/exp/log/wrapAngle(F)F`, `atan2(FF)F` | Trigonometry in radians, exponential, natural logarithm and an angle brought back to [-PI, PI], declared by the patched `Math` class (#463). 0 instead of a NaN or an infinity; `asin`/`acos` clamp their input to [-1, 1], `log` returns 0 for a <= 0. The same class adds the constants `PI`, `HALF_PI`, `TWO_PI`, `DEG2RAD`, `RAD2DEG`, `E` and the script helpers `abs`, `sign`, `min`, `max`, `clamp`, `lerp`, `randomRange`, `randomInt`. |
| `java.game.parts.WheelRef.setRelaxation(F)V` | Sets the tyre's lateral relaxation length in metres (clamped to 0.05–2; 0.3 by default). |
| `java.game.parts.WheelRef.setTyreSpec(F)V` | Tyre compound for its temperature and grip: `Tyre.tcSTREET` 0, `tcSPORT` 1, `tcSUPERSPORT` 2, `tcDRAGRADIAL` 3 (#446). |
| `java.game.parts.WheelRef.setBrakeSpec(FFF)V` | Brake hardware: pad compound (0 street, 1 fast road, 2 race), rotor type (0 solid, 1 grooved, 2 vented, 3 carbon) and rotor diameter in metres (clamped to 0.15–0.5). A positive diameter makes the car's brakes physical (#446). |
| `java.game.OptionsDialog.getRendererOption(I)I` | Reads the active value for the requested identifier. An unknown identifier returns 0. |
| `java.game.OptionsDialog.setRendererOption(II)V` | Applies the value to the session. Unknown identifiers are ignored. |
| `java.openslrr.ModSettings.getInt(Ljava.lang.String;I)I`, `getFloat(Ljava.lang.String;F)F`, `getString(Ljava.lang.String;Ljava.lang.String;)Ljava.lang.String;` | A mod setting from the `mods` section of `save/game/openslrr_options.json`, or the given default when the key is missing or holds another type (#488). |
| `java.openslrr.ModSettings.setInt(Ljava.lang.String;I)V`, `setFloat(Ljava.lang.String;F)V`, `setString(Ljava.lang.String;Ljava.lang.String;)V` | Stores a mod setting; the file is written 500 ms after the last change, at shutdown, or before options reload (#488). A non-finite float is ignored. |
| `java.io.ControlSet.bytesLeft(Ljava.io.File;)I` | Bytes left to read in an open file, so the patched ControlSet reads its optional MKEY mod key trailer only when one is there (#488). |
| `java.openslrr.*` (`OpenSLRR`, `CarState`, `CameraControl`) | The mod API natives (#487); see [docs/scripting/mod-api.md](../docs/scripting/mod-api.md). |

| ID | Option | Values |
| --- | --- | --- |
| 0 | Anti-aliasing | 0 off, 1 MSAA 2×, 2 MSAA 4×, 3 MSAA 8×. Other values are clamped to this range. |
| 1 | Dynamic reflections | 0 off, any non-zero value on |
| 2 | Volumetric clouds | 0 off, any non-zero value on |
| 3 | Scenery sunlight | 0 off, any non-zero value on |
| 4 | Local lights | 0 off, any non-zero value on |
| 5 | Removed (was Framerate adaptive behaviour) | Unknown id: reads 0, writes are ignored |
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
| 32 | Temperatures | 0 off, 1 on (default): the brake and tyre temperature panel above the gauges (#446) |

The menu and ImGui share all of these options. They are saved in `save/game/openslrr_options.json` and persist across launches. Local lights (4) are independent of scenery sunlight (3): turning sunlight off keeps the garage lamps and their shadows. Resolution, the global shadow toggle, and flares use the game's existing configuration.

AO affects indirect diffuse light in the main world view. It leaves direct sunlight,
lamps, emission, reflections and UI unchanged. All AO qualities run at half
resolution; high takes more samples per pixel. AO is off by default to preserve the existing frame cost.
Higher shadow quality increases depth resolution and memory; smoothing changes
comparison filtering independently. Start with medium quality and soft edges.

## Framerate

Cars are always drawn between their last two 60 Hz host steps, so they move smoothly at any frame rate; the drawn car is up to one step (16.7 ms) behind the simulation. The **Framerate Adaptive Behaviour** option and its Classic (60 Hz) mode are gone. An old `framerate_mode` key in `save/game/openslrr_options.json` is ignored.

## Change-view key (#470)

Press **V** to cycle follow → chase → each cockpit camera → follow. Without a second target, V skips chase and goes straight to the cockpit. The cycle skips mirror and look-back cameras; holding LOOK BACK pauses the cycle until release. A short message names each view. Rebind V under **Options → Controls → Game controls → CHANGE VIEW**. The former F1–F5/F9 view hotkeys are gone. TV, reverse chase and free view no longer have a key; race scripts can still select their own cameras.

On load, a stock F1/F2 pair in slots 35/36 migrates to V. Stock F2 is cleared from the hidden slot 36 even if slot 35 was already rebound or cleared. A later F1 binding stays on F1. The hidden F5 look-back binding (slot 37) is also cleared; LOOK BACK defaults to B.

This requires the patched `Track.class`, `OsdxOptions.class` and `system/Scripts/io/ControlSet.class`. A mod that replaces the MWM `Track.class` also replaces this cycle.

**Camera mods (#487).** A mod registers a camera provider (`java.openslrr.CameraProvider`, see [docs/scripting/mod-api.md](../docs/scripting/mod-api.md)). While one is selected it replaces the follow view: V cycles its views and LOOK BACK goes to it; any other camera (cockpit, race staging, TV) takes over until V brings it back. Without a camera mod nothing changes. This needs the patched `Track.class`, `City.class`, `ROCTrack.class` and the `system/Scripts/openslrr` classes.

### Track.class stock baseline

`Track.java` keeps what the installed MWM 2.2.1 `Track.class` does
(76,042 bytes, SHA-256 `ADF4219FDDBE…`), not what `sources/sl/Scripts/game/src/Track.java`
does. Decoding the stock class and the unmodified source build with the repo's
TUFA reader (`src/jvm/tufa.cpp`) shows the same 138 members, including 32 methods.
With MotionBlur declarations available, only `enter` and `exit` differ in
instructions: 1,444 vs 1,457 nodes and 413 vs 421 nodes, respectively.

| In the installed class | In the MWM source |
| --- | --- |
| `enter` calls `GameLogic.autoSaveQuiet()` after `t.setPriority` | the call is commented out |
| no `Refresher_Start` / `Refresher_Stop` strings | `enter` / `exit` call `player.car.chassis.Refresher_Start/Stop()`; the installed `Chassis.class` (35 methods) has neither method |
| no `MW_Mod:0x0079r` or `"egbolt-dawn_02"` | `enter` creates `Test_Render` every time it runs and never destroys it |

The patched source now follows the binary: the save is restored and the
Refresher and Test_Render calls are removed. Blank lines are adjusted so the
source line numbers recorded in the trees match. Remove the #470 additions
(`CMD_NEXTCAM`, the V hotkey, `nextCamera` and its `CMD_NEXTCAM` dispatch)
and put back the six F-key hotkeys. That source then compiles to the installed
class byte for byte, apart from the compiler build stamp in header bytes 9-11
(`cmp -l a b | awk '$1>11'` prints nothing). With the #470 additions, the only
member changes are the added field `CMD_NEXTCAM` and the added method `nextCamera`.

To rebuild, copy the stock game into a private root, never the installed game. Put `Track.java` in
`sl/Scripts/game/src`, then copy `sources/sl/Scripts/game/src/MotionBlur.java`
into a source folder outside the compiled directory, for example
`sl/Scripts/mb/src`. Then run
`compile_game_sources.exe <root> sl/Scripts/game`. The game ships no
`MotionBlur.class`. The compiler reads that source only for its declarations
and does not compile it. Without it, the nine `MB_Animator.changeCam(renderer)`
calls compile to a by-name call instead of the stock method reference
`MotionBlur.changeCam(GameRef)`. The result is one unit, float mode A (auto),
and only the existing `W_UNVERIFIED` (final method) and `W_PACK`
(`cars.traffic.chevy_van`) warnings.

## Transmission and clutch

**Options → Game** shows the gearbox and the clutch as two choices: **Transmission** (Auto, Semi-auto, Manual) and **Clutch** (Auto, Manual). They are saved together in the stock `Config.player_transmission`, which has no automatic gearbox with a pedal clutch: choosing a manual clutch moves the gearbox to Manual, and another gearbox shows the clutch as Auto until Manual comes back.

## Handbrake

The handbrake is analog: bound to an axis (a handbrake lever, a pedal), it brakes in proportion to its travel, up to the force a key gives. **Options → Controls** shows it in **Live input** and has a **Handbrake dead zone**. The dead zone sliders now apply to the keyboard, pad and wheel bindings of their control (#405 added the pad and wheel slots, which they missed), and the steering one to both steering halves.

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
