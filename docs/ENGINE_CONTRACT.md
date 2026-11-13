# OpenSLRR — engine contract and architecture guide for agents

Document status: 11 November 2026, branch `enhanced`.

This document explains how to work on the engine without breaking scripts,
resources, and mods. It describes the implementation in this repository.
It is not an exhaustive specification of the original executable.
The files and symbols cited here are the entry points for checking each
contract. Numeric values can change: reread the code before changing them
or presenting them as a guarantee.

## Contents

1. [Purpose and scope](#1-purpose-and-scope)
2. [Compatibility contract](#2-compatibility-contract)
3. [Repository map](#3-repository-map)
4. [Startup and game loop](#4-startup-and-game-loop)
5. [TUFA/TREE VM and natives](#5-tufatree-vm-and-natives)
6. [Threads, events, and dialogs](#6-threads-events-and-dialogs)
7. [Memory and lifetime](#7-memory-and-lifetime)
8. [Resources and identifiers](#8-resources-and-identifiers)
9. [Scene, maps, and native objects](#9-scene-maps-and-native-objects)
10. [Cars, CFG, and suspension](#10-cars-cfg-and-suspension)
11. [Physics and damage](#11-physics-and-damage)
12. [Enhanced rendering](#12-enhanced-rendering)
13. [Interface, input, and options](#13-interface-input-and-options)
14. [Audio and video](#14-audio-and-video)
15. [Saves and transitions](#15-saves-and-transitions)
16. [Working method and validation](#16-working-method-and-validation)
17. [Diagnosis by symptom](#17-diagnosis-by-symptom)
18. [Limits and references](#18-limits-and-references)

## 1. Purpose and scope

OpenSLRR is a C++20 reimplementation of the engine required to run the scripts
and data of Street Legal Racing: Redline. The current target is Windows
x64, with bgfx for rendering. The game data is not distributed here.
The main functional reference is SLRR 2.2.1 MWM.

Scripts control a large part of the game: menus, sections, the catalog,
assembly, settings, and the career. The host provides execution, natives,
resources, the interface, simulation, and platform services.

`enhanced` allows a modernized renderer: procedural sky, lighting, shadows,
and dynamic reflections. That visual freedom does not remove the lifetime,
data, and call contracts the scripts expect.

## 2. Compatibility contract

### Scripts are clients of the engine

A feature present in MWM can be implemented entirely by the scripts, with no
change to the original executable. Before adding an
`if (class_name == ...)` exception in the VM, follow the script and identify
the general primitive that is misread or missing.

The VM must not know a particular car, the MWM CarSelector, a menu name, or
a specific career step in order to execute an assignment, a call, a wait, or
a destruction correctly.

A fix normally belongs in the owning layer:

| Problem | Layer to inspect first |
| --- | --- |
| Wrong assignment receiver or missing opcode | TREE/JVM |
| Native signature not found | Native registry, inheritance, descriptor |
| Texture bound to the wrong pack | RPK/SDAT/CFG resolution |
| Part placed incorrectly | Slot frames and suspension geometry |
| Sound or event played twice | Event producers and fallback paths |
| Resource left on screen after destruction | Scene/UI/GPU lifetime |
| Window reverting to an old resolution | Options, Config, mode application |

### Java changes

The usual compatibility rule is to make the stock scripts work. Java changes
have been authorized for specific features, notably the catalog and
OptionsDialog. That authorization does not mean every VM bug may be worked
around in the scripts.

For a shipped Java change: keep the old lines as comments when asked, explain
the reason, compile into the appropriate game tree, and distribute the
required files in `patch/`. A file that exists only in `data/game/` is not
a reproducible patch. `data/` is local and stays out of the commit. The
engine repository is this folder.

### Do not confuse the evidence

A native declaration proves an API. A stock call reveals what it expects.
A disassembly can pin down its behavior. A third-party port proposes an
implementation. A function name that resembles the original does not prove
behavioral fidelity. Document the approximations.

## 3. Repository map

All of the following paths are relative to the repository root.

| Directory / file | Role |
| --- | --- |
| `src/main.cpp` | Arguments, startup selection, and diagnostics |
| `src/boot/boot.cpp` | Service assembly and game startup |
| `src/boot/mainloop.cpp` | Host update, input, events, camera |
| `src/boot/section_machine.cpp` | Active state and section transitions |
| `src/boot/script_events.cpp` | GameType notifications and callbacks |
| `src/jvm/` | TUFA, TREE interpretation, heap, GC, fibers |
| `src/natives/` | Adaptation of script APIs onto the C++ services |
| `src/resources/rpk_index.cpp` | Packs, dependencies, and resource resolution |
| `src/resources/gameref.cpp` | Instance graph and GameRef operations |
| `src/resources/scene_instances.cpp` | Scene collection, maps, placements, and collision queries |
| `src/resources/part_config.cpp` | CFG reading and local frames |
| `src/resources/part_runtime.cpp` | Part instances, assembly, and the physics link |
| `src/physics/` | Vehicle dynamics, tires, suspension, dyno, and deformation |
| `src/render/` | bgfx backend, lighting, shadows, OSD, and ImGui debug |
| `shaders/` | Shader sources compiled by the build |
| `src/ui/` | OSD registries and behavior |
| `src/platform/` | Input, options, cursor, effects, music, and FMV |
| `include/slrr/` | Structures and interfaces between these systems |
| `patch/` | Java changes shipped with the host |
| `build/` | Local build products; not a stable API |

`data/game`, `data/sources`, and `data/docs` are the local install and
reference sources. They are gitignored. Do not assume them in a fresh clone.

## 4. Startup and game loop

Startup begins in `main.cpp`, configures the services, opens the resources,
and loads the classes before the game runs. Read `boot.cpp` for the exact
order and the diagnostic paths.

Conceptual outline, without every intermediate call:

```text
Arguments / game root
        ↓
Platform services + RpkIndex + NativeRegistry + Jvm
        ↓
TUFA classes → Init / GameLogic → active section
        ↓
Host pump ↔ cooperative scripts ↔ events / input
        ↓
Part state and simulation → scene collection → bgfx → presentation
```

`mainloop_frame` includes, among other things, music, the options update,
sky time, the script scheduler, section adoption, messages, the controller,
hotkeys, callbacks, the camera, and timers. That order matters: do not move
an event delivery arbitrarily, and do not add a second pump for the same port.

Presentations requested from a load or a dialog can nest a host pump. Every
function called in that context must be checked for reentrancy: callbacks,
memory collection, and transitions must not run twice on a partially modified
state.

## 5. TUFA/TREE VM and natives

### What "Java" means here

This host does not embed a classic Java SE JVM. It loads the game's TUFA
classes and interprets their TREE programs. Do not automatically apply the
conventions of a standard JVM `.class` file or of Java SE bytecode.

Entry points: `tufa.cpp`, `tree_eval.cpp`, `jvm.cpp`, `native_registry.cpp`.
`types.hpp` defines `Value` with the tags `Void`, `Int`, `Float`, `Obj`.
Script integers are `int32_t`, even though the process is x64.

`Object` carries, among other things, the class name, the fields, string
contents, and array slots. An object's identity is not its contents.
Arrays have `is_array`, `slots`, and a script `length` field that must be kept
up to date.

### Resolution and signatures

A native is identified by class, name, and descriptor. Also check overloads,
inherited dispatch, and the receiver. A native registered with the wrong
descriptor can look missing even though its body exists.

For an `invoke_by_name miss`: record the real class, the name, the arguments,
and the available declarations before adding a method.
For `unsupported op`: inspect the TREE semantics and the callers. Do not
replace the instruction with a silent success return.

Statics belong to the VM class state. Do not replace a missing static field
with a constant local to one screen. Check class loading and initialization,
inherited access, and resource references stored in those fields.

### Host-specific conventions

`System.arraycopy` currently keeps an automatic destination-growth behavior
used by the host. The `native_arraycopy` helper validates ranges and
overflows, keeps overlapping copies, and updates `length`. Do not "normalize"
it to Java SE without analyzing the scripts and the existing regressions.

A native with a body is not necessarily complete. Distinguish, in the audits:
absent, explicit no-op, fixed/mock value, partial, implemented, and validated
in a defined scenario. See `NATIVE_AUDIT.md`.

## 6. Threads, events, and dialogs

Script threads are cooperative and implemented with Windows fibers. They keep
their interpreter stack and their C++ call stack across a sleep/wait.
They are not parallel Java workers. The VM stays on the host thread.

`start` schedules execution. `sleep` suspends until a deadline. `wait` waits
for a notification. `stop` must let scopes unwind before the stack is
released. A long C++ job inside a native does not become preemptive just
because the caller is a fiber.

GameType notifications and callbacks are separate registries.
The documented modes 8, 9, and 28 concern `control(float)`, `drive()`, and
`animate()`. Iteration must tolerate an unregister or a reregister during the
callback. See `SCRIPT_RUNTIME.md` for the flags and budgets.

The host keeps some fallback paths for scripts that are not active.
They must go quiet when a script thread is already consuming the port or
producing the same action. Otherwise: a logical double click, a repeated
hover, a looping sound, or a dialog that closes too early.

Modal contract:

- The active window owns its modal shield and its end of life.
- A click on a slider must deliver the adjustment event to the right object.
- A MessagePort wake does not mean the dialog is finished.
- Events must not pass through the dialog to the car.
- Closing one dialog must not remove another dialog's shield.

## 7. Memory and lifetime

`Jvm` owns the managed heap. The GC is mark/sweep and non-moving. It does
not replace resource destructors, cache invalidation, or GPU release.

An `Object*` stored in a C++ structure is not an automatic root.
A system that actually retains an object must expose its roots. Locals that
must survive a yield need `Jvm::RootScope`, or an equivalent protection
already provided by their call path.

Strong versus weak retention is part of the contract:

| Case | Expected property |
| --- | --- |
| Active section, live thread, registered callback | Explicit retention of the required object |
| Local suspended in a fiber | Stack root kept |
| Derived cache indexed by object | Must not make the object immortal without a reason |
| Resource destruction | Release of the associated registries, hooks, and data |
| World reset | No payload from the old world still usable |

The dyno illustrates weak retention: `g_sessions` no longer keeps every
DynoData alive. The calculation that is temporarily active is protected.
`dyno_forget` removes a session and `dyno_clear_sessions` cleans up a reset.
See `dyno_model.cpp`.

`System.gc()` requests a deferred collection. Safe points prevent collection
in the middle of an unsafe mutation. `disableGC`/`enableGC` are idempotent
protections, not a reference count. Finalizers and resurrection require a
second trace. See `VM_GC.md`.

Note: the Java graph, the resource graph, part registries, the CPU geometry
cache, and GPU handles have different owners. An FPS leak after scrolling
through vehicles is not necessarily fixed by `System.gc()`.

## 8. Resources and identifiers

### Three spaces to keep apart

1. Local identifier inside a pack.
2. Serialized reference in a pack payload, using that pack's dependencies.
3. Host runtime identifier: `(pack_id << 16) | (local & 0xffff)`.

The serialized dependency number is not the pack number assigned by the host.
Use `RpkIndex::resolve_embedded_id(owner_pack, rid)` for embedded references.
The owning pack is essential: resolving against the caller's pack can produce
a resource that is valid but wrong.

Moving to x64 does not change the 32-bit file formats. Never serialize a
native pointer into a resource-ID field or a script integer.
Preserve signed/unsigned conversions: the high bit of a RID can be in use
while a script argument is represented as `int32_t`.

### Load chain

```text
RPK / dependencies → RSD entry and payload → render resource / mesh
                                              ↓
                             SCX + texture list in RSD order
                                              ↓
                             DDS / PTX → CPU geometry and materials → GPU
```

The order of repeated `texture` keys is significant. Do not put them in a
container that drops order, and do not deduplicate slots blindly.
A missing texture is not always a decoder problem: check the owner pack,
the dependency, the slot, the path, and the material first.

PTX files contain JPEG RGB data and optionally alpha, with mip levels.
Sizes and offsets must be bounded before any read, including the two bytes
of the JPEG markers. Arithmetic checks must not depend on 32-bit wraparound.
See `ptx_image.cpp`.

## 9. Scene, maps, and native objects

`ResState` and `GameRefState` represent instances, their identities, parents,
and children. A type resource is not its live instance. A script alias and
reference wrappers do not justify duplicating the underlying object.

`scene_instances_collect` produces the instances to draw for the selected
world. Geometry can be shared. Pose, color, and texture replacements are
instance properties. Avoid modifying shared geometry to fix a single car.

A map is not necessarily one SCX. `GroundMapData` distinguishes render and
collision meshes, exterior packs, and placements. Valo City requires resolving
map chains and their placed objects.

"Native object" on a map means world objects that come from the engine's
descriptions and placements. It does not necessarily mean a new Java native
to register. Inspect the data and the instantiation path first.

Keep these decisions separate: loading, CPU residency, GPU upload,
visibility/frustum, shadow participation, and collision. An object outside
the screen can cast a visible shadow. Render culling must not remove the
physical ground. A future Hi-Z strategy does not replace resolving missing
placements.

Ground, picking, and sweep queries use the scene index. The garage's
preloaded floor has a specific lifetime and must not serve as the floor of
the dealer or of a track. Floor revisions invalidate derived data without
rebuilding the whole index every frame.

## 10. Cars, CFG, and suspension

### Units and frames

| Data | Convention to preserve |
| --- | --- |
| Physics and script/CFG positions | Metres, kilograms, seconds |
| CFG poses | Yaw/pitch/roll rotation in radians in `PartPose` |
| Source SCX geometry | Scale conversion handled by loading; source is in centimetres |
| `SceneInstance` matrices | Column-major, with a Z reflection for rendering |
| Physics computation | Script frame, without injecting the renderer's reflection |

Do not apply a second scale conversion or a second Z flip in the solver.
For a wrong steering or rotation sign, follow the frame from the input to
the hub and the mesh before multiplying the input by -1.

### Assembly graph

Scripts choose the parts and call the natives. CFGs describe the slots,
poses, bodies, wheels, and render rules. `part_runtime.cpp` connects the
live objects to that description and to the simulation.

`attach`/`compatible` rules, slot IDs, and wheel indices have distinct roles.
Suspension-to-wheel associations must not be inferred from mesh order alone.

A `linked a [b] [parentSlot]` rule can be filtered by parent slot.
The first matching rule counts. Do not sort these rules. Hub, arm, and slot
frame calculations affect track width, rotation axes, and the visual pose
together.

LOD-level renders are alternatives. Drawing all of them as layers produces
duplicates. A render can belong to a wheel rather than to the body. Keep
that association.

### Derived state

An installed suspension can deform or stretch its render to meet its
anchors. When it is removed to the inventory, the part must return to its
rest pose. When it is installed again, recompute from the new configuration.
Do not confuse that geometric adaptation with persistent damage.

Openable panels use the CFG pivots, axes, and limits, with interpolation.
They must follow the car's transforms and assembly changes without moving
the main physics frame.

## 11. Physics and damage

`VehicleDynamics` owns the rigid body, wheel states, suspension parameters,
and the transmission. `part_runtime` extracts part properties and publishes
the results into the render graph.

The frequency currently declared is **120 Hz**, with an update frequency
declared at **60 Hz** in `vehicle_dynamics.hpp`. `advance(seconds)` uses an
accumulator. Do not bring back "once per frame" integration, and do not
assume that an older 700 Hz discussion describes the current code.
An FPS gain must not speed up simulated time.

A missing ground contact means air, not an implicit floor at Y=0.
Tire radius, centers/hubs, springs, damping, unsprung masses, and bump stops
must stay consistent. Do not pin a car at an arbitrary height to hide a
collision problem.

For driving, follow throttle → dyno torque → clutch/gears → distribution to
the wheels → rotational speed → slip → tire forces → forces and moments on
the body. A loss of grip is not proof that only a differential or an
anti-roll bar is missing.

The active tire code is in `tire_model.cpp`, integrated in
`vehicle_dynamics.cpp`. The presence of `sl2_pacejka.cpp` in the repository
does not mean the whole driving model uses that older model.

For the dyno, a new configuration must rebuild the matching curve banks,
including nitro. Do not mix an old nitro bank with a new RPM step or a new
geometry. Bound indices before the integer conversion. The last exact sample
must remain reachable.

Deformation and detached parts need consistency between the impact, the local
frame, the deformed mesh, the part state, and collision. Check separately:
vibration at rest, permanent deformation, a panel opening, and an attachment
breaking. A visual filter does not fix an unstable solver.

## 12. Enhanced rendering

The current product uses bgfx. `--render-bgfx` is not required. Historical
enums, files, or comments that mention D3D9/WGL are not a description of the
production path. Read CMake and `bgfx_lib_backend`.

Main responsibilities:

| File | Responsibility |
| --- | --- |
| `bgfx_lib_backend.cpp/.hpp` | Backend, GPU resources, and orchestration |
| `bgfx_lighting.cpp` | Lighting, sky, and reflections along the paths that exist |
| `bgfx_shadows.cpp` | Shadow resources and passes |
| `bgfx_osd.cpp` | Interface drawing |
| `debug_overlay.cpp` | Inspection and editing through ImGui |
| `scene_sun.cpp` | Light/time state used by the scene |
| `shaders/` | The actual material, sky, and sampling calculations |

### Reflections

The branch has double-buffered dynamic cubemaps, a capture framebuffer, and
a probe state with an origin and box-projection bounds.
An interior reflection depends on the probe position and the parallax
correction. Changing only the texture size does not fix a reflection that
looks stretched in the garage.

Capture, writing the faces, publishing a ready cubemap, and sampling it have
distinct lifetimes. Check the six orientations, the frame, world transitions,
the uniforms, and probe validity. Do not read a texture at the same time it
is the destination of its own capture.

A checked "dynamic reflections" option does not prove that the car material
reads that cubemap. Follow the binding into the shader. Check fallbacks to
the old envmaps and the `vehicle_material` flags.

### Shadows

Map shadows and car shadows have passes and resources that must be inspected
separately. Enhanced aims at dynamic shadows, not at stacking the old SLRR
projected shadow on top of a new shadow map.

Resolution alone does not explain the pixels: coverage in metres, projection,
depth, bias, filtering, leaf alpha, and the choice of casters also count.
A building missing from the casters stays without a shadow even with a larger
shadow map.

Every shadow pass depends on a linked shader program. bgfx links a program
only when the fragment shader's `$input` list equals the vertex shader's
`$output` list; otherwise it returns an invalid handle and, in Release, prints
nothing. The passes check the handle and switch themselves off. A varying added
to `vs_mesh` must therefore be added to every fragment shader paired with it.
Startup reports each failed program as `[bgfx] program <name> failed to link`.

Do not collect casters using only the visible camera frustum.
The list must respect the useful light volume. When the time of day changes,
sun-dependent matrices and caches must be invalidated.

### Sky and lights

The Enhanced sky must be driven by game time and by parameters consistent
with the sun, the clouds, and the reflection. A night-time appearance must
not leave a noon sun in the materials or in the shadow.
Animated cloud parameters need a stable time, distinct from a count of
elapsed frames.

OSD surfaces have their own blending contract. Do not apply world lighting
to them, or a global correction meant for the body.
For trees and glass, check alpha test, blending, depth write, pass order,
and mipmaps before modifying the original textures.

## 13. Interface, input, and options

OSD registries, script widgets, and picking rectangles must stay in sync.
Distinguish selected, hover, and unavailable. A texture shared by two buttons
does not make them the same widget.

Input goes through `host_input` and the Input/Controller natives: mapping,
the rebind listener, axes, hotkeys, and focus. Do not bypass the mapping with
a key hardcoded in driving. The listener must allow primary and secondary
commands and the other control menus.

Car instruments can come from the stock layout or from the CFG. `HudSlot`
covers, among other things, gauges, needles, odometer, damage, nitro, and
turbo. Preserve the expected size, aspect, and coordinates. A reference image
that looks offset at a high resolution can reflect stock behavior that is
kept on purpose, not an element to recenter automatically.

`host_game_options` centralizes driving, video, and gameplay options.
The stock store is `save/game/options`. Applying a resolution live must
synchronize Config, the host mode, framebuffer dimensions, the viewport, and
size-dependent resources. Rereading an old value when a menu opens must not
put the game back to 800×600.

`EnhancedGraphics` holds the extra options shared with the menus and debug.
Every new option native must have a stable descriptor, bounded behavior, a
defined save strategy, and documentation in the shipped patch. Do not
announce a persistent option before testing a restart.

## 14. Audio and video

`host_sfx` handles effects. `host_music` handles playlists. `video_fmv*`
handles video playback. Engine loops use the scripts' SfxTables.
`scene_ambience` plays the `sfx_loop`/`sfx_random` lines of map placements
through `host_sfx`; it owns those voices and stops them when their ground
shell leaves the active world.
Do not substitute a fixed engine sound for an RPM/load curve.

Simultaneous voices are deliberately bounded, and menu repeats are handled
separately. Installing a kit can produce many install events: avoid adding
them without a bound, and do not remove every audio response. Fast hover
sounds must keep a small bounded overlap, not a loop fed by a duplicate event.

Pause, a section change, and destroying a car must update the associated
loops. Stopping a car's rendering is not enough to stop its engine audio.

The process is x64: a 32-bit codec DLL cannot be loaded into this process.
Inspect the video backends that are present and their real dependencies
before promising a codec or a resolution. Keep FMV, music, and effect volume
correctly separated according to the options.

## 15. Saves and transitions

The active section has an exit/enter cycle. `SectionMachine` can also adopt
a state that Java has already transitioned: do not call exit/enter a second
time in that case. Presentation state holds one full frame during some
cooperative transitions so a destroyed scene does not flash.

The preview car, the car selected in free ride, and the car loaded from a
career are not interchangeable. A save/load bypass must not promote a preview
instance into the career car. Diagnose the stock flow, the references, and
the files that were actually loaded before adding a special CarSelector case.

Resetting a section means inspecting, together: parent world, pose,
velocities, the physics accumulator, contacts, assembly, camera, lights,
reflections, shadows, and loading state. Do not keep the fallen pose of an
old car as the initial pose of the next one.

A catalog/garage round trip does not necessarily recreate the same set of
objects as a dealer round trip. A state repaired by the dealer but not by
the catalog often means a missing invalidation or reset. That is not a reason
to reload the world every frame.

## 16. Working method and validation

### Before changing anything

1. Read `git status`, the branch, and the existing diff.
2. Identify the calling script, the signature, and the owner of the state.
3. Reproduce a reduced scenario with a known car or section.
4. Trace the useful inputs, transitions, and destructions, without per-frame spam.
5. Fix the responsible layer, then check its neighboring clients.

Do not undo another agent's or the user's changes to get back to a supposedly
clean base. To bring a fix over from another branch, apply its hunks or do a
Git integration. Restoring a whole file from that branch can erase Enhanced
work.

### Build

From the repository root, with Visual Studio 2022, CMake, and the dependencies configured:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target slrr_host --parallel
.\build\Release\slrr_host.exe --game --root data\game --window
```

An existing CMake cache must keep its generator. Check the README for
prerequisites and dependencies. The compiled shader must match the source
that was changed. Do not validate an old binary just because it starts.

If the game is using the executable, wait for it to close before replacing
it. Do not stop the user's session without an instruction.

### Checks

There is no separate test target. The `--probe-dyno` and `--probe-gc`
diagnostics cover some scenarios. See `main.cpp` for the accepted arguments.

For a UI fix, exercise interaction and focus. For a transition, repeat the
round trip and watch instance, draw, and memory counts. For rendering,
compare the same place, time, camera, and settings. For physics, compare
simulated time, input, and the same car at several frame rates.

A diagnostic can modify options or a save. Use a copy, or preserve and
restore the files involved. A headless probe does not replace a visual
validation and must not be presented as one.

### Shipping

State the cause, the files involved, the validation that was actually done,
the limits, and the patch files required. Do not edit native-coverage
figures by hand; regenerate them with `python scripts/render_native_coverage.py`.
Document every new native: class, name, descriptor, parameters, effect,
lifetime, and any persistence.

## 17. Diagnosis by symptom

| Symptom | First places to look |
| --- | --- |
| FPS drops after several cars | Instances left parented, strong registries, GPU not released, audio loops |
| Loading stays visible while driving works | Overlay/modal and the presentation cycle, not only load speed |
| Repeated hover or sound | Double host/script dispatch, a hidden widget still active, focus |
| A slider closes a window | Wrong handler/command, an event with no widget, a wake mistaken for a close |
| Car restored after installing a part | Geometry rebuild/invalidation, contacts or pose missing on load |
| Wheels too wide or warped | Slots, wheel index, hub frame, rotation order, a double transform |
| Reflections lost when a part is removed | Material recreation, cubemap binding, instance invalidation |
| Reflections distorted indoors | Probe origin, box projection, frames, and cubemap faces |
| Buildings with no shadow | Caster collection, flags, pass, and light volume |
| Shadows in large pixels | Texel density, coverage, filter, bias, caster alpha |
| White textures | Owning RID, dependencies, slot, decode/upload |
| Multiplied textures or z-fighting | Duplicate mesh/LOD, multipass, UV, and material blend |
| Sparse or dotted trees | Alpha test/blending, mipmaps, depth, SCX material |
| Return to 800×600 | Config out of sync, stock values restored, dialog commit |
| Preview appearing in the career | Identity and the save/load cycle, retention of the previous instance |

These leads direct the investigation. They do not replace evidence in the
code or a reproducible scenario.

## 18. Limits and references

Driving, damage, the city, and the full career are still in development.
Some APIs are absent or partial. Native counts and older comments can date
from a state before the Enhanced renderer. The active code is what counts
for the implementation.

Further local documentation:

- [README](../README.md): overview, build, and features.
- [SCRIPT_RUNTIME](SCRIPT_RUNTIME.md): scheduler, notifications, and modals.
- [VM_GC](VM_GC.md): roots, safe points, and finalization.
- [NATIVE_AUDIT](NATIVE_AUDIT.md): binding coverage and limits.
- [HOST_NATIVE_UTILITIES](HOST_NATIVE_UTILITIES.md): utilities and options.
- [SCRIPT_COMPILER](SCRIPT_COMPILER.md): multi-class TUFA compilation and Language natives.

External working references, to be compared at an identified revision:

- `nedobylskiy/SLRR_rev`: reconstruction, formats, physics, and telemetry.
- `EliasFD3S/slrr-reverse`: reconstruction of native APIs and behavior.
- Scripts and classes of the installed game: concrete callers of the contract.
- The original executable and RE tools when the code is not enough.

When a behavior is only supposed, write it as a hypothesis.
When a value or an algorithm is ported, note its source and the revision.
When a contract changes, update this guide and the matching specialized
documentation in the same delivery.
