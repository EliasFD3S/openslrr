#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/host_game_options.hpp"
#include <thread>
using namespace slrr;
int main() {
 Fixture f;
 fs::create_directories(f.root/"probe/src");
 std::ofstream(f.root/"probe/src/String.java") << "package java.lang; public class String {}";
 std::ofstream(f.root/"probe/src/Config.java") << R"JAVA(package java.util;
 import java.lang.String;
 public class Config { public Config(int mode){} public native int saveConfig(String name); }
 )JAVA";
 auto report=script_compile_all(f.root.string(),"probe");
 SLRR_REQUIRE(report.failed==0 && report.succeeded>0);
 fs::path compiled;for(auto& e:fs::recursive_directory_iterator(f.root))if(e.path().filename()=="Config.class")compiled=e.path();
 SLRR_REQUIRE(f.vm.load_class_file(compiled.string()));
 int attempts=0;bool succeed=false;
 f.registry.register_method("java.util.Config","saveConfig","(Ljava.lang.String;)I",
   [&](Object*,const std::vector<Value>&){++attempts;return Value::make_int(succeed);});
 host_game_options_load(f.vm);
 host_game_options_dirty();
 host_game_options_update(f.vm);SLRR_REQUIRE(attempts==0);
 std::this_thread::sleep_for(std::chrono::milliseconds(550));
 host_game_options_update(f.vm);SLRR_REQUIRE(attempts==1);
 std::this_thread::sleep_for(std::chrono::milliseconds(550));
 for(int i=0;i<100;++i)host_game_options_update(f.vm);
 SLRR_REQUIRE(attempts==1);
 // A real setting change permits another debounced automatic attempt.
 f.vm.set_static("java.util.Config","gpsMode",Value::make_int(2));
 host_game_options_apply(f.vm);
 host_game_options_update(f.vm);SLRR_REQUIRE(attempts==1);
 std::this_thread::sleep_for(std::chrono::milliseconds(550));
 host_game_options_update(f.vm);SLRR_REQUIRE(attempts==2);
 host_game_options_update(f.vm,true);SLRR_REQUIRE(attempts==3);
 succeed=true;
 host_game_options_update(f.vm,true);SLRR_REQUIRE(attempts==4);
 host_game_options_update(f.vm,true);SLRR_REQUIRE(attempts==4);
 host_game_options_dirty();host_game_options_update(f.vm,true);SLRR_REQUIRE(attempts==5);
 host_game_options_update(f.vm,true);SLRR_REQUIRE(attempts==5);
 register_script_natives(f.vm,f.packs,f.sections);
 const auto host_file=f.root/"save/game/openslrr_options";
 auto set_graphics=[&](int key,int value){f.registry.invoke("java.game.OptionsDialog","setRendererOption","(II)V",nullptr,{Value::make_int(key),Value::make_int(value)});};
 for(int mode=0;mode<=3;++mode){
   set_graphics(0,mode);
   SLRR_REQUIRE(fs::exists(host_file));
   host_enhanced_graphics()={};
   host_game_options_load(f.vm);
   SLRR_REQUIRE(host_enhanced_graphics().msaa==mode);
 }
 for(int key=1;key<=4;++key)set_graphics(key,0);
 set_graphics(0,3);
 host_set_framerate_mode(FramerateMode::Classic);
 host_enhanced_graphics()={};
 host_game_options_load(f.vm);
 auto& g=host_enhanced_graphics();
 SLRR_REQUIRE(g.msaa==3 && !g.dynamic_reflections && !g.volumetric_clouds && !g.dynamic_lighting && !g.local_lights);
 SLRR_REQUIRE(host_framerate_mode()==FramerateMode::Classic);
 // Debug menu uses the same persistence entry point.
 g={};g.msaa=2;SLRR_REQUIRE(host_save_graphics_options());
 g={};host_game_options_load(f.vm);SLRR_REQUIRE(g.msaa==2 && g.dynamic_reflections);
 std::ofstream(host_file)<<"framerate_mode=classic\n";
 host_game_options_load(f.vm);SLRR_REQUIRE(g==EnhancedGraphics{});
 std::ofstream(host_file)<<"msaa=999\nlocal_lights=invalid\nvolumetric_clouds=0\n";
 host_game_options_load(f.vm);
 SLRR_REQUIRE(g.msaa==1 && g.local_lights && !g.volumetric_clouds);
 std::puts("PASS graphics disk round-trip: all MSAA modes, booleans, framerate coexistence, debug path, legacy and malformed files");
 std::puts("PASS debounce; failed saves suspend automatic retries; real changes rearm; forced retry and success clear dirty");
}
