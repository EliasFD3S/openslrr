#include "slrr/native_registry.hpp"
#include "slrr/diagnostics.hpp"

#include <chrono>
#include <stdexcept>

namespace slrr {
namespace {

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

}  // namespace

std::string NativeRegistry::key(const std::string& class_fqn,
                                const std::string& method,
                                const std::string& java_sig) {
  return class_fqn + "#" + method + "#" + java_sig;
}

void NativeRegistry::register_method(const std::string& class_fqn,
                                     const std::string& method,
                                     const std::string& java_sig, NativeFn fn) {
  const std::string k = key(class_fqn, method, java_sig);
  if (methods_.count(k))
    throw std::runtime_error("Native already registered: " + k);
  methods_.emplace(k, std::move(fn));
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
  const bool tracing = diagnostics::enabled();
  const auto started = tracing ? Clock::now() : Clock::time_point{};
  auto trace_result = [&](const char* result, const char* error) noexcept {
    if (!tracing) return;
    try {
      diagnostics::Event event;
      event.category = diagnostics::Category::Native;
      event.name = "native.invoke";
      event.object = self;
      event.class_name = class_fqn;
      event.method = method;
      event.signature = java_sig;
      event.duration_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - started).count());
      event.result = result;
      event.error = error;
      diagnostics::trace(std::move(event));
    } catch (...) {
    }
  };

  try {
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
    if (it == methods_.end())
      throw std::runtime_error("Missing native: " + class_fqn + "#" + method +
                               "#" + java_sig);
    Value result = it->second(self, args);
    trace_result("ok", "");
    return result;
  } catch (const std::exception& error) {
    trace_result("error", error.what());
    throw;
  } catch (...) {
    trace_result("error", "non-standard exception");
    throw;
  }
}

std::vector<std::string> NativeRegistry::signatures() const {
  std::vector<std::string> out;
  out.reserve(methods_.size());
  for (const auto& kv : methods_) out.push_back(kv.first);
  return out;
}

}  // namespace slrr
