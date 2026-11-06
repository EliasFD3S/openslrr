# Cooperative scripts and GameType events

This implements priorities 1 and 3 of the native inventory. The loading-reset
and loader-priority natives are unchanged.

## Threads

`Thread.init`, `start`, `stop`, `isAlive`, `sleep`, `suspend` and `resume` now
manage real cooperative executions. Windows fibers retain the C++ interpreter
and native-call stacks, allowing nested Java calls to resume at the exact
instruction after a sleep or wait. All VM execution stays on the host OS
thread; there are no concurrent Java worker threads.

- `start` queues `target.run()` for a host frame. It does not execute it inline.
- `isAlive` covers runnable, sleeping, waiting and suspended threads. It becomes
  false on completion or a stop request.
- `sleep` uses milliseconds and a steady-clock deadline. Zero yields until a
  later scheduling pass. Outside a script thread it waits through the existing
  host pump when available.
- `suspend` prevents scheduling without losing the stack; `resume` restores
  eligibility without overriding an outstanding sleep deadline.
- `stop` requests cancellation and wakes a suspended/sleeping thread. The next
  resume/checkpoint unwinds interpreter scopes before releasing the fiber.
- TREE checks its time slice every 2,048 operations. Each resumption gets about
  1-16 ms according to `Thread.setPriority` (-10 to 10; nonpositive priorities
  retain a 1 ms slice). The scheduling pass has an 8 ms soft budget, raised
  to 16 ms while a MAX_PRIORITY thread is ready, and rotates eligible threads.
  Ordinary native C++ work cannot be preempted mid-call.

`Object.wait` yields within a script fiber and uses the existing modal pump on
the host context. Waiters are tracked separately: `notify` wakes one waiting
execution, `notifyAll` wakes all. MessagePort waits return immediately if a
message is already queued. Waking a message port never clears dialog modal
flags; dialog completion removes only its own modal shield. VM/GC depth counters are switched with each stack;
scoped roots retain suspended locals, operands, arguments and construction
state. See [VM_GC.md](VM_GC.md).

The mechanic/time-display compatibility ticks are skipped while their Java
threads run. The showroom retains host camera selection, but the pose comes
from `CarSelect.run` when its thread is alive. Sections with a running message
thread no longer also drain that port through the host fallback. Tuning slider
events are queued to a live part thread, with synchronous delivery retained
only when no script thread consumes its port.

## Notifications and callbacks

Both `addNotification` overloads, `remNotification`, `registerCallback`,
`unregisterCallback` and `unregisterCallbacks` now have implementations.

Notifications identify their source by its canonical GameRef instance, so
aliases match. Adding the same listener/source/type replaces the old watch;
removal matches the exact type. Dispatch supports event-mask filtering,
low-28-bit event aliases, custom messages (`0x10000000`), custom method names,
skip-related (`0x40000000`) and one-shot (`0x80000000`) flags. One-shot watches
are removed before invoking script code, including on a mask miss. Named
methods take the stock GameRef/event/int or GameRef/event/String signatures;
the default selects `handleEventAsync` when the listener enables async mode.

The event emitter is connected to existing timer and mechanic part-pointer
events. This does not create missing collision/trigger/traffic producers; those
systems must emit their events when implemented. Legacy UI input paths remain.

Callback modes 8, 9 and 28 correspond to `control(float)`, `drive()` and
`animate()`. They have separate ordered lists, idempotent registration and
per-object unregistration. Snapshot iteration plus registration IDs permits
callbacks to remove/re-register entries without invoking stale registrations
later in the same pass. Nested modal pumps do not re-enter callback dispatch.
The host's existing frame delta is passed to control; this is not a port of
the original engine's complete simulation/substep pipeline.

Live thread objects, notification endpoints and callback receivers are GC
roots. Resource destruction removes associated watches/hooks; VM teardown
cancels suspended executions before freeing their stacks and registries.

## Section presentation

`GameLogic.changeActiveSection` and GameState `exit` hold world presentation
while their cooperative stacks are suspended. The last complete frame stays
visible until the transition finishes; explicit loading overlays can still
be presented. This prevents a destroyed CarLot camera from exposing the free
camera and partially detached cars. The scope unwinds on cancellation as well.

## Validation

Temporary headless diagnostics exercised:

- A real TREE `GameType.run` message loop, sleep, suspension, resumption,
  cancellation and collections while its frames are suspended.
- Finite-thread completion, zero-duration yields, separate waiters and a TREE
  loop without sleep that remains preemptible, collectable and stoppable.
- Callback order, duplicate registration and removal during dispatch;
  notification aliases, custom messages, replacement, one-shot/removal and
  native registration entry points.
- Stock Init followed by cooperative frames and repeated collections.
- The actual `Mechanic.handleEvent` tuning path with stock styles and
  rectangle synchronization: slider clicks reach the part, and only explicit
  OK closes the dialog. Assignment paths such as `dlg.osd.globalHandler = part`
  evaluate the intermediate instance receiver before writing the field.
- Cold CarLot navigation using the six local saved vehicles: the first
  transition dropped from about 1.7 s to 0.84 s in the headless probe. This
  excludes GPU uploads and does not imply instantaneous cold loading.
- Fuel-rail pointer hit-testing and dragging through a live part message
  thread while a modal dialog stays open until explicit OK.

These checks do not launch a game window. Full interactive garage/showroom
behavior and compatibility with mods still require in-game verification.

## References

`repos/slrr-reverse/engine/Runtime/System/GameType.cpp` documents watch flags,
replacement/removal and callback modes. `System_natives.cpp` documents the
budgeted cooperative pump and sleeping VM threads. The source declarations and
actual callers in SLRR_rev/the installed TUFA classes supply the script
contracts. The fiber scheduler is the host implementation, not a literal
copy of the original VM's internal stack layout.
