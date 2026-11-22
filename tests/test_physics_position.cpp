#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
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

// Reference the former identity-matrix triangle query, independently of its
// optimized implementation. Volatile coefficients retain 0 * Inf/NaN behavior.
bool identity_floor_reference(const ScxGeometry& mesh, float x, float z,
                              float below, SceneFloorHit& result) {
  volatile float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  bool hit = false;
  float best = 0;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    float v[3][3];
    float min_x = 0, max_x = 0, min_z = 0, max_z = 0;
    for (int k = 0; k < 3; ++k) {
      const size_t p = size_t(mesh.indices[t + k]) * 3;
      const float vx = mesh.positions[p] * mesh.position_scale;
      const float vy = mesh.positions[p + 1] * mesh.position_scale;
      const float vz = mesh.positions[p + 2] * mesh.position_scale;
      v[k][0] = m[0]*vx + m[4]*vy + m[8]*vz + m[12];
      v[k][1] = m[1]*vx + m[5]*vy + m[9]*vz + m[13];
      v[k][2] = m[2]*vx + m[6]*vy + m[10]*vz + m[14];
      if (!k) {
        min_x = max_x = v[k][0]; min_z = max_z = v[k][2];
      } else {
        min_x = std::min(min_x, v[k][0]); max_x = std::max(max_x, v[k][0]);
        min_z = std::min(min_z, v[k][2]); max_z = std::max(max_z, v[k][2]);
      }
    }
    if (x < min_x || x > max_x || z < min_z || z > max_z) continue;
    const float x0=v[0][0], z0=v[0][2], x1=v[1][0], z1=v[1][2];
    const float x2=v[2][0], z2=v[2][2];
    const float denom=(z1-z2)*(x0-x2)+(x2-x1)*(z0-z2);
    if (std::fabs(denom) < 1e-8f) continue;
    const float a=((z1-z2)*(x-x2)+(x2-x1)*(z-z2))/denom;
    const float b=((z2-z0)*(x-x2)+(x0-x2)*(z-z2))/denom;
    const float c=1.f-a-b;
    if (a < -1e-4f || b < -1e-4f || c < -1e-4f) continue;
    const float y=a*v[0][1]+b*v[1][1]+c*v[2][1];
    if (y > below + .05f || (hit && !(y > best))) continue;
    best=y; hit=true;
    const float ax=v[1][0]-v[0][0], ay=v[1][1]-v[0][1], az=v[1][2]-v[0][2];
    const float bx=v[2][0]-v[0][0], by=v[2][1]-v[0][1], bz=v[2][2]-v[0][2];
    const float nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
    const float size=std::sqrt(nx*nx+ny*ny+nz*nz), sign=ny < 0 ? -1.f : 1.f;
    result.normal[0]=nx/size*sign;
    result.normal[1]=ny/size*sign;
    result.normal[2]=nz/size*sign;
  }
  if (hit) result.height=best;
  return hit;
}
struct FloorQueryScene {
  Object world, surface;
  FloorQueryScene() {
    scene_instances_set_resources(nullptr);
    gameref_upsert(&world, GameRefKind::Dummy)->flags=kWorldTreeRoot;
    scene_instances_select(&world, nullptr, false);
  }
  ~FloorQueryScene() {
    scene_instances_set_resources(nullptr);
    gameref_remove(&surface);
    gameref_remove(&world);
  }
};
ScxGeometry sloped_floor(float scale, bool reversed=false) {
  ScxGeometry mesh;
  mesh.info.ok=true;
  mesh.position_scale=scale;
  const float unit=scale == .01f ? 100.f : 1.f;
  mesh.positions={-0.f,-0.f,-0.f, 4*unit,unit,0, 0,2*unit,4*unit};
  mesh.indices=reversed ? std::vector<uint32_t>{0,2,1} : std::vector<uint32_t>{0,1,2};
  return mesh;
}
void same_floor_float(float actual, float expected) {
  if (std::isnan(expected)) SLRR_REQUIRE(std::isnan(actual));
  else if (std::isinf(expected)) SLRR_REQUIRE(actual == expected);
  else {
    SLRR_REQUIRE(std::fabs(actual-expected) <= 2e-6f);
    if (actual == 0 && expected == 0)
      SLRR_REQUIRE(std::signbit(actual) == std::signbit(expected));
  }
}
void compare_floor(const ScxGeometry& reference, float x, float z, float below) {
  SceneFloorHit actual, expected;
  const bool want=identity_floor_reference(reference,x,z,below,expected);
  SLRR_REQUIRE(scene_instances_sample_floor(x,z,below,nullptr,actual) == want);
  if (!want) return;
  same_floor_float(actual.height,expected.height);
  for (int axis=0; axis<3; ++axis) same_floor_float(actual.normal[axis],expected.normal[axis]);
}
SLRR_TEST(identity_floor_scaled_slopes_and_ceiling) {
  for (float scale : {.01f,1.f}) for (bool reversed : {false,true}) {
    FloorQueryScene scene;
    auto mesh=sloped_floor(scale,reversed);
    // A second, higher copy exercises highest-hit selection and ceiling filtering.
    const auto lower=mesh.positions;
    for (size_t p=0; p<lower.size(); ++p)
      mesh.positions.push_back(lower[p]+(p%3 == 1 ? 3.f/scale : 0.f));
    for (auto index : std::vector<uint32_t>(mesh.indices)) mesh.indices.push_back(index+3);
    scene_instances_add_static_mesh(mesh);
    for (const auto& q : std::array<std::array<float,3>,8>{{
        {{1,1,10}}, {{1,1,1}}, {{1,1,.70f}}, {{1,1,.699f}},
        {{-0.f,-0.f,1}}, {{2,2,2}}, {{3,3,10}}, {{5,1,10}}}})
      compare_floor(mesh,q[0],q[1],q[2]);
    SceneFloorHit hit;
    SLRR_REQUIRE(scene_instances_sample_floor(1,1,1,nullptr,hit));
    SLRR_REQUIRE(std::fabs(hit.height-.75f) < 1e-6f && hit.normal[1] > 0);
  }
}
SLRR_TEST(identity_floor_cached_world_transform_and_ignore) {
  FloorQueryScene scene;
  const auto source=sloped_floor(.01f);
  const uint32_t id=0x1234;
  scene_instances_override_collision(id,std::make_shared<ScxGeometry>(source));
  auto* node=gameref_upsert(&scene.surface,GameRefKind::RenderRef);
  gameref_set_type(*node,id);
  gameref_set_parent(&scene.surface,&scene.world);
  node->collision_active=true;
  node->matrix_override=true;
  // Rigid yaw plus tilt, with translation: no render-space Z reflection.
  const float matrix[16]={0,.6f,-.8f,0, 0,.8f,.6f,0, 1,0,0,0, 30,10,-20,1};
  std::copy(matrix,matrix+16,node->local_matrix);
  auto baked=source;
  baked.position_scale=1;
  for (size_t p=0; p<source.positions.size(); p+=3) {
    const float x=source.positions[p]*source.position_scale;
    const float y=source.positions[p+1]*source.position_scale;
    const float z=source.positions[p+2]*source.position_scale;
    for (int axis=0; axis<3; ++axis)
      baked.positions[p+axis]=matrix[axis]*x+matrix[4+axis]*y+matrix[8+axis]*z+matrix[12+axis];
  }
  scene_instances_prepare_physics();
  const float x=(baked.positions[0]+baked.positions[3]+baked.positions[6])/3;
  const float z=(baked.positions[2]+baked.positions[5]+baked.positions[8])/3;
  compare_floor(baked,x,z,100);
  SceneFloorHit hit;
  SLRR_REQUIRE(scene_instances_sample_floor(x,z,100,nullptr,hit));
  SLRR_REQUIRE(!scene_instances_sample_floor(x,z,100,&scene.surface,hit));
  SLRR_REQUIRE(!scene_instances_sample_floor(x,z,100,&scene.world,hit));
}
SLRR_TEST(identity_floor_large_triangle_and_nonfinite_height) {
  {
    FloorQueryScene scene;
    auto mesh=sloped_floor(1);
    for (auto& p : mesh.positions) p*=200;
    scene_instances_add_static_mesh(mesh); // exceeds the 4096-cell triangle limit
    compare_floor(mesh,200,200,1000);
  }
  // XZ remain indexable; the old identity's zero coefficients must still
  // propagate exceptional Y values rather than silently changing hit behavior.
  for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
    FloorQueryScene scene;
    auto mesh=sloped_floor(1);
    mesh.positions[1]=bad;
    scene_instances_add_static_mesh(mesh);
    compare_floor(mesh,1,1,10);
  }
}
}
int main(){const slrr::standalone::Test tests[]={moved_body,independent_result,hotspot_script_coordinates,static_after_motion,identity_floor_scaled_slopes_and_ceiling,identity_floor_cached_world_transform_and_ignore,identity_floor_large_triangle_and_nonfinite_height};return slrr::standalone::run("physics_position",tests);}
