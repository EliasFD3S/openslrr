#include "slrr/natives_boot.hpp"
#include "slrr/script_threads.hpp"
#include "slrr/script_events.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include "natives_internal.hpp"

#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/gameref.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace slrr {

std::unordered_map<Object*, HostFile> g_host_files;

std::string file_name_of(Object* file) {
  if (!file) return {};
  auto it = file->fields.find("name");
  if (it == file->fields.end() || it->second.tag != ValueTag::Obj ||
      !it->second.v.o)
    return {};
  return it->second.v.o->str;
}

// Game-relative path. Rejects anything that climbs out of the game root.
std::filesystem::path under_game(const std::string& root, std::string rel) {
  namespace fs = std::filesystem;
  if (root.empty() || rel.empty()) return {};
  for (char& c : rel)
    if (c == '/') c = '\\';
  if (rel.find("..") != std::string::npos) return {};
  if (rel.size() > 1 && rel[1] == ':') return {};
  while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/'))
    rel.erase(rel.begin());
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(fs::path(root), ec);
  if (ec) return {};
  const fs::path full = fs::weakly_canonical(base / rel, ec);
  const fs::path check = ec ? (base / rel).lexically_normal() : full;
  const std::string bs = base.string();
  const std::string cs = check.string();
  if (cs.size() < bs.size() || cs.compare(0, bs.size(), bs) != 0) return {};
  return check;
}

void wr_u32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void wr_f32(std::vector<uint8_t>& b, float f) {
  uint32_t u = 0;
  static_assert(sizeof(float) == 4, "float");
  std::memcpy(&u, &f, 4);
  wr_u32(b, u);
}

bool rd_u32(const std::vector<uint8_t>& b, size_t& p, uint32_t* out) {
  if (p + 4 > b.size()) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[p + i]) << (8 * i);
  p += 4;
  *out = v;
  return true;
}

bool rd_f32(const std::vector<uint8_t>& b, size_t& p, float* out) {
  uint32_t u = 0;
  if (!rd_u32(b, p, &u)) return false;
  std::memcpy(out, &u, 4);
  return true;
}

// Soft File.write: 4 raw LE bytes, or for String a length (strlen+1) then
// the bytes including the trailing NUL. ResourceRef is rewritten to a
// file-local pack ordinal; File.close wraps the buffer in an SDAT type 2
// trailer. readResID maps that ordinal back through the pack path. A file
// that is not SDAT is still the raw payload (saves written before the
// envelope) and readResID returns the stored id unchanged.
int32_t res_id_of(Object* o) {
  if (!o) return 0;
  auto it = o->fields.find("id");
  if (it == o->fields.end() || it->second.tag != ValueTag::Int) return 0;
  return it->second.v.i;
}

bool file_push(Object* self, const void* data, size_t n) {
  auto it = g_host_files.find(self);
  if (it == g_host_files.end() || !it->second.writing || !data) return false;
  const auto* p = static_cast<const uint8_t*>(data);
  it->second.bytes.insert(it->second.bytes.end(), p, p + n);
  return true;
}

float now_seconds() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  const auto dt = clock::now() - t0;
  return std::chrono::duration<float>(dt).count();
}

bool findfile_glob_at(const std::string& name, const std::string& mask,
                      size_t ni, size_t mi) {
  const auto eq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  while (mi < mask.size()) {
    if (mask[mi] == '*') {
      while (mi < mask.size() && mask[mi] == '*') ++mi;
      if (mi >= mask.size()) return true;
      for (size_t k = ni; k <= name.size(); ++k)
        if (findfile_glob_at(name, mask, k, mi)) return true;
      return false;
    }
    if (ni >= name.size()) return false;
    if (mask[mi] != '?' && !eq(name[ni], mask[mi])) return false;
    ++ni;
    ++mi;
  }
  return ni == name.size();
}

bool findfile_glob(const std::string& name, const std::string& mask) {
  // Win32 FindFirstFile: "*.*" and "*" match every name, including no dot.
  if (mask.empty() || mask == "*" || mask == "*.*") return true;
  return findfile_glob_at(name, mask, 0, 0);
}

// Stock FindFile returns the file name only (callers prefix the directory).
void findfile_start(Jvm& jvm, Object* self, const std::string& pattern,
                    int32_t flags) {
  self->find_entries.clear();
  self->find_index = 0;
  std::string pat = pattern;
  for (char& c : pat)
    if (c == '/') c = '\\';
  const auto slash = pat.find_last_of('\\');
  const std::string dir =
      slash == std::string::npos ? std::string() : pat.substr(0, slash);
  std::string mask =
      slash == std::string::npos ? pat : pat.substr(slash + 1);
  if (mask.empty()) mask = "*";

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path base(jvm.game_root());
  if (!dir.empty()) base /= dir;
  if (!fs::is_directory(base, ec)) {
    std::printf("[native] FindFile.first(%s, %d) -> 0 (no dir)\n",
                pattern.c_str(), flags);
    return;
  }
  for (fs::directory_iterator it(base, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code ec2;
    const bool is_dir = it->is_directory(ec2);
    if (flags == 1 && is_dir) continue;   // FILES_ONLY
    if (flags == 2 && !is_dir) continue;  // DIRS_ONLY
    const std::string name = it->path().filename().string();
    if (name == "." || name == "..") continue;
    if (!findfile_glob(name, mask)) continue;
    self->find_entries.push_back(name);
  }
  std::sort(self->find_entries.begin(), self->find_entries.end(),
            [](const std::string& a, const std::string& b) {
              const auto lower = [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
              };
              return std::lexicographical_compare(
                  a.begin(), a.end(), b.begin(), b.end(),
                  [&](char x, char y) { return lower(x) < lower(y); });
            });
  std::printf("[native] FindFile.first(%s, %d) -> %zu\n", pattern.c_str(),
              flags, self->find_entries.size());
}

std::string obj_str(Object* o) {
  if (!o) return {};
  if (!o->str.empty()) return o->str;
  auto it = o->fields.find("value");
  if (it != o->fields.end() && it->second.tag == ValueTag::Obj &&
      it->second.v.o)
    return it->second.v.o->str;
  return {};
}

int32_t resource_id_of(Object* self) {
  if (!self) return 0;
  if (GameRefState* st = gameref_get(self))
    if (st->type_id != 0) return st->type_id;
  auto it = self->fields.find("id");
  if (it != self->fields.end() && it->second.tag == ValueTag::Int)
    return it->second.v.i;
  return 0;
}

// Same-pack children: super_id low 16 is the parent local id.
uint32_t rpk_child_after(const RpkIndex& rpk, int32_t pack, uint32_t parent_local,
                         uint32_t after_local, bool want_first) {
  const RpkPack* p = rpk.pack_by_id(pack);
  if (!p) return 0;
  bool seen = want_first;
  for (const RpkEntry& e : p->entries) {
    if ((e.super_id & 0xFFFFu) != (parent_local & 0xFFFFu)) continue;
    if ((e.super_id >> 16) != 0) continue;
    if (!seen) {
      if ((e.type_id & 0xFFFFu) == (after_local & 0xFFFFu)) seen = true;
      continue;
    }
    return RpkIndex::make_full_id(pack, e.type_id);
  }
  return 0;
}

int32_t rpk_pack_of(const RpkIndex& rpk, int32_t id) {
  const int32_t pack = RpkIndex::pack_from_full(static_cast<uint32_t>(id));
  if (pack > 0 && rpk.pack_by_id(pack)) return pack;
  const uint32_t local = static_cast<uint32_t>(id) & 0xFFFFu;
  for (int32_t i = 1; i < rpk.pack_count(); ++i) {
    if (rpk.find_entry(i, local)) return i;
  }
  // Folder id that is only a parent, not itself an entry: pack that has children.
  for (int32_t i = 1; i < rpk.pack_count(); ++i) {
    if (rpk_child_after(rpk, i, local, 0, true) != 0) return i;
  }
  return 0;
}

Object* ensure_loading_screen(Jvm& jvm) {
  Value ls = jvm.get_static("java.render.Frontend", "loadingScreen");
  if (ls.tag == ValueTag::Obj && ls.v.o) return ls.v.o;
  Object* o = jvm.new_object("java.render.LoadingScreen");
  jvm.set_static("java.render.Frontend", "loadingScreen", Value::make_obj(o));
  return o;
}



int font_band(int32_t video_x, int32_t video_y) {
  if (video_y >= 768) return 3;
  if (video_x >= 800 || video_y >= 600) return 2;
  if (video_x >= 640 || video_y >= 480) return 1;
  return 0;
}

int g_font_band = -1;

Object* frontend_font_static(Jvm& jvm, const char* name) {
  Value v = jvm.get_static("java.render.Frontend", name);
  return (v.tag == ValueTag::Obj) ? v.v.o : nullptr;
}

SlrrFontSlot font_slot_for_charset(Jvm& jvm, Object* charset) {
  if (!charset) return SlrrFontSlot::None;
  if (charset == frontend_font_static(jvm, "smallFont"))
    return SlrrFontSlot::Small;
  if (charset == frontend_font_static(jvm, "mediumFont"))
    return SlrrFontSlot::Medium;
  if (charset == frontend_font_static(jvm, "largeFont"))
    return SlrrFontSlot::Large;
  return SlrrFontSlot::None;
}

void ensure_charset_loaded(Object* charset) {
  if (!charset || slrr_font_ready(charset)) return;
  int32_t rid = 0;
  auto it = charset->fields.find("id");
  if (it != charset->fields.end() && it->second.tag == ValueTag::Int)
    rid = it->second.v.i;
  if (!slrr_font_load_from_rid(charset, rid))
    slrr_font_load(charset, "simple20");
}



std::function<bool()> g_script_pump;
std::function<void()> g_loading_present;
std::function<void()> g_force_present;
std::function<bool(const std::string&)> g_screen_capture;
// 1 = inside wait, 2 = notify arrived for this object.
struct ObjectWaiter { int state=1; };
std::unordered_map<Object*, std::vector<ObjectWaiter*>> g_object_wait;
// Modal dialogs currently blocked in Object.wait (display() of a DF_MODAL).
int g_dialog_waits = 0;

void object_notify(Object* self,bool all) {
  auto it=g_object_wait.find(self);if(it==g_object_wait.end())return;
  for(auto* waiter:it->second)if(waiter->state==1){waiter->state=2;if(!all)break;}
}

float osd_field_f(Object* o, const char* key, float fallback) {
  if (!o) return fallback;
  auto it = o->fields.find(key);
  if (it == o->fields.end()) return fallback;
  if (it->second.tag == ValueTag::Float) return it->second.v.f;
  if (it->second.tag == ValueTag::Int) return static_cast<float>(it->second.v.i);
  return fallback;
}

// Dialog OSDs are a viewport (size 0.6, centered). Widget x,y are local
// text space (-1..1, y+ down) inside that viewport, not fullscreen NDC.
OsdView osd_view(Object* osd) {
  OsdView v;
  const float w = osd_field_f(osd, "vpWidth", 1.f);
  const float h = osd_field_f(osd, "vpHeight", 1.f);
  if (w > 0.02f) v.width = w;
  if (h > 0.02f) v.height = h;
  v.left = osd_field_f(osd, "vpLeft", 0.f);
  v.top = osd_field_f(osd, "vpTop", 0.f);
  return v;
}

void osd_map(const OsdView& v, float lx, float ly, float* nx, float* ny) {
  const float sx = v.left + (lx + 1.f) * 0.5f * v.width;
  const float sy = v.top + (ly + 1.f) * 0.5f * v.height;
  if (nx) *nx = sx * 2.f - 1.f;
  if (ny) *ny = sy * 2.f - 1.f;
}

void osd_world_to_ndc(Object* osd, float wx, float wy, float* nx, float* ny) {
  // Osd.convertTextCoordinates: (x*SCALE_3D*vpAspect*vpWidth, -y*SCALE_3D*vpHeight).
  constexpr float kScale3d = 1.469f;
  float vp_a = osd_framebuffer_aspect();
  if (vp_a < 0.1f) vp_a = 1.f;
  const float vp_w = osd_field_f(osd, "vpWidth", 1.f);
  const float vp_h = osd_field_f(osd, "vpHeight", 1.f);
  const float dx = kScale3d * vp_a * (vp_w > 0.02f ? vp_w : 1.f);
  const float dy = kScale3d * (vp_h > 0.02f ? vp_h : 1.f);
  const float sx = std::fabs(dx) > 1e-6f ? wx / dx : wx;
  const float sy = std::fabs(dy) > 1e-6f ? -wy / dy : wy;
  osd_map(osd_view(osd), sx, sy, nx, ny);
}

void osd_mesh_halves(Object* osd, float mesh_w, float mesh_h, float* hw,
                     float* hh, bool* plate) {
  (void)osd;
  constexpr float kScaleFs = 1.47f;
  float vp_a = osd_framebuffer_aspect();
  if (vp_a < 0.1f) vp_a = 1.f;
  float half_w = mesh_w / (2.f * kScaleFs * vp_a);
  float half_h = mesh_h / (2.f * kScaleFs);
  // createRectangle bakes vpWidth into the template (mesh taller than 0.5 m
  // or wider than a button). Style.rt does not, except the fullscreen shield.
  const bool is_plate = mesh_h > 0.5f || mesh_w > 2.5f;
  // The OSD camera sits at CAM_OFFSET * vpHeight, so a mesh covers the same
  // fraction of every viewport. Multiplying by the viewport size again made
  // a catalog cell's hitbox a small patch in the middle of the slot. The
  // button is authored to fill that slot (Style(itemWidth*2, itemHeight*2)).
  // Dialog shield is Style(2, 2, vpAspect). backShieldOsd is built with
  // aspect 0, so rWidth is 0 and the box does not cover the menu.
  if (is_plate && half_w < 0.05f && half_h > 0.5f) half_w = 1.f;
  if (is_plate && half_h < 0.05f && half_w > 0.5f) half_h = 1.f;
  if (hw) *hw = half_w;
  if (hh) *hh = half_h;
  if (plate) *plate = is_plate;
}

void object_wait(Object* self, Jvm* vm) {
  if (!self) return;
  if(self->class_name=="java.util.MessagePort") {
    auto f=self->fields.find("fifo");
    if(f!=self->fields.end()&&f->second.tag==ValueTag::Obj&&f->second.v.o) {
      auto n=f->second.v.o->fields.find("elementCount");
      if(n!=f->second.v.o->fields.end()&&n->second.tag==ValueTag::Int&&n->second.v.i>0)return;
    }
  }
  ObjectWaiter waiter;
  const bool cooperative=vm && script_thread_active(*vm);
  g_object_wait[self].push_back(&waiter);
  struct WaitGuard {
    Object* key;ObjectWaiter* waiter;
    ~WaitGuard(){auto i=g_object_wait.find(key);if(i==g_object_wait.end())return;
      auto& list=i->second;list.erase(std::remove(list.begin(),list.end(),waiter),list.end());
      if(list.empty())g_object_wait.erase(i);}
  } wait_guard{self,&waiter};
  // Garage.enter shows its welcome warning before enter() returns. The stock
  // LoadingScreen thread drops the plate meanwhile; count the modal wait so
  // the host does the same instead of covering the dialog indefinitely.
  struct DialogWaitGuard {
    bool active;
    ~DialogWaitGuard(){if(active)--g_dialog_waits;}
  } dialog_guard{vm && vm->tree_instance_of(self,"java.render.osd.dialog.Dialog")};
  if(dialog_guard.active)++g_dialog_waits;
  if(!cooperative)std::printf("[native] Object.wait %s pump=%d\n", self->class_name.c_str(),
              g_script_pump ? 1 : 0);
  std::fflush(stdout);
  bool woken = false;
  if(cooperative) {
    while(waiter.state==1)script_thread_sleep(*vm,1.f);
    woken=true;
  } else if (!g_script_pump) {
    std::printf("[native] Object.wait %s no pump\n",
                self->class_name.c_str());
  } else {
    while (waiter.state == 1) {
      if (!g_script_pump()) {
        std::printf("[native] Object.wait %s pump stop\n",
                    self->class_name.c_str());
        break;
      }
    }
    woken = waiter.state == 2;
  }
  // display() hides next. Drop the shield here too: hide()'s
  // `if (backShieldOsd)` can miss the quad that actually covers the menu.
  if(woken && vm && vm->tree_instance_of(self,"java.render.osd.dialog.Dialog")) {
    auto field=[&](const char* name)->Object* {
      auto i=self->fields.find(name);
      return i!=self->fields.end()&&i->second.tag==ValueTag::Obj?i->second.v.o:nullptr;
    };
    Object* shield=field("backShieldOsd");
    osd_drop_modal_shield(shield?shield:field("osd"));
  }
  // An aborted wait must not look like OK (result stays 0 and NEW career
  // saves and enters the garage). notify() is the only success path.
  if (!woken) {
    auto it = self->fields.find("result");
    if (it == self->fields.end() || it->second.tag != ValueTag::Int ||
        it->second.v.i == 0)
      self->fields["result"] = Value::make_int(-1);
  }
  if(!cooperative)std::printf("[native] Object.wait resume %s woken=%d\n",
              self->class_name.c_str(), woken ? 1 : 0);
  std::fflush(stdout);
}


void host_set_script_pump(std::function<bool()> fn) {
  g_script_pump = std::move(fn);
}
bool host_dialog_wait_active() { return g_dialog_waits > 0; }
bool host_wait_pending(Object* signal) {
  const auto it = g_object_wait.find(signal);
  return it != g_object_wait.end() && std::any_of(
      it->second.begin(), it->second.end(),
      [](const ObjectWaiter* waiter) { return waiter->state == 1; });
}
void host_script_sleep(float milliseconds) {
  if(!std::isfinite(milliseconds)||milliseconds<=0)return;
  const auto start=std::chrono::steady_clock::now();
  while(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<milliseconds) {
    if(g_script_pump && !g_script_pump())break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void host_set_loading_present(std::function<void()> fn) {
  g_loading_present = std::move(fn);
}

void host_loading_present() {
  if (g_loading_present) g_loading_present();
}
void host_set_render_callbacks(std::function<void()> present,
                               std::function<bool(const std::string&)> screenshot) {
  g_force_present = std::move(present);
  g_screen_capture = std::move(screenshot);
}
void host_force_present() {
  static bool presenting = false;
  if (!g_force_present || presenting) return;
  presenting = true;
  struct Guard { bool& value; ~Guard() { value = false; } } guard{presenting};
  g_force_present();
}
bool host_print_screen(const std::string& path) {
  return g_screen_capture && g_screen_capture(path);
}

void frontend_set_fonts(Jvm& jvm, int32_t video_x, int32_t video_y) {
  if (video_x <= 0) video_x = 800;
  if (video_y <= 0) video_y = 600;
  jvm.set_static("java.game.Config", "video_x", Value::make_int(video_x));
  jvm.set_static("java.game.Config", "video_y", Value::make_int(video_y));
  // Viewport reads java.util.Config. Left at 800x600, a 16:9 window builds
  // a dialog plate that is the full window aspect — stretched sideways.
  jvm.set_static("java.util.Config", "video_x", Value::make_int(video_x));
  jvm.set_static("java.util.Config", "video_y", Value::make_int(video_y));

  const int band = font_band(video_x, video_y);
  // Same band → keep existing ResourceRefs so live Text charset pointers stay valid.
  if (band == g_font_band && g_font_band >= 0) return;

  auto mk_font = [&jvm](int32_t rid) {
    Object* f = jvm.new_object("java.util.resource.ResourceRef");
    f->fields["id"] = Value::make_int(rid);
    return f;
  };
  // Text.RID_*: SLII24=0x25, SLII17=0x139, SLII11=0x138, CONSOLE5=0xB2.
  int32_t large = 0x0025, medium = 0x0139, small = 0x0138;
  const char* band_name = "800x600";
  if (band == 3) {
    large = 0x0025;
    medium = 0x0025;
    small = 0x0139;
    band_name = ">=768p";
  } else if (band == 2) {
    large = 0x0025;
    medium = 0x0139;
    small = 0x0138;
    band_name = "800x600";
  } else if (band == 1) {
    large = 0x0139;
    medium = 0x0138;
    small = 0x00B2;
    band_name = "640x480";
  } else {
    large = medium = small = 0x00B2;
    band_name = "low";
  }
  // Stock Frontend.setFonts. simple20/simple40 stay off these slots:
  // they are only the in-car HUD, not menu or garage text.
  // >=768: large SLII24, medium SLII24, small SLII17.
  // 800x600: large SLII24, medium SLII17, small SLII11.
  Object* large_o = mk_font(large);
  Object* medium_o = mk_font(medium);
  Object* small_o = mk_font(small);
  ensure_charset_loaded(large_o);
  ensure_charset_loaded(medium_o);
  ensure_charset_loaded(small_o);
  jvm.set_static("java.render.Frontend", "largeFont", Value::make_obj(large_o));
  jvm.set_static("java.render.Frontend", "mediumFont",
                 Value::make_obj(medium_o));
  jvm.set_static("java.render.Frontend", "smallFont", Value::make_obj(small_o));
  g_font_band = band;
  slrr_font_set_ui_keys(small_o, medium_o, large_o);
  slrr_text_rebind_frontend_fonts(small_o, medium_o, large_o);
  // Keep java.render.Text.charset in sync for rebound instances.
  for (const SlrrOsdText& ot : slrr_osd_texts()) {
    if (!ot.text_key) continue;
    const SlrrFontSlot slot = slrr_text_font_slot(ot.text_key);
    Object* font = nullptr;
    if (slot == SlrrFontSlot::Small)
      font = small_o;
    else if (slot == SlrrFontSlot::Medium)
      font = medium_o;
    else if (slot == SlrrFontSlot::Large)
      font = large_o;
    if (!font) continue;
    auto* text = static_cast<Object*>(ot.text_key);
    text->fields["charset"] = Value::make_obj(font);
  }
  std::printf("[font] Frontend.setFonts %dx%d band=%s small=0x%X medium=0x%X\n",
              video_x, video_y, band_name, small, medium);
}

void register_boot_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  jvm.add_gc_roots([&sections,&jvm](const GcRootVisitor& visit) {
    script_threads_roots(jvm,visit);
    script_events_roots(jvm,visit);
    visit(sections.actual()); visit(sections.garage());
    for (const auto& entry : g_host_files) visit(entry.first);
    for (const auto& entry : g_object_wait) visit(entry.first);
    gameref_visit_gc_roots(visit);
    part_visit_gc_roots(visit);
    osd_visit_gc_roots(visit);
    font_visit_gc_roots(visit);
    mainloop_visit_gc_roots(visit);
    dyno_visit_gc_roots(visit);
    scene_visit_gc_roots(visit);
  });
  scene_instances_set_resources(&rpk);
  register_io_natives(jvm, rpk, sections);
  register_lang_natives(jvm, rpk, sections);
  register_osd_natives(jvm, rpk, sections);
  register_gameref_natives(jvm, rpk, sections);
  register_renderref_natives(jvm, rpk, sections);
  register_resref_natives(jvm, rpk, sections);
  register_script_natives(jvm, rpk, sections);
  register_part_natives(jvm,rpk);
  install_osd_script_hover(jvm);

  // Statics
  jvm.set_static("java.io.Input", "AXIS_CANCEL", Value::make_int(35));
  jvm.set_static("java.io.Input", "AXIS_MENU", Value::make_int(36));
  jvm.set_static("java.io.Input", "AXIS_LOOK_LEFTRIGHT", Value::make_int(7));
  jvm.set_static("java.io.Input", "AXIS_LOOK_UPDOWN", Value::make_int(8));
  jvm.set_static("java.io.Input", "AXIS_LOOK_FWDBACK", Value::make_int(10));
  jvm.set_static("java.io.Input", "VIRTUAL", Value::make_int(1));
  jvm.set_static("java.io.ControlSet", "CAMTURNSET", Value::make_int(3));
  jvm.set_static("java.render.Osd", "HK_STATIC", Value::make_int(0x04));
  jvm.set_static("java.render.Osd", "MD_VERTICAL", Value::make_int(0));
  jvm.set_static("java.render.Osd", "MD_HORIZONTAL", Value::make_int(1));
  jvm.set_static("java.render.Osd", "SCALE_FS", Value::make_float(1.47f));
  jvm.set_static("java.render.Osd", "RID_BACK", Value::make_int(0x0126));
  jvm.set_static("java.render.Osd", "RID_ARROWUP", Value::make_int(0x004C));
  jvm.set_static("java.render.Osd", "RID_ARROWDN", Value::make_int(0x0084));
  jvm.set_static("java.render.Osd", "RID_ARROWLF", Value::make_int(0x0085));
  jvm.set_static("java.render.Osd", "RID_ARROWRG", Value::make_int(0x0088));
  // Osd.RRT_NONE = RRT_TEST = frontend:0x18 — transparent, no button mesh.
  {
    Object* none = jvm.new_object("java.util.resource.ResourceRef");
    none->fields["id"] = Value::make_int(0x0018);
    jvm.set_static("java.render.Osd", "RRT_NONE", Value::make_obj(none));
    jvm.set_static("java.render.Osd", "RRT_TEST", Value::make_obj(none));
  }
  // Stock Text.java (not CSS): LEFT=2, CENTER=1, RIGHT=0.
  jvm.set_static("java.render.Text", "ALIGN_LEFT", Value::make_int(2));
  jvm.set_static("java.render.Text", "ALIGN_CENTER", Value::make_int(1));
  jvm.set_static("java.render.Text", "ALIGN_RIGHT", Value::make_int(0));
  // Default until window size is known (boot calls frontend_set_fonts after open).
  frontend_set_fonts(jvm, 800, 600);
  jvm.set_static("java.game.GameLogic", "day", Value::make_int(1));
  jvm.set_static("java.game.GameLogic", "time",
                 Value::make_float(12.f * 3600.f));
  jvm.set_static("java.game.GameLogic", "CLUBMEMBERS", Value::make_int(20));
  jvm.set_static("java.game.Config", "majomParade", Value::make_int(0));
  // Garage.CMD_* come from TUFA static-field init trees on class load.
  jvm.set_static("java.util.resource.GameRef", "EVENT_TIME",
                 Value::make_int(0x80));
  jvm.set_static("java.util.resource.GameRef", "EVENT_ANY",
                 Value::make_int(0x0FFFFFFF));
  jvm.set_static("java.game.MainMenuDialog", "CMD_NEW", Value::make_int(50));
  // Dialog.java flags — Soft CMD_NEW/LOAD/EXIT IR reads these statics.
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_MODAL",
                 Value::make_int(0x00000001));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_FULLSCREEN",
                 Value::make_int(0x00000002));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_DEFAULTBG",
                 Value::make_int(0x00000004));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_FREEZE",
                 Value::make_int(0x00000008));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_LEAVEPOINTER",
                 Value::make_int(0x00000010));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_LOWPRI",
                 Value::make_int(0x00000020));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_HIGHPRI",
                 Value::make_int(0x00000040));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_WIDE",
                 Value::make_int(0x00000080));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_DARKEN",
                 Value::make_int(0x00000100));
  jvm.set_static("java.render.osd.dialog.Dialog", "DF_SILENT",
                 Value::make_int(0x00000200));
  jvm.set_static("java.render.osd.dialog.Dialog", "SIF_NOEMPTY",
                 Value::make_int(0x00001000));
  jvm.set_static("java.render.osd.dialog.Dialog", "FRF_SAVE",
                 Value::make_int(0x00001000));
  ensure_loading_screen(jvm);
}

}  // namespace slrr
