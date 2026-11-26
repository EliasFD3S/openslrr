#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <cctype>
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
 // Track Wizard addons name their render-only meshes in a native whose cfg
 // stays on disk (`native object <path>\TrackRenders.cfg`), not an inline ICFG.
 const char* addon="maps/Addon_Maps/Tsukuba_Circuit.rpk";
 if(!fs::exists(game/addon)){std::printf("%s SKIP (addon not installed)\n",addon);return failed?1:0;}
 {scene_instances_set_resources(&f.packs);
  auto pack=f.packs.open_lib(addon);auto id=RpkIndex::make_full_id(pack,1);auto map=scene_instances_ground_map(f.packs,id);
  size_t disk_cfg=0;for(const auto& p:map.placements){std::string kind;if(f.packs.payload_prop(p.type,"native").find("TrackRenders.cfg")!=std::string::npos&&!f.packs.native_config(p.type,&kind).empty()&&kind=="object")++disk_cfg;}
  Object* root=f.vm.new_object("java.util.resource.GroundRef");gameref_upsert(root,GameRefKind::GroundRef)->type_id=id;
  spawn_ground_shell(f.vm,f.packs,root);scene_instances_select(root,nullptr,false);
  std::vector<std::shared_ptr<const ScxGeometry>> resident;scene_instances_collect(nullptr,&resident);
  bool render=false;for(const auto& g:resident){auto path=g?g->info.path:std::string{};std::transform(path.begin(),path.end(),path.begin(),[](unsigned char c){return char(std::tolower(c));});std::replace(path.begin(),path.end(),'\\','/');if(path.find("render/tsukuba.scx")!=std::string::npos)render=true;}
  bool ok=disk_cfg>0&&render;
  std::printf("%s placements=%zu disk_cfg=%zu render_resident=%d %s\n",addon,map.placements.size(),disk_cfg,render,ok?"PASS":"FAIL");
  if(!ok)++failed;
  auto children=gameref_get(root)->children;for(auto child:children)gameref_remove(child);gameref_remove(root);
  scene_instances_set_resources(nullptr);}
 return failed?1:0;}
