#include "save_io_fixture.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/osd_registry.hpp"
#include <cmath>
#include <limits>
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
// #487: a CameraControl pose drives preview cameras too, writes back a yaw
// that survives looking straight down, and seeds the stock smoothing so
// clearPose continues from the last drawn pose.
void mod_pose_on_preview_and_clear(){
 Fixture f;gameref_clear();scene_instances_select(nullptr,nullptr,false);
 Object world,camera,viewport;
 auto* root=gameref_upsert(&world,GameRefKind::Dummy);root->flags=kWorldTreeRoot;
 auto* cam=gameref_upsert(&camera,GameRefKind::Camera);gameref_set_parent(&camera,&world);
 auto* vp=gameref_upsert(&viewport,GameRefKind::Viewport);vp->vp_w=.45f;vp->vp_h=.4f;vp->vp_active=true;vp->vp_pri=12;
 gameref_activate_camera(*cam,*vp,1);cam->aim_look=cam->aim_move=true;
 float eye[3],at[3],up[3],fov=0;
 SLRR_REQUIRE(!gameref_mod_camera_view(&camera,eye,at,up,&fov));  // no pose: stock aim
 const float e0[3]={1,5,2},below[3]={1,-5,2},east[3]={1,0,0},north[3]={0,0,-1};
 SLRR_REQUIRE(gameref_mod_camera_set(&camera,ModCameraMode::World,e0,below,east,40.f,nullptr));
 SLRR_REQUIRE(gameref_mod_camera_view(&camera,eye,at,up,&fov) && fov==40.f);
 SLRR_REQUIRE(eye[1]==5.f && at[1]==-5.f && std::fabs(up[0]-1.f)<1e-5f);
 SLRR_REQUIRE(cam->pos[0]==1.f && cam->pos[1]==5.f && cam->pos[2]==2.f);
 // Straight down: the yaw comes from up, and the stock up axis of that yaw
 // and pitch, (sin y sin p, cos p, cos y sin p), is the pose's up.
 auto stock_up=[&](float* u){const float y=cam->ypr[0],p=cam->ypr[1];
   u[0]=std::sin(y)*std::sin(p);u[1]=std::cos(p);u[2]=std::cos(y)*std::sin(p);};
 float u[3];stock_up(u);
 SLRR_REQUIRE(std::fabs(u[0]-1.f)<1e-4f && std::fabs(u[1])<1e-4f && std::fabs(u[2])<1e-4f);
 const float east_yaw=cam->ypr[0];
 SLRR_REQUIRE(gameref_mod_camera_set(&camera,ModCameraMode::World,e0,below,north,40.f,nullptr));
 SLRR_REQUIRE(gameref_mod_camera_view(&camera,eye,at,up,&fov));
 stock_up(u);SLRR_REQUIRE(std::fabs(u[2]+1.f)<1e-4f && std::fabs(cam->ypr[0]-east_yaw)>1.f);
 // The stock smoothing starts from the drawn pose, so clearPose does not snap.
 SLRR_REQUIRE(cam->cam_pose_init && cam->eye_s[1]==5.f && cam->look_s[1]==-5.f);
 // A fullscreen free script camera after clearPose keeps eye and direction.
 const float e1[3]={3,2,1},a1[3]={-1,1.5f,-6},wup[3]={0,1,0};
 SLRR_REQUIRE(gameref_mod_camera_set(&camera,ModCameraMode::World,e1,a1,wup,0.f,nullptr));
 SLRR_REQUIRE(gameref_mod_camera_view(&camera,eye,at,up,&fov));
 gameref_mod_camera_clear(&camera);cam->aim_look=cam->aim_move=false;vp->vp_w=vp->vp_h=1.f;
 scene_instances_select(&world,nullptr,false);
 SLRR_REQUIRE(fullscreen_script_camera()==cam);
 float se[3],sa[3],sf=0,zn=0,zf=0,su[3];
 SLRR_REQUIRE(gameref_world_camera(se,sa,&sf,&zn,&zf,su));
 float d0[3],d1[3];for(int i=0;i<3;++i){d0[i]=a1[i]-e1[i];d1[i]=sa[i]-se[i];}
 const float l0=std::sqrt(d0[0]*d0[0]+d0[1]*d0[1]+d0[2]*d0[2]),l1=std::sqrt(d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2]);
 SLRR_REQUIRE(se[0]==3.f && se[1]==2.f && se[2]==1.f);
 for(int i=0;i<3;++i)SLRR_REQUIRE(std::fabs(d0[i]/l0-d1[i]/l1)<1e-4f);
 scene_instances_select(nullptr,nullptr,false);gameref_clear();
 std::puts("PASS mod poses drive preview cameras, keep a straight-down yaw and hand over to the stock camera without a snap");
}
// #487 review 4: the free script camera keeps its roll, a rejected setMatrix
// is reported, near-vertical poses write a YPR that rebuilds the drawn up,
// and the aimed preview cache starts from the drawn pose.
void camera_roll_and_vertical_write_back(){
 Fixture f;gameref_clear();scene_instances_select(nullptr,nullptr,false);
 Object world,camera,viewport;
 auto* root=gameref_upsert(&world,GameRefKind::Dummy);root->flags=kWorldTreeRoot;
 auto* cam=gameref_upsert(&camera,GameRefKind::Camera);gameref_set_parent(&camera,&world);
 auto* vp=gameref_upsert(&viewport,GameRefKind::Viewport);vp->vp_active=true;vp->vp_pri=12;
 gameref_activate_camera(*cam,*vp,1);scene_instances_select(&world,nullptr,false);
 SLRR_REQUIRE(fullscreen_script_camera()==cam);
 // A rejected pose (NaN) changes nothing and says so; natives_gameref then
 // leaves the roll alone too.
 SLRR_REQUIRE(gameref_set_matrix(&camera,1,2,3,.4f,0,0));
 SLRR_REQUIRE(!gameref_set_matrix(&camera,std::nanf(""),2,3,.4f,0,.5f) && cam->ypr[2]==0.f);
 // camera_free: the free camera path applies camera_roll (it returned before).
 cam->camera_free=true;cam->camera_roll=.5f;
 float e[3],a[3],u[3],fv,zn,zf;
 SLRR_REQUIRE(gameref_world_camera(e,a,&fv,&zn,&zf,u));
 float expect[3]={0,1,0};SLRR_REQUIRE(mod_roll_up(e,a,.5f,expect));
 for(int i=0;i<3;++i)SLRR_REQUIRE(std::fabs(u[i]-expect[i])<1e-4f);
 SLRR_REQUIRE(std::fabs(u[1]-std::cos(.5f))<1e-3f);
 cam->camera_free=false;cam->camera_roll=0.f;
 // Near vertical: the written yaw/pitch/roll rebuild the pose's up.
 for(const float h:{0.f,5e-6f,2e-5f,1e-3f,3e-2f})
  for(const float ux:{1.f,0.f})
   for(const float sign:{-1.f,1.f}){
    const float eye[3]={0,10,0},at[3]={0,10+sign*5,-h*5},up[3]={ux,0,ux>0?0.f:-1.f};
    SLRR_REQUIRE(gameref_mod_camera_set(&camera,ModCameraMode::World,eye,at,up,0.f,nullptr));
    float pe[3],pa[3],pu[3];SLRR_REQUIRE(gameref_mod_camera_view(&camera,pe,pa,pu,nullptr));
    const float y=cam->ypr[0],p=cam->ypr[1],r=cam->ypr[2];
    // The stock camera's up for this YPR: level up rolled by r about the view.
    float rebuilt[3]={std::sin(y)*std::sin(p),std::cos(p),std::cos(y)*std::sin(p)};
    if(r!=0.f){const float fw[3]={-std::sin(y)*std::cos(p),std::sin(p),-std::cos(y)*std::cos(p)};
      const float o[3]={0,0,0};SLRR_REQUIRE(mod_roll_up(o,fw,r,rebuilt));}
    for(int i=0;i<3;++i)SLRR_REQUIRE(std::fabs(rebuilt[i]-pu[i])<2e-3f);
    // The aimed preview cache follows the drawn pose.
    for(int i=0;i<3;++i)SLRR_REQUIRE(cam->aim_eye[i]==pe[i] && cam->aim_at[i]==pa[i]);
   }
 scene_instances_select(nullptr,nullptr,false);gameref_clear();
 std::puts("PASS free camera roll, rejected setMatrix, near-vertical YPR rebuilds up, aim cache seeded");
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
int main(){const standalone::Test tests[]={inset_preview_keeps_page_and_pose,mod_pose_on_preview_and_clear,camera_roll_and_vertical_write_back,cursor_camera_is_ui_only,tooltip_does_not_promote_page_background};return standalone::run("car_select_view",tests);}
