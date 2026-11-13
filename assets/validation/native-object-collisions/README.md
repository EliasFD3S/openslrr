# Native-object collision validation

Validation target: PR #22. See the branch head for the exact final revision.

## Build and automated checks

- Visual Studio 2022 / CMake Release x64 host linked successfully.
- All 28 standalone tests passed with the Linux host compiler: 3 CFG parser
  (malformed shapes keep the body and its mass), 4 PhysicsRef/activation
  (including `setStatic(0)`), 15 collision-kernel (including box-triangle
  SAT), 2 mass-response and 4 trigger-volume (parsing, containment, enter and
  leave edges) regressions.
- The collision-kernel and trigger-volume tests also passed ASan/UBSan/leak
  detection.
- `python3 scripts/check_knowledge_refs.py`: 216 references checked, 0 errors.

## Live Valo City runs

The host used SLRR 2.2.1 MWM data. The two city map chains logged 19,849 native collision shapes (`collisionShapes=8113` and `collisionShapes=11736`).

- Full throttle: the car reached 62.243 km/h on the approach, hit the No Parking sign at **20.9475 m/s**, and dropped to **1.497 km/h** on the next state sample. See `final-head.stderr.log` for the final-revision trace.
- 35% throttle: the same sign produced a **10.7130 m/s** contact. The subsequent state sample was **0.553 km/h**. See `sign-contact.png`.
- The full-speed pass continued to a tree and stopped there. `tree-contact.png` shows the contact, and `tree-telemetry.png` records **0.5 km/h** with all four wheels still on the ground. The trimmed trace is `high-speed.stderr.log`.
- A separate pass with a 33 ms frame cap produced the same **20.9475 m/s** sign contact. Fixed-step simulation did not change the contact speed or the response. See `capped-33ms.stderr.log`.
- Performance traces are in `performance-uncapped.stderr.log` (20–37 FPS during the approach) and `performance-capped-33ms.stderr.log` (16–20 FPS under the cap). Collision-cache builds remained at four and the vehicle telemetry reported zero physics backlog throughout both runs.
- The final rebased revision reproduced the full-throttle sign contact exactly. The tree capture was made during the preceding high-speed validation pass; the later contract cleanup only removed render-mesh fallback for activated objects without CFG bodies and did not change native placement/contact behavior.
- A ground-level approach to `highwayoszlop` produced a **6.3890 m/s** column contact with a forward normal; see `column.stderr.log` and `column-contact.png`. The earlier approach above the column's top was not a valid overlap test.
- An 80 kg small native object moved from `(794.493, 2.444, 1250.013)` to `(794.470, 2.377, 1251.479)` with **3.876 m/s** velocity while the car continued accelerating; see `dynamic-response.stderr.log`. The object is removed from static ground and floor collision caches, sleeps before contact, and wakes for the car impulse.
- The hotel approach also stopped at its south face with a **5.3053 m/s** contact. Buildings and bridge columns remain static; only wholly positive-mass placements enter the loose-body path.

## Building coverage audit (rebased on main 60b143e)

The PR was rebased onto main 60b143e and rebuilt with Visual Studio 2022 x64 Release (validation-only trace hooks applied separately).

- Inventory of all loaded Valo City placements: 760 house/shop/hotel/garage placements, every one with at least one collision body (no placement without geometry, none promoted to a dynamic body). Per-type render-versus-collision bounds are in `building-coverage.txt` (140 native types).
- The only native types without any collision geometry are street lamps (`lamp_*`, `trafficlamp`), low shrubs (`fa03a/b/c`, 1.4 m), balcony and letter fascia (`erkely*`, 6-19 m above the street) and ambience emitters. Positive-mass props (`kuka_*`, `tabla_*`, `lada_*`, `pad`, `tuzcsap`) are the dynamic path.
- Row-shop types (`09x6_shop`, `12x6_uveges`, `08p5x4_shop`, `18x6_haromboltiv`, `22x4_pub`, `14x6_ketszintajtos`, ...) have a single thin facade body (0.1-2.6 m deep). Their models are facades too: the extra render depth is the pavement and fittings in front of the facade (up to 3.6 m) and shallow door and shutter recesses behind it, which the facade body closes. A drive at the back of a `14x6_ketszintajtos` placement started off the map and fell, so there is no drivable ground behind it. An earlier version of this report read the render depth as building volume behind the facade; that was wrong.
- In-game, a car at full throttle (19.4 m/s on approach) stopped at a thin box facade (`09x6_shop`, **8.06 m/s** contact, `storefront-box.stderr.log`) and at a thin SCX-mesh facade (`18x6_haromboltiv`, **8.32 m/s** contact, `storefront-mesh.stderr.log`). Suburban mesh houses (`krthaz11`, **5.20 m/s**) and the hotel boxes also block the car.

## Merge of PR #29, performance and wall contact (594e4b2)

PR #29 (city frame-rate fixes, `f73698d`) and main `1462d3c` were merged into the branch (`205bb16`).

- **Regression found:** with every positive-mass map prop (about 4,000 bins, boards, crates in Valo City) created as a live loose body at load, `205bb16` ran PR #29's benchmark drive at about 2.4 FPS with about 400 ms of physics per frame (`perf-205bb16.csv`).
- **Fix:** those placements now stay ordinary instanced props until the car's padded chassis bounds reach them, and sleeping loose bodies skip the per-part car test unless their bounding spheres can touch.
- **Benchmark** (PR #29's drive, VSync off, 1280×720, RTX 4060 Laptop GPU, i9-13905H, Windows 11): three alternating runs each of the PR #29 head `f73698d` and this branch's clean `594e4b2` (no validation hooks), `perf-f73698d-*.csv` and `perf-594e4b2-*.csv`. Over the first 8 simulated seconds, while both cars drive the same street: `f73698d` 190–287 FPS, 0.82–1.27 ms physics, 0.82–1.23 ms collection; `594e4b2` 191–267 FPS, 0.72–0.95 ms physics, 0.92–1.31 ms collection. GPU time is 3.0–3.3 ms in both. At about 9 s this branch's car stops at a tree trunk that `f73698d` drives through (`bench-*-t10.png`), so later seconds show different views. The opening frames (`bench-*-t00.png`) show the same scene and props; this branch also carries main's later changes, such as restored shadows, so the captures and draw counts are not identical.
- **Dormant props still move:** spawning on the stored 80 kg bin promoted it and three neighbours (`[validate-promote]`); the 80 kg bin and the 10 kg lid were pushed at up to 5.5 m/s and 4.9 m/s while the car accelerated (`prop-push.stderr.log`).
- **Chassis box against walls:** the four corner sphere probes leave a 1.1 m gap across the bumper. The chassis box is now also tested against the wall triangles of every static mesh. On the same build, the validation-only `SLRR_VALIDATION_NO_WALLS` toggle disables that test:
  - House corner at 45° (`krthaz11`): without the test the corner ended 1.45 m ahead of the car centre, about 0.6 m inside the bonnet; with it the car stopped after a 6.89 m/s contact with the corner at the front of the chassis box, 2.06 m ahead (`corner-nowalls.stderr.log`, `corner-walls.stderr.log`).
  - Picket fence (map collision): without the test the car slid 8 m along with the fence through its body (`fence-before.png`); with it the car stopped at the fence (`fence-after.png`, `fence-*.stderr.log`).
  - These four runs were made on `594e4b2` with the validation hooks and repeated on `62cba17` with the same results; the logs and captures in this folder are from `62cba17`.

## Remaining limits and code review (62cba17)

Main `9e5c179` was merged first (`265abad`). The runs below used `62cba17` with `validation-only.patch`, except the benchmark, which used the clean build.

- **Trigger events:** the stock garage triggers (`Valocity.java`, 6 m spheres) were created and reported as volumes. Driving out of the spawn trigger raised `EVENT_TRIGGER_OFF` and driving into garage 2 raised `EVENT_TRIGGER_ON`; each was delivered to the stock handler, which set `activeTrigger` to 0 and then 2 (`trigger.stderr.log`). On the working tree before the review fixes, the car stopped inside the trigger and the stock one-second check opened "This is a higher club's garage, you can't enter!" (`trigger-dialog.png`). On `62cba17` the car slides along the glancing wall there instead (see below) and leaves the trigger, which raises `EVENT_TRIGGER_OFF`.
- **Free PhysicsRef body:** a validation hook created a 1 m `PhysicsRef` box 3 m above the road and called the `setStatic(0)` path. It fell, came to rest (500 kg from the primitive density), was hit by the car at 7.8 m/s and pushed 3.8 m, then came to rest again (`free-body.stderr.log`).
- **Promoted props return to scenery:** on the benchmark drive, two bins were promoted as the car passed. With the validation-only settle distance set to 15 m (the product uses 40 m beyond car reach, which this drive does not reach), both settled back to static props once the car was far away (`settle.stderr.log`, `loose` back to 0). Bins that used to jitter at 0.1-0.3 m/s forever now fall asleep.
- **Facade-only shops:** the earlier limitation was wrong. The 12 stock row-shop types with one facade body have facade models: their extra render depth is the pavement in front (`facade-front.png`, where the car stops at the facade). Behind one there was no ground and the car fell off the map (`facade-back.png`, `facade-back.stderr.log`). A tried render-wall fallback changed nothing on the street side and was removed.
- **Glancing walls:** before the review fix, a car that began a tick already touching a wall, as one pressed along it does, was rewound almost to its start pose every tick. At garage 2 it stopped dead against a 16° glancing wall contact. It now slides along that wall and drives on at about 19 km/h (`glancing-wall.stderr.log`, where later ticks start 2-3 cm inside the wall; the result is the same with the analytic shapes tested raised or at full height).
- **Corner and fence regressions on `62cba17`:** with the wall test off, the `krthaz11` corner ended inside the bonnet and the car slid 8 m along the fence through its body. With it on, the car stopped at the corner (6.89 m/s contact) and at the fence (`fence-before.png`, `fence-after.png`, `corner-*.stderr.log`, `fence-*.stderr.log`).
- **Benchmark:** three alternating runs each of the clean `f73698d` and `62cba17` builds (`perf-f73698d-r*.csv`, `perf-62cba17-r*.csv`). In this session the GPU took about 10.5 ms per frame for both builds, against 3 ms in the earlier session, so both ran at 91-94 FPS. Over the first 8 s, `f73698d` measured 94 FPS with 10.55 ms GPU, 0.85-0.89 ms physics and 0.89-1.08 ms collection; `62cba17` measured 91-92 FPS with 10.88-10.95 ms GPU, 0.79-1.17 ms physics and 0.84-1.21 ms collection. The 0.35 ms of extra GPU time goes with 72 more draws per frame from main's changes since PR #29's base. At 10 s the car is stopped at a tree with parts knocked off (`bench-62cba17-t10.png`).

## Reproducibility and cleanup

`validation-only.patch` (applies to `62cba17`) documents the isolated validation-only startup, throttle, frame-cap and tracing hooks, the wall and analytic-shape toggles, the free-box spawn and the settle-distance override. They are not part of the product change. The patch entered the stock `Valocity` section directly because synthetic menu input was unreliable; physics and native-object collision code came from the PR revision.

Each host was closed after its run. For the earlier runs, the options, controls, career and temporary save directories were restored from the pre-run backup and compared byte-for-byte. The later runs left `save/game/options` and controls identical to that backup; other save folders now also hold careers created outside these runs, so they were not reset.

Sphere overlap is covered by the kernel tests, including exterior and inside contacts. The stock garage marker uses a `GameRef` trigger, and `SCRIPT_RUNTIME.md` explicitly notes that trigger-event producers are still missing; garage trigger activation is therefore outside this PR's validated scope.

Remaining limitations are unchanged: standalone dynamic `PhysicsRef` integration is not implemented, and oriented-shape contact is discrete with bounded sampling of longer movements rather than a general continuous manifold solver. Movable native placements reuse the loose-panel mesh solver; they are not a general dynamic triangle-mesh or trigger-event implementation.
