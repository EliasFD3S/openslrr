#include "slrr/crash_logging.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <io.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace slrr::crash_logging {
namespace {

constexpr std::size_t kPipeBytes = 65536;
constexpr std::size_t kRecentEvents = 16;
constexpr std::size_t kMaximumCrashText = 1024;

std::mutex g_control;
std::atomic_bool g_active{false};
RunPaths g_paths;
bool g_paths_ready = false;

struct PipeCapture {
  int target_fd = -1;
  int saved_fd = -1;
  int read_fd = -1;
  int write_fd = -1;
  bool redirected = false;
  std::thread reader;
};

struct State {
  PipeCapture output{1};
  PipeCapture error{2};
  bool preserve_console = true;
  std::ofstream log;
  std::mutex log_mutex;
  bool diagnostics_started = false;
  bool handlers_installed = false;
};

std::unique_ptr<State> g_state;

struct CrashShared {
  std::atomic_bool armed{false};
  std::atomic_flag handling = ATOMIC_FLAG_INIT;
  wchar_t crash_text_path[32768]{};
  wchar_t crash_dump_path[32768]{};
  wchar_t manifest_path[32768]{};
  std::string metrics_path;
  std::string manifest_prefix;
  std::string identity_text;
};

CrashShared g_crash;

void copy_precomputed_path(wchar_t (&destination)[32768],
                           const std::filesystem::path& source) {
  const auto text = source.wstring();
  const auto count = std::min<std::size_t>(text.size(), 32767);
  std::copy_n(text.data(), count, destination);
  destination[count] = L'\0';
}

std::string wide_to_utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0,
                                        nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string result(static_cast<std::size_t>(bytes), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), result.data(), bytes,
                          nullptr, nullptr) != bytes) {
    return {};
  }
  return result;
}

std::string utc_now() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char buffer[64]{};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond, time.wMilliseconds);
  return buffer;
}

std::string executable_path() {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size() - 1) {
      return wide_to_utf8(std::wstring_view(buffer.data(), length));
    }
    if (buffer.size() >= 32768) return {};
    buffer.resize(buffer.size() * 2);
  }
}

std::string json_escape(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    switch (byte) {
      case '"': out += "\\\""; ++i; continue;
      case '\\': out += "\\\\"; ++i; continue;
      case '\b': out += "\\b"; ++i; continue;
      case '\f': out += "\\f"; ++i; continue;
      case '\n': out += "\\n"; ++i; continue;
      case '\r': out += "\\r"; ++i; continue;
      case '\t': out += "\\t"; ++i; continue;
      default: break;
    }
    if (byte < 0x20) {
      out += "\\u00";
      out.push_back(kHex[(byte >> 4) & 0xf]);
      out.push_back(kHex[byte & 0xf]);
      ++i;
      continue;
    }
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
      ++i;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (byte >= 0xc2 && byte <= 0xdf) {
      length = 2;
      codepoint = byte & 0x1f;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      length = 3;
      codepoint = byte & 0x0f;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      length = 4;
      codepoint = byte & 0x07;
    }
    bool valid = length != 0 && i + length <= text.size();
    for (std::size_t j = 1; valid && j < length; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if ((continuation & 0xc0) != 0x80) {
        valid = false;
      } else {
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
    }
    if (valid && ((length == 3 && codepoint < 0x800) ||
                  (length == 4 && codepoint < 0x10000) ||
                  (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                  codepoint > 0x10ffff)) {
      valid = false;
    }
    if (valid) {
      out.append(text, i, length);
      i += length;
    } else {
      out += "\\ufffd";
      ++i;
    }
  }
  out.push_back('"');
  return out;
}

std::string bounded(std::string_view text) {
  return std::string(text.substr(0, kMaximumCrashText));
}

bool sensitive_option(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "password", "passwd", "token", "secret", "credential", "credentials",
      "api-key", "apikey", "access-key", "auth-token", "session-token"};
  std::string lowered(name);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto candidate : kNames) {
    if (lowered == candidate) return true;
  }
  return false;
}

// Tokenize only to redact values. Other argument spelling and quoting are kept.
std::string redact_command_line(std::string_view command) {
  struct Token {
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Token> tokens;
  bool quoted = false;
  std::size_t begin = 0;
  for (std::size_t i = 0; i <= command.size(); ++i) {
    if (i < command.size() && command[i] == '"') quoted = !quoted;
    if (i == command.size() ||
        (!quoted && (command[i] == ' ' || command[i] == '\t'))) {
      if (i > begin) tokens.push_back({begin, i});
      begin = i + 1;
    }
  }

  std::string result;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto token = command.substr(tokens[i].begin,
                                      tokens[i].end - tokens[i].begin);
    std::string_view view = token;
    if (view.size() >= 2 && view.front() == '"' && view.back() == '"') {
      view = view.substr(1, view.size() - 2);
    }
    std::string_view option;
    if (view.starts_with("--")) {
      option = view.substr(2);
    } else if (view.starts_with("/")) {
      option = view.substr(1);
    } else if (view.starts_with("-") && view.size() > 2) {
      option = view.substr(1);
    }
    const auto separator = option.find_first_of("=:");
    const auto name = option.substr(0, separator);
    const bool attached = separator != std::string_view::npos;
    if (!sensitive_option(name)) {
      if (!result.empty()) result.push_back(' ');
      result.append(token);
      continue;
    }
    if (!result.empty()) result.push_back(' ');
    result.append(token.substr(0, token.size() - view.size()));
    result.append(view.substr(0, view.size() - option.size()));
    result.append(name);
    if (attached) {
      result.push_back(option[separator]);
    } else {
      result.push_back(' ');
      if (i + 1 < tokens.size()) ++i;
    }
    result += "<redacted>";
  }
  return result;
}

std::string compiler_identity() {
#if defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
  return "Clang " + std::string(__clang_version__);
#elif defined(__GNUC__)
  return "GCC " + std::string(__VERSION__);
#else
  return "unknown";
#endif
}

std::string make_manifest_prefix(const Options& options,
                                 const std::string& started_at) {
  const auto command = options.command_line.empty()
                           ? redact_command_line(wide_to_utf8(GetCommandLineW()))
                           : redact_command_line(options.command_line);
  std::string out = "{\"version\":1,\"process\":{";
  out += "\"pid\":" + std::to_string(GetCurrentProcessId());
  out += ",\"executable\":" + json_escape(executable_path());
  out += ",\"command_line\":" + json_escape(command);
  out += ",\"os\":\"windows\",\"architecture\":\"x64\"},\"build\":{";
  out += "\"id\":" + json_escape(options.build_id);
  out += ",\"configuration\":" + json_escape(options.build_configuration);
  out += ",\"compiler\":" + json_escape(compiler_identity());
  out += "},\"started_at_utc\":" + json_escape(started_at);
  out += ",\"files\":{\"session_log\":\"session.log\",";
  out += "\"events_jsonl\":\"events.jsonl\",\"metrics_json\":\"metrics.json\",";
  out += "\"crash_text\":\"crash.txt\",\"crash_dump\":\"crash.dmp\",";
  out += "\"manifest_json\":\"manifest.json\"";
  if (options.native_report) out += ",\"native_report\":\"native-report.json\"";
  out += "},\"disposition\":";
  return out;
}

std::string make_identity_text(const Options& options,
                               const std::string& started_at) {
  const auto command = options.command_line.empty()
                           ? redact_command_line(wide_to_utf8(GetCommandLineW()))
                           : redact_command_line(options.command_line);
  std::string out = "OpenSLRR process crash evidence\n";
  out += "pid: " + std::to_string(GetCurrentProcessId()) + "\n";
  out += "executable: " + executable_path() + "\n";
  out += "build_id: " + options.build_id + "\n";
  out += "build_configuration: " + options.build_configuration + "\n";
  out += "compiler: " + compiler_identity() + "\n";
  out += "started_at_utc: " + started_at + "\n";
  out += "command_line: " + command + "\n";
  return out;
}

bool write_all(HANDLE file, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
        written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

bool write_file(const wchar_t* path, std::string_view bytes) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const bool result = write_all(file, bytes);
  FlushFileBuffers(file);
  CloseHandle(file);
  return result;
}

std::string optional_u64(const std::optional<std::uint64_t>& value) {
  return value ? std::to_string(*value) : "null";
}

const char* category_name(diagnostics::Category category) {
  switch (category) {
    case diagnostics::Category::Native: return "native";
    case diagnostics::Category::Section: return "section";
    case diagnostics::Category::Gc: return "gc";
    case diagnostics::Category::Fiber: return "fiber";
    case diagnostics::Category::Runtime: return "runtime";
  }
  return "runtime";
}

struct RecentDiagnostics {
  std::string text;
  std::string json;
};

RecentDiagnostics recent_diagnostics() {
  RecentDiagnostics result;
  const auto context = diagnostics::current_context();
  result.text = "recent_context: frame=" + optional_u64(context.frame) +
                " tick=" + optional_u64(context.tick) + " section=" +
                context.section + " fiber_id=" + optional_u64(context.fiber_id) +
                "\n";
  result.json = "{\"context\":{\"frame\":" + optional_u64(context.frame) +
                ",\"tick\":" + optional_u64(context.tick) +
                ",\"section\":" + json_escape(context.section) +
                ",\"fiber_id\":" + optional_u64(context.fiber_id) +
                "},\"events\":[";

  const auto events = diagnostics::snapshot();
  const auto first = events.size() > kRecentEvents ? events.size() - kRecentEvents : 0;
  for (std::size_t i = first; i < events.size(); ++i) {
    const auto& event = events[i];
    if (i != first) result.json.push_back(',');
    result.text += "recent_event: category=";
    result.text += category_name(event.category);
    result.text += " name=" + bounded(event.name) +
                   " frame=" + optional_u64(event.frame) +
                   " tick=" + optional_u64(event.tick) +
                   " section=" + bounded(event.section) +
                   " result=" + bounded(event.result) +
                   " error=" + bounded(event.error) + "\n";
    result.json += "{\"category\":" + json_escape(category_name(event.category));
    result.json += ",\"name\":" + json_escape(bounded(event.name));
    result.json += ",\"frame\":" + optional_u64(event.frame);
    result.json += ",\"tick\":" + optional_u64(event.tick);
    result.json += ",\"section\":" + json_escape(bounded(event.section));
    result.json += ",\"fiber_id\":" + optional_u64(event.fiber_id);
    result.json += ",\"result\":" + json_escape(bounded(event.result));
    result.json += ",\"error\":" + json_escape(bounded(event.error)) + "}";
  }
  result.json += "]}";
  return result;
}

std::string hex_value(std::uint64_t value) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "0x%llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool write_minidump(EXCEPTION_POINTERS* exception) {
  HANDLE file = CreateFileW(g_crash.crash_dump_path, GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  MINIDUMP_EXCEPTION_INFORMATION exception_information{};
  exception_information.ThreadId = GetCurrentThreadId();
  exception_information.ExceptionPointers = exception;
  exception_information.ClientPointers = FALSE;
  const BOOL result = MiniDumpWriteDump(
      GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
      exception ? &exception_information : nullptr, nullptr, nullptr);
  FlushFileBuffers(file);
  const auto size = GetFileSize(file, nullptr);
  CloseHandle(file);
  return result != FALSE && size != INVALID_FILE_SIZE && size != 0;
}

void record_crash(std::string_view kind, std::uint64_t code,
                  std::uint64_t address, std::string_view message,
                  EXCEPTION_POINTERS* exception) noexcept {
  if (!g_crash.armed.load(std::memory_order_acquire)) return;
  if (g_crash.handling.test_and_set(std::memory_order_acq_rel)) return;
  g_active.store(false, std::memory_order_release);

  RecentDiagnostics recent;
  try {
    recent = recent_diagnostics();
  } catch (...) {
    recent.text = "recent_context: unavailable\n";
    recent.json = "{\"context\":null,\"events\":[],\"status\":\"unavailable\"}";
  }

  const auto ended = utc_now();
  std::string text = g_crash.identity_text;
  text += "ended_at_utc: " + ended + "\n";
  text += "disposition: abnormal\nkind: " + bounded(kind) + "\n";
  text += "code: " + hex_value(code) + "\n";
  text += "address: " + hex_value(address) + "\n";
  text += "message: " + bounded(message) + "\n";
  text += recent.text;
  write_file(g_crash.crash_text_path, text);

  const bool dump_written = write_minidump(exception);
  const std::string dump_line =
      std::string("crash_dump: crash.dmp\ndump_written: ") +
      (dump_written ? "true\n" : "false\n");
  HANDLE append = CreateFileW(g_crash.crash_text_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (append != INVALID_HANDLE_VALUE) {
    write_all(append, dump_line);
    CloseHandle(append);
  }

  std::string manifest = g_crash.manifest_prefix;
  manifest += "\"abnormal\",\"ended_at_utc\":" + json_escape(ended);
  manifest += ",\"crash\":{\"kind\":" + json_escape(bounded(kind));
  manifest += ",\"code\":" + json_escape(hex_value(code));
  manifest += ",\"address\":" + json_escape(hex_value(address));
  manifest += ",\"message\":" + json_escape(bounded(message));
  manifest += ",\"dump_written\":";
  manifest += dump_written ? "true" : "false";
  manifest += "},\"recent_diagnostics\":" + recent.json + "}\n";
  write_file(g_crash.manifest_path, manifest);

  try {
    std::string ignored;
    metrics::write_snapshot(g_crash.metrics_path, ignored);
    diagnostics::flush();
  } catch (...) {
  }
}

[[noreturn]] void terminate_handler() {
  std::string kind = "terminate";
  std::string message = "std::terminate without a current exception";
  try {
    const auto exception = std::current_exception();
    if (exception) {
      kind = "unhandled_cpp_exception";
      try {
        std::rethrow_exception(exception);
      } catch (const std::exception& error) {
        message = bounded(error.what());
      } catch (...) {
        message = "non-std::exception C++ exception";
      }
    }
  } catch (...) {
    message = "exception inspection failed";
  }
  record_crash(kind, 0, 0, message, nullptr);
  std::_Exit(3);
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception) {
  const auto code = exception && exception->ExceptionRecord
                        ? exception->ExceptionRecord->ExceptionCode
                        : 0;
  const auto address = exception && exception->ExceptionRecord
                           ? reinterpret_cast<std::uintptr_t>(
                                 exception->ExceptionRecord->ExceptionAddress)
                           : 0;
  // MSVC's C++ throw ABI uses this SEH code. An exception escaping main can
  // reach this filter instead of std::terminate; do not mislabel it as a native
  // access violation. Decoding the exception object would use private CRT ABI,
  // so this route deliberately records only the public exception code.
  const bool msvc_cpp_exception = code == 0xe06d7363;
  record_crash(msvc_cpp_exception ? "unhandled_cpp_exception" : "windows_seh",
               code, address,
               msvc_cpp_exception
                   ? "unhandled MSVC C++ exception; what() unavailable in SEH filter"
                   : "unhandled Windows SEH exception",
               exception);
  return EXCEPTION_EXECUTE_HANDLER;
}

void abort_handler(int) {
  record_crash("abort", 0, 0, "SIGABRT", nullptr);
  std::_Exit(3);
}

void purecall_handler() {
  record_crash("purecall", 0, 0, "pure virtual function call", nullptr);
  std::_Exit(3);
}

void invalid_parameter_handler(const wchar_t* expression, const wchar_t* function,
                               const wchar_t* file, unsigned int line,
                               std::uintptr_t) {
  std::string message = "invalid CRT parameter";
  if (expression) message += "; expression=" + bounded(wide_to_utf8(expression));
  if (function) message += "; function=" + bounded(wide_to_utf8(function));
  if (file) message += "; file=" + bounded(wide_to_utf8(file));
  message += "; line=" + std::to_string(line);
  record_crash("invalid_parameter", 0, 0, message, nullptr);
  std::_Exit(3);
}

std::terminate_handler g_previous_terminate = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_exception_filter = nullptr;
void (__cdecl* g_previous_abort)(int) = SIG_DFL;
_purecall_handler g_previous_purecall = nullptr;
_invalid_parameter_handler g_previous_invalid_parameter = nullptr;
UINT g_previous_error_mode = 0;
unsigned int g_previous_abort_behavior = 0;

void install_handlers() {
  g_previous_terminate = std::set_terminate(terminate_handler);
  g_previous_exception_filter = SetUnhandledExceptionFilter(unhandled_exception_filter);
  g_previous_abort = std::signal(SIGABRT, abort_handler);
  g_previous_purecall = _set_purecall_handler(purecall_handler);
  g_previous_invalid_parameter = _set_invalid_parameter_handler(invalid_parameter_handler);
  g_previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  g_previous_abort_behavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void uninstall_handlers() {
  if (std::get_terminate() == terminate_handler) {
    std::set_terminate(g_previous_terminate);
  }
  SetUnhandledExceptionFilter(g_previous_exception_filter);
  std::signal(SIGABRT, g_previous_abort);
  _set_purecall_handler(g_previous_purecall);
  _set_invalid_parameter_handler(g_previous_invalid_parameter);
  SetErrorMode(g_previous_error_mode);
  _set_abort_behavior(g_previous_abort_behavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void console_write(int fd, const char* bytes, int count) {
  int offset = 0;
  while (offset < count) {
    const int written = _write(fd, bytes + offset, count - offset);
    if (written <= 0) return;
    offset += written;
  }
}

void reader_loop(State* state, PipeCapture* pipe) {
  char buffer[16384];
  for (;;) {
    const int count = _read(pipe->read_fd, buffer, sizeof(buffer));
    if (count <= 0) break;
    {
      std::lock_guard lock(state->log_mutex);
      state->log.write(buffer, count);
      state->log.flush();
    }
    if (state->preserve_console) console_write(pipe->saved_fd, buffer, count);
  }
  _close(pipe->read_fd);
  pipe->read_fd = -1;
}

void restore_capture(State& state) {
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);

  for (auto* pipe : {&state.output, &state.error}) {
    if (pipe->redirected) {
      _dup2(pipe->saved_fd, pipe->target_fd);
      pipe->redirected = false;
    }
    if (pipe->write_fd >= 0) {
      _close(pipe->write_fd);
      pipe->write_fd = -1;
    }
  }
  for (auto* pipe : {&state.output, &state.error}) {
    if (pipe->reader.joinable()) pipe->reader.join();
    if (pipe->read_fd >= 0) {
      _close(pipe->read_fd);
      pipe->read_fd = -1;
    }
    if (pipe->saved_fd >= 0) {
      _close(pipe->saved_fd);
      pipe->saved_fd = -1;
    }
  }
  if (state.log.is_open()) {
    state.log.flush();
    state.log.close();
  }
}

void rollback(State& state) {
  g_crash.armed.store(false, std::memory_order_release);
  if (state.handlers_installed) {
    uninstall_handlers();
    state.handlers_installed = false;
  }
  restore_capture(state);
  if (state.diagnostics_started) {
    diagnostics::shutdown();
    state.diagnostics_started = false;
  }
}

void finalize_manifest(Disposition disposition) {
  if (!g_paths_ready) return;
  std::string manifest = g_crash.manifest_prefix;
  manifest += disposition == Disposition::Normal ? "\"normal\"" : "\"abnormal\"";
  manifest += ",\"ended_at_utc\":" + json_escape(utc_now());
  if (disposition == Disposition::Normal) {
    manifest += ",\"crash\":null}\n";
  } else {
    manifest += ",\"crash\":{\"kind\":\"coordinator_abnormal\","
                "\"dump_written\":false}}\n";
  }
  write_file(g_crash.manifest_path, manifest);
}

}  // namespace

bool start(const Options& options, std::string& error) noexcept {
  std::lock_guard control(g_control);
  error.clear();
  if (g_state) {
    error = "Crash logging is already active";
    return false;
  }
  if (options.directory.empty()) {
    error = "Crash logging requires an explicit non-empty run directory";
    return false;
  }

  auto candidate = std::make_unique<State>();
  try {
    std::filesystem::create_directories(options.directory);
    if (!std::filesystem::is_directory(options.directory)) {
      error = "Crash logging path is not a directory";
      return false;
    }

    RunPaths paths;
    paths.directory = options.directory;
    paths.session_log = options.directory / "session.log";
    paths.events_jsonl = options.directory / "events.jsonl";
    paths.metrics_json = options.directory / "metrics.json";
    if (options.native_report)
      paths.native_report = options.directory / "native-report.json";
    paths.crash_text = options.directory / "crash.txt";
    paths.crash_dump = options.directory / "crash.dmp";
    paths.manifest_json = options.directory / "manifest.json";
    for (const auto* path : {&paths.crash_text, &paths.crash_dump, &paths.metrics_json}) {
      std::error_code ignored;
      std::filesystem::remove(*path, ignored);
    }
    if (!paths.native_report.empty()) {
      std::error_code ignored;
      std::filesystem::remove(paths.native_report, ignored);
    }

    candidate->preserve_console = options.preserve_console_output;
    candidate->log.open(paths.session_log, std::ios::binary | std::ios::trunc);
    if (!candidate->log.is_open()) {
      error = "Unable to open session.log";
      return false;
    }
    if (!diagnostics::start(paths.events_jsonl.string(), error)) {
      return false;
    }
    candidate->diagnostics_started = true;

    const auto started_at = utc_now();
    g_crash.manifest_prefix = make_manifest_prefix(options, started_at);
    g_crash.identity_text = make_identity_text(options, started_at);
    g_crash.metrics_path = paths.metrics_json.string();
    copy_precomputed_path(g_crash.crash_text_path, paths.crash_text);
    copy_precomputed_path(g_crash.crash_dump_path, paths.crash_dump);
    copy_precomputed_path(g_crash.manifest_path, paths.manifest_json);
    g_crash.handling.clear(std::memory_order_release);

    if (!write_file(g_crash.manifest_path,
                    g_crash.manifest_prefix +
                        "\"running\",\"ended_at_utc\":null,\"crash\":null}\n")) {
      error = "Unable to write manifest.json";
      rollback(*candidate);
      return false;
    }

    for (auto* pipe : {&candidate->output, &candidate->error}) {
      int descriptors[2]{-1, -1};
      if (_pipe(descriptors, static_cast<unsigned int>(kPipeBytes), _O_BINARY) != 0) {
        error = "Unable to create output capture pipe";
        rollback(*candidate);
        return false;
      }
      pipe->read_fd = descriptors[0];
      pipe->write_fd = descriptors[1];
      pipe->saved_fd = _dup(pipe->target_fd);
      if (pipe->saved_fd < 0) {
        error = "Unable to preserve console descriptor";
        rollback(*candidate);
        return false;
      }
    }

    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    candidate->output.reader = std::thread(reader_loop, candidate.get(), &candidate->output);
    candidate->error.reader = std::thread(reader_loop, candidate.get(), &candidate->error);

    for (auto* pipe : {&candidate->output, &candidate->error}) {
      if (_dup2(pipe->write_fd, pipe->target_fd) != 0) {
        error = "Unable to redirect CRT output descriptor";
        rollback(*candidate);
        return false;
      }
      pipe->redirected = true;
      _close(pipe->write_fd);
      pipe->write_fd = -1;
    }

    g_paths = paths;
    g_paths_ready = true;
    g_crash.armed.store(true, std::memory_order_release);
    install_handlers();
    candidate->handlers_installed = true;
    g_state = std::move(candidate);
    g_active.store(true, std::memory_order_release);
    return true;
  } catch (const std::exception& exception) {
    error = std::string("Crash logging start failed: ") + exception.what();
  } catch (...) {
    error = "Crash logging start failed with an unknown error";
  }
  rollback(*candidate);
  return false;
}

void shutdown(Disposition disposition) noexcept {
  std::lock_guard control(g_control);
  if (!g_state) return;
  g_active.store(false, std::memory_order_release);
  g_crash.armed.store(false, std::memory_order_release);
  try {
    if (g_state->handlers_installed) {
      uninstall_handlers();
      g_state->handlers_installed = false;
    }
    restore_capture(*g_state);
    diagnostics::flush();
    std::string ignored;
    metrics::write_snapshot(g_paths.metrics_json.string(), ignored);
    diagnostics::shutdown();
    g_state->diagnostics_started = false;
    finalize_manifest(disposition);
  } catch (...) {
    // Shutdown is a best-effort lifecycle boundary and cannot propagate through
    // coordinator cleanup. Whatever files reached disk remain available.
  }
  g_state.reset();
}

bool active() noexcept {
  return g_active.load(std::memory_order_acquire);
}

const RunPaths* paths() noexcept {
  return g_paths_ready ? &g_paths : nullptr;
}

}  // namespace slrr::crash_logging
