#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/host_game_options.hpp"
#include <iterator>
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
 const auto host_file=f.root/"save/game/openslrr_options.json";
 const auto legacy_file=f.root/"save/game/openslrr_options";
 auto set_graphics=[&](int key,int value){f.registry.invoke("java.game.OptionsDialog","setRendererOption","(II)V",nullptr,{Value::make_int(key),Value::make_int(value)});};
 for(int mode=0;mode<=3;++mode){
   set_graphics(0,mode);
   SLRR_REQUIRE(fs::exists(host_file));
   host_enhanced_graphics()={};
   host_game_options_load(f.vm);
   SLRR_REQUIRE(host_enhanced_graphics().msaa==mode);
 }
 for(int value=0;value<=1;++value){
   set_graphics(6,value);host_enhanced_graphics()={};host_game_options_load(f.vm);
   SLRR_REQUIRE(host_enhanced_graphics().anisotropic_filtering==bool(value));
   SLRR_REQUIRE(f.registry.invoke("java.game.OptionsDialog","getRendererOption","(I)I",nullptr,{Value::make_int(6)}).v.i==value);
 }
 for(int key=7;key<=12;++key) {
   const int low=key==12?25:0,high=key==11?200:key==12?300:2;
   for(int value:{low,(low+high)/2,high}) {
     set_graphics(key,value);host_enhanced_graphics()={};host_game_options_load(f.vm);
     SLRR_REQUIRE(f.registry.invoke("java.game.OptionsDialog","getRendererOption","(I)I",nullptr,{Value::make_int(key)}).v.i==value);
   }
   set_graphics(key,9999);
   SLRR_REQUIRE(f.registry.invoke("java.game.OptionsDialog","getRendererOption","(I)I",nullptr,{Value::make_int(key)}).v.i==high);
   set_graphics(key,-99);
   SLRR_REQUIRE(f.registry.invoke("java.game.OptionsDialog","getRendererOption","(I)I",nullptr,{Value::make_int(key)}).v.i==low);
 }
 std::puts("PASS AO method/quality/strength/radius and shadow quality/filter clamps + disk roundtrip");
 for(int key=1;key<=4;++key)set_graphics(key,0);
 set_graphics(0,3);
 host_enhanced_graphics()={};
 host_game_options_load(f.vm);
 auto& g=host_enhanced_graphics();
 SLRR_REQUIRE(g.msaa==3 && !g.dynamic_reflections && !g.volumetric_clouds && !g.dynamic_lighting && !g.local_lights);
 // Debug menu uses the same persistence entry point.
 g={};g.msaa=2;SLRR_REQUIRE(host_save_graphics_options());
 g={};host_game_options_load(f.vm);SLRR_REQUIRE(g.msaa==2 && g.dynamic_reflections);
 std::puts("PASS graphics disk round-trip: all MSAA modes, booleans, debug path");
 // JSON (#405): sections, booleans as true/false. The removed framerate mode
 // is no longer written.
 {
   std::ifstream in(host_file);const std::string text{std::istreambuf_iterator<char>(in),{}};
   SLRR_REQUIRE(text.find("\"version\": 1")!=std::string::npos);
   SLRR_REQUIRE(text.find("framerate_mode")==std::string::npos);
   SLRR_REQUIRE(text.find("\"msaa\": 2")!=std::string::npos);
   SLRR_REQUIRE(text.find("\"dynamic_reflections\": true")!=std::string::npos);
   SLRR_REQUIRE(text.find("\"input\": {")!=std::string::npos);
 }
 // Missing keys keep their defaults; out-of-range values clamp; wrong types
 // and unknown keys are ignored.
 // An old framerate_mode key is ignored; option 5 is gone.
 std::ofstream(host_file)<<R"({"version":1,"video":{"framerate_mode":"classic"}})";
 host_game_options_load(f.vm);SLRR_REQUIRE(g==EnhancedGraphics{});
 {int removed=0;SLRR_REQUIRE(!host_option_get(5,removed));}
 std::ofstream(host_file)<<R"({"version":1,"video":{"msaa":999,"local_lights":"x","volumetric_clouds":false,"fog":0,"anisotropic_filtering":[1]},"input":{"ffb_damper":150,"ffb_min_force":-4,"ffb_invert":true},"extra":{"a":{"b":1}}})";
 host_game_options_load(f.vm);
 SLRR_REQUIRE(g.msaa==3 && g.local_lights && !g.volumetric_clouds && !g.fog && g.anisotropic_filtering);
 SLRR_REQUIRE(host_input_tuning().ffb_damper==150 && host_input_tuning().ffb_min_force==0 && host_input_tuning().ffb_invert);
 // A damaged file: defaults, kept aside as .bad, replaced by the next save.
 std::ofstream(host_file)<<R"({"video":{"msaa":0,)";
 host_game_options_load(f.vm);
 SLRR_REQUIRE(g==EnhancedGraphics{} && fs::exists(host_file.string()+".bad") && !fs::exists(host_file));
 set_graphics(0,0);SLRR_REQUIRE(fs::exists(host_file));
 g={};host_game_options_load(f.vm);SLRR_REQUIRE(g.msaa==0);
 // The key=value file of older builds is taken over once, then kept.
 fs::remove(host_file);
 std::ofstream(legacy_file)<<"framerate_mode=classic\nmsaa=2\nlocal_lights=invalid\nvolumetric_clouds=0\nffb_detail=40\nffb_invert=1\n";
 host_game_options_load(f.vm);
 SLRR_REQUIRE(g.msaa==2 && g.local_lights && !g.volumetric_clouds);
 SLRR_REQUIRE(host_input_tuning().ffb_detail==40 && host_input_tuning().ffb_invert);
 SLRR_REQUIRE(fs::exists(host_file) && fs::exists(legacy_file));
 std::ofstream(legacy_file)<<"msaa=0\n";
 host_game_options_load(f.vm);SLRR_REQUIRE(g.msaa==2);  // the JSON wins
 // Every renderer option id reads back what was set, through the file.
 for(int key=0;key<=34;++key){
   if(key==5)continue;  // the removed framerate mode
   int before=0;SLRR_REQUIRE(host_option_get(key,before));
   int target=before==0?1:0;
   set_graphics(key,target);
   host_game_options_load(f.vm);
   int after=-1;SLRR_REQUIRE(host_option_get(key,after));
   // AO radius (12) and view distance (14) clamp 0/1 up to their minimum.
   SLRR_REQUIRE(after==target || ((key==12 || key==14) && after>1));
 }
 int unknown=0;SLRR_REQUIRE(!host_option_get(35,unknown));
 // The camera style (#470) is saved by name and read back from it; an
 // unknown name keeps the classic default.
 SLRR_REQUIRE(host_option_set(34,1));
 {
   std::ifstream in(host_file);const std::string text{std::istreambuf_iterator<char>(in),{}};
   SLRR_REQUIRE(text.find("\"style\": \"arcade\"")!=std::string::npos);
 }
 std::ofstream(host_file)<<R"({"version":1,"camera":{"style":"arcade"}})";
 host_game_options_load(f.vm);SLRR_REQUIRE(host_camera_tuning().style==1);
 std::ofstream(host_file)<<R"({"version":1,"camera":{"style":"nfs"}})";
 host_game_options_load(f.vm);SLRR_REQUIRE(host_camera_tuning().style==0);
 std::puts("PASS options JSON: sections and types, defaults for missing keys, clamps, damaged file, legacy migration, every id");
 if(const char* root=std::getenv("SLRR_COMPILE_OPTIONS_ROOT")) {
   const auto report=script_compile_all(root,"options-build");
   SLRR_REQUIRE(report.attempted==1 && report.succeeded==1 && report.failed==0);
   std::puts("PASS patched OptionsDialog source compilation");
 }
 std::puts("PASS debounce; failed saves suspend automatic retries; real changes rearm; forced retry and success clear dirty");
}
