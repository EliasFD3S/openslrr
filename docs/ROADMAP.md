# Open SLRR engineering roadmap

Baseline reviewed: `60d7d71` on `main`, 10 November 2026. This plan prioritizes demonstrated risk and subsystem dependencies, not the number of registered natives. Detailed evidence is in [SOURCE_REVIEW.md](SOURCE_REVIEW.md); subsystem ownership is in [MULTIAGENT.md](MULTIAGENT.md).

## Delivery rules

- Every defect fix includes a focused regression test and an exact validation command.
- Keep reference MWM/vanilla sources and game data unchanged. If a stock Java defect needs a script-side correction, implement it as an explicitly reviewed compatibility patch or host safeguard, not an edit to the reference checkout.
- Do not call a milestone complete after compilation alone. Each milestone lists behavioral evidence.
- Native audit counts are inventory, not compatibility scores. Update the audit only from a reproducible registry comparison.
- Interactive checks use copied/isolated save directories. Never test malformed saves against a real career.

## Milestone 0 — Safe, reproducible foundation

### 0.1 Add the regression harness

Create a committed `tests/` target with fast Windows x64 tests and CTest integration. Start with pure parser/value tests, then VM/native tests with small fixtures. Add malformed/truncated inputs and sanitizer-supported jobs where the Windows toolchain permits. Add CI for configure, Release build, and tests.

**Integrated foundation:** shared CTest targets, fixture helpers, smoke tests, and a Windows CI workflow are now present. Production regression coverage and sanitizer-supported lanes remain open.

**Exit:** a clean checkout can bootstrap, build, and run the same tests with one command; test binaries do not require the full game installation.

### 0.2 Fix confirmed memory-safety defects

**Integrated:** System.arraycopy and PTX parsing are hardened with 15 focused regressions; the combined CTest suite passes. The DirectShow item below remains open.

- **Array copy:** `src/natives/natives_io.cpp:442-451` computes `dst_pos + length` in signed 32-bit arithmetic. Overflow can bypass resizing and index outside `dst->slots`. Use checked subtraction/size arithmetic, reject unrepresentable sizes, and preserve the intended stock copy/auto-grow semantics.
- **PTX levels:** `src/resources/ptx_image.cpp:63-80` validates a wrapped `rgb_size + alpha_size` and then derives an unchecked alpha pointer. Validate by subtraction in `size_t`, require enough bytes for both JPEG signatures, and bounds-check every payload window before pointer formation.
- **DirectShow frames:** `src/platform/video_fmv.cpp:459-481` sizes scratch from the sample length but copies `g_w * g_h * 4` without proving `need <= sz`, checking stride/orientation, or preventing multiplication overflow. Validate connected dimensions and every row against the actual sample before any read; add short-sample, top-down, huge-dimension, and codec-mismatch mocks.

Add regression cases for `INT_MAX`, wrapped totals, truncated headers/payloads, one-byte RGB segments, overlapping arrays, and ordinary valid inputs. Apply the same checked-window audit to RPK, SCX, DDS, CFG, and TUFA readers. In particular, `src/jvm/tufa.cpp:84-163` must validate declared constant/member/node counts against remaining bytes and explicit ceilings before `reserve`, and `:317-320` must handle `tellg` failure.

**Exit:** malicious and truncated fixtures fail without an out-of-bounds access or uncontrolled allocation; valid stock resources still load.

### 0.3 Close crash-on-reach native paths

Implement or safely contain `Painter.doPaint`, `paintPart`, and `xPaint`, which have live callers in `Painter.java:557-568,699,736-744` but no host registration. `NativeRegistry.invoke` throws on missing methods. Implement `Vehicle.getHorn` and `hasCrime` before enabling the city paths that call them. Do not register empty stubs merely to improve coverage.

**Exit:** painter actions and horn/crime queries execute without a missing-native exception, with behavior verified against stock scripts; any intentionally unsupported path reports a controlled diagnostic.

### 0.4 Make auditing reproducible

Restore the native registry comparison and coverage generator to tracked tooling (the README currently refers to an ignored/missing `tools/render-native-coverage.py`). Record the stock table source and version. Add a documentation/link check and `git diff --check` to CI.

**Exit:** another agent can regenerate `native-coverage.json` and the SVG and obtain the same result without local research-workspace files.

### 0.5 Confine filesystem effects and bound parser work

Replace lexical prefix checks in `src/natives/natives_boot.cpp:48-66` with canonical, path-component-aware containment that handles Windows junctions. Route `FindFile` through the same policy and reject `File.delete` targeting the root (`src/natives/natives_io.cpp:503-520`). Run destructive tests only in temporary isolated fixtures.

Validate RPK TOC counts against minimum record capacity and actual payload ranges before reserve/allocation (`src/resources/rpk_index.cpp:155-199,344-368`), reject partial TOCs, and bound recursive ground-node work with visited offsets and total budgets (`src/resources/scene_instances.cpp:391-521`). Apply consistent file/image ceilings and numeric checks to DDS, SCX, and TUFA.

**Exit:** traversal, absolute-path, junction, root-delete, huge-count, truncated-payload, and recursive-subtree fixtures fail closed without escaping the allowed root, deleting unrelated data, or exhausting memory/CPU.

### 0.6 Make promotion atomic and evidence-based

Before using `validate-and-promote.ps1` again, require a clean quiescent integration worktree, a promotion lock, passing tests/headless smoke checks, and a Git-derived source manifest captured before the build. Exclude `tools/`, build trees, and `third_party/` from source archives; record compiler/SDK/CMake options and dependency commits separately. Canonicalize candidate/release paths, create fresh output parents, use unique checkpoint IDs, and reject unapproved `dev` version provenance.

Stage and hash the candidate in the Release directory, then atomically replace with rollback instead of overwriting the last good executable before verification. Add Pester tests for source mutation during build, concurrent promotion, fresh staged trees, relative paths, running processes, checkpoint collisions, and copy/hash failure.

**Exit:** checkpoints reproduce the exact reviewed source/toolchain state, contain no downloaded toolchain, and an interrupted/failed promotion leaves the previous Release intact and runnable.

## Milestone 1 — Runtime and ownership correctness

### 1.1 VM, fibers, and GC

Resolve the VM review findings before expanding script behavior:

- **Integer semantics and allocation bounds:** direct signed arithmetic and `INT_MIN` division/negation in `src/jvm/tree_eval.cpp:361-368,1027-1032,1811-1820` invite C++ overflow UB; float-to-int conversions need defined NaN/range behavior. Array stores at `:765-774,1162-1169` can request attacker-controlled huge growth. Specify Java-compatible wrapping, checked conversions, bounds, and allocation failures before changing operators.
- **Float inequality:** `:314-345,1767-1793` collapses float `!=` to integer zero. Stock protection checks may depend on this compatibility hack, so replace it only behind a regression-tested policy that preserves required stock behavior without breaking legitimate float comparisons.
- **Control-flow conformance:** `src/jvm/tree_eval.cpp:743-753` silently falls through after a backward-jump budget instead of yielding or reporting an error, and the value dispatcher lacks the remainder operation implemented by the other evaluator path. Use explicit budget/yield behavior and differential tests across both dispatchers for every opcode.
- **Section entry:** Java `changeActiveSection` already enters the new state, while `src/boot/boot.cpp:334-336` routes it through `section_machine.cpp:171-174` and enters it again. Adopt an already-entered Java state instead of duplicating `SplashScreen.enter` OSD/timer side effects; assert enter-call counts in a regression.
- **Reentrancy and scoped lifetime:** install mechanic reentrancy guards before arbitrary listeners and retain/revalidate the dispatch target (`src/boot/mainloop.cpp:320-332`). Make initializer-depth counters RAII-safe, scope callback/root-provider teardown, and define class-initialization ownership plus a quiescent nested-modal GC safe point.
- **Integrated dyno lifecycle:** weak sessions plus in-flight RAII roots now permit finalization without losing live calculations; world teardown clears old sessions, recalculation rebuilds stale nitro state, and exact highest-sample lookup is covered by 7 focused regressions.
- **Dyno ownership and sampling:** `src/jvm/gc.cpp:56-93` roots dyno session keys while dyno cleanup depends on finalization. Replace permanent strong session roots with explicit ownership/weak cleanup semantics and test failed as well as successful calculations. In `src/physics/dyno_model.cpp:630-641`, cover the highest computed RPM sample and recalculation with changed table resolution; `:587-619` must not retain stale nitro arrays sampled using a new `rpm_step`.

Also prioritize suspended-stack rooting, cancellation, and nested modal pumps. Build deterministic tests for every TREE opcode family, reference casts, exceptions, reentrant natives, wait/notify, stop/suspend/resume, finalizer resurrection, and collection with suspended fibers.

**Exit:** repeated stock Init/frames/collections leave no unreachable dyno/session growth; cancellation and nested pumps preserve roots and unwind cleanly; no use-after-free occurs in stress runs.

### 1.2 Career and inventory persistence

The recovered MWM `GameLogic.save` deletes old career files before writing replacements (`GameLogic.java:821-827`), and `load` mutates live state and indexes racer data before full validation (`:953-1024`). Add atomic save transactions, temporary files, backup/restore, count/rank bounds, and full prevalidation before replacing live state. Decide explicitly whether this belongs in host File semantics or a versioned compatibility script patch.

Malformed part saves also need a termination guarantee: `Part.load` waits for a `-1` slot sentinel while host `File.readInt` returns zero without advancing at EOF (`parts/scripts/src/Part.java:126-151`, `src/natives/natives_io.cpp:705-711`). Define read failure semantics and a bounded format parser so truncation cannot hang a load.

As a lower-priority latent compatibility item, `Inventory.drop` permits `src == dst` and can move a new folder into itself (`Inventory.java:217-235`), while `InventoryItem_Folder.save` dereferences an unset `set` (`InventoryItem_Folder.java:17-24,135-138`). No production `Inventory.drop` caller was found, and ordinary catalog folders have a nonnull Set; cover these invariants without presenting them as current save blockers.

Runtime decal resources must round-trip through the same persistence work: `src/natives/natives_resref.cpp:469-487` records only `_texpath`, while catalog purchase/copy and career save expect a live type-7 resource ID. Reconcile `makeTexture` with SDAT embedded-resource serialization/remapping and test a stock Catalog purchase → copy → save → reload cycle.

**Exit:** interrupted/corrupt saves preserve the previous career and reject safely; malformed counts cannot allocate/index out of bounds; folder creation, self-drop, save, reload, and restart preserve a finite acyclic inventory; purchased decals retain their image and identity after reload.

### 1.3 Resource lifetime

Scope CFG/mesh/shadow caches to the active boot/root instead of process statics (`src/resources/script_cfg.cpp:138-183`, `part_runtime.cpp:629-657`, `scene_instances.cpp:773-813`), and prove two roots cannot reuse stale data. Audit caches and registries for who retains, releases, and roots each script object. Compact dead OSD records and remove their GC roots (`src/ui/osd_registry.cpp:138-143,183-192,1039-1045`) with a create/destroy soak test. Make renderer open/close/reopen reset window visibility and FMV texture state (`src/render/bgfx_lib_backend.cpp:250-385,594-598`), and prove LibVLC callbacks cannot outlive their owner. Fix the white fallback OSD veil (`src/render/bgfx_osd.cpp:709-715`) and per-axis XInput defaults (`src/platform/host_input.cpp:101-111`) with focused tests.

Exercise repeated section transitions, car assembly/disassembly, painter/catalog entry, renderer reopen, and FMV/music changes while tracking heap, GPU handles, voices, and sessions.

**Exit:** bounded memory/handle/widget counts over repeated cycles, safe renderer/media reopen, no stale callbacks after resource destruction, correct fallback tint, and unrelated default pad axes surviving a custom binding.

## Milestone 2 — Verified Test Track drive

### 2.1 Establish telemetry baselines

Create repeatable stock-versus-host scenarios for Nonus, Baiern, and Einvagen: idle, acceleration, braking, steering, gear shifts, suspension response, and collisions at several render rates. Record game/script versions, vehicle CFG, input trace, fixed-step state, and units. Define deterministic entity ordering and an explicit lag/time-warp policy: `src/resources/part_runtime.cpp:2727-2735` currently discards excess stall time, and unordered records/panels can change floating-point accumulation order.

### 2.2 Correct simulation dependencies and numerics

The physics review found chassis collision and damage gated by powertrain torque in `src/resources/part_runtime.cpp:2632-2636`. Decouple contact/damage from engine validity so a released or engine-less vehicle still collides. Make `GameRef.setMatrix` perform the documented sleep/reset of linear, angular, and drivetrain state (`src/natives/natives_gameref.cpp:313-349`). Validate finite/range-checked numeric inputs before casts or physics calls, bound synchronous dyno work, and contain per-vehicle failures instead of terminating the host. Verify differential, tyre, suspension, and deformation calculations against telemetry.

### 2.3 Unify collision and damage

Connect `PhysicsRef`, `GameRef.setActiveCollision`, and `GameRef.setState` to live world objects. Compute real ground POLY bounds for picking/collision (`src/resources/scene_instances.cpp:1340-1399,1621-1643`). Verify track barriers, props, ground contacts, deformation, panel detachment, wear, and regional HUD state. Emit collision/trigger notifications required by scripts.

Add active-car/active-car collision and detached-panel impact damage; approximate four-probe wall contact and filename-based panel eligibility remain documented limitations until stock witnesses justify changes.

After correctness gates, connect nitrous activation/consumption, dyno torque multipliers, aerodynamic drag, contact-patch/instant-center settings, and the full accepted tyre coefficients. Add explicit support or a controlled compatibility diagnostic for MWM configurations with more than four wheels.

**Exit:** the same input trace produces stable results across render partitions/stalls; engine-less and powered cars collide correctly; `setMatrix → start` has no stale momentum; acceleration/braking/suspension fall within documented tolerances; keyboard and physical XInput profiles survive relaunch.

## Milestone 3 — Complete garage and career loop

- Validate purchase, repair, sell, install/remove, tuning, paint, save/load, and restart as end-to-end scenarios.
- Define transaction invariants for stock economy edge cases: MWM sell-all indexes element 1 for every item (`Mechanic.java:581-586,623-627`); used-car sale can pay before finding a dealer destination (`CarMarket.java:609-632`); pink-slip rewards need a free-lot precondition before `CarLot.addCar` (`City.java:1237-1242,1731-1736`, `CarLot.java:310-325`). Payment, vehicle transfer, and capacity checks must be atomic.
- Restore async event payloads for CarLot and validate cursor delivery; audit `GameType.handleEventAsync` null-message behavior instead of silently dropping enabled events.
- Test driveability with no driven wheels and Config round-trips, including the `BgScene` int/float mismatch and `loadConfigEnabled=0` save behavior. Keep any stock-script correction in a versioned compatibility layer.
- Complete painter rendering/resource updates and validate the implemented resource-created sound path end to end.
- Verify installed-part and paint state after career save/reload and section transitions.
- Add animation, particles, flares, and fog after their resource/lifetime contracts are covered.

**Exit:** a career can buy, assemble, tune, paint, drive, save, reload, and sell a vehicle without missing natives, lost state, or unbounded resource growth.

## Milestone 4 — Navigation, races, and city

1. Parse and index road/route data; implement `GroundRef` route queries, `getNearestCross`, bone/route line functions, and `Navigator.updateNavigator`.
2. Validate GPS, route rendering, rear view, cruise control, race start/finish, and race-only event handlers.
3. Add traffic and pedestrians only after routes and collision are stable; then enable crime/horn behavior and police workflows.
4. Validate Valo City and race sections repeatedly for frame stability, lifetime, and save compatibility.

**Exit:** a complete race and city loop works from stock scripts with route-following AI, stable collision, correct navigation, and no missing event producers.

## Milestone 5 — Platform and optional systems

- Validate display-mode enumeration, live video-mode changes, resize behavior, and persistence.
- Controller smoothing/force behavior and general hotkey dispatch.
- Music shuffle/repeat semantics and remaining audio capabilities.
- Networking, `System.compileAll`, debugging, and other optional natives only when a supported workflow requires them.
- Document supported mods and add a redistribution/license review before publishing binaries or original assets.

## First parallel coding round

| Harness agent | Task | Writable scope | Dependency |
| --- | --- | --- | --- |
| Safety/resources | Arraycopy and PTX fixes + fixtures | Assigned native/PTX files and their tests | Test harness API agreed first |
| VM/runtime | Dyno ownership and highest-confidence VM defects | VM/dyno/session files and tests | Coordinator reviews GC contract |
| Platform safety | DirectShow checked frame copies and mocked media tests | Assigned FMV files and tests | No LibVLC changes in the same patch |
| Test/tooling | CTest target, malformed-input helpers, CI | `tests/`, `cmake/`, CI; CMake by assignment | No production-file edits |
| Java contracts | Stock call traces and expected painter/save behavior | Read-only; report/fixture specifications | No host edits |
| Physics research | Reproduce torque/collision gate and define telemetry fixture | Read-only initially; physics scope after test plan | Uses isolated game copies |

The coordinator owns shared headers, `CMakeLists.txt`, roadmap/audit updates, integration, and final validation. Start each coding agent in its own worktree using `scripts/New-SlrrAgentWorktree.ps1`.
