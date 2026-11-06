#include "slrr/jvm.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <unordered_set>

namespace slrr {
Jvm::RootScope::RootScope(Jvm& vm,GcRootProvider provider):vm_(vm),id_(++vm.gc_frame_id_) {
  vm_.gc_frames_.emplace(id_,std::move(provider));
}
Jvm::RootScope::~RootScope(){vm_.gc_frames_.erase(id_);}
void Jvm::exchange_execution_state(ExecutionState& state) {
  std::swap(state.calls,gc_call_depth_);
  std::swap(state.invokes,invoke_depth_);
  std::swap(state.constructors,field_init_depth_);
}
void Jvm::add_gc_roots(GcRootProvider provider) {
  gc_roots_.push_back(std::move(provider));
}
int Jvm::request_gc() {
  if (gc_report_ready_ && !gc_running_) {
    gc_report_ready_ = false;
    return gc_report_;
  }
  gc_pending_ = true;
  return -1;
}
void Jvm::gc_safe_point() {
  if (gc_pending_ || gc_allocated_ >= gc_threshold_) collect_garbage();
}
int Jvm::collect_garbage() {
  // Active callbacks can pump nested frames. Only the quiescent host context
  // may sweep; suspended cooperative stacks have explicit frame-root scopes.
  if (gc_call_depth_ || gc_running_) {
    gc_pending_ = true;
    return -1;
  }
  gc_running_ = true;
  struct Guard { bool& running; ~Guard() { running = false; } } guard{gc_running_};
  const bool requested = gc_pending_;
  gc_pending_ = false;
  const size_t before = heap_.size();
  std::unordered_set<const void*> managed;
  std::unordered_set<const void*> marked;
  std::vector<Object*> work;
  auto mark = [&](const void* address) {
    if (address && managed.count(address) && marked.insert(address).second)
      work.push_back(static_cast<Object*>(const_cast<void*>(address)));
  };
  auto mark_roots = [&] {
    managed.clear(); marked.clear(); work.clear();
    for (const auto& o : heap_) managed.insert(o.get());
    for (const auto& type : statics_)
      for (const auto& field : type.second)
        if (field.second.tag == ValueTag::Obj) mark(field.second.v.o);
    for (const auto& o : heap_) if (o->gc_disabled) mark(o.get());
    for (const auto& provider : gc_roots_) provider(mark);
    for (const auto& frame : gc_frames_) frame.second(mark);
  };
  auto trace = [&] {
    for (size_t cursor = 0; cursor < work.size(); ++cursor) {
      Object* o = work[cursor];
      for (const auto& field : o->fields)
        if (field.second.tag == ValueTag::Obj) mark(field.second.v.o);
      for (const Value& v : o->slots) if (v.tag == ValueTag::Obj) mark(v.v.o);
    }
  };
  mark_roots(); trace();
  std::vector<Object*> finalizers;
  std::vector<Object*> snapshot;
  snapshot.reserve(heap_.size());
  for (const auto& o : heap_) {
    snapshot.push_back(o.get());
  }
  for (Object* o : snapshot) {
    if (marked.count(o) || o->gc_finalized) continue;
    std::string owner;
    const auto* method = find_method_inherited(o->class_name, "finalize", "()V", false, &owner);
    const auto* cls = find_class(owner);
    std::string native_owner;
    const bool bound = native_has_alias(owner, "finalize", "()V", &native_owner);
    const bool script = method && cls && method->tree < cls->trees.size() &&
                        !cls->trees[method->tree].empty();
    // String's native payload is already owned by Object::str. Its C++
    // destructor performs the release; do not make a VM call per temporary
    // string or clear a finalizer's input strings before that finalizer runs.
    if ((bound && native_owner != "java.lang.String") || script)
      finalizers.push_back(o);
  }
  for (Object* o : finalizers) {
    o->gc_finalized = true;
    invoke(o->class_name, "finalize", "()V", {Value::make_obj(o)}, false);
  }
  // A finalizer may resurrect an object, change native roots or allocate.
  // Trace again and keep this cycle's new allocations alive until next time.
  if (!finalizers.empty() || heap_.size() != before) {
    mark_roots();
    for (size_t i = before; i < heap_.size(); ++i) mark(heap_[i].get());
    trace();
  }
  const size_t sweep_size = heap_.size();
  heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&](const auto& o) {
    return !marked.count(o.get());
  }), heap_.end());
  gc_last_freed_ = sweep_size - heap_.size();
  if (requested) {
    gc_report_ = static_cast<int>(std::min(gc_last_freed_, static_cast<size_t>(std::numeric_limits<int>::max())));
    gc_report_ready_ = true;
  }
  gc_allocated_ = 0;
  gc_threshold_ = std::max<size_t>(8192, heap_.size() / 2);
  std::printf("[gc] before=%zu alive=%zu freed=%zu finalized=%zu\n",
              before, heap_.size(), gc_last_freed_, finalizers.size());
  return static_cast<int>(std::min(gc_last_freed_, static_cast<size_t>(std::numeric_limits<int>::max())));
}
}
