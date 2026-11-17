#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
using namespace slrr;
int main() {
 gameref_clear();
 Object root,other_world,keep,duplicate,already_hidden,unrelated,late;
 gameref_upsert(&root);gameref_upsert(&other_world);
 auto render=[&](Object& object,void* parent,int type){
   auto* n=gameref_upsert(&object,GameRefKind::RenderRef);
   gameref_set_type(*n,type);gameref_set_parent(&object,parent);return n;
 };
 render(keep,&root,7);
 std::vector<std::unique_ptr<Object>> objects;
 for(int i=0;i<5000;++i){objects.push_back(std::make_unique<Object>());gameref_upsert(objects.back().get());gameref_set_parent(objects.back().get(),&root);}
 render(duplicate,&root,7);
 render(already_hidden,&root,7)->empty=true;
 render(unrelated,&other_world,7);
 GameRefRenderMask mask;
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&root,&keep,7));
 SLRR_REQUIRE(gameref_get(&duplicate)->empty && !gameref_get(&keep)->empty && !gameref_get(&unrelated)->empty);
 SLRR_REQUIRE(mask.hidden.size()==1);
 for(int i=0;i<10000;++i)SLRR_REQUIRE(!gameref_mask_duplicate_renders(mask,&root,&keep,7));
 // Motion and redundant parenting must not trigger a new scene walk.
 gameref_set_matrix(&duplicate,1,2,3,0,0,0);gameref_set_parent(&duplicate,&root);
 SLRR_REQUIRE(!gameref_mask_duplicate_renders(mask,&root,&keep,7));
 render(late,&root,7);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&root,&keep,7) && gameref_get(&late)->empty);
 gameref_set_parent(&late,&other_world);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&root,&keep,7) && !gameref_get(&late)->empty);
 gameref_set_type(*gameref_get(&duplicate),8);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&root,&keep,7) && !gameref_get(&duplicate)->empty);
 gameref_set_type(*gameref_get(&duplicate),7);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&root,&keep,7) && gameref_get(&duplicate)->empty);
 // Alias links and malformed cycles terminate; canonical nodes appear once.
 Object alias;gameref_bind_alias(&alias,&root);
 gameref_set_parent(&root,&duplicate);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&alias,&keep,7));
 SLRR_REQUIRE(mask.hidden.size()==1);
 gameref_set_parent(&root,nullptr);
 // Stale masks must not change a different object reusing the same key.
 gameref_remove(&duplicate);
 render(duplicate,&root,8)->empty=true;
 gameref_release_render_mask(mask);
 SLRR_REQUIRE(gameref_get(&duplicate)->empty && gameref_get(&already_hidden)->empty);
 SLRR_REQUIRE(gameref_mask_duplicate_renders(mask,&other_world,&unrelated,7));
 SLRR_REQUIRE(gameref_get(&late)->empty && !gameref_get(&unrelated)->empty);
 gameref_release_render_mask(mask);SLRR_REQUIRE(!gameref_get(&late)->empty);
 gameref_mask_duplicate_renders(mask,&root,&keep,7);
 gameref_clear();gameref_release_render_mask(mask);
 std::puts("PASS >4096 nodes; 10000 cache hits; motion, creation, type, parent, world, alias/cycle, release and key reuse");
}
