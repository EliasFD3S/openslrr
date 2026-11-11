# Runtime instrumentation

Stage 1 provides opt-in structured runtime tracing. Stage 2 adds bounded
lifetime/resource metrics and final JSON snapshots. Later native-reachability,
fuzzing, replay, fault-injection, renderer-media, and native-coverage stages are
not implemented here.

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
