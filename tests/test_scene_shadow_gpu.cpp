// Real D3D11 pixels through production vehicle/scenery shaders and bindings.
#include "../src/render/bgfx_lib_backend.hpp"
#include "../src/render/reflection_capture.hpp"
#include "../src/render/point_shadows.hpp"
#include "../src/render/shadow_cascades.hpp"
#include "../src/render/shadow_quality.hpp"
#include "save_io_fixture.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/script_lights.hpp"
#include <bx/math.h>
#include "dxbc/fs_mesh.sc.bin.h"
#include "dxbc/fs_vehicle.sc.bin.h"
#include <chrono>
#include <thread>
#include "slrr/frame_profiler.hpp"
#include "slrr/job_system.hpp"
#include <limits>
#include <fstream>
#include <sstream>
#include "slrr/paint_texture.hpp"
#include "slrr/skeleton.hpp"
#include <map>
namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;
namespace slrr {
struct SceneShadowRegression {
  static void ambient_occlusion(LibBgfxBackend& b) {
    Fixture f;Object root;
    gameref_upsert(&root)->flags=kWorldTreeRoot;
    scene_instances_select(&root,nullptr,false,SceneEnvironment::Interior);
    const auto saved=host_enhanced_graphics();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_strength=150;host_enhanced_graphics().ao_radius=150;
    b.garage_=true;b.outdoor_lighting_=false;b.fog_enabled_=false;
    b.fixture_lights_.clear();b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    PointLight ambient;ambient.intensity=0;ambient.ambient={.7f,.7f,.7f};b.point_light_=ambient;
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-4,0,0,4,0,0,-4,0,-8,4,0,-8};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    auto cube=std::make_shared<ScxGeometry>();*cube=*floor;
    cube->positions={-.5f,0,-2.5f,.5f,0,-2.5f,.5f,1,-2.5f,-.5f,1,-2.5f,-.5f,0,-3.5f,.5f,0,-3.5f,.5f,1,-3.5f,-.5f,1,-3.5f};
    cube->indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
    cube->normals.resize(24);cube->uvs.resize(16);
    for(size_t i=0;i<8;++i) {cube->normals[i*3]=0;cube->normals[i*3+1]=1;cube->normals[i*3+2]=0;}
    cube->groups[0].index_count=uint32_t(cube->indices.size());
    SceneInstance receiver,blocker;receiver.geometry=floor;blocker.geometry=cube;
    bx::mtxIdentity(receiver.transform.data());bx::mtxIdentity(blocker.transform.data());
    receiver.lighting.set=receiver.lighting.has_direction=true;receiver.lighting.direction[1]=-1;
    blocker.lighting=receiver.lighting;
    b.instances_={receiver,blocker};b.cache_scene_mesh(floor,nullptr,false);b.cache_scene_mesh(cube,nullptr,false);
    constexpr uint16_t width=160,height=120;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    float view[16],projection[16],render[16];
    bx::mtxLookAt(view,{3,3,-4},{0,0,3});bx::mtxProj(projection,60.f,float(width)/height,.05f,100.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    uint16_t draw_view=5;bool draw_blocker=true;
    auto capture=[&](int method,int quality) {
      host_enhanced_graphics().ao_method=method;host_enhanced_graphics().ao_quality=quality;
      b.update_ambient_occlusion(view,projection,width,height,5);b.draw_previews();
      bgfx::setViewFrameBuffer(draw_view,fb);bgfx::setViewRect(draw_view,0,0,width,height);
      bgfx::setViewClear(draw_view,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(draw_view,view,render);
      b.draw_instance(draw_view,receiver,receiver.transform.data(),false,0);
      if(draw_blocker) b.draw_instance(draw_view,blocker,blocker.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("AO readback timeout");
    };
    b.instances_={receiver};draw_blocker=false;
    const auto flat_plain=capture(0,1);
    for(int method=1;method<=2;++method) for(int quality=0;quality<3;++quality) {
      const auto flat=capture(method,quality);unsigned dark=0,visible=0;uint64_t total=0,reference=0;
      for(size_t i=0;i<flat.size();i+=4) if(flat_plain[i]>10) {
        ++visible;total+=flat[i];reference+=flat_plain[i];
        if(int(flat_plain[i])-int(flat[i])>3) ++dark;
      }
      std::printf("AO flat method=%d dark=%u/%u average=%g reference=%g\n",method,dark,visible,double(total)/visible,double(reference)/visible);
      SLRR_REQUIRE(visible>100 && dark==0);
    }
    // #274: a large flat wall (a house facade) alone and standing on the
    // floor, seen frontally and at a grazing angle, near and far. The AO
    // geometry path (#259) must match the reference sampling: an open wall
    // receives no AO, and the wall-floor crease looks the same.
    {
      auto wall=std::make_shared<ScxGeometry>();*wall=*floor;
      // Render space z = 10 (geometry z is mirrored), 30 m wide, 8 m high.
      wall->positions={-15,0,-10,15,0,-10,-15,8,-10,15,8,-10};
      wall->normals={0,0,1,0,0,1,0,0,1,0,0,1};
      b.cache_scene_mesh(wall,nullptr,false);
      SceneInstance facade=receiver;facade.geometry=wall;
      const auto saved_receiver_positions=floor->positions;
      floor->positions={-20,0,30,20,0,30,-20,0,-40,20,0,-40};
      ++floor->revision;b.cache_scene_mesh(floor,nullptr,false);
      struct Camera { const char* name; bx::Vec3 eye, at; };
      const Camera cameras[]={{"frontal near",{0,1.6f,6},{0,2.5f,10}},{"frontal far",{0,1.6f,-30},{0,2.5f,10}},
                              {"grazing near",{-12,1.6f,8.5f},{12,2.f,10}},{"grazing far",{-40,1.6f,6},{12,2.f,10}}};
      const auto saved_receiver=receiver;
      for(const bool with_floor:{false,true}) for(const auto& camera:cameras) {
        bx::mtxLookAt(view,camera.eye,camera.at);
        bx::mtxProj(projection,60.f,float(width)/height,.05f,200.f,bgfx::getCaps()->homogeneousDepth);
        std::copy(projection,projection+16,render);
        for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
        receiver=facade;b.instances_={facade};draw_blocker=false;
        if(with_floor) {b.instances_={facade,saved_receiver};blocker=saved_receiver;draw_blocker=true;}
        const auto plain=capture(0,2);
        for(int method=1;method<=2;++method) {
          render_debug().ao_reference=true;
          const auto reference=capture(method,2);
          render_debug().ao_reference=false;
          const auto fast=capture(method,2);
          unsigned visible=0,dark_fast=0,dark_ref=0,differ=0;int worst=0;
          for(size_t i=0;i<fast.size();i+=4) {
            if(plain[i]<=10) continue;
            ++visible;
            dark_fast+=int(plain[i])-int(fast[i])>3;dark_ref+=int(plain[i])-int(reference[i])>3;
            const int d=std::abs(int(fast[i])-int(reference[i]));worst=std::max(worst,d);differ+=d>8;
          }
          std::printf("AO wall %s%s method=%d: visible %u, AO pixels %u (reference %u), differ>8 %u, max %d\n",
                      camera.name,with_floor?" on floor":"",method,visible,dark_fast,dark_ref,differ,worst);
          SLRR_REQUIRE(visible>100);
          // Before #274 the open wall seen at a grazing angle got GTAO on
          // 2,817 of 8,257 pixels (reference 0) and HBAO on 7,439.
          if(!with_floor) SLRR_REQUIRE(dark_fast<=dark_ref+visible/200);
          SLRR_REQUIRE(differ<=visible/50);
        }
      }
      receiver=saved_receiver;blocker.geometry=cube;bx::mtxIdentity(blocker.transform.data());
      floor->positions=saved_receiver_positions;++floor->revision;b.cache_scene_mesh(floor,nullptr,false);
      bx::mtxLookAt(view,{3,3,-4},{0,0,3});
      bx::mtxProj(projection,60.f,float(width)/height,.05f,100.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(projection,projection+16,render);
      for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
      b.instances_={receiver};draw_blocker=false;
    }
    // Near-plane precision must not turn a distant, unobstructed road into AO bands.
    const auto near_positions=floor->positions;
    floor->positions={-300,0,0,300,0,0,-300,0,-1500,300,0,-1500};
    ++floor->revision;b.cache_scene_mesh(floor,nullptr,false);
    bx::mtxLookAt(view,{3,1.6f,-4},{0,1.6f,50});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,3000.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    const auto distant_plain=capture(0,2);
    for(int method=1;method<=2;++method) for(int quality=0;quality<3;++quality) {
      const auto image=capture(method,quality);unsigned dark=0,visible=0;
      for(size_t i=0;i<image.size();i+=4) if(distant_plain[i]>10) {
        ++visible;if(int(distant_plain[i])-int(image[i])>3) ++dark;
      }
      std::printf("AO distant road method=%d quality=%d false occlusion=%u/%u pixels\n",method,quality,dark,visible);
      SLRR_REQUIRE(visible>100 && dark==0);
    }
    floor->positions=near_positions;++floor->revision;b.cache_scene_mesh(floor,nullptr,false);
    bx::mtxLookAt(view,{3,3,-4},{0,0,3});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,100.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.instances_={receiver,blocker};draw_blocker=true;
    const auto plain=capture(0,1);
    std::vector<uint8_t> methods[2];
    for(int method=1;method<=2;++method) for(int quality=0;quality<3;++quality) {
      const auto image=capture(method,quality);unsigned shaded=0;
      for(size_t i=0;i<image.size();i+=4) if(int(plain[i])-int(image[i])>3) ++shaded;
      std::printf("AO method=%d quality=%d contact pixels=%u dimensions=%ux%u\n",method,quality,shaded,b.ao_width_,b.ao_height_);
      SLRR_REQUIRE(shaded>10 && shaded<width*height/2);
      SLRR_REQUIRE(b.ao_width_==width/2 && b.ao_height_==height/2);
      methods[method-1]=image;
    }
    // #248: High at half resolution against the old full-resolution High
    // (debug scale 2). The depth-aware upsample must keep the contact shading
    // and not spread AO across the blocker silhouette.
    for(int method=1;method<=2;++method) {
      const auto half=capture(method,2);
      render_debug().ao_resolution_scale=2.f;
      const auto full=capture(method,2);
      SLRR_REQUIRE(b.ao_width_==width && b.ao_height_==height);
      render_debug().ao_resolution_scale=1.f;
      unsigned half_contact=0,full_contact=0,differ=0;
      for(size_t i=0;i<half.size();i+=4) {
        half_contact+=int(plain[i])-int(half[i])>3;
        full_contact+=int(plain[i])-int(full[i])>3;
        differ+=std::abs(int(half[i])-int(full[i]))>12;
      }
      std::printf("AO method=%d High half vs full: contact %u vs %u pixels, %u pixels differ by >12\n",
                  method,half_contact,full_contact,differ);
      // Half resolution widens contact shading by about one half-res texel
      // (2 px); it must not create or lose it.
      SLRR_REQUIRE(half_contact>10 && half_contact*10>full_contact*6 && half_contact*10<full_contact*17);
      SLRR_REQUIRE(differ<width*height/50);
    }
    // #256: AO pass 1 samples the AO-resolution linear depth; the reference
    // path samples the full-resolution depth. The image must stay close.
    for(int method=1;method<=2;++method) for(int quality=0;quality<3;++quality) {
      render_debug().ao_reference=true;
      const auto reference=capture(method,quality);
      render_debug().ao_reference=false;
      const auto fast=capture(method,quality);
      int worst=0;unsigned differ=0,contact_ref=0,contact_fast=0;
      for(size_t i=0;i<fast.size();i+=4) {
        const int d=std::abs(int(fast[i])-int(reference[i]));
        worst=std::max(worst,d);differ+=d>8;
        contact_ref+=int(plain[i])-int(reference[i])>3;contact_fast+=int(plain[i])-int(fast[i])>3;
      }
      std::printf("AO depth method=%d quality=%d: max difference %d, %u pixels differ by >8, contact %u vs %u\n",
                  method,quality,worst,differ,contact_fast,contact_ref);
      SLRR_REQUIRE(differ<=width*height/100);
      SLRR_REQUIRE(contact_fast*10>=contact_ref*8 && contact_fast*10<=contact_ref*12);
    }
    // #248: GPU time of the AO passes at 2560x1440, per method and quality,
    // with High also forced to the old full resolution (scale 2).
    {
      bgfx::setDebug(BGFX_DEBUG_PROFILER);
      const auto ao_ms=[&](int method,int quality,float scale) {
        host_enhanced_graphics().ao_method=method;host_enhanced_graphics().ao_quality=quality;
        host_enhanced_graphics().ao_radius=300;  // Elias uses 3 m (#256)
        render_debug().ao_resolution_scale=scale;
        float big[16];bx::mtxProj(big,60.f,2560.f/1440.f,.05f,100.f,bgfx::getCaps()->homogeneousDepth);
        double pass1=0,all=0;int samples=0;
        for(int frame=0;frame<40;++frame) {
          b.update_ambient_occlusion(view,big,2560,1440,5);
          bgfx::frame();
          const auto* stats=bgfx::getStats();
          if(frame<15||stats->gpuTimerFreq<=0) continue;
          ++samples;
          for(uint16_t i=0;i<stats->numViews;++i) {
            const auto& v=stats->viewStats[i];
            const double ms=double(v.gpuTimeEnd-v.gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
            if(v.view==142) pass1+=ms;
            if(v.view>=140&&v.view<145) all+=ms;
          }
        }
        render_debug().ao_resolution_scale=1.f;
        return std::pair{pass1/std::max(samples,1),all/std::max(samples,1)};
      };
      for(int method=1;method<=2;++method) {
        const auto medium=ao_ms(method,1,1.f),high=ao_ms(method,2,1.f),old_high=ao_ms(method,2,2.f);
        std::printf("AO 2560x1440 %s pass1/all ms: Medium %.3f/%.3f, High (half) %.3f/%.3f, "
                    "High full-res (old) %.3f/%.3f\n",method==1?"HBAO":"GTAO",medium.first,medium.second,
                    high.first,high.second,old_high.first,old_high.second);
        SLRR_REQUIRE(high.first<old_high.first);
      }
      bgfx::setDebug(BGFX_DEBUG_NONE);
      b.destroy_ao_targets();
    }
    // #256: GTAO High at 2560x1440, 3 m radius, on a street-like scene that
    // fills the screen (a 400 m ground and 400 boxes), per AO view.
    {
      auto ground=std::make_shared<ScxGeometry>();*ground=*floor;
      ground->positions={-200,0,200,200,0,200,-200,0,-200,200,0,-200};++ground->revision;
      b.cache_scene_mesh(ground,nullptr,false);
      SceneInstance street;street.geometry=ground;bx::mtxIdentity(street.transform.data());
      std::vector<SceneInstance> scene{street};
      for(int i=0;i<400;++i) {
        SceneInstance box;box.geometry=cube;bx::mtxIdentity(box.transform.data());
        box.transform[12]=float((i*37)%41)-20.f;box.transform[14]=float((i*53)%97)+3.f;
        box.transform[0]=box.transform[10]=1.5f;box.transform[5]=2.f+float(i%5);
        scene.push_back(box);
      }
      const auto saved_instances=b.instances_;
      b.instances_=scene;
      float street_view[16],street_proj[16];
      bx::mtxLookAt(street_view,{0,1.6f,0},{0,1.2f,10});
      bx::mtxProj(street_proj,60.f,2560.f/1440.f,.05f,500.f,bgfx::getCaps()->homogeneousDepth);
      bgfx::setDebug(BGFX_DEBUG_PROFILER);
      host_enhanced_graphics().ao_method=2;host_enhanced_graphics().ao_quality=2;
      host_enhanced_graphics().ao_radius=300;
      std::map<uint16_t,double> per_view;int samples=0;
      for(int frame=0;frame<60;++frame) {
        b.update_ambient_occlusion(street_view,street_proj,2560,1440,5);
        bgfx::frame();
        const auto* stats=bgfx::getStats();
        if(frame<20||stats->gpuTimerFreq<=0) continue;
        ++samples;
        for(uint16_t i=0;i<stats->numViews;++i) {
          const auto& v=stats->viewStats[i];
          if(v.view>=140&&v.view<146)
            per_view[v.view]+=double(v.gpuTimeEnd-v.gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
        }
      }
      std::printf("AO street GTAO High 2560x1440 radius 3 m (ms):");
      for(const auto& [id,ms]:per_view) std::printf(" view %u %.3f",unsigned(id),ms/std::max(samples,1));
      std::printf("\n");
      bgfx::setDebug(BGFX_DEBUG_NONE);
      b.destroy_ao_targets();
      b.instances_=saved_instances;
      host_enhanced_graphics().ao_radius=100;
    }
    SLRR_REQUIRE(methods[0]!=methods[1]);
    b.instances_={receiver};
    SceneBatch batch;batch.geometry=cube;SceneBatchInstance placement;bx::mtxIdentity(placement.transform.data());
    batch.instances.push_back(placement);b.prop_batches_={batch};
    const auto instanced=capture(2,1);unsigned prop_shaded=0;
    for(size_t i=0;i<instanced.size();i+=4) if(int(plain[i])-int(instanced[i])>3) ++prop_shaded;
    SLRR_REQUIRE(prop_shaded>10);b.prop_batches_.clear();b.instances_={receiver,blocker};
    std::printf("AO reversed-depth instanced blocker contact=%u pixels\n",prop_shaded);

    draw_view=72;SLRR_REQUIRE(capture(2,1)==plain);draw_view=5;
    // AO can only affect ambient diffuse: retain exactly the same direct light.
    b.point_light_.reset();b.garage_=false;b.outdoor_lighting_=true;
    receiver.lighting.ambient[0]=receiver.lighting.ambient[1]=receiver.lighting.ambient[2]=0;
    blocker.lighting=receiver.lighting;
    const auto direct=capture(0,1);SLRR_REQUIRE(capture(2,1)==direct);
    b.update_ambient_occlusion(view,projection,80,60,5);
    SLRR_REQUIRE(b.ao_screen_width_==80 && b.ao_width_==40);
    host_enhanced_graphics().ao_method=0;b.update_ambient_occlusion(view,projection,width,height,5);
    SLRR_REQUIRE(!b.ao_active_ && !bgfx::isValid(b.ao_depth_fb_) && !bgfx::isValid(b.ao_raw_fb_));
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;b.instances_.clear();b.garage_=b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&root);scene_instances_set_resources(nullptr);
    std::puts("PASS HBAO/GTAO quality/contact, indirect-only, reflection isolation, resize and off cleanup");
  }
  // #237: a street lamp from the scenery light registry lights the road at
  // night through the production fs_mesh path, not by day, and option 4
  // switches it off. Fake (glow-only) and non-city groups never light.
  // #279: headlight parts cast forward spot beams on the road at night, brake
  // light parts light the road behind red, and a removed headlight takes its
  // own beam away. The lights come from the harness hook in the shape
  // part_car_lights() reports them (render space); tests/test_car_lights.cpp
  // checks which parts own them and when they are lit.
  static void car_lights(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world;gameref_upsert(&world)->flags=kWorldTreeRoot;
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};
    float view[16],projection[16],render[16];
    constexpr uint16_t width=160,height=120;
    bx::mtxLookAt(view,{0,14,-26},{0,0,4});bx::mtxProj(projection,60.f,float(width)/height,.05f,200.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.cull_eye_[0]=0;b.cull_eye_[1]=14;b.cull_eye_[2]=26;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-30,0,40,30,0,40,-30,0,-30,30,0,-30};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.instances_={road};b.cache_scene_mesh(floor,nullptr,false);
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    auto capture=[&]() {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      b.draw_instance(5,road,road.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("car light readback timeout");
    };
    // Road pixel of a render-space ground point.
    const auto pixel=[&](float x,float z) {
      float vp[16];bx::mtxMul(vp,view,projection);
      const float in[4]={x,0,z,1};float clip[4];bx::vec4MulMtx(clip,in,vp);
      const int px=int((clip[0]/clip[3]*.5f+.5f)*width);
      int py=int((1.f-(clip[1]/clip[3]*.5f+.5f))*height);
      if(!bgfx::getCaps()->originBottomLeft) {} else py=height-1-py;
      SLRR_REQUIRE(px>=0&&px<width&&py>=0&&py<height);
      return (size_t(py)*width+px)*4;
    };
    // Road luminance over a ground patch (sum of r+g+b over 5x5 metres).
    const auto patch=[&](const std::vector<uint8_t>& image,float x,float z,int channel) {
      long sum=0;
      for(float dz=-2;dz<=2.01f;dz+=1) for(float dx=-2;dx<=2.01f;dx+=1) {
        const auto at=pixel(x+dx,z+dz);
        sum+=channel<0?long(image[at])+image[at+1]+image[at+2]:long(image[at+channel]);
      }
      return sum;
    };
    // A car at the origin facing +Z (render space): headlight parts 1.9 m
    // ahead, 0.7 m up, beams pitched 5 degrees down; taillights 2 m behind.
    const float pitch=.087f;
    const auto headlight=[&](float x) {
      CarLight l;l.kind=CarLightKind::Headlight;l.part=reinterpret_cast<Object*>(uintptr_t(0x100+int(x*10)));
      l.position={x,.7f,1.9f};l.direction={0,-std::sin(pitch),std::cos(pitch)};
      l.diffuse={.2f,.2f,.2f};l.attenuation={1,.03f,0};l.range=400;l.theta=1.896f;l.phi=2.02f;return l;
    };
    const auto brake=[&](float x) {
      CarLight l;l.kind=CarLightKind::Brake;l.part=reinterpret_cast<Object*>(uintptr_t(0x200+int(x*10)));
      l.position={x,.8f,-2.f};l.direction={0,0,-1};
      l.diffuse={1,0,0};l.attenuation={1,.03f,0};l.range=5;return l;
    };
    scene_sun_set_time(22*3600.f);
    b.car_lights_override_=std::vector<CarLight>{};b.update_local_light();
    SLRR_REQUIRE(b.scenery_lamps_.empty() && b.car_lamp_count_==0);
    const auto unlit=capture();
    b.car_lights_override_=std::vector<CarLight>{headlight(-.7f),headlight(.7f)};b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==2 && b.scenery_lamps_.size()==2);
    SLRR_REQUIRE(b.scenery_lamps_[0].cos_outer>-1.f && b.scenery_lamps_[0].range==LibBgfxBackend::kHeadlightRange);
    const auto both=capture();
    b.car_lights_override_=std::vector<CarLight>{headlight(.7f)};b.update_local_light();
    const auto one=capture();
    b.car_lights_override_=std::vector<CarLight>{brake(-.6f),brake(.6f)};b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==2);
    const auto braking=capture();
    const long ahead0=patch(unlit,0,10,-1),ahead2=patch(both,0,10,-1),ahead1=patch(one,0,10,-1);
    const long far0=patch(unlit,0,24,-1),far2=patch(both,0,24,-1);
    const long rear0=patch(unlit,0,-9,-1),rear2=patch(both,0,-9,-1);
    const long red0=patch(unlit,0,-3.5f,0),red=patch(braking,0,-3.5f,0);
    const long green0=patch(unlit,0,-3.5f,1),green=patch(braking,0,-3.5f,1);
    const long front_red=patch(braking,0,10,-1);
    std::printf("CAR LIGHTS road 10 m ahead: unlit %ld, two headlights %ld, one %ld; 24 m ahead %ld -> %ld; "
                "9 m behind %ld -> %ld; brake red 3.5 m behind %ld -> %ld (green %ld -> %ld), ahead while braking %ld\n",
                ahead0,ahead2,ahead1,far0,far2,rear0,rear2,red0,red,green0,green,front_red);
    // Beams light the road ahead, a lost headlight takes about half away,
    // nothing reaches behind the car; brake lights add red only, behind.
    // The road 24 m ahead lies above these 5-degree-down beams: the low-beam
    // cutoff and the quadratic falloff leave it a little light.
    SLRR_REQUIRE(ahead2>ahead0+25*60 && far2>far0+25*5 && far2-far0<(ahead2-ahead0)/3);
    SLRR_REQUIRE(ahead1>ahead0+(ahead2-ahead0)/4 && ahead1<ahead2-(ahead2-ahead0)/4);
    SLRR_REQUIRE(rear2==rear0);
    SLRR_REQUIRE(red>red0+25*20 && green-green0<(red-red0)/8 && front_red==ahead0);
    // Debug toggle off: the stock image, no dynamic car lights.
    render_debug().car_lights=false;
    b.car_lights_override_=std::vector<CarLight>{headlight(-.7f),headlight(.7f),brake(0)};b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==0);
    SLRR_REQUIRE(capture()==unlit);
    render_debug().car_lights=true;
    // Local lights (option 4) off: none either.
    host_enhanced_graphics().local_lights=false;b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==0 && b.scenery_lamps_.empty());
    host_enhanced_graphics().local_lights=true;
    b.car_lights_override_.reset();b.scenery_lamps_.clear();
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.instances_.clear();b.fixture_lights_.clear();b.outdoor_lighting_=false;b.car_lamp_count_=0;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&world);
    std::puts("PASS car lights: headlight beams, brake light, one beam per headlight part");
  }

  // #304: actual stock FakeSpot artwork, viewed from its authored cone.
  // This exercises the primitive; the video.flares switch is owned by flush.
  static void nitro_flares(LibBgfxBackend& b) {
    Fixture f;f.packs.set_root(fs::absolute("game").string());
    const int particles=f.packs.open_lib("particles.rpk");SLRR_REQUIRE(particles>0);
    const auto light=load_light_resource(f.packs,RpkIndex::make_full_id(particles,0x1f));
    SLRR_REQUIRE(light && light->type==LightResourceType::FakeSpot && light->flare.texture);
    SLRR_REQUIRE(light->flare_texture_mode==2 && light->flare.max_size==10);
    scene_instances_set_resources(&f.packs);
    auto glow_path=scene_instances_texture(light->flare.texture);
    if(glow_path.empty())glow_path=light->flare_texture_path;
    SLRR_REQUIRE(!glow_path.empty());
    const auto glow_texture=b.tex_for_osd_icon(glow_path);
    std::printf("NITRO resource: texture=0x%x path=%s gpu=%u white=%u range=%g phi=%g\n",
                light->flare.texture,glow_path.c_str(),unsigned(glow_texture.idx),unsigned(b.white_tex_.idx),light->range,light->phi);
    SLRR_REQUIRE(glow_texture.idx!=b.white_tex_.idx);
    Object world;gameref_upsert(&world)->flags=kWorldTreeRoot;
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    const auto saved=host_enhanced_graphics();const bool debug_lights=render_debug().car_lights;
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();b.instances_.clear();
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};
    render_debug().car_lights=true;host_enhanced_graphics().local_lights=true;
    CarLight nitro;nitro.kind=CarLightKind::Nitro;nitro.light=*light;
    // Identity script frame into render space, exactly as part_car_lights:
    // reflect Z on position and direction, with no additional rotation.
    nitro.position={light->position[0],light->position[1],-light->position[2]};
    nitro.direction={light->direction[0],light->direction[1],-light->direction[2]};
    b.car_lights_override_=std::vector<CarLight>{nitro};b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==0 && b.scenery_lamps_.empty());
    auto mesh=std::make_shared<ScxGeometry>();mesh->info.ok=true;mesh->info.version=4;mesh->position_scale=1;
    mesh->positions={-2,-2,-1.5f,2,-2,-1.5f,-2,2,-1.5f,2,2,-1.5f};
    mesh->normals={0,0,1,0,0,1,0,0,1,0,0,1};mesh->uvs.resize(8);mesh->indices={0,1,2,1,3,2};
    ScxGroup group;group.index_count=6;group.material_flags=0x02000000u;mesh->groups={group};
    SceneInstance wall;wall.geometry=mesh;wall.cull_backfaces=true;wall.color=0;
    bx::mtxIdentity(wall.transform.data());b.cache_scene_mesh(mesh,nullptr,false);
    constexpr uint16_t width=160,height=120;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    auto capture=[&](float camera_z,bool occluded,bool glow) {
      float view[16],projection[16],render[16];
      bx::mtxLookAt(view,{0,0,camera_z},{0,0,0});
      bx::mtxProj(projection,60.f,float(width)/height,.05f,100.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(projection,projection+16,render);
      for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);b.set_view_sequential(5);bgfx::touch(5);
      if(occluded)b.draw_instance(5,wall,wall.transform.data(),false,0);
      if(glow)b.draw_nitro_flares(view,render,5);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i)if(bgfx::frame()>=ready)return bytes;
      throw std::runtime_error("nitro flare readback timeout");
    };
    const auto dark=capture(3,false,false),visible=capture(3,false,true);
    unsigned glow_pixels=0;uint64_t energy=0;
    for(size_t i=0;i<visible.size();i+=4) {
      const unsigned rgb=unsigned(visible[i])+visible[i+1]+visible[i+2];
      energy+=rgb;glow_pixels+=rgb>3;
    }
    std::printf("NITRO visible readback: pixels=%u energy=%llu direction=(%g,%g,%g)\n",
                glow_pixels,static_cast<unsigned long long>(energy),nitro.direction[0],nitro.direction[1],nitro.direction[2]);
    SLRR_REQUIRE(glow_pixels>10 && energy>100);
    SLRR_REQUIRE(capture(3,true,true)==capture(3,true,false));
    SLRR_REQUIRE(capture(-3,false,true)==dark);  // opposite authored cone
    host_enhanced_graphics().local_lights=false;b.update_local_light();
    SLRR_REQUIRE(b.car_lamp_count_==0 && b.scenery_lamps_.empty());
    SLRR_REQUIRE(capture(3,false,true)==visible);  // optical, not option-4 illumination
    b.car_lights_override_=std::vector<CarLight>{};
    SLRR_REQUIRE(capture(3,false,true)==dark);  // runtime deactivation removes glow
    std::printf("NITRO authored glow: pixels=%u energy=%llu; depth/cone/deactivation occluded, no road light, local-light option independent\n",
                glow_pixels,static_cast<unsigned long long>(energy));
    // Independently exercise the real particles:1E SCX3/PNG vehicle material;
    // no timing, FakeSpot cone or halo is involved in this mesh pixel test.
    SLRR_REQUIRE(f.packs.open_lib("MW_Mod.rpk")>0);
    const auto flame=scene_instances_geometry(RpkIndex::make_full_id(particles,0x1e));
    SLRR_REQUIRE(flame && flame->info.version==3 && !flame->groups.empty() && !flame->textures.empty());
    SLRR_REQUIRE(flame->groups[0].material_flags==0x101u && flame->groups[0].material_type==0);
    SLRR_REQUIRE(scx_surface(flame->info.version,flame->groups[0]).blend);
    SceneInstance exhaust;exhaust.geometry=flame;exhaust.vehicle_material=true;exhaust.light_flags=256;
    bx::mtxIdentity(exhaust.transform.data());b.cache_scene_mesh(flame,nullptr,false);
    const auto& uploaded=b.instance_meshes_.at(flame.get());
    SLRR_REQUIRE(!uploaded.tex.empty() && bgfx::isValid(uploaded.tex[0]) && uploaded.tex[0].idx!=b.white_tex_.idx);
    auto card=std::make_shared<ScxGeometry>(*flame);card->textures.clear();
    for(auto& g:card->groups) {
      g.texture_index=g.mix_index=g.reflection_index=0xffff;
      std::fill(g.diffuse,g.diffuse+4,1.f);
    }
    SceneInstance silhouette=exhaust;silhouette.geometry=card;b.cache_scene_mesh(card,nullptr,false);
    const float k=flame->position_scale;
    const bx::Vec3 centre{(flame->info.bbox_min[0]+flame->info.bbox_max[0])*.5f*k,
                           (flame->info.bbox_min[1]+flame->info.bbox_max[1])*.5f*k,
                          -(flame->info.bbox_min[2]+flame->info.bbox_max[2])*.5f*k};
    const float radius=.5f*k*std::max({flame->info.bbox_max[0]-flame->info.bbox_min[0],
        flame->info.bbox_max[1]-flame->info.bbox_min[1],flame->info.bbox_max[2]-flame->info.bbox_min[2]});
    SLRR_REQUIRE(radius>0);
    float flame_view[16],flame_projection[16],flame_render[16],camera[16];
    bx::mtxLookAt(flame_view,bx::add(centre,{2.f*radius,.7f*radius,2.f*radius}),centre);
    bx::mtxProj(flame_projection,60.f,float(width)/height,.01f,100.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(flame_projection,flame_projection+16,flame_render);
    for(int i:{2,6,10,14})flame_render[i]=bgfx::getCaps()->homogeneousDepth?-flame_render[i]:flame_render[i+1]-flame_render[i];
    bx::mtxInverse(camera,flame_view);
    auto back_mesh=std::make_shared<ScxGeometry>(*mesh);
    back_mesh->positions={-2,-2,0,2,-2,0,-2,2,0,2,2,0};
    SceneInstance backdrop=wall;backdrop.geometry=back_mesh;backdrop.color=0x203040;
    std::copy(camera,camera+16,backdrop.transform.begin());
    const float centre_xyz[3]={centre.x,centre.y,centre.z};
    for(int i=0;i<3;++i) {
      backdrop.transform[i]*=4.f*radius;backdrop.transform[4+i]*=4.f*radius;
      backdrop.transform[12+i]=centre_xyz[i]+camera[8+i]*4.f*radius;
    }
    b.cache_scene_mesh(back_mesh,nullptr,false);
    auto mesh_capture=[&](int mode) {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,0x203040ff,0.f);
      bgfx::setViewTransform(5,flame_view,flame_render);b.set_view_sequential(5);bgfx::touch(5);
      if(mode==1 || mode==4)b.draw_instance(5,exhaust,exhaust.transform.data(),false,1);
      if(mode==2)b.draw_instance(5,exhaust,exhaust.transform.data(),false,0);
      if(mode==3)b.draw_instance(5,silhouette,silhouette.transform.data(),false,1);
      if(mode==4 || mode==5)b.draw_instance(5,backdrop,backdrop.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i)if(bgfx::frame()>=ready)return bytes;
      throw std::runtime_error("nitro flame mesh readback timeout");
    };
    const auto background=mesh_capture(0),textured=mesh_capture(1),footprint=mesh_capture(3);
    unsigned flame_pixels=0,card_pixels=0,transparent_pixels=0;
    for(size_t i=0;i<background.size();i+=4) {
      const bool actual=std::abs(int(textured[i])-int(background[i]))>2 ||
          std::abs(int(textured[i+1])-int(background[i+1]))>2 || std::abs(int(textured[i+2])-int(background[i+2]))>2;
      const bool covered=footprint[i]!=background[i] || footprint[i+1]!=background[i+1] || footprint[i+2]!=background[i+2];
      flame_pixels+=actual;card_pixels+=covered;transparent_pixels+=covered && !actual;
    }
    std::printf("NITRO actual SCX3/PNG: radius=%g textured=%u card=%u transparent-background=%u\n",
                radius,flame_pixels,card_pixels,transparent_pixels);
    SLRR_REQUIRE(flame_pixels>10 && transparent_pixels>10 && flame_pixels<card_pixels);
    SLRR_REQUIRE(mesh_capture(2)==background);  // blend group never enters opaque pass
    SLRR_REQUIRE(mesh_capture(4)==mesh_capture(5));  // no flame depth writes, even at bright texels
    b.car_lights_override_.reset();bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;render_debug().car_lights=debug_lights;
    b.instances_.clear();b.scenery_lamps_.clear();b.car_lamp_count_=0;b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);gameref_remove(&world);
  }

  // #281: emissive overlays (night facade masks, traffic-light glass) sit
  // flush with the surface they light. They used to be pushed 2 mm along
  // their normal, so glass authored behind or level with a body floated in
  // front of it. A coplanar overlay with another tessellation must still win
  // every depth tie against its surface, seen at a slant.
  static void emissive_overlay_flush(LibBgfxBackend& b) {
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    host_enhanced_graphics().ao_method=0;
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};
    b.scenery_lamps_.clear();b.fixture_lights_.clear();
    scene_sun_set_time(12*3600.f);b.update_local_light();
    constexpr uint16_t width=160,height=120;
    float view[16],projection[16],render[16];
    bx::mtxLookAt(view,{3.f,1.f,-5.f},{0,0,0});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,500.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    // A 4 x 4 m wall in z = 0 facing the camera, as one quad;
    // the glass is the same plane as an 8 x 8 grid with the other diagonal.
    const auto plane=[](int cells,bool flip,float emission) {
      auto g=std::make_shared<ScxGeometry>();g->info.ok=true;g->info.version=4;g->position_scale=1;
      for(int y=0;y<=cells;++y) for(int x=0;x<=cells;++x) {
        g->positions.insert(g->positions.end(),{-2.f+4.f*x/cells,-2.f+4.f*y/cells,0.f});
        // +Z here is toward the camera once drawn (render space flips Z).
        g->normals.insert(g->normals.end(),{0.f,0.f,1.f});g->uvs.insert(g->uvs.end(),{0.f,0.f});
      }
      for(int y=0;y<cells;++y) for(int x=0;x<cells;++x) {
        const uint32_t a=uint32_t(y*(cells+1)+x),c=a+1,d=a+uint32_t(cells+1),e=d+1;
        if(flip) g->indices.insert(g->indices.end(),{a,d,c,c,d,e});
        else g->indices.insert(g->indices.end(),{a,d,e,a,e,c});
      }
      ScxGroup group;group.index_count=uint32_t(g->indices.size());
      group.emission_color[0]=group.emission_color[1]=group.emission_color[2]=emission;
      g->groups={group};return g;
    };
    const auto wall_mesh=plane(1,false,0.f),glass_mesh=plane(8,true,1.f);
    SceneInstance wall;wall.geometry=wall_mesh;bx::mtxIdentity(wall.transform.data());
    wall.lighting.set=wall.lighting.has_direction=true;wall.lighting.direction[2]=1;
    for(int i=0;i<3;++i){wall.lighting.diffuse[i]=.3f;wall.lighting.ambient[i]=.1f;wall.lighting.specular[i]=0;}
    b.cache_scene_mesh(wall_mesh,nullptr,false);b.cache_scene_mesh(glass_mesh,nullptr,false);
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    // offset: glass position along +Z (away from the camera) in metres;
    // nan draws no glass. with_wall false draws the glass alone.
    auto capture=[&](float offset,bool with_wall=true) {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      bgfx::setViewMode(5,bgfx::ViewMode::Sequential);  // the wall's depth first, as in the world view
      if(with_wall) b.draw_instance(5,wall,wall.transform.data(),false,0);
      if(!std::isnan(offset)) {
        SceneInstance glass=wall;glass.geometry=glass_mesh;glass.emissive_overlay=true;
        glass.transform[14]=offset;
        b.draw_instance(5,glass,glass.transform.data(),false,1);
      }
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("overlay readback timeout");
    };
    const auto bare=capture(std::nanf(""));
    const auto behind=capture(.0015f);  // 1.5 mm behind the wall
    const auto level=capture(0.f);
    const auto front=capture(-.0015f);
    // The glass drawn alone covers exactly the wall's pixels.
    const auto alone=capture(0.f,false);
    size_t wall_pixels=0,lit_level=0,lit_behind=0,lit_front=0,edge_misses=0;
    int weakest_level=255;
    const auto inside=[&](int x,int y){return x>=0&&y>=0&&x<width&&y<height&&alone[(size_t(y)*width+x)*4]>=200;};
    for(size_t i=0;i<bare.size();i+=4) {
      const int x=int(i/4%width),y=int(i/4/width);
      if(!inside(x,y)) continue;
      // Silhouette pixels: the two tessellations cover them differently.
      if(!inside(x-1,y)||!inside(x+1,y)||!inside(x,y-1)||!inside(x,y+1)) {edge_misses+=level[i]<=bare[i]+60;continue;}
      ++wall_pixels;
      lit_level+=level[i]>bare[i]+60;lit_behind+=behind[i]>bare[i]+60;lit_front+=front[i]>bare[i]+60;
      weakest_level=std::min(weakest_level,int(level[i])-int(bare[i]));
    }
    std::printf("EMISSIVE OVERLAY wall %zu px: glass level lit %zu (weakest gain %d), 1.5 mm in front %zu, 1.5 mm behind %zu; edge misses %zu\n",
                wall_pixels,lit_level,weakest_level,lit_front,lit_behind,edge_misses);
    SLRR_REQUIRE(wall_pixels>4000);
    // Flush and in front: every wall pixel shows the glass (no depth ties lost).
    SLRR_REQUIRE(lit_level==wall_pixels && lit_front==wall_pixels);
    // Behind the surface: hidden, as authored.
    SLRR_REQUIRE(lit_behind==0);
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);b.outdoor_lighting_=false;
    b.instances_.clear();
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    std::puts("PASS emissive overlays sit flush: level glass shows, glass behind the surface stays hidden");
  }

  // #283: headlight beams cast shadows. A box 8 m ahead of the car darkens
  // the road behind it only with headlight shadows on; the road beside it is
  // unchanged. The followed car's headlights take the first tiles, the
  // budget caps the rest. Cost per frame with six tiles.
  static void headlight_shadows(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world;gameref_upsert(&world)->flags=kWorldTreeRoot;
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};
    float view[16],projection[16],render[16];
    constexpr uint16_t width=160,height=120;
    bx::mtxLookAt(view,{0,14,-26},{0,0,4});bx::mtxProj(projection,60.f,float(width)/height,.05f,200.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.cull_eye_[0]=0;b.cull_eye_[1]=14;b.cull_eye_[2]=26;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-30,0,40,30,0,40,-30,0,-30,30,0,-30};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    floor->ground_polygons=true;  // a receiver, like the city ground
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    // A 1.2 x 1.5 x 0.4 m box standing on the road 8 m ahead.
    auto box=std::make_shared<ScxGeometry>();box->info.ok=true;box->info.version=4;box->position_scale=1;
    for(int c=0;c<8;++c) box->positions.insert(box->positions.end(),{c&1?.6f:-.6f,c&2?1.5f:0.f,c&4?.2f:-.2f});
    for(int c=0;c<8;++c) box->normals.insert(box->normals.end(),{0.f,1.f,0.f});
    box->uvs.resize(16);
    box->indices={0,1,3,0,3,2, 4,6,7,4,7,5, 0,4,5,0,5,1, 2,3,7,2,7,6, 0,2,6,0,6,4, 1,5,7,1,7,3};
    {ScxGroup g;g.index_count=uint32_t(box->indices.size());box->groups={g};}
    SceneInstance block=road;block.geometry=box;block.transform[14]=8.f;
    b.instances_={road,block};b.cache_scene_mesh(floor,nullptr,false);b.cache_scene_mesh(box,nullptr,false);
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    // Tiles are views 145+, after this test's view 5: draw them in the
    // frame before the capture (the game orders them before the world).
    auto capture=[&]() {
      bgfx::frame();
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      b.draw_instance(5,road,road.transform.data(),false,0);
      b.draw_instance(5,block,block.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("headlight shadow readback timeout");
    };
    const auto pixel=[&](float x,float z) {
      float vp[16];bx::mtxMul(vp,view,projection);
      const float in[4]={x,0,z,1};float clip[4];bx::vec4MulMtx(clip,in,vp);
      const int px=int((clip[0]/clip[3]*.5f+.5f)*width);
      int py=int((1.f-(clip[1]/clip[3]*.5f+.5f))*height);
      if(bgfx::getCaps()->originBottomLeft) py=height-1-py;
      SLRR_REQUIRE(px>=0&&px<width&&py>=0&&py<height);
      return (size_t(py)*width+px)*4;
    };
    const auto patch=[&](const std::vector<uint8_t>& image,float x,float z) {
      long sum=0;
      for(float dz=-1;dz<=1.01f;dz+=.5f) for(float dx=-.5f;dx<=.51f;dx+=.5f) {
        const auto at=pixel(x+dx,z+dz);sum+=long(image[at])+image[at+1]+image[at+2];
      }
      return sum;
    };
    Object own_car,other_car;
    const auto headlight=[&](float x,float z,const Object* car) {
      CarLight l;l.kind=CarLightKind::Headlight;l.car=const_cast<Object*>(car);
      l.part=reinterpret_cast<Object*>(uintptr_t(0x1000+int(x*10)+int(z*100)));
      l.position={x,.7f,z};l.direction={0,-.087f,.996f};
      l.diffuse={.2f,.2f,.2f};l.attenuation={1,.03f,0};l.range=400;return l;
    };
    VideoOptions video;video.shadows=2;
    scene_sun_set_time(22*3600.f);
    b.set_headlight_focus(&own_car);
    b.car_lights_override_=std::vector<CarLight>{headlight(-.7f,1.9f,&own_car),headlight(.7f,1.9f,&own_car)};
    render_debug().headlight_shadows=false;
    b.update_local_light();b.update_headlight_shadows(video);
    SLRR_REQUIRE(b.headlight_shadow_count_==0);
    const auto open=capture();
    render_debug().headlight_shadows=true;
    b.update_local_light();b.update_headlight_shadows(video);
    std::printf("HEADLIGHT SHADOW tiles %zu (lamp tiles %d %d)\n",b.headlight_shadow_count_,
                b.scenery_lamps_.size()>1?b.scenery_lamps_[0].shadow_tile:-9,b.scenery_lamps_.size()>1?b.scenery_lamps_[1].shadow_tile:-9);
    SLRR_REQUIRE(b.headlight_shadow_count_==2 && b.scenery_lamps_.size()==2);
    SLRR_REQUIRE(b.scenery_lamps_[0].shadow_tile>=0 && b.scenery_lamps_[1].shadow_tile>=0);
    const auto shadowed=capture();
    const long behind_open=patch(open,0,14),behind_shadow=patch(shadowed,0,14);
    const long beside_open=patch(open,3.5f,14),beside_shadow=patch(shadowed,3.5f,14);
    const long front_open=patch(open,0,5),front_shadow=patch(shadowed,0,5);
    std::printf("HEADLIGHT SHADOW road behind the box %ld -> %ld, beside it %ld -> %ld, in front %ld -> %ld\n",
                behind_open,behind_shadow,beside_open,beside_shadow,front_open,front_shadow);
    SLRR_REQUIRE(behind_shadow<behind_open-(behind_open/3));
    SLRR_REQUIRE(std::abs(beside_shadow-beside_open)<=beside_open/20+15);
    SLRR_REQUIRE(std::abs(front_shadow-front_open)<=front_open/20+15);
    // Budget: the followed car's two headlights first, even when farther;
    // six tiles in all.
    std::vector<CarLight> crowd;
    for(int k=0;k<4;++k) {crowd.push_back(headlight(-.7f,1.9f+k*3.f,&other_car));crowd.push_back(headlight(.7f,1.9f+k*3.f,&other_car));}
    crowd.push_back(headlight(-.7f,-20.f,&own_car));crowd.push_back(headlight(.7f,-20.f,&own_car));
    b.car_lights_override_=crowd;b.update_local_light();b.update_headlight_shadows(video);
    size_t own_tiles=0,tiles=0;
    for(const auto& lamp:b.scenery_lamps_) {tiles+=lamp.shadow_tile>=0;own_tiles+=lamp.shadow_tile>=0&&lamp.car==&own_car;}
    std::printf("HEADLIGHT SHADOW budget: %zu tiles, %zu on the followed car (of %zu headlights)\n",tiles,own_tiles,b.scenery_lamps_.size());
    SLRR_REQUIRE(tiles==LibBgfxBackend::kHeadlightShadows && own_tiles==2 && b.headlight_shadow_count_==tiles);
    // Selection cost with six tiles; the GPU and render-thread cost is
    // measured in the city (city_submission, workers case).
    double cpu=0;
    for(int frame=0;frame<60;++frame) {
      const auto t0=std::chrono::steady_clock::now();
      b.update_local_light();b.update_headlight_shadows(video);
      cpu+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
      bgfx::frame();
    }
    std::printf("HEADLIGHT SHADOW selection and submission, 6 tiles, 2 casters: CPU %.3f ms per frame\n",cpu/60.0);
    // Shadows off (option) or local lights off: none.
    video.shadows=0;b.update_local_light();b.update_headlight_shadows(video);
    SLRR_REQUIRE(b.headlight_shadow_count_==0);
    video.shadows=2;
    b.car_lights_override_.reset();b.set_headlight_focus(nullptr);b.scenery_lamps_.clear();b.update_headlight_shadows(video);
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.instances_.clear();b.fixture_lights_.clear();b.outdoor_lighting_=false;b.car_lamp_count_=0;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&world);
    std::puts("PASS headlight shadows: a caster darkens the road behind it, budget favours the followed car");
  }

  static void scenery_lamps(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);
    gameref_set_matrix(&shell,0,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    SceneryLight lamp;
    lamp.position={0,7.8f,0};lamp.color={.972f,.888f,.615f};lamp.range=500;
    lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
    lights->push_back(lamp);
    SceneryLight glow=lamp;glow.lights_scene=false;glow.position={6,5.5f,0};lights->push_back(glow);
    SceneryLight brake=lamp;brake.group=1;brake.position={-6,5.5f,0};lights->push_back(brake);
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};
    float view[16],projection[16],render[16];
    constexpr uint16_t width=160,height=120;
    bx::mtxLookAt(view,{0,14,-26},{0,0,0});bx::mtxProj(projection,60.f,float(width)/height,.05f,200.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    // sync_instances state: script-space eye, render-space view/projection.
    b.cull_eye_[0]=0;b.cull_eye_[1]=14;b.cull_eye_[2]=26;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    // A second real lamp 80 m behind the camera: its 45 m sphere never
    // reaches the view, so it takes no slot.
    {
      auto more=std::make_shared<std::vector<SceneryLight>>(*lights);
      SceneryLight behind=lamp;behind.position={0,7.8f,106};more->push_back(behind);
      lights=more;SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    }
    scene_sun_set_time(22*3600.f);b.update_local_light();
    SLRR_REQUIRE(b.scenery_lamps_.size()==1 && b.fixture_lights_.empty());
    const auto street=b.scenery_lamps_.front();
    SLRR_REQUIRE(street.valid() && std::abs(street.position[1]-7.8f)<1e-4f && std::abs(street.position[2])<1e-4f);
    SLRR_REQUIRE(street.range==kSceneryLightMaxRange && street.intensity==kSceneryLightIntensity);
    SLRR_REQUIRE(street.attenuation[1]==.03f && street.color[2]==.615f);
    host_enhanced_graphics().local_lights=false;b.update_local_light();
    SLRR_REQUIRE(b.scenery_lamps_.empty());
    host_enhanced_graphics().local_lights=true;
    scene_sun_set_time(12*3600.f);b.update_local_light();
    SLRR_REQUIRE(b.scenery_lamps_.empty());
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-30,0,30,30,0,30,-30,0,-30,30,0,-30};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.instances_={road};b.cache_scene_mesh(floor,nullptr,false);
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    auto capture=[&]() {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      b.draw_instance(5,road,road.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("scenery lamp readback timeout");
    };
    const auto day=capture();  // update_local_light at noon: no lamp
    scene_sun_set_time(22*3600.f);b.update_local_light();
    SLRR_REQUIRE(b.scenery_lamps_.size()==1);
    const auto night=capture();
    b.scenery_lamps_.clear();
    const auto unlit=capture();
    SLRR_REQUIRE(day==unlit);
    unsigned lit=0;int centre_gain=0,edge_gain=0;
    const auto at=[&](const std::vector<uint8_t>& image,int x,int y){return int(image[(size_t(y)*width+x)*4]);};
    for(size_t i=0;i<night.size();i+=4) if(int(night[i])-int(unlit[i])>10) ++lit;
    centre_gain=at(night,width/2,height/2)-at(unlit,width/2,height/2);
    edge_gain=at(night,2,height-3)-at(unlit,2,height-3);
    std::printf("SCENERY lamp lit pixels=%u of %u, centre gain=%d, corner gain=%d\n",
                lit,unsigned(width*height),centre_gain,edge_gain);
    SLRR_REQUIRE(lit>2000 && centre_gain>40 && centre_gain>edge_gain);
    // A lamp that fades to zero (entering or leaving the set) adds nothing.
    LampLight faded=street;faded.intensity=0;b.scenery_lamps_={faded};
    SLRR_REQUIRE(capture()==unlit);
    // A lamp more than 45 m from every road pixel adds nothing either.
    LampLight away=street;away.position={100,7.8f,0};b.scenery_lamps_={away};
    SLRR_REQUIRE(capture()==unlit);
    b.scenery_lamps_.clear();
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.instances_.clear();b.fixture_lights_.clear();b.scenery_lamps_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&shell);SLRR_REQUIRE(scenery_lights_count()==0);
    gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS scenery lamp lights the road at night only, option 4 and groups");
  }
  // #237: GPU cost of the scenery lamp loop in fs_mesh at 1920x1080, one
  // street-level road filling the lower half, for 0, 12 and 64 lamps. Times are bgfx GPU
  // timer queries over whole frames; the difference to 0 lamps is the cost.
  static void lamp_cost(LibBgfxBackend& b) {
    Fixture f;Object world;gameref_upsert(&world)->flags=kWorldTreeRoot;
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    const auto saved=host_enhanced_graphics();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-600,0,600,600,0,600,-600,0,-600,600,0,-600};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.cache_scene_mesh(floor,nullptr,false);
    constexpr uint16_t width=1920,height=1080;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    float view[16],projection[16],render[16];
    // Street-level view down a straight road, as when driving.
    bx::mtxLookAt(view,{0,2,0},{0,1,40});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    const auto measure=[&](const std::vector<LampLight>& lamps) {
      b.scenery_lamps_=lamps;
      double total=0;int samples=0;
      for(int frame=0;frame<200;++frame) {
        bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
        bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
        bgfx::setViewTransform(5,view,render);
        bgfx::setViewMode(5,bgfx::ViewMode::Sequential);
        // Eight layers, each 2 cm nearer the camera, so every one is shaded.
        for(int layer=0;layer<8;++layer){auto t=road.transform;t[13]=.02f*float(layer);b.draw_instance(5,road,t.data(),false,0);}
        bgfx::frame();
        const auto* stats=bgfx::getStats();
        if(frame>=40 && stats->gpuTimerFreq>0) {
          total+=double(stats->gpuTimeEnd-stats->gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
          ++samples;
        }
      }
      return samples?total/samples:0.0;
    };
    LampLight lamp;lamp.color={.972f,.888f,.615f};lamp.attenuation={1,.03f,0};
    lamp.intensity=kSceneryLightIntensity;lamp.range=kSceneryLightMaxRange;
    // Street: lamps every 30 m on both sides ahead of the camera (the
    // selection's frustum case); most pixels are out of range of most lamps.
    const auto street=[&](size_t count) {
      std::vector<LampLight> out;
      for(size_t i=0;out.size()<count;++i) for(float side:{-8.f,8.f})
        if(out.size()<count){auto l=lamp;l.position={side,7.8f,15.f+30.f*float(i/1)};out.push_back(l);}
      return out;
    };
    // Worst case: every lamp within range of the whole visible road.
    const auto cluster=[&](size_t count) {
      std::vector<LampLight> out;
      for(size_t i=0;i<count;++i){auto l=lamp;l.position={float(int(i%8))*2.f-7.f,7.8f,10.f+float(i/8)*2.f};out.push_back(l);}
      return out;
    };
    const double none=measure({});
    const double street12=measure(street(12)),street64=measure(street(64));
    const double cluster12=measure(cluster(12)),cluster64=measure(cluster(64));
    std::printf("LAMP GPU cost 1920x1080, 8 road layers over the lower half (ms/frame): none=%.3f "
                "street12=%.3f street64=%.3f cluster12=%.3f cluster64=%.3f\n",
                none,street12,street64,cluster12,cluster64);
    SLRR_REQUIRE(none>0);
    bgfx::setViewMode(5,bgfx::ViewMode::Default);
    bgfx::frame();bgfx::destroy(fb);
    host_enhanced_graphics()=saved;b.scenery_lamps_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&world);
    std::puts("PASS scenery lamp GPU timing recorded");
  }
  // #237: street lamp shadows. A static prop and a car under a lamp darken the
  // road at night; nothing changes by day; at most kLampShadows lamps are
  // shadowed; a newly assigned lamp starts at zero shadow strength; the tile
  // is redrawn only when a dynamic caster moves. Then GPU timing.
  static void lamp_shadows(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell,prop;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);gameref_set_matrix(&shell,0,0,0,0,0,0);
    auto box=[](float x0,float y0,float z0,float x1,float y1,float z1) {
      auto cube=std::make_shared<ScxGeometry>();cube->info.ok=true;cube->info.version=4;cube->position_scale=1;
      cube->positions={x0,y0,z0,x1,y0,z0,x1,y1,z0,x0,y1,z0,x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1};
      cube->indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
      cube->normals.resize(24,0.f);cube->uvs.resize(16,0.f);
      cube->info.bbox_min[0]=x0;cube->info.bbox_min[1]=y0;cube->info.bbox_min[2]=std::min(z0,z1);
      cube->info.bbox_max[0]=x1;cube->info.bbox_max[1]=y1;cube->info.bbox_max[2]=std::max(z0,z1);
      ScxGroup group;group.index_count=36;cube->groups={group};return cube;
    };
    // Static prop: a 4 m block 4 m beside the lamp, through the world tree.
    constexpr uint32_t prop_id=0x7ff10001;
    scene_instances_override_mesh(prop_id,box(-1.f,0,-1.f,1.f,4.f,1.f));
    auto* prop_node=gameref_upsert(&prop,GameRefKind::RenderRef);prop_node->empty=false;
    gameref_set_type(*prop_node,int32_t(prop_id));
    gameref_set_parent(&prop,&world);gameref_make_live(&prop);gameref_set_matrix(&prop,4,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    SceneryLight lamp;
    lamp.position={0,7.8f,0};lamp.color={.972f,.888f,.615f};lamp.range=500;
    lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
    lights->push_back(lamp);
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;host_enhanced_graphics().shadow_quality=2;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    float view[16],projection[16],render[16];
    constexpr uint16_t width=160,height=120;
    bx::mtxLookAt(view,{0,14,-26},{0,0,0});bx::mtxProj(projection,60.f,float(width)/height,.05f,200.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.cull_eye_[0]=0;b.cull_eye_[1]=14;b.cull_eye_[2]=26;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-30,0,30,30,0,30,-30,0,-30,30,0,-30};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    floor->info.bbox_min[0]=-30;floor->info.bbox_max[0]=30;floor->info.bbox_min[2]=-30;floor->info.bbox_max[2]=30;
    floor->ground_polygons=true;
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.cache_scene_mesh(floor,nullptr,false);
    // Dynamic caster: a car-sized box 4 m on the other side of the lamp.
    auto car_mesh=box(-.9f,0,-2,.9f,2.5f,2);
    b.cache_scene_mesh(car_mesh,nullptr,false);
    SceneInstance car;car.geometry=car_mesh;car.vehicle_material=true;bx::mtxIdentity(car.transform.data());
    car.transform[12]=-4;
    b.instances_={road,car};
    VideoOptions video;video.shadows=2;
    b.lamp_shadow_dt_override_=1.f;  // instant full strength
    scene_sun_set_time(22*3600.f);b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(b.lamp_shadow_count_==1 && b.lamp_shadow_atlas_bound_);
    const auto& slot=b.lamp_shadow_slots_[0];
    std::printf("LAMP SHADOW statics=%zu batches=%zu redraws=%llu\n",slot.statics.size(),
                slot.static_batches.size(),(unsigned long long)b.lamp_shadow_redraws_);
    // The prop casts; the road (ground POLY) does not.
    SLRR_REQUIRE(slot.statics.size()==1 && slot.statics[0].geometry->info.bbox_max[1]==4.f);
    SLRR_REQUIRE(slot.weight==1.f && b.lamp_shadow_redraws_==1);
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    auto capture=[&]() {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      b.draw_instance(5,road,road.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("lamp shadow readback timeout");
    };
    const auto darker=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b2,int x0,int x1) {
      unsigned count=0;
      for(int y=0;y<height;++y) for(int x=x0;x<x1;++x) {
        const size_t i=(size_t(y)*width+x)*4;
        if(int(b2[i])-int(a[i])>12) ++count;
      }
      return count;
    };
    bgfx::frame();
    const auto shadowed=capture();
    const auto keep_bound=b.lamp_shadow_atlas_bound_;b.lamp_shadow_atlas_bound_=false;
    const auto unshadowed=capture();
    b.lamp_shadow_atlas_bound_=keep_bound;
    // Camera looks down +z: script +x (prop) is image right? Count both halves.
    const unsigned left=darker(shadowed,unshadowed,0,width/2),right=darker(shadowed,unshadowed,width/2,width);
    std::printf("LAMP SHADOW darkened pixels left=%u right=%u\n",left,right);
    SLRR_REQUIRE(left>15 && right>15 && left+right>60);
    // Unchanged casters: no redraw. A moved car: one redraw.
    b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(b.lamp_shadow_redraws_==1);
    b.instances_[1].transform[14]+=.5f;
    b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(b.lamp_shadow_redraws_==2);
    // Shadows off, option 4 off or day: no lamp shadows.
    video.shadows=0;b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(!b.lamp_shadow_atlas_bound_ && b.lamp_shadow_count_==0);
    video.shadows=2;
    scene_sun_set_time(12*3600.f);b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(!b.lamp_shadow_atlas_bound_ && b.scenery_lamps_.empty());
    const auto day=capture();
    b.scenery_lamps_.clear();
    SLRR_REQUIRE(capture()==day);
    // Reassignment: a freshly assigned lamp starts at zero strength, so the
    // image equals the unshadowed one, then fades in.
    scene_sun_set_time(22*3600.f);
    b.lamp_shadow_dt_override_=0.f;
    b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(b.lamp_shadow_count_==1 && b.lamp_shadow_slots_[0].weight==0.f);
    SLRR_REQUIRE(capture()==unshadowed);
    b.lamp_shadow_dt_override_=.1f;
    b.update_local_light();b.update_lamp_shadows(video);
    SLRR_REQUIRE(std::abs(b.lamp_shadow_slots_[0].weight-.1f/LibBgfxBackend::kLampShadowFade)<1e-5f);
    const auto partial=capture();
    const unsigned partial_count=darker(partial,unshadowed,0,width);
    SLRR_REQUIRE(partial_count>0 && partial_count<left+right);
    // Budget: ten lamps in view, at most kLampShadows shadowed, the nearest.
    {
      auto many=std::make_shared<std::vector<SceneryLight>>();
      for(int i=0;i<10;++i){auto l=lamp;l.position={-20.f+4.f*i,7.8f,float(i%2)*6.f};many->push_back(l);}
      SLRR_REQUIRE(scenery_lights_attach(&shell,many));
      b.lamp_shadow_dt_override_=1.f;
      b.update_local_light();b.update_lamp_shadows(video);
      SLRR_REQUIRE(b.lamp_shadow_count_==LibBgfxBackend::kLampShadows);
      float nearest_unshadowed=1e9f,farthest_shadowed=0;
      const bx::Vec3 eye{0,14,-26};
      for(size_t i=0;i<b.scenery_lamps_.size();++i) {
        const auto& l=b.scenery_lamps_[i];
        const float d=bx::length(bx::sub(bx::Vec3{l.position[0],l.position[1],l.position[2]},eye));
        if(i<b.lamp_shadow_count_) farthest_shadowed=std::max(farthest_shadowed,d);
        else nearest_unshadowed=std::min(nearest_unshadowed,d);
      }
      SLRR_REQUIRE(farthest_shadowed<=nearest_unshadowed+1e-3f);
      // GPU cost: road 8 layers at 1920x1080 plus re-rendering every shadow
      // tile each frame (the car moves), for 0 and kLampShadows shadowed lamps.
      constexpr uint16_t big_w=1920,big_h=1080;
      const auto big_color=bgfx::createTexture2D(big_w,big_h,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
      const auto big_depth=bgfx::createTexture2D(big_w,big_h,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
      const bgfx::TextureHandle big_att[]={big_color,big_depth};const auto big_fb=bgfx::createFrameBuffer(2,big_att,true);
      float big_view[16],big_proj[16],big_render[16];
      bx::mtxLookAt(big_view,{0,2,-30},{0,1,0});
      bx::mtxProj(big_proj,60.f,float(big_w)/big_h,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(big_proj,big_proj+16,big_render);
      for(int i:{2,6,10,14}) big_render[i]=bgfx::getCaps()->homogeneousDepth?-big_render[i]:big_render[i+1]-big_render[i];
      const auto measure=[&](bool shadows,bool move) {
        double total=0;int samples=0;
        for(int frame=0;frame<160;++frame) {
          if(move) b.instances_[1].transform[14]=std::sin(frame*.1f);
          b.update_local_light();
          VideoOptions v;v.shadows=shadows?2:0;b.update_lamp_shadows(v);
          bgfx::setViewMode(5,bgfx::ViewMode::Sequential);
          bgfx::setViewFrameBuffer(5,big_fb);bgfx::setViewRect(5,0,0,big_w,big_h);
          bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
          bgfx::setViewTransform(5,big_view,big_render);
          for(int layer=0;layer<8;++layer){auto t=road.transform;t[13]=.02f*float(layer);b.draw_instance(5,road,t.data(),false,0);}
          bgfx::frame();
          const auto* stats=bgfx::getStats();
          if(frame>=40 && stats->gpuTimerFreq>0) {
            total+=double(stats->gpuTimeEnd-stats->gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);++samples;
          }
        }
        bgfx::setViewMode(5,bgfx::ViewMode::Default);
        return samples?total/samples:0.0;
      };
      const auto redraws_before=b.lamp_shadow_redraws_;
      const double off=measure(false,true);
      const double cached=measure(true,false);
      const auto cached_redraws=b.lamp_shadow_redraws_-redraws_before;
      const double moving=measure(true,true);
      std::printf("LAMP SHADOW GPU 1920x1080, 8 road layers, 10 lamps (ms/frame): "
                  "unshadowed=%.3f shadowed-cached=%.3f shadowed-redraw-every-frame=%.3f "
                  "(cached redraws=%llu, tile %u px)\n",off,cached,moving,
                  (unsigned long long)cached_redraws,unsigned(b.fixture_shadow_tile_));
      SLRR_REQUIRE(cached_redraws<=LibBgfxBackend::kLampShadows);
      bgfx::frame();bgfx::destroy(big_fb);
    }
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.scenery_lamps_.clear();b.lamp_shadow_dt_override_=-1.f;b.update_lamp_shadows(VideoOptions{});
    b.instances_.clear();b.fixture_lights_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&prop);gameref_remove(&shell);SLRR_REQUIRE(scenery_lights_count()==0);
    gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS street lamp shadows: static and car casters at night, none by day, budget, fade-in, caching");
  }
  // #237 follow-up: the shadowed lamps must not depend on where the camera
  // looks. A stationary car with the chase camera orbiting it must keep the
  // same shadowed lamps; driving along a street reassigns only as lamps are
  // passed.
  static void lamp_shadow_stability(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);gameref_set_matrix(&shell,0,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    // A straight street along script Z: lamps every 30 m on both sides.
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    for(int i=-20;i<=20;++i) for(float side:{-8.f,8.f}) {
      SceneryLight lamp;lamp.position={side,7.8f,30.f*i+(side>0?15.f:0.f)};lamp.color={.972f,.888f,.615f};
      lamp.range=500;lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
      lights->push_back(lamp);
    }
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().shadow_quality=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fixture_shadow_active_=false;b.fixture_lights_.clear();
    b.instances_.clear();
    scene_sun_set_time(22*3600.f);
    VideoOptions video;video.shadows=2;
    b.lamp_shadow_dt_override_=1.f/60.f;
    const auto aspect=16.f/9.f;
    // Chase camera: 7 m behind/around the car, 2.5 m up, looking at it.
    const auto place_camera=[&](const std::array<float,3>& car,float yaw) {
      const float ex=car[0]+7.f*std::sin(yaw),ez=car[2]-7.f*std::cos(yaw);
      b.cull_eye_[0]=ex;b.cull_eye_[1]=2.5f;b.cull_eye_[2]=ez;
      bx::mtxLookAt(b.cull_view_,{ex,2.5f,-ez},{car[0],1.f,-car[2]});
      bx::mtxProj(b.cull_proj_,60.f,aspect,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
    };
    const auto shadowed=[&] {
      std::vector<const void*> out;
      for(const auto& slot:b.lamp_shadow_slots_) if(slot.source) out.push_back(slot.source);
      std::sort(out.begin(),out.end());
      return out;
    };
    const auto frame=[&](const std::array<float,3>& car,float yaw) {
      place_camera(car,yaw);
      b.set_lamp_shadow_focus(car.data());
      b.update_local_light();b.update_lamp_shadows(video);
    };
    // Settle with the car still, then orbit the camera twice around it.
    const std::array<float,3> parked{0,0,3};
    for(int i=0;i<120;++i) frame(parked,0.f);
    const auto settled=shadowed();
    const auto assignments=b.lamp_shadow_assignments_;
    size_t changes=0;auto previous=settled;
    for(int step=0;step<=720;++step) {
      frame(parked,float(step)*3.14159265f/180.f);
      const auto now=shadowed();
      if(now!=previous) ++changes;
      previous=now;
    }
    std::printf("LAMP SHADOW orbit: shadowed=%zu set changes=%zu new assignments=%llu\n",
                settled.size(),changes,(unsigned long long)(b.lamp_shadow_assignments_-assignments));
    SLRR_REQUIRE(settled.size()==LibBgfxBackend::kLampShadows);
    SLRR_REQUIRE(changes==0 && b.lamp_shadow_assignments_==assignments);
    // Every shadowed lamp lights, wherever the camera looks.
    for(size_t i=0;i<b.lamp_shadow_count_;++i) SLRR_REQUIRE(b.scenery_lamps_[i].intensity>0.f);
    // Drive 300 m at 20 m/s (60 fps): lamps are passed every 15 m, so about
    // 20 lamps come into the near set; with hysteresis each one is assigned
    // once, never swapped back and forth.
    const auto before=b.lamp_shadow_assignments_;
    std::map<const void*,int> assigned_count;
    std::vector<const void*> last=shadowed();
    for(int i=0;i<=900;++i) {
      const std::array<float,3> car{0,0,3.f+i*(20.f/60.f)};
      frame(car,0.f);
      const auto now=shadowed();
      for(const auto* source:now)
        if(!std::binary_search(last.begin(),last.end(),source)) ++assigned_count[source];
      last=now;
    }
    int repeats=0;for(const auto& [source,count]:assigned_count) repeats=std::max(repeats,count);
    std::printf("LAMP SHADOW drive 300 m: new assignments=%llu lamps newly shadowed=%zu max per lamp=%d\n",
                (unsigned long long)(b.lamp_shadow_assignments_-before),assigned_count.size(),repeats);
    SLRR_REQUIRE(repeats==1);
    SLRR_REQUIRE(b.lamp_shadow_assignments_-before<=assigned_count.size()+LibBgfxBackend::kLampShadows);
    SLRR_REQUIRE(assigned_count.size()>=15 && assigned_count.size()<=30);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.scenery_lamps_.clear();b.lamp_shadow_dt_override_=-1.f;b.set_lamp_shadow_focus(nullptr);
    b.update_lamp_shadows(VideoOptions{});b.outdoor_lighting_=false;
    gameref_remove(&shell);gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS lamp shadows ignore camera rotation and reassign with hysteresis while driving");
  }
  // #237: the followed car casts a shadow from every lamp whose light reaches
  // it (car-only tiles). Seen from a fixed top-down observer, the car's
  // shadow must not change while the game camera orbits the parked car, and
  // must change only gradually while the car drives slowly past lamps.
  static void car_lamp_shadows(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);gameref_set_matrix(&shell,0,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    for(int i=-12;i<=12;++i) for(float side:{-8.f,8.f}) {
      SceneryLight lamp;lamp.position={side,7.8f,30.f*i+(side>0?15.f:0.f)};lamp.color={.972f,.888f,.615f};
      lamp.range=500;lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
      lights->push_back(lamp);
    }
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;host_enhanced_graphics().shadow_quality=2;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-60,0,60,60,0,60,-60,0,-60,60,0,-60};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    floor->info.bbox_min[0]=-60;floor->info.bbox_max[0]=60;floor->info.bbox_min[2]=-60;floor->info.bbox_max[2]=60;
    floor->ground_polygons=true;
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.cache_scene_mesh(floor,nullptr,false);
    auto car_mesh=std::make_shared<ScxGeometry>();
    {
      const float x0=-.9f,y0=0,z0=-2,x1=.9f,y1=1.4f,z1=2;
      car_mesh->info.ok=true;car_mesh->info.version=4;car_mesh->position_scale=1;
      car_mesh->positions={x0,y0,z0,x1,y0,z0,x1,y1,z0,x0,y1,z0,x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1};
      car_mesh->indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
      car_mesh->normals.resize(24,0.f);car_mesh->uvs.resize(16,0.f);
      car_mesh->info.bbox_min[0]=x0;car_mesh->info.bbox_min[1]=y0;car_mesh->info.bbox_min[2]=z0;
      car_mesh->info.bbox_max[0]=x1;car_mesh->info.bbox_max[1]=y1;car_mesh->info.bbox_max[2]=z1;
      ScxGroup g;g.index_count=36;car_mesh->groups={g};
    }
    b.cache_scene_mesh(car_mesh,nullptr,false);
    SceneInstance car;car.geometry=car_mesh;car.vehicle_material=true;bx::mtxIdentity(car.transform.data());
    b.instances_={road,car};
    VideoOptions video;video.shadows=2;
    b.lamp_shadow_dt_override_=1.f/60.f;
    scene_sun_set_time(22*3600.f);
    // The game's chase camera; the focus is the car, as set_lamp_shadow_focus
    // gets it in the frame (script space: z flipped from render space).
    const auto game_frame=[&](float car_z,float yaw) {
      b.instances_[1].transform[14]=car_z;
      const float ex=7.f*std::sin(yaw),ez=car_z-7.f*std::cos(yaw);
      b.cull_eye_[0]=ex;b.cull_eye_[1]=2.5f;b.cull_eye_[2]=-ez;
      bx::mtxLookAt(b.cull_view_,{ex,2.5f,ez},{0,1.f,car_z});
      bx::mtxProj(b.cull_proj_,60.f,16.f/9.f,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
      const float focus[3]={0,0,-car_z};
      b.set_lamp_shadow_focus(focus);
      b.update_local_light();b.update_lamp_shadows(video);
    };
    // Fixed observer straight above the car.
    constexpr uint16_t width=192,height=192;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    float view[16],projection[16],render[16];
    const auto observe=[&](float car_z) {
      bx::mtxLookAt(view,{0,40,car_z},{0,0,car_z},{0,0,1});
      bx::mtxProj(projection,40.f,1.f,1.f,100.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(projection,projection+16,render);
      for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    };
    auto capture=[&]() {
      bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
      bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(5,view,render);
      b.draw_instance(5,road,road.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("car shadow readback timeout");
    };
    // Shadow mask: pixels the lamp shadows darken by more than 8 levels.
    const auto mask=[&]() {
      bgfx::frame();
      const auto lit=capture();
      const auto bound=b.lamp_shadow_atlas_bound_;b.lamp_shadow_atlas_bound_=false;
      const auto plain=capture();
      b.lamp_shadow_atlas_bound_=bound;
      std::vector<uint8_t> out(width*height);
      for(size_t i=0;i<out.size();++i) out[i]=int(plain[i*4])-int(lit[i*4])>8;
      return out;
    };
    for(int i=0;i<120;++i) game_frame(0.f,0.f);
    observe(0.f);
    const auto reference=mask();
    size_t reference_pixels=0;for(auto v:reference) reference_pixels+=v;
    const size_t tiles=b.car_shadow_tiles_.size();
    size_t reach=0;
    for(const auto& light:*lights) if(std::hypot(light.position[0],light.position[1]-1.f,light.position[2])<
                                      kSceneryLightMaxRange+LibBgfxBackend::kCarShadowCover) ++reach;
    std::printf("CAR SHADOW parked: car tiles=%zu lamps reaching the car=%zu shadow pixels=%zu\n",
                tiles,reach,reference_pixels);
    SLRR_REQUIRE(tiles==std::min(reach,LibBgfxBackend::kCarShadowTiles) && tiles>LibBgfxBackend::kLampShadows);
    SLRR_REQUIRE(reference_pixels>200);
    // Orbit the game camera twice around the parked car.
    size_t worst=0;
    const auto redraws=b.car_shadow_redraws_;
    for(int step=1;step<=72;++step) {
      game_frame(0.f,float(step)*10.f*3.14159265f/180.f);
      if(step%6) continue;
      const auto now=mask();
      size_t differ=0;for(size_t i=0;i<now.size();++i) differ+=now[i]!=reference[i];
      worst=std::max(worst,differ);
    }
    std::printf("CAR SHADOW orbit: worst differing shadow pixels=%zu of %zu, car tile redraws=%llu\n",
                worst,reference_pixels,(unsigned long long)(b.car_shadow_redraws_-redraws));
    SLRR_REQUIRE(worst==0 && b.car_shadow_redraws_==redraws);
    // Drive slowly (3 m/s) for 30 m: car tile strengths change smoothly, a
    // tile appears or disappears only at near-zero strength, and the scenery
    // tiles are not redrawn for the moving car.
    std::map<const void*,float> previous;
    for(const auto& tile:b.car_shadow_tiles_) previous[tile.source]=tile.strength;
    float max_step=0,max_enter=0,max_leave=0;
    const auto scenery_redraws=b.lamp_shadow_redraws_;
    const auto assignments=b.lamp_shadow_assignments_;
    for(int i=1;i<=600;++i) {
      game_frame(i*.05f,0.f);
      std::map<const void*,float> now;
      for(const auto& tile:b.car_shadow_tiles_) now[tile.source]=tile.strength;
      for(const auto& [source,value]:now) {
        const auto old=previous.find(source);
        if(old==previous.end()) max_enter=std::max(max_enter,value);
        else max_step=std::max(max_step,std::abs(value-old->second));
      }
      for(const auto& [source,value]:previous) if(!now.contains(source)) max_leave=std::max(max_leave,value);
      previous=std::move(now);
    }
    std::printf("CAR SHADOW slow drive: max strength step=%.4f entering=%.4f leaving=%.4f "
                "scenery redraws=%llu (new assignments=%llu)\n",max_step,max_enter,max_leave,
                (unsigned long long)(b.lamp_shadow_redraws_-scenery_redraws),
                (unsigned long long)(b.lamp_shadow_assignments_-assignments));
    SLRR_REQUIRE(max_step<.05f && max_enter<.05f && max_leave<.05f);
    SLRR_REQUIRE(b.lamp_shadow_redraws_-scenery_redraws==b.lamp_shadow_assignments_-assignments);
    // GPU cost: 1920x1080, 8 road layers, car moving every frame.
    {
      constexpr uint16_t big_w=1920,big_h=1080;
      const auto big_color=bgfx::createTexture2D(big_w,big_h,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
      const auto big_depth=bgfx::createTexture2D(big_w,big_h,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
      const bgfx::TextureHandle big_att[]={big_color,big_depth};const auto big_fb=bgfx::createFrameBuffer(2,big_att,true);
      float big_view[16],big_proj[16],big_render[16];
      bx::mtxLookAt(big_view,{0,2.5f,-7},{0,1,0});
      bx::mtxProj(big_proj,60.f,float(big_w)/big_h,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(big_proj,big_proj+16,big_render);
      for(int i:{2,6,10,14}) big_render[i]=bgfx::getCaps()->homogeneousDepth?-big_render[i]:big_render[i+1]-big_render[i];
      const auto measure=[&](bool car_tiles,bool shadows) {
        double total=0;int samples=0;size_t tile_count=0;
        for(int frame=0;frame<160;++frame) {
          const float z=std::sin(frame*.05f)*.5f;
          b.instances_[1].transform[14]=z;
          const float focus[3]={0,0,-z};
          b.set_lamp_shadow_focus(car_tiles?focus:nullptr);
          b.update_local_light();VideoOptions v;v.shadows=shadows?2:0;b.update_lamp_shadows(v);
          tile_count=b.car_shadow_tiles_.size();
          bgfx::setViewMode(5,bgfx::ViewMode::Sequential);
          bgfx::setViewFrameBuffer(5,big_fb);bgfx::setViewRect(5,0,0,big_w,big_h);
          bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
          bgfx::setViewTransform(5,big_view,big_render);
          for(int layer=0;layer<8;++layer){auto t=road.transform;t[13]=.02f*float(layer);b.draw_instance(5,road,t.data(),false,0);}
          bgfx::frame();
          const auto* stats=bgfx::getStats();
          if(frame>=40 && stats->gpuTimerFreq>0) {
            total+=double(stats->gpuTimeEnd-stats->gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);++samples;
          }
        }
        bgfx::setViewMode(5,bgfx::ViewMode::Default);
        return std::pair{samples?total/samples:0.0,tile_count};
      };
      const auto off=measure(false,false);
      const auto scenery=measure(false,true);
      const auto with_car=measure(true,true);
      std::printf("CAR SHADOW GPU 1920x1080, 8 road layers, car moving (ms/frame): no lamp shadows=%.3f "
                  "6 scenery tiles=%.3f scenery + %zu car tiles=%.3f\n",
                  off.first,scenery.first,with_car.second,with_car.first);
      // #250: the same with each shadow smoothing (Very soft is the 4x4 tent).
      const int saved_smoothing=host_enhanced_graphics().shadow_smoothing;
      for(int smoothing=0;smoothing<3;++smoothing) {
        host_enhanced_graphics().shadow_smoothing=smoothing;
        const auto timed=measure(true,true);
        std::printf("CAR SHADOW GPU smoothing=%d scenery + %zu car tiles=%.3f ms (no lamp shadows %.3f)\n",
                    smoothing,timed.second,timed.first,measure(false,false).first);
      }
      host_enhanced_graphics().shadow_smoothing=saved_smoothing;
      bgfx::frame();bgfx::destroy(big_fb);
    }
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.scenery_lamps_.clear();b.lamp_shadow_dt_override_=-1.f;b.set_lamp_shadow_focus(nullptr);
    b.update_lamp_shadows(VideoOptions{});
    b.instances_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&shell);gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS car casts a shadow from every lamp reaching it, fixed under camera orbit, smooth while driving");
  }
  // #243: renderer debug overrides. Defaults equal normal behaviour; each
  // pass toggle removes its views (no GPU time) and restores them; budgets
  // bound the lamp selections. Also prints a first per-view cost table.
  static void render_debug_overrides(LibBgfxBackend& b) {
    // The harness runs with the sun-step crossfade off (main); compare the rest.
    const float harness_fade=render_debug().shadow_crossfade;
    render_debug().shadow_crossfade=RenderDebugOverrides{}.shadow_crossfade;
    SLRR_REQUIRE(render_debug()==RenderDebugOverrides{});
    render_debug().shadow_crossfade=harness_fade;
    SLRR_REQUIRE(render_debug_budget(-1,40,40)==40 && render_debug_budget(70,40,40)==40 &&
                 render_debug_budget(3,40,40)==3);
    SLRR_REQUIRE(local_shadow_resolution()==uint16_t(256u<<std::clamp(host_enhanced_graphics().shadow_quality,0,2)));
    SLRR_REQUIRE(cascade_radii()==shadow_radii);
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);gameref_set_matrix(&shell,0,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    for(int i=-12;i<=12;++i) for(float side:{-8.f,8.f}) {
      SceneryLight lamp;lamp.position={side,7.8f,30.f*i+(side>0?15.f:0.f)};lamp.color={.972f,.888f,.615f};
      lamp.range=500;lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
      lights->push_back(lamp);
    }
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=1;host_enhanced_graphics().ao_quality=1;
    host_enhanced_graphics().shadow_quality=2;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-60,0,60,60,0,60,-60,0,-60,60,0,-60};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    floor->info.bbox_min[0]=-60;floor->info.bbox_max[0]=60;floor->info.bbox_min[2]=-60;floor->info.bbox_max[2]=60;
    floor->ground_polygons=true;
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.cache_scene_mesh(floor,nullptr,false);
    auto car_mesh=std::make_shared<ScxGeometry>();
    {
      const float x0=-.9f,y0=0,z0=-2,x1=.9f,y1=1.4f,z1=2;
      car_mesh->info.ok=true;car_mesh->info.version=4;car_mesh->position_scale=1;
      car_mesh->positions={x0,y0,z0,x1,y0,z0,x1,y1,z0,x0,y1,z0,x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1};
      car_mesh->indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
      car_mesh->normals.resize(24,0.f);car_mesh->uvs.resize(16,0.f);
      car_mesh->info.bbox_min[0]=x0;car_mesh->info.bbox_min[1]=y0;car_mesh->info.bbox_min[2]=z0;
      car_mesh->info.bbox_max[0]=x1;car_mesh->info.bbox_max[1]=y1;car_mesh->info.bbox_max[2]=z1;
      ScxGroup g;g.index_count=36;car_mesh->groups={g};
    }
    b.cache_scene_mesh(car_mesh,nullptr,false);
    SceneInstance car;car.geometry=car_mesh;car.vehicle_material=true;bx::mtxIdentity(car.transform.data());
    b.instances_={road,car};
    scene_sun_set_time(22*3600.f);
    b.lamp_shadow_dt_override_=1.f;
    constexpr uint16_t width=1280,height=720;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    float view[16],projection[16],render[16];
    bx::mtxLookAt(view,{0,2.5f,-7},{0,1,0});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.cull_eye_[0]=0;b.cull_eye_[1]=2.5f;b.cull_eye_[2]=7;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    bgfx::setDebug(BGFX_DEBUG_PROFILER);
    // GPU ms per view over the last frames, with the car moving (every lamp
    // and car tile is redrawn each frame).
    const auto run=[&](int frames) {
      std::map<uint16_t,double> total;int samples=0;
      for(int frame=0;frame<frames;++frame) {
        const float z=std::sin(frame*.1f)*.5f;
        b.instances_[1].transform[14]=z;
        const float focus[3]={0,0,-z};b.set_lamp_shadow_focus(focus);
        b.update_local_light();VideoOptions v;v.shadows=2;b.update_lamp_shadows(v);
        b.update_ambient_occlusion(view,projection,width,height,5);
        bgfx::setViewFrameBuffer(5,fb);bgfx::setViewRect(5,0,0,width,height);
        bgfx::setViewClear(5,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
        bgfx::setViewTransform(5,view,render);
        b.draw_instance(5,road,road.transform.data(),false,0);
        b.draw_instance(5,b.instances_[1],b.instances_[1].transform.data(),false,0);
        bgfx::frame();
        const auto* stats=bgfx::getStats();
        if(frame<frames/3 || stats->gpuTimerFreq<=0) continue;
        ++samples;
        for(uint16_t i=0;i<stats->numViews;++i) {
          const auto& vs=stats->viewStats[i];
          total[vs.view]+=double(vs.gpuTimeEnd-vs.gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
        }
      }
      for(auto& [id,value]:total) value/=std::max(samples,1);
      return total;
    };
    const auto lamp_views=[](const std::map<uint16_t,double>& t) {
      double sum=0;for(const auto& [id,value]:t) if(id>=128&&id<136) sum+=value;return sum;
    };
    const auto ao_views=[](const std::map<uint16_t,double>& t) {
      double sum=0;for(const auto& [id,value]:t) if(id>=140&&id<145) sum+=value;return sum;
    };
    const auto has=[](const std::map<uint16_t,double>& t,uint16_t lo,uint16_t hi) {
      for(const auto& [id,value]:t) if(id>=lo&&id<hi) return true;
      return false;
    };
    // Cached scenery lamp tiles are not resubmitted: with caching on they
    // have no view time after the first frame.
    auto cached=run(20);
    SLRR_REQUIRE(!has(cached,128,136) && has(cached,136,137));
    // Shadow caching off redraws every tile each frame (worst case).
    render_debug().shadow_caching=false;
    auto all=run(60);
    std::printf("RENDER DEBUG per-view GPU ms (1280x720, 50 lamps, caching off: 6 lamp + 7 car tiles redrawn, HBAO):\n");
    for(const auto& [id,value]:all) std::printf("  view %3u %-32s %.3f\n",unsigned(id),render_debug_view_name(id),value);
    SLRR_REQUIRE(has(all,128,136) && has(all,136,137) && has(all,140,145));
    auto& o=render_debug();
    // Each toggle removes its views and restores them.
    o.lamp_shadows=false;auto t=run(20);
    std::printf("RENDER DEBUG lamp shadows off: lamp views %.3f ms (on %.3f), car tiles present=%d\n",
                lamp_views(t),lamp_views(all),int(has(t,136,137)));
    SLRR_REQUIRE(!has(t,128,137) && b.lamp_shadow_count_==0);
    o.lamp_shadows=true;
    o.car_lamp_tiles=false;t=run(20);
    SLRR_REQUIRE(has(t,128,136) && !has(t,136,137) && b.car_shadow_tiles_.empty());
    o.car_lamp_tiles=true;
    o.ambient_occlusion=false;t=run(20);
    std::printf("RENDER DEBUG AO off: AO views %.3f ms (on %.3f)\n",ao_views(t),ao_views(all));
    SLRR_REQUIRE(!has(t,140,145) && !b.ao_active_);
    o.ambient_occlusion=true;
    o.lamp_lighting=false;t=run(10);
    SLRR_REQUIRE(b.scenery_lamps_.empty() && !has(t,128,137));
    o.lamp_lighting=true;
    t=run(20);
    SLRR_REQUIRE(has(t,128,136) && has(t,136,137) && has(t,140,145));
    // Budgets.
    o.shadowed_lamps=2;o.car_tiles=3;o.lamp_budget=5;o.lamp_distance=60.f;
    run(10);
    size_t wanted=0;for(const auto& slot:b.lamp_shadow_slots_) wanted+=slot.source&&slot.wanted;
    SLRR_REQUIRE(wanted==2 && b.car_shadow_tiles_.size()==3);
    // Lit lamps: at most 5 frustum lamps within 60 m, plus the shadowed/car ones.
    size_t frustum_only=b.scenery_lamps_.size()-b.lamp_shadow_count_;
    SLRR_REQUIRE(frustum_only<=5);
    for(const auto& lamp:b.scenery_lamps_)
      SLRR_REQUIRE(bx::length(bx::sub(bx::Vec3{lamp.position[0],lamp.position[1],lamp.position[2]},
                                      bx::Vec3{0,2.5f,-7}))<=60.f+LibBgfxBackend::kLampShadowReach);
    o.local_shadow_tile=256;SLRR_REQUIRE(local_shadow_resolution()==256);
    o.cascade_scale=2.f;SLRR_REQUIRE(cascade_radii()[0]==2.f*shadow_radii[0]);
    {
      // Snapshot header (#243): only overrides that differ from the defaults.
      const auto text=render_debug_overrides_text(o);
      std::printf("RENDER DEBUG overrides header:\n%s",text.c_str());
      SLRR_REQUIRE(text.find("shadowed lamps = 2")!=std::string::npos);
      SLRR_REQUIRE(text.find("car lamp tiles budget = 3")!=std::string::npos);
      SLRR_REQUIRE(text.find("lamp distance = 60 m")!=std::string::npos);
      SLRR_REQUIRE(text.find("cascade radius scale = 2x")!=std::string::npos);
      SLRR_REQUIRE(text.find("ambient occlusion")==std::string::npos);
      SLRR_REQUIRE(render_debug_overrides_text(RenderDebugOverrides{}).empty());
    }
    o={};SLRR_REQUIRE(render_debug()==RenderDebugOverrides{});o.shadow_crossfade=harness_fade;
    run(5);
    bgfx::setDebug(BGFX_DEBUG_NONE);
    bgfx::frame();bgfx::destroy(fb);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.destroy_ao_targets();
    b.scenery_lamps_.clear();b.lamp_shadow_dt_override_=-1.f;b.set_lamp_shadow_focus(nullptr);
    b.update_lamp_shadows(VideoOptions{});
    b.instances_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&shell);gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS renderer debug overrides: defaults, pass toggles remove their views, budgets");
  }
  // #246: per-view light uniforms. In a Sequential view the lamp, fixture and
  // shadow arrays are uploaded on the first lit draw of the view and frame;
  // the image must equal the per-draw upload, and unknown views, new frames
  // and changed light data upload again.
  static void per_view_light_uniforms(LibBgfxBackend& b) {
    Fixture f;scene_instances_set_resources(&f.packs);
    Object world,shell;
    gameref_upsert(&world)->flags=kWorldTreeRoot;
    gameref_upsert(&shell,GameRefKind::RenderRef)->empty=false;
    gameref_set_parent(&shell,&world);gameref_make_live(&shell);gameref_set_matrix(&shell,0,0,0,0,0,0);
    scene_instances_select(&world,nullptr,false,SceneEnvironment::Exterior);
    auto lights=std::make_shared<std::vector<SceneryLight>>();
    for(int i=-6;i<=6;++i) for(float side:{-8.f,8.f}) {
      SceneryLight lamp;lamp.position={side,7.8f,30.f*i+(side>0?15.f:0.f)};lamp.color={.972f,.888f,.615f};
      lamp.range=500;lamp.attenuation={1,.03f,0};lamp.group=kLightGroupCity;lamp.lights_scene=true;
      lights->push_back(lamp);
    }
    SLRR_REQUIRE(scenery_lights_attach(&shell,lights));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;host_enhanced_graphics().shadow_quality=2;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    auto floor=std::make_shared<ScxGeometry>();floor->info.ok=true;floor->info.version=4;floor->position_scale=1;
    floor->positions={-60,0,60,60,0,60,-60,0,-60,60,0,-60};floor->normals={0,1,0,0,1,0,0,1,0,0,1,0};
    floor->uvs.resize(8);floor->indices={0,2,1,1,2,3};ScxGroup group;group.index_count=6;floor->groups={group};
    floor->info.bbox_min[0]=-60;floor->info.bbox_max[0]=60;floor->info.bbox_min[2]=-60;floor->info.bbox_max[2]=60;
    floor->ground_polygons=true;
    SceneInstance road;road.geometry=floor;bx::mtxIdentity(road.transform.data());
    road.lighting.set=road.lighting.has_direction=true;road.lighting.direction[1]=-1;
    for(int i=0;i<3;++i){road.lighting.diffuse[i]=0;road.lighting.ambient[i]=.08f;road.lighting.specular[i]=0;}
    b.cache_scene_mesh(floor,nullptr,false);
    auto car_mesh=std::make_shared<ScxGeometry>();
    {
      const float x0=-.9f,y0=0,z0=-2,x1=.9f,y1=1.4f,z1=2;
      car_mesh->info.ok=true;car_mesh->info.version=4;car_mesh->position_scale=1;
      car_mesh->positions={x0,y0,z0,x1,y0,z0,x1,y1,z0,x0,y1,z0,x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1};
      car_mesh->indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
      car_mesh->normals.resize(24,0.f);
      for(size_t i=0;i<8;++i) car_mesh->normals[i*3+1]=1.f;
      car_mesh->uvs.resize(16,0.f);
      car_mesh->info.bbox_min[0]=x0;car_mesh->info.bbox_min[1]=y0;car_mesh->info.bbox_min[2]=z0;
      car_mesh->info.bbox_max[0]=x1;car_mesh->info.bbox_max[1]=y1;car_mesh->info.bbox_max[2]=z1;
      ScxGroup g;g.index_count=36;car_mesh->groups={g};
    }
    b.cache_scene_mesh(car_mesh,nullptr,false);
    SceneInstance car;car.geometry=car_mesh;car.vehicle_material=true;bx::mtxIdentity(car.transform.data());
    SceneInstance block=car;block.vehicle_material=false;block.transform[12]=4;
    b.instances_={road,car};
    scene_sun_set_time(22*3600.f);
    b.lamp_shadow_dt_override_=1.f;
    constexpr uint16_t width=320,height=180;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    float view[16],projection[16],render[16];
    bx::mtxLookAt(view,{0,2.5f,-7},{0,1,0});
    bx::mtxProj(projection,60.f,float(width)/height,.05f,2000.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    b.cull_eye_[0]=0;b.cull_eye_[1]=2.5f;b.cull_eye_[2]=7;
    std::copy(view,view+16,b.cull_view_);std::copy(projection,projection+16,b.cull_proj_);
    const float focus[3]={0,0,0};b.set_lamp_shadow_focus(focus);
    VideoOptions video;video.shadows=2;
    for(int i=0;i<3;++i){b.update_local_light();b.update_lamp_shadows(video);}
    SLRR_REQUIRE(b.scenery_lamps_.size()>6 && b.lamp_shadow_count_>0 && !b.car_shadow_tiles_.empty());
    const uint16_t view_id=5;
    b.set_view_sequential(view_id);
    // Four lit draws in one view: road, car, block, road again (a layer).
    auto capture=[&]() {
      ++b.frames_;
      bgfx::setViewFrameBuffer(view_id,fb);bgfx::setViewRect(view_id,0,0,width,height);
      bgfx::setViewClear(view_id,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
      bgfx::setViewTransform(view_id,view,render);
      b.draw_instance(view_id,road,road.transform.data(),false,0);
      b.draw_instance(view_id,b.instances_[1],b.instances_[1].transform.data(),false,0);
      b.draw_instance(view_id,block,block.transform.data(),false,0);
      b.draw_instance(view_id,road,road.transform.data(),false,0);
      bgfx::TextureRegion dst,src;dst.init(readback);src.init(color);bgfx::blit(100,dst,src);bgfx::frame();
      std::vector<uint8_t> bytes(width*height*4);const auto ready=bgfx::read(dst,bytes.data());
      for(int i=0;i<16;++i) if(bgfx::frame()>=ready) return bytes;
      throw std::runtime_error("light uniform readback timeout");
    };
    b.light_array_cache_=false;
    auto before=b.main_bind_.light_array_uploads;
    const auto per_draw=capture();
    const auto per_draw_uploads=b.main_bind_.light_array_uploads-before;
    b.light_array_cache_=true;
    before=b.main_bind_.light_array_uploads;
    const auto per_view=capture();
    const auto per_view_uploads=b.main_bind_.light_array_uploads-before;
    unsigned lit=0;for(size_t i=0;i<per_view.size();i+=4) lit+=per_view[i]>30;
    std::printf("LIGHT UNIFORMS per-draw uploads=%llu per-view uploads=%llu lit pixels=%u, images %s\n",
                (unsigned long long)per_draw_uploads,(unsigned long long)per_view_uploads,lit,
                per_draw==per_view?"identical":"DIFFER");
    SLRR_REQUIRE(per_draw_uploads==4 && per_view_uploads==1 && per_draw==per_view && lit>1000);
    // Next frame: one upload again. Changed light data within a frame: another.
    before=b.main_bind_.light_array_uploads;
    SLRR_REQUIRE(capture()==per_view && b.main_bind_.light_array_uploads-before==1);
    before=b.main_bind_.light_array_uploads;
    b.update_local_light();b.update_lamp_shadows(video);
    b.main_bind_.binding_view=view_id;
    SLRR_REQUIRE(!b.light_arrays_current());
    b.main_bind_.binding_view=UINT16_MAX;
    // An unmarked (sorted) view uploads on every draw.
    b.sequential_views_.reset(view_id);
    before=b.main_bind_.light_array_uploads;
    SLRR_REQUIRE(capture()==per_view && b.main_bind_.light_array_uploads-before==4);
    // #250: the early-out lamp shadow path against the #237 reference loop.
    // Sharp and Soft must match closely (only the receiver gradient is
    // computed differently); Very soft uses the capped 4-tap kernel.
    {
      const int saved_smoothing=host_enhanced_graphics().shadow_smoothing;
      for(int smoothing=0;smoothing<3;++smoothing) {
        host_enhanced_graphics().shadow_smoothing=smoothing;
        render_debug().lamp_shadow_reference=true;
        const auto reference=capture();
        render_debug().lamp_shadow_reference=false;
        const auto fast=capture();
        int worst=0;unsigned differ=0,shaded=0;
        for(size_t i=0;i<fast.size();i+=4) for(int c=0;c<3;++c) {
          const int d=std::abs(int(fast[i+c])-int(reference[i+c]));
          worst=std::max(worst,d);differ+=d>4;
        }
        for(size_t i=0;i<reference.size();i+=4) shaded+=reference[i]>30;
        std::printf("LAMP EARLY-OUT smoothing=%d: max channel difference %d, %u channels differ by >4 "
                    "(%u lit pixels)\n",smoothing,worst,differ,shaded);
        if(smoothing<2) SLRR_REQUIRE(differ<=fast.size()/400);
        else SLRR_REQUIRE(differ<=fast.size()/50);
      }
      host_enhanced_graphics().shadow_smoothing=saved_smoothing;
    }
    // Submission cost: 2,000 lit draws in one view, per-draw vs per-view arrays.
    b.set_view_sequential(view_id);
    const auto submit_cost=[&](bool cache) {
      b.light_array_cache_=cache;
      double submit=0,render_thread=0;int samples=0;
      for(int frame=0;frame<30;++frame) {
        ++b.frames_;
        bgfx::setViewFrameBuffer(view_id,fb);bgfx::setViewRect(view_id,0,0,width,height);
        bgfx::setViewClear(view_id,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
        bgfx::setViewTransform(view_id,view,render);
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<2000;++i) b.draw_instance(view_id,block,block.transform.data(),false,0);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        bgfx::frame();
        const auto* stats=bgfx::getStats();
        if(frame<10) continue;
        submit+=ms;
        render_thread+=double(stats->cpuTimeEnd-stats->cpuTimeBegin)*1000.0/double(stats->cpuTimerFreq);
        ++samples;
      }
      return std::pair{submit/samples,render_thread/samples};
    };
    const auto uncached=submit_cost(false),cached=submit_cost(true);
    b.light_array_cache_=true;
    std::printf("LIGHT UNIFORMS 2000 lit draws: submit %.3f -> %.3f ms, render thread %.3f -> %.3f ms\n",
                uncached.first,cached.first,uncached.second,cached.second);
    // #254: fragment uniform size (vec4) of fs_mesh / fs_vehicle, from the
    // embedded D3D11 shaders' uniform tables.
    const auto uniform_vec4=[](const uint8_t* data,uint32_t size) {
      const auto shader=bgfx::createShader(bgfx::makeRef(data,size));
      bgfx::UniformHandle handles[128];
      const uint16_t count=bgfx::getShaderUniforms(shader,handles,128);
      uint32_t vec4=0;
      for(uint16_t i=0;i<count;++i) {
        bgfx::UniformInfo info;bgfx::getUniformInfo(handles[i],info);
        const uint32_t rows=info.type==bgfx::UniformType::Mat4?4:info.type==bgfx::UniformType::Mat3?3
                           :info.type==bgfx::UniformType::Vec4?1:0;
        vec4+=rows*info.num;
      }
      bgfx::destroy(shader);
      return vec4;
    };
    std::printf("FRAGMENT UNIFORMS fs_mesh %u vec4, fs_vehicle %u vec4\n",
                uniform_vec4(fs_mesh_dxbc,sizeof(fs_mesh_dxbc)),uniform_vec4(fs_vehicle_dxbc,sizeof(fs_vehicle_dxbc)));
    bgfx::setViewMode(view_id,bgfx::ViewMode::Default);
    b.sequential_views_.reset(view_id);
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.scenery_lamps_.clear();b.lamp_shadow_dt_override_=-1.f;b.set_lamp_shadow_focus(nullptr);
    b.update_lamp_shadows(VideoOptions{});
    b.instances_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    gameref_remove(&shell);gameref_remove(&world);scene_instances_set_resources(nullptr);
    std::puts("PASS per-view light uniforms: one upload per Sequential view and frame, same image");
  }
  // #262: main-thread cost of a real city frame. Valo City at night, street
  // view: sync_instances (collection + residency) and world submission
  // (draw_dynamic_world into a Sequential view), plus the render thread, and
  // a byte capture compared with the previous build when one is on disk.
  // #264: `width` x `height` also drives the far-size filter, so 2560x1440
  // keeps the props (and night overlays) the game keeps at that resolution.
  // #268: `crowd` adds what the game has around a street at night and the
  // harness lacked: 80 Valo City walkers (humans 0x57-0x5B, animated, 60 in
  // front of the camera), a parented car-like subtree of 30 cars.rpk renders
  // whose nodes carry a paint texture id, and the same subtree in an inset
  // script viewport preview.
  // #275: Nevada Desert (MW_Mod's Nevada_Track starts maps\ROC_track.rpk) at
  // noon, the full daytime pipeline (sun passes, GTAO High 3 m). The terrain
  // POLY carries baked per-vertex light (grey, mostly 0 or 1); multiplied into
  // the texture under our own sun it turned the sunlit mountains black.
  static void desert_capture(LibBgfxBackend& b) {
    Fixture f;
    const auto game=fs::absolute("game");
    f.vm.set_game_root(game.string());f.packs.set_root(game.string());f.packs.open_all();
    scene_instances_set_resources(&f.packs);
    const auto pack=f.packs.open_lib("maps/ROC_track.rpk");
    const auto ground_id=RpkIndex::make_full_id(pack,1);
    Object* root=f.vm.new_object("java.util.resource.GroundRef");
    gameref_upsert(root,GameRefKind::GroundRef)->type_id=static_cast<int32_t>(ground_id);
    gameref_get(root)->flags|=kWorldTreeRoot;
    spawn_ground_shell(f.vm,f.packs,root);
    scene_instances_select(root,nullptr,false,SceneEnvironment::Exterior);
    float frame_matrix[16];
    SLRR_REQUIRE(scene_instances_ground_frame(root,frame_matrix));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.car_depth_active_=false;b.fixture_shadow_active_=false;
    b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    scene_sun_set_time(13.5f*3600.f);
    constexpr uint16_t width=1280,height=720;
    const int32_t saved_width=b.width_,saved_height=b.height_;b.width_=width;b.height_=height;
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    const uint16_t view_id=5;b.set_view_sequential(view_id);
    std::printf("DESERT start %.1f %.1f %.1f\n",frame_matrix[12],frame_matrix[13],frame_matrix[14]);
    const auto frame_at=[&](float yaw,bool ao,bool shadows) {
      const bx::Vec3 eye{frame_matrix[12],frame_matrix[13]+3.f,-frame_matrix[14]};
      float view[16],projection[16],render[16];
      bx::mtxLookAt(view,eye,bx::add(eye,bx::Vec3{std::cos(yaw),.05f,std::sin(yaw)}));
      bx::mtxProj(projection,60.f,float(width)/height,.1f,5000.f,bgfx::getCaps()->homogeneousDepth);
      std::copy(projection,projection+16,render);
      for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
      host_enhanced_graphics().ao_method=ao?2:0;host_enhanced_graphics().ao_quality=2;
      host_enhanced_graphics().ao_radius=300;host_enhanced_graphics().ao_strength=100;
      b.debug_overlay_.render_options().map_shadows=shadows;
      std::vector<uint8_t> image(size_t(width)*height*4);
      for(int frame=0;frame<4;++frame) {
        ++b.frames_;
        scene_instances_frame_cache(true);
        b.sync_instances(view,projection);
        b.update_local_light();
        VideoOptions video;video.shadows=2;
        const float script_eye[3]={eye.x,eye.y,-eye.z};
        b.update_static_shadow(script_eye,video);
        b.update_ambient_occlusion(view,projection,width,height,view_id);
        b.draw_previews();
        bgfx::setViewFrameBuffer(view_id,fb);bgfx::setViewRect(view_id,0,0,width,height);
        bgfx::setViewClear(view_id,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0x8090a0ff),0.f);
        bgfx::setViewTransform(view_id,view,render);
        b.draw_sky(view_id,view,render);
        b.draw_dynamic_world(view_id);
        scene_instances_frame_cache(false);
        bgfx::TextureRegion dst,src;
        if(frame==3){dst.init(readback);src.init(color);bgfx::blit(100,dst,src);}
        bgfx::frame();
        if(frame==3) {const auto ready=bgfx::read(dst,image.data());for(int i=0;i<16&&bgfx::frame()<ready;++i){}}
      }
      return image;
    };
    {
      // Heading 1 faces the start gantry and the mountains behind it.
      const auto image=frame_at(1.5707963f,true,true);
      // The terrain mesh and its baked vertex light.
      size_t dark_vertices=0,rock_vertices=0;
      for(const auto& inst:b.instances_) {
        const auto& g=*inst.geometry;
        if(!g.ground_polygons) continue;
        for(const auto& group:g.groups)
          if(group.texture_index<g.textures.size()&&fs::path(g.textures[group.texture_index]).filename()=="sziklafal2.dds")
            for(uint32_t k=group.index_start;k<group.index_start+group.index_count;++k) {
              ++rock_vertices;dark_vertices+=(g.vertex_colors[g.indices[k]]&0xffu)<26;
            }
      }
      size_t dark=0,pixels=0;double sum=0;
      for(int y=310;y<360;++y) for(int x=600;x<1150;++x) {
        const uint8_t* px=&image[(size_t(y)*width+x)*4];
        const double lum=0.2126*px[0]+0.7152*px[1]+0.0722*px[2];
        dark+=lum<40.0;sum+=lum;++pixels;
      }
      std::printf("DESERT noon mountains: %zu of %zu rock vertex colours under 0.1; mountain band %.1f %% under luminance 40, mean %.1f\n",
                  dark_vertices,rock_vertices,100.0*double(dark)/double(pixels),sum/double(pixels));
      SLRR_REQUIRE(rock_vertices>1000&&dark_vertices*5>rock_vertices);  // the data still bakes dark vertices
      // Before #275: 72 % of the band under 40 (mean 54).
      SLRR_REQUIRE(dark*5<pixels);
    }
    b.debug_overlay_.render_options().map_shadows=true;
    bgfx::setViewMode(view_id,bgfx::ViewMode::Default);b.sequential_views_.reset(view_id);
    bgfx::setViewOrder();
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    b.static_shadow_active_=false;b.shadow_cascades_active_=false;b.shadow_cache_valid_=false;b.ao_active_=false;
    b.width_=saved_width;b.height_=saved_height;
    b.scenery_lamps_.clear();b.instances_.clear();b.prop_batches_.clear();b.night_batches_.clear();b.outdoor_lighting_=false;
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    for(auto* child:std::vector<void*>(gameref_get(root)->children)) gameref_remove(child);
    gameref_remove(root);scene_instances_set_resources(nullptr);
    std::puts("PASS desert mountains lit by the sun, not by baked vertex light");
  }
  // #271: `day` renders the crowd street at noon with the sun shadow passes
  // (update_static_shadow: cascades, map and walker casters) before the world.
  // #270: `workers` runs the night crowd street with the passes that may go
  // to worker encoders (AO prepass, car lamp tiles, a reflection face) queued
  // as in a game frame, then renders it again on one thread and compares.
  static void city_submission(LibBgfxBackend& b, uint16_t width, uint16_t height, bool crowd=false,
                              bool day=false, bool workers=false) {
    Fixture f;
    const auto game=fs::absolute("game");
    f.vm.set_game_root(game.string());f.packs.set_root(game.string());f.packs.open_all();
    scene_instances_set_resources(&f.packs);
    const auto pack=f.packs.open_lib("maps/city.rpk");
    const auto ground_id=RpkIndex::make_full_id(pack,1);
    Object* root=f.vm.new_object("java.util.resource.GroundRef");
    gameref_upsert(root,GameRefKind::GroundRef)->type_id=static_cast<int32_t>(ground_id);
    gameref_get(root)->flags|=kWorldTreeRoot;
    spawn_ground_shell(f.vm,f.packs,root);
    scene_instances_select(root,nullptr,false,SceneEnvironment::Exterior);
    float frame_matrix[16];
    SLRR_REQUIRE(scene_instances_ground_frame(root,frame_matrix));
    const auto saved=host_enhanced_graphics();const float old_time=scene_sun_time();
    auto saved_parts=std::move(b.parts_);auto saved_batches=std::move(b.prop_batches_);
    b.parts_.clear();b.prop_batches_.clear();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    host_enhanced_graphics().ao_method=0;
    b.garage_=false;b.outdoor_lighting_=true;b.fog_enabled_=false;
    b.interior_sun_active_=false;b.static_shadow_active_=false;b.car_depth_active_=false;
    b.fixture_shadow_active_=false;b.point_light_.reset();b.local_light_={};b.fixture_lights_.clear();
    scene_sun_set_time(day?13.5f*3600.f:22*3600.f);
    const int32_t saved_width=b.width_,saved_height=b.height_;
    b.width_=width;b.height_=height;
    struct Walker { Object* render; float phase; };
    std::vector<Walker> walkers;
    std::vector<Object*> extra_nodes;  // removed at the end, children first
    uint32_t paint_id=0;
    Object* preview_root=nullptr;
    if(crowd) {
      const int humans=f.packs.pack_by_name("humans.rpk")->pack_id;
      struct Kind { uint32_t render=0,anim=0; float sole=0.f; };
      // part_runtime.cpp:parse_ped_kind, for the test.
      const auto kind_of=[&](uint32_t type) {
        Kind kind;
        const auto bytes=f.packs.read_payload(type);
        std::string text(bytes.begin(),bytes.end());
        if(text.find("eof")==std::string::npos) {
          std::string relative=f.packs.payload_prop(type,"native");
          if(relative.rfind("ped ",0)==0||relative.rfind("ped\t",0)==0) relative.erase(0,4);
          else relative=f.packs.payload_prop(type,"sourcefile");
          while(!relative.empty()&&(relative.front()==' '||relative.front()=='\t')) relative.erase(relative.begin());
          std::ifstream file(game/relative,std::ios::binary);
          std::ostringstream buffer;buffer<<file.rdbuf();text=buffer.str();
        }
        std::istringstream lines(text);
        for(std::string line;std::getline(lines,line);) {
          std::istringstream words(line);std::string word,value;words>>word>>value;
          if(word=="render"&&!kind.render) kind.render=uint32_t(std::strtoul(value.c_str(),nullptr,0));
          else if(word=="anim"&&!kind.anim) kind.anim=uint32_t(std::strtoul(value.c_str(),nullptr,0));
        }
        const auto full=[&](uint32_t local) {
          const uint32_t resolved=local?f.packs.resolve_embedded_id(humans,local):0;
          return resolved?resolved:local?RpkIndex::make_full_id(humans,local):0u;
        };
        kind.render=full(kind.render);kind.anim=full(kind.anim);
        if(const auto mesh=scene_instances_geometry(kind.render))
          kind.sole=mesh->info.bbox_min[1]*(mesh->position_scale>0.f?mesh->position_scale:.01f);
        return kind;
      };
      std::vector<Kind> kinds;
      for(uint32_t id=0x57;id<=0x5B;++id)
        if(const auto kind=kind_of(RpkIndex::make_full_id(humans,id));kind.render) kinds.push_back(kind);
      SLRR_REQUIRE(!kinds.empty());
      if(day) scene_instances_prepare_physics();
      for(int i=0;i<80;++i) {
        const Kind& kind=kinds[size_t(i)%kinds.size()];
        Object* render=f.vm.new_object("java.util.resource.RenderRef");
        auto* node=gameref_upsert(render,GameRefKind::RenderRef);
        gameref_set_type(*node,int32_t(kind.render));
        node->script=render;node->alias="pedestrian";node->empty=false;node->matrix_override=true;
        gameref_set_parent(render,root);
        // 60 along the street ahead (4-80 m, both pavements), 20 behind.
        const float ahead=i<60?4.f+float(i)*1.3f:-6.f-float(i-60)*2.f;
        const float side=(i%2?3.2f:-3.2f)+float(i%5)*.3f;
        const float yaw=float(i)*.7f,cy=std::cos(yaw),sy=std::sin(yaw);
        float* m=node->local_matrix;std::fill(m,m+16,0.f);
        m[0]=cy;m[2]=-sy;m[8]=sy;m[10]=cy;m[5]=m[15]=1.f;
        m[12]=frame_matrix[12]+ahead;m[13]=frame_matrix[13]-kind.sole;m[14]=frame_matrix[14]+side;
        // Feet on the street surface (#271: the daytime case needs their shadows).
        float floor_y=0.f;
        if(day&&scene_instances_floor(m[12],m[14],frame_matrix[13]+6.f,nullptr,&floor_y)) m[13]=floor_y-kind.sole;
        if(kind.anim) {character_bind_animation(render,render,int32_t(kind.anim),f.packs);character_play(render,true);}
        walkers.push_back({render,.11f*float(i)});
      }
      PaintImage image;image.width=image.height=4;image.rgba.assign(64,200);
      paint_id=paint_texture_create(image);
      // The car: a chassis node and 30 renders below it, two levels deep.
      const auto* cars=f.packs.pack_by_name("cars.rpk");
      std::vector<uint32_t> parts;
      for(const auto& entry:cars->entries) {
        if(parts.size()>=30) break;
        if(f.packs.model_for_resource(cars->pack_id,entry.type_id&0xffffu).ok)
          parts.push_back(RpkIndex::make_full_id(cars->pack_id,entry.type_id&0xffffu));
      }
      SLRR_REQUIRE(parts.size()>=10);
      const auto build_car=[&](void* parent,const float* at) {
        Object* chassis=f.vm.new_object("java.util.resource.RenderRef");
        auto* node=gameref_upsert(chassis,GameRefKind::RenderRef);node->script=chassis;
        // #270 needs the car where its lamp tiles are; the older cases keep
        // their captures (the chassis matrix was ignored there).
        node->matrix_override=workers;
        gameref_set_parent(chassis,parent);
        float* m=node->local_matrix;std::fill(m,m+16,0.f);m[0]=m[5]=m[10]=m[15]=1.f;
        m[12]=at[0];m[13]=at[1];m[14]=at[2];
        std::vector<Object*> nodes{chassis};
        void* group=chassis;
        for(size_t i=0;i<parts.size();++i) {
          Object* part=f.vm.new_object("java.util.resource.RenderRef");
          auto* pn=gameref_upsert(part,GameRefKind::RenderRef);
          gameref_set_type(*pn,int32_t(parts[i]));
          pn->script=part;pn->empty=false;pn->vehicle_material=workers;
          pn->paint_source=paint_id;pn->texture_replacements={{paint_id,paint_id}};
          gameref_set_parent(part,i%3==0?chassis:group);
          float* pm=pn->local_matrix;std::fill(pm,pm+16,0.f);pm[0]=pm[5]=pm[10]=pm[15]=1.f;
          pm[12]=float(i%3)*.4f;
          if(i%3==0) group=part;
          nodes.push_back(part);
        }
        extra_nodes.insert(extra_nodes.end(),nodes.rbegin(),nodes.rend());
      };
      const float car_at[3]={frame_matrix[12]+14.f,frame_matrix[13],frame_matrix[14]+1.5f};
      build_car(root,car_at);
      // The preview: its own Dummy world, a camera in it and an inset viewport.
      preview_root=f.vm.new_object("java.util.resource.GameRef");
      gameref_upsert(preview_root,GameRefKind::Dummy);
      const float origin[3]={0.f,0.f,0.f};
      build_car(preview_root,origin);
      Object* camera=f.vm.new_object("java.util.resource.GameRef");
      Object* viewport=f.vm.new_object("java.util.resource.GameRef");
      auto* vp=gameref_upsert(viewport,GameRefKind::Viewport);
      vp->vp_x=.62f;vp->vp_y=.05f;vp->vp_w=.33f;vp->vp_h=.33f;vp->vp_pri=5;vp->vp_active=true;
      auto* cam=gameref_upsert(camera,GameRefKind::Camera);
      gameref_set_parent(camera,preview_root);
      cam->pos[0]=0.f;cam->pos[1]=1.f;cam->pos[2]=-6.f;cam->half_aov=40.f;cam->dmin=.1f;cam->dmax=200.f;
      gameref_activate_camera(*cam,*vp,5);
      extra_nodes.push_back(camera);extra_nodes.push_back(viewport);extra_nodes.push_back(preview_root);
    }
    const auto color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_RT);
    const auto depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[]={color,depth};const auto fb=bgfx::createFrameBuffer(2,attachments,true);
    const auto readback=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    // Street level, looking along +x (render space), as test_prop_lod does.
    const bx::Vec3 eye{frame_matrix[12],frame_matrix[13]+2.f,-frame_matrix[14]};
    float view[16],projection[16],render[16];
    bx::mtxLookAt(view,eye,bx::add(eye,bx::Vec3{1.f,0.f,.2f}));
    bx::mtxProj(projection,60.f,float(width)/height,.05f,5000.f,bgfx::getCaps()->homogeneousDepth);
    std::copy(projection,projection+16,render);
    for(int i:{2,6,10,14}) render[i]=bgfx::getCaps()->homogeneousDepth?-render[i]:render[i+1]-render[i];
    const uint16_t view_id=5;
    b.set_view_sequential(view_id);
    // #262: the uniform value cache, the kept texture stages and the
    // residency skip are measured on and off; both captures must match.
    struct Run { double sync=0,submit=0,render=0,gpu=0,headlight_gpu=0; uint32_t draws=0,world_draws=0; std::vector<uint8_t> image;
                 perf::Snapshot zones;
                 uint64_t uniform_sets=0,uniform_skips=0,texture_sets=0,texture_skips=0,residency_skips=0; };
    // #266: `cold` overwrites 96 MB before every frame, as scripts, physics
    // and the rest of a game frame evict the caches between two collections.
    static std::vector<uint8_t> evict(96u<<20);
    perf::enable("");
    auto run=[&](bool caches,int frames,int warmup,bool cold=false) {
      b.uniform_cache_=b.texture_cache_=b.residency_cache_=caches;
      Run out;int samples=0;
      const auto u0=b.main_bind_.uniform_sets,s0=b.main_bind_.uniform_skips,t0=b.main_bind_.texture_sets,k0=b.main_bind_.texture_skips,r0=b.residency_skips_;
      for(int frame=0;frame<frames;++frame) {
        ++b.frames_;
        // Walkers animate every frame (a fresh pose to build), yet show the
        // same pose on the captured frame of every run: the clock ends at the
        // same value in every run, so the last step rounds the same way.
        character_set_sim_time(100.+double(frame-frames+1)/60.);
        for(const auto& walker:walkers) character_seek(walker.render,walker.phase);
        if(frame==warmup-1) perf::restart(frames-warmup);
        if(cold) std::memset(evict.data(),frame&0xff,evict.size());
        scene_instances_frame_cache(true);
        auto start=std::chrono::steady_clock::now();
        b.sync_instances(view,projection);
        const double sync=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        b.update_local_light();
        if(workers) {
          // The frame order of LibBgfxBackend::flush between update_local_light
          // and the world (#270).
          b.resolved_textures_.clear();
          // Lamp shadow slots fade in real time; full weight at once keeps
          // the runs comparable on a slow (pinned) machine.
          b.lamp_shadow_dt_override_=10.f;
          b.defer_submits_=b.parallel_submission_ready();
          VideoOptions video;video.shadows=2;
          if(day) {
            // #359: the sun cascades queue one job each, as in a game frame.
            const float script_eye[3]={frame_matrix[12],frame_matrix[13]+2.f,frame_matrix[14]};
            b.update_static_shadow(script_eye,video);
          }
          const float car_script[3]={frame_matrix[12]+14.f,frame_matrix[13],frame_matrix[14]+1.5f};
          b.set_lamp_shadow_focus(car_script);
          b.update_lamp_shadows(video);
          b.update_headlight_shadows(video);  // #283; no car lights unless the cost run sets them
          host_enhanced_graphics().ao_method=2;host_enhanced_graphics().ao_quality=2;
          host_enhanced_graphics().ao_radius=300;host_enhanced_graphics().ao_strength=100;
          b.update_ambient_occlusion(view,projection,width,height,view_id);
          b.reflection_scene_=true;
          const float probe[3]={frame_matrix[12]+14.f,frame_matrix[13]+.5f,frame_matrix[14]+1.5f};
          b.update_reflections(probe);
          b.upload_light_data();
          b.defer_submits_=false;
        }
        if(day && !workers) {
          VideoOptions video;video.shadows=2;
          const float script_eye[3]={frame_matrix[12],frame_matrix[13]+2.f,frame_matrix[14]};
          b.update_static_shadow(script_eye,video);
          // The game runs the lamp passes every frame, day or night (#271).
          b.set_lamp_shadow_focus(script_eye);
          b.update_lamp_shadows(video);
          static int lamp_report=0;
          if(lamp_report++==0)
            std::printf("CITY FRAME day lamps: %zu lit, %zu shadow slots, %zu car tiles, atlas bound %d, registered %zu\n",
                        b.scenery_lamps_.size(),b.lamp_shadow_count_,b.car_shadow_tiles_.size(),
                        int(b.lamp_shadow_atlas_bound_),scenery_lights_count());
          // Elias's settings: GTAO High, 3 m radius.
          host_enhanced_graphics().ao_method=2;host_enhanced_graphics().ao_quality=2;
          host_enhanced_graphics().ao_radius=300;host_enhanced_graphics().ao_strength=100;
          b.update_ambient_occlusion(view,projection,width,height,view_id);
        }
        bgfx::setViewFrameBuffer(view_id,fb);bgfx::setViewRect(view_id,0,0,width,height);
        bgfx::setViewClear(view_id,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0xff),0.f);
        bgfx::setViewTransform(view_id,view,render);
        b.view_draws_.fill(0);
        start=std::chrono::steady_clock::now();
        if(workers) {
          // The game's world submission (#359): with worker encoders the
          // opaque part goes to kWorldOpaqueView on a worker, ordered first.
          b.apply_base_view_order();
          b.submit_world(view_id,fb,{0,0,width,height},BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,0xff,view,render,false);
        }
        else b.draw_dynamic_world(view_id);
        if(crowd) {bgfx::setViewFrameBuffer(LibBgfxBackend::kPreview0,fb);b.draw_previews();}
        const double submit=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        out.world_draws=b.view_draws_[view_id];
        scene_instances_frame_cache(false);
        const bool last=frame==frames-1;
        bgfx::TextureRegion dst,src;
        if(last){dst.init(readback);src.init(color);bgfx::blit(100,dst,src);}
        bgfx::frame();
        if(frame>=warmup-1) perf::end_frame();
        const auto* stats=bgfx::getStats();
        if(frame>=warmup) {
          out.sync+=sync;out.submit+=submit;++samples;out.draws=stats->numDraw;
          out.render+=double(stats->cpuTimeEnd-stats->cpuTimeBegin)*1000.0/double(stats->cpuTimerFreq);
          if(stats->gpuTimerFreq>0) {
            out.gpu+=double(stats->gpuTimeEnd-stats->gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
            for(uint16_t i=0;i<stats->numViews;++i) {
              const auto& v=stats->viewStats[i];
              if(v.view>=LibBgfxBackend::kHeadlightShadowView && v.view<LibBgfxBackend::kHeadlightShadowView+LibBgfxBackend::kHeadlightShadows)
                out.headlight_gpu+=double(v.gpuTimeEnd-v.gpuTimeBegin)*1000.0/double(stats->gpuTimerFreq);
            }
          }
        }
        if(last) {
          out.image.resize(size_t(width)*height*4);
          const auto ready=bgfx::read(dst,out.image.data());
          for(int i=0;i<16&&bgfx::frame()<ready;++i) {}
        }
      }
      out.sync/=samples;out.submit/=samples;out.render/=samples;out.gpu/=samples;out.headlight_gpu/=samples;
      out.zones=perf::snapshot();
      out.uniform_sets=(b.main_bind_.uniform_sets-u0)/frames;out.uniform_skips=(b.main_bind_.uniform_skips-s0)/frames;
      out.texture_sets=(b.main_bind_.texture_sets-t0)/frames;out.texture_skips=(b.main_bind_.texture_skips-k0)/frames;
      out.residency_skips=b.residency_skips_-r0;
      b.uniform_cache_=b.texture_cache_=b.residency_cache_=true;
      return out;
    };
    if(workers) {
      // Threaded against single-threaded, same frames; then the timings.
      const bool saved_workers=render_debug().worker_encoders;
      render_debug().worker_encoders=true;
      const auto runs0=b.worker_submit_runs_;
      const auto threaded=run(true,40,15);
      const auto threaded_runs=b.worker_submit_runs_-runs0;
      b.parallel_submission_=false;
      const auto single=run(true,40,15);
      const auto single_runs=b.worker_submit_runs_-runs0-threaded_runs;
      b.parallel_submission_=true;
      render_debug().worker_encoders=saved_workers;
      size_t differ=0;int worst=0;
      for(size_t i=0;i<threaded.image.size()&&i<single.image.size();++i) {
        const int d=std::abs(int(threaded.image[i])-int(single.image[i]));differ+=d>0;worst=std::max(worst,d);
      }
      uint64_t lit=0;for(size_t i=0;i<threaded.image.size();i+=4) lit+=threaded.image[i]+threaded.image[i+1]+threaded.image[i+2];
      std::printf("WORKERS %ux%u (%zu pool threads): car tiles %zu, AO %d, reflection ready %d; threaded frames %llu, single %llu\n",
                  width,height,jobs_concurrency(),b.car_shadow_tiles_.size(),int(b.ao_active_),int(b.reflection_ready_),
                  (unsigned long long)threaded_runs,(unsigned long long)single_runs);
      for(const auto* r:{&single,&threaded})
        std::printf("WORKERS %s: host submission %.3f ms (world + wait), worker submission %.3f ms, host wait %.3f ms, "
                    "render thread %.3f ms, %u draws\n",r==&single?"one thread":"workers  ",r->submit,
                    r->zones.zones[size_t(perf::Zone::WorkerSubmit)],r->zones.zones[size_t(perf::Zone::SubmitWait)],
                    r->render,r->draws);
      std::printf("WORKERS threaded vs one thread: %zu bytes differ, max %d (sum %llu)\n",differ,worst,(unsigned long long)lit);
      SLRR_REQUIRE(lit>0);
      SLRR_REQUIRE(single_runs==0);
      SLRR_REQUIRE(jobs_concurrency()<3||threaded_runs>0);
      SLRR_REQUIRE(differ<=64&&worst<=1);  // the known 1-LSB flake at most
      // #283: headlight shadow cost in this frame: the followed car's two
      // headlights and four of traffic ahead, beams along the street.
      {
        const float cx=frame_matrix[12]+14.f,cy=frame_matrix[13],cz=-(frame_matrix[14]+1.5f);
        std::vector<CarLight> beams;
        static Object followed,traffic;
        for(int k=0;k<3;++k) for(float side:{-.7f,.7f}) {
          CarLight l;l.kind=CarLightKind::Headlight;l.car=k==0?&followed:&traffic;
          l.part=reinterpret_cast<Object*>(uintptr_t(0x2000+k*16+(side>0)));
          l.position={cx+2.f+12.f*k,cy+.7f,cz+side};l.direction={.996f,-.087f,0.f};
          l.diffuse={.2f,.2f,.2f};l.attenuation={1,.03f,0};l.range=400;beams.push_back(l);
        }
        b.car_lights_override_=beams;b.set_headlight_focus(&followed);
        bgfx::setDebug(BGFX_DEBUG_PROFILER);
        render_debug().headlight_shadows=false;
        const auto without=run(true,40,15);
        render_debug().headlight_shadows=true;
        const auto with=run(true,40,15);
        const size_t tiles=b.headlight_shadow_count_;
        bgfx::setDebug(BGFX_DEBUG_NONE);
        b.car_lights_override_.reset();b.set_headlight_focus(nullptr);
        for(const auto* r:{&without,&with})
          std::printf("HEADLIGHT SHADOW city %ux%u %s: host submission %.3f ms, render thread %.3f ms, GPU %.3f ms "
                      "(headlight tiles %.3f ms), %u draws\n",width,height,r==&with?"6 tiles":"off    ",
                      r->submit,r->render,r->gpu,r->headlight_gpu,r->draws);
        std::printf("HEADLIGHT SHADOW city: %zu tiles of %u px, %u more draws\n",tiles,unsigned(b.headlight_shadow_tile_),
                    with.draws>without.draws?with.draws-without.draws:0u);
        SLRR_REQUIRE(tiles==6 && with.draws>without.draws);
      }
    }
    const auto off=run(false,30,10);
    const auto on=run(true,40,15);
    const auto cold=run(true,40,15,true);
    for(const auto* r:{&off,&on,&cold})
      std::printf("CITY FRAME %ux%u caches %s: sync_instances %.3f ms, world submission %.3f ms, render thread %.3f ms, "
                  "%u draws, %zu instances, %zu batch slots; per frame %llu uniform sets / %llu skipped, "
                  "%llu texture sets / %llu skipped; %llu residency skips; %u world-view draws\n",width,height,
                  r==&on?"on ":r==&cold?"on, cold":"off",
                  r->sync,r->submit,r->render,r->draws,b.instances_.size(),b.prop_batches_.size(),
                  (unsigned long long)r->uniform_sets,(unsigned long long)r->uniform_skips,
                  (unsigned long long)r->texture_sets,(unsigned long long)r->texture_skips,
                  (unsigned long long)r->residency_skips,r->world_draws);
    for(const auto* r:{&on,&cold}) {
      std::string line;char item[96];
      const std::pair<perf::Zone,const char*> names[]={{perf::Zone::CollectBatches,"batches"},{perf::Zone::WorldList,"world list"},{perf::Zone::WorldCopy,"world copy"},
          {perf::Zone::CollectInstances,"instances"},{perf::Zone::CollectSkins,"skins"},
          {perf::Zone::CollectPreviews,"previews"},{perf::Zone::Residency,"residency"}};
      for(const auto& [zone,name]:names){std::snprintf(item,sizeof(item)," %s %.3f",name,r->zones.zones[size_t(zone)]);line+=item;}
      std::printf("CITY FRAME %ux%u zones (%s, %zu job threads):%s ms\n",width,height,r==&on?"warm":"cold",
                  jobs_concurrency(),line.c_str());
    }
    {
      // #266: the pooled batch loop against the same loop run serially (a
      // batch started inside a job runs on its own thread), same frame state.
      const auto frustum=LibBgfxBackend::view_frustum(view,projection);
      std::vector<SceneBatch> pooled,serial;
      scene_instances_frame_cache(true);
      scene_instances_set_size_filter(&LibBgfxBackend::screen_size_keep,&b);
      scene_instances_collect_batches(&frustum,pooled);
      jobs_parallel_for(2,[&](size_t i){if(i==0) scene_instances_collect_batches(&frustum,serial);});
      scene_instances_set_size_filter(nullptr,nullptr);
      scene_instances_frame_cache(false);
      bool same=pooled.size()==serial.size();size_t placements=0;
      for(size_t i=0;same&&i<pooled.size();++i){
        same=pooled[i].geometry==serial[i].geometry&&pooled[i].instances.size()==serial[i].instances.size();
        for(size_t k=0;same&&k<pooled[i].instances.size();++k)
          same=pooled[i].instances[k].transform==serial[i].instances[k].transform&&
               pooled[i].instances[k].color==serial[i].instances[k].color;
        placements+=pooled[i].instances.size();
      }
      std::printf("CITY FRAME %ux%u pooled vs serial batches: %s (%zu placements, %zu job threads)\n",
                  width,height,same?"identical":"DIFFERENT",placements,jobs_concurrency());
      SLRR_REQUIRE(same);
    }
    {
      size_t night=0,night_meshes=0,blended=0;
      for(const auto& batch:b.night_batches_) if(!batch.instances.empty()){night+=batch.instances.size();++night_meshes;}
      for(const auto& instance:b.instances_) blended+=LibBgfxBackend::blended_prop_instance(instance);
      std::printf("CITY FRAME %ux%u night overlays: %zu placements in %zu meshes; %zu blended prop instances\n",
                  width,height,night,night_meshes,blended);
    }
    const auto compare=[](const std::vector<uint8_t>& a,const std::vector<uint8_t>& c,int& worst) {
      size_t differ=0;worst=0;
      for(size_t i=0;i<a.size()&&i<c.size();++i){const int d=std::abs(int(a[i])-int(c[i]));differ+=d>0;worst=std::max(worst,d);}
      return differ;
    };
    int worst=0;
    const size_t differ=compare(off.image,on.image,worst);
    uint64_t lit=0;for(size_t i=0;i<on.image.size();i+=4) lit+=on.image[i]+on.image[i+1]+on.image[i+2];
    std::printf("CITY FRAME caches on vs off: %zu bytes differ, max %d (sum %llu)\n",differ,worst,(unsigned long long)lit);
    // A known flake, older than #266 (docs/rendering/performance.md, #266 and #268):
    // about one run in three a capture differs from another run's by 1 in a
    // few dozen scattered lamp-lit pixels. Anything larger is a real change.
    const auto flake=[](size_t bytes,int max){return bytes<=64&&max<=1;};
    SLRR_REQUIRE(differ==0||flake(differ,worst));
    const size_t cold_differ=compare(on.image,cold.image,worst);
    std::printf("CITY FRAME warm vs cold capture: %zu bytes differ, max %d\n",cold_differ,worst);
    SLRR_REQUIRE(lit>0);
    SLRR_REQUIRE(on.uniform_skips>0&&on.texture_skips>0&&on.residency_skips>0);
    SLRR_REQUIRE(off.uniform_skips==0&&off.texture_skips==0&&off.residency_skips==0);
    if(day) {
      // #271: street lamps against the sun. Each hour renders the street with
      // street lamp lighting on and off; the difference is what the lamps add
      // (their light and their shadow tiles). At noon nothing, at dusk a faint
      // contribution, at night the full lamps.
      std::printf("CITY FRAME sun by hour (old clock gate; elevation deg, direct sun / noon, night factor):\n");
      for(float hour:{5.5f,5.75f,5.9f,6.f,6.25f,6.5f,12.f,17.f,17.5f,17.75f,17.9f,18.f,18.1f,18.25f,18.5f,22.f}) {
        scene_sun_set_time(hour*3600.f);
        const auto sun=scene_instances_sun_state();
        std::printf("CITY FRAME   %5.2fh gate %d elevation %6.2f direct %.3f sun shadows %d night %.3f\n",hour,
                    int(scene_sun_is_night()),sun.elevation*57.2958f,sun.direct,int(sun.shadowing),sun.night);
      }
      const float saved_dt=b.lamp_shadow_dt_override_;
      b.lamp_shadow_dt_override_=10.f;  // shadow slots at full weight at once
      struct Hour { float hour; const char* name; };
      uint64_t previous=0;
      for(const Hour hour:{Hour{12.f,"noon"},Hour{18.f,"dusk"},Hour{22.f,"night"}}) {
        scene_sun_set_time(hour.hour*3600.f);
        render_debug().lamp_lighting=true;
        const auto lit=run(true,6,2);
        const size_t lamps=b.scenery_lamps_.size(),shadowed=b.lamp_shadow_count_;
        float strongest=0.f;
        for(const auto& lamp:b.scenery_lamps_) strongest=std::max(strongest,lamp.intensity);
        render_debug().lamp_lighting=false;
        const auto dark=run(true,6,2);
        render_debug().lamp_lighting=true;
        uint64_t added=0;size_t pixels=0;
        for(size_t i=0;i+3<lit.image.size();i+=4) {
          int d=0;for(int c=0;c<3;++c) d+=std::abs(int(lit.image[i+c])-int(dark.image[i+c]));
          added+=uint64_t(d);pixels+=d>0;
        }
        std::printf("CITY FRAME lamps at %s (%.2fh, factor %.3f): %zu lit, %zu shadowed, strongest %.2f; "
                    "lamp contribution %llu over %zu pixels\n",hour.name,hour.hour,scene_instances_sun_state().night,
                    lamps,shadowed,strongest,(unsigned long long)added,pixels);
        if(hour.hour==12.f) SLRR_REQUIRE(lamps==0&&shadowed==0&&added==0);
        else SLRR_REQUIRE(lamps>0&&added>previous);
        // Dusk: the sun still casts its shadow; lamps are faint and cast none.
        if(hour.hour==18.f) SLRR_REQUIRE(strongest>0.f&&strongest<.2f*kSceneryLightIntensity&&shadowed==0&&
                                         b.static_shadow_active_);
        if(hour.hour==22.f) SLRR_REQUIRE(strongest>=.99f*kSceneryLightIntensity);
        previous=added;
      }
      b.lamp_shadow_dt_override_=saved_dt;
    }
    // An earlier build's capture (#254, #262, #264): compared when present,
    // written when absent. Delete the file to take a new reference.
    char reference[96];
    std::snprintf(reference,sizeof(reference),"build/city_frame_reference_%ux%u%s%s.bin",width,height,
                  crowd?"_crowd":"",day?"_day":"");
    if(FILE* in=std::fopen(reference,"rb")) {
      std::vector<uint8_t> base(on.image.size());
      base.resize(std::fread(base.data(),1,base.size(),in));std::fclose(in);
      size_t pixels=0;
      for(size_t i=0;i+3<base.size()&&i+3<on.image.size();i+=4)
        pixels+=std::memcmp(&base[i],&on.image[i],4)!=0;
      std::printf("CITY FRAME %ux%u vs reference capture: %zu bytes differ in %zu pixels, max %d\n",
                  width,height,compare(base,on.image,worst),pixels,worst);
    } else if(FILE* out=std::fopen(reference,"wb")) {
      std::fwrite(on.image.data(),1,on.image.size(),out);std::fclose(out);
      std::printf("CITY FRAME %ux%u reference capture written\n",width,height);
    }
    b.width_=saved_width;b.height_=saved_height;
    bgfx::setViewMode(view_id,bgfx::ViewMode::Default);
    b.sequential_views_.reset(view_id);
    bgfx::frame();bgfx::destroy(fb);bgfx::destroy(readback);
    host_enhanced_graphics()=saved;scene_sun_set_time(old_time);
    if(day) {b.static_shadow_active_=false;b.shadow_cascades_active_=false;b.shadow_cache_valid_=false;}
    if(workers) {b.reflection_scene_=false;b.destroy_reflections();b.destroy_ao_targets();b.lamp_shadow_dt_override_=-1.f;}
    for(const auto& walker:walkers) {character_release_animation(walker.render);gameref_remove(walker.render);}
    for(auto* node:extra_nodes) gameref_remove(node);
    if(paint_id) paint_texture_release(paint_id);
    b.previews_.clear();
    if(crowd) {
      // #268: the camera list read every node of the scene, several times a
      // frame. A game scene has thousands of GameRefs (cars, walkers, OSD and
      // script objects); time the list with 6,000 more nodes than the harness.
      std::vector<Object> filler(6000);
      for(auto& node:filler) gameref_upsert(&node);
      auto t0=std::chrono::steady_clock::now();
      size_t sink=0;
      for(int i=0;i<200;++i) sink+=gameref_nodes_with_mesh().size();
      const double scan=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/200.;
      t0=std::chrono::steady_clock::now();
      for(int i=0;i<200;++i) sink+=gameref_active_cameras().size();
      const double cameras=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/200.;
      std::printf("CITY FRAME camera list with %zu extra nodes: %.1f us per call (one full node scan, the old list: %.1f us) [%zu]\n",
                  filler.size(),cameras,scan,sink);
      for(auto& node:filler) gameref_remove(&node);
      // draw_previews changed the view order and view 8; later cases expect neither.
      bgfx::setViewOrder();
      bgfx::setViewFrameBuffer(LibBgfxBackend::kPreview0,BGFX_INVALID_HANDLE);
      bgfx::setViewMode(LibBgfxBackend::kPreview0,bgfx::ViewMode::Default);
      b.sequential_views_.reset(LibBgfxBackend::kPreview0);
    }
    b.scenery_lamps_.clear();b.instances_.clear();b.prop_batches_.clear();b.night_batches_.clear();b.outdoor_lighting_=false;
    b.update_lamp_shadows(VideoOptions{});
    b.parts_=std::move(saved_parts);b.prop_batches_=std::move(saved_batches);
    for(auto* child:std::vector<void*>(gameref_get(root)->children)) gameref_remove(child);
    gameref_remove(root);scene_instances_set_resources(nullptr);
    std::puts("PASS city frame cost recorded");
  }
  static void reflection_occupants() {
    Fixture f;Object car,seat,person,pedestrian,world;
    auto* c=gameref_upsert(&car);c->vehicle_material=true;
    gameref_upsert(&seat);gameref_upsert(&person);gameref_upsert(&pedestrian);gameref_upsert(&world);
    gameref_set_parent(&person,&seat);gameref_set_parent(&seat,&car);
    gameref_set_parent(&pedestrian,&world);
    SceneInstance occupant;occupant.key=&person;
    SLRR_REQUIRE(!reflection_capture_visible(occupant));
    occupant.key=&pedestrian;SLRR_REQUIRE(reflection_capture_visible(occupant));
    gameref_set_parent(&person,&world);occupant.key=&person;
    SLRR_REQUIRE(reflection_capture_visible(occupant));
    for(auto* key:{&person,&pedestrian,&seat,&car,&world}) gameref_remove(key);
    std::puts("PASS reflection excludes seated subtree but preserves independent pedestrians");
  }

  static void point_lifetime(LibBgfxBackend &b) {
    Fixture f;
    scene_instances_set_resources(&f.packs);
    Object world, other, neon;
    gameref_upsert(&world)->flags = kWorldTreeRoot;
    gameref_upsert(&other)->flags = kWorldTreeRoot;
    auto *node = gameref_upsert(&neon, GameRefKind::RenderRef);
    node->type_id = 0x1d;
    node->directional_light.set = node->directional_light.has_direction = true;
    gameref_set_parent(&neon, &world);
    gameref_make_live(&neon);
    gameref_register_light(&neon);
    const auto saved = host_enhanced_graphics();
    host_enhanced_graphics().local_lights =
        host_enhanced_graphics().dynamic_lighting = true;
    b.garage_ = true;
    scene_instances_select(&world, nullptr, false, SceneEnvironment::Interior);
    b.update_local_light();
    SLRR_REQUIRE(b.point_light_ && b.point_light_->valid() &&
                 b.point_light_->owner == &neon);
    gameref_set_matrix(&neon, 2, 1, 3, 0, 0, 0);
    b.update_local_light();
    SLRR_REQUIRE(b.point_light_->position[0] == 2 &&
                 std::abs(b.point_light_->position[1] - 4.3f) < .0001f &&
                 b.point_light_->position[2] == -3);
    auto invalid = *b.point_light_;
    invalid.range = 0;
    SLRR_REQUIRE(!invalid.valid());
    invalid = *b.point_light_;
    invalid.intensity = std::numeric_limits<float>::quiet_NaN();
    SLRR_REQUIRE(!invalid.valid());
    host_enhanced_graphics().local_lights = false;
    b.update_local_light();
    SLRR_REQUIRE(!b.point_light_);
    host_enhanced_graphics().local_lights = true;
    b.update_local_light();
    SLRR_REQUIRE(b.point_light_);
    scene_instances_select(&other, nullptr, false, SceneEnvironment::Interior);
    b.update_local_light();
    SLRR_REQUIRE(!b.point_light_ && b.local_light_[3] == 0);
    host_enhanced_graphics() = saved;
    b.garage_ = false;
    gameref_remove(&neon);
    gameref_remove(&world);
    gameref_remove(&other);
    scene_instances_set_resources(nullptr);
    std::puts("PASS point-light source lifetime, invalid data, option toggle "
              "and world isolation");
  }
  static void vehicle_wall(LibBgfxBackend &b) {
    Fixture f;
    scene_instances_set_resources(&f.packs);
    Object root;
    gameref_upsert(&root)->flags = kWorldTreeRoot;
    scene_instances_select(&root, nullptr, false, SceneEnvironment::Exterior);
    scene_sun_set_time(9 * 3600.f);
    auto car = std::make_shared<ScxGeometry>();
    car->info.ok = true;
    car->info.version = 4;
    car->position_scale = 1;
    car->positions = {-.6f, 0, -1, .6f, 0, -1, .6f, 1.5f, -1, -.6f, 1.5f, -1,
                      -.6f, 0, 1,  .6f, 0, 1,  .6f, 1.5f, 1,  -.6f, 1.5f, 1};
    car->indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                    3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    car->info.bbox_min[0] = -.6f;
    car->info.bbox_min[1] = 0;
    car->info.bbox_min[2] = -1;
    car->info.bbox_max[0] = .6f;
    car->info.bbox_max[1] = 1.5f;
    car->info.bbox_max[2] = 1;
    car->normals.resize(car->positions.size(), 0);
    car->uvs.resize(16, 0);
    ScxGroup group;
    group.index_count = uint32_t(car->indices.size());
    car->groups.push_back(group);
    b.cache_scene_mesh(car, nullptr, false);
    SceneInstance caster;
    caster.geometry = car;
    caster.vehicle_material = true;
    bx::mtxIdentity(caster.transform.data());
    b.instances_ = {caster};
    b.garage_ = false;
    b.outdoor_lighting_ = true;
    b.local_light_ = {};
    b.static_shadow_active_ = false;
    VideoOptions video;
    video.shadows = 2;
    const float centre[3] = {0, 0, 0};
    b.update_car_depth(centre, video);
    bgfx::frame();
    SLRR_REQUIRE(b.car_depth_active_);
    auto wall = std::make_shared<ScxGeometry>();
    wall->info.ok = true;
    wall->info.version = 4;
    wall->position_scale = 1;
    wall->positions = {-1.3f, 0, -2, -1.3f, 2, -2, -1.3f, 0, 2, -1.3f, 2, 2};
    wall->normals = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
    wall->uvs.resize(8, 0);
    wall->indices = {0, 1, 2, 2, 1, 3};
    group.index_count = 6;
    wall->groups = {group};
    b.cache_scene_mesh(wall, nullptr, false);
    SceneInstance receiver;
    receiver.geometry = wall;
    receiver.lighting = scene_instances_light();
    bx::mtxIdentity(receiver.transform.data());
    constexpr uint16_t size = 128;
    auto color = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    auto depth = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT);
    bgfx::TextureHandle attachments[] = {color, depth};
    auto fb = bgfx::createFrameBuffer(2, attachments, true);
    auto readback =
        bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    float identity[16];
    bx::mtxIdentity(identity);
    float projection[16]{};
    projection[8] = .5f;
    projection[5] = 1;
    projection[13] = -1;
    projection[14] = .5f;
    projection[15] = 1;
    bool point_mode = false;
    uint16_t view = 5;
    float receiver_view[16];
    bx::mtxIdentity(receiver_view);
    auto capture = [&](bool active) {
      if (point_mode) {
        b.car_depth_active_ = false;
        b.static_shadow_active_ = active;
      } else
        b.car_depth_active_ = active;
      bgfx::setViewFrameBuffer(view, fb);
      bgfx::setViewRect(view, 0, 0, size, size);
      bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                         uint32_t(0xff), 0.f);
      bgfx::setViewTransform(view, receiver_view, projection);
      b.draw_instance(view, receiver, receiver.transform.data(), false, 0);
      bgfx::TextureRegion dst, src;
      dst.init(readback);
      src.init(color);
      bgfx::blit(100, dst, src);
      bgfx::frame();
      std::vector<uint8_t> bytes(size * size * 4);
      auto ready = bgfx::read(dst, bytes.data());
      for (int i = 0; i < 16; ++i)
        if (bgfx::frame() >= ready)
          return bytes;
      throw std::runtime_error("wall readback timeout");
    };
    auto lit = capture(false), shadow = capture(true);
    unsigned changed = 0;
    for (size_t i = 0; i < lit.size(); i += 4)
      if (int(lit[i]) - int(shadow[i]) > 10)
        ++changed;
    std::printf("Vehicle production depth -> vertical wall: %u shadow pixels\n",
                changed);
    SLRR_REQUIRE(changed > 100);
    PointLight point;
    point.owner = &root;
    point.position = {0, 0, 0};
    b.point_light_ = point;
    point_mode = true;
    b.outdoor_lighting_ = false;
    constexpr bx::Vec3 axes[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (unsigned face = 0; face < 6; ++face) {
      const auto axis = axes[face];
      const auto normal = bx::mul(axis, -1.f);
      const bx::Vec3 up =
          std::abs(normal.y) > .9f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
      const auto side = bx::cross(normal, up);
      bx::mtxIdentity(receiver.transform.data());
      for (int row = 0; row < 3; ++row) {
        receiver.transform[row] = (&normal.x)[row];
        receiver.transform[4 + row] = (&up.x)[row];
        receiver.transform[8 + row] = (&side.x)[row];
        receiver.transform[12 + row] = 2.7f * (&axis.x)[row] - (&up.x)[row];
      }
      bx::mtxInverse(receiver_view, receiver.transform.data());
      b.instances_[0].transform[12] = axis.x * 2;
      b.instances_[0].transform[13] = axis.y * 2 - .75f;
      b.instances_[0].transform[14] = axis.z * 2;
      b.update_static_shadow(centre, video);
      b.draw_previews(); // production ordering must place face/view 64 before
                         // world 5
      SLRR_REQUIRE(b.point_shadow_active_ && b.static_shadow_local_);
      auto occluded = capture(true), plain = capture(false);
      unsigned coverage = 0;
      for (size_t i = 0; i < plain.size(); i += 4)
        if (int(plain[i]) - int(occluded[i]) > 10)
          ++coverage;
      std::printf("Point light face %u: %u blocked wall pixels\n", face,
                  coverage);
      SLRR_REQUIRE(coverage > 100);
      if (face == 5) {
        view = 72;
        auto reflection = capture(true);
        SLRR_REQUIRE(reflection == occluded);
        view = 5;
      }
      // This must be a moving occluder, not a dark atlas or an unlit wall.
      for (int row = 0; row < 3; ++row)
        b.instances_[0].transform[12 + row] += (&up.x)[row] * 4;
      b.update_static_shadow(centre, video);
      bgfx::frame();
      auto revealed = capture(true);
      unsigned remaining = 0;
      for (size_t i = 0; i < plain.size(); i += 4)
        if (int(plain[i]) - int(revealed[i]) > 10)
          ++remaining;
      std::printf("Point light face %u: %u pixels blocked after lateral move\n",
                  face, remaining);
      SLRR_REQUIRE(remaining < coverage / 20);
      const auto revision = b.shadow_atlas_revision_;
      b.update_static_shadow(centre, video);
      bgfx::frame();
      SLRR_REQUIRE(b.shadow_atlas_revision_ == revision);
    }
    b.point_light_->position[0] += .1f;
    auto revision = b.shadow_atlas_revision_;
    b.update_static_shadow(centre, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision + 1);
    revision = b.shadow_atlas_revision_;
    b.point_light_->color = {.1f, .2f, 1.f};
    b.point_light_->intensity = 2;
    b.update_static_shadow(centre, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision);
    const auto blue = capture(false);
    const size_t pixel = (size / 2 * size + size / 2) * 4;
    SLRR_REQUIRE(blue[pixel + 2] > blue[pixel] + 10);
    b.point_light_->range = 1;
    b.update_static_shadow(centre, video);
    bgfx::frame();
    const auto outside = capture(false);
    SLRR_REQUIRE(std::abs(int(outside[pixel + 2]) - int(outside[pixel])) <= 1);
    std::puts("PASS point colour/intensity, radial range cutoff and "
              "unchanged-depth reuse");
    // A lit vertical body panel is also in its own wide-cone shadow map.
    // Grazing projection/PCF must not invent stripes across that same plane.
    auto panel=std::make_shared<ScxGeometry>();
    panel->info.ok=true;panel->info.version=4;panel->position_scale=1;
    panel->positions={-1.3f,0,-2,-1.3f,3,-2,-1.3f,0,2,-1.3f,3,2};
    panel->normals={1,0,0,1,0,0,1,0,0,1,0,0};panel->uvs.resize(8,0);
    panel->indices={0,1,2,2,1,3};panel->groups={group};panel->groups[0].index_count=6;
    panel->info.bbox_min[0]=panel->info.bbox_max[0]=-1.3f;
    panel->info.bbox_min[2]=-2;panel->info.bbox_max[2]=2;panel->info.bbox_max[1]=3;
    b.cache_scene_mesh(panel,nullptr,false);
    receiver.geometry=panel;receiver.vehicle_material=true;
    bx::mtxIdentity(receiver.transform.data());bx::mtxIdentity(receiver_view);
    std::fill(std::begin(projection),std::end(projection),0.f);
    projection[8]=.5f;projection[5]=2.f/3;projection[13]=-1;
    projection[14]=.5f;projection[15]=1;view=5;
    b.instances_={receiver};b.garage_=true;
    SpotLight lamp;lamp.position={1,6.5f,0};lamp.intensity=24;
    b.fixture_lights_={lamp};
    PointLight ambient;ambient.intensity=0;ambient.casts_shadows=false;
    b.point_light_=ambient;b.point_shadow_active_=false;b.car_depth_active_=false;
    b.update_fixture_shadows(video);b.draw_previews();bgfx::frame();
    b.fixture_shadow_active_=false;const auto clean_panel=capture(false);
    b.fixture_shadow_active_=true;const auto shadowed_panel=capture(false);
    unsigned acne=0;
    for(size_t i=0;i<clean_panel.size();i+=4)
      if(int(clean_panel[i])-int(shadowed_panel[i])>8) ++acne;
    std::printf("NEON grazing panel self-shadow stripes=%u pixels\n",acne);
    SLRR_REQUIRE(acne<size*size/100);
    // Put a vehicle 20 cm from the wall: its shadow must climb onto the
    // actual vertical receiver, not merely appear on a horizontal floor.
    receiver.vehicle_material=false;
    bx::mtxIdentity(caster.transform.data());caster.transform[12]=-.5f;
    b.instances_={receiver,caster};b.update_fixture_shadows(video);
    b.draw_previews();bgfx::frame();
    b.fixture_shadow_active_=false;const auto bare_wall=capture(false);
    b.fixture_shadow_active_=true;const auto car_wall=capture(false);
    unsigned wall_pixels=0;
    for(size_t i=0;i<bare_wall.size();i+=4)
      if(int(bare_wall[i])-int(car_wall[i])>8) ++wall_pixels;
    std::printf("NEON vehicle shadow on vertical wall=%u pixels\n",wall_pixels);
    SLRR_REQUIRE(wall_pixels>100);
    const auto saved_shadow_options=host_enhanced_graphics();
    for(int quality=0;quality<3;++quality) {
      host_enhanced_graphics().shadow_quality=quality;
      b.update_fixture_shadows(video);b.draw_previews();bgfx::frame();
      SLRR_REQUIRE(b.fixture_shadow_tile_==(256u<<quality));
      const auto revision=b.fixture_shadow_revision_;
      b.update_fixture_shadows(video);SLRR_REQUIRE(b.fixture_shadow_revision_==revision);
      std::vector<uint8_t> filters[3];
      for(int filter=0;filter<3;++filter) {
        host_enhanced_graphics().shadow_smoothing=filter;
        filters[filter]=capture(false);unsigned shaded=0;
        for(size_t i=0;i<bare_wall.size();i+=4)
          if(int(bare_wall[i])-int(filters[filter][i])>8) ++shaded;
        SLRR_REQUIRE(shaded>100);
        b.update_fixture_shadows(video);SLRR_REQUIRE(b.fixture_shadow_revision_==revision);
      }
      SLRR_REQUIRE(filters[0]!=filters[1] && filters[1]!=filters[2]);
    }
    host_enhanced_graphics()=saved_shadow_options;
    std::puts("PASS shadow quality reallocates, smoothing changes pixels without depth redraw");
    caster.transform[14]=5; b.instances_={receiver,caster};
    b.update_fixture_shadows(video);b.draw_previews();bgfx::frame();
    const auto cleared_wall=capture(false);unsigned remaining_wall=0;
    for(size_t i=0;i<bare_wall.size();i+=4)
      if(int(bare_wall[i])-int(cleared_wall[i])>8) ++remaining_wall;
    SLRR_REQUIRE(remaining_wall<wall_pixels/10);
    b.fixture_lights_.clear();b.fixture_shadow_active_=false;b.garage_=false;
    b.point_light_.reset();
    b.point_shadow_active_ = false;
    bgfx::destroy(readback);
    bgfx::destroy(fb);
    gameref_remove(&root);
    scene_instances_set_resources(nullptr);
    b.instances_.clear();
    b.car_depth_active_ = false;
  }
  static void garage_geometry(LibBgfxBackend &b) {
    Fixture f;
    f.packs.set_root(fs::absolute("game").string());
    f.packs.open_all();
    scene_instances_set_resources(&f.packs);
    const auto pack = f.packs.open_lib("misc/garage.rpk");
    Object root, stuff;
    root.class_name = "java.util.resource.GroundRef";
    gameref_upsert(&root, GameRefKind::GroundRef)->type_id =
        RpkIndex::make_full_id(pack, 0x7a);
    spawn_ground_shell(f.vm, f.packs, &root);
    gameref_upsert(&stuff, GameRefKind::RenderRef)->type_id =
        RpkIndex::make_full_id(pack, 0x42);
    gameref_set_parent(&stuff, &root);
    scene_instances_select(&root, nullptr, false, SceneEnvironment::Interior);
    b.instances_ = scene_instances_collect();
    for (const auto &obj : b.instances_)
      b.cache_scene_mesh(obj.geometry, nullptr, false);
    auto realFloor =
        std::make_shared<ScxGeometry>(*b.instances_.front().geometry);
    realFloor->indices.clear();
    realFloor->groups.clear();
    const auto &source = *b.instances_.front().geometry;
    for (const auto &original : source.groups) {
      auto group = original;
      group.index_start = uint32_t(realFloor->indices.size());
      for (size_t i = original.index_start;
           i + 2 < original.index_start + original.index_count; i += 3) {
        bool floor = true;
        for (int k = 0; k < 3; ++k) {
          auto v = source.indices[i + k] * 3;
          floor &= std::abs(source.positions[v + 1]) < .2f &&
                   source.normals[v + 1] > .5f;
        }
        if (floor)
          realFloor->indices.insert(realFloor->indices.end(),
                                    source.indices.begin() + i,
                                    source.indices.begin() + i + 3);
      }
      group.index_count =
          uint32_t(realFloor->indices.size()) - group.index_start;
      if (group.index_count)
        realFloor->groups.push_back(group);
    }
    SLRR_REQUIRE(!realFloor->indices.empty());
    b.garage_ = true;
    b.outdoor_lighting_ = false;
    b.local_light_ = {0, 3.3f, 0, 32};
    PointLight fixture_light;
    fixture_light.position = {0, 3.3f, 0};
    b.point_light_ = fixture_light;
    VideoOptions video;
    video.shadows = 2;
    const float eye[3] = {0, 2, -4};
    b.update_static_shadow(eye, video);
    bgfx::frame();
    // All room corners must lie inside the actual fitted shadow footprint.
    for (int corner = 0; corner < 4; ++corner) {
      bx::Vec3 point{
          corner & 1 ? source.info.bbox_max[0] : source.info.bbox_min[0], 0,
          -(corner & 2 ? source.info.bbox_max[2] : source.info.bbox_min[2])};
      const std::array<float, 3> delta{point.x - fixture_light.position[0],
                                       point.y - fixture_light.position[1],
                                       point.z - fixture_light.position[2]};
      const auto face = point_shadow_face(delta);
      const auto uv = bx::mulH({delta[0], delta[1], delta[2]},
                               b.point_shadow_matrices_[face].data());
      SLRR_REQUIRE(uv.x > 0 && uv.x < 1 && uv.y > 0 && uv.y < 1);
    }
    SLRR_REQUIRE(b.point_shadow_active_ && b.static_shadow_local_);
    constexpr uint16_t size = 256;
    const auto color = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    const auto depth = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[] = {color, depth};
    const auto fb = bgfx::createFrameBuffer(2, attachments, true);
    const auto readback =
        bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    auto floor = realFloor;
    b.cache_scene_mesh(floor, nullptr, false);
    SceneInstance receiver;
    receiver.geometry = floor;
    bx::mtxIdentity(receiver.transform.data());
    receiver.lighting.set = receiver.lighting.has_direction = true;
    for (int i = 0; i < 3; ++i) {
      receiver.lighting.diffuse[i] = .8f;
      receiver.lighting.ambient[i] = .1f;
    }
    float identity[16];
    bx::mtxIdentity(identity);
    float projection[16]{};
    projection[0] = projection[9] = .05f;
    projection[6] = .01f;
    projection[14] = .5f;
    projection[15] = 1;
    uint16_t capture_view=5;
    auto capture = [&](bool enabled) {
      b.static_shadow_active_ = enabled;
      bgfx::setViewFrameBuffer(capture_view, fb);
      bgfx::setViewRect(capture_view, 0, 0, size, size);
      bgfx::setViewClear(capture_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, uint32_t(0xff),
                         0.f);
      bgfx::setViewTransform(capture_view, identity, projection);
      b.draw_instance(capture_view, receiver, identity, false, 0);
      bgfx::TextureRegion dst, src;
      dst.init(readback);
      src.init(color);
      bgfx::blit(100, dst, src);
      bgfx::frame();
      std::vector<uint8_t> bytes(size * size * 4);
      auto ready = bgfx::read(dst, bytes.data());
      for (int i = 0; i < 16; ++i)
        if (bgfx::frame() >= ready)
          return bytes;
      throw std::runtime_error("garage readback timeout");
    };
    const auto lit = capture(false), shadow = capture(true);
    unsigned changed = 0;
    for (size_t i = 0; i < lit.size(); i += 4)
      if (int(lit[i]) - int(shadow[i]) > 10)
        ++changed;
    std::printf("GARAGE real geometry shadow coverage=%u pixels\n", changed);
    SLRR_REQUIRE(changed > 20 && changed < size * size / 2);
    // Move the actual furniture mesh: baked texture pixels stay identical,
    // while its depth-map footprint must move across the real garage floor.
    for (auto &instance : b.instances_)
      if (instance.key == &stuff)
        instance.transform[12] += 1.f;
    b.update_static_shadow(eye, video);
    bgfx::frame();
    const auto moved = capture(true);
    unsigned moving = 0;
    for (size_t i = 0; i < moved.size(); i += 4)
      if (std::abs(int(moved[i]) - int(shadow[i])) > 10)
        ++moving;
    std::printf("GARAGE furniture motion changes %u real-floor shadow pixels\n",
                moving);
    SLRR_REQUIRE(moving > 20);
    // Restore the stock window positions: the furniture SCX also contains
    // the window overlays, so the preceding movement test translated those.
    for (auto& instance : b.instances_) if (instance.key == &stuff) instance.transform[12]-=1.f;
    b.update_static_shadow(eye,video);
    // Daylight is an independent directional source: adding it must keep the
    // point source/shadow field, and changing only clock time moves its
    // footprint.
    const auto point_before = b.point_light_;
    const float old_time = scene_sun_time();
    scene_sun_set_time(15.f * 3600);
    b.update_interior_sun(video);
    SLRR_REQUIRE(b.interior_sun_active_);
    SLRR_REQUIRE(b.point_light_ == point_before && b.point_shadow_active_);
    b.draw_previews();
    bgfx::frame();
    const auto solar = capture(true);
    b.interior_sun_active_ = false;
    const auto neon_only = capture(true);
    unsigned sunlight_pixels = 0;
    for (size_t i = 0; i < solar.size(); i += 4)
      if (int(solar[i]) - int(neon_only[i]) > 10)
        ++sunlight_pixels;
    std::printf("GARAGE directional sunlight through alpha windows=%u pixels\n",
                sunlight_pixels);
    SLRR_REQUIRE(sunlight_pixels > 20 && sunlight_pixels < size * size / 2);
    // Same mesh and camera, but an opaque alpha mask must seal the window.
    std::string mask_path;
    for (const auto &obj : b.instances_)
      for (const auto &g : obj.geometry->groups)
        if (g.name == "ablak")
          mask_path = obj.geometry->textures.at(g.texture_index);
    SLRR_REQUIRE(!mask_path.empty());
    const auto original_mask = b.osd_tex_.at(mask_path);
    const uint32_t opaque_pixel = 0xffffffff;
    const auto opaque_mask =
        bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
                              bgfx::copy(&opaque_pixel, 4));
    b.osd_tex_[mask_path] = opaque_mask;
    b.update_interior_sun(video);
    b.draw_previews();
    bgfx::frame();
    const auto sealed = capture(true);
    unsigned masked_pixels = 0;
    for (size_t i = 0; i < solar.size(); i += 4)
      if (int(solar[i]) - int(sealed[i]) > 10)
        ++masked_pixels;
    std::printf("GARAGE opaque window alpha blocks %u sunlight pixels\n",
                masked_pixels);
    SLRR_REQUIRE(masked_pixels > 20);
    unsigned leaked_sun=0;
    for(size_t i=0;i<sealed.size();i+=4)
      if(int(sealed[i])-int(neon_only[i])>1) ++leaked_sun;
    std::printf("GARAGE sealed-aperture sunlight leaks=%u pixels\n",leaked_sun);
    SLRR_REQUIRE(leaked_sun==0);
    b.osd_tex_[mask_path] = original_mask;
    bgfx::destroy(opaque_mask);
    const auto afternoon_matrix = b.interior_sun_matrix_;
    scene_sun_set_time(16.f * 3600);
    b.update_interior_sun(video);
    SLRR_REQUIRE(b.interior_sun_matrix_ != afternoon_matrix);
    b.draw_previews();
    bgfx::frame();
    const auto late_solar = capture(true);
    unsigned solar_motion = 0;
    for (size_t i = 0; i < solar.size(); i += 4)
      if (std::abs(int(solar[i]) - int(late_solar[i])) > 10)
        ++solar_motion;
    std::printf("GARAGE clock-only sunlight motion=%u pixels\n", solar_motion);
    SLRR_REQUIRE(solar_motion > 20);
    auto flare_capture = [&](const bx::Vec3 &camera) {
      const auto &sun = b.interior_sun_;
      const bx::Vec3 direction{-sun.direction[0], -sun.direction[1],
                               sun.direction[2]};
      float view[16], proj[16];
      bx::mtxLookAt(view, camera, bx::add(camera, direction));
      bx::mtxProj(proj, 60.f, 1.f, .1f, 100.f,
                  bgfx::getCaps()->homogeneousDepth);
      bgfx::setViewFrameBuffer(5, fb);
      bgfx::setViewRect(5, 0, 0, size, size);
      bgfx::setViewClear(5, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, uint32_t(0xff),
                         0.f);
      bgfx::setViewTransform(5, view, proj);
      bgfx::touch(5);
      b.draw_sun_flare(view, proj, 5);
      bgfx::TextureRegion dst, src;
      dst.init(readback);
      src.init(color);
      bgfx::blit(100, dst, src);
      bgfx::frame();
      std::vector<uint8_t> bytes(size * size * 4);
      const auto ready = bgfx::read(dst, bytes.data());
      for (int i = 0; i < 16; ++i)
        if (bgfx::frame() >= ready) {
          uint64_t sum = 0;
          for (size_t j = 0; j < bytes.size(); j += 4)
            sum += bytes[j] + bytes[j + 1] + bytes[j + 2];
          return sum;
        }
      throw std::runtime_error("flare readback timeout");
    };
    const auto visible_flare = flare_capture({-17, 2.5f, -8.5f});
    const auto blocked_flare = flare_capture({0, 1.5f, 0});
    const auto frame_flare = flare_capture({-17, 2.5f, -9.46f});
    std::printf("GARAGE frame flare=%llu\n",static_cast<unsigned long long>(frame_flare));
    SLRR_REQUIRE(frame_flare == 0);
    std::printf("GARAGE camera-to-sun flare: window=%llu roof=%llu\n",
                static_cast<unsigned long long>(visible_flare),
                static_cast<unsigned long long>(blocked_flare));
    SLRR_REQUIRE(visible_flare > blocked_flare + 1000 && blocked_flare == 0);
    scene_sun_set_time(0);
    b.update_interior_sun(video);
    SLRR_REQUIRE(!b.interior_sun_active_);
    scene_sun_set_time(old_time);
    b.interior_sun_active_=false;
    Object neon;
    auto* neon_node=gameref_upsert(&neon,GameRefKind::RenderRef);
    neon_node->type_id=RpkIndex::make_full_id(pack,0x1d);
    neon_node->directional_light.set=neon_node->directional_light.has_direction=true;
    gameref_set_parent(&neon,&root);gameref_make_live(&neon);gameref_register_light(&neon);
    const auto graphics=host_enhanced_graphics();
    host_enhanced_graphics().local_lights=host_enhanced_graphics().dynamic_lighting=true;
    b.update_local_light();
    std::printf("NEON actual fixture count=%zu\n",b.fixture_lights_.size());
    SLRR_REQUIRE(b.fixture_lights_.size()==9);
    SLRR_REQUIRE(b.point_light_ && b.point_light_->intensity==0 && !b.point_light_->casts_shadows);
    for(const auto& light:b.fixture_lights_) {
      SLRR_REQUIRE(light.valid() && light.position[1]>6.4f && light.position[1]<6.5f);
      SLRR_REQUIRE(light.owner==&neon && light.direction[1]<-.99f);
    }
    b.update_static_shadow(eye,video);b.update_fixture_shadows(video);
    SLRR_REQUIRE(b.fixture_shadow_active_);
    b.draw_previews();bgfx::frame();
    const auto fixtures=b.fixture_lights_;
    {
      // #235: the patched Garage.java creates one SpotLight per fitting,
      // parented to the stuff mesh at these local anchors. They must give the
      // adapter's world positions and replace it.
      const float anchors[9][3]={{-14.05739f,6.43138f,5.03007f},{-8.68388f,6.43138f,5.03007f},
          {-3.03047f,6.43138f,5.03007f},{2.63350f,6.43138f,5.03007f},{8.82223f,6.43138f,5.03007f},
          {5.77201f,6.43138f,-4.00419f},{-5.88736f,6.43138f,-4.00419f},
          {12.71575f,6.43138f,1.23414f},{12.71575f,6.43138f,-5.19158f}};
      Object scripted[9];
      for(int i=0;i<9;++i) {
        SLRR_REQUIRE(script_lights_create(&scripted[i],&stuff,ScriptLightKind::Spot));
        auto* light=script_lights_get(&scripted[i]);
        light->position={anchors[i][0],anchors[i][1],anchors[i][2]};
        light->outer_degrees=80;
      }
      b.update_local_light();
      SLRR_REQUIRE(b.fixture_lights_.size()==fixtures.size());
      for(size_t i=0;i<fixtures.size();++i) {
        const auto& got=b.fixture_lights_[i];const auto& want=fixtures[i];
        for(int axis=0;axis<3;++axis) {
          SLRR_REQUIRE(std::abs(got.position[axis]-want.position[axis])<1e-3f);
          SLRR_REQUIRE(std::abs(got.direction[axis]-want.direction[axis])<1e-5f);
        }
        SLRR_REQUIRE(got.owner==&scripted[i] && got.intensity==want.intensity &&
                     got.range==want.range && std::abs(got.outer_cos-want.outer_cos)<1e-5f);
      }
      // The neon stays an ambient-only record next to script fixtures.
      SLRR_REQUIRE(b.point_light_ && b.point_light_->intensity==0 && !b.point_light_->casts_shadows);
      std::printf("NEON script fixtures match adapter=%zu\n",b.fixture_lights_.size());
      // Disabled script lights still replace the adapter: the garage is dark.
      for(auto& light:scripted) script_lights_get(&light)->enabled=false;
      b.update_local_light();SLRR_REQUIRE(b.fixture_lights_.empty());
      // The neon stays ambient-only: no shadowed point light comes back.
      SLRR_REQUIRE(b.point_light_ && b.point_light_->owner==&neon &&
                   b.point_light_->intensity==0 && !b.point_light_->casts_shadows);
      // A script point light takes the point record and keeps the neon ambient.
      Object bulb;SLRR_REQUIRE(script_lights_create(&bulb,&stuff,ScriptLightKind::Point));
      script_lights_get(&bulb)->position={0,3,0};
      b.update_local_light();
      SLRR_REQUIRE(b.point_light_ && b.point_light_->owner==&bulb && b.point_light_->intensity==24);
      SLRR_REQUIRE(std::abs(b.point_light_->position[1]-3)<1e-4f);
      for(auto& light:scripted) script_lights_destroy(&light);
      script_lights_destroy(&bulb);
      b.update_local_light();
      SLRR_REQUIRE(b.fixture_lights_.size()==fixtures.size() && b.fixture_lights_[0].owner==&neon);
      b.fixture_lights_=fixtures;
    }
    const auto fixture_image=capture(false);
    capture_view=72;
    SLRR_REQUIRE(capture(false)==fixture_image);
    capture_view=5;
    b.fixture_lights_.clear();
    const auto ambient_image=capture(false);
    unsigned neon_pixels=0;
    for(size_t i=0;i<fixture_image.size();i+=4) if(int(fixture_image[i])-int(ambient_image[i])>10) ++neon_pixels;
    std::printf("NEON real-floor illuminated pixels=%u\n",neon_pixels);
    SLRR_REQUIRE(neon_pixels>100);
    b.fixture_lights_=fixtures;
    auto revision=b.fixture_shadow_revision_;
    b.update_fixture_shadows(video);
    SLRR_REQUIRE(b.fixture_shadow_revision_==revision);
    // Each atlas tile/source must contribute independently, including > 6.
    for(size_t lamp=0;lamp<fixtures.size();++lamp) {
      b.fixture_lights_=fixtures;
      for(size_t j=0;j<fixtures.size();++j) if(j!=lamp) b.fixture_lights_[j].intensity=0;
      const auto image=capture(false);unsigned pixels=0;
      for(size_t i=0;i<image.size();i+=4) if(int(image[i])-int(ambient_image[i])>3) ++pixels;
      std::printf("NEON source %zu contributes %u pixels\n",lamp,pixels);
      SLRR_REQUIRE(pixels>10);
      b.update_fixture_shadows(video);SLRR_REQUIRE(b.fixture_shadow_revision_==revision);
    }
    b.fixture_lights_=fixtures;
    b.fixture_shadow_active_=false;
    const auto unshadowed=capture(false);
    unsigned blocked=0;
    for(size_t i=0;i<fixture_image.size();i+=4) if(int(unshadowed[i])-int(fixture_image[i])>5) ++blocked;
    std::printf("NEON occluded pixels=%u\n",blocked);SLRR_REQUIRE(blocked>10);
    b.update_fixture_shadows(video);b.draw_previews();bgfx::frame();
    revision=b.fixture_shadow_revision_;
    b.fixture_lights_[0].position[0]+=1.f;
    b.update_fixture_shadows(video);SLRR_REQUIRE(b.fixture_shadow_revision_==revision+1);
    b.draw_previews();bgfx::frame();
    const auto shifted=capture(false);unsigned moving_neon=0;
    for(size_t i=0;i<shifted.size();i+=4) if(std::abs(int(shifted[i])-int(fixture_image[i]))>5) ++moving_neon;
    SLRR_REQUIRE(moving_neon>10);
    // The luminous tube faces keep their original white atlas sample and do
    // not depend on ambient, punctual shadows or the sunlight enable state.
    auto tubes=std::make_shared<ScxGeometry>();
    for(const auto& obj:b.instances_) if(!obj.geometry->ceiling_light_anchors.empty()) {
      *tubes=*obj.geometry;tubes->indices.clear();tubes->groups.clear();
      for(auto group:obj.geometry->groups) if(group.self_illuminated) {
        auto first=group.index_start;group.index_start=uint32_t(tubes->indices.size());
        tubes->indices.insert(tubes->indices.end(),obj.geometry->indices.begin()+first,
            obj.geometry->indices.begin()+first+group.index_count);tubes->groups.push_back(group);
      }
      break;
    }
    SLRR_REQUIRE(tubes->indices.size()==432*3);
    b.cache_scene_mesh(tubes,nullptr,false);
    receiver.geometry=tubes;
    const auto tubes_on=capture(false);
    b.fixture_lights_.clear();b.point_light_.reset();
    const auto tubes_off=capture(false);
    SLRR_REQUIRE(tubes_on==tubes_off);
    unsigned white=0;
    for(size_t i=0;i<tubes_off.size();i+=4) if(tubes_off[i]>180 && tubes_off[i+1]>180 && tubes_off[i+2]>180) ++white;
    std::printf("NEON white tube pixels retained without lights=%u\n",white);SLRR_REQUIRE(white>10);
    for(auto& object:b.instances_) if(object.key==&stuff) object.transform[12]+=2.f;
    b.update_local_light();
    SLRR_REQUIRE(b.fixture_lights_.size()==fixtures.size());
    for(size_t i=0;i<fixtures.size();++i)
      SLRR_REQUIRE(std::abs(b.fixture_lights_[i].position[0]-fixtures[i].position[0]-2.f)<.001f);
    for(auto& object:b.instances_) if(object.key==&stuff) object.transform[12]-=2.f;
    scene_instances_select(nullptr,nullptr,false,SceneEnvironment::Interior);
    b.update_local_light();SLRR_REQUIRE(b.fixture_lights_.empty());
    scene_instances_select(&root,nullptr,false,SceneEnvironment::Interior);
    host_enhanced_graphics().local_lights=false;b.update_local_light();
    SLRR_REQUIRE(b.fixture_lights_.empty());
    host_enhanced_graphics()=graphics;
    for(int quality=0;quality<3;++quality) {
      host_enhanced_graphics().shadow_quality=quality;
      b.update_interior_sun(video);b.draw_previews();bgfx::frame();
      SLRR_REQUIRE(b.interior_sun_size_==(512u<<quality));
      SLRR_REQUIRE(bgfx::isValid(b.interior_sun_fb_));
    }
    host_enhanced_graphics()=graphics;
    b.fixture_shadow_active_=false;
    gameref_remove(&neon);
    bgfx::destroy(readback);
    bgfx::destroy(fb);
    auto children = gameref_get(&root)->children;
    for (auto child : children)
      gameref_remove(child);
    gameref_remove(&root);
    scene_instances_set_resources(nullptr);
    b.instances_.clear();
    b.garage_ = false;
    b.point_light_.reset();
    b.local_light_ = {};
  }
  // #235: the three club garages. Garage.java places one SpotLight per
  // twin-tube fitting of the stuff mesh. The loader must turn exactly the tube
  // faces into unlit emitters (they stay white without any light), each anchor
  // must sit 8 cm under them, the registry must place the spots there, and the
  // fittings must light the real floor at Garage.java's brightness.
  static void garage_fixtures(LibBgfxBackend &b) {
    struct Room { uint32_t ground, stuff; const char* lamps; std::vector<std::array<float,3>> anchors; };
    const Room rooms[] = {
      {0x7a, 0x42, "texturak", {{-14.05739f,6.43138f,5.03007f},{-8.68388f,6.43138f,5.03007f},
        {-3.03047f,6.43138f,5.03007f},{2.63350f,6.43138f,5.03007f},{8.82223f,6.43138f,5.03007f},
        {5.77201f,6.43138f,-4.00419f},{-5.88736f,6.43138f,-4.00419f},
        {12.71575f,6.43138f,1.23414f},{12.71575f,6.43138f,-5.19158f}}},
      {0x06, 0x18, "neon", {{-7.04596f,5.42426f,-3.26745f},{-7.04596f,5.42426f,4.32187f},
        {0.02651f,5.42426f,4.32187f},{0.02651f,5.42426f,-3.26745f},{7.12191f,5.42426f,4.32187f},
        {7.12191f,5.42426f,-3.26745f},{14.09471f,5.42426f,-3.26745f},{14.09471f,5.42426f,4.32187f},
        {-13.63311f,5.42426f,-3.26745f},{-13.63311f,5.42426f,4.32187f}}},
      {0xc7, 0x3c, "Material #11345", {{-12.53611f,5.69371f,-0.75343f},{20.72244f,5.52448f,6.01213f},
        {-6.72907f,6.41467f,9.38976f},{20.72244f,5.53801f,10.85484f},{-12.53611f,5.69371f,-6.80128f},
        {-0.08248f,6.43135f,-3.24062f},{-0.08248f,6.43135f,1.71936f},{3.34185f,6.41467f,9.38976f},
        {9.71543f,6.43135f,-6.30282f},{9.71543f,6.43135f,-1.04061f},{13.13120f,6.41467f,9.38976f},
        {20.72244f,5.55018f,1.16555f}}}};
    for (const auto& room : rooms) {
      Fixture f;
      f.packs.set_root(fs::absolute("game").string());
      f.packs.open_all();
      scene_instances_set_resources(&f.packs);
      const auto pack = f.packs.open_lib("misc/garage.rpk");
      Object root, stuff;
      root.class_name = "java.util.resource.GroundRef";
      gameref_upsert(&root, GameRefKind::GroundRef)->type_id = RpkIndex::make_full_id(pack, room.ground);
      spawn_ground_shell(f.vm, f.packs, &root);
      gameref_upsert(&stuff, GameRefKind::RenderRef)->type_id = RpkIndex::make_full_id(pack, room.stuff);
      gameref_set_parent(&stuff, &root);
      scene_instances_select(&root, nullptr, false, SceneEnvironment::Interior);
      b.instances_ = scene_instances_collect();
      const ScxGeometry* mesh = nullptr;
      for (const auto& obj : b.instances_) if (obj.key == &stuff) mesh = obj.geometry.get();
      SLRR_REQUIRE(mesh);
      // The loader's adapter finds the same fittings as Garage.java.
      SLRR_REQUIRE(mesh->ceiling_light_anchors.size() == room.anchors.size());
      for (const auto& anchor : room.anchors) {
        bool found = false;
        for (const auto& a : mesh->ceiling_light_anchors)
          found |= std::abs(a[0] - anchor[0]) < 1e-4f && std::abs(a[1] - anchor[1]) < 1e-4f &&
                   std::abs(a[2] - anchor[2]) < 1e-4f;
        SLRR_REQUIRE(found);
      }
      // Only faces of the named lamp group become emitters, two tubes each.
      size_t emitting = 0;
      for (const auto& group : mesh->groups) if (group.self_illuminated) {
        SLRR_REQUIRE(group.name == room.lamps);
        emitting += group.index_count / 3;
      }
      SLRR_REQUIRE(emitting >= room.anchors.size() * 2 * 12);
      // Derivation check: the emitting tube faces lie 8 cm above each anchor,
      // along a 3 m tube through it.
      for (const auto& anchor : room.anchors) {
        float lowest = 1e9f;
        for (const auto& group : mesh->groups) if (group.self_illuminated)
          for (uint32_t i = group.index_start; i < group.index_start + group.index_count; ++i) {
            const auto v = mesh->indices[i] * 3;
            const float dx = std::abs(mesh->positions[v] * mesh->position_scale - anchor[0]);
            const float dz = std::abs(mesh->positions[v + 2] * mesh->position_scale - anchor[2]);
            if (std::max(dx, dz) < 1.6f && std::min(dx, dz) < .2f)
              lowest = std::min(lowest, mesh->positions[v + 1] * mesh->position_scale);
          }
        SLRR_REQUIRE(std::abs(lowest - .08f - anchor[1]) < .002f);
      }
      std::vector<Object> lights(room.anchors.size());
      // Garage.java's brightness for every garage (the first version used the
      // registry defaults, 24 and 20 m).
      constexpr float intensity = 30.f, range = 26.f;
      for (size_t i = 0; i < lights.size(); ++i) {
        SLRR_REQUIRE(script_lights_create(&lights[i], &stuff, ScriptLightKind::Spot));
        auto* light = script_lights_get(&lights[i]);
        light->position = room.anchors[i];
        light->intensity = intensity;
        light->range = range;
      }
      for (const auto& obj : b.instances_) b.cache_scene_mesh(obj.geometry, nullptr, false);
      b.garage_ = true; b.outdoor_lighting_ = false;
      const auto graphics = host_enhanced_graphics();
      host_enhanced_graphics().local_lights = true;
      b.update_local_light();
      SLRR_REQUIRE(b.fixture_lights_.size() == room.anchors.size());
      for (size_t i = 0; i < lights.size(); ++i) {
        const auto& light = b.fixture_lights_[i];
        SLRR_REQUIRE(light.owner == &lights[i] && light.valid() && light.direction[1] < -.99f &&
                     light.intensity == intensity && light.range == range);
        SLRR_REQUIRE(std::abs(light.position[0] - room.anchors[i][0]) < 1e-3f &&
                     std::abs(light.position[1] - room.anchors[i][1]) < 1e-3f &&
                     std::abs(light.position[2] + room.anchors[i][2]) < 1e-3f);
      }
      // The fittings light the actual floor of this garage.
      auto floor = std::make_shared<ScxGeometry>(*b.instances_.front().geometry);
      floor->indices.clear(); floor->groups.clear();
      const auto& source = *b.instances_.front().geometry;
      for (auto group : source.groups) {
        const auto first = group.index_start, count = group.index_count;
        group.index_start = uint32_t(floor->indices.size());
        for (uint32_t i = first; i + 2 < first + count; i += 3) {
          bool flat = true;
          for (int k = 0; k < 3; ++k) {
            const auto v = source.indices[i + k] * 3;
            flat &= std::abs(source.positions[v + 1] * source.position_scale) < .3f && source.normals[v + 1] > .5f;
          }
          if (flat) floor->indices.insert(floor->indices.end(), source.indices.begin() + i, source.indices.begin() + i + 3);
        }
        group.index_count = uint32_t(floor->indices.size()) - group.index_start;
        if (group.index_count) floor->groups.push_back(group);
      }
      SLRR_REQUIRE(!floor->indices.empty());
      b.cache_scene_mesh(floor, nullptr, false);
      SceneInstance receiver;
      receiver.geometry = floor;
      bx::mtxIdentity(receiver.transform.data());
      receiver.lighting.set = receiver.lighting.has_direction = true;
      for (int i = 0; i < 3; ++i) { receiver.lighting.diffuse[i] = .8f; receiver.lighting.ambient[i] = .1f; }
      constexpr uint16_t size = 256;
      const auto color = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
      const auto depth = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT);
      const bgfx::TextureHandle attachments[] = {color, depth};
      const auto fb = bgfx::createFrameBuffer(2, attachments, true);
      const auto readback = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8,
                                                  BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
      float identity[16];
      bx::mtxIdentity(identity);
      float projection[16]{};
      projection[0] = projection[9] = .04f;
      projection[6] = .01f;
      projection[14] = .5f;
      projection[15] = 1;
      auto capture = [&]() {
        b.static_shadow_active_ = false;
        b.fixture_shadow_active_ = false;
        bgfx::setViewFrameBuffer(5, fb);
        bgfx::setViewRect(5, 0, 0, size, size);
        bgfx::setViewClear(5, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, uint32_t(0xff), 0.f);
        bgfx::setViewTransform(5, identity, projection);
        b.draw_instance(5, receiver, identity, false, 0);
        bgfx::TextureRegion dst, src;
        dst.init(readback);
        src.init(color);
        bgfx::blit(100, dst, src);
        bgfx::frame();
        std::vector<uint8_t> bytes(size * size * 4);
        auto ready = bgfx::read(dst, bytes.data());
        for (int i = 0; i < 16; ++i)
          if (bgfx::frame() >= ready) return bytes;
        throw std::runtime_error("garage fixture readback timeout");
      };
      const auto lit = capture();
      const auto fixtures = b.fixture_lights_;
      b.fixture_lights_.clear();
      const auto dark = capture();
      // Previous brightness, for the before/after report.
      b.fixture_lights_ = fixtures;
      for (auto& light : b.fixture_lights_) { light.intensity = 24.f; light.range = 20.f; }
      const auto before = capture();
      b.fixture_lights_ = fixtures;
      // 8-bit captures clip under the fittings, so also report the floor that
      // was dim before (gain under 60), where the change is not clipped.
      unsigned pixels = 0, floor_pixels = 0, dim_pixels = 0;
      double gain = 0, gain_before = 0, dim = 0, dim_before = 0;
      for (size_t i = 0; i < lit.size(); i += 4) {
        if (!dark[i] && !dark[i + 1] && !dark[i + 2]) continue; // black clear colour
        ++floor_pixels;
        const int now = int(lit[i]) - int(dark[i]), old = int(before[i]) - int(dark[i]);
        gain += now;
        gain_before += old;
        if (old < 60) { ++dim_pixels; dim += now; dim_before += old; }
        if (now > 10) ++pixels;
      }
      gain /= std::max(floor_pixels, 1u);
      gain_before /= std::max(floor_pixels, 1u);
      dim /= std::max(dim_pixels, 1u);
      dim_before /= std::max(dim_pixels, 1u);
      std::printf("GARAGE 0x%x script fittings=%zu lit floor pixels=%u/%u mean floor gain 24/20m=%.1f 30/26m=%.1f;"
                  " dim floor (%u px) %.1f -> %.1f\n", room.ground, fixtures.size(), pixels, floor_pixels,
                  gain_before, gain, dim_pixels, dim_before, dim);
      SLRR_REQUIRE(pixels > 100 && gain >= gain_before);
      SLRR_REQUIRE(!dim_pixels || dim > dim_before * 1.15);
      // The tube faces keep their texture without any light (#235): white.
      auto tubes = std::make_shared<ScxGeometry>(*mesh);
      tubes->indices.clear(); tubes->groups.clear();
      for (auto group : mesh->groups) if (group.self_illuminated) {
        const auto first = group.index_start;
        group.index_start = uint32_t(tubes->indices.size());
        tubes->indices.insert(tubes->indices.end(), mesh->indices.begin() + first,
                              mesh->indices.begin() + first + group.index_count);
        tubes->groups.push_back(group);
      }
      b.cache_scene_mesh(tubes, nullptr, false);
      receiver.geometry = tubes;
      b.fixture_lights_.clear();
      b.point_light_.reset();
      const auto tubes_off = capture();
      unsigned white = 0;
      for (size_t i = 0; i < tubes_off.size(); i += 4)
        if (tubes_off[i] > 180 && tubes_off[i + 1] > 180 && tubes_off[i + 2] > 180) ++white;
      std::printf("GARAGE 0x%x emitting tube triangles=%zu white tube pixels without lights=%u\n",
                  room.ground, emitting, white);
      SLRR_REQUIRE(white > 10);
      host_enhanced_graphics() = graphics;
      bgfx::destroy(readback);
      bgfx::destroy(fb);
      for (auto& light : lights) script_lights_destroy(&light);
      auto children = gameref_get(&root)->children;
      for (auto child : children) gameref_remove(child);
      gameref_remove(&root);
      scene_instances_set_resources(nullptr);
      b.instances_.clear();
      b.fixture_lights_.clear();
      b.garage_ = false;
      b.outdoor_lighting_ = true;
      b.point_light_.reset();
      b.local_light_ = {};
    }
  }
  static void addon_bounds(LibBgfxBackend& b) {
    if (!fs::exists("game/maps/Addon_Maps/Drag_Track.rpk")) {
      std::puts("SKIP optional Drag Track asset regression: addon not installed");
      return;
    }
    Fixture f;
    f.packs.set_root(fs::absolute("game").string());
    f.packs.open_all();
    scene_instances_set_resources(&f.packs);
    const auto id=RpkIndex::make_full_id(
        f.packs.open_lib("maps/Addon_Maps/Drag_Track.rpk"),1);
    Object root;
    root.class_name="java.util.resource.GroundRef";
    gameref_upsert(&root,GameRefKind::GroundRef)->type_id=id;
    spawn_ground_shell(f.vm,f.packs,&root);
    scene_instances_select(&root,nullptr,false,SceneEnvironment::Exterior);
    scene_sun_set_time(13*3600.f);
    b.instances_=scene_instances_collect();
    SLRR_REQUIRE(!b.instances_.empty());
    const auto& instance=b.instances_.front();
    const auto& g=*instance.geometry;
    SLRR_REQUIRE(g.ground_polygons && g.positions.size()>200000);
    for(int axis=0;axis<3;++axis) {
      float minimum=g.positions[axis],maximum=minimum;
      for(size_t i=axis;i<g.positions.size();i+=3) {
        minimum=std::min(minimum,g.positions[i]);
        maximum=std::max(maximum,g.positions[i]);
      }
      SLRR_REQUIRE(g.info.bbox_min[axis]==minimum && g.info.bbox_max[axis]==maximum);
    }
    const float eye[3]={27,2,-36};
    const auto light=scene_instances_light();
    const float focus[3]={eye[0],eye[1],-eye[2]};
    const float dir[3]={light.direction[0],light.direction[1],-light.direction[2]};
    for(float radius:shadow_radii) {
      const auto p=shadow_projection(focus,dir,radius,2048,false,false);
      const auto fr=b.view_frustum(p.view.data(),p.projection.data());
      SLRR_REQUIRE(shadow_bounds_visible(g,instance.transform,fr));
    }
    VideoOptions video;video.shadows=2;
    b.garage_=false;b.local_light_={};
    b.update_static_shadow(eye,video);bgfx::frame();
    SLRR_REQUIRE(b.static_shadow_active_ && b.shadow_meshes_.contains(&g));
    const auto children=gameref_get(&root)->children;
    for(auto child:children)gameref_remove(child);
    gameref_remove(&root);
    scene_instances_set_resources(nullptr);b.instances_.clear();
    std::puts("PASS real Drag Track: decoded bounds match vertices and retain map in all four shadow levels");
  }
  static void scene_environment(LibBgfxBackend &b) {
    Fixture f;
    scene_instances_set_resources(&f.packs);
    Object world, sky, mesh, light, unrelated, other_light, section;
    gameref_upsert(&world)->flags = kWorldTreeRoot;
    gameref_upsert(&sky, GameRefKind::RenderRef);
    gameref_set_parent(&sky, &world);
    auto geometry = std::make_shared<ScxGeometry>();
    geometry->info.ok = true;
    geometry->info.version = 4;
    geometry->position_scale = 1;
    geometry->positions = {-1, 0, 0, 1, 0, 0, 0, 2, 0};
    geometry->normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    geometry->uvs = {0, 0, 1, 0, .5f, 1};
    geometry->indices = {0, 1, 2};
    geometry->info.bbox_min[0] = -1;
    geometry->info.bbox_max[0] = 1;
    geometry->info.bbox_max[1] = 2;
    ScxGroup group;
    group.index_count = 3;
    geometry->groups.push_back(group);
    gameref_upsert(&mesh, GameRefKind::RenderRef)->line_geometry = geometry;
    gameref_set_parent(&mesh, &world);
    auto make_light = [](Object *key, Object *root, float red) {
      auto *node = gameref_upsert(key, GameRefKind::RenderRef);
      node->type_id = 1;
      node->directional_light.set = node->directional_light.has_direction = true;
      node->directional_light.diffuse[0] = red;
      node->directional_light.direction[0] = .2f;
      node->directional_light.direction[1] = -.8f;
      node->directional_light.direction[2] = .3f;
      gameref_set_parent(key, root);
      gameref_make_live(key);
      gameref_register_light(key);
    };
    make_light(&light, &world, .25f);
    gameref_upsert(&unrelated)->flags = kWorldTreeRoot;
    make_light(&other_light, &unrelated, .9f);
    section.fields["map"] = Value::make_obj(&world);
    section.fields["skydome"] = Value::make_obj(&sky);
    section.fields["internalScene"] = Value::make_int(1);
    f.sections.adopt_actual(&section);
    mainloop_sync_section_render(b, f.sections);
    SLRR_REQUIRE(!scene_instances_has_ground_props() && !scene_instances_outdoor());
    scene_sun_set_time(12 * 3600.f);
    auto indoor = scene_instances_light();
    SLRR_REQUIRE(indoor.diffuse[0] == .25f && indoor.direction[0] == .2f);
    scene_sun_set_time(0);
    SLRR_REQUIRE(scene_instances_light().direction[0] == .2f);
    // A skydome also exists in internalScene: it must not enable sunlight.
    VideoOptions video;
    video.shadows = 2;
    const float eye[3] = {0, 1, 4};
    b.instances_ = scene_instances_collect();
    b.update_static_shadow(eye, video);
    SLRR_REQUIRE(!b.static_shadow_active_);
    // CarMarket inherits an unassigned int: the host object is sparse, but
    // Scene.addSceneElements has created its sun. Exercise that real layout.
    section.fields.erase("internalScene");
    section.fields["sun"] = Value::make_obj(&light);
    mainloop_sync_section_render(b, f.sections);
    scene_sun_set_time(12 * 3600.f);
    SLRR_REQUIRE(scene_instances_outdoor());
    SLRR_REQUIRE(scene_instances_light().direction[1] < -.8f);
    // No main-view ground or even visible instances are needed to find the sun.
    b.instances_.clear();
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.static_shadow_active_ && b.shadow_cascades_active_);
    SLRR_REQUIRE(b.shadow_meshes_.contains(geometry.get()));
    // Garage uses the authored downwards neon for direct shading/visibility,
    // and furniture must cast too, not only vehicle_material instances.
    section.fields["internalScene"] = Value::make_int(1);
    section.class_name = "java.game.Garage";
    mainloop_sync_section_render(b, f.sections);
    b.instances_ = scene_instances_collect();
    b.cache_scene_mesh(geometry, nullptr, false);
    b.local_light_ = {0, 3.3f, 0, 32};
    PointLight fixture_light;
    fixture_light.position = {0, 3.3f, 0};
    b.point_light_ = fixture_light;
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.static_shadow_active_ && b.static_shadow_local_ &&
                 b.point_shadow_active_);
    b.garage_ = false;
    b.point_light_.reset();
    b.local_light_ = {};
    b.instances_.clear();
    scene_instances_select(&world, &sky, true, SceneEnvironment::Exterior);
    SLRR_REQUIRE(!scene_instances_outdoor());
    b.update_static_shadow(eye, video);
    SLRR_REQUIRE(!b.static_shadow_active_);
    scene_instances_select(&world, &sky, false);
    SLRR_REQUIRE(!scene_instances_outdoor());
    f.sections.adopt_actual(nullptr);
    for (Object *object : {&other_light, &unrelated, &light, &mesh, &sky, &world})
      gameref_remove(object);
    scene_instances_set_resources(nullptr);
    std::puts("PASS scene intent: mesh-only exterior shadows, interior clock/sky "
              "isolation, root-scoped lights, preview and automatic fallback");
  }

  static void real_world_cache(LibBgfxBackend &b) {
    Fixture f;
    f.packs.set_root(fs::absolute("game").string());
    f.packs.open_all();
    scene_instances_set_resources(&f.packs);
    const auto id =
        RpkIndex::make_full_id(f.packs.open_lib("maps/test_track.rpk"), 1);
    auto *root = f.vm.new_object("java.util.resource.GroundRef");
    gameref_upsert(root, GameRefKind::GroundRef)->type_id = id;
    spawn_ground_shell(f.vm, f.packs, root);
    scene_instances_select(root, nullptr, false);
    scene_sun_set_time(13 * 3600.f);
    b.instances_ = scene_instances_collect();
    b.garage_ = false;
    b.point_light_.reset();
    b.local_light_ = {};
    VideoOptions video;
    video.shadows = 2;
    const float eye[3] = {1000, 5, -2000};
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.static_shadow_active_);
    // One stable redraw establishes the caster snapshot after moving light.
    b.update_static_shadow(eye,video);bgfx::frame();
    SLRR_REQUIRE(b.shadow_cache_valid_);
    auto revision = b.shadow_atlas_revision_;
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision);
    // #371: the shadow light direction holds until the sun has moved
    // shadow_sun_step degrees. Seconds of game clock move it by a few
    // thousandths of a degree: the atlas stays.
    const float sun_step = render_debug().shadow_sun_step;
    SLRR_REQUIRE(sun_step > 0.f);
    const auto held = b.shadow_sun_direction_;
    scene_sun_set_time(scene_sun_time() + 2.f);
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision && b.shadow_sun_direction_ == held);
    // A larger move (about a degree) redraws it with the new direction.
    scene_sun_set_time(scene_sun_time() + 240.f);
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision + 1 && b.shadow_sun_direction_ != held);
    revision = b.shadow_atlas_revision_;
    b.update_static_shadow(eye, video);
    bgfx::frame();
    revision = b.shadow_atlas_revision_;
    // Step 0 follows the sun every frame, as before #371.
    render_debug().shadow_sun_step = 0.f;
    scene_sun_set_time(scene_sun_time() + .01f);
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision + 1);
    render_debug().shadow_sun_step = sun_step;
    revision = b.shadow_atlas_revision_;
    {
      // #371 crossfade: a sun step in consecutive frames keeps the previous
      // atlas and its matrices, swaps the atlases and fades the old one out.
      const float saved_fade = render_debug().shadow_crossfade;
      render_debug().shadow_crossfade = .2f;
      // Two continuous frames with a moved camera draw a complete atlas.
      const float moved[3] = {eye[0] + 7.f, eye[1], eye[2]};
      ++b.frames_;
      b.update_static_shadow(moved, video);bgfx::frame();
      ++b.frames_;
      b.update_static_shadow(eye, video);bgfx::frame();
      const auto old_texture = b.static_shadow_tex_;
      const auto old_matrices = b.shadow_cascade_matrices_;
      scene_sun_set_time(scene_sun_time() + 240.f);
      ++b.frames_;
      b.update_static_shadow(eye, video);bgfx::frame();
      SLRR_REQUIRE(b.shadow_blending_ && b.shadow_blend_ > .5f);
      SLRR_REQUIRE(b.static_shadow_prev_tex_.idx == old_texture.idx && b.static_shadow_tex_.idx != old_texture.idx);
      SLRR_REQUIRE(b.shadow_cascade_prev_matrices_ == old_matrices && b.shadow_cascade_matrices_ != old_matrices);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      ++b.frames_;
      b.update_static_shadow(eye, video);bgfx::frame();
      SLRR_REQUIRE(!b.shadow_blending_ && b.shadow_blend_ == 0.f);
      // Crossfade 0: the step lands at once, without a second atlas swap.
      render_debug().shadow_crossfade = 0.f;
      const auto current = b.static_shadow_tex_;
      scene_sun_set_time(scene_sun_time() + 240.f);
      ++b.frames_;
      b.update_static_shadow(eye, video);bgfx::frame();
      SLRR_REQUIRE(!b.shadow_blending_ && b.static_shadow_tex_.idx == current.idx);
      render_debug().shadow_crossfade = saved_fade;
      revision = b.shadow_atlas_revision_;
      std::puts("PASS sun step crossfade keeps the previous atlas and fades it out (#371)");
    }
    const auto children = gameref_get(root)->children;
    SLRR_REQUIRE(!children.empty());
    auto *shell = gameref_get(children.front());
    shell->local_matrix[12] += .25f;
    shell->matrix_override = true;
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision + 1);
    revision = b.shadow_atlas_revision_;
    for (auto &object : b.instances_)
      if (object.geometry->ground_polygons) {
        // The fixture's scene cache owns mutable geometry behind this const
        // view.
        ++std::const_pointer_cast<ScxGeometry>(object.geometry)->revision;
        break;
      }
    b.update_static_shadow(eye, video);
    bgfx::frame();
    SLRR_REQUIRE(b.shadow_atlas_revision_ == revision + 1);
    {
      // #361: a moving camera redraws the far cascades (2, 3) on alternate
      // frames; the skipped one keeps the matrix its tile was drawn with.
      // A still camera, a gap of one frame or alternation off draws all.
      const bool saved = render_debug().cascade_alternation;
      float moving[3] = {eye[0], eye[1], eye[2]};
      const auto step = [&](float dx) {
        moving[0] += dx;
        ++b.frames_;
        const auto before = b.shadow_cascade_matrices_;
        b.update_static_shadow(moving, video);
        bgfx::frame();
        std::array<bool, 4> drawn{};
        for (size_t c = 0; c < 4; ++c) drawn[c] = b.shadow_cascade_matrices_[c] != before[c];
        return drawn;
      };
      render_debug().cascade_alternation = true;
      step(3.f);  // continuity starts here: everything is drawn
      size_t far_draws = 0;
      for (int frame = 0; frame < 6; ++frame) {
        const auto drawn = step(3.f);
        SLRR_REQUIRE(drawn[0] && drawn[1]);
        SLRR_REQUIRE(drawn[2] != drawn[3]);
        far_draws += size_t(drawn[2]) + size_t(drawn[3]);
      }
      SLRR_REQUIRE(far_draws == 6);
      b.frames_ += 2;  // a frame without the sun cascades: all tiles stale
      {
        const auto drawn = step(3.f);
        SLRR_REQUIRE(drawn[0] && drawn[1] && drawn[2] && drawn[3]);
      }
      render_debug().cascade_alternation = false;
      for (int frame = 0; frame < 2; ++frame) {
        const auto drawn = step(3.f);
        SLRR_REQUIRE(drawn[0] && drawn[1] && drawn[2] && drawn[3]);
      }
      render_debug().cascade_alternation = saved;
      std::puts("PASS far sun cascades alternate while the camera moves (#361)");
    }
    for (auto child : children)
      gameref_remove(child);
    gameref_remove(root);
    scene_instances_set_resources(nullptr);
    b.instances_.clear();
    std::puts("PASS real map: unchanged atlas reused, sun held within its step (#371), "
              "larger sun and caster motion invalidates");
  }

  static void run(LibBgfxBackend &b) {
    constexpr uint16_t size = 64;
    const auto color = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    const auto depth = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT);
    const bgfx::TextureHandle attachments[] = {color, depth};
    const auto output = bgfx::createFrameBuffer(2, attachments, true);
    const auto readback =
        bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    const auto shadow = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    SLRR_REQUIRE(bgfx::isValid(output) && bgfx::isValid(readback) &&
                 bgfx::isValid(shadow));
    b.static_shadow_fb_ = bgfx::createFrameBuffer(1, &shadow, true);
    SLRR_REQUIRE(bgfx::isValid(b.static_shadow_fb_));
    b.static_shadow_tex_ = shadow;
    b.static_shadow_size_ = size;
    b.static_shadow_distance_ = 100;
    b.static_shadow_eye_ = {0, 0, 0};
    b.debug_overlay_.render_options().map_shadows = true;
    b.outdoor_lighting_ = true;
    float identity[16];
    bx::mtxIdentity(identity);
    bx::mtxIdentity(b.static_shadow_matrix_.data());
    b.static_shadow_matrix_[0] = b.static_shadow_matrix_[5] = .1f;
    b.static_shadow_matrix_[12] = b.static_shadow_matrix_[13] = .5f;
    auto geo = std::make_shared<ScxGeometry>();
    geo->info.ok = true;
    geo->info.version = 4;
    geo->position_scale = 1;
    geo->positions = {-1, -1, -.5f, 3, -1, -.5f, -1, 3, -.5f};
    geo->normals = {0, 0, -1, 0, 0, -1, 0, 0, -1};
    geo->uvs = {0, 0, 1, 0, 0, 1};
    geo->uv2 = geo->uv3 = geo->uvs;
    geo->vertex_colors = {0xffffffff, 0xffffffff, 0xffffffff};
    geo->indices = {0, 1, 2};
    ScxGroup group;
    group.index_count = 3;
    group.material_flags = 0x02000000u;
    group.diffuse[0] = group.diffuse[1] = group.diffuse[2] = group.diffuse[3] =
        1;
    group.specular[0] = group.specular[1] = group.specular[2] = 0;
    geo->groups.push_back(group);
    b.cache_scene_mesh(geo, nullptr, false);
    SceneInstance instance;
    instance.geometry = geo;
    instance.lighting.set = instance.lighting.has_direction = true;
    instance.lighting.direction[0] = instance.lighting.direction[1] = 0;
    instance.lighting.direction[2] =
        1; // CPU converts travel to +Z toward-light.
    for (int i = 0; i < 3; ++i) {
      instance.lighting.ambient[i] = .1f;
      instance.lighting.diffuse[i] = .6f;
      instance.lighting.specular[i] = 0;
    }
    int darkTile = -1;
    int stripeX = -1;
    bool sloped_self_receiver=false;
    auto pixel = [&](bool vehicle, bool enabled, float shadowDepth,
                     uint16_t view, bool preview = false, bool lamp = false) {
      instance.vehicle_material = vehicle;
      instance.light_flags = lamp ? 1 : 0;
      b.static_shadow_active_ = enabled;
      if (b.shadow_cascades_active_) {
        for (size_t i = 0; i < 4; ++i) {
          const auto id = shadow_views[i];
          bgfx::setViewFrameBuffer(id, b.static_shadow_fb_);
          bgfx::setViewRect(id, uint16_t(i % 2 * size), uint16_t(i / 2 * size),
                            size, size);
          bgfx::setViewClear(id, BGFX_CLEAR_DEPTH, uint32_t(0),
                             int(i) == darkTile ? shadowDepth : .9f);
          bgfx::touch(id);
        }
      } else {
        bgfx::setViewFrameBuffer(0, b.static_shadow_fb_);
        bgfx::setViewRect(0, 0, 0, size, size);
        bgfx::setViewClear(0, BGFX_CLEAR_DEPTH, uint32_t(0), shadowDepth);
        bgfx::touch(0);
      }
      if(sloped_self_receiver) {
        float sloped_projection[16];bx::mtxIdentity(sloped_projection);
        sloped_projection[2]=.2f;sloped_projection[6]=.15f;sloped_projection[10]=0;sloped_projection[14]=.5f;
        if(bgfx::getCaps()->homogeneousDepth) for(int row:{2,6,10,14}) sloped_projection[row]=2*sloped_projection[row]-sloped_projection[row+1];
        bgfx::setViewTransform(0,identity,sloped_projection);
        const auto& part=b.instance_meshes_.at(geo.get());
        const float params[4]={0,-1,0,0};b.set_uniform(b.u_static_shadow_params_,params);
        const float opaque[4]{};bgfx::setUniform(b.u_alpha_test_,opaque);bgfx::setUniform(b.u_aperture_params_,opaque);
        bgfx::setTransform(identity);bgfx::setVertexBuffer(0,part.vbh);bgfx::setIndexBuffer(part.ibh);
        bgfx::setState(BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS);
        bgfx::submit(0,b.static_shadow_program_);
      }
      if (stripeX >= 0) {
        bgfx::setViewFrameBuffer(1, b.static_shadow_fb_);
        bgfx::setViewRect(1, uint16_t(stripeX), 0, 1, size);
        bgfx::setViewClear(1, BGFX_CLEAR_DEPTH, uint32_t(0), .2f);
        bgfx::touch(1);
      }
      bgfx::setViewFrameBuffer(view, output);
      bgfx::setViewRect(view, 0, 0, size, size);
      bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                         uint32_t(0x000000ff), 0.f);
      bgfx::setViewTransform(view, identity, identity);
      bgfx::touch(view);
      b.draw_instance(view, instance, identity, preview, 0);
      bgfx::TextureRegion dst, src;
      dst.init(readback);
      src.init(color);
      bgfx::blit(100, dst, src);
      bgfx::frame();
      std::vector<uint8_t> bytes(size * size * 4);
      const auto ready = bgfx::read(dst, bytes.data());
      bool completed = false;
      for (int i = 0; i < 16; ++i)
        if (bgfx::frame() >= ready) {
          completed = true;
          break;
        }
      SLRR_REQUIRE(completed);
      const auto pos = (size / 2 * size + size / 2) * 4;
      return int(bytes[pos]) + bytes[pos + 1] + bytes[pos + 2];
    };
    const auto lit = pixel(true, true, .9f, 2);
    const auto shaded = pixel(true, true, .2f, 2);
    const auto disabled = pixel(true, false, .2f, 2);
    const auto preview = pixel(true, true, .2f, 2, true);
    const auto lamp = pixel(true, true, .2f, 2, false, true);
    const auto meshLit = pixel(false, true, .9f, 2);
    const auto meshShadow = pixel(false, true, .2f, 2);
    const auto captureShadow = pixel(false, true, .2f, 72);
    b.garage_ = true;
    b.outdoor_lighting_ = false;
    b.static_shadow_local_ = false;
    const auto garageLit = pixel(false, true, .9f, 2);
    const auto garageShadow = pixel(false, true, .2f, 2);
    const auto garageCapture = pixel(false, true, .2f, 72);
    std::printf("Garage authored light sums: lit=%d shadow=%d capture=%d\n",
                garageLit, garageShadow, garageCapture);
    SLRR_REQUIRE(garageShadow * 2 < garageLit && garageCapture == garageShadow);
    b.garage_ = false;
    b.outdoor_lighting_ = true;
    b.static_shadow_local_ = true;
    const auto unrelatedSun = pixel(true, true, .2f, 2);
    b.local_light_ = {0, 0, 3, 8};
    const auto localLit = pixel(true, true, .9f, 2);
    const auto localShadow = pixel(true, true, .2f, 2);
    const auto localMeshLit = pixel(false, true, .9f, 2);
    const auto localMeshShadow = pixel(false, true, .2f, 72);
    std::printf("Local light sums: sun unaffected=%d vehicle lit=%d shadow=%d "
                "scenery lit=%d capture shadow=%d\n",
                unrelatedSun, localLit, localShadow, localMeshLit,
                localMeshShadow);
    SLRR_REQUIRE(std::abs(unrelatedSun - lit) <= 3);
    SLRR_REQUIRE(localLit > localShadow + 20 &&
                 std::abs(localShadow - lit) <= 3);
    SLRR_REQUIRE(localMeshLit > localMeshShadow + 20 &&
                 std::abs(localMeshShadow - meshLit) <= 3);
    std::printf("RGB sums: vehicle lit=%d shadow=%d disabled=%d preview=%d "
                "lamp=%d; scenery lit=%d shadow=%d capture=%d\n",
                lit, shaded, disabled, preview, lamp, meshLit, meshShadow,
                captureShadow);
    SLRR_REQUIRE(lit > 300 && shaded > 30 && shaded < lit / 2);
    SLRR_REQUIRE(std::abs(disabled - lit) <= 3 &&
                 std::abs(preview - lit) <= 3 && lamp > lit);
    SLRR_REQUIRE(meshLit > 300 && meshShadow > 30 && meshShadow < meshLit / 2);
    SLRR_REQUIRE(std::abs(captureShadow - meshShadow) <= 3);
    // PCF compares depths before interpolation. Sampling interpolated depth
    // and then step() produced binary popping at sub-texel car-shadow edges.
    b.static_shadow_local_ = false;
    b.local_light_ = {};
    b.car_depth_active_ = true;
    b.car_depth_tex_ = shadow;
    b.car_depth_matrix_ = b.static_shadow_matrix_;
    stripeX = 32;
    std::vector<int> carEdge;
    for (int i = 0; i < 12; ++i) {
      b.car_depth_matrix_[12] = .5f + float(i) * .08f / size;
      carEdge.push_back(pixel(false, false, .9f, 5));
    }
    auto uniqueCarEdge = carEdge;
    std::sort(uniqueCarEdge.begin(), uniqueCarEdge.end());
    uniqueCarEdge.erase(std::unique(uniqueCarEdge.begin(), uniqueCarEdge.end()),
                        uniqueCarEdge.end());
    std::printf("Car shadow sub-texel edge: %zu distinct coverage values\n",
                uniqueCarEdge.size());
    SLRR_REQUIRE(uniqueCarEdge.size() > 6);
    stripeX = -1;
    b.car_depth_active_ = false;
    b.car_depth_tex_ = BGFX_INVALID_HANDLE;
    // A depth edge moving by fractions of a shadow texel must not snap,
    // even in Sharp. Very Soft must retain fractional phase in its wider tent.
    const auto saved_filters=host_enhanced_graphics();
    b.fixture_shadow_tex_=shadow;b.fixture_shadow_active_=true;b.fixture_shadow_tile_=size/4;
    SpotLight probe;probe.position={0,0,5};probe.direction={0,0,-1};probe.intensity=2;
    b.fixture_lights_={probe};
    b.fixture_shadow_matrices_={};
    auto& fixture_matrix=b.fixture_shadow_matrices_[0];
    fixture_matrix[13]=.5f;fixture_matrix[14]=.5f;fixture_matrix[15]=1;
    stripeX=size/8;
    for(int filter=0;filter<3;++filter) {
      host_enhanced_graphics().shadow_smoothing=filter;
      std::vector<int> samples;int jump=0;
      for(int step=0;step<=32;++step) {
        fixture_matrix[12]=.5f+(-1.f+step/32.f)/(size/4);
        const int value=pixel(false,false,.9f,5);
        if(!samples.empty()) jump=std::max(jump,std::abs(value-samples.back()));
        samples.push_back(value);
      }
      auto distinct=samples;std::sort(distinct.begin(),distinct.end());
      distinct.erase(std::unique(distinct.begin(),distinct.end()),distinct.end());
      std::printf("Fixture filter %d fractional edge: %zu levels, max step %d\n",filter,distinct.size(),jump);
      SLRR_REQUIRE(distinct.size()>(filter==2?5:8) && jump<=8);
    }
    host_enhanced_graphics()=saved_filters;stripeX=-1;
    b.fixture_lights_.clear();b.fixture_shadow_active_=false;b.fixture_shadow_tex_=BGFX_INVALID_HANDLE;
    const auto flat_matrix=b.static_shadow_matrix_;
    bx::mtxIdentity(b.static_shadow_matrix_.data());
    b.static_shadow_matrix_[0]=.5f;b.static_shadow_matrix_[5]=bgfx::getCaps()->originBottomLeft?.5f:-.5f;
    b.static_shadow_matrix_[2]=.2f;b.static_shadow_matrix_[6]=.15f;b.static_shadow_matrix_[10]=0;
    b.static_shadow_matrix_[12]=b.static_shadow_matrix_[13]=b.static_shadow_matrix_[14]=.5f;
    sloped_self_receiver=true;
    for(int filter=0;filter<3;++filter) {
      host_enhanced_graphics().shadow_smoothing=filter;
      const auto self=pixel(false,true,1.f,5);
      std::printf("Outdoor grazing plane filter %d: lit=%d self=%d\n",filter,meshLit,self);
      SLRR_REQUIRE(std::abs(self-meshLit)<=3);
    }
    sloped_self_receiver=false;b.static_shadow_matrix_=flat_matrix;
    host_enhanced_graphics()=saved_filters;
    // Distinct depth in each tile detects wrong atlas selection or clears
    // accidentally overwriting other view rectangles on D3D11.
    bgfx::destroy(b.static_shadow_fb_);
    const auto atlas = bgfx::createTexture2D(
        size * 2, size * 2, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    b.static_shadow_fb_ = bgfx::createFrameBuffer(1, &atlas, true);
    b.static_shadow_tex_ = atlas;
    b.static_shadow_size_ = size * 2;
    b.shadow_cascades_active_ = true;
    b.static_shadow_local_ = false;
    b.local_light_ = {};
    for (auto &matrix : b.shadow_cascade_matrices_)
      matrix = b.static_shadow_matrix_;
    const float distances[4] = {10, 70, 220, 700};
    for (int i = 0; i < 4; ++i) {
      darkTile = i;
      b.static_shadow_eye_ = {-distances[i], 0, 0};
      const auto dark = pixel(true, true, .2f, 5);
      const auto light = pixel(true, true, .9f, 5);
      const auto captured = pixel(false, true, .2f, 72);
      std::printf("Cascade %d distance %.0fm: lit=%d shadow=%d capture=%d\n", i,
                  distances[i], light, dark, captured);
      SLRR_REQUIRE(light > 300 && dark > 30 && dark < light / 2 &&
                   std::abs(captured - dark) <= 3);
    }
    darkTile = 0;
    b.static_shadow_eye_ = {-.85f * shadow_radii[0], 0, 0};  // the 85-100 % blend band
    const auto transitionStart = pixel(true, true, .2f, 5);
    b.static_shadow_eye_ = {-.925f * shadow_radii[0], 0, 0};
    const auto transitionMid = pixel(true, true, .2f, 5);
    b.static_shadow_eye_ = {-1.003f * shadow_radii[0], 0, 0};
    const auto transitionEnd = pixel(true, true, .2f, 5);
    SLRR_REQUIRE(transitionStart < transitionMid &&
                 transitionMid < transitionEnd);
    darkTile = 3;
    b.static_shadow_eye_ = {-950, 0, 0};
    const auto fade = pixel(true, true, .2f, 5);
    b.static_shadow_eye_ = {-1001, 0, 0};
    const auto beyond = pixel(true, true, .2f, 5);
    SLRR_REQUIRE(fade > 100 && fade < lit && std::abs(beyond - lit) <= 3);
    std::printf("Cascade blend=%d/%d/%d; far fade=%d beyond 1km=%d\n",
                transitionStart, transitionMid, transitionEnd, fade, beyond);
    darkTile = -1;
    b.static_shadow_eye_ = {-10, 0, 0};
    int thin[5]{};
    for (int i = 0; i < 5; ++i) {
      stripeX = 30 + i;
      thin[i] = pixel(true, true, .9f, 5);
    }
    stripeX = -1;
    std::printf("Thin shadow footprint RGB sums: %d %d %d %d %d\n", thin[0],
                thin[1], thin[2], thin[3], thin[4]);
    SLRR_REQUIRE(*std::min_element(thin, thin + 5) > 75 &&
                 *std::min_element(thin, thin + 5) < 500);
    for (int i = 1; i < 5; ++i)
      SLRR_REQUIRE(std::abs(thin[i] - thin[i - 1]) < 230);
    // The receiver must subtract the same nonzero origin as caster passes.
    darkTile = 0;
    b.static_shadow_eye_ = {-10, 0, 0};
    b.shadow_origin_ = {100, 0, 100};
    const auto absoluteMatrices = b.shadow_cascade_matrices_;
    for (auto &matrix : b.shadow_cascade_matrices_) {
      matrix[12] += 10;
      matrix[14] += 100;
    }
    const auto rebasedPixel = pixel(true, true, .2f, 5);
    SLRR_REQUIRE(std::abs(rebasedPixel - shaded) <= 3);
    b.shadow_cascade_matrices_ = absoluteMatrices;
    b.shadow_origin_ = {};
    // Render-space bounds: SCX Z is reflected before the instance transform.
    ScxGeometry bounds;
    bounds.position_scale = 1;
    for (int i = 0; i < 3; ++i) {
      bounds.info.bbox_min[i] = 1;
      bounds.info.bbox_max[i] = 2;
    }
    SceneFrustum positiveZ{};
    positiveZ[0] = {0, 0, 1, 0};
    std::array<float, 16> pose;
    bx::mtxIdentity(pose.data());
    SLRR_REQUIRE(!shadow_bounds_visible(bounds, pose, positiveZ));
    pose[14] = 3;
    SLRR_REQUIRE(shadow_bounds_visible(bounds, pose, positiveZ));
    pose[0] = 2;
    pose[5] = .5f;
    SLRR_REQUIRE(shadow_bounds_visible(bounds, pose, positiveZ));
    const float origin[3] = {0, 0, 0}, tinyMove[3] = {.0001f, 0, 0};
    const float direction[3] = {.3f, -.8f, .5f},
                laterSun[3] = {.30001f, -.8f, .5f};
    const auto initial =
        shadow_projection(origin, direction, 32, 2048, false, false);
    const auto moved =
        shadow_projection(tinyMove, direction, 32, 2048, false, false);
    const auto sunMoved =
        shadow_projection(origin, laterSun, 32, 2048, false, false);
    SLRR_REQUIRE(std::abs(initial.sample[12] - moved.sample[12]) < 1e-7f &&
                 std::abs(initial.sample[13] - moved.sample[13]) < 1e-7f);
    // Stationary receiver/camera far from the map origin; only sun angle
    // varies.
    const float farEye[3] = {5000, 20, -7000}, localEye[3] = {0, 0, 0},
                localPoint[3] = {8, -2, 3};
    float oldPrevious[2]{}, newPrevious[2]{};
    double oldJump = 0, newJump = 0;
    for (int frame = 0; frame < 400; ++frame) {
      const float sun[3] = {.3f + frame * .000001f, -.8f, .5f};
      const auto oldGrid =
          shadow_projection(farEye, sun, 32, 2048, false, false);
      const auto localGrid =
          shadow_projection(localEye, sun, 32, 2048, false, false);
      for (int row = 0; row < 2; ++row) {
        float oldUV = oldGrid.sample[12 + row],
              newUV = localGrid.sample[12 + row];
        for (int axis = 0; axis < 3; ++axis) {
          oldUV += oldGrid.sample[axis * 4 + row] *
                   (farEye[axis] + localPoint[axis]);
          newUV += localGrid.sample[axis * 4 + row] * localPoint[axis];
        }
        if (frame) {
          oldJump = std::max(oldJump,
                             double(std::abs(oldUV - oldPrevious[row]) * 2048));
          newJump = std::max(newJump,
                             double(std::abs(newUV - newPrevious[row]) * 2048));
        }
        oldPrevious[row] = oldUV;
        newPrevious[row] = newUV;
      }
    }
    std::printf("Stationary moving-sun lattice max pixel jump: global=%.5f "
                "local=%.5f\n",
                oldJump, newJump);
    SLRR_REQUIRE(oldJump > .5 && newJump < .01);
    std::array<float, 3> anchor{0, 0, 0};
    const float longDrive[3] = {500, 3, -200};
    shadow_rebase_origin(anchor, longDrive, direction);
    SLRR_REQUIRE(std::hypot(anchor[0] - longDrive[0], anchor[1] - longDrive[1],
                            anchor[2] - longDrive[2]) < 10);
    float maximumRebaseJump = 0;
    const float relativeDrive[3] = {longDrive[0] - anchor[0],
                                    longDrive[1] - anchor[1],
                                    longDrive[2] - anchor[2]};
    for (float radius : shadow_radii) {
      const auto beforeRebase =
          shadow_projection(longDrive, direction, radius, 2048, false, false);
      const auto afterRebase = shadow_projection(relativeDrive, direction,
                                                 radius, 2048, false, false);
      for (int row = 0; row < 2; ++row) {
        float before = beforeRebase.sample[12 + row],
              after = afterRebase.sample[12 + row];
        for (int axis = 0; axis < 3; ++axis) {
          before += beforeRebase.sample[axis * 4 + row] * longDrive[axis];
          after += afterRebase.sample[axis * 4 + row] * relativeDrive[axis];
        }
        maximumRebaseJump =
            std::max(maximumRebaseJump, std::abs(before - after) * 2048);
      }
    }
    std::printf("All-cascade rebase phase error: %.5f texels\n",
                maximumRebaseJump);
    SLRR_REQUIRE(maximumRebaseJump < .1f);
    SLRR_REQUIRE(initial.sample != sunMoved.sample);
    std::puts(
        "PASS sub-texel camera stability and continuous small sun changes");
    bgfx::destroy(readback);
    bgfx::destroy(output);
  }
};
} // namespace slrr
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  LibBgfxBackend backend;
  try {
    SLRR_REQUIRE(backend.open(320, 240, "Scene shadow integration"));
    SLRR_REQUIRE(bgfx::getRendererType() == bgfx::RendererType::Direct3D11);
    ShowWindow(static_cast<HWND>(backend.native_hwnd()), SW_HIDE);
    // Captures compare frames rendered at once: a real-time crossfade after a
    // sun step would weigh them differently (#371). Its own test enables it.
    render_debug().shadow_crossfade = 0.f;
    SceneShadowRegression::run(backend);
    SceneShadowRegression::real_world_cache(backend);
    SceneShadowRegression::addon_bounds(backend);
    SceneShadowRegression::scene_environment(backend);
    SceneShadowRegression::reflection_occupants();
    SceneShadowRegression::garage_geometry(backend);
    SceneShadowRegression::garage_fixtures(backend);
    SceneShadowRegression::vehicle_wall(backend);
    SceneShadowRegression::ambient_occlusion(backend);
    SceneShadowRegression::point_lifetime(backend);
    SceneShadowRegression::emissive_overlay_flush(backend);
    SceneShadowRegression::scenery_lamps(backend);
    SceneShadowRegression::car_lights(backend);
    SceneShadowRegression::nitro_flares(backend);
    SceneShadowRegression::headlight_shadows(backend);
    SceneShadowRegression::lamp_cost(backend);
    SceneShadowRegression::lamp_shadows(backend);
    SceneShadowRegression::lamp_shadow_stability(backend);
    SceneShadowRegression::car_lamp_shadows(backend);
    SceneShadowRegression::render_debug_overrides(backend);
    SceneShadowRegression::per_view_light_uniforms(backend);
    SceneShadowRegression::city_submission(backend,640,360);
    SceneShadowRegression::city_submission(backend,2560,1440);
    SceneShadowRegression::desert_capture(backend);
    SceneShadowRegression::city_submission(backend,2560,1440,true);
    SceneShadowRegression::city_submission(backend,1280,720,true,true);
    SceneShadowRegression::city_submission(backend,1280,720,true,false,true);
    // #359: by day, with the sun cascades on worker encoders.
    SceneShadowRegression::city_submission(backend,1280,720,true,true,true);
    backend.close();
    std::puts("PASS vehicle/scenery shadow visibility, preview/emission "
              "isolation and reflection-view parity");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "scene_shadow_gpu: %s\n", e.what());
    if (backend.ready())
      backend.close();
    return 1;
  }
}
