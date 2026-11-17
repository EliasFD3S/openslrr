#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
using namespace slrr;
namespace {
struct PhysicsFixture : Fixture {
 Object ref, position;
 PhysicsFixture(){register_osd_natives(vm,packs,sections);position.fields["x"]=Value::make_float(1);position.fields["y"]=Value::make_float(2);position.fields["z"]=Value::make_float(3);}
 ~PhysicsFixture(){gameref_remove(&ref);}
 Value call(const char* name,const char* sig,const std::vector<Value>& args={}){return registry.invoke("java.util.resource.PhysicsRef",name,sig,&ref,args);}
 Object* read(){return call("getPos","()Ljava.lang.Vector3;").v.o;}
 void set(){call("setMatrix","(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",{Value::make_obj(&position),Value::make_obj(nullptr)});}
 void body(){physicsref_create_box(&ref,nullptr,1,1,1,"test");set();}
};
SLRR_TEST(moved_body){PhysicsFixture f;f.body();auto* s=gameref_get(&f.ref);s->pos[0]=10;s->pos[1]=20;s->pos[2]=30;auto* v=f.read();SLRR_REQUIRE(v&&v->fields["x"].v.f==10&&v->fields["y"].v.f==20&&v->fields["z"].v.f==30);}
SLRR_TEST(independent_result){PhysicsFixture f;f.body();auto* first=f.read();first->fields["x"]=Value::make_float(99);SLRR_REQUIRE(f.read()->fields["x"].v.f==1);SLRR_REQUIRE(f.position.fields["x"].v.f==1);}
SLRR_TEST(hotspot_script_coordinates){PhysicsFixture f;gameref_upsert(&f.ref,GameRefKind::PhysicsRef);f.set();auto* s=gameref_get(&f.ref);s->pos[0]=100;auto* v=f.read();SLRR_REQUIRE(v!=&f.position&&v->fields["x"].v.f==1);v->fields["x"]=Value::make_float(99);SLRR_REQUIRE(f.read()->fields["x"].v.f==1);}
SLRR_TEST(static_after_motion){PhysicsFixture f;f.body();auto* s=gameref_get(&f.ref);s->pos[1]=42;s->physics_static=true;SLRR_REQUIRE(f.read()->fields["y"].v.f==42);}
}
int main(){const slrr::standalone::Test tests[]={moved_body,independent_result,hotspot_script_coordinates,static_after_motion};return slrr::standalone::run("physics_position",tests);}
