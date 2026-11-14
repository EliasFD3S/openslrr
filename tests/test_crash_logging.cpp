#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/crash_logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ChildResult {
  bool launched = false;
  bool timed_out = false;
  DWORD exit_code = 0;
  std::string output;
  std::string error;
};

std::filesystem::path fixture_path() {
  std::array<wchar_t, 32768> buffer{};
  const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
  SLRR_REQUIRE(length > 0 && length < buffer.size());
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() /
         "slrr_crash_logging_fixture.exe";
}

std::wstring quote_argument(const std::wstring& argument) {
  if (argument.empty()) return L"\"\"";
  if (argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const auto character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring quote_argument(const std::filesystem::path& argument) {
  return quote_argument(argument.wstring());
}

void read_pipe(HANDLE pipe, std::string& destination) {
  std::array<char, 16384> buffer{};
  for (;;) {
    DWORD count = 0;
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &count, nullptr) ||
        count == 0) {
      break;
    }
    destination.append(buffer.data(), count);
  }
}

ChildResult run_fixture(const std::string& mode,
                        const std::filesystem::path& directory,
                        const std::vector<std::wstring>& extra = {}) {
  std::wstring command = quote_argument(fixture_path());
  command += L" " + quote_argument(std::wstring(mode.begin(), mode.end()));
  command += L" " + quote_argument(directory);
  for (const auto& argument : extra) command += L" " + quote_argument(argument);

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  HANDLE error_read = nullptr;
  HANDLE error_write = nullptr;
  SLRR_REQUIRE(CreatePipe(&output_read, &output_write, &security, 0));
  SLRR_REQUIRE(CreatePipe(&error_read, &error_write, &security, 0));
  SLRR_REQUIRE(SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0));
  SLRR_REQUIRE(SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_write;
  startup.hStdError = error_write;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL created = CreateProcessW(
      nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, nullptr, &startup, &process);
  CloseHandle(output_write);
  CloseHandle(error_write);
  SLRR_REQUIRE(created);

  ChildResult result;
  result.launched = true;
  std::thread output_reader(read_pipe, output_read, std::ref(result.output));
  std::thread error_reader(read_pipe, error_read, std::ref(result.error));
  if (WaitForSingleObject(process.hProcess, 30000) == WAIT_TIMEOUT) {
    result.timed_out = true;
    TerminateProcess(process.hProcess, 99);
    WaitForSingleObject(process.hProcess, 5000);
  }
  GetExitCodeProcess(process.hProcess, &result.exit_code);
  output_reader.join();
  error_reader.join();
  CloseHandle(output_read);
  CloseHandle(error_read);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return result;
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void require_dump(const std::filesystem::path& directory) {
  const auto dump = slrr::test::read_binary_file(directory / "crash.dmp");
  SLRR_REQUIRE(dump.size() > 4);
  SLRR_REQUIRE(dump[0] == 'M' && dump[1] == 'D' && dump[2] == 'M' &&
               dump[3] == 'P');
}

void require_abnormal(const ChildResult& child,
                      const std::filesystem::path& directory,
                      std::string_view kind) {
  SLRR_REQUIRE(child.launched);
  SLRR_REQUIRE(!child.timed_out);
  SLRR_REQUIRE(child.exit_code != 0);
  const auto session = slrr::test::read_text_file(directory / "session.log");
  SLRR_REQUIRE(session.find("fixture-printf-stdout") != std::string::npos);
  SLRR_REQUIRE(session.find("fixture-fprintf-stderr") != std::string::npos);
  const auto crash = slrr::test::read_text_file(directory / "crash.txt");
  SLRR_REQUIRE(crash.find("disposition: abnormal") != std::string::npos);
  SLRR_REQUIRE(crash.find("kind: " + std::string(kind)) != std::string::npos);
  SLRR_REQUIRE(crash.find("build_id: fixture-build-123") != std::string::npos);
  SLRR_REQUIRE(crash.find("recent_context:") != std::string::npos);
  SLRR_REQUIRE(crash.find("fixture.CrashLogging") != std::string::npos);
  SLRR_REQUIRE(crash.find("fixture.event") != std::string::npos);
  const auto manifest = slrr::test::read_text_file(directory / "manifest.json");
  SLRR_REQUIRE(manifest.find("\"disposition\":\"abnormal\"") !=
               std::string::npos);
  SLRR_REQUIRE(manifest.find("\"dump_written\":true") != std::string::npos);
  SLRR_REQUIRE(manifest.find("\"recent_diagnostics\":") != std::string::npos);
  require_dump(directory);
}

}  // namespace

SLRR_TEST(crash_logging_requires_explicit_directory) {
  slrr::test::TemporaryDirectory temporary("crash-logging-negative");
  slrr::crash_logging::Options options;
  std::string error;
  SLRR_REQUIRE(!slrr::crash_logging::start(options, error));
  SLRR_REQUIRE(!error.empty());
  SLRR_REQUIRE(!slrr::crash_logging::active());
  SLRR_REQUIRE(slrr::crash_logging::paths() == nullptr);
  SLRR_REQUIRE(!std::filesystem::exists(temporary.path() / "unexpected"));
}

SLRR_TEST(crash_logging_normal_child_captures_output_and_final_files) {
  slrr::test::TemporaryDirectory temporary("crash-logging-normal");
  const auto directory = temporary.path() / "selected" / "run";
  const auto child = run_fixture("normal", directory,
                                 {L"--token", L"fixture-secret-value"});
  SLRR_REQUIRE(child.launched);
  SLRR_REQUIRE(!child.timed_out);
  SLRR_REQUIRE(child.exit_code == 0);

  const auto session = slrr::test::read_text_file(directory / "session.log");
  SLRR_REQUIRE(session.find("fixture-printf-stdout") != std::string::npos);
  SLRR_REQUIRE(session.find("fixture-fprintf-stderr") != std::string::npos);
  SLRR_REQUIRE(session.find("fixture-native-stdout") != std::string::npos);
  SLRR_REQUIRE(session.find("fixture-native-stderr") != std::string::npos);
  SLRR_REQUIRE(session.size() > 500000);
  SLRR_REQUIRE(occurrences(session, "fixture-bulk-stdout") == 32);
  SLRR_REQUIRE(session.find("fixture-after-shutdown") == std::string::npos);

  SLRR_REQUIRE(child.output.find("fixture-printf-stdout") != std::string::npos);
  SLRR_REQUIRE(child.output.find("fixture-native-stdout") != std::string::npos);
  SLRR_REQUIRE(child.output.find("fixture-after-shutdown") != std::string::npos);
  SLRR_REQUIRE(child.error.find("fixture-fprintf-stderr") != std::string::npos);
  SLRR_REQUIRE(child.error.find("fixture-native-stderr") != std::string::npos);
  SLRR_REQUIRE(child.output.find("fixture-fprintf-stderr") == std::string::npos);

  const auto events = slrr::test::read_text_file(directory / "events.jsonl");
  SLRR_REQUIRE(events.find("\"event\":\"fixture.event\"") !=
               std::string::npos);
  SLRR_REQUIRE(events.find("\"section\":\"fixture.CrashLogging\"") !=
               std::string::npos);
  SLRR_REQUIRE(events.find("\"detail\":\"normal\"") != std::string::npos);
  const auto metrics = slrr::test::read_text_file(directory / "metrics.json");
  SLRR_REQUIRE(metrics.find("\"fixture.counter\":19") != std::string::npos);

  const auto manifest = slrr::test::read_text_file(directory / "manifest.json");
  SLRR_REQUIRE(manifest.find("\"disposition\":\"normal\"") !=
               std::string::npos);
  SLRR_REQUIRE(manifest.find("\"started_at_utc\":\"") != std::string::npos);
  SLRR_REQUIRE(manifest.find("\"ended_at_utc\":\"") != std::string::npos);
  SLRR_REQUIRE(manifest.find("\"id\":\"fixture-build-123\"") !=
               std::string::npos);
  SLRR_REQUIRE(manifest.find("\"pid\":") != std::string::npos);
  SLRR_REQUIRE(manifest.find("fixture-secret-value") == std::string::npos);
  SLRR_REQUIRE(manifest.find("--token <redacted>") != std::string::npos);
  SLRR_REQUIRE(!std::filesystem::exists(directory / "crash.txt"));
  SLRR_REQUIRE(!std::filesystem::exists(directory / "crash.dmp"));
}

SLRR_TEST(crash_logging_cpp_child_records_exception_and_dump) {
  slrr::test::TemporaryDirectory temporary("crash-logging-cpp");
  const auto child = run_fixture("cpp", temporary.path());
  require_abnormal(child, temporary.path(), "unhandled_cpp_exception");
  const auto crash = slrr::test::read_text_file(temporary.path() / "crash.txt");
  SLRR_REQUIRE(crash.find("code: 0xe06d7363") != std::string::npos);
  SLRR_REQUIRE(crash.find("what() unavailable in SEH filter") != std::string::npos);
}

SLRR_TEST(crash_logging_terminate_child_captures_exception_what) {
  slrr::test::TemporaryDirectory temporary("crash-logging-terminate");
  const auto child = run_fixture("terminate", temporary.path());
  require_abnormal(child, temporary.path(), "unhandled_cpp_exception");
  const auto crash = slrr::test::read_text_file(temporary.path() / "crash.txt");
  SLRR_REQUIRE(crash.find("fixture-terminate-cpp-exception") != std::string::npos);
}

SLRR_TEST(crash_logging_seh_child_records_text_and_real_dump) {
#if defined(__SANITIZE_ADDRESS__)
  // AddressSanitizer installs its own access-violation handler, which reports
  // and terminates the instrumented child before (or instead of) the crash
  // logger's unhandled-exception filter. The normal lane covers this path.
  std::printf("skipped under AddressSanitizer\n");
#else
  slrr::test::TemporaryDirectory temporary("crash-logging-seh");
  const auto child = run_fixture("seh", temporary.path());
  require_abnormal(child, temporary.path(), "windows_seh");
  const auto crash = slrr::test::read_text_file(temporary.path() / "crash.txt");
  SLRR_REQUIRE(crash.find("code: 0xc0000005") != std::string::npos);
#endif
}

SLRR_TEST(crash_logging_crt_termination_children_record_causes) {
  const std::array<std::pair<std::string, std::string>, 3> modes = {{
      {"abort", "abort"},
      {"purecall", "purecall"},
      {"invalid", "invalid_parameter"},
  }};
  for (const auto& [mode, kind] : modes) {
    slrr::test::TemporaryDirectory temporary("crash-logging-" + mode);
    const auto child = run_fixture(mode, temporary.path());
    require_abnormal(child, temporary.path(), kind);
  }
}
