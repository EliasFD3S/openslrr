#pragma once

#include "slrr/types.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {

// Mirrors stock JVM_RegisterNative: class + method + JNI-like signature.
// Fail-closed: missing natives throw (no silent success).
using NativeFn = std::function<Value(Object* self, const std::vector<Value>& args)>;

class NativeRegistry {
 public:
  void register_method(const std::string& class_fqn, const std::string& method,
                       const std::string& java_sig, NativeFn fn);

  bool has(const std::string& class_fqn, const std::string& method,
           const std::string& java_sig) const;

  Value invoke(const std::string& class_fqn, const std::string& method,
               const std::string& java_sig, Object* self,
               const std::vector<Value>& args) const;

  std::vector<std::string> signatures() const;

 private:
  static std::string key(const std::string& class_fqn, const std::string& method,
                         const std::string& java_sig);
  std::unordered_map<std::string, NativeFn> methods_;
};

}  // namespace slrr
