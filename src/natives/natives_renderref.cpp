#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/skeleton.hpp"
#include "slrr/scene_sun.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace slrr {
namespace {
std::array<float, 3> line_vector(Object* object, std::array<float, 3> fallback) {
  if (!object) return fallback;
  const char* fields[] = {"x", "y", "z"};
  for (size_t i = 0; i < fallback.size(); ++i) {
    const auto field = object->fields.find(fields[i]);
    if (field == object->fields.end()) continue;
    const auto value = field->second;
    if (value.tag == ValueTag::Float) fallback[i] = value.v.f;
    if (value.tag == ValueTag::Int) fallback[i] = static_cast<float>(value.v.i);
  }
  return fallback;
}

void append_line_segment(GameRefState& line, const RenderLinePoint& start,
                         const RenderLinePoint& end, uint32_t color) {
  using physics::Vec3;
  const auto vector = [](const auto& p) { return Vec3{p[0], p[1], p[2]}; };
  const Vec3 delta = vector(end.pos) - vector(start.pos);
  const double length = delta.length();
  if (length < 1e-5) return;
  const auto side = [&](const RenderLinePoint& p) {
    return vector(p.normal).cross(delta).normalized() * (p.width * .5f);
  };
  if (side(start).length() < 1e-6 || side(end).length() < 1e-6) return;
  // Published geometry is immutable while the renderer owns a snapshot.
  if (line.line_geometry.use_count() > 1)
    line.line_geometry = std::make_shared<ScxGeometry>(*line.line_geometry);
  auto& mesh = *line.line_geometry;
  const auto first = static_cast<uint32_t>(mesh.positions.size() / 3);
  const auto index_start = static_cast<uint32_t>(mesh.indices.size());
  const float next_length = line.line_length + static_cast<float>(length);
  const auto add_pair = [&](const RenderLinePoint& point, float distance) {
    const auto pos = vector(point.pos);
    const auto offset = side(point);
    const auto normal = vector(point.normal).normalized();
    for (const auto& vertex : {pos - offset, pos + offset}) {
      mesh.positions.insert(mesh.positions.end(),
          {float(vertex.x), float(vertex.y), float(vertex.z)});
      mesh.normals.insert(mesh.normals.end(),
          {float(normal.x), float(normal.y), float(normal.z)});
    }
    mesh.uvs.insert(mesh.uvs.end(), {0.f, distance, 1.f, distance});
  };
  add_pair(start, line.line_length);
  add_pair(end, next_length);
  line.line_length = next_length;
  mesh.indices.insert(mesh.indices.end(),
      {first, first + 2, first + 1, first + 1, first + 2, first + 3});
  ScxGroup group;
  group.index_start = index_start;
  group.index_count = 6;
  if (!mesh.textures.empty()) group.texture_index = 0;
  for (int i = 0; i < 3; ++i) group.diffuse[i] = ((color >> (16 - i * 8)) & 255) / 255.f;
  if (!mesh.groups.empty() && std::equal(std::begin(group.diffuse),
      std::end(group.diffuse), std::begin(mesh.groups.back().diffuse)))
    mesh.groups.back().index_count += group.index_count;
  else
    mesh.groups.push_back(group);
  mesh.info.vertices = static_cast<int32_t>(mesh.positions.size() / 3);
  mesh.info.triangles = static_cast<int32_t>(mesh.indices.size() / 3);
  mesh.info.groups = static_cast<int32_t>(mesh.groups.size());
}
}

// A directional light resource is text. Colours are "a r g b". Direction is
// where the light travels. setLight on the type (time of day) wins over the
// file colours; the file still supplies the direction. Point lights are
// left alone. The garage light is misc.garage:0x001D, straight down.
void apply_directional_light(const RpkIndex& rpk, int32_t full_id, Object* type,GameRefState& instance) {
  if (full_id == 0) return;
  const std::vector<uint8_t> bytes =
      rpk.read_payload(static_cast<uint32_t>(full_id));
  if (bytes.size() < 8) return;
  const std::string text(bytes.begin(), bytes.end());
  bool directional = false;
  float diff[3]{}, amb[3]{}, spec[3] = {1.f, 1.f, 1.f}, dir[3]{};
  bool have_d = false, have_a = false, have_s = false, have_dir = false;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, 4, "type") == 0 &&
        line.find("directional") != std::string::npos)
      directional = true;
    float a = 0, r = 0, g = 0, b = 0;
    if (line.compare(0, 7, "diffuse") == 0 &&
        std::sscanf(line.c_str(), "%*s %f %f %f %f", &a, &r, &g, &b) == 4) {
      diff[0] = r;
      diff[1] = g;
      diff[2] = b;
      have_d = true;
    } else if (line.compare(0, 7, "ambient") == 0 &&
               std::sscanf(line.c_str(), "%*s %f %f %f %f", &a, &r, &g, &b) ==
                   4) {
      amb[0] = r;
      amb[1] = g;
      amb[2] = b;
      have_a = true;
    } else if (line.compare(0, 8, "specular") == 0 &&
               std::sscanf(line.c_str(), "%*s %f %f %f %f", &a, &r, &g, &b) ==
                   4) {
      spec[0] = r;
      spec[1] = g;
      spec[2] = b;
      have_s = true;
    } else if (line.compare(0, 9, "direction") == 0 &&
               std::sscanf(line.c_str(), "%*s %f %f %f", &r, &g, &b) == 3) {
      dir[0] = r;
      dir[1] = g;
      dir[2] = b;
      have_dir = true;
    }
  }
  if (!directional || !have_d) return;
  if (!have_a) amb[0] = amb[1] = amb[2] = 0.f;
  if (!have_s) spec[0] = spec[1] = spec[2] = 1.f;
  if (!have_dir) dir[0] = dir[1] = dir[2] = 0.f;
  auto& light=instance.directional_light;
  if (const auto* source = gameref_get(type)) light.flare = source->directional_light.flare;
  std::copy(diff,diff+3,light.diffuse);
  std::copy(amb,amb+3,light.ambient);
  std::copy(spec,spec+3,light.specular);
  std::copy(dir,dir+3,light.direction);
  light.set=true;
  if (instance.parent) gameref_make_live(instance.key);
  gameref_register_light(instance.key);
  light.has_direction=dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]>1e-8f;
  auto packed = [](Object* o, const char* key, int32_t* out) {
    if (!o) return false;
    auto it = o->fields.find(key);
    if (it == o->fields.end() || it->second.tag != ValueTag::Int) return false;
    *out = it->second.v.i;
    return true;
  };
  int32_t sd = 0, sa = 0, ss = 0;
  if (packed(type, "_sun_d", &sd) && packed(type, "_sun_a", &sa) &&
      packed(type, "_sun_s", &ss))
    scene_sun_apply_packed(light,sd,sa,ss);
}

void register_renderref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  jvm.natives().register_method(
      "java.game.Navigator", "updateNavigator", "(Ljava.util.resource.GameRef;I)V",
      [](Object* self, const std::vector<Value>& args) {
        const auto field = [](const Object* object, const char* name) {
          if (!object) return Value::make_void();
          const auto value = object->fields.find(name);
          return value == object->fields.end() ? Value::make_void() : value->second;
        };
        const auto object = [&](const Object* owner, const char* name) -> Object* {
          const auto value = field(owner, name);
          return value.tag == ValueTag::Obj ? value.v.o : nullptr;
        };
        const auto number = [&](const Object* owner, const char* name) {
          const auto value = field(owner, name);
          if (value.tag == ValueTag::Int) return float(value.v.i);
          return value.tag == ValueTag::Float ? value.v.f : 0.f;
        };
        const auto pose = [](Object* ref, float& x, float& z, float& yaw) {
          const auto* state = gameref_get(ref);
          if (!state || state->empty) return false;
          x = state->pos[0];
          z = state->pos[2];
          yaw = state->ypr[0];
          float frame[16];
          if (scene_instances_script_matrix(ref, frame)) {
            x = frame[12];
            z = frame[14];
            yaw = std::atan2(frame[8], frame[10]);
          }
          return true;
        };
        Object* car = !args.empty() && args[0].tag == ValueTag::Obj ? args[0].v.o : nullptr;
        const int mode = args.size() >= 2 && args[1].tag == ValueTag::Int ? args[1].v.i : 0;
        float x = 0, z = 0, yaw = 0;
        auto* camera = object(self, "cam");
        auto* target = mode >= 2 ? object(self, "gmCamera") : car;
        if (auto* cam = gameref_get(camera); cam && pose(target ? target : car, x, z, yaw))
          gameref_set_matrix(camera, x * .01f, cam->pos[1], z * .01f,
                             (mode & 1) ? yaw : 0.f, -1.5707963f, 0.f);
        for (const char* name : {"marker", "dynamarker"}) {
          const auto* list = object(self, name);
          const auto* entries = object(list, "elementData");
          const auto count = field(list, "elementCount");
          if (!entries || count.tag != ValueTag::Int || count.v.i <= 0) continue;
          const size_t end = std::min(entries->slots.size(), size_t(count.v.i));
          for (size_t i = 0; i < end; ++i) {
            const auto entry = entries->slots[i];
            if (entry.tag != ValueTag::Obj) continue;
            auto* symbol = object(entry.v.o, "symbol");
            if (const auto* pos = object(entry.v.o, "pos")) {
              gameref_set_matrix(symbol, number(pos, "x") * .01f, number(pos, "y"),
                                 number(pos, "z") * .01f, 0, 0, 0);
            } else if (pose(object(entry.v.o, "mover"), x, z, yaw)) {
              gameref_set_matrix(symbol, x * .01f, .1f, z * .01f, yaw, 0, 0);
            }
          }
        }
        return Value::make_void();
      });
  // ---- java.util.resource.RenderRef (declaring class) ----
  // Java: create(parent,rid,alias) → create(parent, new RenderRef(rid), alias).
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "create",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;"
      "Ljava.lang.String;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* parent =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* type =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        Object* alias =
            (args.size() >= 3 && args[2].tag == ValueTag::Obj) ? args[2].v.o
                                                               : nullptr;
        // Soft PE RenderRef.create @ 0x480EE0 — INSTANCE_RENDER.
        GameRefState* st = gameref_upsert(self, GameRefKind::RenderRef);
        if (!st) return Value::make_void();
        st->script = self;
        st->class_name = self->class_name;
        st->empty = false;
        if (parent) gameref_set_parent(self, parent);
        if (type) {
          int32_t tid = 0;
          if (GameRefState* ts = gameref_get(type)) {
            tid = ts->type_id;
            st->texture_replacements = ts->texture_replacements;
          }
          if (tid == 0) {
            auto it = type->fields.find("id");
            if (it != type->fields.end() && it->second.tag == ValueTag::Int)
              tid = it->second.v.i;
          }
          st->type_id = tid;
          self->fields["id"] = Value::make_int(tid);
        }
        if (alias && !alias->str.empty()) st->alias = alias->str;
        if (st->type_id) {
          apply_script_cfg(rpk, st, static_cast<uint32_t>(st->type_id));
          apply_directional_light(rpk, st->type_id, type,*st);
        }
        auto copy_num = [](Object* from, Object* to, const char* key) {
          if (!from || !to) return;
          auto it = from->fields.find(key);
          if (it != from->fields.end()) to->fields[key] = it->second;
        };
        copy_num(type, self, "_mw");
        copy_num(type, self, "_mh");
        copy_num(type, self, "_tid");
        copy_num(type, self, "_texpath");
        if (st->alias == "Rectangle")
          self->fields["_rect"] = Value::make_int(1);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setMatrix",
      "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, roll = 0;
        auto getf = [](Object* o, const char* n, float* out) {
          if (!o) return;
          auto it = o->fields.find(n);
          if (it != o->fields.end() && it->second.tag == ValueTag::Float)
            *out = it->second.v.f;
        };
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          getf(args[0].v.o, "x", &px), getf(args[0].v.o, "y", &py),
              getf(args[0].v.o, "z", &pz);
        if (args.size() >= 2 && args[1].tag == ValueTag::Obj)
          getf(args[1].v.o, "y", &yaw), getf(args[1].v.o, "p", &pitch),
              getf(args[1].v.o, "r", &roll);
        gameref_set_matrix(self, px, py, pz, yaw, pitch, roll);
        // Text.renderinst.setMatrix moves the OSD line (credits scroller).
        Object* text = nullptr;
        if (self) {
          auto tit = self->fields.find("_host_text");
          if (tit != self->fields.end() && tit->second.tag == ValueTag::Obj)
            text = tit->second.v.o;
        }
        if (text) {
          float nx = px, ny = py;
          void* osd = nullptr;
          // createText stored the mapped position; remap script y the same way.
          auto oit = text->fields.find("osd");
          if (oit != text->fields.end() && oit->second.tag == ValueTag::Obj)
            osd = oit->second.v.o;
          if (osd)
            osd_map(osd_view(static_cast<Object*>(osd)), px, py, &nx, &ny);
          slrr_text_set_pos(text, nx, ny);
        }
        const bool rect =
            self && self->fields.count("_rect") &&
            self->fields["_rect"].tag == ValueTag::Int &&
            self->fields["_rect"].v.i != 0;
        if (rect) {
          auto num = [](Object* o, const char* k) -> float {
            if (!o) return 0.f;
            auto it = o->fields.find(k);
            if (it == o->fields.end()) return 0.f;
            if (it->second.tag == ValueTag::Float) return it->second.v.f;
            if (it->second.tag == ValueTag::Int)
              return static_cast<float>(it->second.v.i);
            return 0.f;
          };
          Object* group = nullptr;
          if (GameRefState* st = gameref_get(self))
            group = static_cast<Object*>(st->parent);
          Object* osd = nullptr;
          if (group) {
            auto oit = group->fields.find("osd");
            if (oit != group->fields.end() && oit->second.tag == ValueTag::Obj)
              osd = oit->second.v.o;
          }
          const float mw = num(self, "_mw");
          const float mh = num(self, "_mh");
          uint32_t tid = 0;
          auto tit = self->fields.find("_tid");
          if (tit != self->fields.end() && tit->second.tag == ValueTag::Int)
            tid = static_cast<uint32_t>(tit->second.v.i);
          std::string texpath;
          auto pit = self->fields.find("_texpath");
          if (pit != self->fields.end() && pit->second.tag == ValueTag::Obj &&
              pit->second.v.o)
            texpath = pit->second.v.o->str;
          const uint32_t local = tid & 0xFFFFu;
          float nx = px, ny = py;
          if (osd) osd_world_to_ndc(osd, px, py, &nx, &ny);
          auto already = self->fields.find("_osd_wi");
          if (already != self->fields.end() &&
              already->second.tag == ValueTag::Int) {
            osd_set_widget_pos(already->second.v.i, nx, ny);
            if (tid != 0 || !texpath.empty())
              osd_set_widget_icon(
                  already->second.v.i, tid,
                  !texpath.empty() ? texpath : rpk.texture_path_for_id(tid));
            osd_attach_button_picture(self);
          } else if ((tid != 0 || !texpath.empty()) && local != 0x0018u &&
                     mw > 0.f && mh > 0.f) {
            bool plate = false;
            float hw = 0.f, hh = 0.f;
            osd_mesh_halves(osd, mw, mh, &hw, &hh, &plate);
            const std::string icon =
                !texpath.empty() ? texpath : rpk.texture_path_for_id(tid);
            // The dealer swaps catalog → sell on the same button. The new
            // rectangle is a different size after the viewport activates, so
            // a size check left the first glyph up under the second one.
            if (osd_claim_button_icon(self, nx, ny, tid, icon))
              return Value::make_void();
            OsdWidget w;
            w.picture_source = self;
            w.owner = osd;
            w.x = nx;
            w.y = ny;
            w.half_w = hw;
            w.half_h = hh;
            // Slider knob (RID_SLD_KNOB) is created before the track. Marking
            // it a plate draws it underneath the bar.
            w.background = local != 0x0091u && local != 0x012bu && local != 0x013cu;
            w.no_hit = true;
            w.icon_id = tid;
            w.tex_alpha = local == 0x00A2u;
            w.icon_path = icon;
            if (group) {
              auto git = group->fields.find("_host_group");
              if (git != group->fields.end() && git->second.tag == ValueTag::Int)
                w.group = git->second.v.i;
            }
            const int32_t wi = static_cast<int32_t>(osd_widgets().size());
            self->fields["_osd_wi"] = Value::make_int(wi);
            osd_add_widget(std::move(w));
            osd_attach_button_picture(self);
          }
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setType",
      "(Ljava.util.resource.ResourceRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* type =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        GameRefState* st = gameref_upsert(self, GameRefKind::RenderRef);
        if (!st || !type) return Value::make_void();
        int32_t tid = 0;
        if (GameRefState* ts = gameref_get(type)) tid = ts->type_id;
        if (tid == 0) {
          auto it = type->fields.find("id");
          if (it != type->fields.end() && it->second.tag == ValueTag::Int)
            tid = it->second.v.i;
        }
        st->type_id = tid;
        self->fields["id"] = Value::make_int(tid);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getTypeID", "()I",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        return Value::make_int(st ? st->type_id : 0);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getPos", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Vector3");
        GameRefState* st = gameref_get(self);
        if (v && st) {
          v->fields["x"] = Value::make_float(st->pos[0]);
          v->fields["y"] = Value::make_float(st->pos[1]);
          v->fields["z"] = Value::make_float(st->pos[2]);
        }
        return Value::make_obj(v);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getBoneId", "(Ljava.lang.String;)I",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_int(0);
        const GameRefState* state = gameref_get(self);
        const int32_t type_id = state ? state->type_id : 0;
        const std::string name =
            !args.empty() && args[0].tag == ValueTag::Obj ? obj_str(args[0].v.o) : std::string{};
        return Value::make_int(character_bone_id(self, type_id, rpk, name));
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getDetail", "()F",
      [](Object* self, const std::vector<Value>&) {
        const auto* state = gameref_get(self);
        return Value::make_float(state && !state->empty ? state->detail : 0.f);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "changeResource",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* neu =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        if (!neu) return Value::make_void();
        auto num = [](Object* o, const char* k) -> float {
          auto it = o->fields.find(k);
          if (it == o->fields.end()) return 0.f;
          if (it->second.tag == ValueTag::Float) return it->second.v.f;
          if (it->second.tag == ValueTag::Int)
            return static_cast<float>(it->second.v.i);
          return 0.f;
        };
        const float mw = num(neu, "_mw");
        const float mh = num(neu, "_mh");
        if (mw > 0.f || mh > 0.f) {
          self->fields["_mw"] = Value::make_float(mw);
          self->fields["_mh"] = Value::make_float(mh);
          return Value::make_void();
        }
        int32_t id = 0;
        if (GameRefState* st = gameref_get(neu)) id = st->type_id;
        if (id == 0) {
          auto it = neu->fields.find("id");
          if (it != neu->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
        }
        auto kept = neu->fields.find("_tid");
        if (id == 0 && kept != neu->fields.end() &&
            kept->second.tag == ValueTag::Int)
          id = kept->second.v.i;
        if (id != 0) self->fields["_tid"] = Value::make_int(id);
        const uint32_t from = args[0].tag == ValueTag::Obj ? resource_id_of(args[0].v.o) : 0;
        if (from && id && from != uint32_t(id)) {
          const auto image = [&rpk](uint32_t resource) {
            auto ext = std::filesystem::path(rpk.texture_path_for_id(resource)).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            return ext == ".dds" || ext == ".ptx" || ext == ".png" || ext == ".tga" ||
                   ext == ".bmp" || ext == ".jpg" || ext == ".jpeg";
          };
          if (image(from) && image(id)) {
            auto& replacements = gameref_upsert(self, GameRefKind::RenderRef)->texture_replacements;
            bool found = false;
            for (auto& [original, current] : replacements) {
              if (original != from && current != from) continue;
              current = id;
              found = true;
            }
            if (!found) replacements.emplace_back(from, id);
          }
        }
        auto path = neu->fields.find("_texpath");
        if (path != neu->fields.end()) self->fields["_texpath"] = path->second;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setColor", "(I)V",
      [](Object* self,const std::vector<Value>& args){
        if(auto* state=gameref_get(self);state&&!args.empty()&&args[0].tag==ValueTag::Int)state->color=static_cast<uint32_t>(args[0].v.i)&0xffffff;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setLight", "(III)V",
      [](Object* self, const std::vector<Value>& args) {
        auto as_i = [](const Value& v) -> int32_t {
          if (v.tag == ValueTag::Int) return v.v.i;
          if (v.tag == ValueTag::Float) return static_cast<int32_t>(v.v.f);
          return 0;
        };
        const int32_t diffuse = !args.empty() ? as_i(args[0]) : 0xffffff;
        const int32_t ambient = args.size() > 1 ? as_i(args[1]) : 0x404040;
        const int32_t specular = args.size() > 2 ? as_i(args[2]) : 0xffffff;
        if (self) {
          self->fields["_sun_d"] = Value::make_int(diffuse);
          self->fields["_sun_a"] = Value::make_int(ambient);
          self->fields["_sun_s"] = Value::make_int(specular);
        }
        if(auto* light=gameref_get(self)) {
          scene_sun_apply_packed(light->directional_light,diffuse,ambient,specular);
          gameref_register_light(light->key);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setFlare",
      "(Ljava.util.resource.ResourceRef;IFFII)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() != 6 || args[0].tag != ValueTag::Obj ||
            args[1].tag != ValueTag::Int || args[2].tag != ValueTag::Float ||
            args[3].tag != ValueTag::Float || args[4].tag != ValueTag::Int ||
            args[5].tag != ValueTag::Int || !std::isfinite(args[2].v.f) ||
            !std::isfinite(args[3].v.f)) return Value::make_void();
        auto& flare = gameref_upsert(self, GameRefKind::RenderRef)->directional_light.flare;
        flare = {uint32_t(resource_id_of(args[0].v.o)), uint32_t(args[1].v.i),
                 std::max(0.f, args[2].v.f), std::max(0.f, args[3].v.f),
                 std::clamp(args[4].v.i, 0, 32), std::clamp(args[5].v.i, 0, 32)};
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setMatrix",
      "(ILjava.util.resource.ResourceRef;Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        const int bone = !args.empty() && args[0].tag == ValueTag::Int ? args[0].v.i : 0;
        if (bone < 0) return Value::make_void();
        auto component = [](Object* object, const char* field) {
          if (!object) return 0.f;
          const auto found = object->fields.find(field);
          if (found == object->fields.end()) return 0.f;
          if (found->second.tag == ValueTag::Float && std::isfinite(found->second.v.f))
            return found->second.v.f;
          if (found->second.tag == ValueTag::Int) return static_cast<float>(found->second.v.i);
          return 0.f;
        };
        Object* bone_ref = args.size() > 1 && args[1].tag == ValueTag::Obj ? args[1].v.o : nullptr;
        Object* position = args.size() > 2 && args[2].tag == ValueTag::Obj ? args[2].v.o : nullptr;
        Object* orientation = args.size() > 3 && args[3].tag == ValueTag::Obj ? args[3].v.o : nullptr;
        const float x = component(position, "x");
        const float y = component(position, "y");
        const float z = component(position, "z");
        const float yaw = component(orientation, "y");
        const float pitch = component(orientation, "p");
        const float roll = component(orientation, "r");
        const GameRefState* state = gameref_get(self);
        const int32_t type_id = state ? state->type_id : 0;
        if (!bone_ref || bone_ref == self) {
          character_set_bone_local(self, type_id, rpk, bone, x, y, z, yaw, pitch, roll);
          return Value::make_void();
        }
        character_link_bone(self, bone_ref, bone);
        gameref_set_parent(self, bone_ref);
        gameref_set_matrix(self, x, y, z, yaw, pitch, roll);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "lineCreate",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() != 2 || args[0].tag != ValueTag::Obj ||
            args[1].tag != ValueTag::Obj || !args[0].v.o || !args[1].v.o ||
            args[0].v.o == self) return Value::make_int(0);
        auto* line = gameref_upsert(self, GameRefKind::RenderRef);
        line->binding = nullptr;
        line->type_id = 0;
        line->line_end.reset();
        line->line_length = 0.f;
        line->line_geometry = std::make_shared<ScxGeometry>();
        auto& mesh = *line->line_geometry;
        mesh.info.ok = true;
        mesh.position_scale = 1.f;
        const auto type = resource_id_of(args[1].v.o);
        auto texture = rpk.texture_path_for_id(type);
        if (texture.empty()) {
          const auto material = scene_instances_geometry(type);
          if (material && !material->textures.empty()) texture = material->textures.front();
        }
        if (!texture.empty()) mesh.textures.push_back(std::move(texture));
        gameref_set_parent(self, args[0].v.o);
        gameref_make_live(self);
        return Value::make_int(1);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "lineAdd",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V",
      [](Object* self, const std::vector<Value>& args) {
        auto* line = gameref_get(self);
        if (!line || !line->line_geometry || args.size() != 4 ||
            args[0].tag != ValueTag::Obj || args[1].tag != ValueTag::Obj ||
            args[2].tag != ValueTag::Int || args[3].tag != ValueTag::Float)
          return Value::make_void();
        const RenderLinePoint point{line_vector(args[0].v.o, {}),
                                    line_vector(args[1].v.o, {0.f, 1.f, 0.f}),
                                    args[3].v.f};
        const auto finite = [](const auto& v) {
          return std::all_of(v.begin(), v.end(), [](float f) { return std::isfinite(f); });
        };
        if (!finite(point.pos) || !finite(point.normal) ||
            !std::isfinite(point.width) || point.width <= 0.f)
          return Value::make_void();
        if (line->line_end)
          append_line_segment(*line, *line->line_end, point, uint32_t(args[2].v.i));
        line->line_end = point;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "plotRoute",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IF"
      "Ljava.lang.Vector3;)I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(0);
      });

  jvm.natives().register_method(
      "java.util.resource.GroundRef", "getNearestCross",
      "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;",
      [](Object*, const std::vector<Value>&) {
        // Track.changeCamTV uses its current camera pose when a map has no
        // crossing index. Route data can supply a result when available.
        return Value::make_obj(nullptr);
      });
  jvm.natives().register_method(
      "java.util.resource.GroundRef", "setPedestrianDensityN", "(F)V",
      [](Object* self, const std::vector<Value>& args) {
        float density = 0.f;
        if (!args.empty() && args[0].tag == ValueTag::Float && std::isfinite(args[0].v.f))
          density = args[0].v.f;
        else if (!args.empty() && args[0].tag == ValueTag::Int)
          density = static_cast<float>(args[0].v.i);
        part_set_pedestrian_density(self, density);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GroundRef", "addPedestrianType",
      "(Ljava/util/resource/GameRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* type = !args.empty() && args[0].tag == ValueTag::Obj ? args[0].v.o : nullptr;
        part_add_pedestrian_type(self, type);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GroundRef", "remPedestrianType",
      "(Ljava/util/resource/GameRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* type = !args.empty() && args[0].tag == ValueTag::Obj ? args[0].v.o : nullptr;
        part_remove_pedestrian_type(self, type);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GroundRef", "setFog", "(IFF)V",
      [](Object*, const std::vector<Value>& args) {
        const int32_t col =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        const float near_z =
            (args.size() >= 2 && args[1].tag == ValueTag::Float) ? args[1].v.f
                                                                 : 0.f;
        const float far_z =
            (args.size() >= 3 && args[2].tag == ValueTag::Float) ? args[2].v.f
                                                                 : 0.f;
        gameref_set_fog(col, near_z, far_z);
        return Value::make_void();
      });

  auto finite_arg = [](const std::vector<Value>& args, float* out) {
    if (args.empty()) return false;
    if (args[0].tag == ValueTag::Float && std::isfinite(args[0].v.f)) {
      *out = args[0].v.f;
      return true;
    }
    if (args[0].tag == ValueTag::Int) {
      *out = static_cast<float>(args[0].v.i);
      return true;
    }
    return false;
  };
  jvm.natives().register_method(
      "java.util.resource.Animation", "<init>",
      "(Ljava/util/resource/RenderRef;Ljava/util/resource/ResourceRef;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() < 2 || args[0].tag != ValueTag::Obj ||
            args[1].tag != ValueTag::Obj)
          return Value::make_void();
        character_bind_animation(self, args[0].v.o, resource_id_of(args[1].v.o), rpk);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "finalize", "()V",
      [](Object* self, const std::vector<Value>&) {
        character_release_animation(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "setSpeed", "(F)V",
      [finite_arg](Object* self, const std::vector<Value>& args) {
        float speed = 0.f;
        if (finite_arg(args, &speed)) character_set_speed(self, speed);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "setFade", "(F)V",
      [finite_arg](Object* self, const std::vector<Value>& args) {
        float fade = 0.f;
        if (finite_arg(args, &fade)) character_set_fade(self, fade);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "play", "()V",
      [](Object* self, const std::vector<Value>&) {
        character_play(self, false);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "loopPlay", "()V",
      [](Object* self, const std::vector<Value>&) {
        character_play(self, true);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "pause", "()V",
      [](Object* self, const std::vector<Value>&) {
        character_pause(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "seek", "(F)V",
      [finite_arg](Object* self, const std::vector<Value>& args) {
        float position = 0.f;
        if (finite_arg(args, &position)) character_seek(self, position);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.Animation", "getPos", "()F",
      [](Object* self, const std::vector<Value>&) {
        return Value::make_float(character_position(self));
      });
}

}  // namespace slrr
