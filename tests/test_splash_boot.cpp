#include "standalone_test.hpp"
#include "slrr/boot.hpp"
#include "slrr/gameref.hpp"
#include "slrr/native_registry.hpp"
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
int main(){const standalone::Test tests[]={boot_adopts_one_splash};return standalone::run("splash_boot",tests);}
