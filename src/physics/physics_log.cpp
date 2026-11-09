#include "slrr/physics_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace slrr {
namespace {
std::mutex log_mutex;
FILE *log_file = nullptr;
bool log_initialized = false;

void initialize_log() {
  if (log_initialized)
    return;
  log_initialized = true;
  if (!std::getenv("SLRR_PHYSICS_DUMP") &&
      !std::getenv("SLRR_PHYSICS_PROFILE"))
    return;
  const char *path = std::getenv("SLRR_PHYSICS_LOG");
  if (!path || !*path)
    path = "slrr-physics.log";
  log_file = std::fopen(path, "w");
  if (!log_file)
    std::fprintf(stderr, "[phys] cannot open log file: %s\n", path);
}
} // namespace

void physics_log(const char *format, ...) {
  std::lock_guard<std::mutex> lock(log_mutex);
  initialize_log();
  va_list args;
  va_start(args, format);
  va_list file_args;
  va_copy(file_args, args);
  std::vfprintf(stdout, format, args);
  if (log_file) {
    std::vfprintf(log_file, format, file_args);
    std::fflush(log_file);
  }
  va_end(file_args);
  va_end(args);
}
} // namespace slrr
