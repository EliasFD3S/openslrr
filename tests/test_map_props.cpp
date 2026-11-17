#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
namespace slrr { void spawn_ground_shell(Jvm&,const RpkIndex&,Object*); }
int main(){using namespace slrr;Fixture f;auto game=fs::absolute("game");f.vm.set_game_root(game.string());f.packs.set_root(game.string());f.packs.open_all();
 int failed=0;
 for(const char* path:{"maps/test_track.rpk","maps/ROC_track.rpk"}){
  scene_instances_set_resources(&f.packs);
  auto pack=f.packs.open_lib(path);auto id=RpkIndex::make_full_id(pack,1);auto map=scene_instances_ground_map(f.packs,id);
  bool ok=map.render_id&&!map.placements.empty()&&map.exterior_maps.empty();
  Object* root=f.vm.new_object("java.util.resource.GroundRef");gameref_upsert(root,GameRefKind::GroundRef)->type_id=id;
  spawn_ground_shell(f.vm,f.packs,root);scene_instances_select(root,nullptr,false);
  std::vector<std::shared_ptr<const ScxGeometry>> resident;
  scene_instances_collect(nullptr,&resident);
  ok=ok&&scene_instances_has_ground_props()&&!resident.empty();
  auto count=gameref_get(root)->children.size();spawn_ground_shell(f.vm,f.packs,root);ok=ok&&gameref_get(root)->children.size()==count;
  std::printf("%s placements=%zu exterior=%zu registered=%d %s\n",path,map.placements.size(),map.exterior_maps.size(),scene_instances_has_ground_props(),ok?"PASS":"FAIL");
  if(!ok)++failed;
  auto children=gameref_get(root)->children;for(auto child:children)gameref_remove(child);gameref_remove(root);
  scene_instances_set_resources(nullptr);
 }
 return failed?1:0;}
