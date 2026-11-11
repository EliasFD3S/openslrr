#pragma once

#include "slrr/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {

// Mirrors stock JVM_RegisterNative: class + method + JNI-like signature.
// Fail-closed: missing natives throw (no silent success).
using NativeFn = std::function<Value(Object* self, const std::vector<Value>& args)>;

enum class NativeImplementation {
  Body,
  ExplicitNoop,
};

struct NativeRuntimeStats {
  std::string class_name;
  std::string method;
  std::string signature;
  bool registered = true;
  NativeImplementation implementation = NativeImplementation::Body;
  std::uint64_t invocation_count = 0;
  std::uint64_t success_count = 0;
  std::uint64_t error_count = 0;
  std::uint64_t missing_count = 0;
  std::uint64_t total_duration_ns = 0;
  std::optional<std::string> last_section;
};

struct NativeReport {
  std::uint64_t registered = 0;
  std::uint64_t called = 0;
  std::uint64_t success = 0;
  std::uint64_t error = 0;
  std::uint64_t missing = 0;
  std::uint64_t noop = 0;
  std::uint64_t untracked_missing_invocations = 0;
  std::vector<NativeRuntimeStats> signatures;
};

class NativeRegistry {
 public:
  NativeRegistry();
  ~NativeRegistry();
  NativeRegistry(const NativeRegistry&) = delete;
  NativeRegistry& operator=(const NativeRegistry&) = delete;
  NativeRegistry(NativeRegistry&&) = delete;
  NativeRegistry& operator=(NativeRegistry&&) = delete;

  static constexpr std::size_t kMaximumMissingSignatures = 128;
  static constexpr std::size_t kMaximumMissingComponentBytes = 512;
  static constexpr std::size_t kMaximumSectionBytes = 256;

  void register_method(const std::string& class_fqn, const std::string& method,
                       const std::string& java_sig, NativeFn fn,
                       NativeImplementation implementation =
                           NativeImplementation::Body);

  bool has(const std::string& class_fqn, const std::string& method,
           const std::string& java_sig) const;

  Value invoke(const std::string& class_fqn, const std::string& method,
               const std::string& java_sig, Object* self,
               const std::vector<Value>& args) const;

  std::vector<std::string> signatures() const;

  // The report contains metadata and aggregate counts only: no argument values,
  // script locals, object addresses, or behavioral-verification claims.
  NativeReport report() const;
  std::string report_json() const;
  bool write_report(const std::string& path, std::string& error) const noexcept;

 private:
  struct Entry {
    NativeFn fn;
    NativeImplementation implementation = NativeImplementation::Body;
    mutable NativeRuntimeStats stats;
  };
  struct Totals {
    std::uint64_t success = 0;
    std::uint64_t error = 0;
    std::uint64_t missing = 0;
    std::uint64_t untracked_missing_invocations = 0;
  };

  static std::string key(const std::string& class_fqn, const std::string& method,
                         const std::string& java_sig);
  std::unordered_map<std::string, Entry> methods_;
  mutable std::unordered_map<std::string, Entry> missing_;
  mutable Totals totals_;
  std::uint64_t metrics_provider_ = 0;
};

}  // namespace slrr
