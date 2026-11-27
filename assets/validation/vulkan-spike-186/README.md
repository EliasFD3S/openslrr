# Vulkan spike (#186)

Evidence for [rendering/backend.md](../../../docs/rendering/backend.md#bgfx-backend-selection-and-the-vulkan-spike-186).

Machine: i9-13905H (6P + 8E, 20 threads), RTX 4060 Laptop GPU (driver
31.0.15.4692) with Iris Xe, 32 GB, Windows 11, balanced power plan on mains.
Every backend ran on the RTX 4060 (`10de:28e0`). This is not the #358
machine. Build: `main` at 09535c9 plus the spike changes.

- `gpu-tests.txt`: result and key lines of the four GPU suites on
  Direct3D 11, Direct3D 12 and Vulkan.
- `perf-runs.csv`: one row per benchmark run (averages without the two
  warm-up seconds, worst frame, first-second stall) with what else ran on the
  machine before and after it. `perf-log/` holds the `--perf-log` CSV of
  each run. Runs `-r1` to `-r3` are one session, `-r1b` and `-r2b` a
  second one.
- `day-d3d11-d3d12-vulkan.png`, `night-d3d11-d3d12-vulkan.png`: the
  standing start of the drive on the three backends, left to right.
- `day-difference-d3d11-vulkan-x8.png`: absolute difference of the day
  capture between Direct3D 11 and Vulkan, times 8. The clouds and the car's
  reflection move between two runs on any backend.

- `ao-attribution.txt`: the AO targets of the failing `ambient_occlusion`
  case read back on each backend, stage by stage, and the size of the city
  capture differences.
- `edge-classification.txt`: the in-game difference between Direct3D 11 and
  Vulkan with one setting changed at a time.
- `runtime-checks.txt`, `vulkan-garage-debug-menu.jpg`,
  `vulkan-intro-video.jpg`, `vulkan-main-menu.jpg`: garage, intro video,
  main menu, resize and minimise on each backend.
- `shader-build-time.txt`: shader compilation time per profile.

Each run (1600x900 window, MSAA 8x, AO method 1 High, shadows High,
smoothing 2, reflections and clouds on):

```powershell
slrr_host.exe --game --window --root <copy of the game> --no-vsync `
  --spawn-car cars/racers/Baiern.rpk:0x6 --bench-map ValoCity --bench-time 13 --seed 3 `
  --perf-log <run>.csv --renderer d3d11 `
  --replay-path assets\validation\perf-baseline\drive.path --bench-drive 45
```

with `--bench-time 22` for night and `--renderer d3d12` or `vulkan`, the
three backends in turn within each round. To repeat on another machine:

```powershell
powershell -File scripts\perf_baseline.ps1 -SourceRoot game -Out <dir> `
  -ReplayPath assets\validation\perf-baseline\drive.path -Cpus all -Presets saved `
  -Renderers d3d11,d3d12,vulkan -Shots
```
