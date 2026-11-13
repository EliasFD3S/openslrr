# Runtime instrumentation

For complete per-run stdout/stderr capture, manifests, and crash dumps, use
`--log-all DIR`. It is mutually exclusive with `--trace` and `--metrics`; see
[CRASH_LOGGING.md](CRASH_LOGGING.md) for files, guarantees, and privacy limits.

Stage 1 provides opt-in structured runtime tracing. Stage 2 adds bounded
lifetime/resource metrics and final JSON snapshots. Stage 3 adds opt-in MSVC
AddressSanitizer and deterministic bounded parser mutation lanes. Stage 4 adds
bounded fixed-step physics telemetry and tick-state replay comparison. Stage 5
adds test-controlled filesystem/persistence fault injection. Stage 6 adds native
coverage/reachability reports and a reproducible static binding audit. Stage 7 renderer, media, input, and contact instrumentation is documented below.

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

`--native-report PATH` independently writes native coverage/reachability JSON;
neither `--trace` nor `--metrics` is required:

```powershell
slrr_host.exe --probe-gc --root C:\path\to\game --native-report C:\temp\slrr-gc-natives.json
slrr_host.exe --probe-dyno --native-report C:\temp\slrr-dyno-natives.json
```

The native report guard has the same normal/early-return and error-reporting
policy as metrics. An early return before registry construction writes a valid
empty report. With `--log-all DIR`, the run directory takes precedence over the
report PATH argument: the report is `DIR/native-report.json`, listed as
`native_report` in the manifest. No separate report is written at the overridden
PATH. Existing `--trace`/`--metrics` exclusions with `--log-all` are unchanged.

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
- `file.fault.injected`: operation/fault kind, requested and admitted byte counts,
  and failure versus partial outcome. Paths and file payloads are never included.

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
| `host.file_faults_injected` | counter | Stage 5 injected persistence faults; zero when no rule has triggered |
| `natives.registered`, `natives.called`, `natives.noop` | gauges | Live registry bindings, distinct tracked attempted signatures, and explicit no-op bindings |
| `natives.missing` | counter | Failed native lookup invocations, including calls whose missing record exceeded the cap |
| `sfx.voices_active` | gauge | Existing SFX voice statistics |
| `sfx.voices_coalesced` | counter | Existing SFX coalescing statistic |
| `gameref.records` | gauge | GameRef registry, including binding aliases |
| `scene.meshes_cached`, `scene.mesh_overrides`, `scene.collision_overrides` | gauges | Existing scene mesh registries |
| `scene.texture_paths_cached`, `scene.ground_prop_maps`, `scene.static_meshes` | gauges | Existing scene resource registries |

These are counts, not byte-size estimates. Stage 7 adds the renderer, GPU, FMV, media,
and input values documented below. SFX active-voice sampling uses the existing reap behavior
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
sections. A compact native line summarizes registered/called/explicit-noop/missing
counts and warns that body registration is not behavioral verification. This is
a view of the same registry, not a second counter source.

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

### JSONL schema, version 1 and Stage 7 version 2

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
  invented finite values. Schema version 2 adds the spawn pose and bounded
  contact-identity records specified in Stage 7 below.
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
& $cmake -S . -B build-asan -G 'Visual Studio 17 2022' -A x64 -DSLRR_ENABLE_ASAN=ON
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

## Stage 5: filesystem/persistence fault injection

Include `slrr/file_faults.hpp`. The API is intended for focused C++ tests and
headless probes; there is currently no command-line option that arms a rule.
With no installed rule, every interception returns the requested byte allowance,
emits no event, and leaves the existing operation and its result unchanged.
Instrumentation-internal failures also fail open. The registered
`host.file_faults_injected` counter remains zero until an injection occurs.

### Rules, thresholds, and scopes

```cpp
using namespace slrr::file_faults;

// The first open/read/write/flush/close/delete/copy/rename attempt matching this
// exact normalized path fails with permission_denied. Other paths are unaffected.
Rule open_rule;
open_rule.operation = Operation::Open;
open_rule.fault = Fault::PermissionDenied;
open_rule.path = temporary_path / "case.sdat";
open_rule.fail_after_operations = 0;  // zero means inject on the first attempt
open_rule.fail_after_bytes = kUnlimited;
ScopedRule open_scope(open_rule);

// Admit four bytes across matching writes, partially write at the crossing,
// then apply disk_full. Operation counting is disabled for this byte rule.
Rule write_rule;
write_rule.operation = Operation::Write;
write_rule.fault = Fault::DiskFull;
write_rule.path = temporary_path / "case.sdat";
write_rule.fail_after_operations = kUnlimited;
write_rule.fail_after_bytes = 4;
ScopedRule write_scope(write_rule);
```

`Operation` covers `Open`, `Read`, `Write`, `Flush`, `Close`,
`Delete`, `Copy`, and `Rename`. `File.move` uses `Rename`; its existing
overwrite behavior is not replaced with a new atomic/replace policy. `File.copy`
continues to use its existing `overwrite_existing` operation under `Copy`.
There is no separate production replace operation in this lane.

`Fault` covers `PermissionDenied`, `DiskFull`, `IoError`, `ShortRead`,
`EndOfFile`, and `PartialWrite`. The first three produce a hard
`Decision::failure` and a `std::error_code` (`permission_denied`,
`no_space_on_device`, or `io_error`). Short read, EOF, and partial write are
valid only for their corresponding read/write operations and return a reduced
`allowed_bytes`. Invalid combinations are rejected without installing a rule.

For `fail_after_operations=N`, the first N matching attempts are untouched;
attempt N+1 and later attempts inject. For `fail_after_bytes=N`, the first N
admitted bytes are untouched. A request crossing that boundary is shortened to
the remaining byte count, and later requests apply the configured fault. Byte
thresholds are meaningful for read/write calls; the other operations carry no
payload byte count. Set the unused threshold to `kUnlimited`. On an
operation-threshold short read/partial write, or after the byte budget is
exhausted, `short_bytes` selects the allowance. At a byte-boundary crossing the
remaining budget takes precedence. Operation and byte counters saturate rather
than wrapping.

Rules persist until `reset()`. They are process-global, mutex-protected, and
bounded to 32 entries. Installing another rule for the same operation and path
replaces it and resets that rule's counters. An empty path matches all paths; a
nonempty rule path is weakly canonicalized/lexically normalized and must match
the supplied operation path, or either source/destination for copy/rename.
Path matching does not put the path in diagnostics.

`ScopedRule` installs one rule and restores the complete previous rule set and
all threshold counters on destruction, including for nested scopes. An explicit
`reset()` inside a scope still takes effect immediately; scope destruction then
restores the snapshot captured at scope entry. Tests should also call `reset()`
at lane boundaries. Reset does not rewind the diagnostic event ring or the
process-lifetime metric counter.

### Host hooks and preserved behavior

The hooks are confined to the existing `java.io.File` host persistence paths:

| Operation | Existing intercepted boundary and injected result |
| --- | --- |
| Open | Read-mode `File.open` returns 0 before opening. Write-mode files are physically opened by `File.close`, so an injected open failure follows the existing failed-close path. |
| Read | The whole-file buffer fill in read-mode `File.open` fails or is shortened before SDAT parsing. An in-memory retained failed save does not perform a physical read and is not shortened. |
| Write | The physical framed write in `File.close` is failed or partially issued, then the existing stream failure state is set. Script `File.write` only appends to memory and is not a second physical-write hook. |
| Flush | The flush phase of write-mode `File.close` can set the existing stream failure state. No new explicit production flush is introduced. |
| Close | Write-mode close participates in the existing success/retained-write decision; read-side logical close can emit/count a fault but remains void. The host file record is still erased as before. |
| Delete | An existing path returns the existing 0 failure result before `remove_all`. The pre-existing nonexistent-path success behavior is unchanged. |
| Copy | `File.copy` returns -1 before `copy_file`; source and prior destination bytes are not modified by the injected operation. |
| Rename | `File.move` returns -1 before `rename`; source and destination are left in place. |

No-injection behavior retains the existing return values, stream/destructor
lifetime, SDAT framing, and `g_unflushed_saves` policy. In particular, this lane
does not make `File.close` return a new status, add a stronger close/flush check,
introduce atomic career saves, or change career-save retry/recovery policy.
Partial/failing writes use the same retained-failed-write path that an ordinary
stream failure already used.

Each injection emits `file.fault.injected` with `category=file`, the operation
in `method`, the fault in `error`, `result=failure` or `partial`, and numeric
`operation`, `requested_bytes`, and `allowed_bytes` values. It also increments
`host.file_faults_injected`. The event has no path, resource name, file bytes,
script string, or secret. Enable the existing `--trace PATH` or
`--log-all DIR` sink to capture events after a C++ probe installs a rule. A
normal `--probe-gc --root DIR --log-all DIR` run installs no rule and therefore
checks the zero-injection baseline rather than manufacturing a fault.

### Limitations

This is deterministic host-operation interception, not an OS disk driver,
filesystem filter, fault-injection framework, or proof of power-loss consistency.
Read shortening is applied to the existing whole-file buffer after the stream
fill; it does not reduce kernel I/O. Write/flush/close faults model the existing
buffered persistence boundary, not every possible native filesystem error.
Atomic rename exchange, fsync durability, disk quotas, actual ACL enforcement,
renderer/media I/O, and native reachability are outside this stage. Rule state is
process-global, so independently running threads must coordinate their scopes;
VM/fiber execution remains on the host thread and no concurrent Java execution
is introduced.

### Exact automated coverage

`tests/files-faults-tests.cmake` registers `slrr_files_faults_tests` from the
lane-owned `tests/test_file_faults.cpp`. Its test-only stream adapter drives
real files under unique temporary directories; it is not production persistence
code or an atomic-write helper. The 12 tests cover:

1. no-injection open/read/write/flush/close/delete/copy/rename baseline;
2. open permission failure after one operation and exact path isolation;
3. repeated short reads, EOF, and read permission failure;
4. read byte-budget crossing, exhausted-budget allowance, and hard failure;
5. partial write after one operation and immediate disk-full failure;
6. write byte-budget crossing, partial bytes on disk, and exhausted-budget disk full;
7. flush permission failure after one operation;
8. close I/O failure after one operation without changing actual close cleanup;
9. delete/copy/rename failure thresholds and source/destination preservation;
10. reset, invalid-rule rejection, nested scope restoration, and counter restoration;
11. structured `file` diagnostics, one metric increment, and path/content exclusion;
12. byte-for-byte content and last-write-time preservation of a pre-existing
    reference-save fixture while faults target another file.

Build and run the lane directly with:

```powershell
& $cmake --build build-bgfx-x64 --config Release --target slrr_files_faults_tests --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ./build-bgfx-x64/tests/Release/slrr_files_faults_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

The full configure/build/CTest workflow remains the required integration gate.
No test reads or writes a game/reference tree. The reference-save preservation
case creates its own pre-existing fixture in a temporary directory and compares
exact bytes and timestamp after the injected attempts.

## Stage 6: native coverage and reachability

### Runtime JSON schema and semantics

The version-1 native report contains a `summary` and a `signatures` array sorted
by class, method, signature, and registration state. Summary fields are:

| Field | Meaning |
| --- | --- |
| `registered` | Unique class/method/descriptor bindings currently in this registry. |
| `called` | Distinct tracked signatures attempted at least once, including tracked missing signatures. Calls with omitted-void aliases are attributed to the canonical registered descriptor. |
| `success` | Total registered handler invocations that returned normally. Explicit no-op calls are included. |
| `error` | Total registered handler invocations that threw standard or non-standard exceptions. Missing lookups are not also counted as errors. |
| `missing` | Total failed lookup invocations, including those whose per-signature record could not be retained. |
| `noop` | Registrations explicitly marked `NativeImplementation::ExplicitNoop` at the six `native_noop` registration sites. |
| `untracked_missing_invocations` | Missing calls omitted from per-signature records because of the record/component bounds. These still contribute to `missing`, but cannot contribute new entries to `called`. |

Each signature record has `class`, `method`, `signature`, `registered`,
`implementation` (`body`, `explicit_noop`, or `missing`), `invocation_count`,
`success_count`, `error_count`, `missing_count`, `total_duration_ns`, and nullable
`last_section`. Duration accumulates across outcomes, saturating at uint64
limits, and does not require tracing. The last nonempty diagnostics section is
retained when available and capped at 256 bytes. Registered metadata covers the
host's fixed registration set. At most 128 missing signatures are retained;
each missing class/method/descriptor component is capped at 512 bytes before a
record is admitted. Counts saturate instead of overflowing.

**Registered**, **called**, **no-op**, **missing**, and **behaviorally verified**
are deliberately different claims. `body` means only that a C++ callable was
registered; it includes fixed, empty, and partial implementations and is never a
behavioral-verification label. `explicit_noop` comes from registration metadata,
not from inspecting a body or counting a return value. Runtime invocation and
success counts measure the exercised path only. They do not establish stock
coverage, behavioral fidelity, or validation of arbitrary implementations.
Reports contain no native argument values, script locals, object addresses, or
error payloads.

The Metrics tab and `natives.*` metrics use the same registry state. Registry
construction/destruction registers/unregisters a metrics provider; native
statistics and report output otherwise require no diagnostics sink.

### Static stock audit

`assets/progress/stock-natives.json` is the versioned 376-signature source. It is
derived from `engine/Core/natives_stubs.cpp` in
[slrr-reverse](https://github.com/EliasFD3S/slrr-reverse) at commit
`5c42f3b139aef4c7b9e0b6420c5704e22c0e6cd7`, with source SHA-256
`7a09d8f3dd63079dbf50632efd7400aeecebdaa05a786e4958cfc12ad4c6b9f3`.
`scripts/render_native_coverage.py` (Python standard library only, no game
data or network) generates the coverage JSON, SVG and README table from that
manifest and the `register_method` sites under `src/`:

```powershell
# Rebuild JSON, SVG and the README table from the current registration sites.
python scripts/render_native_coverage.py

# Read-only drift check; list the remaining signatures.
python scripts/render_native_coverage.py --check
python scripts/render_native_coverage.py --list missing
```

The generator matches owner, method, and parameter descriptor types (not return
types), expands registration loops and helper forwarding functions, and fails on
unresolved registration expressions or conflicting sources. It does not infer
behavioral status. A registration counts as an explicit no-op when its handler
is `native_noop` or it carries `NativeImplementation::ExplicitNoop`. CI and
CTest run the drift check.

`tests/native-report-tests.cmake` registers `slrr_native_report_tests`, and
`slrr_native_coverage_drift` when a Python 3 interpreter is available. The C++
lane covers metadata, explicit no-op classification, success/error/missing
outcomes, aliases, durations/counts, section capture, metrics lifecycle, JSON
escaping/file output, and bounds. The Python lane checks tracked-output drift.


# Stage 7 instrumentation

Stage 7 adds observation only. It does not change rendering, backend destruction,
media conversion/playback, input mapping, or the contact solver. Structured events
are emitted only when the existing diagnostics sink is active; numeric metrics are
available through the existing metrics provider.

## Renderer and backend

A successful libbgfx open allocates a process-monotonic renderer generation. Close
keeps that generation in the final snapshot but marks the renderer closed and
clears live handle/view statistics. A stale frame submitted for an old generation
is ignored. Reopening receives a new generation and starts with no valid frame
sample.

Immediately after each actual `bgfx::frame()`, the backend copies bgfx statistics
into an owned snapshot. The overlay never retains `Stats*` or `ViewStats*`.
The **Renderer → Platform / Renderer diagnostics** section shows generation,
open/closed state, submitted frames, sample validity, CPU/GPU times, bgfx GPU frame,
draw/compute/blit counts, up to 16 per-view timing records, bgfx global used
vertex/index/dynamic-buffer, texture and framebuffer counts, and active uploaded
FMV texture handles. View truncation is explicit; early returns that do not submit
a frame do not increment `renderer.frames_submitted`.

Metrics: `renderer.open`, `renderer.generation`,
`renderer.frames_submitted` (counter), `renderer.stats_valid`,
`renderer.views_truncated`, `renderer.views`, `renderer.draw_calls`,
`renderer.compute_calls`, `renderer.blit_calls`,
`renderer.gpu_vertex_buffers`, `renderer.gpu_index_buffers`,
`renderer.gpu_dynamic_vertex_buffers`, `renderer.gpu_dynamic_index_buffers`,
`renderer.gpu_textures`, `renderer.gpu_frame_buffers`, and
`renderer.fmv_texture_handles`. Events are `renderer.open` and
`renderer.close` with numeric `generation`. No raw bgfx handle is logged. The
FMV gauge is active uploaded state, not a historical allocation count.

## Media samples and LibVLC callbacks

`media.directshow.sample` and `media.libvlc.sample` contain only
`generation`, `width`, `height`, `pitch`, `byte_length`,
`orientation`, and `accepted`. Orientation is 0 unknown, 1 top-down, 2
bottom-up. DirectShow metadata comes from the connected RGB32 type and Sample
Grabber byte count; pitch is the existing `width * 4` conversion pitch. LibVLC
uses decoded RV32 pitch and visible-plane byte extent. No pixel payload, path,
credential, native pointer, or plane address is recorded. Orientation reporting
does not alter conversion or flip behavior.

Counters are `media.directshow.samples`, `media.directshow.rejected`,
`media.libvlc.samples`, and `media.libvlc.rejected`. DirectShow rejection is a
buffer query/read failure; LibVLC rejection is frame allocation failure. The
AVIFile compatibility path does not produce Sample Grabber events.

Each LibVLC player receives a process-monotonic decoder generation.
`media.libvlc.open` and `media.libvlc.close` carry it; close also carries
numeric `reason` (0 orderly, 1 play-start failure). `media.libvlc.callback`
carries `generation`, `callback`, and `phase`. Callback IDs: 1 format, 2 lock,
3 display, 4 cleanup. Phase IDs: 1 begin, 2 end. Matching counters are
`media.libvlc.format.begin/end`, `media.libvlc.lock.begin/end`,
`media.libvlc.display.begin/end`, and `media.libvlc.cleanup.begin/end`.
`media.libvlc.closes` counts decoder close events.

## Input binding diagnostics

`input_axis_binding_diagnostics` inspects declarations, not typed text, key
names, or physical values. The Input tab and
`input.axis.<axis>.source`/`input.axis.<axis>.fallback` gauges cover axes
1, 4, 5, 6, 28, 29, 48, 49, 50, and 54.

Source IDs: 0 missing controller, 1 unbound, 2 forced, 3 profile, 4 XInput
fallback, 5 profile plus XInput fallback. Fallback IDs: 0 not eligible, 1
available, 2 suppressed by an XInput binding elsewhere in the profile, 3
overridden by a forced axis. This exposes partial XInput profiles because stock
fallback is suppressed globally by any pad binding. `input.axis.binding` is
emitted only on status changes with numeric axis/source/fallback and binding
counts.

## Physics telemetry schema version 2

Each available entity includes `spawn_pose` (or null), `contacts`, and
`dropped_contacts`. `spawn_pose.position` is script-space metres and
`spawn_pose.orientation` is quaternion `[x,y,z,w]`, captured from the first
successfully configured rigid-body frame before later movement.

At most 16 contacts per entity per recorded solver update are retained; excess
contacts increment `dropped_contacts`. Each contact has `kind` (0
chassis/terrain, 1 wheel/terrain, 2 chassis/static sweep), `wheel`
(zero-based wheel index or -1), nullable `resource`, `mesh`, `triangle`,
and `material` numeric IDs, script-space `point`/`normal`, metre
`penetration`, and the existing normal `impulse` magnitude.

`resource` is the scene render/type resource when known. `mesh` is its
geometry resource only for non-override collision meshes. Static preload and
override meshes report null instead of an invented identity. `triangle` is the
original mesh triangle ordinal; duplicate vertex triples are ambiguous and null.
`material` is the authored SCX group `material_type` when group coverage
exists. Ground PHYS data and meshes without groups report null. Points are solver
contact points for kinds 0/1 and sweep surface points for kind 2. Penetration is
ground overlap for kinds 0/1 and sphere radius-minus-distance for kind 2.
Friction impulse is not included. Non-finite values serialize as null. Missing
identity does not mean no contact occurred. Identity integers compare exactly in
replay comparison; validation enforces bounds and uint32/null identities.

## CI and local diagnostic outputs

Both normal and ASan workflows build `slrr_host` and `slrr_tests`, run CTest,
then run physics, replay self-compare, dyno, and GC probes. Outputs are rooted at
`artifacts/normal/` or `artifacts/asan/` in the CI workspace, never in game or
reference data. Physics uses `physics.jsonl`; comparison uses
`physics-compare.json`. Each `--log-all` directory contains the normal session,
event, metrics, and manifest outputs. GC also writes `native-report.json` and,
without game data, three deterministic fixture classes under `hermetic-classes/`.
Failure uploads include these directories, CTest temporary logs, and fuzz
reproducers. Successful lanes upload the compact diagnostics directory and
`LastTest.log`. These definitions are not evidence of a GitHub-hosted run.

The GC probe uses a valid game root when supplied. Without game data,
`--probe-gc` alone runs the same hermetic object-graph/finalizer/native checks
with three deterministic native-method TUFA fixture classes in the log directory
(or system temp) and without starting a game.
