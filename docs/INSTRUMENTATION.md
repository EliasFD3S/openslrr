# Runtime instrumentation

Stage 1 provides opt-in structured runtime tracing. Stage 2 adds bounded
lifetime/resource metrics and final JSON snapshots. Stage 3 adds opt-in MSVC
AddressSanitizer and deterministic bounded parser mutation lanes. Stage 4 adds
bounded fixed-step physics telemetry and tick-state replay comparison. Later
filesystem-fault-injection, native-reachability, renderer-media, and
native-coverage stages are not implemented here.

## CLI

`--trace PATH` enables tracing and creates or truncates the explicitly supplied
JSONL file:

```powershell
slrr_host.exe --probe-gc --root C:\path\to\game --trace C:\temp\slrr-gc.jsonl
```

`--metrics PATH` independently requests one final JSON metrics document. It does
not enable or require `--trace`:

```powershell
slrr_host.exe --probe-dyno --metrics C:\temp\slrr-dyno-metrics.json
slrr_host.exe --probe-gc --root C:\path\to\game --metrics C:\temp\slrr-gc-metrics.json
```

There is no default trace or metrics path and no per-frame trace output on
stdout. The host never selects a file in game or reference data; only an explicit
user-supplied path is opened. Treat both paths as output files because existing
contents are replaced. Trace startup fails if its file cannot be opened. Normal
returns and early returns flush and close the trace through the process-lifetime
session guard.

The metrics guard writes on normal and early-return paths, including probe
failures after argument parsing. VM-era returns take the final sample before the
VM provider is destroyed; earlier returns use the outer session guard. Metrics
open/write failures are reported on stderr without replacing the probe's return
status. As with other buffered files, an abrupt process termination is not a
clean-flush guarantee.

## Trace events and bounds

Every trace line is one JSON object with microsecond Unix `timestamp_us`, optional
`frame`, `tick`, and `fiber_id`, plus `section`, `category`, `event`,
`object`, `resource`, `class`, `method`, `signature`, `detail`,
`duration_ns`, `result`, `error`, and numeric `values`. Unavailable frame,
tick, and fiber context is `null`; object identity is an address formatted as a
hex string, never a dereference. Strings are valid JSON/UTF-8, with malformed
input bytes replaced.

The in-memory ring retains the newest 1024 events by default and drops the
oldest when full. Its capacity is configurable through the C++ API and capped at
65536. Each string is capped at 512 bytes and each event at 16 numeric values.
The JSONL file is not rotated by this stage. Native argument values and script
locals are not recorded. Error text can still contain application/resource
paths, so handle traces as diagnostic data.

Current trace producers are:

- `native.invoke`: class/method/signature, receiver identity, duration, and
  success or error. Missing natives and native exceptions retain their original
  throw behavior.
- `section.transition`: adopted or changed section and old/new class detail.
- `gc.start`, `gc.finalization`, and `gc.end`: collection counts and duration.
- `fiber.start`, `fiber.stop`, `fiber.exit`, and `fiber.checkpoint`:
  lifecycle and actual timeslice yields, not every VM instruction/checkpoint call.

Tracing is runtime-disabled and allocation-free at the API entry check until
`--trace` or `diagnostics::start` is used. Defining
`SLRR_ENABLE_DIAGNOSTICS=0` at compile time replaces the Stage 1 API with no-op
inline functions; `--trace` then reports that diagnostics were disabled at
compile time. Metrics remain available in that configuration, with unavailable
context represented as empty/`null`.

## Metrics JSON and bounds

The final document has this shape (values shown are examples, not witnesses):

```json
{
  "version": 1,
  "snapshot": {
    "frame": null,
    "tick": null,
    "section": "",
    "values": {"vm.heap_objects": 0, "dyno.sessions": 0},
    "kinds": {"vm.heap_objects": "gauge", "dyno.sessions": "gauge"}
  },
  "section_snapshots": []
}
```

All numeric values are signed 64-bit JSON integers. `values` contains the current
numbers and `kinds` identifies each as a `gauge` or an event-total `counter`
with saturating arithmetic. Unavailable frame/tick context is `null`; unavailable section context
is an empty string. Output contains no pointer values, object identities, file
paths, script locals, or secrets. JSON strings escape controls and preserve valid
UTF-8, replacing malformed input bytes.

Explicit metric names are limited to 96 ASCII alphanumeric, `.`, `_`, or `-`
bytes. At most 128 explicit/current values and 32 providers are retained.
Counter addition saturates at signed 64-bit limits rather than overflowing.
Section history retains the newest 64 distinct consecutive transitions.

Current production values are:

| Name | Kind | Source |
| --- | --- | --- |
| `vm.heap_objects`, `vm.classes_loaded`, `vm.gc_last_freed` | gauges | Live `Jvm` heap/class/GC state |
| `dyno.sessions`, `dyno.calculations_in_flight` | gauges | Existing dyno registries |
| `fibers.live` | gauge | Scheduler records whose cooperative fiber is alive |
| `osd.records_live`, `osd.records_dead`, `osd.records_total` | gauges | Existing OSD widget records |
| `host.files_open`, `host.pending_writes` | gauges | Open `java.io.File` records and retained failed writes |
| `sfx.voices_active` | gauge | Existing SFX voice statistics |
| `sfx.voices_coalesced` | counter | Existing SFX coalescing statistic |
| `gameref.records` | gauge | GameRef registry, including binding aliases |
| `scene.meshes_cached`, `scene.mesh_overrides`, `scene.collision_overrides` | gauges | Existing scene mesh registries |
| `scene.texture_paths_cached`, `scene.ground_prop_maps`, `scene.static_meshes` | gauges | Existing scene resource registries |

These are counts, not byte-size estimates. Renderer/GPU/FMV internals are
intentionally absent. SFX active-voice sampling uses the existing reap behavior
of `host_sfx_stats`.

## Comparing section cycles

`metrics::capture_section_transition()` snapshots the current values and
diagnostics context. Consecutive identical section names are coalesced. The
diagnostics `set_context`/`set_section` integration invokes it after publishing a
new section, so a traced Garage → Track → Garage run produces separate baseline,
loaded, and returned-baseline samples in `section_snapshots`.

The existing `SectionMachine` publishes section context only inside its Stage 1
tracing branch. Consequently, current CLI section histories require `--trace`;
`--metrics` alone still writes the final document and never enables tracing.
Embedders/tests can publish context and call the capture API directly. Frame and
tick are included when a producer has supplied them; Stage 1 does not yet have
frame/tick producers in every probe.

Compare corresponding values across several repeated cycles and their returned
baselines. A single final sample, or one matching Garage pair, does not establish
leak freedom. Dead OSD records are reported separately from live records so a
stable live count does not hide unbounded retired-record growth.

## C++ API and debug overlay

Include `slrr/diagnostics.hpp` for tracing and `slrr/metrics.hpp` for metrics.
`diagnostics::start`, `flush`, and `shutdown` control the trace sink;
`diagnostics::trace(Event)` submits an event; `snapshot` and `dropped_events`
expose its bounded ring. Context remains thread-local and the trace sink/ring is
mutex-protected.

`metrics::register_metric`, `set`, and `add` manage explicit signed values.
`register_provider`/`unregister_provider` add live registry-backed providers;
provider context pointers are never serialized. `metrics::snapshot` reads the
calling thread's diagnostics context and invokes providers on that thread.
VM/fiber execution and snapshots therefore remain on the host OS thread, and no
concurrent Java execution is introduced. `to_json`, `snapshot_json`, and
`write_snapshot` serialize current/history data. `metrics::reset` clears explicit
values/history without unregistering providers, for isolated probes and tests.

The existing Ctrl+F12 debug overlay has a **Metrics** tab showing current names,
kinds, values, available context, and the retained section-history count/recent
sections. This is a view of the same registry, not a second counter source.

Tests live in `tests/test_diagnostics.cpp` and `tests/test_metrics.cpp`, registered
as `slrr_instrumentation_diagnostics_tests` and
`slrr_instrumentation_metrics_tests`. The shared instrumentation fragment adds
`metrics.cpp` to test support because `diagnostics.cpp` now publishes section
snapshots through it; no shared test harness code is changed.

## Stage 4: physics telemetry

`--physics-telemetry PATH` enables a separate JSONL sink. There is no default
path and no per-tick stdout. The explicit file is created or truncated; the host
does not select an output in game or reference data. Normal-sink startup open
failures are fatal with exit status 2 (a failed physics probe returns 3).
Delayed write/flush/close failures are reported on
stderr. The process-lifetime guard flushes and closes on normal and early
returns, but abrupt process termination is not a clean-flush guarantee.

```powershell
# Headless deterministic scenario; writes its baseline fixed-partition run.
slrr_host.exe --probe-physics --physics-telemetry C:\temp\slrr-physics.jsonl

# Normal game/probe modes use the same option.
slrr_host.exe --game --root C:\path\to\game --physics-telemetry C:\temp\game-physics.jsonl
```

The deterministic probe does not need a game root. It runs one second of scripted
throttle and fixed 60 Hz vehicle updates using the same 0.1 s simulation clamp
and four-update physics admission cap as production. A uniform 120 Hz render
partition and a bursty partition with the same total time must produce identical
60-tick state. A 0.2 s initial stall intentionally loses time and produces fewer
ticks. The probe checks both the positive comparison and that negative control;
it does not render or start interactive gameplay.

### JSONL schema, version 1

The first line is a `run` record and the last is `summary`. A closing `frame`
record follows the ticks admitted for that frame. Every line is one bounded JSON
object. Strings are limited to 160 input bytes; controls and non-ASCII bytes are
escaped, so output is valid JSON without trusting source text.

* `run`: `schema` (`slrr-physics-telemetry`), `version`, `type`,
  `scenario`, nullable `vehicle`, `content_hash`, and `config_hash`, plus
  the active `limits`. The headless probe has fixed synthetic identities. Game
  mode hashes `system/Scripts/lang/System.class` when available; the first car
  supplies its numeric resource identity and raw CFG hash when available. Hashes
  are `fnv1a64:` followed by 16 hex digits. They are change-detection
  fingerprints, not cryptographic authenticity checks. Missing identity/hash
  data is `null`, never fabricated.
* `frame`: numeric `frame`, `render_dt`, `admitted_sim_dt`,
  `discarded_sim_dt`, `admitted_physics_dt`, `discarded_physics_dt`, and
  `time_warp`, all in seconds except the dimensionless frame and warp. The sim
  pair measures the JVM 0.1 s clamp; the physics pair measures admission to the
  four-update accumulator. Residual accumulator time is not discarded.
* `input`: monotonic `timestamp_us`, nullable `frame` and `tick`, strictly
  increasing `sequence`, numeric `device`, `control`, and `value`.
  Production records keyboard transitions and polled XInput transitions.
  Timestamps and render-frame associations are observations, not deterministic
  replay keys. No typed text, key names, controller strings, or user data is
  recorded.
* `tick`: numeric `frame` (or `null` for a synthetic unframed capture),
  fixed `tick` index, `tick_dt`, and `entities`. Entities have a stable
  creation-ordered numeric `entity` ID and numeric `resource`; they are
  serialized in increasing entity order. No pointer or GameRef address is used.
  `active` distinguishes released, ready, non-suspended vehicles. If reliable
  dynamics is unavailable, `available` is false and state objects are omitted.
* Available entity state contains `chassis` (mass, position, velocity,
  momentum, angular velocity, quaternion), `drivetrain` (120 Hz substep count,
  engine angular velocity/running state, gear, throttle/brake/handbrake/clutch),
  four `wheels`, `ground_impact`, and `chassis_contact_impulse`. Wheel state
  includes enabled/contact, suspension length/speed/load, spin/rotation, slip,
  tire forces, rigid-contact impulse, center/normal/patch. Contact impulses are
  normal impulses observed in the final 120 Hz substep of that 60 Hz update;
  non-rigid contacts without an impulse measurement retain the actual zero.
  Non-finite observed floating values serialize as `null` rather than as
  invented finite values.
* `summary`: accepted `frames`, `ticks`, and `inputs`, dropped
  frame/tick/input/entity counts, `truncated`, and accumulated admitted and
  discarded timing. An output-byte stop can drop an otherwise accepted record
  and is reflected by `truncated` and its dropped counter.

Default bounds are 100,000 frames, 250,000 ticks, 100,000 input transitions, 64
entities per tick, 1 MiB per JSON record, and 256 MiB per file. Excess work is
dropped and counted; it is not queued without limit. The C++ `Limits` API can
lower these caps. No script locals, arbitrary resource paths, pointer values,
or secrets are serialized.

### Replay comparison and its scope

```powershell
slrr_host.exe --compare-physics-replay A.jsonl B.jsonl
slrr_host.exe --compare-physics-replay A.jsonl B.jsonl --physics-tolerance 1e-8 --physics-compare-report C:\temp\mismatch.json
```

The comparison uses an absolute floating tolerance (default `1e-9`, accepted
range 0 through 1) for physical state. Integer identity, entity order, contact
flags, gear, and substep counts are exact. It aligns fixed ticks and scripted
input sequence, while deliberately ignoring render-frame ownership, wall/monotonic
timestamps, frame timing records, and summary totals. Run scenario and available
vehicle/content/config identities must match. This lets equivalent fixed-tick
work compare across render partitions without hiding tick loss: a tick-count
difference is a mismatch.

Malformed, oversized, duplicate, non-monotonic, or structurally incomplete input
returns exit status 2 with a useful error (JSON syntax errors include the line).
A state mismatch returns exit
status 4 and a bounded JSON mismatch record (up to 32 entries) on stderr; a match
returns 0. `--physics-compare-report PATH` explicitly writes the same JSON
report for either outcome. Report paths are never inferred from game data.

This is **not full stock-game input replay**. The current mechanism records and
compares host transitions and deterministic fixed-tick snapshots; it does not
reinject OS/controller events, reconstruct script-forced axes, seed every source
of game randomness, or guarantee that a stock script scenario reaches the same
state. The probe's scripted throttle is synthetic. Render timing, dropped-time
policy, and other intentionally non-compared metadata must be inspected
separately when diagnosing a mismatch. The checks cover this host's current
fixed-step kernel only; they do not prove whole-game determinism or bit-identical
results across different CPUs, compilers, or future solver builds.

### Production context and C++ API

`mainloop_frame` now publishes its real host frame to Stage 1/2 diagnostics.
The fixed vehicle update publishes its real 60 Hz tick immediately before the
existing update, so traces/metrics around native work inherit useful context.
Recording occurs after that update and does not reorder solver work, change
accumulator arithmetic, move VM/fibers off the host thread, or add per-tick
logging. Snapshots are only collected when telemetry is enabled.

Include `slrr/physics_telemetry.hpp`. `Session` provides bounded file or
in-memory capture, explicit identity/frame/input/tick methods, and checked
`flush`/`shutdown`. Free functions manage the production process-wide sink.
`VehicleDynamics::telemetry_snapshot` is a read-only state copy.
`validate_replay`, `compare_replay`, and `compare_replay_files` implement
strict bounded input and tolerant tick comparison. `deterministic_scenario`
and `run_deterministic_probe` expose the hermetic headless check.

Tests are registered by `tests/physics-telemetry-tests.cmake` as
`slrr_physics_telemetry_tests`. They cover serialization/nulls/bounds,
render-partition equality, lag and direct mismatch detection, tolerance,
malformed input, and actual file/report output.

## Stage 3: MSVC AddressSanitizer

`SLRR_ENABLE_ASAN` is OFF by default. The normal `build-bgfx-x64` Release
configuration and the pinned bgfx configuration are unchanged. ASan uses a
separate build directory and target-scoped `/fsanitize=address` compile/link
options for `slrr_host`, `slrr_test_support`, and every test executable. Third-party
bgfx libraries are not rebuilt with ASan by this option. No global package or
CMake installation is needed.

Prerequisites are Visual Studio 2022 with the MSVC x64 compiler, the **C++
AddressSanitizer** individual component, and a Windows SDK. Run executables from
an x64 Native Tools developer environment (or otherwise make the matching MSVC
ASan runtime DLLs available on `PATH`). Initialize the repository-local CMake
and pinned bgfx checkout first:

```powershell
& ./scripts/Initialize-SlrrWorkspace.ps1
$cmake = './tools/cmake-3.31.8-windows-x86_64/bin/cmake.exe'
```

In a worktree without `tools/`, use the main checkout's local CMake path printed
by the initialization script. Do not reuse `build-bgfx-x64` for ASan.

Normal configure, host/test build, and full CTest:

```powershell
& $cmake --preset windows-x64-release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build build-bgfx-x64 --config Release --target slrr_host slrr_tests --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $ctest --test-dir build-bgfx-x64 -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Separate ASan configure, complete test build, and full CTest with explicit
nonzero-status enforcement:

```powershell
& $cmake -S . -B build-asan -G 'Visual Studio 17 2022' -A x64 -DSLRR_ENABLE_ASAN=ON -DSLRR_STAGE_BUILD=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build build-asan --config Release --target slrr_tests --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:ASAN_OPTIONS = 'abort_on_error=1:symbolize=0'
& $ctest --test-dir build-asan -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

To build the instrumented host as well, use
`& $cmake --build build-asan --config Release --target slrr_host --parallel`.
It remains an opt-in diagnostic binary; do not stage or promote it as the normal
Release host.

## Stage 3: bounded mutation lanes

`tests/fuzz-tests.cmake` registers four CTest executables in both normal and ASan
builds:

- `slrr_fuzz_arraycopy` checks native arraycopy against a small model, including
  overlap, self-copy, invalid ranges, gap filling, and integer extremes.
- `slrr_fuzz_ptx` exercises PTX headers, mip payloads, JPEG markers, wrapping
  sizes, and truncation. This test target intentionally does not enable stb/JPEG
  decoding.
- `slrr_fuzz_rpk` exercises RPAK dependency counts, TOC entries, table bounds,
  and payload reads using only generated files in the reproducer directory.
- `slrr_fuzz_tufa` exercises TUFA chunks, constant/member/tree/interface counts,
  UTF8 lengths, truncation, and concatenated parser input.

The runner in `tests/test_fuzz_support.*` uses only the C++ standard library;
libFuzzer, coverage runtimes, downloads, and reference game data are not needed.
Each run executes its directed regression corpus and then 384 mutations by
default. `--iterations` is capped at 2048 (zero selects directed cases only),
`--seed` accepts a decimal or `0x` hexadecimal uint32, and generated inputs are
capped at 512 bytes. Mutations start from the valid seed independently rather
than running an unbounded evolutionary loop. Arraycopy destinations are capped
at 64 slots: valid copies that would grow beyond that test-only ceiling are
skipped, so this lane does not authorize or test multi-gigabyte auto-grow.

Run the executables directly after building `slrr_tests`:

```powershell
$repro = './build-asan/manual-fuzz-repro'
& ./build-asan/tests/Release/slrr_fuzz_arraycopy.exe --repro-dir "$repro/arraycopy"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ./build-asan/tests/Release/slrr_fuzz_ptx.exe --repro-dir "$repro/ptx"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ./build-asan/tests/Release/slrr_fuzz_rpk.exe --repro-dir "$repro/rpk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ./build-asan/tests/Release/slrr_fuzz_tufa.exe --repro-dir "$repro/tufa"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

For normal binaries, replace `build-asan` with `build-bgfx-x64`. A different
bounded run can be requested, for example with `--iterations 32 --seed 0x1234`.
CTest uses the build test directory's `fuzz-repro` directory by default. Always
choose a test-output reproducer directory, never a reference or game directory.

Before every target call, the runner writes `slrr-LANE-current.bin`. Successful
cases remove it. A detected failure saves `slrr-LANE-case-NNNNN-seed-XXXXXXXX.bin`
plus a `.txt` metadata sidecar and exits nonzero. If ASan aborts the process, the
current-case file remains as the reproducer even though no sidecar can be
written. File-based PTX/RPK targets read this same generated file; no reference
data is modified. Reproducer output is diagnostic data and may be overwritten by
a later failing run with the same lane/case/seed name.

## Stage 3 hardening and limitations

Directed cases isolate RPK/TUFA attacker-declared `0xffffffff` counts. RPK now
uses overflow-safe dependency totals, rejects dependency/entry counts that
cannot fit their remaining input before `reserve`, checks table-offset addition,
and validates payload offset/size against the actual file before allocation.
Malformed impossible counts therefore produce no TOC entries instead of an
allocation exception. Out-of-file payload reads return an empty vector instead
of attempting the declared allocation. Valid in-file payload behavior is
unchanged.

TUFA now validates constant, member, tree, node, and interface counts against
minimum encoded record sizes and remaining bytes before every corresponding
`reserve`. These malformed inputs return `false` with a chunk-specific error
instead of `bad_alloc`/`length_error`. Existing chunk length checks cover UTF8
and other variable-size payloads. No valid TUFA schema or runtime semantics are
changed.

This stage is deterministic regression fuzzing, not coverage-guided fuzzing or a
proof of parser correctness. It does not implement full stock-game input replay,
persistence fault injection, native reachability, renderer/media instrumentation, or interactive
validation. ASan coverage excludes uninstrumented third-party code and does not
replace the normal Release test lane. The Windows CI workflow contains an ASan
job and uploads generated reproducers/CTest logs on failure; local validation
does not itself establish GitHub-hosted execution.
