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

The host thread runs the VM, the frame pump, physics, scene collection and
bgfx submission. bgfx presents from its own render thread, pedestrians step
on their own worker (`PedSim` in `part_runtime.cpp`), and audio and video
decoders keep their library threads.

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
the frame mutates; [MILESTONES.md M7](MILESTONES.md#m7--multicore-frame) plans
how to spread it. The first exception (#266) is the placement loop of
`scene_instances_collect_batches`: it reads data no one writes during the
batch, its jobs list placements per run of visible cells, and the host
thread appends the runs in order. Map prop LOD levels are the one value it
writes, through `std::atomic_ref` (see KNOWLEDGE_BASE.md §5.10). Walker
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
  drive it open to the stop, where a strut holds it against gravity, or shut,
  where it latches. A load that forces a held panel well back from the stop
  beats the strut, so a door left open swings shut when the car pulls away.
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
suspended, a panel opens without world contacts. Its constants are an
OpenSLRR design; the original flap solver is not recovered and SLRR_rev has
no `flap` native.

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
KNOWLEDGE_BASE.md for recovery, retry and testing limits.

The frequency currently declared is **120 Hz**, with an update frequency
declared at **60 Hz** in `vehicle_dynamics.hpp`. `advance(seconds)` uses an
accumulator. Do not bring back "once per frame" integration, and do not
assume that an older 700 Hz discussion describes the current code.
An FPS gain must not speed up simulated time.

Frames do not line up with host ticks. In the Adaptive framerate mode
(`host_framerate_mode`, Advanced Game Options), a car is drawn between its
last two ticks, by the fraction of a tick still left unsimulated
(`gameref_render_matrix`). Classic (60 Hz) draws the last tick. Scripts, collisions and the published node matrix
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
(`-v`, no wind yet), with ρ = 1.225 kg/m³ and typical road-car coefficients,
not values recovered from the original game.
`VehicleDynamics::apply_aero` adds, each substep:

- **Body drag and lift.** The rigid body's bounds (W × H × L) give a frontal
  area `0.85 W H`. Drag uses Cd 0.32 on it, and Cd 1.0 on `0.85 L H` (side)
  and `0.85 W L` (plan) in proportion to the flow's direction cosines, so a
  sliding car meets its side area. Lift uses CL 0.25 on the frontal area with
  the flow along the car. Both act at the centre of the bounds; native
  traffic templates keep their own force policy and get neither.
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
moves load to the rear axle and lifts the front. Every drag force is
clamped to the impulse that stops its point in one step, as the EXE does for
`C_drag`.

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

### Intended driving model: revision `8ac2ba9`

The maintainer's `8ac2ba9601300b36efca54dd81031fb6bc9de98b` revision is the
intended model for PR #48. It supersedes that PR's earlier script-derived
curve and instant-centre load path. It is OpenSLRR tuning, not recovered
stock physics; do not restore the earlier solver just to make its old tests
pass. The follow-up work awaiting approval is in
[the vehicle-dynamics migration plan](VEHICLE_DYNAMICS_PLAN.md).

`configure_drive` copies the tyre script's `WheelRef` values, but the active
curve scales a reference tyre rather than interpreting those values as exact
force-curve coefficients:

- Friction, stiffness and sliction are divided by `TireParameters` defaults
  (1.76863, 17.6839 and 0.814), then clamped respectively to 0.2–2.5,
  0.4–2.5 and 0.3–1.5. Width, compound, profile and inflation enter through
  these script-derived inputs, not through independent width parameters.
- Slip ratio uses the greater of 1.5 m/s, road speed and wheel surface speed;
  slip angle uses road speed with a 1.5 m/s floor. The base lateral Magic
  Formula has B=11.5, C=1.32, E=0.35; the longitudinal one B=14, C=1.55,
  E=0.45. Stiffness scales B, not an exact `stiffness × load` zero-slip slope.
- `setMaxLoad` introduces progressive load efficiency above half capacity.
  An arctangent factor blended by `setLoadSmooth × load_sensitivity` further
  scales the lateral peak. The nominal lateral/longitudinal peak multipliers
  are 1.42 and `0.85 × 1.76863`, before load efficiency, sliding, tuning and
  `setFrictn_x` (longitudinal only); neither is a guaranteed measured peak mu.
- Sliction scales the grip as combined slip grows; longitudinal slip also
  reduces lateral B. A final friction ellipse bounds combined demand.
  Low-slip forces are additionally capped in proportion to slip speed and
  load. `TireTuning` exposes lateral grip/response, longitudinal grip and load
  sensitivity. `setRollRes` supplies the wheel's rolling torque.

Wheel alignment now scales load by the square of the hub-down/road-normal
cosine, with a 0.25 floor. The copied `setCPatch` tolerance (`patch_angle`) is
not read by the solver. Likewise, `setInstantCenter` values and the
`WheelState::jacking` field are retained but have no active load effect:
there is no instant-centre anti-dive/anti-squat force or jacking feedback.
Wheel YPR and the existing arm/hub kinematics still affect contact geometry.
The proposed anti-roll-bar loop is commented out, not implemented. The
original has no effect from stock anti-roll-bar parts either: MWM
`Swaybar.updatevariables` has its `addrollbar` commands commented out. Only
the native CFG `rollbar` lines of fake-racer and traffic templates carry one.

The approved migration keeps that tyre law but corrects brake coupling.
After the drivetrain impulse, one bearing/service/handbrake impulse budget
is consumed before tyre force sees the spin. Its unused portion holds a
locked wheel against the subsequent tyre reaction; it is not a second full
brake application. ABS reduces the service budget and can release an
already-locked wheel while the road moves. Only longitudinal force retains
the per-wheel no-slip-reversal bound; the earlier shared lateral impulse
bound is absent. Automatic first/reverse launch take-up is still raised
only while both service brake and handbrake are below 0.05, not while
holding the service brake. Previous burnout measurements do not validate
this revision. The `8ac2ba9` brake-hold reproducer (traction despite zero
final wheel spin) is covered by forward/reverse and weak/strong brake tests.

The approved support migration replaces the spring-less corner's old
previous-load/post-step-impulse path with a same-substep compliant tyre /
bump-stop load. Fixed-travel corners and up to eight installed-body low
points solve their implicit normal impulses jointly on a predicted body;
the resulting loads reach the tyre law before integration. Stiffness scales
with mass, the installed damper is retained, and a deep-penetration safety
stop remains. An absent scripted compression limit falls back to CFG spring
`base - travel`, not `base` (which could prop up the missing-spring corner).

`rebuild_body_support` extracts actual low mesh vertices across left/right
and four longitudinal sectors. It excludes running gear (already represented
by wheels) and hinged panels (whose floor contact remains unsupported),
respects slot poses/deformation, and refreshes after assembly changes,
damage and repair. Hiding a mesh does not remove its physical support. In an
upright car these samples replace fictitious underside AABB corners; the
old box remains the fallback for callers without samples and after rollover.
Floor-only support does not turn a low panel rubbing the ground into a
chassis wall-impact damage event.

A rim-less corner with only hub hardware uses a metal friction disk (0.4)
and rolling-resistance factor 0.04, not the lost tyre's script values. An
installed outer dually remains a rubber contact. Parked tyres use a 0.8
friction upper bound; body and metal contacts use 0.4. These are host tuning.
Rubber sticking is additionally bounded by the intended model's zero-slip
friction-ellipse axes (`tire_friction_limits`), including script grip,
alignment, progressive load efficiency and lateral smoothing. A zero
longitudinal grip cannot be replaced with free parking friction. The same
capacity equations are shared with the dynamic curve; its forces do not
change. Native traffic keeps its separate force policy.
A projected friction solve acts after forces/gravity but before advancing
the pose, allowing real sticking instead of permanent tiny sliding steps.
A mechanical brake's longitudinal hold additionally cannot spend more than
its remaining brake/bearing impulse budget; lateral grip is independent of
brake torque. The remaining friction ellipse still permits slipping when
support is insufficient; no general chassis sleep state or new wake-state
machine is introduced. `RigidBody::integrate` keeps its existing behaviour;
the vehicle owner uses its force/pose split to insert the constraints.

For the dyno, a new configuration must rebuild the matching curve banks,
including nitro. Do not mix an old nitro bank with a new RPM step or a new
geometry. Bound indices before the integer conversion. The last exact sample
must remain reachable.

Mass distribution and crush zones are separate models. The car's mass,
centre of mass and inertia come from the installed parts' CFG `body` lines
(`assembly_mass_properties`): each line's mass is spread over its own shape (a
box, a sphere, or its phys SCX hull when that is a closed surface, else the
hull's bounds), and the pieces are summed in the chassis frame with the
parallel-axis theorem. The stock game only sums point masses and has no
authored centre; this is a host model, and the products of inertia are dropped
because the rigid body takes principal moments along the chassis axes
(`dynamics_configure` passes them to `RigidBody::configure`; each supplied
axis is floored at 30 % of the full-bounds box moment). In `8ac2ba9` the
explicit-moment overload accepts the vector only when all three components
are positive and finite; otherwise all three axes use the default. That
default uses a box with dimensions 0.75 times the bounds, so its moments are
0.5625 times the full-bounds box values. This generic fallback also affects
loose bodies and native traffic, not just the player car. The collision
bounds themselves are not shrunk. Angular momentum is damped by
`exp(-0.015 × dt)` each substep. Parts
without bodies (stock running gear, rims and tyres) add no mass. Crush zones
are not those meshes: a deformation cage is generated over the part's visible
render shell, as SLRR_rev (77b0fb6, `vehicle-3d.ts`) does, and its cage mass is
the part's own CFG mass (the chassis shell's own, not the assembly's). The phys
mesh stays the part's collider and follows the cage.

A corner loses its wheels when either of two independent conditions reaches
zero: its strut, once the impact energy |J|²/2M weighted by exp(-(d/0.65 m)²) of
the horizontal distance to the corner has added up to 55 kJ, or its mount, once
the chassis cage, settled after an impact, holds the wheel centre or strut top
0.3 m from where they were when the wheel was fitted. The transient crush peak right after an impact does
not count, only the settled (plastic) shape. These are SLRR_rev values
(`releaseFailedWheels`, `suspension-3d.ts` `deform`, `damageImpact`), not stock
ones; floor contacts below 8 m/s do not count. Every part on a wheel slot of
that corner (the rim with its tyre, an outer dually wheel) then breaks off like
a panel and becomes a loose body of at least 14 kg. The chassis is refreshed,
so the corner rests on the hardware still on its hub or, with none, on the
chassis box. The hub hardware stays, and the dented chassis does not move the
remaining wheels' hardpoints. Fitting a wheel to that corner restores a sound
mount: a corner that is still dented keeps its current dent as the baseline,
so the new wheel only falls off after a further 0.3 m of settled crush or 55 kJ
of new impact energy. Repair clears the dent, the baseline and the mount.

The engine breaks off on a much bigger impact. Its mount takes the same impact
energy weighted by 0.5 exp(-(d/1.1 m)²) of the horizontal distance to the engine
and fails at 120 kJ, so 240 kJ at the engine itself (SLRR_rev `damageImpact` and
its block release; not stock values). Earlier Baiern captures retained the
engine at 73–74 km/h and released it near 95 km/h; those approach speeds and
outcomes have not been revalidated with the `8ac2ba9` solver. The engine is
the chassis child that carries the `dynodata` (the stock `Block`), found as
`configure_drive` finds it, and everything installed on it goes too. It leaves
as one loose body with its pre-impact velocity, so it carries on out of the car;
as it starts inside the car's collision shell, it skips the car contacts until
none of its corners is inside the car's bounds (at most 1 s). Whenever a part
breaks off, the reconfigured chassis keeps its velocity and angular velocity, as
SLRR_rev does, not its angular momentum: with less inertia the same momentum
would spin it up (a car that lost its engine pitched over).
The mechanical car then has no dyno, so no drive torque, engine audio or
rpm. `configure_drive` gives it a neutral drive with a zero-torque engine
that never starts; the tyre and brake paths stay active. This is the coasting
mechanism, not a guarantee that strong-brake holding is correct in the new
solver. Mechanical AI input is preserved as well as player input.
Released bodies keep wall collision, damage and distance accumulation
regardless of engine or controller ownership; native traffic uses the separate
`drive.native_traffic` force path without Java engine/gear data. Damage
options still distinguish the player from other cars. Repair restores the
mount but not the engine; fitting an engine again clears the state.

Deformation and detached parts need consistency between the impact, the local
frame, the deformed mesh, the part state, and collision. Check separately:
vibration at rest, permanent deformation, a panel opening, and an attachment
breaking. A visual filter does not fix an unstable solver. A hinged panel
opening from a hit is the hinge of §10, not deformation: the impact releases
its latch and the hinge, not the cage, moves it.

Stock `Part.repair()` owns the wear/tear repair caps and recursively visits the
installed tree. Its native `repair` command performs a one-shot local reset of
that part's deformation, deformed collision/render data, and damage-associated
slot/wheel state, then restores the affected CFG mount frames and base meshes.
Use the script's `attachedParts` coverage and recurse natively only into detached
assembly children that those lists omit; never traverse a child twice. Do not add
a whole-car relink per part, a world reload, or a `GameRef.setState` call. The
latter controls pose and velocities and can release the solver; it is not a damage
repair API.
Keep the latest intentional `setSlotPos` state separate from damage overlays so
repair preserves it even on the same slot. Detached inventory damage must remain
intact. Queue affected link visuals once per repaired part and flush them on the
next existing part update, after the complete scripted walk; do not relink every
ancestor/descendant subtree during each native call.

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

Environment capture excludes vehicle/Part-owned subtrees, including seated
characters. Filtering only `vehicle_material` lets a seated actor be captured
from the probe inside the excluded car and reflected onto its external panels.
Use the render owner and bounded GameRef ancestry; independent pedestrians remain
visible in reflections. This does not hide occupants in the main camera.

Every world camera is a reflection scene: the garage, the dealers and the
city all use the cubemap. The garage probe sits on the car body and its box
is the garage bounds. Without a followed car, a world camera captures from
the car nearest its target, so a showroom car reflects the room around it.
A capture must draw what the main view draws; the garage shell is preloaded
SCX geometry outside the instance list and is drawn explicitly.

A checked "dynamic reflections" option does not prove that the car material
reads that cubemap. Follow the binding into the shader. Check fallbacks to
the old envmaps and the `vehicle_material` flags.

Map shadow visibility is shared by scenery and vehicle shaders through
`shaders/scene_shadow.sh`. Bind it at mesh stage 3 or vehicle stage 5; stage 4
is the vehicle cubemap and must not be overwritten. Captured world meshes and
prop batches receive the same world-space shadow field as the main view.
The field identifies its source (directional or added local light), so only that light's
direct diffuse/specular contribution is occluded. Ambient/environment light
is preserved. Vehicle previews and emissive lamp surfaces disable reception;
the separate car-depth map is not sampled by the same car as a receiver.
Scene.java's `internalScene` is forwarded as scene environment metadata.
Exterior lighting and shadow eligibility must not depend on stock ground POLY
or ground-prop registrations, nor on a terrain mesh being visible. Select the
root-scoped light directly. An interior may have a skydome; its presence alone
must not enable the sun. Preview scenes opt out; callers without section metadata
retain automatic ground-prop detection. Resource reset clears the scene intent.
An inherited unassigned `internalScene` may be absent from the sparse host object;
in that case a live Scene.sun parented to the current world also establishes
exterior intent. Explicit interior metadata takes priority.
The stock club garages derive paired ceiling emitters from their actual SCX
tube geometry (g1 9, g2 10, g3 12). `legacy_neon.hpp` is an explicit asset
adapter. Candidate faces are the `texturak` section of `garazs1.dds` that samples
a known white swatch (g1), or groups with an authored SCX4 emissive colour and no
emission mask (the g2 `neon` and g3 `Material #11345` lamp groups). Only
connected tube-shaped components (2–4 m long, under 0.25 m wide and tall, above
3.3 m) are paired into fittings, which keeps emissive signs out; local anchors sit 8 cm below the tube's
lowest face and follow the instance's transform. The loader splits only tube
triangles into `self_illuminated` groups, preserving their original texture/UVs.
These opaque surfaces remain white without dynamic light or sky ambient; the
rest of the fitting/material stays normally lit. This is runtime material
metadata, not a new serialized SCX version or arbitrary bright-pixel detection.

The live rooted neon owner scopes up to twelve `SpotLight` records: neutral white,
intensity 24 (engine units, not calibrated lumens), range 20 m, a downward hood
with a 65-degree inner and 80-degree outer half-angle. Captured scenery and
vehicle diffuse/specular use the same inverse-square, radial/cone falloff and
per-source shadow visibility. These are broad punctual approximations to the
fittings, not area lights. Local/dynamic-light options and world transitions
clear/reselect the records. Garage fixtures are gated by Local lights (option 4)
only; Map sunlight (option 3) must not switch them off (#233). The 15% legacy ambient remains an explicit fill;
when fixtures are present the point compatibility record supplies ambient only
(zero direct intensity), suppressing the old directional neon contribution.

For unadapted interiors, the previous `PointLight` fallback remains: a local
3.3 m anchor through the owner transform, intensity 18 and range 32 m, preserving
its owner/colour. Missing fixtures must not invent nine lights in other garages.
These fallback settings are compatibility defaults, not recovered positions.

One point-shadow atlas contains six 512-square D32F faces in a 1536x1024 2D
texture (6 MiB depth payload). Face views 0/1/2/3/4/64 precede the world;
`draw_previews` must reserve/reorder view 64 before world 5/6, including inset
sorting. Do not collide with previews 8..63, OSD 70 or reflections 72+.
Perspectives cover 92 degrees for overlap, near .05 m and the light's range.
Caster transforms and face matrices share a light-relative origin. Cutout
materials retain their alpha rules. Skinned casters remain excluded.

`u_pointShadowFaces[6]` and `u_pointShadowParams` select the dominant cube face
in the existing scenery/vehicle depth sampler. Four comparison taps stay in
that face. The radial receiver bias is .015 m. Light radiance uses inverse-square
attenuation with a smooth range window and a bounded near-source denominator.
`u_sunDir.w=-1` selects explicit positional-light ambient instead of the legacy
fixed/directional light. Direct local diffuse/specular is occluded; ambient and
reflections remain separate. Captured scenery samples the same light/shadow field.
Exact unchanged caster/projection state reuses the atlas; light position/range or
caster identity/revision/buffer/transform changes invalidate it. Colour/intensity
updates change shading without redrawing depth.

The fixture path instead allocates a 2048x1536 D32F atlas (12 MiB), at most
twelve 512-square tiles, one downward perspective per fitting. Reserved views
128..139 are ordered before world 5/6 and the sun view 65; reset the full order
0..139, preserving preview/inset/OSD/reflection order. Main/captured mesh and
vehicle programs use fixture sampler 7. Unchanged projection/caster state reuses
all tiles; position, direction, range or caster changes refresh depth, while
colour/intensity updates only affect shading. Fixture PCF aligns four comparisons
with depth texel centres and evaluates each reference depth on the local receiver
plane (derivatives taken before bounds branches). This avoids grazing-angle
self-shadow stripes without a large constant bias, disabling vehicle occlusion,
or increasing atlas resolution/tap count. Bilinear weights reconstruct visibility;
the small residual depth bias is .000005. The fallback cube atlas is inactive
when the paired fixtures are active. Close destroys the extra atlas/uniforms.

Generic fixture authoring, adapters for other garages, outdoor punctual sources,
projector headlights, PBR, HDR/tonemapping, area lights and removal of baked
detail in diffuse textures are still separate work.

Interior daylight uses a separate 1024-square D32F directional shadow map
(4 MiB), on reserved view 65 ordered before world 5/6, alongside the point atlas.
Its direction/radiance use the continuous day cycle; sky ambient never substitutes
for direct sunlight. Scenery, vehicle diffuse/specular and reflection captures
sample this independent field (mesh stage 5, vehicle stage 6). The existing
point-light ambient/falloff is retained. No extra ambient is added by daylight.

The stock garage adapter treats the blended SCX section `ablak` as a sunlight
aperture: actual transformed triangles, authored base UVs and alpha mask. Up to
32 triangles sharing one mask are supported. The mask cuts opaque shell fragments
only within 8 cm of the window plane, and the window surfaces themselves cast
alpha-tested shadows (threshold .5). Walls elsewhere, roof, frames and ordinary
objects remain casters. Unsupported masks/triangles do not open extra shell.
This is a legacy asset adapter, not automatic transmission for all blended glass.
Texture alpha controls binary coverage, not physical coloured-glass transmission.

A separate 1024-square RG32F aperture field stores window light-space depth and
transmission, with its own D32F depth attachment (12 MiB together). View 66 is
ordered after the sun field and before world 5/6, including inset views. Mesh and
vehicle sampler 8 reads it: only projected alpha windows in front of a receiver
admit direct interior sunlight. This prevents thin wall/roof seams from becoming
unintended openings. Opaque-frame/object occlusion still comes from the sun depth
field. Four texel-aligned, receiver-plane comparisons use a .0000005 residual
sun depth bias; physical lighting/reflection coordinates must stay unbiased.
The old 5 cm normal offset is confined to the legacy shadow varying, not
`v_worldPos`, in mesh, instanced and vehicle shaders. Sky draws explicitly reset
material mode; aperture capture mode cannot leak into the visible sky.

An unchanged projection/mask handle/caster stamp reuses both daylight fields.
Clock, geometry and caster motion update it; night, missing apertures or disabled
map shadows/dynamic lighting disable interior daylight safely. Skinned casters
remain excluded. The source and sky share time, but are distinct contributions.
Interior flare artwork resolves through the active root's skydome resource; all
rays/ghosts use camera-to-sun shadow visibility, including window alpha and frames.
It draws as a lens overlay only when that visibility permits it. Preview/emission
passes disable daylight reception, and backend close destroys the extra resources.
Stock g1 fixture placement and white tube emission are implemented above;
other interiors retain the point fallback until they have fixture metadata.

Outdoor receivers now use four camera-centred distance cascades (32/100/320/
1,000 m) in a 4096-square depth atlas, with 2048-square tiles. Projection
translation is snapped in light space; every frame uses the current sun rather
than an angular cache threshold. Blend the last 15% of each inner range into
the next and fade the final level over 900-1,000 m. Views 0/2/3/4 and vehicle
depth view 1 precede world view 5; ordering must keep all shadow passes before world
receivers. The garage instead uses the six-face point atlas described above. These
are radial distance cascades, not camera-frustum-fitted splits. Scenery casters
outside the main view must be uploaded and retained for their shadow submissions.
Collect outer shadow candidates once and cull their reflected-Z render bounds
for each cascade. Skip character palette completion in that collection; do not
change default completion for ordinary/reflection views. Depth draws may merge
adjacent opaque material sections, but must preserve alpha-cutout and blended
index boundaries, rebuilding their metadata with every geometry revision.
Outdoor shadow comparisons align to depth texel centres and evaluate each
reference depth on the receiver plane. Derivatives are evaluated for every
cascade before distance selection. Sharp uses four continuous comparisons,
Soft a 4x4 cubic/tent filter, and Very Soft a broader 4x4 fractional tent. A constant
reference across a wide footprint produces moire/acne on sloped receivers;
inflating a single slope bias is not an adequate replacement. Only sample an
adjacent cascade in its blend interval; clamp every tap inside its own tile.

Sun rotation is evaluated around a nearby shared floating origin. Rebase
caster model translations and receiver positions together; depth vertices must
apply model and view-projection separately, avoiding a precombined large-world
MVP cancellation. A long-drive origin shift must preserve every cascade's grid
phase. Frustum culling remains in absolute render space.
An unchanged atlas may be reused only with identical matrices, origin and
caster identity/revision/buffer/transform records. Never skip a changed sun or
caster through a time/angular threshold. The nearby car-depth receiver uses a
comparison sampler so depth is compared before bilinear filtering, matching
PCF rather than interpolating depth and thresholding afterwards.

This is not yet a complete real-light implementation. See RENDERING_ROADMAP.md
for #201 progress, performance cost and unvalidated scene cases.

### Mesh CPU/GPU ownership

Scene geometry remains the authoritative CPU source for collision, picking,
animation and re-upload. The GPU cache shares that source and retains only draw
metadata, texture bindings and GPU handles. Never deep-copy vertex/index/skin
payloads just to keep a resident draw entry. Reordered material indices are
upload scratch and may be freed once bgfx has copied them. Animated revisions
must read the current source, rebuild metadata/index counts and recreate skin
buffers as needed; deformed replacements preserve compatible texture bindings.

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

Ambient occlusion uses a separate camera-matched reversed-Z D32F prepass in view
140, an AO-resolution geometry pass (linear depth and normal) in view 141,
then R8 horizon evaluation and separable depth-aware filtering in views
142–144 before the main world draw. It does not replace the swapchain or require
a colour post-process chain. Every AO quality runs at half resolution (#248); high
uses more directions/slices (HBAO 10×6, GTAO 5×8) instead of full resolution.
HBAO and GTAO are independent horizon/slice implementations, not vendor SDKs.
Reconstruct sample positions at the exact depth texel centres, including the
centre used by normal reconstruction; otherwise flat receivers develop false AO.
Choose normal derivatives with two-sample projected-depth extrapolation on each
side of the receiver, rejecting unsupported discontinuities. Recover the plane
normal from projected-depth slopes rather than subtracting reconstructed world
positions: the latter loses precision at grazing angles. The separable filter
weights distance to the receiver plane, so tilted surfaces are denoised without
accepting foreground depth discontinuities.
Finite-slice GTAO is normalised against its unobstructed hemisphere. Reject
coplanar/behind-tangent samples: quantised screen samples may leave the nominal
slice and must not occlude a flat road. Clear AO depth to zero and use GREATER
for individual, skinned and instanced depth draws. Use the actual reversed
projection coefficients when reconstructing positions; do not reinterpret the
main-view field as normal-Z. AO radius stays in world metres; a fixed pixel
cap must not shrink it when approaching a receiver.
Per-texel sampling variation is a 4x4 tile: each texel of a block has its own
direction rotation and radial step offset, and the two filter passes use
[1,2,2,2,1]/8 weights, which average exactly one tile period on a flat
receiver. Do not reintroduce fixed step distances or a non-periodic per-pixel
hash (such as interleaved gradient noise) without temporal accumulation: both
leave occluder silhouettes stamped on the ground as parallel streaks.
Only main-view opaque indirect diffuse is attenuated. Direct sun/lamps, emission,
transparent glass, preview/inset views, cubemap captures and UI opt out. Off or
zero strength destroys the AO targets; resizing/quality changes recreate them.
Do not sample an earlier camera's field in another view. Samplers 9/10 are AO/depth.
No temporal accumulation, depth pyramid, bent normals or HDR is implied.

Host shadow quality selects local projection tiles 256/512/1024, interior sun
512/1024/2048, and vehicle depth/exterior cascade tiles 1024/2048/2048.
Resize invalidates the corresponding cached depths. Medium preserves the former
resolution. Smoothing is independent: sharp, soft, very soft comparison filters;
it does not invalidate caster depths or change lighting direction/distance.
All comparison taps preserve receiver-plane correction where present. For fixture
and interior sunlight, Sharp uses a narrow continuous four-tap edge (never raw
nearest-texel visibility), Soft uses bilinear four-tap coverage, and Very Soft
uses a fractional 4x4 tent. Do not snap the wider filter to the nearest depth
texel: this creates square coverage jumps. Fixture taps stay inside their atlas tile.
Outdoor cascade receiver-plane gradients are clamped per cascade to a 20-unit
world slope converted to normalized depth (`extent/(extent+768)`, #233). A pixel
quad that straddles two surfaces (pole on ground, kerb, wall/floor crease) must
not extrapolate wide filter taps against distant depths. Fixture and interior-sun
gradients are not clamped yet.
Options
persist alongside host graphics; OptionsDialog native IDs 7–12 and debug UI agree.

The active world renders to the swap chain. No offscreen scene-copy or HiZ depth
pyramid is wired into the frame path; the unused implementations are removed.
The optional scene size filter only rejects small projected bounds and does not
read occlusion depth. Keep it disabled for inset, shadow and reflection views.
Program creation owns and releases its input shader references on every outcome;
rejected shader pairs must not leak handles during opening or reopening.

Do not collect casters using only the visible camera frustum.
The list must respect the useful light volume. When the time of day changes,
sun-dependent matrices and caches must be invalidated.

Skinned characters (pedestrians, seated drivers) cast into the outdoor sun
cascades only. They come from the main-view records, whose bone palettes are
already complete; the outer shadow collection keeps skipping palette
completion, so a character outside the camera frustum or beyond 100 m casts
nothing. Rest-pose bounds do not describe animated limbs: cull them by the
cascade radius, not by mesh bounds. Their caster stamp hashes the bone palette,
so a pose change redraws the atlas. Point-light and interior-daylight fields
still exclude skinned casters.

The renderer factory always selects libbgfx for Bgfx in the supported build.
NullBackend is retained for headless probes and tests; there is no separate
WGL fallback. Image and font decoders do not depend on an optional renderer
macro. The Windows x64/MSVC toolchain requirement remains in CMake.

### Sky and lights

The Enhanced sky must be driven by game time and by parameters consistent
with the sun, the clouds, and the reflection. A night-time appearance must
not leave a noon sun in the materials or in the shadow.
Animated cloud parameters need a stable time, distinct from a count of
elapsed frames.

OSD text uses a dedicated position/UV layout and unlit shader pair. A text
line submits one indexed batch per available transient-buffer chunk, preserving
font, colour, layer and draw order. Do not route glyphs through world-lighting
bindings or merge labels across the plates/icons that separate them. Long lines
must rebase their 16-bit indices while retaining pen advance. See
`src/render/osd_text_batch.hpp` and `LibBgfxBackend::draw_osd_line`.

Font loading prefers `frontend/fonts/<stem>.ttf`, then `.otf`, with the legacy
SCX/atlas loader as fallback. `scripts/bake_slrr_fonts.py` converts user-owned
stock assets to outline TTFs, keeping the byte map, authored advances and top
origin (zero hhea ascent). Ordinary replacement fonts use their hhea ascent as
the baseline offset. Both outline formats rasterize to padded SDF atlases through
the pinned dependency's stb_truetype; one shader composes fill, outline and
shadow without extra glyph passes. Font assets/cache lifetime remain shared and
weakly indexed as before. `slrr_text_set_effects` overrides a line's bounded
outline width, shadow offsets and ARGB colours; effects do not affect measure,
alignment or advance. They are not persistent options or Java APIs. Legacy
atlases retain their baked effects and bypass SDF composition. The byte-based
text contract is unchanged; this does not add Unicode shaping or kerning.
Only trusted local font files are supported: SFNT table bounds are checked, but
stb's parser does not provide comprehensive hostile-font validation.

OSD surfaces have their own blending contract. Do not apply world lighting
to them, or a global correction meant for the body.
For trees and glass, check alpha test, blending, depth write, pass order,
and mipmaps before modifying the original textures.

### Script light sources (#235)

Scripts create lights with `java.render.LightSource` and its subclasses
`PointLight`, `SpotLight` and `RectLight`. These classes are new: they are
OpenSLRR additions shipped in `patch/system/Scripts/render`, not stock API.
Car head- and taillights stay engine-driven and do not use this API. Since
#279 they light the scene from the car data: each part owns the lights its
cfg places as `render <type-13 light>` children, one headlight beam (first
`flags 4` light of type `spot`) and one brake light (first `flags 1` light)
at most (`src/resources/part_runtime.cpp:part_car_lights`). A light exists
only while its part is installed on a car whose engine runs and its part
condition (wear × tear) is above 0.25; headlights follow the city-lamp night
window and the brake light the brake pedal, like the part's lamp glass
(`part_lamp_lit`). The renderer draws them as spot lamps ahead of the street
lamps in the lamp array; since #283 up to six headlight beams (the followed
car's first) cast shadows from their own atlas.

| Native (owner) | Descriptor | Effect |
| --- | --- | --- |
| `LightSource.create` | `(Ljava.util.resource.ResourceRef;I)V` | Registers the receiver as kind 0 point, 1 spot or 2 rect, attached to the parent's canonical GameRef. A parent without GameRef state, or another kind, creates nothing and logs `[light]`. A second call restarts the light with defaults. |
| `LightSource.setPosition` | `(Ljava.lang.Vector3;)V` | Parent-local position, metres, script axes (as `RenderRef.setMatrix`). Each axis is clamped to ±1e5. |
| `LightSource.setDirection` | `(Ljava.lang.Vector3;)V` | Parent-local direction of travel, normalised. A zero vector is ignored. |
| `LightSource.setColor` | `(I)V` | `0xRRGGBB`; alpha ignored. |
| `LightSource.setIntensity` | `(F)V` | Engine units, clamped to 0–1000. |
| `LightSource.setRange` | `(F)V` | Metres, clamped to 0.2–500. |
| `LightSource.setShadows` | `(I)V` | Stored for every kind; only the point light honours it today. |
| `LightSource.setEnabled` | `(I)V` | Enables or disables the light. |
| `LightSource.destroy` | `()V` | Removes the light; later calls on the object are ignored. |
| `SpotLight.setCone` | `(FF)V` | Inner/outer half-angles in degrees; outer clamped to 1–89, inner to 0–outer. |
| `RectLight.setSize` | `(FF)V` | Width and height in metres, clamped to 0–100. Stored only. |

Every setter ignores non-finite or mistagged arguments and keeps the previous
value. Defaults: position 0, direction (0,-1,0), white, intensity 24, range
20 m, cone 65/80°, shadows and enabled on.

Lifetime. The registry (`src/resources/script_lights.cpp`) is keyed by the Java
object and is a GC root provider: a registered light and its parent stay alive
until `destroy`, removal of the parent GameRef (`gameref_remove`), or a world
reset (`gameref_clear`, or a new VM registering the natives). Lights are not
saved.

Rendering. `LibBgfxBackend::update_local_light` reads the registry every frame,
only in the garage with Local lights (option 4) on (scenery lights outside
garages are described under "Scenery lights (#237)"). The parent's drawn
script matrix M gives the render-space position S·M·p (S flips Z), the same
convention as the instance transform S·M·S applied to mesh vertices. Spot and
rect lights become fixture `SpotLight`s, at most 12, in creation order; the
outer angle is clamped to 65.5–89° because the shader's inner edge is fixed at
65°, and every fixture shares the fixture shadow atlas. A rect light is drawn as
a spot. The first enabled point light becomes the point record and keeps the
neon's ambient fill. Any registered light whose parent is in the drawn world,
enabled or not, disables the stock g1 anchor adapter (`legacy_neon.hpp`), so a
script can turn the garage dark. Without registry lights the adapter path is
unchanged. While script lights own the scene, including when all of them are
disabled, the neon point record stays ambient-only (intensity 0, no shadows).
The patched Garage creates one spot per twin-tube fitting in garages 1–3 (9, 10
and 12 lights, intensity 30, range 26 m, set per garage in its fixture tables);
the open-air ROC garage has none. The stock `misc.garage:0x001D` neon remains an ambient-only record
whenever fixtures exist.

### Scenery lights (#237)

Map objects place stock light resources as `render <id> <pos> [<ypr>]`
children followed by `flags <group>`. A resource is a light when its RPK entry
has typeof 13 (`RESTYPE_RENDER_LIGHT`); never infer it from names or
keywords. `parse_light_resource` reads the type-13 text payload. Point and
spot light the scene; `fakepoint`/`fake point`/`fakespot` are glow only and
never add scene lighting.

Each ground shell owns the light children of its map's placements in its local
script space (`scenery_lights_attach`). They leave the registry with the shell
(`gameref_remove`) or on a world reset, have no Java object and are not saved.

Outside garages, with Local lights (option 4) on, lights of the city group
(bit 4) light the scene at night (`scene_sun_is_night`); other groups stay
off. The renderer keeps lamps whose capped light sphere meets the view
frustum, the nearest 64 within 500 m, and fades each to zero before the cut,
so no lamp appears or disappears with power. They have their own unshadowed
lamp array (64 entries, separate from the 12 garage fixtures) with the stock
D3D attenuation 1/(c + l·d + q·d²), range capped at 45 m and intensity 0.8 ×
diffuse, evaluated in the mesh and vehicle shaders. Scenery spots are drawn as
omni lights, and flares are not drawn. Garages keep the fixture path of #235
unchanged.

With shadows on, the 6 lamps nearest the followed car (else the camera),
whatever the camera direction, cast shadows: one downward
150-degree perspective tile each in the fixture atlas (free outside garages),
sized by Shadow quality and filtered like the fixtures, faded out at the tile
rim. Static scenery casts from a list collected when the lamp gets its slot
(not the ground POLY, not the lamp's own housing); vehicles and characters
cast every frame, and a tile is redrawn only when its casters change. A lamp
keeps its shadow slot until a clearly nearer lamp (75 % of its distance and
4 m nearer) has waited 1 s; shadow strength fades in and out over 0.8 s, so
turning the camera never moves a car's shadows and no shadow pops. In
addition, every lamp whose light reaches the player car (up to 16) casts the
car's shadow from a small car-only tile holding just the vehicles within 6 m
of it, so the car always has a shadow from each lamp that lights it; that
shadow fades in over 10 m where the lamp's light on the car is near zero. The intensity values above were accepted in game by the user.

### Renderer debug overrides (#243)

The OpenSLRR Debug menu (Ctrl+F12, Renderer tab) can switch renderer passes
off and change budgets for performance work. These overrides live in
`render_debug()` (`src/render/render_debug.hpp`), default to the normal
renderer, are never saved and reset on restart. The renderer must read them
where the pass is decided, so that an override removes the pass's view (no GPU
time) rather than drawing it empty. Options that already exist (Enhanced
graphics such as AO method, shadow quality, MSAA, clouds; the overlay's map
and vehicle shadows, OSD, flare and reflection faces) are edited in place: the
Enhanced ones are saved as before, the overrides only further disable them.
A new pass should get a toggle, a named view in `render_debug_view_name` and a
profiler zone when it has measurable CPU cost.

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

`host_sfx` handles effects. `host_music` handles playlists. Both use one
`host_audio` FMOD Core system with separate effects/music channel groups;
`host_music_update` updates it in the normal and modal frame pumps even when
music is stopped. Sound owners retain the shared system through shutdown.
Stock Cinepak FMV PCM uses a third, independent FMOD group. Its playback
position drives the picture, with continuous wall-clock fallback if audio
ends or fails; the FMV pump also updates FMOD during boot intros. Optional
VLC/Windows codec fallbacks keep their library-owned audio outputs.
Engine loops use the scripts' SfxTables.
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

`java.io.File` writes versioned JSON at the existing script-selected paths
(including career, car, controls and options files). Reads detect JSON first,
then fall back to legacy SDAT type 1/type 2 and older raw host streams. JSON
save opening keys are reserved for detection, rather than treating every raw
integer beginning with `{` as JSON. Merely reading a legacy file does not
rewrite it; its next normal save writes JSON.
New saves cannot be read by the original game or earlier SDAT-only hosts.

The version 1 object has `format: "OpenSLRR-save"`, `version: 1`, and three
arrays. `values` is the ordered script stream of `[type, value]` pairs: `int`,
`float`, `resource`, or `string` (including null). Finite floats use round-trip
precision; `floatBits` stores a nonfinite float's unsigned 32-bit pattern.
`packs` contains `[ordinal, game-relative path]`; `blobs` contains
`[ordinal, kind, hex bytes]`. Resource values retain SDAT's file-local IDs, not
session pack numbers. Dynamic resources retain the existing limitation: their
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
career transaction. Regressions: `scripts/run_save_io_tests.ps1 -Suite save_io`
and `-Suite save_persistence`, after a Release build.

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
(the SDAT trailer, or a JSON `blobs` entry). `readResID` turns the blob back
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
not exact rotational CCD. See [the #197 audit](TRAFFIC_RACE_AUDIT.md).
Java model weights, including weight-one registered rivals, determine
the traffic/adversary share under the configured host population cap. Streaming visibility must exclude an entire
car subtree from rendering, physics and triggers without destroying its Java
identity. Incarnations prevent stale traffic/brain references after removal.
See SCRIPT_RUNTIME and EXTERNAL_RESEARCH for the pinned v221 sources, host
approximations and validation limits. Registry coverage is not finish/results
validation for any of the three race flows.


Race follow-up: trlight fixtures share one approach-cycle state between traffic
and cached emissive lens geometry. Clock/registration changes invalidate that
geometry; the host thread publishes it, without a worker touching live VM or
scene state. Legacy cue animation must exclude vehicle/Part renders even when
resource flags match a standalone marker. Closed per-car spline routes project
the initial position and choose direction from its heading. See SCRIPT_RUNTIME
for the updated real-terrain tests and remaining gameplay/host-model limits.


Performance diagnosis: the debug Renderer tab enables frame profiling and shows
separate traffic/physics/script/GC stages. Vehicle, pedestrian and collision
preparation scopes are subtimes of Physics; render subpasses are inside Flush.
Snapshots are published at the end of a completed measurement window on the host
thread. Read the current career with an isolated save root for reproducible
--bench-career runs; --bench-realtime preserves stall/catch-up behavior. A short
spawn-car/fixed-step smoke cannot establish career or manual-play FPS parity.
