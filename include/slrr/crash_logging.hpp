#pragma once

#include <filesystem>
#include <string>

namespace slrr::crash_logging {

// One process-wide capture session. directory is always the exact run directory;
// there is no default location and start() never derives one from game data.
struct Options {
  std::filesystem::path directory;
  bool preserve_console_output = true;

  // Supplied by the coordinator so this platform library does not depend on the
  // host's generated version header. Empty values are recorded as empty strings.
  std::string build_id;
  std::string build_configuration;

  // Empty selects GetCommandLineW on Windows. Common credential-bearing options
  // are redacted before storage; callers must still avoid secrets in arguments.
  std::string command_line;
};

struct RunPaths {
  std::filesystem::path directory;
  std::filesystem::path session_log;
  std::filesystem::path events_jsonl;
  std::filesystem::path metrics_json;
  std::filesystem::path crash_text;
  std::filesystem::path crash_dump;
  std::filesystem::path manifest_json;
};

enum class Disposition {
  Normal,
  Abnormal,
};

// Creates only Options::directory and its predictable capture files. This also
// starts diagnostics at events_jsonl and redirects CRT stdout/stderr through
// draining tee readers. Only one session may be active in a process.
bool start(const Options& options, std::string& error) noexcept;

// Restores console descriptors and crash handlers, drains both pipes, writes
// metrics_json, and finalizes manifest_json. Abnormal is for a coordinator-owned
// fatal error path that did not itself invoke a crash handler.
void shutdown(Disposition disposition = Disposition::Normal) noexcept;

bool active() noexcept;

// Valid after a successful start, including after shutdown, until the next
// start call. The returned pointer is borrowed; start/shutdown are host-thread
// lifecycle operations and must not race pointer use.
const RunPaths* paths() noexcept;

}  // namespace slrr::crash_logging
