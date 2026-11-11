#include "slrr/file_faults.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace slrr::file_faults {
namespace {

constexpr std::size_t kMaximumRules = 32;
constexpr char kFaultMetric[] = "host.file_faults_injected";

struct ActiveRule {
  Rule rule;
  std::uint64_t operations_seen = 0;
  std::uint64_t bytes_seen = 0;
};

struct State {
  std::mutex mutex;
  std::vector<ActiveRule> rules;
};

State& state() {
  static State value;
  return value;
}

std::atomic_bool& active() {
  static std::atomic_bool value{false};
  return value;
}

const bool g_metrics_registered = [] {
  return metrics::register_metric(kFaultMetric, metrics::Kind::Counter);
}();

void ensure_metric_registered() noexcept {
  (void)g_metrics_registered;
  (void)metrics::register_metric(kFaultMetric, metrics::Kind::Counter);
}

bool valid_operation(Operation operation) noexcept {
  switch (operation) {
    case Operation::Open:
    case Operation::Read:
    case Operation::Write:
    case Operation::Flush:
    case Operation::Close:
    case Operation::Delete:
    case Operation::Copy:
    case Operation::Rename:
      return true;
  }
  return false;
}

bool valid_rule(const Rule& rule) noexcept {
  if (!valid_operation(rule.operation)) return false;
  if ((rule.fault == Fault::ShortRead || rule.fault == Fault::EndOfFile) &&
      rule.operation != Operation::Read)
    return false;
  if (rule.fault == Fault::PartialWrite && rule.operation != Operation::Write)
    return false;
  switch (rule.fault) {
    case Fault::PermissionDenied:
    case Fault::DiskFull:
    case Fault::IoError:
    case Fault::ShortRead:
    case Fault::EndOfFile:
    case Fault::PartialWrite:
      return true;
  }
  return false;
}

std::filesystem::path normalized(const std::filesystem::path& path) {
  if (path.empty()) return {};
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  return (error ? path : canonical).lexically_normal();
}

bool same_path(const std::filesystem::path& left,
               const std::filesystem::path& right) {
  if (left.empty() || right.empty()) return left.empty() && right.empty();
  return normalized(left) == normalized(right);
}

bool path_matches(const ActiveRule& active_rule,
                  const std::filesystem::path& path,
                  const std::filesystem::path& second_path) {
  if (active_rule.rule.path.empty()) return true;
  return same_path(active_rule.rule.path, path) ||
         same_path(active_rule.rule.path, second_path);
}

bool install_rule_locked(State& s, const Rule& rule) {
  if (!valid_rule(rule)) return false;
  ActiveRule replacement;
  replacement.rule = rule;
  replacement.rule.path = normalized(rule.path);
  const auto existing = std::find_if(
      s.rules.begin(), s.rules.end(), [&](const ActiveRule& candidate) {
        return candidate.rule.operation == replacement.rule.operation &&
               candidate.rule.path == replacement.rule.path;
      });
  if (existing != s.rules.end()) {
    *existing = std::move(replacement);
    return true;
  }
  if (s.rules.size() >= kMaximumRules) return false;
  s.rules.push_back(std::move(replacement));
  return true;
}

void saturating_increment(std::uint64_t& value, std::uint64_t amount) noexcept {
  if (amount > kUnlimited - value) {
    value = kUnlimited;
  } else {
    value += amount;
  }
}

void emit(const Decision& decision, Operation operation) noexcept {
  if (!decision.injected) return;
  ensure_metric_registered();
  (void)metrics::add(kFaultMetric, 1);
  try {
    diagnostics::Event event;
    event.category = diagnostics::Category::File;
    event.name = "file.fault.injected";
    event.method = std::string(operation_name(operation));
    event.result = decision.failure ? "failure" : "partial";
    event.error = std::string(fault_name(decision.fault));
    event.values.push_back({"operation", static_cast<std::int64_t>(operation)});
    event.values.push_back(
        {"requested_bytes", static_cast<std::int64_t>(std::min<std::uint64_t>(
                                decision.requested_bytes,
                                static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max())))});
    event.values.push_back(
        {"allowed_bytes", static_cast<std::int64_t>(std::min<std::uint64_t>(
                              decision.allowed_bytes,
                              static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max())))});
    diagnostics::trace(std::move(event));
  } catch (...) {
    // Fault observation must never change the intercepted operation.
  }
}

}  // namespace

struct ScopedRule::Snapshot {
  std::vector<ActiveRule> rules;
};

bool set_rule(const Rule& rule) noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!install_rule_locked(s, rule)) return false;
    active().store(!s.rules.empty(), std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

void reset() noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.rules.clear();
    active().store(false, std::memory_order_release);
  } catch (...) {
  }
}

bool enabled() noexcept {
  return active().load(std::memory_order_acquire);
}

Decision before(Operation operation, std::uint64_t requested_bytes,
                const std::filesystem::path& path,
                const std::filesystem::path& second_path) noexcept {
  Decision decision;
  decision.requested_bytes = requested_bytes;
  decision.allowed_bytes = requested_bytes;
  if (!enabled()) return decision;
  try {
    auto& s = state();
    {
      std::lock_guard lock(s.mutex);
      for (auto& active_rule : s.rules) {
        if (active_rule.rule.operation != operation ||
            !path_matches(active_rule, path, second_path))
          continue;

        const auto& rule = active_rule.rule;
        const bool operation_threshold =
            active_rule.operations_seen >= rule.fail_after_operations;
        const bool byte_threshold =
            rule.fail_after_bytes != kUnlimited &&
            active_rule.bytes_seen >= rule.fail_after_bytes;
        const std::uint64_t remaining =
            rule.fail_after_bytes == kUnlimited
                ? kUnlimited
                : rule.fail_after_bytes -
                      std::min(rule.fail_after_bytes, active_rule.bytes_seen);
        const bool crosses_byte_threshold =
            rule.fail_after_bytes != kUnlimited && !byte_threshold &&
            requested_bytes > remaining;

        decision.fault = rule.fault;
        if (operation_threshold || byte_threshold) {
          decision.injected = true;
          switch (rule.fault) {
            case Fault::PermissionDenied:
            case Fault::DiskFull:
            case Fault::IoError:
              decision.failure = true;
              decision.allowed_bytes = 0;
              break;
            case Fault::ShortRead:
            case Fault::PartialWrite:
              decision.allowed_bytes =
                  std::min(requested_bytes, rule.short_bytes);
              break;
            case Fault::EndOfFile:
              decision.allowed_bytes = 0;
              break;
          }
        } else if (crosses_byte_threshold) {
          decision.injected = true;
          decision.allowed_bytes = remaining;
        }

        saturating_increment(active_rule.operations_seen, 1);
        saturating_increment(active_rule.bytes_seen,
                             decision.failure ? 0 : decision.allowed_bytes);
        decision.error = error_code(rule.fault);
        break;
      }
    }
    emit(decision, operation);
  } catch (...) {
    // An instrumentation failure fails open: production I/O keeps its baseline.
    return Decision{false, false, Fault::IoError, requested_bytes,
                    requested_bytes, {}};
  }
  return decision;
}

ScopedRule::ScopedRule(const Rule& rule)
    : previous_(std::make_unique<Snapshot>()) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  previous_->rules = s.rules;
  if (!install_rule_locked(s, rule))
    throw std::invalid_argument("Invalid or excess file fault rule");
  active().store(true, std::memory_order_release);
}

ScopedRule::~ScopedRule() noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.rules = std::move(previous_->rules);
    active().store(!s.rules.empty(), std::memory_order_release);
  } catch (...) {
    active().store(false, std::memory_order_release);
  }
}

std::string_view operation_name(Operation operation) noexcept {
  switch (operation) {
    case Operation::Open: return "open";
    case Operation::Read: return "read";
    case Operation::Write: return "write";
    case Operation::Flush: return "flush";
    case Operation::Close: return "close";
    case Operation::Delete: return "delete";
    case Operation::Copy: return "copy";
    case Operation::Rename: return "rename";
  }
  return "unknown";
}

std::string_view fault_name(Fault fault) noexcept {
  switch (fault) {
    case Fault::PermissionDenied: return "permission_denied";
    case Fault::DiskFull: return "disk_full";
    case Fault::IoError: return "io_error";
    case Fault::ShortRead: return "short_read";
    case Fault::EndOfFile: return "end_of_file";
    case Fault::PartialWrite: return "partial_write";
  }
  return "unknown";
}

std::error_code error_code(Fault fault) noexcept {
  switch (fault) {
    case Fault::PermissionDenied:
      return std::make_error_code(std::errc::permission_denied);
    case Fault::DiskFull:
      return std::make_error_code(std::errc::no_space_on_device);
    case Fault::IoError:
    case Fault::PartialWrite:
      return std::make_error_code(std::errc::io_error);
    case Fault::ShortRead:
    case Fault::EndOfFile:
      return {};
  }
  return std::make_error_code(std::errc::io_error);
}

}  // namespace slrr::file_faults
