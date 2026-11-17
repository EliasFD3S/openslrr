#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
namespace slrr { void spawn_ground_shell(Jvm&,const RpkIndex&,Object*); }
int main(){
 using namespace slrr;
 Fixture f;
 const auto game=fs::absolute("game");
 if(!fs::exists(game/"misc/garage.rpk")){std::fprintf(stderr,"Requires local game/misc/garage.rpk (read-only asset test)\n");return 2;}
 f.vm.set_game_root(game.string());f.packs.set_root(game.string());
 const auto pack=f.packs.open_lib("misc/garage.rpk");scene_instances_set_resources(&f.packs);
 int failures=0;
 for(unsigned local:{0x7au,0x06u,0xc7u,0x2fu}){
  Object* ground=f.vm.new_object("java.util.resource.GroundRef");
  auto* state=gameref_upsert(ground,GameRefKind::GroundRef);
  state->type_id=RpkIndex::make_full_id(pack,local);
  spawn_ground_shell(f.vm,f.packs,ground);
  bool ok=!state->children.empty();
  auto before=state->children.size();spawn_ground_shell(f.vm,f.packs,ground);ok=ok&&state->children.size()==before;
  auto scene=scene_instances_collect_root(ground);ok=ok&&!scene.empty();
  std::printf("garage 0x%x shell=%zu meshes=%zu %s\n",local,before,scene.size(),ok?"PASS":"FAIL");
  if(!ok)++failures;
  auto children=state->children;for(auto* child:children)gameref_remove(child);gameref_remove(ground);
 }
 scene_instances_set_resources(nullptr);
 return failures?1:0;
}
