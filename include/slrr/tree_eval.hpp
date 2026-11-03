#pragma once

#include "slrr/tufa.hpp"
#include "slrr/types.hpp"

#include <string>
#include <vector>

namespace slrr {

// Callbacks TREE → host (load / alloc / call). Quarried from slrr-reverse TreeHost.
struct TreeHost {
  virtual ~TreeHost() = default;
  virtual Object* tree_new(const std::string& class_fqn) = 0;
  virtual Value tree_call(const std::string& class_fqn, const std::string& method,
                          const std::string& jni_sig, Object* self,
                          const std::vector<Value>& args, bool is_static) = 0;
  virtual Value tree_call_by_name(const std::string& class_fqn,
                                  const std::string& method,
                                  const std::vector<Value>& args,
                                  bool prefer_static) = 0;
  virtual Value tree_get_static(const std::string& class_fqn,
                                const std::string& field) = 0;
  virtual void tree_set_static(const std::string& class_fqn,
                               const std::string& field, Value v) = 0;
  virtual Object* tree_new_string(const std::string& utf8) = 0;
  // `obj instanceof Type`, including the superclass chain.
  virtual bool tree_instance_of(Object* obj, const std::string& type) = 0;
  // 1 = static field, 2 = instance field, 0 = not declared (array length).
  virtual int tree_field_kind(const std::string& owner,
                              const std::string& field) = 0;
};

struct TreeEvalResult {
  bool ok = false;
  Value value;
  std::string error;
};

TreeEvalResult tree_eval(TreeHost* host, const TufaClass& cls,
                         const TufaMember& method, Object* self,
                         const std::vector<Value>& args);

}  // namespace slrr
