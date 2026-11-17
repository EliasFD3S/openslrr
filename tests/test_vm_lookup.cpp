#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include <chrono>
using namespace slrr;
int main() {
  Fixture f;
  NativeRegistry r;
  auto returns = [](int n) { return [n](Object*, const std::vector<Value>&) { return Value::make_int(n); }; };
  r.register_method("A", "voidCall", "(I)V", returns(1));
  r.register_method("A", "bare", "()", returns(2));
  r.register_method("A", "value", "()I", returns(3));
  SLRR_REQUIRE(r.has("A", "voidCall", "(I)"));
  SLRR_REQUIRE(r.has("A", "bare", "()V"));
  SLRR_REQUIRE(r.invoke("A", "voidCall", "(I)", nullptr, {}).v.i == 1);
  SLRR_REQUIRE(r.invoke("A", "bare", "()V", nullptr, {}).v.i == 2);
  SLRR_REQUIRE(!r.has("A", "value", "()"));
  SLRR_REQUIRE(!r.has("A", "value", "()F"));
  SLRR_REQUIRE(!r.has("B", "value", "()I"));
  bool missing = false, duplicate = false;
  try { r.invoke("A", "value", "()V", nullptr, {}); } catch (const std::runtime_error&) { missing = true; }
  try { r.register_method("A", "bare", "()", returns(5)); } catch (const std::runtime_error&) { duplicate = true; }
  SLRR_REQUIRE(missing && duplicate);
  // Two exact registrations remain distinct, with exact lookup taking priority.
  r.register_method("A", "bare", "()V", returns(4));
  SLRR_REQUIRE(r.invoke("A", "bare", "()", nullptr, {}).v.i == 2);
  SLRR_REQUIRE(r.invoke("A", "bare", "()V", nullptr, {}).v.i == 4);
  SLRR_REQUIRE(!r.has("Late", "method", "()"));
  r.register_method("Late", "method", "()V", returns(6));
  SLRR_REQUIRE(r.has("Late", "method", "()"));
  for (int i = 0; i < 4000; ++i) r.register_method("Bulk" + std::to_string(i), "m", "()V", returns(i));
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100000; ++i) SLRR_REQUIRE(!r.has("JavaBody", "method", "()I"));
  std::printf("PASS 100000 missing native lookups with 4000 registrations: %lld us (informational)\n",
      (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
  fs::create_directories(f.root / "probe/src");
  // Pack existence preserves the Resource literal in the compiler output.
  std::ofstream(f.root / "frontend.rpk", std::ios::binary) << "RPAK";
  rpk_set_active(nullptr);
  auto source = f.root / "probe/src/Lookup.java";
  std::ofstream(source) << R"JAVA(package probe;
public class Lookup {
 static int command = 123;
 static int resource = frontend:0x0095r;
}
)JAVA";
  auto report = script_compile_all(f.root.string(), "probe");
  SLRR_REQUIRE(report.failed == 0 && report.succeeded > 0);
  fs::path compiled;
  for (auto& entry : fs::recursive_directory_iterator(f.root))
    if (entry.path().extension() == ".class") compiled = entry.path();
  SLRR_REQUIRE(!compiled.empty() && f.vm.load_class_file(compiled.string()));
  for (int i = 0; i < 10000; ++i) SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "command").v.i == 123);
  f.vm.set_static("probe.Lookup", "command", Value::make_int(321));
  SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "command").v.i == 321);
  for (int i = 0; i < 100; ++i)
    SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "resource").v.i == 149);
  f.packs.open_lib("frontend.rpk");
  rpk_set_active(&f.packs);
  const auto resolved = rpk_resolve_resource("frontend.rpk", 149);
  SLRR_REQUIRE(resolved > 0xffff);
  SLRR_REQUIRE(static_cast<uint32_t>(f.vm.get_static("probe.Lookup", "resource").v.i) == resolved);
  f.vm.set_static("probe.Lookup", "resource", Value::make_int(77));
  SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "resource").v.i == 77);
  // Loaded definitions stay immutable; a fresh VM observes recompiled metadata.
  std::ofstream(source) << "package probe; public class Lookup { static int resource = 149; static int command = 456; }";
  fs::remove(compiled);
  report = script_compile_all(f.root.string(), "probe");
  SLRR_REQUIRE(report.failed == 0 && report.succeeded > 0);
  SLRR_REQUIRE(f.vm.load_class_file(compiled.string()));
  SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "command").v.i == 321);
  SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "resource").v.i == 77);
  Jvm restarted(&f.registry);restarted.set_game_root(f.root.string());
  SLRR_REQUIRE(restarted.load_class_file(compiled.string()));
  SLRR_REQUIRE(restarted.get_static("probe.Lookup", "command").v.i == 456);
  SLRR_REQUIRE(restarted.get_static("probe.Lookup", "resource").v.i == 149);
  rpk_set_active(nullptr);
  SLRR_REQUIRE(f.vm.get_static("probe.Lookup", "missing").tag == ValueTag::Void);
  std::puts("PASS descriptor aliases, exact priority, late registration, mutable statics and class reload");
}
