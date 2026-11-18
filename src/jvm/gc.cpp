#include "slrr/jvm.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/slrr_font.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>

namespace slrr {
Jvm::RootScope::RootScope(Jvm &vm, GcRootProvider provider)
    : vm_(vm), id_(++vm.gc_frame_id_) {
  vm_.gc_frames_.emplace(id_, std::move(provider));
}
Jvm::RootScope::~RootScope() { vm_.gc_frames_.erase(id_); }
void Jvm::exchange_execution_state(ExecutionState &state) {
  std::swap(state.calls, gc_call_depth_);
  std::swap(state.invokes, invoke_depth_);
  std::swap(state.constructors, field_init_depth_);
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
  if (gc_pending_ || gc_allocated_ >= gc_threshold_)
    collect_garbage();
}
bool Jvm::needs_finalizer(const std::string &name) {
  // Array descriptors are not classes and cannot declare a finalizer.
  if (name.empty() || name.front() == '[')
    return false;
  if (gc_native_count_ != natives_->method_count()) {
    gc_finalizer_cache_.clear();
    gc_native_count_ = natives_->method_count();
  }
  if (const auto it = gc_finalizer_cache_.find(name);
      it != gc_finalizer_cache_.end())
    return it->second;
  ++gc_finalizer_lookups_;
  std::string owner;
  const auto *method =
      find_method_inherited(name, "finalize", "()V", false, &owner);
  const auto *cls = owner.empty() ? nullptr : find_class(owner);
  std::string native_owner;
  const bool bound = !owner.empty() &&
                     native_has_alias(owner, "finalize", "()V", &native_owner);
  const bool script = method && cls && method->tree < cls->trees.size() &&
                      !cls->trees[method->tree].empty();
  // String owns its payload in C++; avoid clearing a finalizer's arguments.
  const bool needed = (bound && native_owner != "java.lang.String") || script;
  // Lookup can load classes, whose insertion clears this cache. Publish only
  // after that lookup; never retain an iterator across class loading.
  gc_finalizer_cache_[name] = needed;
  return needed;
}
int Jvm::collect_garbage() {
  // Active callbacks can pump nested frames. Only the quiescent host context
  // may sweep; suspended cooperative stacks have explicit frame-root scopes.
  if (gc_call_depth_ || gc_running_) {
    gc_pending_ = true;
    return -1;
  }
  gc_running_ = true;
  struct Guard {
    bool &running;
    ~Guard() { running = false; }
  } guard{gc_running_};
  const bool requested = gc_pending_;
  gc_pending_ = false;
  osd_compact_widgets();
  const size_t before = heap_.size();
  const size_t lookups_before = gc_finalizer_lookups_;
  std::vector<Object *> work;
  auto mark = [&](const void *address) {
    if (!address || !heap_members_.contains(address))
      return;
    auto *object = static_cast<Object *>(const_cast<void *>(address));
    if (object->gc_mark_epoch == gc_epoch_)
      return;
    object->gc_mark_epoch = gc_epoch_;
    work.push_back(object);
  };
  auto mark_roots = [&] {
    work.clear();
    if (++gc_epoch_ == 0) {
      for (const auto &o : heap_)
        o->gc_mark_epoch = 0;
      gc_epoch_ = 1;
    }
    for (const auto &type : statics_)
      for (const auto &field : type.second)
        if (field.second.tag == ValueTag::Obj)
          mark(field.second.v.o);
    for (const auto &o : heap_)
      if (o->gc_disabled)
        mark(o.get());
    for (const auto &provider : gc_roots_)
      provider(mark);
    for (const auto &frame : gc_frames_)
      frame.second(mark);
  };
  auto trace = [&] {
    for (size_t cursor = 0; cursor < work.size(); ++cursor) {
      Object *o = work[cursor];
      for (const auto &field : o->fields)
        if (field.second.tag == ValueTag::Obj)
          mark(field.second.v.o);
      for (const Value &v : o->slots)
        if (v.tag == ValueTag::Obj)
          mark(v.v.o);
    }
  };
  mark_roots();
  trace();
  std::vector<Object *> finalizers;
  // Finalizer lookup can allocate/load classes, but cannot sweep recursively.
  // Indexing a fixed prefix avoids copying every pointer into a snapshot.
  const size_t candidates = heap_.size();
  for (size_t i = 0; i < candidates; ++i) {
    Object *o = heap_[i].get();
    if (o->gc_mark_epoch == gc_epoch_ || o->gc_finalized)
      continue;
    if (needs_finalizer(o->class_name))
      finalizers.push_back(o);
  }
  for (Object *o : finalizers) {
    o->gc_finalized = true;
    invoke(o->class_name, "finalize", "()V", {Value::make_obj(o)}, false);
  }
  // A finalizer may resurrect an object, change native roots or allocate.
  // Trace again and keep this cycle's new allocations alive until next time.
  if (!finalizers.empty() || heap_.size() != before) {
    mark_roots();
    for (size_t i = before; i < heap_.size(); ++i)
      mark(heap_[i].get());
    trace();
  }
  const size_t sweep_size = heap_.size();
  heap_.erase(std::remove_if(heap_.begin(), heap_.end(),
                             [&](const auto &o) {
                               if (o->gc_mark_epoch == gc_epoch_)
                                 return false;
                               heap_members_.erase(o.get());
                               osd_forget(o.get());
                               slrr_font_forget(o.get());
                               host_input_forget(o.get());
                               return true;
                             }),
              heap_.end());
  gc_last_freed_ = sweep_size - heap_.size();
  if (requested) {
    gc_report_ = static_cast<int>(std::min(
        gc_last_freed_, static_cast<size_t>(std::numeric_limits<int>::max())));
    gc_report_ready_ = true;
  }
  gc_allocated_ = 0;
  gc_threshold_ = std::max<size_t>(8192, heap_.size() / 2);
  std::printf("[gc] before=%zu alive=%zu freed=%zu finalized=%zu "
              "finalizer_lookups=%zu\n",
              before, heap_.size(), gc_last_freed_, finalizers.size(),
              gc_finalizer_lookups_ - lookups_before);
  return static_cast<int>(std::min(
      gc_last_freed_, static_cast<size_t>(std::numeric_limits<int>::max())));
}
} // namespace slrr
