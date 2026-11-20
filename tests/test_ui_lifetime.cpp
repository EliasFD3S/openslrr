#include "save_io_fixture.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/gc_roots.hpp"
using namespace slrr;
int main() {
  Fixture f;
  osd_clear_widgets();slrr_text_clear_all();
  f.vm.add_gc_roots([](const GcRootVisitor& visit){osd_visit_gc_roots(visit);font_visit_gc_roots(visit);});
  Object owner,source;
  OsdWidget a;a.owner=&owner;a.picture_source=&source;a.hidden=true;
  int32_t retired=osd_add_widget(a);
  OsdWidget b;b.cmd=123;
  int32_t survivor=osd_add_widget(b);
  Object value;
  osd_mark_string_input(nullptr,&value,nullptr,"a");
  osd_set_hover_at(0,0);
  auto rooted=[&](void* address){bool found=false;osd_visit_gc_roots([&](const void* p){found=found||p==address;});return found;};
  SLRR_REQUIRE(rooted(&owner)); // hidden pages must remain reopenable
  osd_hide_widget(retired);
  SLRR_REQUIRE(!rooted(&owner)&&!rooted(&source));
  osd_compact_widgets();
  SLRR_REQUIRE(osd_widgets().size()==1);
  osd_on_char('b');
  SLRR_REQUIRE(value.str=="ab");
  SLRR_REQUIRE(osd_hover_widget()&&osd_hover_widget()->cmd==123);
  SLRR_REQUIRE(!osd_widget_mut(retired));
  SLRR_REQUIRE(osd_widget_mut(survivor)&&osd_widget_mut(survivor)->cmd==123);
  osd_set_widget_pos(survivor,.25f,.5f);
  SLRR_REQUIRE(osd_widgets()[0].x==.25f);
  osd_clear_widgets();
  int32_t replacement=osd_add_widget(b);
  SLRR_REQUIRE(replacement!=survivor&&!osd_widget_mut(survivor));
  osd_hide_widget(replacement);osd_compact_widgets();
  for(int i=0;i<200;++i){int id=osd_add_widget(a);osd_hide_widget(id);osd_compact_widgets();}
  SLRR_REQUIRE(osd_widgets().empty());
  std::puts("PASS stable handles, hidden roots, destroyed roots and repeated compaction");

  // An Osd whose viewport was never activated takes no clicks (the garage
  // parts bin built while painting lay over the decal picker).
  Object osd,viewport,icon;osd.class_name="java.render.Osd";
  viewport.fields["active"]=Value::make_int(0);osd.fields["vp"]=Value::make_obj(&viewport);
  osd_on_field_write(&osd,"vp");
  OsdWidget slot;slot.owner=&osd;slot.event_source=&icon;slot.hit_only=true;
  osd_add_widget(slot);
  SLRR_REQUIRE(osd_hit_index(0,0)<0);
  osd_set_owner_visible(&osd,true);
  SLRR_REQUIRE(osd_hit_index(0,0)>=0&&rooted(&icon));
  osd_set_owner_visible(&osd,false);
  SLRR_REQUIRE(osd_hit_index(0,0)<0);
  osd_clear_widgets();
  std::puts("PASS inactive viewports take no clicks until shown; icon sources stay rooted");

  slrr_font_set_game_root(fs::absolute("game").string());
  auto* first=f.vm.new_object("java.util.resource.ResourceRef");
  auto* second=f.vm.new_object("java.util.resource.ResourceRef");
  SLRR_REQUIRE(slrr_font_load(first,"slii24")&&slrr_font_load(second,"SLII24"));
  const auto* shared=slrr_font_get(first);
  SLRR_REQUIRE(shared&&shared==slrr_font_get(second));
  const auto asset=shared->cache_key;
  auto* text=f.vm.new_object("java.render.Text");
  slrr_text_create(text,first,0,0);slrr_text_set_osd(text,&owner);
  f.vm.collect_garbage();
  SLRR_REQUIRE(slrr_font_ready(first)&&!slrr_font_ready(second));
  SLRR_REQUIRE(slrr_font_asset_live(asset));
  osd_destroy_owner(&owner);
  f.vm.collect_garbage();
  SLRR_REQUIRE(!slrr_font_asset_live(asset));
  std::puts("PASS shared font atlas, live text retention and weak font alias collection");
  osd_clear_widgets();slrr_text_clear_all();
  return 0;
}
