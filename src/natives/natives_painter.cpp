// java.game.Painter: the garage paint booth.
//
// The stock natives (StreetLegal_Redline.exe 0x004827D0 paintPart,
// 0x00482960 doPaint, 0x00482C00 xPaint; see EliasFD3S/slrr-reverse
// engine/Runtime/Cars/Cars.cpp) pick the object under the cursor and, when
// it is a part or a vehicle, queue a command on it: "paint 0 0 <color> 0 -1"
// for a whole part and "bigdecal <color>,<brush> <hit> <normal> <size>
// <rot> <temp> <flip>" for spray and decals. xPaint queues the empty
// temporary decal on the car, which takes the last preview off. The part's
// command handler then draws into the part's runtime texture. Here the
// natives draw directly: paint_texture.hpp holds the textures and the
// projected stamp; part_runtime shows them in the part's paint slot.
//
// Host approximations, not recovered from the executable: the stamp is a
// square `size` metres wide projected along the hit normal, "up" being the
// screen up; a temporary stroke is replaced by the next one and expires
// two unrefreshed host frames after the last; whole-part paint replaces the texture
// with one flat colour.

#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_input.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/scene_instances.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace slrr {
namespace {

constexpr const char* kPainter = "java.game.Painter";

struct PointerPart {
  Object* part = nullptr;
  ScenePickSurface surface;
};

// The stock natives ask the cursor GameRef for its position. The host has
// one system pointer, so the cursor argument only has to be present.
bool part_under_pointer(PointerPart& out) {
  const auto& in = host_input();
  float origin[3], direction[3];
  if (!in.pointer_valid ||
      !scene_instances_pointer_ray(in.pointer_x, in.pointer_y, in.view_w, in.view_h, origin,
                                   direction) ||
      !scene_instances_pick_surface(origin, direction, &out.surface))
    return false;
  out.part = part_render_owner(out.surface.key);
  return out.part != nullptr;
}

// Decoded pack textures: brushes, decals and the paint a part starts from.
std::unordered_map<uint32_t, PaintImage> g_pack_images;
const PaintImage* texture_pixels(uint32_t id) {
  if (!id) return nullptr;
  if (paint_texture_exists(id)) return paint_texture_committed(id);
  if (auto it = g_pack_images.find(id); it != g_pack_images.end())
    return it->second.valid() ? &it->second : nullptr;
  PaintImage image;
  const std::string path = scene_instances_texture(id);
  if (!path.empty() && !paint_image_from_texture(load_texture_file(path), &image))
    std::printf("[painter] texture 0x%08x (%s) could not be decoded\n", id, path.c_str());
  if (!image.valid()) return nullptr;
  return &g_pack_images.emplace(id, std::move(image)).first->second;
}

// Painter textures made for a temporary stroke only. Clearing the stroke
// gives the part its previous texture back, so a decal preview that was
// never applied does not turn the part into a saved image.
std::unordered_map<uint32_t, uint32_t> g_preview_only;
uint32_t g_temp_texture = 0;
uint64_t g_frame = 0, g_temp_frame = 0;

void clear_temp(uint32_t id) {
  if (!id) return;
  paint_texture_clear_temp(id);
  const auto fresh = g_preview_only.find(id);
  if (fresh == g_preview_only.end()) return;
  const uint32_t original = fresh->second;
  g_preview_only.erase(fresh);
  if (Object* part = part_showing_texture(id)) part_set_paint_texture(part, original);
}

// A texture this part may draw on: its own Painter texture, else a new one
// holding what the part shows now (a shared copy is never drawn on).
uint32_t own_texture(Object* part, bool temp) {
  uint32_t current = 0, source = 0;
  if (!part_paint_textures(part, &current, &source) || !source) return 0;
  if (paint_texture_exists(current) && !part_paint_texture_shared(current, part)) {
    if (!temp) g_preview_only.erase(current);
    return current;
  }
  PaintImage base;
  if (const PaintImage* shown = texture_pixels(current ? current : source)) base = *shown;
  if (!base.valid() && current != source)
    if (const PaintImage* fallback = texture_pixels(source)) base = *fallback;
  if (!base.valid()) return 0;
  const uint32_t id = paint_texture_create(std::move(base));
  if (!id || !part_set_paint_texture(part, id)) {
    paint_texture_release(id);
    return 0;
  }
  if (temp) g_preview_only[id] = current;
  return id;
}

int32_t int_arg(const std::vector<Value>& args, size_t i) {
  if (i >= args.size()) return 0;
  if (args[i].tag == ValueTag::Int) return args[i].v.i;
  if (args[i].tag == ValueTag::Float) return static_cast<int32_t>(args[i].v.f);
  return 0;
}

float float_arg(const std::vector<Value>& args, size_t i) {
  if (i >= args.size()) return 0.f;
  if (args[i].tag == ValueTag::Float) return args[i].v.f;
  if (args[i].tag == ValueTag::Int) return static_cast<float>(args[i].v.i);
  return 0.f;
}

bool has_cursor(const std::vector<Value>& args) {
  return !args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o;
}

}  // namespace

void painter_tick() {
  ++g_frame;
  if (g_temp_texture && g_frame - g_temp_frame > 2) {
    clear_temp(g_temp_texture);
    g_temp_texture = 0;
  }
  paint_texture_collect([](uint32_t id) {
    if (part_showing_texture(id)) return true;
    for (const auto& [file, state] : g_host_files)
      for (const auto& blob : state.sdat_blobs)
        if (static_cast<uint32_t>(blob.abs_id) == id) return true;
    return false;
  });
}

void register_painter_natives(Jvm& jvm) {
  g_pack_images.clear();
  g_preview_only.clear();
  g_temp_texture = 0;
  g_frame = g_temp_frame = 0;
  // void paintPart(GameRef cursor, int color): MODE_PAINTPART left click.
  jvm.natives().register_method(
      kPainter, "paintPart", "(Ljava.util.resource.GameRef;I)V",
      [](Object*, const std::vector<Value>& args) {
        PointerPart hit;
        if (!has_cursor(args) || !part_under_pointer(hit)) return Value::make_void();
        const uint32_t color = static_cast<uint32_t>(int_arg(args, 1));
        if (g_temp_texture) clear_temp(g_temp_texture), g_temp_texture = 0;
        if (const uint32_t id = own_texture(hit.part, false))
          paint_texture_draw(id, false, [color](PaintImage& image) { paint_fill(image, color); });
        return Value::make_void();
      });
  // void doPaint(GameRef cursor, int color, int brush, int temp, float rot,
  //              float size, int flip): spray (MODE_PAINTCOLOR, every
  //              control tick while held) and decals (MODE_PAINTDECAL, a
  //              temporary preview each tick, committed on click).
  jvm.natives().register_method(
      kPainter, "doPaint", "(Ljava.util.resource.GameRef;IIIFFI)V",
      [](Object*, const std::vector<Value>& args) {
        const bool temp = int_arg(args, 3) != 0;
        PointerPart hit;
        if (!has_cursor(args) || !part_under_pointer(hit) || !hit.surface.geometry ||
            hit.surface.paint_slot < 0) {
          if (temp && g_temp_texture) clear_temp(g_temp_texture), g_temp_texture = 0;
          return Value::make_void();
        }
        const PaintImage* brush = texture_pixels(static_cast<uint32_t>(int_arg(args, 2)));
        if (!brush) return Value::make_void();
        // Keep the pixels alive even if assigning the destination or
        // clearing an old preview releases this same dynamic brush.
        const PaintImage brush_copy = *brush;
        PaintStamp stamp;
        std::copy(hit.surface.local, hit.surface.local + 3, stamp.hit);
        std::copy(hit.surface.normal, hit.surface.normal + 3, stamp.normal);
        std::copy(hit.surface.screen_up, hit.surface.screen_up + 3, stamp.up);
        stamp.argb = static_cast<uint32_t>(int_arg(args, 1));
        stamp.rotation = float_arg(args, 4);
        stamp.size = float_arg(args, 5);
        stamp.flip = (int_arg(args, 6) != 0) != hit.surface.mirrored;
        const uint32_t id = own_texture(hit.part, temp);
        if (!id) return Value::make_void();
        if (g_temp_texture && g_temp_texture != id) clear_temp(g_temp_texture);
        g_temp_texture = 0;
        const auto geometry = hit.surface.geometry;
        const int slot = hit.surface.paint_slot;
        paint_texture_draw(id, temp, [&](PaintImage& image) {
          paint_stamp_mesh(image, *geometry, slot, brush_copy, stamp);
        });
        if (temp) {
          g_temp_texture = id;
          g_temp_frame = g_frame;
        }
        return Value::make_void();
      });
  // void xPaint(GameRef part): leaving the car in decal mode removes the
  // preview from every part of it.
  jvm.natives().register_method(
      kPainter, "xPaint", "(Ljava.util.resource.GameRef;)V",
      [](Object*, const std::vector<Value>& args) {
        if (g_temp_texture) clear_temp(g_temp_texture), g_temp_texture = 0;
        Object* car = !args.empty() && args[0].tag == ValueTag::Obj ? args[0].v.o : nullptr;
        if (Object* chassis = car ? part_chassis(car) : nullptr)
          for (const auto& part : part_installed_tree(chassis))
            if (paint_texture_has_temp(part.texture)) clear_temp(part.texture);
        return Value::make_void();
      });
}

}  // namespace slrr
