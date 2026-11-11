# Per-run process and crash logging

`slrr/crash_logging.hpp` provides an explicitly enabled, process-wide Windows x64
capture session. It is independent of the future command-line parser integration:
the library never chooses a directory from the game root, reference data, current
directory, or a player profile.

## API

```cpp
slrr::crash_logging::Options options;
options.directory = selected_run_directory;  // required; used exactly as supplied
options.preserve_console_output = true;
options.build_id = SLRR_VERSION;             // coordinator-supplied identity
options.build_configuration = "Release";
// options.command_line is empty: capture GetCommandLineW with option redaction.

std::string error;
if (!slrr::crash_logging::start(options, error)) {
  // Handle startup failure before starting the engine.
}

// On every orderly exit:
slrr::crash_logging::shutdown(slrr::crash_logging::Disposition::Normal);
```

Only one session can be active. `start` and `shutdown` are host-thread
lifecycle operations and must not race each other or borrowed path access.
`paths()` returns a borrowed `RunPaths` after a successful start; it remains
available after shutdown until the next start. `active()` reports whether capture
and handlers are active. `shutdown(Disposition::Abnormal)` is available for a
coordinator-handled fatal error that does not invoke one of the crash handlers.

Starting also calls `diagnostics::start` for the run's `events.jsonl`.
Shutdown flushes/closes diagnostics and calls `metrics::write_snapshot` for the
same run's `metrics.json`. A crash attempts both operations on a best-effort
basis. Do not independently start a second diagnostics sink for the same run.

## Output files

All names are fixed directly under the caller-selected run directory:

| File | Contents |
| --- | --- |
| `session.log` | Raw bytes drained from CRT stdout and stderr pipes. |
| `events.jsonl` | Existing structured diagnostics sink for this run. |
| `metrics.json` | Existing metrics snapshot at clean shutdown, or best-effort crash snapshot. |
| `native-report.json` | Optional native registration/invocation report, present only when `--native-report` is requested; `--log-all` owns this path. |
| `crash.txt` | Crash kind, code/address, message, identity, and recent diagnostic context/events. Created only for a handled crash. |
| `crash.dmp` | Windows `MiniDumpNormal` written through dbghelp, with SEH exception information when available. |
| `manifest.json` | Process/build identity, redacted command line, file names, UTC start/end, and `running`, `normal`, or `abnormal` disposition. |

Starting removes stale `crash.txt`, `crash.dmp`, and `metrics.json`
from a reused directory and truncates the new session/events/manifest outputs. It
does not recursively delete a previous run. Callers should normally select a new
empty directory per run.

The manifest records PID, executable path, supplied build ID/configuration,
compiler identity, and Windows/x64 architecture. Recent crash diagnostics include
at most 16 events and omit event values, object pointers, resource payloads,
script locals, and native argument values. The logging implementation does not
inspect or serialize script locals/native arguments. An operating-system memory
dump can nevertheless inherently contain process memory.

## Console capture

The implementation flushes C/C++ streams, duplicates CRT descriptors 1 and 2,
and redirects them to separate binary pipes. Two reader threads continuously
drain the pipes so routine output does not fill a pipe and deadlock the host.
Writes are appended to `session.log` and, when requested, teed to the original
stdout/stderr descriptor. This covers `printf`, `fprintf`, iostreams after their
buffers flush, and native/library `_write` calls through those CRT descriptors.

Clean shutdown flushes and restores descriptors before closing pipe writers,
joins both readers, and restores handlers. Cross-stream ordering is only the
order in which reader threads acquire the log lock; chunks can interleave and
are not timestamped. There is no size cap or rotation, so the caller must budget
disk space.

Capture is process-local. It does not follow child processes, intercept direct
`WriteFile` calls to saved Win32 handles, or control code that replaces the CRT
descriptors after startup. Debuggers, consoles, CRT buffering, and third-party
libraries can affect what reaches disk.

## Crash handling and hard limits

On Windows the session installs best-effort handlers for:

- unhandled C++ exceptions reaching `std::terminate` (including `what()` for
  `std::exception`), and explicit terminate;
- MSVC C++ exceptions escaping `main`, which can instead reach the SEH filter as
  `0xE06D7363`; these are classified as unhandled C++ exceptions, but `what()`
  is unavailable without relying on private CRT exception-object layouts;
- other unhandled SEH through the top-level exception filter;
- `SIGABRT`;
- pure virtual calls; and
- CRT invalid-parameter callbacks.

Crash paths and identity strings are computed before handlers are installed. The
first handled crash writes text, attempts a dump, writes an abnormal manifest,
and attempts to flush structured diagnostics/metrics. CRT termination paths exit
with status 3 after recording; the SEH filter requests abnormal process
termination. Clean shutdown restores the prior process handlers and error mode.

**No complete-log guarantee exists.** These handlers are not promised to be
async-signal-safe. They can allocate, take diagnostics locks, call the CRT, and
invoke dbghelp, so a crash while those facilities are locked or corrupted can
prevent output. Stack exhaustion, heap/stack corruption, recursive failure, a
deadlocked reader, killed/dumped threads, full disks, and ACL failures can also
leave missing or partial files. `RaiseFailFastException`, `__fastfail`,
immediate process termination, debugger/policy interception, power loss, and OS
failure can bypass these handlers or prevent buffered output/dumps entirely.
There is no promise of a dump in every fail-fast case, or of valid recent
context after severe process corruption. Only the first handled crash is
recorded.

## Security and privacy

Treat the entire run directory as sensitive. Paths can reveal user/profile names;
logs can contain player names, game state, paths, or library messages; and memory
dumps can contain credentials, tokens, personal data, script state, and arbitrary
process memory even when those values never appear in text logs. Use appropriate
ACLs, retention limits, and deliberate sharing procedures.

Environment variables are not collected. Common command-line options named
`password`, `passwd`, `token`, `secret`, `credential(s)`,
`api-key`, `apikey`, `access-key`, `auth-token`, and
`session-token` have their following/attached values replaced by
`<redacted>`. This is best-effort for known option names, not a general secret
scanner: callers must not place credentials in command lines or diagnostic text.

## Host `--log-all DIR` integration

The host now exposes `--log-all DIR`. It uses the following integration contract:

1. Add `src/platform/crash_logging.cpp` to `slrr_host` and link
   `dbghelp` in the top-level `CMakeLists.txt`.
2. Parse `--log-all DIR` early in `src/main.cpp`. Reject a missing/empty DIR.
   Use DIR itself as `Options::directory` (or append a coordinator-defined
   unique run name before calling start); never fall back to game/reference data.
3. Include `slrr_version.h`, set `build_id = SLRR_VERSION`, and define the
   build-configuration string from CMake's `$<CONFIG>` (or the coordinator's
   equivalent). Leave `command_line` empty unless a separately normalized
   command line is desired.
4. Call `crash_logging::start` before engine boot and before any code that must
   be captured. On failure, print the error and abort startup or make an explicit
   user-facing policy decision; do not silently continue as though capture exists.
   An outer RAII guard is recommended so argument/probe early returns also finalize.
5. When `--log-all` is active, do not separately call `diagnostics::start`
   for `--trace`, and leave `DiagnosticsSession::metrics_path` empty so its
   destructor does not write an alternate `--metrics` file. Either reject
   explicit `--trace`/`--metrics` conflicts or define their precedence. The
   capture session owns those sinks; trace/context/metric registration is unchanged.
6. On every orderly main exit, call `shutdown(Normal)`. On a caught fatal
   coordinator error that is returning abnormally without a crash handler, call
   `shutdown(Abnormal)`. Ensure this occurs after engine/service diagnostics
   are no longer being emitted and before static destruction. To preserve the
   existing live VM metrics, place a nested finalizer next to `MetricsFinalizer`
   so capture shutdown runs before `Jvm` teardown; the outer guard then becomes
   a no-op for that path. The later `DiagnosticsSession` diagnostics shutdown is
   harmless but must not own a competing metrics path.
7. Preserve `preserve_console_output=true` for normal interactive/headless CLI
   behavior. A future explicitly documented quiet mode may set it false.

`tests/crash-logging-tests.cmake` is the complete test-side integration
example: it adds the implementation to `slrr_test_support`, links dbghelp,
builds `slrr_crash_logging_fixture`, and runs out-of-process assertions in
`slrr_crash_logging_tests`.

## Stage 7 CI diagnostic artifacts

The normal and ASan test workflows write probe diagnostics only below
`artifacts/normal/` or `artifacts/asan/` in the CI workspace. They do not select
output names in game data or reference trees. Physics telemetry and replay
comparison files sit directly in those directories; each `--log-all` probe uses
its own child directory with the files listed above. GC uses
`gc-log/native-report.json`; no-game-data GC probes also create three small
native-method fixtures under `gc-log/hermetic-classes/`. Failure artifact uploads also include CTest
`Testing/Temporary` output and fuzz reproducers; success uploads include the
probe directories and `LastTest.log`. Workflow YAML alone does not demonstrate
that a hosted run or artifact upload occurred.

Stage 7 renderer/media/input events add numeric generation, frame, sample,
orientation, binding-source, and fallback metadata only. They intentionally do
not add frame payloads, media paths, typed input text, credentials, or native
pointer values. The privacy warning for the whole run directory still applies
because session logs and memory dumps can contain information outside those
structured fields.
