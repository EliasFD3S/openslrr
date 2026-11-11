# Runtime instrumentation

Stage 1 provides opt-in structured runtime tracing. It does not collect native
reachability statistics, metrics, fuzzing, replay, or fault-injection data.

## CLI

`--trace PATH` enables tracing and creates or truncates the explicitly supplied
JSONL file:

```powershell
slrr_host.exe --probe-gc --root C:\path\to\game --trace C:\temp\slrr-gc.jsonl
```

There is no default trace path and no per-frame trace output on stdout. The host
never selects a file in game or reference data; only an explicit user-supplied
path is opened. Treat that path as an output file because existing contents are
replaced. Startup fails if the file cannot be opened. Normal returns and probe
failure returns flush and close the file through the process-lifetime session
guard. As with other buffered files, an abrupt process termination is not a
clean-flush guarantee.

## Events and bounds

Every line is one JSON object with microsecond Unix `timestamp_us`, optional
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

Current producers are:

- `native.invoke`: class/method/signature, receiver identity, duration, and
  success or error. Missing natives and native exceptions retain their original
  throw behavior.
- `section.transition`: adopted or changed section and old/new class detail.
- `gc.start`, `gc.finalization`, and `gc.end`: collection counts and duration.
- `fiber.start`, `fiber.stop`, `fiber.exit`, and `fiber.checkpoint`:
  lifecycle and actual timeslice yields, not every VM instruction/checkpoint call.

## C++ API and later stages

Include `slrr/diagnostics.hpp`. `diagnostics::start`, `flush`, and
`shutdown` control the sink; `trace(Event)` submits an event; `snapshot` and
`dropped_events` expose the bounded ring for tests and later tooling.

`set_context` or the `set_frame`, `set_tick`, `set_section`, and
`set_fiber` setters update context without changing the event API. Context is
thread-local, and the sink/ring are mutex-protected, so media and worker callbacks
can submit events without touching VM state or inheriting host-thread context.
VM execution and fiber scheduling remain on the host OS thread. Fiber identity is
installed only around an actual `SwitchToFiber`; section identity is installed
only by `SectionMachine` transitions.

Tracing is runtime-disabled and allocation-free at the API entry check until
`--trace` or `start` is used. Defining `SLRR_ENABLE_DIAGNOSTICS=0` at compile
time replaces the API with no-op inline functions; `--trace` then reports that
diagnostics were disabled at compile time.

Tests live in `tests/test_diagnostics.cpp` and are registered as
`slrr_instrumentation_diagnostics_tests` by `tests/instrumentation-tests.cmake`.
