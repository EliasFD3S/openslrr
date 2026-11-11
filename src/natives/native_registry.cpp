#include "slrr/native_registry.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace slrr {
namespace {

constexpr const char* kRegisteredMetric = "natives.registered";
constexpr const char* kCalledMetric = "natives.called";
constexpr const char* kMissingMetric = "natives.missing";
constexpr const char* kNoopMetric = "natives.noop";

// Soft TUFA may omit void return ("(…)V" vs bare "(…)").
bool descriptor_match(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto norm = [](std::string s) {
    if (s.empty() || s[0] != '(') return s;
    const auto rp = s.find(')');
    if (rp == std::string::npos) return s;
    if (rp + 1 >= s.size()) s.push_back('V');
    return s;
  };
  return norm(a) == norm(b);
}

void saturating_increment(std::uint64_t& value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void saturating_add(std::uint64_t& value, std::uint64_t delta) noexcept {
  if (delta > std::numeric_limits<std::uint64_t>::max() - value) {
    value = std::numeric_limits<std::uint64_t>::max();
  } else {
    value += delta;
  }
}

std::int64_t metric_value(std::uint64_t value) noexcept {
  constexpr auto kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return static_cast<std::int64_t>(value > kMaximum ? kMaximum : value);
}

std::string current_section() {
  const auto section = diagnostics::current_context().section;
  return section.substr(0, NativeRegistry::kMaximumSectionBytes);
}

void note_invocation(NativeRuntimeStats& stats, bool success, bool missing,
                     std::uint64_t duration_ns) {
  saturating_increment(stats.invocation_count);
  if (missing) {
    saturating_increment(stats.missing_count);
  } else if (success) {
    saturating_increment(stats.success_count);
  } else {
    saturating_increment(stats.error_count);
  }
  saturating_add(stats.total_duration_ns, duration_ns);
  auto section = current_section();
  if (!section.empty()) stats.last_section = std::move(section);
}

void append_metrics(void* context, std::vector<metrics::Value>& values) {
  const auto* registry = static_cast<const NativeRegistry*>(context);
  if (!registry) return;
  const auto report = registry->report();
  values.push_back({kRegisteredMetric, metric_value(report.registered),
                    metrics::Kind::Gauge});
  values.push_back({kCalledMetric, metric_value(report.called),
                    metrics::Kind::Gauge});
  values.push_back({kMissingMetric, metric_value(report.missing),
                    metrics::Kind::Counter});
  values.push_back({kNoopMetric, metric_value(report.noop),
                    metrics::Kind::Gauge});
}

void append_json_string(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
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
}

const char* implementation_name(const NativeRuntimeStats& stats) {
  if (!stats.registered) return "missing";
  return stats.implementation == NativeImplementation::ExplicitNoop
             ? "explicit_noop"
             : "body";
}

}  // namespace

NativeRegistry::NativeRegistry() {
  metrics_provider_ = metrics::register_provider(append_metrics, this);
}

NativeRegistry::~NativeRegistry() {
  if (metrics_provider_ != 0) metrics::unregister_provider(metrics_provider_);
}

std::string NativeRegistry::key(const std::string& class_fqn,
                                const std::string& method,
                                const std::string& java_sig) {
  return class_fqn + "#" + method + "#" + java_sig;
}

void NativeRegistry::register_method(const std::string& class_fqn,
                                     const std::string& method,
                                     const std::string& java_sig, NativeFn fn,
                                     NativeImplementation implementation) {
  const std::string k = key(class_fqn, method, java_sig);
  if (methods_.count(k))
    throw std::runtime_error("Native already registered: " + k);
  Entry entry;
  entry.fn = std::move(fn);
  entry.implementation = implementation;
  entry.stats.class_name = class_fqn;
  entry.stats.method = method;
  entry.stats.signature = java_sig;
  entry.stats.implementation = implementation;
  methods_.emplace(k, std::move(entry));
}

bool NativeRegistry::has(const std::string& class_fqn, const std::string& method,
                         const std::string& java_sig) const {
  if (methods_.count(key(class_fqn, method, java_sig)) != 0) return true;
  const std::string prefix = class_fqn + "#" + method + "#";
  for (const auto& kv : methods_) {
    if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
    if (descriptor_match(kv.first.substr(prefix.size()), java_sig)) return true;
  }
  return false;
}

Value NativeRegistry::invoke(const std::string& class_fqn,
                             const std::string& method,
                             const std::string& java_sig, Object* self,
                             const std::vector<Value>& args) const {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  const bool tracing = diagnostics::enabled();
  auto elapsed = [&]() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
            .count());
  };
  auto trace_result = [&](const char* result, const char* error,
                          std::uint64_t duration_ns) noexcept {
    if (!tracing) return;
    try {
      diagnostics::Event event;
      event.category = diagnostics::Category::Native;
      event.name = "native.invoke";
      event.object = self;
      event.class_name = class_fqn;
      event.method = method;
      event.signature = java_sig;
      event.duration_ns = duration_ns;
      event.result = result;
      event.error = error;
      diagnostics::trace(std::move(event));
    } catch (...) {
    }
  };

  auto it = methods_.find(key(class_fqn, method, java_sig));
  if (it == methods_.end()) {
    const std::string prefix = class_fqn + "#" + method + "#";
    for (auto jt = methods_.begin(); jt != methods_.end(); ++jt) {
      if (jt->first.compare(0, prefix.size(), prefix) != 0) continue;
      if (descriptor_match(jt->first.substr(prefix.size()), java_sig)) {
        it = jt;
        break;
      }
    }
  }
  if (it == methods_.end()) {
    const std::string k = key(class_fqn, method, java_sig);
    auto missing = missing_.find(k);
    if (missing == missing_.end() &&
        missing_.size() < kMaximumMissingSignatures &&
        class_fqn.size() <= kMaximumMissingComponentBytes &&
        method.size() <= kMaximumMissingComponentBytes &&
        java_sig.size() <= kMaximumMissingComponentBytes) {
      Entry entry;
      entry.stats.class_name = class_fqn;
      entry.stats.method = method;
      entry.stats.signature = java_sig;
      entry.stats.registered = false;
      missing = missing_.emplace(k, std::move(entry)).first;
    }
    if (missing == missing_.end()) {
      saturating_increment(totals_.untracked_missing_invocations);
    } else {
      note_invocation(missing->second.stats, false, true, elapsed());
    }
    saturating_increment(totals_.missing);
    const std::string message =
        "Missing native: " + class_fqn + "#" + method + "#" + java_sig;
    trace_result("error", message.c_str(), elapsed());
    throw std::runtime_error(message);
  }

  try {
    Value result = it->second.fn(self, args);
    const auto duration = elapsed();
    note_invocation(it->second.stats, true, false, duration);
    saturating_increment(totals_.success);
    trace_result("ok", "", duration);
    return result;
  } catch (const std::exception& error) {
    const auto duration = elapsed();
    note_invocation(it->second.stats, false, false, duration);
    saturating_increment(totals_.error);
    trace_result("error", error.what(), duration);
    throw;
  } catch (...) {
    const auto duration = elapsed();
    note_invocation(it->second.stats, false, false, duration);
    saturating_increment(totals_.error);
    trace_result("error", "non-standard exception", duration);
    throw;
  }
}

std::vector<std::string> NativeRegistry::signatures() const {
  std::vector<std::string> out;
  out.reserve(methods_.size());
  for (const auto& kv : methods_) out.push_back(kv.first);
  return out;
}

NativeReport NativeRegistry::report() const {
  NativeReport out;
  out.registered = methods_.size();
  out.success = totals_.success;
  out.error = totals_.error;
  out.missing = totals_.missing;
  out.untracked_missing_invocations = totals_.untracked_missing_invocations;
  out.signatures.reserve(methods_.size() + missing_.size());
  for (const auto& [unused, entry] : methods_) {
    (void)unused;
    if (entry.implementation == NativeImplementation::ExplicitNoop)
      saturating_increment(out.noop);
    if (entry.stats.invocation_count != 0) saturating_increment(out.called);
    out.signatures.push_back(entry.stats);
  }
  for (const auto& [unused, entry] : missing_) {
    (void)unused;
    if (entry.stats.invocation_count != 0) saturating_increment(out.called);
    out.signatures.push_back(entry.stats);
  }
  std::sort(out.signatures.begin(), out.signatures.end(),
            [](const NativeRuntimeStats& a, const NativeRuntimeStats& b) {
              return std::tie(a.class_name, a.method, a.signature, a.registered) <
                     std::tie(b.class_name, b.method, b.signature, b.registered);
            });
  return out;
}

std::string NativeRegistry::report_json() const {
  const auto current = report();
  std::string out = "{\"version\":1,\"summary\":{";
  out += "\"registered\":" + std::to_string(current.registered);
  out += ",\"called\":" + std::to_string(current.called);
  out += ",\"success\":" + std::to_string(current.success);
  out += ",\"error\":" + std::to_string(current.error);
  out += ",\"missing\":" + std::to_string(current.missing);
  out += ",\"noop\":" + std::to_string(current.noop);
  out += ",\"untracked_missing_invocations\":" +
         std::to_string(current.untracked_missing_invocations);
  out += "},\"signatures\":[";
  for (std::size_t i = 0; i < current.signatures.size(); ++i) {
    const auto& stats = current.signatures[i];
    if (i != 0) out.push_back(',');
    out += "{\"class\":";
    append_json_string(out, stats.class_name);
    out += ",\"method\":";
    append_json_string(out, stats.method);
    out += ",\"signature\":";
    append_json_string(out, stats.signature);
    out += ",\"registered\":";
    out += stats.registered ? "true" : "false";
    out += ",\"implementation\":";
    append_json_string(out, implementation_name(stats));
    out += ",\"invocation_count\":" + std::to_string(stats.invocation_count);
    out += ",\"success_count\":" + std::to_string(stats.success_count);
    out += ",\"error_count\":" + std::to_string(stats.error_count);
    out += ",\"missing_count\":" + std::to_string(stats.missing_count);
    out += ",\"total_duration_ns\":" +
           std::to_string(stats.total_duration_ns);
    out += ",\"last_section\":";
    if (stats.last_section) {
      append_json_string(out, *stats.last_section);
    } else {
      out += "null";
    }
    out += "}";
  }
  out += "]}";
  return out;
}

bool NativeRegistry::write_report(const std::string& path,
                                  std::string& error) const noexcept {
  try {
    error.clear();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      error = "Unable to open native report: " + path;
      return false;
    }
    output << report_json() << '\n';
    output.flush();
    if (!output) {
      error = "Unable to write native report: " + path;
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    try {
      error = exception.what();
    } catch (...) {
    }
    return false;
  } catch (...) {
    return false;
  }
}

}  // namespace slrr
