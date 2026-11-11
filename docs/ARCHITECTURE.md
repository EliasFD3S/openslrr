# Architecture map

## Repository layout

The workspace combines the editable C++ engine with two reference installations. `open-SL/game` and `open-SL/sources` are the MWM variants; `game2` and `sources2` are vanilla. The C++ repository is the only ordinary development target.

The engine contains roughly 25,000 lines across 54 C++ implementation files and 46 project headers. The reference inventory contains system, game, car, and part Java sources; the car/part files are primarily data-facing declarations and configuration logic rather than independent host subsystems.

## Runtime flow

1. **Entry and startup** — `src/main.cpp` parses host options and selects the game root. `src/boot/boot.cpp` initializes the VM, resources, natives, renderer, audio, and stock script entry points.
2. **Section and frame orchestration** — `src/boot/section_machine.cpp`, `mainloop.cpp`, and `script_events.cpp` own section transitions, frame pumps, callback dispatch, and presentation holds during cooperative transitions.
3. **Script execution** — `src/jvm/tufa.cpp` decodes TUFA classes; `tree_eval.cpp` interprets TREE programs; `jvm.cpp` owns objects, invocation, native dispatch, and heap state; `script_threads.cpp` implements Windows-fiber cooperative execution; `gc.cpp` performs safe-point mark/sweep collection.
4. **Native boundary** — `src/natives/` registers script-callable functions and connects them to host services. Registration coverage is not behavioral coverage; the audit explicitly tracks no-ops and partial implementations.
5. **Resources and vehicle assembly** — `src/resources/` parses RPK, SCX, DDS, PTX, CFG, scene, and part data. `part_runtime.cpp` is the largest integration point and connects script parts, CFG state, installed assemblies, and runtime vehicle state.
6. **Presentation** — `src/render/` implements bgfx/D3D rendering, OSD, lighting, shadows, fonts, and debug UI. `src/ui/` maintains script OSD objects and hit testing. `src/platform/` provides Windows input, cursor, audio, music, options, and FMV.
7. **Simulation** — `src/physics/` contains fixed-step vehicle dynamics, tyres, suspension, dyno calculations, and deformation. World collision, route/navigation, traffic, and several effects remain incomplete according to the native audit.

## Principal dependency directions

- Boot depends on nearly every subsystem and should contain orchestration, not new subsystem policy.
- The JVM calls natives; natives call resource, UI, render, platform, and physics services.
- Java game scripts depend on native signatures and exact behavioral contracts. Use both MWM and vanilla callers to establish contracts.
- Resource and part runtime objects feed rendering and physics. Shared lifetime or root changes here require coordinator review.
- Rendering and platform code must not mutate reference data or implicitly change save files outside the established options/save workflows.

## High-risk integration points

- `tree_eval.cpp`, `jvm.cpp`, and `script_threads.cpp`: interpreter state, reentrant native calls, suspended stacks, cancellation, and GC roots.
- `part_runtime.cpp`: large shared mutable state spanning assembly, inventory, physics, rendering, and scripts.
- RPK/SCX/PTX/DDS parsers: untrusted sizes, offsets, overflow, allocation bounds, and truncated data.
- `bgfx_*`: CPU/GPU resource lifetime and frame retirement.
- `mainloop.cpp` and section transitions: nested modal pumps and reentrancy.
- `CMakeLists.txt`, shared headers, and native registration tables: frequent parallel-edit conflicts.

## Verification layers

The roadmap uses four separate evidence levels:

1. **Static**: focused regression/unit tests, malformed-input tests, and source audit.
2. **Headless**: stock Init, VM/GC/scheduler probes, resource load probes, and save/reload in isolated directories.
3. **Interactive**: garage, track, input, rendering, audio, and gameplay checks with reference MWM data.
4. **Comparative**: stock-versus-host telemetry and behavior at multiple render rates, with versions and commands recorded.

A native body, successful build, or headless probe alone does not establish complete gameplay behavior.
