# Driving stutters (#195)

Repeatable Valo City drive, fixed 1/60 s steps, full throttle:

```
slrr_host.exe --game --root <game> --window --no-vsync \
  --spawn-car cars/racers/Baiern.rpk:0x6 --bench-map ValoCity \
  --bench-drive 60 --bench-time 13 --perf-log out.csv
```

## Cause

A promoted map prop (loose body) near the car is swept against the car's body
parts in `collide_loose_with_car` (`src/resources/part_runtime.cpp`). Each of
the loose body's eight corners was swept against every part's **full,
unindexed** collision mesh (`scene_mesh_sweep_sphere` -> `sweep_triangles`).
When a corner's sweep overlapped a dense panel mesh, the conservative-
advancement loop ran over thousands of triangles, costing up to hundreds of
milliseconds in a single physics tick. This is the recurring
`part_physics_advance` stutter.

`before-physics-spikes.txt` is a per-frame breakdown from a diagnostic build
(timers not part of the PR). `carcol` is `collide_loose_with_car`:

```
f=293 tot=804.4 ... loose=803.8 ... carcol=803.8   (one tick, 2 loose bodies)
```

## Fix

- `scene_mesh_sweep_sphere_grid` sweeps through a cached per-part XZ cell index
  (`MeshSweepGrid`, `Record::collision_grid`), testing only triangles near the
  sweep path. Deformed parts still use the direct sweep.
- `sweep_triangles` rejects triangles outside the sweep's XZ footprint before
  the costly closest-point work (also helps the indexed static floor sweep).

## Verification

- `sweep-equivalence.txt`: the grid sweep and the direct sweep were run side by
  side on every corner sweep of a 60 s drive: **904,960 checks, 0 mismatches**
  (fraction, penetration and normal).
- `before.csv` / `after.csv`: `--perf-log` rows, clean base vs fixed build. The
  overlap hitch is intermittent (it depends on a loose body landing against a
  dense panel), so a given drive may not contain it; these rows show normal
  driving physics is unchanged (~0.5-1 ms). The hitch itself is captured in the
  per-frame breakdowns below, from builds with local step timers that are not
  part of the PR.
- `after-physics-spikes.txt`: the `carcol` hitch is gone. Smaller hitches remain
  and are separate follow-ups: `refresh_chassis` runs the stock
  `updatevariables` script when a panel detaches (`refresh`, ~12-26 ms), a
  periodic pedestrian tick cost (`peds`, ~10 ms), and the VM GC pause just after
  the frame.

Note: `frame_max_ms` in the CSVs includes GPU/present wait and varied with GPU
load on the shared machine; the physics-zone numbers are the CPU-side measure.
