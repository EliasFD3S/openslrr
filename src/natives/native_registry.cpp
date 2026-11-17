#include "slrr/native_registry.hpp"

#include <stdexcept>

namespace slrr {
namespace {

// Only the omitted void return is an alias; keep exact registrations first.
std::string alternate_descriptor(const std::string& sig) {
  if (sig.empty() || sig.front() != '(') return {};
  const auto rp = sig.find(')');
  if (rp == std::string::npos) return {};
  if (rp + 1 == sig.size()) return sig + "V";
  if (rp + 2 == sig.size() && sig.back() == 'V') return sig.substr(0, rp + 1);
  return {};
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
  const auto alternate = alternate_descriptor(java_sig);
  return !alternate.empty() && methods_.count(key(class_fqn, method, alternate)) != 0;
}

Value NativeRegistry::invoke(const std::string& class_fqn,
                             const std::string& method,
                             const std::string& java_sig, Object* self,
                             const std::vector<Value>& args) const {
  auto it = methods_.find(key(class_fqn, method, java_sig));
  if (it == methods_.end()) {
    const auto alternate = alternate_descriptor(java_sig);
    if (!alternate.empty()) it = methods_.find(key(class_fqn, method, alternate));
  }
  if (it == methods_.end())
    throw std::runtime_error("Missing native: " + class_fqn + "#" + method +
                             "#" + java_sig);
  return it->second(self, args);
}

std::vector<std::string> NativeRegistry::signatures() const {
  std::vector<std::string> out;
  out.reserve(methods_.size());
  for (const auto& kv : methods_) out.push_back(kv.first);
  return out;
}

}  // namespace slrr
