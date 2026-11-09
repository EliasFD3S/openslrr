#pragma once

#include "slrr/native_registry.hpp"
#include "slrr/object.hpp"
#include "slrr/tree_eval.hpp"
#include "slrr/tufa.hpp"
#include "slrr/gc_roots.hpp"
#include "slrr/types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slrr {

class Jvm : public TreeHost {
 public:
  explicit Jvm(NativeRegistry* natives);
  ~Jvm();
  struct ExecutionState { int calls=0, invokes=0, constructors=0; };
  void exchange_execution_state(ExecutionState& state);
  class RootScope {
   public:
    RootScope(Jvm& vm, GcRootProvider provider);
    ~RootScope();
    RootScope(const RootScope&)=delete;
   private:
    Jvm& vm_; uint64_t id_;
  };

  void set_game_root(std::string root);
  const std::string& game_root() const { return game_root_; }

  NativeRegistry& natives() { return *natives_; }
  const NativeRegistry& natives() const { return *natives_; }

  bool bootstrap_classpath();
  bool load_class(const std::string& fqn_or_path);
  bool load_class_file(const std::string& path);
  const TufaClass* find_class(const std::string& fqn) const;

  Object* new_object(const std::string& class_fqn);
  Object* new_string(const std::string& utf8);

  Value invoke(const std::string& class_fqn, const std::string& method,
               const std::string& jni_sig, const std::vector<Value>& args,
               bool is_static);

  Value invoke_by_name(const std::string& class_fqn, const std::string& method,
                       const std::vector<Value>& args, bool prefer_static);

  Value get_static(const std::string& class_fqn, const std::string& field);
  void set_static(const std::string& class_fqn, const std::string& field,
                  Value v);

  bool classpath_ready() const { return classpath_ready_; }
  int classes_loaded() const { return static_cast<int>(by_name_.size()); }
  void add_gc_roots(GcRootProvider provider);
  int request_gc(); // -1: queued/busy; collection runs at the host safe point.
  int collect_garbage();
  void gc_safe_point();
  size_t heap_size() const { return heap_.size(); }
  size_t gc_last_freed() const { return gc_last_freed_; }

  double advance_sim_time(double seconds);
  double sim_time() const { return sim_time_; }
  float time_warp(float scale);

  // While set, the first native method with no host binding aborts that call
  // chain (nested TREE invokes propagate the stop to the outermost invoke).
  void set_halt_on_unbound(bool on) { halt_on_unbound_ = on; }
  void clear_unbound() { unbound_.clear(); }
  const std::string& unbound_native() const { return unbound_; }

  // TreeHost
  Object* tree_new(const std::string& class_fqn) override;
  Value tree_call(const std::string& class_fqn, const std::string& method,
                  const std::string& jni_sig, Object* self,
                  const std::vector<Value>& args, bool is_static) override;
  Value tree_call_by_name(const std::string& class_fqn,
                          const std::string& method,
                          const std::vector<Value>& args,
                          bool prefer_static) override;
  Value tree_get_static(const std::string& class_fqn,
                        const std::string& field) override;
  void tree_set_static(const std::string& class_fqn, const std::string& field,
                       Value v) override;
  Object* tree_new_string(const std::string& utf8) override;
  bool tree_instance_of(Object* obj, const std::string& type) override;
  int tree_field_kind(const std::string& owner,
                      const std::string& field) override;

 private:
  double sim_time_ = 0.0;
  float time_warp_ = 1.0f;

  struct GcCallScope {
    int& depth;
    explicit GcCallScope(int& d) : depth(d) { ++depth; }
    ~GcCallScope() { --depth; }
  };
  int gc_call_depth_ = 0;
  bool gc_pending_ = false, gc_running_ = false, gc_report_ready_ = false;
  int gc_report_ = 0;
  size_t gc_allocated_ = 0, gc_last_freed_ = 0, gc_threshold_ = 8192;
  uint32_t gc_next_identity_ = 1;
  std::vector<GcRootProvider> gc_roots_;
  uint64_t gc_frame_id_=0;
  std::unordered_map<uint64_t,GcRootProvider> gc_frames_;
  std::string resolve_class_path(const std::string& fqn) const;
  void seed_static_fields(const TufaClass& cls);
  void enrich_static_fields(const TufaClass& cls);
  void apply_field_defaults(Object* obj);
  const TufaMember* find_method(const TufaClass& cls, const std::string& method,
                                const std::string& jni_sig,
                                bool prefer_static) const;
  const TufaMember* find_method_by_name(
      const TufaClass& cls, const std::string& method, int argc,
      bool prefer_static, const std::vector<Value>* runtime = nullptr,
      int* match_score = nullptr) const;
  // Walk class → super… for TREE member + declaring class FQN.
  const TufaMember* find_method_inherited(const std::string& class_fqn,
                                          const std::string& method,
                                          const std::string& jni_sig,
                                          bool prefer_static,
                                          std::string* out_owner_fqn) const;
  // Native registry lookup along the same inheritance chain.
  bool find_native_owner(const std::string& class_fqn, const std::string& method,
                         const std::string& jni_sig,
                         std::string* out_owner_fqn) const;
  // TUFA uses short class names; host natives register FQNs — try both.
  bool native_has_alias(const std::string& class_name, const std::string& method,
                        const std::string& jni_sig,
                        std::string* out_registry_key) const;
  static int count_args(const std::string& jni_sig);
  void halt_unbound(const std::string& owner, const std::string& method,
                    const std::string& jni);

  NativeRegistry* natives_ = nullptr;
  std::string game_root_;
  bool classpath_ready_ = false;
  std::vector<std::pair<std::string, std::string>> classpath_;
  std::unordered_map<std::string, TufaClass> by_name_;
  // Class files currently inside load_class_file. A field path during static
  // init must not enrich the same file again.
  std::unordered_set<std::string> loading_paths_;
  std::vector<std::unique_ptr<Object>> heap_;
  std::unordered_map<std::string, std::unordered_map<std::string, Value>>
      statics_;
  int invoke_depth_ = 0;
  int field_init_depth_ = 0;
  bool halt_on_unbound_ = false;
  std::string unbound_;
};

}  // namespace slrr
