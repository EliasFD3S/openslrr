#include "save_io_fixture.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/gameref.hpp"
using namespace slrr;
int main() {
 Fixture f;gameref_clear();
 f.packs.set_root(fs::absolute("game").string());
 const auto humans=f.packs.open_lib("humans.rpk");
 scene_instances_set_resources(&f.packs);
 register_resref_natives(f.vm,f.packs,f.sections);
 register_renderref_natives(f.vm,f.packs,f.sections);
 register_osd_natives(f.vm,f.packs,f.sections);
 auto call=[&](const char* owner,const char* method,const char* sig,Object* self,std::vector<Value> args={}){return f.registry.invoke(owner,method,sig,self,args);};
 Object mesh,original,texture,parent,tmp,render,copy,path;
 mesh.fields["id"]=Value::make_int(RpkIndex::make_full_id(humans,0xC));
 original.fields["id"]=Value::make_int(RpkIndex::make_full_id(humans,0xD));
 gameref_upsert(&parent);
 call("java.util.resource.ResourceRef","duplicate","(Ljava.util.resource.ResourceRef;)V",&tmp,{Value::make_obj(&mesh)});
 path.str="skin-a.dds";
 call("java.util.resource.ResourceRef","makeTexture","(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",&texture,{Value::make_obj(nullptr),Value::make_obj(&path)});
 call("java.util.resource.RenderRef","changeResource","(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V",&tmp,{Value::make_obj(&original),Value::make_obj(&texture)});
 SLRR_REQUIRE(gameref_get(&tmp)->texture_file_replacements.size()==1);
 const auto expected=gameref_get(&tmp)->texture_file_replacements[0].second;
 call("java.util.resource.RenderRef","create","(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V",&render,{Value::make_obj(&parent),Value::make_obj(&tmp),Value::make_obj(nullptr)});
 call("java.util.resource.ResourceRef","duplicate","(Ljava.util.resource.ResourceRef;)V",&copy,{Value::make_obj(&tmp)});
 SLRR_REQUIRE(gameref_get(&copy)->texture_file_replacements==gameref_get(&tmp)->texture_file_replacements);
 auto instances=scene_instances_collect_root(&parent);
 bool replaced=false;
 for(const auto& instance:instances)if(instance.key==&render){
   for(const auto& entry:instance.texture_replacements)if(entry.second==expected)replaced=true;
 }
 SLRR_REQUIRE(replaced);
 // Later changes must not mutate the already-created instance or the original mesh.
 path.str="skin-b.dds";
 call("java.util.resource.ResourceRef","makeTexture","(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",&texture,{Value::make_obj(nullptr),Value::make_obj(&path)});
 call("java.util.resource.RenderRef","changeResource","(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V",&tmp,{Value::make_obj(&original),Value::make_obj(&texture)});
 SLRR_REQUIRE(gameref_get(&tmp)->texture_file_replacements.size()==1);
 SLRR_REQUIRE(gameref_get(&tmp)->texture_file_replacements[0].second!=expected);
 SLRR_REQUIRE(gameref_get(&render)->texture_file_replacements[0].second==expected);
 SLRR_REQUIRE(mesh.fields["id"].v.i==int32_t(RpkIndex::make_full_id(humans,0xC)));
 Object viewport,camera;
 call("java.render.Viewport","create","(IFFFF)V",&viewport,{Value::make_int(1),Value::make_float(0),Value::make_float(0),Value::make_float(1),Value::make_float(1)});
 call("java.render.Viewport","activate","(I)V",&viewport,{Value::make_int(0)});
 call("java.render.Camera","create","(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V",&camera,{Value::make_obj(&parent),Value::make_obj(&viewport),Value::make_int(0)});
 SLRR_REQUIRE(fullscreen_script_camera()==gameref_get(&camera));
 call("java.render.Viewport","deactivate","()V",&viewport);
 SLRR_REQUIRE(!fullscreen_script_camera() && gameref_active_cameras().empty());
 call("java.render.Viewport","activate","(I)V",&viewport,{Value::make_int(0)});
 SLRR_REQUIRE(fullscreen_script_camera()==gameref_get(&camera));
 call("java.render.Viewport","destroy","()V",&viewport);
 SLRR_REQUIRE(!fullscreen_script_camera());
 scene_instances_set_resources(nullptr);gameref_clear();
 std::puts("PASS makeTexture/changeResource/duplicate/create reaches humans material; instance independence; hidden/destroyed viewport cameras excluded");
}
