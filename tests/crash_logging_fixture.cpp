#include "slrr/crash_logging.hpp"
#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <io.h>
#include <stdexcept>
#include <string>

namespace {

void write_all(int fd, const char* bytes, int size) {
  int offset = 0;
  while (offset < size) {
    const int written = _write(fd, bytes + offset, size - offset);
    if (written <= 0) std::_Exit(90);
    offset += written;
  }
}

void emit_process_output(bool bulk) {
  std::printf("fixture-printf-stdout\n");
  std::fprintf(stderr, "fixture-fprintf-stderr\n");
  write_all(1, "fixture-native-stdout\n", 22);
  write_all(2, "fixture-native-stderr\n", 22);
  if (bulk) {
    std::string block(16384, 'x');
    block += "fixture-bulk-stdout\n";
    for (int i = 0; i < 16; ++i) {
      write_all(1, block.data(), static_cast<int>(block.size()));
      write_all(2, block.data(), static_cast<int>(block.size()));
    }
  }
  std::fflush(nullptr);
}

void emit_structured_output(const char* mode) {
  slrr::diagnostics::set_frame(42);
  slrr::diagnostics::set_tick(7);
  slrr::diagnostics::set_section("fixture.CrashLogging");
  slrr::diagnostics::set_fiber(11);
  slrr::diagnostics::Event event;
  event.name = "fixture.event";
  event.detail = mode;
  event.result = "fixture-result";
  slrr::diagnostics::trace(std::move(event));
  if (!slrr::metrics::register_metric("fixture.counter",
                                      slrr::metrics::Kind::Counter) ||
      !slrr::metrics::add("fixture.counter", 19)) {
    std::_Exit(91);
  }
}

__declspec(noinline) void cause_seh() {
  volatile int* invalid = nullptr;
  *invalid = 0x534c5252;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 64;
  const std::string mode = argv[1];
  slrr::crash_logging::Options options;
  options.directory = argv[2];
  options.preserve_console_output = true;
  options.build_id = "fixture-build-123";
  options.build_configuration = "fixture-release";
  std::string error;
  if (!slrr::crash_logging::start(options, error)) {
    std::fprintf(stderr, "fixture-start-error: %s\n", error.c_str());
    return 65;
  }
  emit_process_output(mode == "normal");
  emit_structured_output(argv[1]);

  if (mode == "normal") {
    slrr::crash_logging::shutdown();
    std::printf("fixture-after-shutdown\n");
    std::fflush(stdout);
    return 0;
  }
  if (mode == "cpp") {
    throw std::runtime_error("fixture-unhandled-cpp-exception");
  }
  if (mode == "terminate") {
    try {
      throw std::runtime_error("fixture-terminate-cpp-exception");
    } catch (...) {
      std::terminate();
    }
  }
  if (mode == "seh") {
    cause_seh();
    std::_Exit(92);
  }
  if (mode == "abort") {
    std::abort();
  }
  if (mode == "purecall") {
    _purecall();
  }
  if (mode == "invalid") {
    _close(-1);
    std::_Exit(93);
  }
  std::_Exit(94);
}
