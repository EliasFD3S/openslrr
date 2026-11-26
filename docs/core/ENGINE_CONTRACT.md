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
| `docs/` | Documentation; the index is `docs/README.md` |
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
in a defined scenario. See `docs/scripting/natives.md`.

Loaded class definitions are immutable for the VM lifetime, whether requested
by name or `.class` path. Never replace a class's methods/TREE vectors or rerun
its statics under executing or suspended frames. Recompilation takes effect
for an already-loaded class after restart. Null and arrays are reference
arguments during by-name overload selection; null must not tie an integer
constructor solely because both are one-argument calls.

TREE increment/decrement must write back to the operand's actual storage:
local, instance field, static owner/field or array slot. Preserve the owner
resolved by a field path; computing a stepped stack value alone is insufficient.
Postfix expressions return the old value, prefix expressions the new value.
The career clock/day transition uses this general static-field behavior.

## 6. Threads, events, and dialogs

Script threads are cooperative and implemented with Windows fibers. They keep
their interpreter stack and their C++ call stack across a sleep/wait.
They are not parallel Java workers. The VM stays on the host thread.

`start` schedules execution. `sleep` suspends until a deadline. `wait` waits
for a notification. `stop` must let scopes unwind before the stack is
released. A long C++ job inside a native does not become preemptive just
because the caller is a fiber.

### Host threads and the worker pool

The host thread runs the VM, the frame pump, scene collection and bgfx
submission. Physics steps on its own thread (#203, `PhysicsWorker` in
`part_runtime.cpp`). bgfx presents from its own render thread, pedestrians
step on their own worker (`PedSim` in `part_runtime.cpp`), and audio and video
decoders keep their library threads.

In the game the physics thread is free-running (#380): it runs one solver
step at a time (1 ms at the default 1000 Hz) on the wall clock, independently
of the frame rate, over every car, loose body and free PhysicsRef body of an
open batch (`PhysicsClock` in `part_runtime.cpp`). `part_physics_begin` is
the sync point: it pauses the thread after its current step, commits the
steps run since the last sync, publishes the poses this frame draws, prepares
the next batch and resumes the thread. Game time keeps running during the
sync (the thread catches up, up to 0.1 s), and its rate follows the frame's
simulation time (time warp, 0 while paused). The host owns the records only
between the pause and the resume, so scripts, natives and callbacks still
never run concurrently with a step on the records they read. In free-running
mode each solver step is its own host tick.

The free-running thread keeps real time first: when it falls behind, the sync
waits up to 50 ms for the steps due (the frame rate drops, not the game
speed), and the solver rate halves down to 250 Hz, then doubles back to the
requested rate once there is room (`free_running_sync`). Distant native
traffic steps at about 125 Hz, or 60 Hz beyond 150 m, as merged steps of its
owed time; crush cages step at their own 120 Hz. A host function called every
frame must join only when it changes what the thread reads: a join pauses the
thread until the next sync.

The fixed-step benchmark, `--physics-batch` and the tests keep the
frame-locked mode: one batch per frame of the 60 Hz host ticks the frame
owes, each as `VehicleDynamics::steps_in_host_tick` solver substeps (2 at
120 Hz, 16 or 17 at 1000 Hz); `part_physics_begin` starts it before
`render.flush()` and `part_physics_end` waits for it and publishes it.

A batch works on copies taken while the host owns everything: bodies, drive
and steering inputs, options, nitrous capacity, crush-cage probe shifts, car
shells and immutable dyno curves. On the physics thread it reads only those
and the scene collision prepared for it. It must not call the VM, GameRef,
`records`, audio, bgfx or anything with a lazily filled cache; it records
impacts, props to wake, loose bodies to settle and diagnostics for the commit,
which applies them in solver order with the chassis as it was when hit. Host
code that may run while a batch is in flight and changes what a batch reads
calls `part_physics_join()` first: the scene collision mutators,
`part_destroy` and the debug overlay's tyre tuning do. In free-running mode
the join pauses the thread until the next sync. `--physics-inline` runs the
frame-locked batch on the host thread; from the same starting state the
fixed-step benchmark gives the same states either way.

Loading work that only reads files goes to the shared worker pool in
`src/platform/job_system.cpp`: one worker per logical processor besides
the host thread. `jobs_parallel_for` spreads the indices over the workers
and the calling thread and returns when all are done; a nested call runs
serially. Its users are pack indexing at startup, the immutable traffic-lane spatial
index (per-edge fragments merged on the host after the join), mesh loading for a map's
props and for the render objects new in a frame, a map's object types (CFG
and collision meshes), the placement collision bakes, their merge and prop
batching, static collision that appeared or moved
(`scene_instances_prepare_physics`), the floor index of meshes of 32k
triangles or more (`index_floor`), and decoding the texture files of the
meshes new in a frame. An exterior map (Valo City's `city2`, named by `city`)
is parsed on a `std::async` thread as soon as its parent map is accepted,
while the parent's props load on the pool.

A job writes only its own slot of a result that the caller prepared, such
as a cache entry the caller reserved before the batch. It may read the
`RpkIndex`, game files and data no one modifies during the batch; it must
not touch the VM, script objects, GameRef state, other cache entries or
bgfx. The caller publishes the results on the host thread, in the same order as a serial load, so pack numbers and cache
contents do not depend on the thread count. Code that can run as a job logs
with `jobs_printf`: its lines are held per index and printed in index order
when the batch ends, so the log reads as a serial run's. A task outside the
pool holds its lines with `JobLogCapture` for its caller to print. Steady
per-frame work stays serial: it takes a few milliseconds and reads state that
the frame mutates; [MILESTONES.md M7](../planning/MILESTONES.md#m7--multicore-frame) plans
how to spread it. The first exception (#266) is the placement loop of
`scene_instances_collect_batches`: it reads data no one writes during the
batch, its jobs list placements per run of visible cells, and the host
thread appends the runs in order. Map prop LOD levels are the one value it
writes, through `std::atomic_ref` (see [KNOWLEDGE_BASE.md §5.10](KNOWLEDGE_BASE.md#510-threads-and-the-worker-pool)). Walker
skin palettes (#268) are the second: the host thread prepares each
character, jobs rebuild different characters, and the host publishes them.

Worker bgfx encoders (#270) are the third, behind the renderer debug option
"Worker encoders", on by default. The AO prepass, the car lamp shadow tiles
and the reflection cube faces are queued during the frame and submitted by pool
jobs, each on its own `bgfx::begin(true)` encoder and its own binding state,
one job per Sequential view, while the host thread submits the main view on
encoder 0. View setup, blits, transient vertex data of the sky, light data
uploads (moved to frame setup) and texture creation stay on the host thread:
the host resolves every texture a job will look up before the jobs start.
With fewer than 3 pool threads the queued draws run on the host thread in the
same order, and the image matches the serial path byte for byte.

Map loading used to spend most of its time on the host thread: about 13 s
of a warm Valo City start, mostly a merge of the placement collision that
grew its buffer by one mesh at a time. `merge_static_meshes` now sizes its
buffers once; keep such accumulations linear.

GameType notifications and callbacks are separate registries.
The documented modes 8, 9, and 28 concern `control(float)`, `drive()`, and
`animate()`. Iteration must tolerate an unregister or a reregister during the
callback. See `docs/scripting/runtime.md` for the flags and budgets.

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

The allocation/sweep membership index validates addresses before dereference;
it is weak and does not mark objects. Reachability uses per-object epochs, with
a fresh epoch for resurrection tracing. Finalizer eligibility is cached per
class and invalidated by class loading, root/classpath changes and new native
registrations. Keep these invalidations when adding class or binding APIs.

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
second trace. See `docs/scripting/vm-gc.md`.

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

There is no stock 286-pack cap in this host. Pack registration uses dynamic
containers, but the compatible runtime namespace is finite: pack IDs
`1..0x6fff` (28,671 packs) precede live handles at `0x70000000`; the
`0xffff` high word also identifies transient painter textures. Registration
returns zero at exhaustion rather than aliasing a live handle or wrapping a
pack number. Existing paths remain reopenable. Removing this ABI boundary
would require changing script ID arithmetic and save/resource remapping,
not just widening the host's pack counter.

Indexing reads only the header, dependencies and TOC; payloads are read on
demand. Payload streams use a 64-entry LRU, independent of installed pack
count, so visiting many packs does not retain a CRT file stream for each.
Pack basename lookup and the generation-scoped parent/child index avoid
repeated global scans during resource traversal. Exact path lookup still
wins over the first-opened basename match, and children retain pack/TOC
order. A duplicate local type ID resolves to its first TOC entry; child
traversal uses that same entry, so a shadowed duplicate cannot reparent or
repeat the canonical resource. Registration/root changes invalidate derived
traversal data and must remain quiescent as described in §6.

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

Map placements keep the stock LOD chain of their object type (#252): `lod
first last` on a render and the object's `lods N s0 s1 ...` size ratios. The
main camera chooses one level per placement, with 10 % hysteresis and the debug
LOD bias, and every other pass (AO, sun cascades, reflections, lamp tiles)
draws that same level. Types without `lods` keep only their highest LOD.

`ResState` and `GameRefState` represent instances, their identities, parents,
and children. A type resource is not its live instance. A script alias and
reference wrappers do not justify duplicating the underlying object.

Painter textures (#64) are host images with ids `0xFFFFxxxx`
(`paint_texture.hpp`). A part shows one through its `Part.setTexture` value
like a pack texture; it is drawn into only while that part alone shows it,
and it is reclaimed after three host frames without a part, native resource
wrapper or open save reader using it. `ResourceRef.makeTexture` file textures
have live native handles; their pixels are embedded the same way on save.
Painter textures are saved as
type-7 blobs, never as pack references; see §15.

`scene_instances_collect` produces the instances to draw for the selected
world. Geometry can be shared. Pose, color, and texture replacements are
instance properties. Avoid modifying shared geometry to fix a single car.

File-backed textures created by `ResourceRef.makeTexture` must reach the same
per-instance material path as RPK textures through `changeResource`,
`duplicate`, and `RenderRef.create`. Copy the replacement path by value; do not
make the render depend on a temporary Java texture wrapper or mutate shared
mesh textures. Standalone dynamic texture ID queries remain incomplete.

An active camera with a non-null viewport may render only while that viewport
exists and is active. `Viewport.deactivate` / OSD hiding must stop an old
preview even when the script retains its camera and scene. Reactivation may
reuse those objects. Do not implement this by recognizing a mod's selector
class or forcibly deleting its model. The same holds for OSD widgets: an Osd
whose viewport has never been activated neither draws nor takes clicks.

A map is not necessarily one SCX. `GroundMapData` distinguishes render and
collision meshes, exterior packs, and placements. Valo City requires resolving
map chains and their placed objects.

"Native object" on a map means world objects that come from the engine's
descriptions and placements. It does not necessarily mean a new Java native
to register. Inspect the data and the instantiation path first.

A native type's cfg is either embedded (ICFG) or, in packs built by tools
such as Track Wizard, a `native <kind> <path.cfg>` line naming a file on
disk; `src/resources/rpk_index.cpp:RpkIndex::native_config` reads both.
Native ICFG loading retains both renders and collision bodies. CFG `box`
half extents, `sphere` radii and SCX collision meshes are cached per type,
then instanced with `placement pose × body pose`. `nocollision` suppresses
the body. Collision follows the active ground shell's world transform and is
not selected by render variant, LOD, visibility or culling. Script-created
`PhysicsRef` primitives and generic objects activated by
`GameRef.setActiveCollision` use the same scene index; OSD click volumes are
a separate contract. A placement whose collision-enabled bodies all have
positive mass stays an ordinary instanced prop with no collider until a car's
padded chassis bounds reach it. It is then promoted, after the vehicle loop,
to a loose native body with its full CFG collision mesh and mass-weighted
body-pose centre, and its static instances are hidden. The loose body sleeps
until contact, reuses the loose-part solver for car and static-scene contacts,
and updates every render slot, with its CFG render pose, from the simulated
frame. Once it rests more than 40 m beyond every active car's reach it becomes an
ordinary prop again at the pose where it settled (for the rest of the
session), so only props near the car are live. It returns to its map pose
instead when it was pushed more than a 64 m prop cell away, falls below
-500 m, or keeps moving out there for 10 s, and becomes scenery wherever it is
when its world stops being the active one. A map can hold thousands of such props: creating them all as live
bodies at load cost about 400 ms of physics per frame in Valo City.
Mixed-mass placements and zero-mass buildings, columns, trees and containers
remain on the exact static path.

`PhysicsRef.setStatic(0)` hands a script body to the host free-body solver:
it falls under gravity, rests on the scene, and exchanges impulses with the
active physical cars like a loose part, publishing its pose and velocities for
`getPos`, `getOri`, `getVel` and `getAngVel`. `setMatrix` places it at rest,
and `setStatic(1)` fixes it again. Created bodies stay fixed until then; the
stock jack, lifts and showroom walls never call `setStatic`. A primitive has
no authored mass, so it gets a 500 kg/m³ density; a `create` resource uses its
CFG mass when it has one.

`GameRef.RID_TRIGGER` (system:0x34) objects, which `java.game.Trigger` creates
with `"x,y,z,yaw,pitch,roll,sphere,r"` or `"...,box,x,y,z"` params, are
volumes. Each frame `scene_triggers_tick` raises `EVENT_TRIGGER_ON` when a
top-level car of the active world enters one and `EVENT_TRIGGER_OFF` when it
leaves, with the car's `id()` as the String param. A car that first appears
inside a volume, or that `setMatrix` moves more than 25 m in one frame, raises
nothing for that change. Box sizes are read as half extents, as in SLRR_rev.

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

An installed suspension poses its linked renders on their anchors, and
the anchors on a wheel carrier move with the suspension at every published
physics tick. A two-anchor render (`linked a b`) is bound like the original
`add_linked`: bone00 on anchor `a`, bone01 on anchor `b`. Its frame sits on
`a` with local +Y toward `b`, and the SCX skin weights deform the mesh
between the two bones, so a spring's coils close up and a shock absorber
telescopes instead of the whole mesh scaling. A render descriptor without a
`skeleton` property binds the BON next to its skinned SCX
(`parts/running_gear/meshes/spring.bon`, `shock_absorber.bon`) only when that
BON is a link rig, naming bone00 and bone01; the BONs next to human meshes
are not. A two-anchor
render with no such rig, such as a CV shaft, falls back to stretching its
authored +Y bounds between the anchors. A one-anchor render, such as a
control arm, moves whole onto its anchor frame: travel without steering when
that anchor is on a wheel carrier, fixed when it is on the chassis.

When it is removed to the inventory, the part must return to its rest pose
and its bind skeleton. When it is installed again, recompute from the new
configuration. Do not confuse that geometric adaptation with persistent
damage.

Openable panels use the CFG pivots, axes, and limits. They must follow the
car's transforms and assembly changes without moving the main physics frame.

A `flap px py pz ax ay az dx dy dz lo hi` line belongs to the slot before
it. Pivot, axis and `dir` (hinge towards the free edge) are in the parent
part's frame and follow a `setSlotPos`/damage pose of that slot. 0 is the CFG
pose, which is closed; the open stop is the limit farther from 0, so
`0 1.134` (Baiern doors) and `-0.960 0` (Erbilac trunk) both open. A part on
such a slot is a one-degree-of-freedom hinged plate
(`include/slrr/flap_hinge.hpp`), the same for every car and in the garage:

- Closed, it is latched and does not move.
- `Part.flap(1)` and the `flap_toggle` command (Mechanic's tuning click)
  push it with build 606's spring (gain 60 per second squared): open aims at
  95% of the travel, so the panel overshoots, hits its stop and shakes before
  it rests (a heavy hood sags below the target, where the spring carries its
  weight); close aims through the closed pose, where it latches. A load that
  forces a held panel well back from the stop beats the spring once it has
  settled, so a door left open swings shut when the car pulls away.
  `flap(0)` returns 1 while it is open, by script or by a hit.
- A chassis impact reaching the panel (`damage_chassis` hands it on) releases
  the latch above 3.5 m/s local speed, independently of the damage options.
  The obstacle's push swings a free panel, and the closed stop returns an
  inward push.
- A free panel reacts to gravity, to the carrier's velocity change at the
  hinge and its rotation, and to the relative wind, with damped end stops.
  It latches again when it slams shut or comes to rest closed, unless the
  panel is bent (its own deformation cage below half condition): then it
  closes onto its stop but stays unlatched. The simulated hinge follows the
  current slot pose and installed mass, like the drawn one. A part moved to
  another slot or taken off starts closed and latched. The hinge state is not
  saved: a loaded car starts with every panel closed and latched.
- An unlatched panel of a released, moving car collides with the static
  scene and native objects: the oriented box of its bounds, in its current
  pose and swept from the last frame, goes through the same native box/sphere
  and wall-triangle queries as the chassis
  (`scene_instances_contact_box`, `scene_instances_contact_walls`). The panel
  turns out of the obstacle and loses its approach speed along its swing, so
  an obstacle in front of an open door swings it shut, where it latches if it
  can. At 4.5 m/s or more of contact speed (the car's motion at that point
  plus the swing), or when an obstacle it cannot swing away from presses more
  than 0.15 m into it, the hinge gives way: the panel comes off through
  `detach_panel` and becomes a loose body. Driving forward also blows an open
  door shut through the wind and the car's acceleration.

The car does not feel its panels' contacts, and the panels do not collide
with each other, loose bodies, other cars or the ground (floor triangles are
left out, as for the chassis walls). In the garage, where the car is
suspended, a panel opens without world contacts. The script spring's gain and
95% target are build 606's (flap solver 0x0043F850); its light damping and
the other constants are an OpenSLRR design, and SLRR_rev has no `flap`
native.

## 11. Physics and damage

`VehicleDynamics` owns the rigid body, wheel states, suspension parameters,
and the transmission. `part_runtime` extracts part properties and publishes
the results into the render graph.

Static scene collision has two complementary paths. Triangle meshes serve
support and sphere-sweep queries. The chassis box is also tested directly:
against indexed CFG/`PhysicsRef` boxes and spheres, and against the wall
triangles (|normal.y| <= 0.65) of every static triangle set, with
`overlap_triangle` in `scene_collision.cpp`. A thin trunk, a house corner or a
fence end therefore cannot pass between the four chassis corner probes. Both
tests leave out the lowest 30 % of the chassis box, the band below the corner
probes, so kerbs, steps and road seams are wheel support, not walls. The chassis query
uses the bounds centre (not necessarily the centre of mass), samples longer
movements up to 8 times, then, when the tick began clear, bisects between the
last clear sample and the first overlap so the car is put back near the moment
of contact, and feeds
the contact normal into the existing impact response. A slow approach (under
1 m/s, such as a car held against a wall by its tyres) is instead pushed
straight out of the overlap, a quarter-millimetre off the surface, keeping its
movement along it; a larger gap made such a car touch only every other tick
and shake. The oriented-shape
query is still discrete, not a general continuous manifold solver; keep the
triangle sweep path for terrain and tunneling protection.
Positive-mass native placements do not enter static mesh or analytic collision
caches. Their loose body is created only near a car; it uses the authored
mesh, mass and centre, starts asleep, wakes on car contact, and exchanges
equal/opposite impulses with both car and static scene. A sleeping loose body
skips the per-part car test unless its bounding sphere can reach the car's.
Free `PhysicsRef` bodies (`setStatic(0)`) use the same loose-body contacts.
Native/mechanical chassis also resolve unique finite-mass OBB pairs on shared
120 Hz substeps. Dynamic-body reach, wake, contact and map-prop demotion consider
all active physical cars, not just the controlled car; only a freshly detached
engine temporarily ignores its original carrier. See the #197 audit for the
bounded translational sweep, actual-impulse damage and remaining limits.
This is a host loose-body approximation, not a general dynamic triangle-mesh
engine: loose bodies collide with the car and the static scene, not with
each other.

Rigid-body validation exceptions are contained by the owning body runtime.
Keep strict kernel checks; do not catch a whole frame and silently skip the
remaining bodies. Invalid GameRef poses and wheel inputs are rejected before
mutation. A failed chassis is suspended at its last finite published pose,
a failed loose map prop returns to scenery, and a failed free PhysicsRef stops
simulation. Native map-body creation is transactional. See the #75 entry in
[vehicles/notes.md](../vehicles/notes.md#per-body-physics-failure-containment-75) for recovery, retry and testing limits.

The frequency currently declared is **120 Hz**, with an update frequency
declared at **60 Hz** in `vehicle_dynamics.hpp`. `advance(seconds)` uses an
accumulator. Do not bring back "once per frame" integration, and do not
assume that an older 700 Hz discussion describes the current code.
An FPS gain must not speed up simulated time.

Frames do not line up with host ticks. A car is always drawn between its
last two ticks, by the fraction of a tick still left unsimulated
(`gameref_render_matrix`); the Classic framerate mode that drew the last
tick was removed with its option. Scripts, collisions and the published node matrix
keep the ticked pose. Rendering, the driving cameras and car shadows use the
drawn pose (`scene_instance_transform`, `scene_instances_render_matrix`); do
not mix the two for one visible element.

A missing ground contact means air, not an implicit floor at Y=0.
Tire radius, centers/hubs, springs, damping, unsprung masses, and bump stops
must stay consistent. Do not pin a car at an arbitrary height to hide a
collision problem.

The scripts size the wheel of the stock rim slot (`101 + corner`). When that
slot is empty, the corner keeps a physical wheel while hardware remains on
its other wheel slots (type 101–104, such as an outer dually wheel) or on its
hub (type 201–204). Its radius is how far that hardware reaches below the
wheel centre, so the car rests on the brake or dually wheel as drawn.
`Brake.updatevariables` writes the rotor size plus 0.02 m, but the stock rotor
mesh does not scale with it. A corner with none of this hardware loses its
wheel and falls back to the chassis box contacts.

For driving, follow throttle → dyno torque → clutch/gears → distribution to
the wheels → rotational speed → slip → tire forces → forces and moments on
the body. A loss of grip is not proof that only a differential or an
anti-roll bar is missing.

The active tire code is in `tire_model.cpp`, integrated in
`vehicle_dynamics.cpp`. The presence of `sl2_pacejka.cpp` in the repository
does not mean the whole driving model uses that older model.

Aerodynamics are real-life calculations, as the owner decided on PR #223:
every force is `½ ρ C A v²` from the air velocity relative to the car
(`w - v`, with the wind `w` below), with ρ = 1.225 kg/m³ and typical road-car
coefficients, not values recovered from the original game.
`VehicleDynamics::apply_aero` adds, each substep:

- **Body drag, side force and lift.** The rigid body's bounds (W × H × L)
  give a frontal area `0.85 W H`, a side area `0.85 L H` and a plan area
  `0.85 W L`. Each component of the air velocity in the car's axes drags on
  its own area: Cd 0.32 on the frontal area for the flow along the car, and a
  crossflow Cd 1.0 on the side and plan areas, so a sliding car meets its
  side area. In yawed flow the side view also acts as a slender wing of
  chord L and span 2H (the ground mirrors it): a side force of
  `½ρ A_side (πH/L) |v_x v_z|`, about 0.04 per degree of yaw on the frontal
  area as road cars measure, at a quarter of the length from the leading
  end. Lift uses CL 0.25 on the frontal area with the flow along the car.
  The drags and lift act at the centre of the bounds; native traffic
  templates keep their own force policy and get none of them.
- **Chassis drag.** `Chassis.updatevariables` sets `C_drag` to
  `fully_stripped_drag` minus the `drag_reduction` of each body part on the
  chassis slots, floored at 0, and the EXE reads it with `drag_center`
  (0x448CD2). It is already in real units (kg/m, `½ρCdA`): above 1 m/s it
  adds `-C_drag |v| v` at `drag_center` (0x456A83), the extra drag of missing
  panels. A complete stock car has 0.
- **Wings.** A CFG `wing` line authored as a wing (type 5, or type 4 with
  exponent 0.667) is one face of an airfoil in its part's frame; body surface
  lines only describe the shape the body model covers. The face meeting the
  air, with incidence `sin α = v̂ · n`, carries the element's normal force
  `½ρ A v² min(4.2 sin α, 1.5)` along `-n` (lift slope `2πA/(A+2)` for aspect
  ratio 4, stalled single-element limit) plus profile drag Cd 0.05; the face
  turned away carries nothing. Its tilt splits the force into downforce and
  induced drag.

The centre of pressure follows from where these forces act: a rear wing
moves load to the rear axle and lifts the front, and the side force ahead of
the centre of mass turns a car in a crosswind downwind, as a road car is
aerodynamically unstable in yaw. Every drag force is
clamped to the impulse that stops its point in one step, as the EXE does for
`C_drag`.

**Wind.** `--wind M/S[,HEADING]` gives the mean wind of a weather report: m/s
at 10 m height, blowing towards HEADING degrees from +Z towards +X. The
default is 4 m/s (a gentle breeze) towards 60°; `--wind 0` is calm air. The
original game has no wind. `wind_velocity` turns it into the air velocity at
car height:

- **Mean.** The log wind profile over open terrain, roughness length
  z0 = 0.03 m, at z = 1 m: `ln(z/z0) / ln(10/z0)` = 0.60 of the report.
- **Gusts.** Turbulence intensity `1/ln(z/z0)` = 0.29 along the wind
  (EN 1991-1-4 §4.4) and three quarters of it across, each from three
  sinusoidal plane waves of 28–240 m wavelength with fixed phases. The waves
  are frozen in the air and carried by the mean wind (Taylor's hypothesis):
  in the default wind a standing car feels gusts every 12 s to 3 minutes,
  and a car at 100 km/h crosses the shortest in about a second. There is no
  vertical wind.

The wind changes the dynamic pressure `½ρ|w - v|²` on every surface: a
headwind adds drag, lift and wing downforce, a tailwind removes them, and a
crosswind yaws the flow: it pushes the car downwind and turns its nose
downwind through the body's side force.
On the physics thread, `sim_step_car` samples it at each released car's
centre of mass every substep, from the wind and driving-world clock that
`prepare_batch` copies into the batch on the host; cars in the garage or a
preview, and native traffic templates, feel none. Hinged panels feel the
same wind. Air density stays at ISA sea level: there is no weather, altitude
or temperature change of pressure.

`part_runtime` reads `C_drag`/`drag_center` with the drive inputs and
collects the wing faces in chassis coordinates when the body support is
rebuilt (assembly change, deformation, repair), at the parts' poses at that
moment (`rebuild_aerodynamics`). A swinging hinged panel keeps the pose it
had then; loose detached parts get no aerodynamics.

`wing` lines (handler 0x4443C0, 16 per part) are authored by the original
Invictus Aero v1.2 3ds Max calculator (posted on PR #223): position and
unnormalised normal are the area-weighted centre and normal of the selected
faces. Its SLRR method writes type 4 with `area` = 30 × the flattened area in
m² for surfaces and 90 × for wings, suction −0.25/0.25 and exponent
0.666/0.667; its CRC and Experimental methods store about 1.27 × the area and
write wings as type 5. `part_wing_is_airfoil`/`part_wing_area` invert this.
For reference only, the original EXE's own formulas, no longer applied:
type 4 pushes along the normal with `-area · d' · (d' > 0 ? 1 : suction)`,
`d' = sign(d) · 100 (|d|/100)^exponent` for the normal velocity `d`
(0x48EB3C); type 5 with `-area · d + suction · |v_t|²` (0x48ED73). With the
stock exponent 0.666 that grows with speed^0.666, far stronger than real air
at town speeds. slrr-reverse decodes the same `C_drag` drag and clamp
(`Chassis_part2.inc`).

The tyre/drive model (revision `8ac2ba9`) and the nitrous contract are in
[vehicles/driving-model.md](../vehicles/driving-model.md).

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

The detailed contracts for reflections, mesh CPU/GPU ownership, shadows, sky
and lights, script and scenery light sources and the renderer debug overrides
are in [rendering/contract.md](../rendering/contract.md).

## 13. Interface, input, and options

OSD registries, script widgets, and picking rectangles must stay in sync.
Distinguish selected, hover, and unavailable. A texture shared by two buttons
does not make them the same widget.

Input goes through `host_input` and the Input/Controller natives: mapping,
the rebind listener, axes, hotkeys, and focus. Do not bypass the mapping with
a key hardcoded in driving. The listener must allow primary and secondary
commands and the other control menus.

Pointer gestures have one owner at a time. A loading plate (`LoadingScreen`
overlay or dialog) takes every click; a section change, a modal or a movie
takes the 3D-view clicks, including the Mechanic's part clicks. Starting or
ending a plate or a section change cancels latched clicks and every held
button (`host_input_cancel_pointer_gestures`): its release clicks nothing, and
only a fresh press counts. The right button starts a camera drag only when the
world camera can be dragged (`gameref_world_camera_can_drag`), never in menus.
Camera look keys are the controller's bindings (stock `CAMTURNSET`), not raw
arrows, so a script that resets to `DRIVERSET` stops the orbit. The pointer is
visible while `MouseCursor.visible` (set by `MouseCursor.enable`) is nonzero.
A driving chase look is not undone by time: a parked car keeps it, and
forward travel tows the camera back behind the car like a rope (tan(a/2)
shrinks by exp(-s/r) over s metres at follow distance r). A script
`setMatrix` on the bound follow camera holds it in place until the car pulls
the `dist` range taut. `angle offset gain rate` turns a held follow camera or
a script orbit camera around its target (relative azimuth 0 is behind it);
its rate runs on game time unless `simulate 1` keeps the camera on real time.
`zoom aov rate` eases the view angle toward aov at rate per second (a negative
rate snaps).
The driving chase and interior views (not a script-attached camera or the
free camera) are an openslrr addition over the native fixed 42 degrees: their
view angle widens with the smoothed chassis speed (smoothstep from 30 to
250 km/h, up to 62 degrees) and starts from the current speed's angle when the
view is activated. In the chase view only, smoothed forward acceleration
moves the eye up to 1.2 m back while pulling and 0.8 m closer while braking
(`update_chase_feel` in `gameref.cpp`).

SDL3 owns joystick/gamepad discovery and polling on that same host thread;
Win32 still owns the window, keyboard scan codes and mouse. Devices 0/1 remain
`SysKeyboard`/`SysMouse`. Device 2 keeps the saved name `XInput Gamepad` and its
24 split-positive channels/default controls as a compatibility alias for the
primary SDL gamepad. Slots 3+ expose every peripheral's signed axes, then buttons,
then four positive directions per hat (up/down/left/right). The primary pad is
captured through the alias only, and only an SDL device of gamepad type can be
it: a wheel that SDL also maps as a gamepad keeps its raw channels. Explicit
peripheral bindings replace the matching pad driving default; wheel axes use
analog steering rather than keyboard ramps. Disconnected/unfocused bindings are
skipped, not evaluated at signed zero.

Each device id has a class (`host_input_device_kind`, `InputDevices.kind`):
keyboard, mouse, pad (device 2 and SDL gamepads) or wheel (every other SDL
peripheral: wheel base, pedals, shifter, button box). Filters are chosen per
class, never per product: keys are digital and ramped by `steering_filter`,
pad sticks keep their noise floor and dead zone, wheel axes are read as they
are. `HostInput::drive_steer_kind` says which class steers in this frame.

The rebind listener is `InputCapture` (`src/platform/host_input_capture.hpp`),
fed every channel of every device each frame by `host_input_poll_keys`. It
waits until no channel moves for 0.3 s (2 s at most, for a noisy axis),
snapshots that as the rest, ignores what is still held (an H-shifter in gear,
a toggle), takes the channel that moves farthest from its rest, and waits for
its release (2 s at most, for a toggle that stays on). Device order never
decides, and one channel that never rests cannot block it. The left mouse
button cancels; Esc clears the binding. `InputDevices.applyCapture` writes the
range: 0..1 for a key or button, rest..far end for an axis at an end stop, the
half it moved into for a centred axis (TURN LEFT/RIGHT on a raw axis bind both
halves). Screens show the steps (release, listen, release again) and set
`capturingInput` so hotkeys stay off meanwhile.

ControlSet saves exact device names and snapshots enumeration when constructed.
Names include a GUID plus serial-or-path identity hash; slots remain reserved on
disconnect and are reused on reconnect. Reopen control options after adding a new
device. Without a serial, changing USB ports can change identity; indistinguishable
same-model devices without a serial/path use discovery-order suffixes. A capture on a
device plugged in after the set was made adds its name (`InputDevices.ensureDevice`)
so the binding survives the save. The patched `ControlSet` (#405) has 118
controls instead of 78: the car controls get a primary and a secondary slot
per device class (`ControlSet.carSlot`: keyboard and mouse in the stock 0-9
and 15-24, pad in 78-97, wheel in 98-117). The file format is unchanged; a
stock reader skips the extra slots, so it loses only the pad and wheel
bindings. Regression: `scripts/run_save_io_tests.ps1 -Suite sdl_input`
uses virtual peripherals; real rig calibration remains a separate hardware check.

Force feedback (#395) is SDL haptics on the host thread: a constant force
and a damper on the steering axis (`SDL_HAPTIC_STEERING_AXIS`) of the first
steering binding whose device has haptics. The torque comes from the
controlled car's front tyres after each committed physics batch
(`part_runtime.cpp:update_force_feedback`, `physics::SteeringFeedback`): trail
times lateral force, the pneumatic trail fading past the grip peak, normalised
by the front tyres' peak torque at the resting front load. Its sign follows the
binding's direction; `Config.ForceFeedBack`, `Config.FFB_strength` and host
option 25 (invert) apply. It stops when no car is driven, under a modal, out
of focus, and 0.25 s after the last committed batch. The wheel's own
autocentre is turned off when it is opened. Detail (#399, reworked in #405)
adds bump kicks (the filtered rates of the two front suspensions moving apart,
past a dead band), a single decaying knock on each impact of the controlled
car, and a light sine vibration only while the front tyres scrub past their
peak; a rough road plays no periodic effect. Each part has its own host
option, in percent: 26 road feel (saved as `ffb_detail`), 27 impacts, 28
vibration, 29 damper; 30 is a minimum force (0-20 %) for a wheel's dead band
and 31 a low-pass on the force (0-50 ms). `Config.FFB_strength_emulated`
and `Controller.user_SetAxisForce`'s force coefficient are not implemented.

Service brakes (#400): the host gain is 25 / `brake_inertia` (the handbrake
50), and the chassis `brake_balance` is read as a front share
(`include/slrr/brake_balance.hpp`): 0.5 puts 70 % of the braking on the front
axle, an adjuster moves it from 100 % to 40 %. The total and each wheel's share
of its axle come from the brake scripts.

Car instruments can come from the stock layout or from the CFG. `HudSlot`
covers, among other things, gauges, needles, odometer, damage, nitro, and
turbo. Preserve the expected size, aspect, and coordinates. A reference image
that looks offset at a high resolution can reflect stock behavior that is
kept on purpose, not an element to recenter automatically.

The chassis CFG lines `cockpit_rpm` and `cockpit_speed` are a separate,
3D instrument: a pin mesh parented to the chassis at the CFG pose, turned
about its own posed Z axis by `value * factor + offset` limited to
`[low, high]` (rpm, km/h). They never replace or move the overlay pins
(`osd_rpmpin`, `osd_spdpin`), and they live and die with the chassis
(`src/resources/part_runtime.cpp:pose_cockpit_needles`).

The damage gauge is a map of the car, not one colour. The stock font holds
a neutral car outline and one blip per region in one coordinate system,
front up. Each blip shows the worst condition (stock `wear × tear`) of the
installed parts it covers and is not drawn when no such part is installed:
engine parts (category 1) light the drivetrain blips, body panels
(category 2) the body blips, and parts on a corner's wheel slot (type 101 +
corner) light that wheel blip. For the stock damage font, only instances of
script `Quarterpanel` (including subclasses) own the fender blips `q/p/u/v`,
selected by their rest-installed side and front/rear. Unrelated nearby body
parts must not light or change a fender indicator. Other regions keep spatial
aggregation. Use closed mount frames, excluding flap/damage motion, and stable
chassis bounds; do not let detached panels rescale surviving indicators.
This is a host approximation, not recovered native part-to-glyph mapping.
Do not key it on car names or CFG filenames. Custom HUD fonts retain spatial
mapping. Correct the stock `q` tile's longitudinal flip by horizontally
mirroring `p` only when its resource and exact stock UV layout match; never
rewrite the shared atlas or mesh, or force that correction onto custom UVs.

`host_game_options` centralizes driving, video, and gameplay options.
The stock store is `save/game/options`. Applying a resolution live must
synchronize Config, the host mode, framebuffer dimensions, the viewport, and
size-dependent resources. Rereading an old value when a menu opens must not
put the game back to 800×600. A mode equal to the monitor size (for example
1920×1080 on a 1080p screen) gets a borderless window even in windowed mode,
because a framed window of that size cannot fit on the screen.

`EnhancedGraphics` holds the extra options shared with the menus and debug.
Anisotropic filtering uses renderer option 6 (off/hardware maximum, default on),
persisted beside stock options. Apply it explicitly to world colour/detail
samplers and enable bgfx maximum anisotropy; keep UI and depth-comparison
samplers independent. Do not claim selectable 2x/4x/8x/16x levels from a backend
API that only exposes off/maximum.
Every new option native must have a stable descriptor, bounded behavior, a
defined save strategy, and documentation in the shipped patch. Do not
announce a persistent option before testing a restart.

## 14. Audio and video

`host_sfx` handles effects. `host_music` handles playlists. Both use the
`host_audio` SDL3_mixer service: one mixer per group (effects, music, FMV),
each a logical device on the default output with its own gain. SDL mixes on
its own audio thread, so nothing needs a per-frame update;
`host_music_update` only advances the playlist at EOF. Sound owners retain
the shared service through shutdown, since destroying a mixer destroys its
tracks. Effects and stock FMV PCM are upsampled to the mixer rate with
linear interpolation and mono is centred at -3 dB, matching FMOD (#389);
SDL's band-limited resampler would muffle the 11/22 kHz stock samples.
Stock Cinepak FMV PCM plays on the FMV mixer. Its playback position
drives the picture, with continuous wall-clock fallback if audio ends or
fails. No third-party audio SDK is needed to build. Optional
VLC/Windows codec fallbacks keep their library-owned audio outputs.
Engine loops use the scripts' SfxTables.
World sounds are placed voices (#390): `host_sfx_play_at`/`host_sfx_set_place`
give a position (and velocity) in script space, and the world camera set each
frame by `mainloop_frame` (`host_sfx_set_listener`) is the listener. A placed
voice gets a constant-power pan from its direction, an inverse-distance gain
(`reference`, `maximum`, `rolloff`; callers with their own gain law pass
`attenuate = false`) and a Doppler factor on its pitch, and joins the world
group of the effects mixer, which the environment reverb/echo of the active
section processes (garage, city, open track; menus dry). Unplaced voices
(menus, UI) stay centred and dry. The three behaviours are host options 22-24
(`openslrr_options.json`), switched from the SOUND page.
`scene_ambience` plays the `sfx_loop`/`sfx_random` lines of map placements
through `host_sfx`; it owns those voices and stops them when their ground
shell leaves the active world.
Do not substitute a fixed engine sound for an RPM/load curve. Native
script-less cars (traffic, fake racers) have no SfxTables; they use the
speed-modelled host loop of `update_traffic_engine_audio` (distance gain, stereo
pan, mass-dependent pitch, plus the CFG `sfx_loop` radio), bounded to the
nearest four cars, and its voices stop with the car like the SfxTable loops.
Tyre squeal, skid marks and collision sounds are host-driven (SLRR_rev
`vehicle-audio.ts`, `tire-effects.ts`): each released non-traffic car holds one
`sound.rpk` `surface/slide` loop scaled by its most slipping wheel, lays
`skid_marks` quads sampled once per host frame (never per solver step), and
`commit_batch` plays one stock `collision` clip per car and batch from its
hardest impact (160 ms apart). Ground materials are not read yet, so every
surface uses the road clip and mark colour.

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

`java.io.File` writes versioned JSON at the existing script-selected paths
(including career, car, controls and options files). Reads detect version 2
JSON, then version 1 JSON, then fall back to legacy SDAT type 1/type 2 and
older raw host streams. JSON save opening keys are reserved for detection,
rather than treating every raw integer beginning with `{` as JSON. A version
1 or SDAT file under `save/` that a script reads to its last byte is
rewritten as version 2 when it is closed; a partial read (`fileCheck`) keeps
it. New saves cannot be read by the original game or earlier SDAT-only hosts.

Version 2 (`version: 2`, schema in [saves/json-schema.md](../saves/json-schema.md))
names the stock career (`GameLogic.save`), car (`Vehicle.save`, with the
part tree) and skin layouts, and stores any other file as a typed `values`
list. Resources are stored by game-relative pack path and pack-local id, not
by session pack number. The named form is published only when it decodes to
the same stream; otherwise the file is written as `values`. Part subclass
data is attributed through script frames: `Jvm::invoke` reports each TREE
method receiving the open File (`set_script_frame_hooks`). Do not add a
script-specific layout without a matching decoder: reads rebuild the stream
from names, and a missing or mistyped field fails the open.

Version 1 has three arrays. `values` is the ordered script stream of `[type, value]` pairs: `int`,
`float`, `resource`, or `string` (including null). `packs` contains
`[ordinal, game-relative path]`; `blobs` contains `[ordinal, kind, hex bytes]`.
It is read only. Dynamic resources retain the existing limitation: their
metadata/bytes can be read, but they are not reinstantiated by the remounter,
except Painter textures stored as PNG blobs (below).

Script strings are byte strings: JSON U+0001..U+00FF represents the corresponding
byte, not a UTF-8 transcoding of it. The writer escapes non-ASCII/control bytes;
null and empty remain distinct, and the existing `strlen` write behavior is
unchanged. The reader rebuilds the binary stream before script reads, preserving
cross-type reads used by stock Config and skin scripts. Damaged/unsupported
JSON saves make `File.open` return 0 rather than falling through to binary reads. Paths and
atomic single-file publication (including pending in-memory saves after a
failed replacement) keep their existing contracts. This is not a multi-file
career transaction. Regressions: `scripts/run_save_io_tests.ps1 -Suite save_io`,
`-Suite save_json` and `-Suite save_persistence`, after a Release build.

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

`File.write(ResourceRef)` stores a Painter texture as a PNG in its save blob
(the SDAT trailer, or a JSON `blobs` entry, base64 in version 2). `readResID` turns the blob back
into a new Painter texture; a blob the stock game left empty is read from its
side file `<save>.<ordinal>` (JPEG). A texture id that is no longer live keeps
an empty blob, like other dynamic resources, and reads back as null.

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

For frame rate, `--perf-log` splits each second into scripts, physics,
scene collection, shadows, reflections, world and OSD submission and the
present wait, with bgfx render-thread and GPU time and draw counts. The
`--bench-map ValoCity --bench-drive N` run advances a fixed 1/60 s per frame,
so builds compare row by row on the same street until the car's first
crash (README, [KNOWLEDGE_BASE §3](KNOWLEDGE_BASE.md#3-build-run-and-diagnostics)).

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

- [README](../../README.md): overview, build, and features.
- [scripting/runtime.md](../scripting/runtime.md): scheduler, notifications, and modals.
- [scripting/vm-gc.md](../scripting/vm-gc.md): roots, safe points, and finalization.
- [scripting/natives.md](../scripting/natives.md): binding coverage and limits.
- [host utility natives](../scripting/natives.md#rendering-sound-and-cursor-native-utilities): utilities and options.
- [scripting/compiler.md](../scripting/compiler.md): multi-class TUFA compilation and Language natives.

External working references, to be compared at an identified revision:

- `nedobylskiy/SLRR_rev`: reconstruction, formats, physics, and telemetry.
- `EliasFD3S/slrr-reverse`: reconstruction of native APIs and behavior.
- Scripts and classes of the installed game: concrete callers of the contract.
- The original executable and RE tools when the code is not enough.

When a behavior is only supposed, write it as a hypothesis.
When a value or an algorithm is ported, note its source and the revision.
When a contract changes, update this guide and the matching specialized
documentation in the same delivery.


### Shared race primitives (#179)

Selected GroundRef resources own their road graph, cached route, traffic model
weights and traffic restrictions (`race_runtime.cpp`, `traffic_runtime.cpp`).
AI command interpretation belongs to `race_brain.cpp`; its controls feed the
existing Part mechanical solver. Java still owns the race state machine and
outcome. Visible four-wheel native traffic, native racer doubles and scripted rivals
use the shared rigid-body/suspension solver. Native CFG wheel renders must use
the same hub compression, steering and spin as physical contacts; inline wheels
do not require scripted rim slots. Unsupported chassis retain an external-motion
fallback. Released bodies keep static-world response without requiring a live
engine or controller. The 60 Hz host/control tick contains shared 120 Hz vehicle
substeps; each unordered chassis OBB pair exchanges finite-mass impulses once,
then all corrected bodies/wheels are published. Parked physical cars can wake on
contact without a driver. Movable map props and free bodies query all active
physical cars rather than only the controlled one. Pair impacts carry actual
normal impulse and pre-hit motion for mount damage/detachment; legacy wall
callers retain their default reconstruction. Bounded translational sweeping is
not exact rotational CCD. See [the #197 audit](../world/traffic-races.md).
Java model weights, including weight-one registered rivals, determine
the traffic/adversary share under the configured host population cap. Streaming visibility must exclude an entire
car subtree from rendering, physics and triggers without destroying its Java
identity. Incarnations prevent stale traffic/brain references after removal.
See scripting/runtime.md and EXTERNAL_RESEARCH for the pinned v221 sources, host
approximations and validation limits. Registry coverage is not finish/results
validation for any of the three race flows.


Race follow-up: trlight fixtures share one approach-cycle state between traffic
and cached emissive lens geometry. Clock/registration changes invalidate that
geometry; the host thread publishes it, without a worker touching live VM or
scene state. Legacy cue animation must exclude vehicle/Part renders even when
resource flags match a standalone marker. Closed per-car spline routes project
the initial position and choose direction from its heading. See scripting/runtime.md
for the updated real-terrain tests and remaining gameplay/host-model limits.


Performance diagnosis: the debug Renderer tab enables frame profiling and shows
separate traffic/physics/script/GC stages. Vehicle, pedestrian and collision
preparation scopes are subtimes of Physics; render subpasses are inside Flush.
Snapshots are published at the end of a completed measurement window on the host
thread. Read the current career with an isolated save root for reproducible
--bench-career runs; --bench-realtime preserves stall/catch-up behavior. A short
spawn-car/fixed-step smoke cannot establish career or manual-play FPS parity.
