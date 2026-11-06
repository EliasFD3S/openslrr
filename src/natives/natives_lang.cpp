#include "slrr/mainloop.hpp"
#include "slrr/natives_boot.hpp"
#include "slrr/ui_time.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <algorithm>
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

float bits_f32(uint32_t u) {
  float f = 0.f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

float field_f(Object* o, const char* key) {
  if (!o) return 0.f;
  auto it = o->fields.find(key);
  if (it == o->fields.end()) return 0.f;
  if (it->second.tag == ValueTag::Float) return it->second.v.f;
  if (it->second.tag == ValueTag::Int) return static_cast<float>(it->second.v.i);
  return 0.f;
}

void store_vec3(Object* o, float x, float y, float z) {
  if (!o) return;
  o->fields["x"] = Value::make_float(x);
  o->fields["y"] = Value::make_float(y);
  o->fields["z"] = Value::make_float(z);
}

void store_ypr(Object* o, float y, float p, float r) {
  if (!o) return;
  o->fields["y"] = Value::make_float(y);
  o->fields["p"] = Value::make_float(p);
  o->fields["r"] = Value::make_float(r);
}

void load_vec3(Object* o, float& x, float& y, float& z) {
  x = field_f(o, "x");
  y = field_f(o, "y");
  z = field_f(o, "z");
}

void load_ypr(Object* o, float& y, float& p, float& r) {
  y = field_f(o, "y");
  p = field_f(o, "p");
  r = field_f(o, "r");
}

// PE Ypr_toMatrix @ 0x0054ECD0: R = Ry(yaw) * Rx(pitch) * Rz(roll).
void ypr_to_mat34(float m[12], float yaw, float pitch, float roll) {
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sp = std::sin(pitch), cp = std::cos(pitch);
  const float sr = std::sin(roll), cr = std::cos(roll);
  const float sr_sp = sr * sp;
  const float cr_sp = cr * sp;
  m[0] = sr_sp * sy + cr * cy;
  m[1] = cr_sp * sy - sr * cy;
  m[2] = cp * sy;
  m[3] = 0.f;
  m[4] = sr * cp;
  m[5] = cr * cp;
  m[6] = -sp;
  m[7] = 0.f;
  m[8] = sr_sp * cy - cr * sy;
  m[9] = cr_sp * cy + sr * sy;
  m[10] = cp * cy;
  m[11] = 0.f;
}

void mat34_set_identity(float m[12]) {
  const float one = bits_f32(0x3F800000u);
  m[0] = one;
  m[1] = 0.f;
  m[2] = 0.f;
  m[3] = 0.f;
  m[4] = 0.f;
  m[5] = one;
  m[6] = 0.f;
  m[7] = 0.f;
  m[8] = 0.f;
  m[9] = 0.f;
  m[10] = one;
  m[11] = 0.f;
}

void mat34_from_axis_angle(float m[12], float ax, float ay, float az,
                           float angle) {
  const float kEps = bits_f32(0x358637BDu);
  const float len2 = ax * ax + ay * ay + az * az;
  if (len2 < kEps) return;
  if (std::fabs(len2 - 1.f) > kEps) {
    const float inv = 1.f / std::sqrt(len2);
    ax *= inv;
    ay *= inv;
    az *= inv;
  }
  const float c = std::cos(angle), s = std::sin(angle);
  const float t = 1.f - c;
  const float xx = ax * ax, yy = ay * ay, zz = az * az;
  const float xy = ax * ay, xz = ax * az, yz = ay * az;
  m[0] = c + xx * t;
  m[1] = xy * t - az * s;
  m[2] = xz * t + ay * s;
  m[3] = 0.f;
  m[4] = xy * t + az * s;
  m[5] = c + yy * t;
  m[6] = yz * t - ax * s;
  m[7] = 0.f;
  m[8] = xz * t - ay * s;
  m[9] = yz * t + ax * s;
  m[10] = c + zz * t;
  m[11] = 0.f;
}

void mat34_mul_left(float th[12], const float a[12]) {
  float o[12];
  o[0] = a[0] * th[0] + a[1] * th[4] + a[2] * th[8];
  o[1] = a[0] * th[1] + a[1] * th[5] + a[2] * th[9];
  o[2] = a[0] * th[2] + a[1] * th[6] + a[2] * th[10];
  o[3] = 0.f;
  o[4] = a[4] * th[0] + a[5] * th[4] + a[6] * th[8];
  o[5] = a[4] * th[1] + a[5] * th[5] + a[6] * th[9];
  o[6] = a[4] * th[2] + a[5] * th[6] + a[6] * th[10];
  o[7] = 0.f;
  o[8] = a[8] * th[0] + a[9] * th[4] + a[10] * th[8];
  o[9] = a[8] * th[1] + a[9] * th[5] + a[10] * th[9];
  o[10] = a[8] * th[2] + a[9] * th[6] + a[10] * th[10];
  o[11] = 0.f;
  std::memcpy(th, o, sizeof(o));
}

void mat34_mul_vec(const float m[12], float x, float y, float z, float& ox,
                   float& oy, float& oz) {
  ox = m[0] * x + m[1] * y + m[2] * z;
  oy = m[4] * x + m[5] * y + m[6] * z;
  oz = m[8] * x + m[9] * y + m[10] * z;
}

void ypr_from_mat34(const float m[12], float& yaw, float& pitch, float& roll) {
  if (m[10] == 0.f && m[2] == 0.f) {
    pitch = bits_f32(0x40490FDBu);
    roll = 0.f;
    yaw = std::atan2(m[0], -m[1]);
    return;
  }
  yaw = std::atan2(m[2], m[10]);
  roll = std::atan2(m[4], m[5]);
  const float cr = std::cos(roll);
  const float v6 = cr * m[6];
  if (!(cr > 0.f))
    pitch = std::atan2(v6, -m[5]);
  else
    pitch = std::atan2(-v6, m[5]);
}

void ypr_from_direction(float x, float y, float z, float& yaw, float& pitch,
                        float& roll) {
  roll = 0.f;
  if (x == 0.f && z == 0.f) {
    yaw = 0.f;
    pitch = (y <= 0.f) ? bits_f32(0xBFC90FDBu) : bits_f32(0x3FC90FDBu);
  } else {
    yaw = std::atan2(-x, -z);
    pitch = std::atan2(y, std::sqrt(x * x + z * z));
  }
}

float arg_f(const std::vector<Value>& args, size_t i) {
  if (i >= args.size()) return 0.f;
  if (args[i].tag == ValueTag::Float) return args[i].v.f;
  if (args[i].tag == ValueTag::Int) return static_cast<float>(args[i].v.i);
  return 0.f;
}

Object* arg_o(const std::vector<Value>& args, size_t i) {
  if (i >= args.size() || args[i].tag != ValueTag::Obj) return nullptr;
  return args[i].v.o;
}

}  // namespace

void register_gc_natives(Jvm& jvm) {
  jvm.natives().register_method("java.lang.System", "gc", "()I",
      [&jvm](Object*, const std::vector<Value>&) { return Value::make_int(jvm.request_gc()); });
  // The original native returns zero; finalizers are part of the GC cycle.
  jvm.natives().register_method("java.lang.System", "runFinalization", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(0); });
  jvm.natives().register_method("java.lang.Object", "disableGC", "()V",
      [](Object* o, const std::vector<Value>&) {
        if (o) o->gc_disabled = true;
        return Value::make_void();
      });
  jvm.natives().register_method("java.lang.Object", "enableGC", "()V",
      [](Object* o, const std::vector<Value>&) {
        if (o) o->gc_disabled = false;
        return Value::make_void();
      });
  jvm.natives().register_method("java.lang.Object", "hashCode", "()I",
      [](Object* o, const std::vector<Value>&) {
        return Value::make_int(o ? static_cast<int32_t>(o->identity_hash) : 0);
      });
  jvm.natives().register_method("java.lang.Object", "toString", "()Ljava.lang.String;",
      [&jvm](Object* o, const std::vector<Value>&) {
        char identity[24];
        std::snprintf(identity, sizeof(identity), "@%x", o ? o->identity_hash : 0);
        return Value::make_obj(jvm.new_string(o ? o->class_name + identity : "null"));
      });
  jvm.natives().register_method("java.lang.String", "finalize", "()V",
      [](Object* o, const std::vector<Value>&) {
        if (o) o->str.clear();
        return Value::make_void();
      });
}

void register_lang_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  register_gc_natives(jvm);
  jvm.natives().register_method("java.lang.Vector3", "rotate", "(Ljava.lang.Ypr;)Ljava.lang.Vector3;",
      [&jvm](Object* self,const std::vector<Value>& args){
        jvm.natives().invoke("java.lang.Vector3","rotate","(Ljava.lang.Ypr;)V",self,args);
        return Value::make_obj(self);
      });
  jvm.natives().register_method("java.lang.String", "timeToString", "(FI)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        const float time = args.empty() ? 0.f : args[0].tag == ValueTag::Float ? args[0].v.f : static_cast<float>(args[0].v.i);
        const int flags = args.size() > 1 ? args[1].v.i : 0;
        return Value::make_obj(jvm.new_string(format_game_time(time, flags)));
      });
  jvm.natives().register_method("java.lang.String", "timeToString", "(F)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        const float time = args.empty() ? 0.f : args[0].tag == ValueTag::Float ? args[0].v.f : static_cast<float>(args[0].v.i);
        return Value::make_obj(jvm.new_string(format_game_time(time, 0)));
      });
  // GameLogic.save / load / loadDefaults / changeActiveSection are the Java
  // TREE. File.write persists the bytes; the frame adopts actualState after.

  // MainMenuDialog.osdCommand — Soft Java TREE (no host switch). PE leaves:
  // Dialog.display, changeActiveSection, LoadingScreen, File.*, etc.
  // OptionsDialog.changeMode Soft → osd.hideGroup/showGroup host.

  // Soft PlayerSetupDialog field init: startId + Math.random()*(60-1)
  // OptionsDialog labels: Float.toString(range, "%.0f m"), gamma "%.2f".
  jvm.natives().register_method(
      "java.lang.Float", "toString",
      "(FLjava.lang.String;)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float v = 0.f;
        bool have_v = false;
        std::string fmt;
        for (const Value& a : args) {
          if (a.tag == ValueTag::Obj && a.v.o &&
              a.v.o->str.find('%') != std::string::npos) {
            fmt = a.v.o->str;
          } else if (!have_v && a.tag == ValueTag::Float) {
            v = a.v.f;
            have_v = true;
          } else if (!have_v && a.tag == ValueTag::Int) {
            v = static_cast<float>(a.v.i);
            have_v = true;
          }
        }
        // Gamma is printValue("%.2f"). A missing format must not dump 6 digits.
        if (fmt.empty()) fmt = "%.2f";
        std::string out;
        out.reserve(fmt.size() + 16);
        for (size_t i = 0; i < fmt.size(); ++i) {
          if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
          }
          if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
          }
          ++i;
          while (i < fmt.size() &&
                 (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
                  fmt[i] == '#' || fmt[i] == '0'))
            ++i;
          while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') ++i;
          int prec = 2;
          if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            prec = 0;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
              prec = prec * 10 + (fmt[i++] - '0');
          }
          if (i < fmt.size() && (fmt[i] == 'f' || fmt[i] == 'F' ||
                                 fmt[i] == 'g' || fmt[i] == 'G')) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.*f", prec,
                          static_cast<double>(v));
            out += buf;
          }
        }
        return Value::make_obj(jvm.new_string(out));
      });
  jvm.natives().register_method(
      "java.lang.Math", "sqrt", "(F)F",
      [](Object*, const std::vector<Value>& args) {
        float a = 0.f;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Float) a = args[0].v.f;
          else if (args[0].tag == ValueTag::Int)
            a = static_cast<float>(args[0].v.i);
        }
        if (a < 0.f) a = 0.f;
        return Value::make_float(std::sqrt(a));
      });
  jvm.natives().register_method(
      "java.lang.Math", "random", "()F",
      [](Object*, const std::vector<Value>&) {
        return Value::make_float(
            static_cast<float>(std::rand()) /
            static_cast<float>(RAND_MAX + 1u));
      });
  jvm.natives().register_method(
      "java.lang.Math", "randomize", "()V",
      [](Object*, const std::vector<Value>&) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        return Value::make_void();
      });
  // HotkeyWatcher.run and the game-time refresher loop. start() must return
  // so Init can continue; the watcher is not entered.
  jvm.natives().register_method(
      "java.lang.Thread", "init", "(Ljava.lang.String;)V", native_noop);
  jvm.natives().register_method(
      "java.lang.Thread", "start", "()V",
      [&jvm](Object* self, const std::vector<Value>&) {
        const char* tgt = "-";
        if (self) {
          auto it = self->fields.find("target");
          if (it != self->fields.end() && it->second.tag == ValueTag::Obj &&
              it->second.v.o)
            tgt = it->second.v.o->class_name.c_str();
        }
        std::printf("[native] Thread.start %s\n", tgt);
        // GameLogic.run stays unscheduled. Mechanic.run only refreshes
        // the info line, and that tick is applied here.
        mainloop_note_thread(jvm, self, true);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Thread", "stop", "()V",
      [&jvm](Object* self, const std::vector<Value>&) {
        mainloop_note_thread(jvm, self, false);
        return Value::make_void();
      });
  jvm.natives().register_method("java.lang.Thread", "sleep", "(F)V", native_noop);
  jvm.natives().register_method(
      "java.lang.Thread", "isAlive", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(0); });

  // Dialog.display is the script: show, Object.wait, hide, return result.
  // notify wakes only the object that is inside wait (nested dialogs).
  jvm.natives().register_method(
      "java.lang.Object", "wait", "()V",
      [](Object* self, const std::vector<Value>&) {
        object_wait(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Object", "notify", "()V",
      [](Object* self, const std::vector<Value>&) {
        object_notify(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Object", "notifyAll", "()V",
      [](Object* self, const std::vector<Value>&) {
        object_notify(self);
        return Value::make_void();
      });

  // PE Vector3 / Ypr (Math3D.cpp). Menu and garage poses go through these.
  jvm.natives().register_method(
      "java.lang.Vector3", "length", "()F",
      [](Object* self, const std::vector<Value>&) {
        float x, y, z;
        load_vec3(self, x, y, z);
        return Value::make_float(std::sqrt(x * x + y * y + z * z));
      });
  jvm.natives().register_method(
      "java.lang.Vector3", "distance", "(Ljava.lang.Vector3;)F",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_float(0.f);
        float vx, vy, vz, tx, ty, tz;
        load_vec3(arg_o(args, 0), vx, vy, vz);
        load_vec3(self, tx, ty, tz);
        const float dx = tx - vx, dy = ty - vy, dz = tz - vz;
        return Value::make_float(std::sqrt(dx * dx + dy * dy + dz * dz));
      });
  jvm.natives().register_method(
      "java.lang.Vector3", "normalize", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_void();
        float x, y, z;
        load_vec3(self, x, y, z);
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len != 0.f) {
          const float inv = 1.f / len;
          x *= inv;
          y *= inv;
          z *= inv;
        }
        store_vec3(self, x, y, z);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Vector3", "rotate", "(Ljava.lang.Ypr;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        float x, y, z;
        load_vec3(self, x, y, z);
        float m[12];
        Object* ypr = arg_o(args, 0);
        if (ypr) {
          float yaw, pitch, roll;
          load_ypr(ypr, yaw, pitch, roll);
          ypr_to_mat34(m, yaw, pitch, roll);
        } else {
          mat34_set_identity(m);
        }
        float ox, oy, oz;
        mat34_mul_vec(m, x, y, z, ox, oy, oz);
        store_vec3(self, ox, oy, oz);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Vector3", "rotate", "(Ljava.lang.Vector3;F)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        float x, y, z, ax, ay, az;
        load_vec3(self, x, y, z);
        load_vec3(arg_o(args, 0), ax, ay, az);
        const float angle = arg_f(args, 1);
        if (ax * ax + ay * ay + az * az < bits_f32(0x358637BDu))
          return Value::make_void();
        float m[12] = {};
        mat34_from_axis_angle(m, ax, ay, az, angle);
        float ox, oy, oz;
        mat34_mul_vec(m, x, y, z, ox, oy, oz);
        store_vec3(self, ox, oy, oz);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Vector3", "<init>", "(Ljava.lang.Ypr;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        float m[12];
        Object* ypr = arg_o(args, 0);
        if (ypr) {
          float yaw, pitch, roll;
          load_ypr(ypr, yaw, pitch, roll);
          ypr_to_mat34(m, yaw, pitch, roll);
        } else {
          mat34_set_identity(m);
        }
        const float kNeg1 = bits_f32(0xBF800000u);
        store_vec3(self, m[2] * kNeg1, m[6] * kNeg1, m[10] * kNeg1);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Ypr", "mul", "(Ljava.lang.Ypr;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        float y, p, r;
        load_ypr(self, y, p, r);
        float m_this[12] = {};
        float m_v[12] = {};
        ypr_to_mat34(m_this, y, p, r);
        Object* other = arg_o(args, 0);
        if (other) {
          float oy, op, ov;
          load_ypr(other, oy, op, ov);
          ypr_to_mat34(m_v, oy, op, ov);
        } else {
          mat34_set_identity(m_v);
        }
        mat34_mul_left(m_this, m_v);
        ypr_from_mat34(m_this, y, p, r);
        store_ypr(self, y, p, r);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.Ypr", "<init>", "(Ljava.lang.Vector3;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        float x, y, z, yaw, pitch, roll;
        load_vec3(arg_o(args, 0), x, y, z);
        ypr_from_direction(x, y, z, yaw, pitch, roll);
        store_ypr(self, yaw, pitch, roll);
        return Value::make_void();
      });

  jvm.natives().register_method(
      "java.lang.Integer", "toString", "(I)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        const int32_t i =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d", i);
        return Value::make_obj(jvm.new_string(buf));
      });
  jvm.natives().register_method(
      "java.lang.Integer", "toHexString", "(I)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        const int32_t i =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(i));
        return Value::make_obj(jvm.new_string(buf));
      });
  jvm.natives().register_method(
      "java.lang.String", "floatValue", "()F",
      [](Object* self, const std::vector<Value>&) {
        float v = 0.f;
        if (self && !self->str.empty()) std::sscanf(self->str.c_str(), "%f", &v);
        return Value::make_float(v);
      });
  jvm.natives().register_method(
      "java.lang.String", "append", "(I)Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>& args) {
        const int32_t ascii =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        std::string buf = self ? self->str : std::string{};
        if (buf.size() > 255) buf.resize(255);
        if (ascii == 8) {
          if (!buf.empty()) buf.pop_back();
        } else if (ascii >= 0x20 && buf.size() + 1u < 256u) {
          buf.push_back(static_cast<char>(ascii));
        }
        return Value::make_obj(jvm.new_string(buf));
      });
  jvm.natives().register_method(
      "java.lang.Math", "setrandseed", "(I)V",
      [](Object*, const std::vector<Value>& args) {
        const int32_t seed =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        std::srand(static_cast<unsigned>(seed));
        return Value::make_void();
      });
}

}  // namespace slrr
