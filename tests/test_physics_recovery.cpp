#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include <limits>
using namespace slrr;
const float nan_value=std::numeric_limits<float>::quiet_NaN();

struct WorldFixture:Fixture {
 Object world;
 WorldFixture(){gameref_clear();register_part_natives(vm,packs);auto* root=gameref_upsert(&world,GameRefKind::Dummy);root->empty=false;root->flags=kWorldTreeRoot;scene_instances_select(&world,nullptr,false);}
 ~WorldFixture(){scene_instances_select(nullptr,nullptr,false);gameref_clear();}
 void body(Object& object){physicsref_create_box(&object,&world,1,1,1,"fixture");gameref_set_matrix(&object,0,10,0,0,0,0);gameref_set_physics_simulated(&object,true);}
};
void matrix_rejects_nonfinite(){
 WorldFixture f;Object ref;auto* state=gameref_upsert(&ref);state->empty=false;
 gameref_set_matrix(&ref,1,2,3,.1f,.2f,.3f);
 for(int index=0;index<6;++index){float args[6]={4,5,6,.4f,.5f,.6f};args[index]=nan_value;gameref_set_matrix(&ref,args[0],args[1],args[2],args[3],args[4],args[5]);SLRR_REQUIRE(state->pos[0]==1 && state->pos[1]==2 && state->ypr[0]==.1f);}
 std::puts("PASS non-finite GameRef pose rejected atomically");
}
void malformed_free_body_does_not_stop_other_bodies(){
 WorldFixture f;Object bad,good;f.body(bad);f.body(good);
 gameref_get(&bad)->physics_size[0]=-1; // Bypass script validation: malformed engine data.
 part_physics_advance(1./60.);
 SLRR_REQUIRE(!gameref_get(&bad)->physics_simulated);
 SLRR_REQUIRE(gameref_get(&good)->pos[1]<10);
 auto y=gameref_get(&good)->pos[1];part_physics_advance(1./60.);
 SLRR_REQUIRE(gameref_get(&good)->pos[1]<y);
 std::puts("PASS malformed body disabled locally; healthy body keeps advancing");
}
void invalid_free_frame_recovers_last_pose(){
 WorldFixture f;Object bad,good;f.body(bad);f.body(good);
 part_physics_advance(1./60.);auto* state=gameref_get(&bad);float y=state->pos[1];
 std::fill(state->local_matrix,state->local_matrix+16,0.f);state->local_matrix[0]=nan_value;state->matrix_override=true;
 state->pos[0]=1; // Force the next pose update, while the matrix is corrupt.
 part_physics_advance(1./60.);
 SLRR_REQUIRE(!state->physics_simulated && !state->matrix_override);
 SLRR_REQUIRE(std::isfinite(state->pos[1]) && state->pos[1]==y);
 SLRR_REQUIRE(gameref_get(&good)->pos[1]<y);
 std::puts("PASS invalid free-body frame stays at last finite published pose");
}
struct CarFixture:WorldFixture {
 Object *good=nullptr,*bad=nullptr,*other=nullptr;
 CarFixture(){
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
  good=part_create(vm,packs,&world,RpkIndex::make_full_id(pack,6),"good");
  bad=part_create(vm,packs,&world,RpkIndex::make_full_id(pack,8),"bad");
  SLRR_REQUIRE(good && bad);gameref_set_matrix(good,0,10,0,0,0,0);gameref_set_matrix(bad,10,10,0,0,0,0);part_command(good,"start");part_command(bad,"start");
 }
 ~CarFixture(){if(other)part_destroy(other);part_destroy(good);part_destroy(bad);scene_instances_set_resources(nullptr);}
};
void degenerate_chassis_is_suspended(){
 CarFixture f;part_physics_advance(1./60.);PartPhysicsState good,bad;
 SLRR_REQUIRE(part_physics_state(f.good,good) && good.active && good.ticks>0);
 SLRR_REQUIRE(part_physics_state(f.bad,bad) && !bad.active);
 auto ticks=good.ticks;part_physics_advance(1./60.);part_physics_state(f.good,good);SLRR_REQUIRE(good.ticks>ticks);
 std::puts("PASS degenerate chassis bounds suspended locally while a healthy chassis keeps ticking");
}
void corrupt_chassis_restores_pose(){
 CarFixture f;part_destroy(f.bad);f.bad=nullptr;part_physics_advance(1./60.);auto* node=gameref_get(f.good);
 const float y=node->local_matrix[13];node->local_matrix[0]=nan_value;node->ypr[0]=nan_value;
 part_physics_advance(1./60.);PartPhysicsState state;part_physics_state(f.good,state);
 SLRR_REQUIRE(!state.active && std::isfinite(node->local_matrix[0]) && std::isfinite(node->ypr[0]) && node->local_matrix[13]==y);
 part_physics_advance(1./60.);SLRR_REQUIRE(node->local_matrix[13]==y);
 std::puts("PASS corrupted chassis pose restored and suspended without repeated simulation");
}
void wheel_setters_reject_nonfinite_atomically(){
 CarFixture f;Object wheel,angles;wheel.fields["chassis"]=Value::make_obj(f.good);wheel.fields["wheel_id"]=Value::make_int(0);
 auto call=[&](const char* name,const char* signature,std::vector<Value> args={}){return f.registry.invoke("java.game.parts.WheelRef",name,signature,&wheel,args);};
 call("setRadius","(F)V",{Value::make_float(.4f)});call("setRadius","(F)V",{Value::make_float(nan_value)});
 SLRR_REQUIRE(call("getRadius","()F").v.f==.4f);
 angles.fields["y"]=Value::make_float(.1f);angles.fields["p"]=Value::make_float(.2f);angles.fields["r"]=Value::make_float(.3f);
 call("setYpr","(Ljava.lang.Ypr;)V",{Value::make_obj(&angles)});
 angles.fields["y"]=Value::make_float(9);angles.fields["p"]=Value::make_float(nan_value);
 call("setYpr","(Ljava.lang.Ypr;)V",{Value::make_obj(&angles)});
 auto* read=call("getYpr","()Ljava.lang.Ypr;").v.o;
 SLRR_REQUIRE(read && read->fields["y"].v.f==.1f && read->fields["p"].v.f==.2f);
 std::puts("PASS wheel scalar and vector setters reject invalid updates atomically");
}
void numerical_failure_keeps_native_pump_alive(){
 CarFixture f;part_destroy(f.bad);f.bad=nullptr;
 const auto type=part_installed_tree(f.good).front().resource;
 f.other=part_create(f.vm,f.packs,&f.world,type,"other");SLRR_REQUIRE(f.other);
 gameref_set_matrix(f.other,20,10,0,0,0,0);part_command(f.other,"start");
 part_physics_advance(1./60.);
 const float velocity[3]={std::numeric_limits<float>::max(),0,0},angular[3]={0,0,0};
 part_apply_state(f.good,velocity,angular);
 f.registry.register_method("probe.Pump","tick","()I",[](Object*,const std::vector<Value>&){
  part_physics_advance(1./60.);return Value::make_int(7); // Modal/native cleanup after pumping must remain reachable.
 });
 bool failed=false;
 for(int i=0;i<180;++i){
  SLRR_REQUIRE(f.registry.invoke("probe.Pump","tick","()I",nullptr,{}).v.i==7);
  auto* node=gameref_get(f.good);
  for(float value:node->local_matrix)SLRR_REQUIRE(std::isfinite(value));
  PartPhysicsState state;part_physics_state(f.good,state);
  if(!state.active){failed=true;break;}
 }
 SLRR_REQUIRE(failed);
 PartPhysicsState other;SLRR_REQUIRE(part_physics_state(f.other,other) && other.active && other.ticks>0);
 auto ticks=other.ticks;part_physics_advance(1./60.);part_physics_state(f.other,other);SLRR_REQUIRE(other.ticks>ticks);
 gameref_set_matrix(f.good,0,10,0,0,0,0);part_command(f.good,"start");
 part_physics_advance(1./60.);PartPhysicsState recovered;
 SLRR_REQUIRE(part_physics_state(f.good,recovered) && recovered.active && std::isfinite(recovered.height));
 std::puts("PASS numerical overflow contained; native pump and healthy chassis continue; valid placement/start retries successfully");
}
void malformed_dynamic_native_creation_is_transactional(){
 CarFixture f;PartConfig config;config.resource=0x12345678;config.name="invalid native test";
 PartBody body;body.shape=PartBodyShape::Box;body.box_half={1,1,1};
 body.mass=std::numeric_limits<float>::infinity();config.bodies.push_back(body);
 const auto before=gameref_get(&f.world)->children.size();
 const auto frame=part_pose_matrix({});
 SLRR_REQUIRE(part_add_dynamic_native(config,{},frame,&f.world,0)==nullptr);
 SLRR_REQUIRE(gameref_get(&f.world)->children.size()==before);
 SLRR_REQUIRE(part_add_dynamic_native(config,{},frame,&f.world,0)==nullptr);
 SLRR_REQUIRE(gameref_get(&f.world)->children.size()==before);
 std::puts("PASS invalid dynamic-native creation cleans its partial state and suppresses repeated retries");
}
int main(){const standalone::Test tests[]={matrix_rejects_nonfinite,malformed_free_body_does_not_stop_other_bodies,invalid_free_frame_recovers_last_pose,degenerate_chassis_is_suspended,corrupt_chassis_restores_pose,wheel_setters_reject_nonfinite_atomically,numerical_failure_keeps_native_pump_alive,malformed_dynamic_native_creation_is_transactional};return standalone::run("physics_recovery",tests);}
