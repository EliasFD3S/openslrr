#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

namespace slrr::file_faults {

inline constexpr std::uint64_t kUnlimited =
    std::numeric_limits<std::uint64_t>::max();

enum class Operation : std::uint8_t {
  Open,
  Read,
  Write,
  Flush,
  Close,
  Delete,
  Copy,
  Rename,
};

enum class Fault : std::uint8_t {
  PermissionDenied,
  DiskFull,
  IoError,
  ShortRead,
  EndOfFile,
  PartialWrite,
};

// Rules are process-global, bounded, and persistent until reset or scope exit.
// fail_after_operations=N leaves the first N matching attempts untouched.
// fail_after_bytes=N admits the first N matching bytes, then applies the fault.
// Set the other threshold to kUnlimited when only one threshold is wanted.
// An empty path matches every path; otherwise either supplied path must match.
struct Rule {
  Operation operation = Operation::Open;
  Fault fault = Fault::IoError;
  std::uint64_t fail_after_operations = 0;
  std::uint64_t fail_after_bytes = kUnlimited;
  // Byte allowance for ShortRead/PartialWrite triggered by an operation or
  // already-exhausted byte threshold. A byte-threshold crossing instead admits
  // exactly the remaining bytes.
  std::uint64_t short_bytes = 0;
  std::filesystem::path path;
};

struct Decision {
  bool injected = false;
  // True for permission/disk-full/I/O failures. Short reads, EOF, partial
  // writes, and a request crossing a byte threshold return false with a reduced
  // allowed_bytes so the caller can reproduce its existing partial-I/O path.
  bool failure = false;
  Fault fault = Fault::IoError;
  std::uint64_t requested_bytes = 0;
  std::uint64_t allowed_bytes = 0;
  std::error_code error;
};

// Install or replace the rule for the same operation and normalized path scope.
// At most 32 rules are retained. Invalid short-operation combinations and
// allocation failures return false without installing a rule.
bool set_rule(const Rule& rule) noexcept;
void reset() noexcept;
bool enabled() noexcept;

// The test/probe interception point used by host persistence hooks. It never
// touches the filesystem itself and never includes paths or contents in events.
Decision before(Operation operation, std::uint64_t requested_bytes = 0,
                const std::filesystem::path& path = {},
                const std::filesystem::path& second_path = {}) noexcept;

// Nested scopes restore the exact previous rule set and threshold counters.
class ScopedRule {
 public:
  explicit ScopedRule(const Rule& rule);
  ~ScopedRule() noexcept;

  ScopedRule(const ScopedRule&) = delete;
  ScopedRule& operator=(const ScopedRule&) = delete;

 private:
  struct Snapshot;
  std::unique_ptr<Snapshot> previous_;
};

std::string_view operation_name(Operation operation) noexcept;
std::string_view fault_name(Fault fault) noexcept;
std::error_code error_code(Fault fault) noexcept;

}  // namespace slrr::file_faults
