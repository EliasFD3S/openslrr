# Vehicle dynamics validation (issue #42)

Before is `main` at 51e65b2. After is this branch. Both builds are Windows x64
Release (MSVC) with the same local validation hooks, which are not committed:
scripted inputs from the bench start, a 60 Hz telemetry log of the controlled
car, timed window captures, a tyre swap through `Tyre.SetupTyre`, a brake
balance override and steering help off. Game data: SLRR 2.2.1 MWM, stock Baiern
CoupeSport 2.5 (`--spawn-car cars/racers/Baiern.rpk:0x6`), fixed 1/60 s step
(`--bench-map`/`--bench-drive`).

| Image | Scenario | After revision |
| --- | --- | --- |
| `chart-roll.png`, `cmp-rollover.png` | Drag_Track: full throttle for 8 s, then throttle 0.3 and full left steer at 107 km/h (steering help as in the default options) | 4048185 |
| `chart-tyre.png` | Drag_Track: the same car with 195/50 street and 275/40 supersport tyres fitted by the tyre script on the same rim; full-throttle launch, then full brake | 4048185 |
| `chart-burn.png` | Drag_Track: full throttle with the brake pedal at 40 % and the brake balance at 25 % rear | 4048185 |
| `chart-wall.png` | Valo City bench (`--bench-drive 22`): the car held against a wall by full throttle after its crash, every tick | ddfa1eb (`collide_chassis` is identical at the branch head) |
| `bones-*-before-after.png` | Garage, body hidden, wheels forced 7 cm into bump (left) and droop (right) through `pose_wheels` | eee32bd |
| `cmp-flap-hit.png` | Valo City bench at about 58 km/h: a scripted 8 m/s impact through `damage_chassis` on the left door's outer face | 6d5f9cf |
| `cmp-wheel-loss.png` | Valo City bench, full throttle into a tree (74 km/h offset hit, then a 38 km/h hit on the next tree) | a988a74 |

## Measured

| Check | Before | After |
| --- | --- | --- |
| Full lock at 107 km/h | Trips on the kerb and rolls onto its roof | Slides over the kerb and hits the wall upright (roll under 3°) |
| No-assist full lock at 85 km/h, TopGearTrack | Stays upright | Stays upright, about 0.7 g lateral |
| 195/50 street tyre (script: friction 1.66, stiffness 22, load 5.6 kN), 0–2 s | 33.3 km/h, 3.9 m/s peak rear slip | 19.9 km/h, rear wheels spin at up to 13 m/s |
| 275/40 supersport tyre (friction 2.19, stiffness 17.5, load 12.3 kN), 0–2 s | 32.9 km/h | 32.7 km/h, 1.2 m/s peak rear slip |
| Stock tyre pressure | Every tyre at the 0.5 bar floor (stiffness 8) | 2.0 bar as scripted (stiffness 17.7) |
| Throttle + 40 % brake, brake balance 25 % rear | Engine held at 850 rpm, rear wheels locked | Engine at 5,800 rpm, rear wheels at 15.5 m/s while the car creeps at 3.8 m/s |
| Car pushed against a wall | ±7.6 m/s² longitudinal, ±2 m/s² vertical, alternating every tick | Below 0.01 m/s² |
| Parked on flat ground | 0.002 m/s² tick-to-tick | 0.001 m/s² |
| Spring and shock at ±7 cm travel | Whole mesh scaled 0.48–0.66 along its axis | Coils close up with a constant wire radius, the shock rod slides |
| Door hit at 8 m/s | Stays shut | Latch releases, swings to 62°, pushed shut and re-latched after 1.35 s |
| Erbilac trunk (`flap` limits -0.960 0) | Stays at 0° | Opens to 55° |
| 74 km/h + 38 km/h tree hits | Hood and headlight detach, all wheels stay on | Hood and headlight detach, the front-left rim and tyre break off; the corner rests on its hub |
| Baiern inertia (kg m²) | Box of the render bounds: 2421 / 2632 / 549 | Sum of the parts' bodies: 2193 / 2435 / 430 |

The street-tyre launch is slower because that tyre now loses grip under full
throttle in first gear; the old model gave both tyres the same launch.
