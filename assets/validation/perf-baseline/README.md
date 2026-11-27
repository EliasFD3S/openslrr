# Performance baseline (#358)

- `drive.path`: a 135 s drive through Valo City recorded by Elias with
  `--record-path` (4.65 km: downtown with pedestrians, residential streets,
  the expressway and the underpass). It is replayed by `--replay-path`.
- `route-night.jpg`: replay captures at 0, 30, 60, 90, 110 and 130 s, at night.
- `replay-expo-off/`: `scripts/perf_baseline.ps1` on that replay, one CSV per
  run, plus `summary.csv` / `summary.txt` (averages without the two warm-up
  seconds).

Machine: i7-13700KF (8P + 8E, 24 threads), RTX 4080 16 GB, 64 GB DDR5
running at 4000 MT/s (EXPO off), Windows 10 22H2, 2560x1440 window.
Build: `main` at 25d8fd7 plus the #358 benchmark changes.

Reproduce:

```powershell
powershell -File scripts\perf_baseline.ps1 -SourceRoot game -Out <dir> -ReplayPath assets\validation\perf-baseline\drive.path
```

Two replays of the same build differ by about ±2 % in draws and by less than
0.4/255 mean pixel difference: traffic and pedestrians still move a little
differently. The route and camera path are identical.
- `issue359-cascades/`: the same replay by day after the sun cascades moved
  to worker encoders (#359), saved and low presets on every CPU profile.
- `issue359-batch-merge/`: the same, after the parallel batch merge and the
  `sun_collect` zone (#359, second increment).
- `issue359-world-split/`: `before` (prop-batch merge threshold only) and
  `after` (opaque world on workers, 16 encoders), measured in the same session
  (#359, third increment).
- `issue361-cascade-alternation/`, `issue361-occlusion/`: #361, before/after
  in one session (the occlusion prototype was parked).
- `issue362-gpu-passes/`: per-pass GPU times (#362). `overhead-*` checks the
  bgfx profiler cost, `settings/` runs single-setting variants by day,
  `ao-split/` separates the AO prepass from the AO passes.

Since #362, `--perf-log` writes one `gpu_<pass>_ms` column per renderer pass
(`render_debug.hpp:render_gpu_pass`), and `perf_baseline.ps1 -Presets` also
takes `msaa4`, `noao`, `hard` (shadow smoothing 0) and `norefl`, each changing
one setting of `saved`. Close any other 3D application first: a game left
running kept the RTX 4080 at full clock and made GPU times swing between
about 3.8 and 5.8 ms from one run to the next.
