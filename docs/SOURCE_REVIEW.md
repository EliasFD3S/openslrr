# Open SLRR consolidated source review

Review baseline: 60d7d710e2a47b178f1c0b3a31bdffa979e9e9d2 (main, 10 November 2026). Reviews were read-only; workspace instructions, build tooling, documentation, and dependencies were prepared separately by the coordinator.

## Scope and confidence

Six parallel reviewers covered every C++ host module and public header: VM/boot, resources/natives, rendering/UI/platform, physics, build/tooling, and recovered Java contracts. All 1,468 MWM and 1,451 vanilla Java paths were inventoried and hashed, with full-tree native/persistence/lifecycle scans and deep review of central runtime, game, transaction, race, inventory, and save/load classes. Individual car/part Java files and third-party dependencies were not all reviewed line by line.

Confirmed means the defect follows from source inspection; it does not mean a runtime exploit or stock gameplay reproduction was executed. Conditional lifetime, race, original-engine parity, and audiovisual items require dynamic validation.

## Executive assessment

The project has a substantial working reimplementation: stock scripts, cooperative fibers, safe-point GC, resource formats, rendering, garage/vehicle assembly, input/audio/video, and fixed-step vehicle simulation are connected. The fiber scheduler and core mark/sweep/finalizer design are internally coherent.

The immediate constraint is not the 63 missing native signatures. It is memory-unsafe boundary code, destructive persistence, incomplete ownership, and absent committed regression tests. Native counts (307 bodies, 6 no-ops, 63 missing) measure inventory only. Follow ROADMAP.md: safety and reproducibility, then VM/lifetime, driving/collision, career, city, and optional systems.

## P0 — Fix before broader feature work

| Area | Evidence and impact | Required gate |
| --- | --- | --- |
| Java array native | src/natives/natives_io.cpp:432-451 computes dst_pos + length in signed 32-bit. INT_MAX + 1 can bypass resize and write outside destination slots. | Checked arithmetic plus negative/overflow/overlap/exact-boundary regressions under a memory checker. |
| PTX parser | src/resources/ptx_image.cpp:63-80 permits wrapped rgb_size + alpha_size, reads payload[1] without a two-byte guarantee, and can derive an out-of-file alpha pointer. | Checked remaining-size windows, JPEG-length checks, malformed/truncated/fuzz fixtures, valid-stock regression. |
| DirectShow frame copy | src/platform/video_fmv.cpp:459-481 sizes scratch from sample length but copies media-declared dimensions without need <= sz, pitch/orientation, dimension, or overflow validation. | Mocked short/mismatched/top-down/huge-frame tests; then RAII and explicit LibVLC callback lifetime. |
| Career durability | MWM GameLogic.java:821-827,918-927 deletes old saves before replacement and reports success without checking durability. CarLot.java:247-255 also deletes first; host File.close cannot communicate failure (natives_io.cpp:604-639). | Atomic temporary-write/flush/replace/backup with fault injection; previous career survives every failure. |

## P1 — High-priority correctness and lifecycle findings

### VM, boot, and interpreter

- Numeric UB: direct signed add/sub/mul, INT_MIN negation/division, and unchecked float-to-int conversions occur in src/jvm/tree_eval.cpp:149-153,361-368,818,1027-1032,1626-1630,1811-1820 and src/jvm/jvm.cpp:325-328. Define wrapping and NaN/range conversion centrally.
- Array semantics/OOM: tree_eval.cpp:765-774,1162-1169 grows slots to arbitrary script indices; allocation clamps invalid sizes at :930-947 and invalid reads return null at :1636-1642. Specify bounds/errors and allocation ceilings.
- Duplicate initial section entry: Java Init already enters SplashScreen. src/boot/boot.cpp:334-336 enters it again through section_machine.cpp:171-174, duplicating OSD/timer side effects. Adopt the state and assert one enter.
- Silent loop fallthrough: tree_eval.cpp:743-753 stops taking a backward branch after its guard trips without failing or yielding.
- Comparison/remainder: float != collapses through integer conversion at :314-345,1767-1793. It is an MWM protection workaround but breaks legitimate comparisons. The value dispatcher omits remainder at :1816-1824. Use compatibility-scoped differential tests.
- TUFA exhaustion: src/jvm/tufa.cpp:84-163 reserves declared counts before proving records exist; :317-320 does not check tellg failure.
- Conditional reentrancy: src/boot/mainloop.cpp:320-332 emits arbitrary script before guarding mechanic pointer dispatch and then uses a clearable global. Retain/revalidate the target and install guards first.

### Resources, natives, and persistence

- Decal/resource round-trip: src/natives/natives_resref.cpp:469-487 stores only _texpath; stock Catalog purchase/copy expects a live type-7 ID. SDAT unsaved blobs and abs-ID remapping in natives_io.cpp:201-260 cannot reconstruct it. Test purchase, copy, save, relaunch, load and every embedded resource kind.
- RPK allocation/ranges: src/resources/rpk_index.cpp:155-199 reserves unbounded TOC counts and accepts partial tables; :344-368 allocates payload size before proving file range.
- Filesystem confinement: src/natives/natives_boot.cpp:48-66 uses lexical prefixes; FindFile bypasses it, and natives_io.cpp:503-520 permits recursive root deletion. Use canonical component-aware policy, junction tests, and isolated destructive fixtures.
- Recursive scene work: src/resources/scene_instances.cpp:391-521 has depth but no visited/total-work budget.
- Malformed saves: GameLogic.java:930-1024 erases live state before structural validation, accepts unchecked rank/counts, and continues after bad headers. Part.java:126-151 waits for a sentinel while host readInt returns zero without advancing at EOF (natives_io.cpp:705-711). Prevalidate and budget structures before mutation.
- Reachable missing natives: Painter doPaint, paintPart, xPaint and Vehicle getHorn, hasCrime have live callers but no registration; NativeRegistry.invoke throws. Implement behavior, not empty stubs.
- Root-scoped cache drift: CFG, mesh-span, and shadow caches survive root changes (script_cfg.cpp:138-183, part_runtime.cpp:629-657, scene_instances.cpp:773-813).

### Physics and vehicle integration

- Collision depends on powertrain: src/resources/part_runtime.cpp:2632-2636 gates wall collision and all impact damage on drive.torque; invalid/missing dyno or ratios disables them.
- Dyno ownership leak: src/physics/dyno_model.cpp:650-679,735-737 strongly roots every session key, preventing the finalizer that removes the session. Failed first calculations also leave rooted entries.
- Dyno sampling: dyno_model.cpp:630-641 drops the highest computed RPM sample; :587-619 can retain old nitro arrays sampled with a new primary RPM step.
- Sleep/reset: src/natives/natives_gameref.cpp:313-349 parks but does not clear velocity, momentum, or drivetrain state despite the stock Vehicle.setMatrix stop contract.
- Numeric input: dyno conversions can occur before bounds checks; wheel and matrix natives accept NaN/Inf, which can reach throwing rigid-body operations without containment.
- Collision completeness: no active-car pair solver or detached-panel damage path exists. Nitrous, torque multipliers, aerodynamic drag, contact-patch/instant-center settings, several tyre coefficients, and MWM wheels 5-8 are not fully consumed.
- Determinism: excess stall time is discarded (part_runtime.cpp:2727-2735), while unordered entity/panel iteration affects accumulation order. Define lag and stable-order policies before claiming replay parity.

### Rendering, UI, and platform

- Dead OSD retention: src/ui/osd_registry.cpp:138-143,183-192,1039-1045 marks widgets dead but retains and GC-roots every record.
- Backend reopen: src/render/bgfx_lib_backend.cpp:250-385,594-598 preserves shown state and potentially an old-lifetime FMV texture; reopened windows can stay hidden and handles can be invalid.
- Fallback veil: src/render/bgfx_osd.cpp:709-715 sets dark tint, then the quad helper overwrites it with white.
- Partial XInput profile: src/platform/host_input.cpp:101-111 disables all fallback pad axes when any binding uses the pad instead of checking the specific axis.
- Media lifetime: LibVLC stop/release versus decoder callback ownership needs a proven join/generation protocol. COM media-type leaks, top-down RGB orientation, ignored Run failure, cursor cleanup, and NaN voice values are additional issues.

### Stock Java behavior and compatibility policy

- MWM sell-all: Mechanic.java:581-586,623-627 prices every item using element 1, then removes all and credits the repeated total.
- Used-car sale: CarMarket.java:609-632 pays before finding a destination; a full dealer can allow repeated payout while the player retains the car.
- Full-lot pink slips: City.java:1237-1242,1731-1736 awards without capacity checks, leading to CarLot.java:310-325 setting/indexing curcar=-1.
- MWM driveability: MWM Vehicle.isDriveable2 removed vanilla driven-wheel validation, allowing no-driven-wheel configurations through track/race entry.
- These are stock-script defects or regressions, not reasons to edit the read-only reference tree. Select a versioned compatibility-patch or host-safeguard policy and test money/ownership/capacity invariants.

## P2 and conditional findings

- Initializer depth is not RAII-safe; nested modal pumps have no GC safe point; boot callbacks/root providers are not fully scoped for a second boot; class initialization can be observed mid-enrichment.
- Ground POLY geometry lacks computed bounds, undermining picking (scene_instances.cpp:1340-1399,1621-1643). DDS/SCX numeric and index validation is inconsistent.
- Inventory self-drop/null-Set folder serialization are latent: no production Inventory.drop caller was found and catalog folders have a Set. CarLot async cursor events are dropped because GameType.handleEventAsync enqueues no event. Config has destructive flag behavior and a BgScene int/float mismatch.
- The non-libbgfx fallback has duplicate camera definitions and missing declared functions; the active target does not exercise it. shaders/varying.def.sc declares v_worldPos twice and needs a shader-clean-build check.
- Original numerical suspension/tire constants and timer watcher ordering require native differential evidence; they are not declared defects from Java alone.

## Build, test, and release readiness

- At the review baseline, no committed first-party test tree, CTest registration, CI workflow, or reproducible version of the documented temporary VM/audio/UI diagnostics existed. Shared CTest infrastructure, fixture helpers, smoke tests, and Windows CI have since been integrated; production regression coverage remains active work.
- README.md refers to ignored/missing tools/render-native-coverage.py; NATIVE_AUDIT.md depends on external docs/stubs.txt. Coverage cannot be regenerated from a clean checkout.
- validate-and-promote.ps1 builds and copies but runs no behavioral gate. Its source list omits required inputs, now includes downloaded tools, snapshots sources after building without a clean/lock check, and overwrites Release before verification. It needs canonical paths, fresh-parent creation, unique checkpoints, and pinned CMake selection.
- The new multi-agent scripts must be integrated into a commit before agent worktrees are created; the base guard intentionally refuses a base lacking them.

## Multi-agent workspace prepared

- Root and repository AGENTS.md files define read-only reference data, build rules, subsystem ownership, and integration policy.
- CMakePresets.json standardizes VS2022 x64 Release configure/build.
- scripts/Initialize-SlrrWorkspace.ps1 provides checksum-pinned project-local CMake 3.31.8 and pinned clean bgfx setup, with correct worktree build routing.
- scripts/New-SlrrAgentWorktree.ps1 creates isolated agent branches/worktrees and refuses unusable uncommitted bases.
- ARCHITECTURE.md, MULTIAGENT.md, and ROADMAP.md provide the architecture, task-packet protocol, and phased execution plan.

## Validation record

Static review and script syntax/diff/worktree-base-guard checks passed. The pinned bgfx checkout is clean at 5ea74ee66662daef4089695846d6322da0dde94e.

The corrected project-local bootstrap configured successfully with CMake 3.31.8 and Visual Studio 2022 x64. The full Release host and all shaders built successfully; warnings observed were in third-party code. Binary: build-bgfx-x64/Release/slrr_host.exe, SHA-256 1B2D0D06DBC58CCB739563FB86F4F79D2AA808A0D9AB1E7C729C3DF6384AB47E.

Non-interactive results: --help exit 0; --probe-gc --root ..\open-SL\game exit 0 (roots, cycles, arrays, pinning, resurrection, deferred collection and pressure); --probe-dyno exit 0; --probe-pacejka exit 0. These narrow smokes do not cover the newly identified edge cases and are not a substitute for the proposed regression suite.

No interactive rendering, audio, physical gamepad, original-engine telemetry, or gameplay parity is claimed by this review.
