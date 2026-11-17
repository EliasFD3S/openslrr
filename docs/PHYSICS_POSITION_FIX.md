# PhysicsRef position readback (#86)

World PhysicsRef objects with a physics shape now return a fresh Vector3 from
canonical GameRefState.pos, the position updated by the free-body simulation.
Previously the stored setMatrix argument always won, hiding subsequent motion.
A body made static after moving must also report its current state.

Shape-less OSD hit rectangles retain their script-space position copy. They use
setMatrix for hotspot coordinates but physicsref_set_matrix deliberately does
not create a physical pose for them. Both paths return independent vectors so
Osd.moveFocus cannot mutate the stored rectangle position through its result.
No Java changes or section-specific exceptions are introduced. This fixes an
existing native readback contract; it does not change integration or collisions.

## Validation

Release x64 compilation succeeds. Tests invoke registered native methods,
create real box state and update its position as the simulation does. They
cover moved bodies, frozen bodies, independent results, and shape-less OSD
coordinates. The state update is deterministic; this is not a solver test.
Main c26ef62: 2 pass / 2 fail. Corrected: 4 pass / 0 fail.
Knowledge references: 219 checked, zero errors.

```powershell
./scripts/run_save_io_tests.ps1 -Suite physics_position
./scripts/run_save_io_tests.ps1 -Suite physics_position -BaselineRef c26ef62
python scripts/check_knowledge_refs.py
```

[Before](../assets/validation/physics-position/before.txt) /
[after](../assets/validation/physics-position/after.txt).
In-game validation remains pending: inspect moving PhysicsRef readback and
navigate menu focus with the arrow keys. There is no in-game screenshot yet.
