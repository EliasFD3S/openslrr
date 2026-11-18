#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/script_threads.hpp"
#include <chrono>
using namespace slrr;
struct GcFixture {
  fs::path base = fs::absolute("build/gc-runtime-fixtures"), root;
  NativeRegistry registry;
  Jvm vm{&registry};
  RpkIndex packs;
  SectionMachine sections{&vm};
  GcFixture() {
    root = base /
           ("test-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    packs.set_root(root.string());
    register_io_natives(vm, packs, sections);
    vm.set_game_root(fs::absolute("game").string());
    SLRR_REQUIRE(vm.bootstrap_classpath());
    vm.new_object("java.lang.Object");
    vm.new_string("warm");
    vm.collect_garbage();
  }
  ~GcFixture() {
    script_threads_shutdown(vm);
    if (root.parent_path() == base &&
        root.filename().string().starts_with("test-")) {
      std::error_code error;
      fs::remove_all(root, error);
    }
  }
  void compile(const char *name, const std::string &code) {
    fs::create_directories(root / "probe/src");
    std::ofstream(root / "probe/src" / (std::string(name) + ".java")) << code;
    const auto report = script_compile_all(root.string(), "probe");
    SLRR_REQUIRE(report.failed == 0);
    SLRR_REQUIRE(vm.load_class_file(
        (root / "probe" / (std::string(name) + ".class")).string()));
  }
};
static void roots_cycles_and_foreign_pointers() {
  GcFixture f;
  const auto baseline = f.vm.heap_size();
  Object foreign;
  Object *live = nullptr;
  f.vm.add_gc_roots([&](const GcRootVisitor &visit) {
    visit(live);
    visit(&foreign);
    visit(reinterpret_cast<void *>(uintptr_t(1)));
  });
  auto *a = f.vm.new_object("java.lang.Object");
  auto *b = f.vm.new_object("java.lang.Object");
  a->fields["peer"] = Value::make_obj(b);
  b->slots.push_back(Value::make_obj(a));
  b->is_array = true;
  live = a;
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline + 2);
  a->gc_disabled = true;
  live = nullptr;
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline + 2);
  a->gc_disabled = false;
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline);
  auto *orphan = f.vm.new_object("java.lang.Object");
  foreign.fields["child"] = Value::make_obj(orphan);
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline);
  live = orphan; // stale native address must not be dereferenced after sweep.
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline);
  live = nullptr;
  auto *scoped = f.vm.new_object("java.lang.Object");
  {
    Jvm::RootScope roots(f.vm,
                         [&](const GcRootVisitor &visit) { visit(scoped); });
    f.vm.collect_garbage();
    SLRR_REQUIRE(f.vm.heap_size() == baseline + 1);
  }
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline);
}
static void finalizers_and_resurrection() {
  GcFixture f;
  f.compile("FinalBase", R"JAVA(package gcprobe;
    public class FinalBase { public native void finalize(); }
    class Child extends FinalBase { }
    class Scripted { static int calls; public void finalize(){calls=calls+1;} }
  )JAVA");
  const auto baseline = f.vm.heap_size();
  int count = 0;
  bool deferred = false;
  f.registry.register_method(
      "gcprobe.FinalBase", "finalize", "()V",
      [&](Object *self, const std::vector<Value> &) {
        ++count;
        SLRR_REQUIRE(self->fields.at("payload").v.o->str == "input intact");
        f.vm.set_static("java.lang.Object", "gc_live", Value::make_obj(self));
        self->fields["child"] =
            Value::make_obj(f.vm.new_string("resurrected child"));
        f.vm.new_object("java.lang.Object");
        deferred = f.vm.collect_garbage() == -1;
        return Value::make_void();
      });
  auto *child = f.vm.new_object("gcprobe.Child");
  child->fields["payload"] = Value::make_obj(f.vm.new_string("input intact"));
  f.vm.new_object("gcprobe.Scripted");
  f.vm.collect_garbage();
  SLRR_REQUIRE(count == 1 && deferred && child->gc_finalized);
  SLRR_REQUIRE(f.vm.get_static("gcprobe.Scripted", "calls").v.i == 1);
  SLRR_REQUIRE(f.vm.heap_size() == baseline + 4);
  // Newly allocated, unrooted objects survive only this collection.
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline + 3 && count == 1);
  f.vm.set_static("java.lang.Object", "gc_live", Value::make_obj(nullptr));
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline && count == 1);
}
static void cache_and_late_bindings() {
  GcFixture f;
  f.compile("LateNative", "package gcprobe; public class LateNative {public "
                          "native void finalize();}");
  const auto baseline = f.vm.heap_size();
  auto before = f.vm.gc_finalizer_lookups();
  for (int i = 0; i < 200; ++i)
    f.vm.new_object("gcprobe.LateNative");
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.gc_finalizer_lookups() == before + 1);
  before = f.vm.gc_finalizer_lookups();
  for (int i = 0; i < 200; ++i)
    f.vm.new_object("gcprobe.LateNative");
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.gc_finalizer_lookups() == before);
  int calls = 0;
  f.registry.register_method("gcprobe.LateNative", "finalize", "()V",
                             [&](Object *, const std::vector<Value> &) {
                               ++calls;
                               return Value::make_void();
                             });
  for (int i = 0; i < 200; ++i)
    f.vm.new_object("gcprobe.LateNative");
  f.vm.collect_garbage();
  SLRR_REQUIRE(calls == 200 && f.vm.gc_finalizer_lookups() == before + 1);
  f.registry.register_method("gcprobe.LateClass", "finalize", "()V",
                             [&](Object *, const std::vector<Value> &) {
                               ++calls;
                               return Value::make_void();
                             });
  auto *unknown = f.vm.new_object("java.lang.Object");
  unknown->class_name = "gcprobe.LateClass";
  f.vm.collect_garbage();
  SLRR_REQUIRE(calls == 200);
  f.compile("LateClass", "package gcprobe; public class LateClass {public "
                         "native void finalize();}");
  f.vm.new_object("gcprobe.LateClass");
  f.vm.collect_garbage();
  SLRR_REQUIRE(calls == 201 && f.vm.heap_size() == baseline);
}
static void suspended_stack_roots() {
  GcFixture f;
  f.compile("Worker", R"JAVA(package gcprobe; import java.lang.*;
    public class Worker {
      public native void hold(Payload value);
      public void run(){Payload local=new Payload();hold(local);}
    }
    class Payload {}
  )JAVA");
  const auto baseline = f.vm.heap_size();
  int stage = 0;
  f.vm.add_gc_roots(
      [&](const GcRootVisitor &visit) { script_threads_roots(f.vm, visit); });
  f.registry.register_method(
      "gcprobe.Worker", "hold", "(Lgcprobe.Payload;)V",
      [&](Object *, const std::vector<Value> &args) {
        SLRR_REQUIRE(args.size() == 1 && args[0].tag == ValueTag::Obj);
        args[0].v.o->fields["proof"] = Value::make_int(42);
        stage = 1;
        script_thread_sleep(f.vm, 0);
        SLRR_REQUIRE(args[0].v.o->fields.at("proof").v.i == 42);
        stage = 2;
        return Value::make_void();
      });
  auto *worker = f.vm.new_object("gcprobe.Worker");
  script_thread_start(f.vm, worker);
  for (int i = 0; i < 10 && stage == 0; ++i)
    script_threads_tick(f.vm);
  SLRR_REQUIRE(stage == 1 && script_thread_alive(f.vm, worker));
  SLRR_REQUIRE(f.vm.collect_garbage() >= 0 && f.vm.heap_size() == baseline + 3);
  for (int i = 0; i < 10 && script_thread_alive(f.vm, worker); ++i)
    script_threads_tick(f.vm);
  SLRR_REQUIRE(stage == 2 && !script_thread_alive(f.vm, worker));
  f.vm.collect_garbage();
  SLRR_REQUIRE(f.vm.heap_size() == baseline);
  script_threads_shutdown(f.vm);
}
static void benchmark() {
  GcFixture f;
  const auto baseline = f.vm.heap_size();
  for (int round = 0; round < 2; ++round) {
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 60000; ++i) {
      switch (i % 3) {
      case 0:
        f.vm.new_object("java.lang.Object");
        break;
      case 1:
        f.vm.new_string("temporary");
        break;
      default:
        f.vm.new_object("[I")->is_array = true;
        break;
      }
    }
    const auto allocated = std::chrono::steady_clock::now();
    f.vm.collect_garbage();
    const auto end = std::chrono::steady_clock::now();
    std::printf(
        "GC BENCH 60000 mixed objects round=%d alloc=%.3f collect=%.3f ms\n",
        round,
        std::chrono::duration<double, std::milli>(allocated - start).count(),
        std::chrono::duration<double, std::milli>(end - allocated).count());
    SLRR_REQUIRE(f.vm.heap_size() == baseline);
  }
}
int main() {
  const standalone::Test tests[] = {
      roots_cycles_and_foreign_pointers, finalizers_and_resurrection,
      cache_and_late_bindings, suspended_stack_roots, benchmark};
  return standalone::run("gc_runtime", tests);
}
