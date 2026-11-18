#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/osd_registry.hpp"
using namespace slrr;
void rectangle_renders_once(){
 Fixture f;gameref_clear();osd_clear_widgets();
 f.packs.set_root(fs::absolute("game").string());auto pack=f.packs.open_lib("frontend.rpk");SLRR_REQUIRE(pack>0);scene_instances_set_resources(&f.packs);
 Object cursor,rectangle;cursor.class_name="java.io.MouseCursor";
 auto* root=gameref_upsert(&cursor,GameRefKind::GameType);root->flags=kWorldTreeRoot;
 auto* node=gameref_upsert(&rectangle,GameRefKind::RenderRef);node->script=&rectangle;gameref_set_parent(&rectangle,&cursor);gameref_set_type(*node,RpkIndex::make_full_id(pack,0xd7));
 auto ordinary=scene_instances_collect_root(&cursor);SLRR_REQUIRE(ordinary.size()==1);
 rectangle.fields["_rect"]=Value::make_int(1);
 OsdWidget widget;widget.picture_source=&rectangle;widget.background=true;widget.half_w=.1f;widget.half_h=.02f;
 const auto id=osd_add_widget(widget);rectangle.fields["_osd_wi"]=Value::make_int(id);
 SLRR_REQUIRE(scene_instances_collect_root(&cursor).empty());
 SLRR_REQUIRE(osd_widget_mut(id)&&!osd_widget_mut(id)->dead);
 // Hidden UI quads must not leak into the cursor camera either.
 osd_widget_mut(id)->hidden=true;SLRR_REQUIRE(scene_instances_collect_root(&cursor).empty());
 osd_hide_widget(id);SLRR_REQUIRE(scene_instances_collect_root(&cursor).empty());
 // The filter is for registered UI rectangles, not ordinary world objects.
 rectangle.fields.erase("_rect");SLRR_REQUIRE(scene_instances_collect_root(&cursor).size()==1);
 scene_instances_set_resources(nullptr);osd_clear_widgets();gameref_clear();
 std::puts("PASS cursor rectangle stays in 2D, including hidden state; ordinary mesh remains in 3D");
}
int main(){const standalone::Test tests[]={rectangle_renders_once};return standalone::run("tooltip_render",tests);}
