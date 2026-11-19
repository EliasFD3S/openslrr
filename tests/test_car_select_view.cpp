#include "save_io_fixture.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/osd_registry.hpp"
#include <limits>
#include <cmath>
#include <array>
using namespace slrr;
void inset_preview_keeps_page_and_pose(){
 Fixture f;gameref_clear();scene_instances_select(nullptr,nullptr,false);
 Object catalog,world,camera,viewport,cursor,cursor_camera,cursor_view;
 catalog.class_name="java.game.Catalog";cursor.class_name="java.io.MouseCursor";
 catalog.fields["CarScene"]=Value::make_obj(&world);catalog.fields["Cam"]=Value::make_obj(&camera);
 auto* root=gameref_upsert(&world,GameRefKind::Dummy);root->flags=kWorldTreeRoot;
 auto* cam=gameref_upsert(&camera,GameRefKind::Camera);gameref_set_parent(&camera,&world);
 auto* vp=gameref_upsert(&viewport,GameRefKind::Viewport);vp->vp_w=.45f;vp->vp_h=.4f;vp->vp_active=true;vp->vp_pri=12;
 gameref_activate_camera(*cam,*vp,1);cam->aim_look=cam->aim_move=true;cam->aim_eye[0]=-2;cam->aim_eye[1]=1;cam->aim_eye[2]=-4;cam->aim_at[0]=.3f;cam->aim_at[1]=-.6f;
 auto expected=*cam;
 f.sections.adopt_actual(&catalog);auto render=create_render_backend(RenderBackendKind::Null);
 mainloop_sync_section_render(*render,f.sections);
 SLRR_REQUIRE(scene_instances_world()==nullptr && fullscreen_script_camera()==nullptr);
 SLRR_REQUIRE(cam->aim_eye[0]==expected.aim_eye[0] && cam->aim_eye[1]==expected.aim_eye[1] && cam->aim_eye[2]==expected.aim_eye[2]);
 // A tooltip uses a fullscreen cursor camera at a higher viewport priority.
 auto* cr=gameref_upsert(&cursor,GameRefKind::GameType);cr->class_name=cursor.class_name;cr->flags=kWorldTreeRoot;
 auto* cc=gameref_upsert(&cursor_camera,GameRefKind::Camera);gameref_set_parent(&cursor_camera,&cursor);
 auto* cv=gameref_upsert(&cursor_view,GameRefKind::Viewport);cv->vp_active=true;cv->vp_pri=127;gameref_activate_camera(*cc,*cv,0);
 for(int i=0;i<20;++i){mainloop_sync_section_render(*render,f.sections);SLRR_REQUIRE(scene_instances_world()==nullptr && fullscreen_script_camera()==nullptr);}
 const auto cameras=gameref_active_cameras();SLRR_REQUIRE(std::find(cameras.begin(),cameras.end(),cam)!=cameras.end());
 // Fullscreen showroom remains a main scene, and cursor cannot steal it.
 vp->vp_w=vp->vp_h=1.f;mainloop_sync_section_render(*render,f.sections);
 SLRR_REQUIRE(scene_instances_world()==&world && fullscreen_script_camera()==cam);
 scene_instances_select(nullptr,nullptr,false);gameref_clear();
 std::puts("PASS catalog inset retains script pose and preview camera; cursor hover cannot replace world; fullscreen showroom preserved");
}
void cursor_camera_is_ui_only(){
 Fixture f;gameref_clear();scene_instances_select(nullptr,nullptr,false);
 Object cursor,camera,viewport;
 auto* root=gameref_upsert(&cursor,GameRefKind::GameType);root->class_name="java.io.MouseCursor";root->flags=kWorldTreeRoot;
 auto* cam=gameref_upsert(&camera,GameRefKind::Camera);gameref_set_parent(&camera,&cursor);
 auto* vp=gameref_upsert(&viewport,GameRefKind::Viewport);vp->vp_active=true;vp->vp_pri=127;gameref_activate_camera(*cam,*vp,0);
 SLRR_REQUIRE(fullscreen_script_camera()==nullptr);
 SLRR_REQUIRE(gameref_active_cameras().size()==1); // UI camera remains usable by UI
 gameref_clear();std::puts("PASS cursor-only UI cannot become the main 3D camera");
}
void tooltip_does_not_promote_page_background(){
 Fixture f;osd_clear_widgets();Object page,dialog;
 page.class_name="java.render.Osd";dialog.fields["flags"]=Value::make_int(1);
 OsdWidget bg;bg.owner=&page;bg.layer=10;bg.background=true;bg.icon_path="catalog.ptx";bg.half_w=bg.half_h=1;
 int page_id=osd_add_widget(bg);SLRR_REQUIRE(osd_page_background_layer()==10);
 // Tooltip Rectangle is parented to MouseCursor, with no page OSD owner.
 OsdWidget tip;tip.layer=0;tip.background=true;tip.icon_path="tooltip.ptx";tip.half_w=.1f;tip.half_h=.02f;
 int tip_id=osd_add_widget(tip);
 int old_first_layer=10;for(const auto& w:osd_widgets())if(!w.hidden)old_first_layer=std::min(old_first_layer,w.layer);
 SLRR_REQUIRE(old_first_layer==0); // former renderer criterion would put page above car
 SLRR_REQUIRE(osd_page_background_layer()==10);
 // Actual modal dialog plates still cover the car; they are not page plates.
 bg.owner=&dialog;bg.layer=22;osd_add_widget(bg);SLRR_REQUIRE(osd_page_background_layer()==10);
 for(int i=0;i<20;++i){osd_widget_mut(tip_id)->hidden=(i%2)==0;SLRR_REQUIRE(osd_page_background_layer()==10);}
 osd_hide_widget(page_id);SLRR_REQUIRE(osd_page_background_layer()==std::numeric_limits<int32_t>::max());
 osd_clear_widgets();std::puts("PASS tooltip layer does not lift catalog background over car, repeated show/hide and modal plates");
}
void preview_mesh_and_camera_share_the_root_frame(){
  Fixture f;gameref_clear();
  Object root,object,camera;
  auto* r=gameref_upsert(&root,GameRefKind::Dummy);r->flags=kWorldTreeRoot;
  gameref_upsert(&object,GameRefKind::RenderRef);gameref_set_parent(&object,&root);
  auto* c=gameref_upsert(&camera,GameRefKind::Camera);gameref_set_parent(&camera,&root);
  gameref_set_matrix(&camera,-.0778f,.13f,-.1041f,-2.5f,-.7f,0);
  auto projected=[&]{
    float model[16],view[16];
    SLRR_REQUIRE(scene_instance_transform(&object,&root,model));
    SLRR_REQUIRE(scene_instances_preview_view(*c,view));
    std::array<float,3> result{};
    for(int row=0;row<3;++row)
      result[row]=view[row]*model[12]+view[4+row]*model[13]+view[8+row]*model[14]+view[12+row];
    return result;
  };
  const auto expected=projected();
  SLRR_REQUIRE(expected[2]>0.f); // can is in front of the camera
  // The shared local world may move, turn or scale; it must not make an
  // inventory item bounce relative to the camera in that same world.
  for(float y:{.482f,-.2f,1.1f}){
    gameref_set_matrix(&root,.3f,y,-.6f,.8f,.2f,0);
    const auto actual=projected();
    for(int i=0;i<3;++i)SLRR_REQUIRE(std::fabs(actual[i]-expected[i])<1e-5f);
  }
  gameref_clear();
}
int main(){const standalone::Test tests[]={inset_preview_keeps_page_and_pose,cursor_camera_is_ui_only,tooltip_does_not_promote_page_background,preview_mesh_and_camera_share_the_root_frame};return standalone::run("car_select_view",tests);}
