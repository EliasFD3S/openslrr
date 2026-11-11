# VM garbage collection

The C++ host owns TUFA/TREE objects through `Jvm::heap_`. It now uses a
non-moving mark/sweep collector. This is an implementation for this VM,
not a claim that the original executable used this exact algorithm.

## Native contract

- `System.gc()I` queues a collection and returns -1 while pending. After
  the host safe point, a subsequent call reports the number freed.
- `Object.disableGC()` pins the receiver and its reachable graph.
  `enableGC()` clears that protection. Both are idempotent, not counters.
- Unreachable objects with a finalizer are finalized once before reclamation.
  Roots are traced again afterwards to support resurrection. New allocations
  made by finalization survive the current cycle.
- `System.runFinalization()` returns zero, matching the reference native.
- `Object.hashCode()` has a stable per-allocation identity. `Object.toString()`
  returns its class and identity. `String.finalize()` clears its payload.

## Safe point and roots

Collection runs after an outer host frame, never inside a Java/native call,
object construction, nested modal pump or another collection. Explicit calls
are deferred. Allocation pressure also requests collection (initial threshold:
8192 new objects; it grows with the surviving heap).

Roots include class statics and pinned objects, plus explicit providers for
scene handles, installed/inventory part records, UI widgets/text/fonts, open
files, monitors, active sections, mechanic state, hover callbacks,
notifications, callback registrations and live script threads. Dyno sessions
are weak native payloads: only an in-flight calculation temporarily roots its
receiver. Finalization removes the session; world reinitialization clears the
registry. See `src/physics/dyno_model.cpp` and the focused dyno tests.
Suspended cooperative stacks retain scoped roots for TREE locals/operands,
pending receivers and arrays, invocation arguments, and constructed objects.
The scheduler saves/restores VM call-depth state when switching contexts; a
quiescent host safe point can collect while other stacks are suspended. Object fields and array slots are traced recursively, including
cycles. Native addresses are checked against the managed heap before use.

Native ownership is deliberately conservative: an object retained by a native
registry stays alive until that registry releases it. The collector does not
automatically evict graphics caches or replace missing resource destruction.
New native systems retaining Java pointers must register a root provider.

`--probe-gc --root ..\game` is a headless diagnostic for cycles, static/native
roots, arrays, pin/unpin semantics, finalizer resurrection, reentrant deferral,
explicit requests and allocation pressure. `[gc]` lines report reclamation.
It does not start the game or open a window.

## References consulted

- `repos/slrr-reverse/engine/Runtime/System/System_natives.cpp` documents
  `System.gc` (0x0047C060, delegating to 0x00417F80) and the zero-returning
  `runFinalization` (0x0047C070). Its host GC implementation is a placeholder.
- `repos/slrr-reverse/engine/Runtime/IO/IO_part2.inc` documents the idempotent
  enable/disable flag (0x00408780 / 0x00408720).
- `repos/SLRR_rev/docs/formats/tufa.md` distinguishes the source declarations
  from the installed classes and does not establish a complete GC algorithm.

Notification/callback dispatch and cooperative thread scheduling are described
in [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md). Collision activation and the other
missing game subsystems remain separate work. Native code retaining managed
locals across a cooperative yield must use `Jvm::RootScope` or another explicit
root provider.
