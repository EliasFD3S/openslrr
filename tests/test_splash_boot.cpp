#include "save_io_fixture.hpp"
#include "slrr/boot.hpp"
#include "slrr/gameref.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/section_machine.hpp"
using namespace slrr;
void boot_adopts_one_splash() {
 NativeRegistry natives; Jvm vm(&natives);
 BootOptions options; options.game_root="game"; options.render=RenderBackendKind::Null;
 auto render=create_render_backend(options.render);
 SLRR_REQUIRE(boot_game(vm,*render,options)==0);
 const Value actual=vm.get_static("java.game.GameLogic","actualState");
 SLRR_REQUIRE(actual.tag==ValueTag::Obj && actual.v.o);
 auto* splash=actual.v.o;
 SLRR_REQUIRE(splash->class_name=="java.game.SplashScreen");
 const auto osd=splash->fields.at("osd");
 SLRR_REQUIRE(osd.tag==ValueTag::Obj && osd.v.o);
 auto* node=gameref_get(splash); SLRR_REQUIRE(node);
 std::printf("Splash pending timers=%zu\n",node->timers.size());
 SLRR_REQUIRE(node->timers.size()==1 && node->timers[0].timer_id==1);
 // Repeated host synchronization must preserve the existing presentation/timer.
 SectionMachine sections(&vm); sections.adopt_actual(splash); sections.adopt_actual(splash);
 SLRR_REQUIRE(sections.actual()==splash && splash->fields.at("osd").v.o==osd.v.o);
 SLRR_REQUIRE(node->timers.size()==1);
 std::puts("PASS real Init boot creates one splash timer; repeated adoption preserves its OSD");
}
void failed_init_skips_mods() {
 Fixture f;
 const auto write = [&](const fs::path& relative, const char* text) {
   const auto path = f.root / relative;
   fs::create_directories(path.parent_path());
   std::ofstream out(path);
   SLRR_REQUIRE(out << text);
 };
 write("sl/Scripts/game/src/Init.java", R"JAVA(package java.game;
public class Init {
  public static native void requiredForBoot();
  public Init(int mode) { requiredForBoot(); }
}
)JAVA");
 write("mods/probe/mod.json", R"JSON({"name":"Boot probe","version":"1","api":1,"entry":"java.mods.probe.Mod"})JSON");
 write("mods/probe/system/Scripts/mods/probe/src/Mod.java", R"JAVA(package java.mods.probe;
public class Mod {
  public static native void entered();
  public static void modInit() { entered(); }
}
)JAVA");
 const auto stock = script_compile_all(f.root.string(), "sl/Scripts/game");
 const auto mod = script_compile_all(f.root.string(), "mods/probe");
 SLRR_REQUIRE(stock.succeeded == 1 && stock.failed == 0);
 SLRR_REQUIRE(mod.succeeded == 1 && mod.failed == 0);
 SLRR_REQUIRE(f.vm.bootstrap_classpath());

 int entries = 0;
 f.registry.register_method("java.mods.probe.Mod", "entered", "()V",
     [&](Object*, const std::vector<Value>&) { ++entries; return Value::make_void(); });
 SLRR_REQUIRE(!boot_stock_init_and_mods(f.vm));
 SLRR_REQUIRE(f.vm.unbound_native().find("requiredForBoot") != std::string::npos);
 SLRR_REQUIRE(entries == 0);

 // Prove the same manifest and entry are runnable if stock Init succeeds.
 f.registry.register_method("java.game.Init", "requiredForBoot", "()V",
     [](Object*, const std::vector<Value>&) { return Value::make_void(); });
 SLRR_REQUIRE(boot_stock_init_and_mods(f.vm));
 SLRR_REQUIRE(entries == 1);
 std::puts("PASS unbound native in stock Init skips modInit; successful Init calls it");
}
int main(){const standalone::Test tests[]={failed_init_skips_mods,boot_adopts_one_splash};return standalone::run("splash_boot",tests);}
