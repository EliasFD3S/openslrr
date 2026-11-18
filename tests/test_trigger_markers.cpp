#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_triggers.hpp"
#include "slrr/script_events.hpp"
#include "slrr/script_compiler.hpp"
#include <thread>
#include <chrono>
#include <cmath>
using namespace slrr;
void check_trigger_events() {
  Fixture f;
  auto& root=f.root; auto& packs=f.packs; auto& vm=f.vm;
  register_part_natives(vm,packs);
  Object world; gameref_upsert(&world)->flags=kWorldTreeRoot;
  auto put=[](std::vector<uint8_t>& b,size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));};
  auto mesh=[&](const char* name,bool flat){
   std::vector<uint8_t> b(8+56+4+3*44+4+12,0);b[0]='I';b[1]='N';b[2]='V';b[3]='O';put(b,4,3);put(b,8,56);put(b,8+44,0xffff);
   put(b,8+56,3);float positions[9]={-100,-100,-200,100,flat?-100.f:100.f,-200,100,flat?-100.f:100.f,200};
   for(int v=0;v<3;++v)std::memcpy(b.data()+68+v*44,positions+v*3,12);
   size_t face=68+3*44;put(b,face,1);put(b,face+4,0);put(b,face+8,1);put(b,face+12,2);
   std::ofstream out(root/name,std::ios::binary);out.write((char*)b.data(),b.size());
  };mesh("good.scx",false);mesh("bad.scx",true);
  {std::ofstream cfg(root/"good.cfg");cfg<<"body 0 0 0 0 0 0 1000 box 1 1 2\nrender 0x0007\n";}
  {std::ofstream cfg(root/"bad.cfg");cfg<<"body 0 0 0 0 0 0 1000 box 1 1 2\nrender 0x0009\n";}
  const std::array<std::string,4> data={"native car good.cfg\r\n","sourcefile good.scx\r\n","native car bad.cfg\r\n","sourcefile bad.scx\r\n"};
  std::vector<uint8_t> bytes(32+4*23,0);bytes[0]='R';bytes[1]='P';bytes[2]='A';bytes[3]='K';put(bytes,4,0x200);put(bytes,16,4*23);put(bytes,20,4);
  for(size_t i=0;i<data.size();++i){size_t p=32+i*23;put(bytes,p+4,6+uint32_t(i));bytes[p+8]=i%2?14:8;put(bytes,p+14,uint32_t(bytes.size()));put(bytes,p+18,uint32_t(data[i].size()));bytes.insert(bytes.end(),data[i].begin(),data[i].end());}
  {std::ofstream out(root/"cars.rpk",std::ios::binary);out.write((char*)bytes.data(),bytes.size());}
  auto pack=packs.open_lib("cars.rpk");scene_instances_set_resources(&packs);scene_instances_select(&world,nullptr,false);

  Object* car=part_create(vm,packs,&world,RpkIndex::make_full_id(pack,6),"car");
  SLRR_REQUIRE(car);
  fs::create_directories(root/"probe/src");
  std::ofstream(root/"probe/src/GameRef.java") << "package java.util.resource; public class GameRef {}";
  std::ofstream(root/"probe/src/String.java") << "package java.lang; public class String {}";
  std::ofstream(root/"probe/src/Listener.java") << "package probe; import java.util.resource.*; import java.lang.*; public class Listener { public native void entered(GameRef src, int event, String param); }";
  const auto report=script_compile_all(root.string(),"probe");
  SLRR_REQUIRE(report.failed==0);
  for(const auto& entry:fs::recursive_directory_iterator(root/"probe"))
    if(entry.path().extension()==".class") SLRR_REQUIRE(vm.load_class_file(entry.path().string()));
  int enters=0, leaves=0; std::string car_id;
  vm.natives().register_method("probe.Listener","entered","(Ljava.util.resource.GameRef;ILjava.lang.String;)V",
    [&](Object*,const std::vector<Value>& args) {
      if(args[1].v.i==0x20) ++enters; else if(args[1].v.i==0x40) ++leaves;
      car_id=args[2].v.o->str; return Value::make_void();
    });
  Object* listener=vm.new_object("probe.Listener");
  gameref_set_event_mask(listener,0x60);
  Object sensor; gameref_upsert(&sensor)->type_id=0x34;
  gameref_set_parent(&sensor,&world); gameref_bind_trigger(&sensor,"0,0,0,0,0,0,sphere,2");
  script_notification_add(vm,listener,&sensor,0x20,0,"","entered");
  script_notification_add(vm,listener,&sensor,0x40,0,"","entered");
  scene_triggers_reset();
  gameref_set_matrix(car,4,0,0,0,0,0); scene_triggers_tick(vm);
  gameref_set_matrix(car,0,0,0,0,0,0); scene_triggers_tick(vm); scene_triggers_tick(vm);
  gameref_set_matrix(car,4,0,0,0,0,0); scene_triggers_tick(vm);
  SLRR_REQUIRE(enters==1 && leaves==1);
  SLRR_REQUIRE(car_id==std::to_string(gameref_get(car)->live_id));
  std::puts("PASS native car enters/leaves sensor once, callback receives chassis live ID");
  script_events_shutdown(vm);scene_triggers_reset();part_destroy(car);
  gameref_remove(&sensor);gameref_remove(listener);gameref_remove(&world);
  scene_instances_select(nullptr,nullptr,false);scene_instances_set_resources(nullptr);
}

int run_markers() {
  Fixture f;
  f.packs.set_root(fs::absolute("game").string()); f.packs.open_all();
  scene_instances_set_resources(&f.packs);
  Object world;
  gameref_upsert(&world)->flags = kWorldTreeRoot;
  scene_instances_select(&world, nullptr, false);
  scene_instances_enable_static_floor(false);
  int failures = 0;
  auto check = [&](bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures;
  };
  for (const auto& asset : std::vector<std::pair<const char*, unsigned>>{
       {"frontend.rpk", 0x70}, {"DestructionTools.rpk", 0x24}, {"DestructionTools.rpk", 0x26}}) {
    const auto* pack = f.packs.pack_by_name(asset.first);
    if (!pack) { check(false, asset.first); continue; }
    Object marker;
    auto* n = gameref_upsert(&marker, GameRefKind::RenderRef);
    n->type_id = RpkIndex::make_full_id(pack->pack_id, asset.second);
    gameref_set_parent(&marker, &world);
    gameref_set_matrix(&marker, 0, 3, 0, 0, 0, 0);
    auto before = scene_instances_collect_root(&world);
    check(before.size() == 1, "marker remains visible");
    scene_instances_prepare_physics();
    float floor = 0;
    bool obstacle = false;
    // Probe the complete cue footprint, not only the arrow's empty corners.
    for (float x = -5; x <= 5; x += .25f)
      for (float z = -5; z <= 5; z += .25f)
        obstacle |= scene_instances_floor(x, z, 30, nullptr, &floor);
    check(!obstacle, "visual marker has no collision");
    if (asset.second == 0x70 && !before.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      auto after = scene_instances_collect_root(&world);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      auto later = scene_instances_collect_root(&world);
      // Two bob samples avoid an equal-height pair straddling the sine peak.
      // Rotation uses both basis components, including near a cosine extremum.
      check(after.size() == 1 && later.size() == 1 &&
            std::max(std::fabs(after[0].transform[13] - before[0].transform[13]),
                     std::fabs(later[0].transform[13] - before[0].transform[13])) > .001f &&
            std::hypot(after[0].transform[0] - before[0].transform[0],
                       after[0].transform[2] - before[0].transform[2]) > .001f,
            "cue rotates and bobs");
      check(before[0].opacity > 0 && before[0].opacity < 1, "cue alpha enabled");
      check(gameref_get(&marker)->pos[1] == 3, "animation leaves script pose fixed");
      n->vehicle_material=true;auto vehicle=scene_instances_collect_root(&world);check(vehicle.size()==1&&vehicle[0].transform[13]==3&&vehicle[0].transform[0]==1&&vehicle[0].opacity==1,"resource cue flags never rotate or float a vehicle mesh");

    }
    gameref_remove(&marker);
  }
  Object body;
  auto* b = gameref_upsert(&body, GameRefKind::PhysicsRef);
  b->physics_shape = PhysicsShape::Box;
  b->physics_size[0] = b->physics_size[1] = b->physics_size[2] = 1;
  gameref_set_parent(&body, &world); gameref_set_matrix(&body, 0, 3, 0, 0, 0, 0);
  scene_instances_prepare_physics();
  SceneSphereHit hit;
  const float start[3] = {-5,3,0}, delta[3] = {10,0,0};
  check(scene_instances_sweep_sphere(start, delta, .2f, hit), "physical body remains solid");
  b->trigger_shape = TriggerShape::Sphere; b->trigger_size[0] = 1;
  scene_instances_prepare_physics();
  check(!scene_instances_sweep_sphere(start, delta, .2f, hit), "sensor never becomes a solid body");
  gameref_remove(&body); gameref_remove(&world);
  scene_instances_select(nullptr, nullptr, false); scene_instances_set_resources(nullptr);
  check_trigger_events();
  return failures ? 1 : 0;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  try { return run_markers(); }
  catch (const std::exception& e) { std::fprintf(stderr, "trigger_markers: %s\n", e.what()); return 1; }
}
