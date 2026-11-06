#pragma once

// Shared pieces for the natives_*.cpp sectors. Not a public host API.

#include "slrr/jvm.hpp"
#include "slrr/object.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/section_machine.hpp"
#include "slrr/slrr_font.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {

inline Value native_noop(Object*, const std::vector<Value>&) {
  return Value::make_void();
}

// One row of an SDAT trailer. ordinal is the file-local pack slot;
// pack_id is the host slot the path resolved to.
struct SdatPackRef {
  int32_t ordinal = 0;
  int32_t pack_id = 0;
  std::string path;
};

// Kind 7 carries embedded bytes. Other kinds are recorded with an empty
// payload. abs_id is only known for a resource written in this session.
struct SdatBlobRef {
  int32_t ordinal = 0;
  int32_t kind = 0;
  int32_t abs_id = 0;
  std::vector<uint8_t> bytes;
};

struct HostFile {
  std::vector<uint8_t> bytes;
  size_t pos = 0;
  bool writing = false;
  // True when bytes is the Java payload of an SDAT container. A raw file
  // left by an older host stays false, so readResID returns the stored id.
  bool sdat = false;
  std::filesystem::path path;
  std::vector<SdatPackRef> sdat_packs;
  std::vector<SdatBlobRef> sdat_blobs;
};

extern std::unordered_map<Object*, HostFile> g_host_files;

std::string file_name_of(Object* file);
std::filesystem::path under_game(const std::string& root, std::string rel);
bool rd_u32(const std::vector<uint8_t>& b, size_t& p, uint32_t* out);
bool rd_f32(const std::vector<uint8_t>& b, size_t& p, float* out);
int32_t res_id_of(Object* o);
bool file_push(Object* self, const void* data, size_t n);
float now_seconds();
void findfile_start(Jvm& jvm, Object* self, const std::string& pattern,
                    int32_t flags);

std::string obj_str(Object* o);
int32_t resource_id_of(Object* self);
uint32_t rpk_child_after(const RpkIndex& rpk, int32_t pack,
                         uint32_t parent_local, uint32_t after_local,
                         bool want_first);
int32_t rpk_pack_of(const RpkIndex& rpk, int32_t id);
Object* ensure_loading_screen(Jvm& jvm);

SlrrFontSlot font_slot_for_charset(Jvm& jvm, Object* charset);
void ensure_charset_loaded(Object* charset);

struct OsdView {
  float left = 0.f;
  float top = 0.f;
  float width = 1.f;
  float height = 1.f;
};

OsdView osd_view(Object* osd);
void osd_map(const OsdView& v, float lx, float ly, float* nx, float* ny);
// convertTextCoordinates world metres → OSD NDC (y+ down).
void osd_world_to_ndc(Object* osd, float wx, float wy, float* nx, float* ny);
// Mesh metres (RectangleTemplate / Style.rWidth) → NDC half extents.
// *plate is true for createRectangle backgrounds (vp baked into the mesh).
void osd_mesh_halves(Object* osd, float mesh_w, float mesh_h, float* hw,
                     float* hh, bool* plate);
void object_notify(Object* self,bool all=false);
void object_wait(Object* self, Jvm* vm=nullptr);

void register_io_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_lang_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_osd_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_gameref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_renderref_natives(Jvm& jvm, RpkIndex& rpk,
                                SectionMachine& sections);
void register_resref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_script_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);

}  // namespace slrr
