# Native-object collision validation

Validation target: PR #22. See the branch head for the exact final revision.

## Build and automated checks

- Visual Studio 2022 / CMake Release x64 host linked successfully.
- All 21 standalone tests passed with the Linux host compiler: 3 CFG parser,
  4 PhysicsRef/activation, 12 collision-kernel, and 2 mass-response regressions.
- The 12 collision-kernel tests also passed ASan/UBSan/leak detection.
- `python3 scripts/check_knowledge_refs.py`: 190 references checked, 0 errors.

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
- Several row-shop types (`09x6_shop`, `12x6_uveges`, `08p5x4_shop`, `18x6_haromboltiv`, `22x4_pub`, `14x6_ketszintajtos`, ...) are authored as a single thin facade body (0.1-0.9 m deep) over a 4-8 m deep render. Their side/back/roof volumes have no authored body in the CFG, so only the street face is solid. This is the stock data, not a loading failure.
- In-game, a car at full throttle (19.4 m/s on approach) stopped at a thin box facade (`09x6_shop`, **8.06 m/s** contact, `storefront-box.stderr.log`) and at a thin SCX-mesh facade (`18x6_haromboltiv`, **8.32 m/s** contact, `storefront-mesh.stderr.log`). Suburban mesh houses (`krthaz11`, **5.20 m/s**) and the hotel boxes also block the car.

## Reproducibility and cleanup

`validation-only.patch` documents the isolated validation-only startup, throttle, frame-cap, and tracing hooks. They are not part of the product change. The patch entered the stock `Valocity` section directly because synthetic menu input was unreliable; physics and native-object collision code came from the PR revision.

Each host was closed after its run. The original options, controls, career, and temporary save directories were restored from the pre-run backup and compared byte-for-byte.

Sphere overlap is covered by the kernel tests, including exterior and inside contacts. The stock garage marker uses a `GameRef` trigger, and `SCRIPT_RUNTIME.md` explicitly notes that trigger-event producers are still missing; garage trigger activation is therefore outside this PR's validated scope.

Remaining limitations are unchanged: standalone dynamic `PhysicsRef` integration is not implemented, and oriented-shape contact is discrete with bounded sampling of longer movements rather than a general continuous manifold solver. Movable native placements reuse the loose-panel mesh solver; they are not a general dynamic triangle-mesh or trigger-event implementation.
