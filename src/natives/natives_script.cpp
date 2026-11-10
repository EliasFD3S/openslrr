#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/scene_instances.hpp"
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

#ifdef _WIN32
#include <windows.h>
#endif

namespace slrr {
namespace {
std::vector<VideoMode> display_modes() {
  std::vector<VideoMode> modes;
#ifdef _WIN32
  for (DWORD i = 0;; ++i) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, i, &mode)) break;
    if (mode.dmPelsWidth < 640 || mode.dmPelsHeight < 480 || mode.dmBitsPerPel != 32)
      continue;
    modes.push_back({int(mode.dmPelsWidth), int(mode.dmPelsHeight), 32, false});
  }
#endif
  modes.push_back({800, 600, 32, false});
  const auto key = [](const VideoMode& mode) {
    return std::pair(mode.width, mode.height);
  };
  std::sort(modes.begin(), modes.end(), [&](const auto& a, const auto& b) {
    return key(a) < key(b);
  });
  modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
  return modes;
}
}


void register_script_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  jvm.natives().register_method("java.game.OptionsDialog", "getRendererOption", "(I)I",
      [](Object*, const std::vector<Value>& args) {
        const auto& graphics = host_enhanced_graphics();
        switch (args[0].v.i) {
          case 0: return Value::make_int(graphics.msaa);
          case 1: return Value::make_int(graphics.dynamic_reflections);
          case 2: return Value::make_int(graphics.volumetric_clouds);
          case 3: return Value::make_int(graphics.dynamic_lighting);
          case 4: return Value::make_int(graphics.local_lights);
          default: return Value::make_int(0);
        }
      });
  jvm.natives().register_method("java.game.OptionsDialog", "setRendererOption", "(II)V",
      [](Object*, const std::vector<Value>& args) {
        auto& graphics = host_enhanced_graphics();
        const int value = args[1].v.i;
        switch (args[0].v.i) {
          case 0: graphics.msaa = std::clamp(value, 0, 3); break;
          case 1: graphics.dynamic_reflections = value != 0; break;
          case 2: graphics.volumetric_clouds = value != 0; break;
          case 3: graphics.dynamic_lighting = value != 0; break;
          case 4: graphics.local_lights = value != 0; break;
        }
        return Value::make_void();
      });
  jvm.natives().register_method("java.lang.System", "getConfigOptions", "()V",
    [&jvm](Object*, const std::vector<Value>&) {
      host_game_options_apply(jvm);
      return Value::make_void();
    });
  jvm.natives().register_method("java.lang.System", "setMeasure", "(F)V",
    [&jvm](Object*, const std::vector<Value>& args) {
      float meters = args.empty() ? 1000.f : args[0].tag==ValueTag::Float ? args[0].v.f :
                     args[0].tag==ValueTag::Int ? static_cast<float>(args[0].v.i) : 1000.f;
      if(std::isfinite(meters) && meters>0.f)
        jvm.set_static("java.lang.System","_hostDistanceUnitMeters",Value::make_float(meters));
      return Value::make_void();
    });
  jvm.natives().register_method("java.game.GameLogic","kismajomCheck","([Ljava.lang.String;)I",
    [](Object*,const std::vector<Value>& args){
      int match=-1;
      if(!args.empty()&&args[0].tag==ValueTag::Obj&&args[0].v.o){auto& codes=args[0].v.o->slots;
        for(size_t i=0;i<codes.size();i++){if(codes[i].tag!=ValueTag::Obj||!codes[i].v.o)continue;
          std::string decoded=codes[i].v.o->str;for(char& c:decoded)--c;
          if(!decoded.empty()&&host_input_history().ends_with(decoded)){match=static_cast<int>(i);break;}
        }
      }
      host_input_clear_history();return Value::make_int(match);
    });
  jvm.natives().register_method("java.util.resource.SfxRef", "nplay", "(Ljava.lang.Vector3;FFFII)I",
    [&rpk](Object* self, const std::vector<Value>& args) {
      const auto* sound = gameref_get(self);
      const auto path = sound && !sound->sound_file.empty()
          ? std::filesystem::path(sound->sound_file)
          : under_game(rpk.game_root(), rpk.payload_prop(resource_id_of(self), "sourcefile"));
      auto num=[&](size_t i,float fallback){return i>=args.size()?fallback:args[i].tag==ValueTag::Float?args[i].v.f:args[i].tag==ValueTag::Int?static_cast<float>(args[i].v.i):fallback;};
      if(path.empty())return Value::make_int(0);
      return Value::make_int(host_sfx_play_file(path.string(),num(2,1),num(3,1),static_cast<int>(num(4,0))&1,static_cast<int>(num(5,0))));
    });
  jvm.natives().register_method("java.util.resource.SfxRef", "stop", "(I)V",
    [](Object*,const std::vector<Value>& args){if(!args.empty())host_sfx_stop(args[0].v.i);return Value::make_void();});
  // Soft OptionsDialog.show: "800 600 32".token(i).intValue()
  jvm.natives().register_method(
      "java.lang.String", "token", "(ILjava.lang.String;)Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_obj(jvm.new_string(""));
        const int32_t n =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        std::string delims = " \t\n";
        if (args.size() > 1 && args[1].tag == ValueTag::Obj && args[1].v.o)
          delims = args[1].v.o->str;
        const std::string& s = self->str;
        size_t i = 0;
        int32_t tok = 0;
        while (i < s.size()) {
          while (i < s.size() && delims.find(s[i]) != std::string::npos) ++i;
          if (i >= s.size()) break;
          const size_t start = i;
          while (i < s.size() && delims.find(s[i]) == std::string::npos) ++i;
          if (tok == n)
            return Value::make_obj(jvm.new_string(s.substr(start, i - start)));
          ++tok;
        }
        return Value::make_obj(jvm.new_string(""));
      });
  jvm.natives().register_method(
      "java.lang.String", "token", "(I)Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>& args) {
        return jvm.invoke(
            "java.lang.String", "token",
            "(ILjava.lang.String;)Ljava.lang.String;",
            {Value::make_obj(self),
             args.empty() ? Value::make_int(0) : args[0],
             Value::make_obj(jvm.new_string(" \t\n"))},
            false);
      });
  jvm.natives().register_method(
      "java.lang.String", "intValue", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self || self->str.empty()) return Value::make_int(0);
        try {
          return Value::make_int(std::stoi(self->str));
        } catch (...) {
          return Value::make_int(0);
        }
      });
  // StringRequesterDialog rejects OK when input.length()==0 (SIF_NOEMPTY).
  jvm.natives().register_method(
      "java.lang.String", "length", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_int(0);
        return Value::make_int(static_cast<int32_t>(self->str.size()));
      });

  // Dialog.show splits "YES;NO" / "OK;Cancel". Java countTokens leaves its
  // counter uninitialized and String.token returns "" (truthy) when exhausted,
  // so both stay on the host. nextToken returns null when finished.
  auto tokenizer_at = [&jvm](Object* self, int32_t index,
                             Object* delim) -> Value {
    if (!self) return Value::make_obj(nullptr);
    Object* str = nullptr;
    auto sit = self->fields.find("string");
    if (sit != self->fields.end() && sit->second.tag == ValueTag::Obj)
      str = sit->second.v.o;
    if (!str) return Value::make_obj(nullptr);
    if (!delim) {
      auto dit = self->fields.find("delimiters");
      if (dit != self->fields.end() && dit->second.tag == ValueTag::Obj)
        delim = dit->second.v.o;
    }
    Value tok = jvm.invoke(
        "java.lang.String", "token",
        "(ILjava.lang.String;)Ljava.lang.String;",
        {Value::make_obj(str), Value::make_int(index),
         Value::make_obj(delim)},
        false);
    if (tok.tag != ValueTag::Obj || !tok.v.o || tok.v.o->str.empty())
      return Value::make_obj(nullptr);
    return tok;
  };
  auto tokenizer_progress = [](Object* self) -> int32_t {
    if (!self) return 0;
    auto pit = self->fields.find("progress");
    if (pit != self->fields.end() && pit->second.tag == ValueTag::Int)
      return pit->second.v.i;
    return 0;
  };
  jvm.natives().register_method(
      "java.util.StringTokenizer", "nextToken", "()Ljava.lang.String;",
      [tokenizer_at, tokenizer_progress](Object* self,
                                         const std::vector<Value>&) {
        const int32_t p = tokenizer_progress(self);
        Value tok = tokenizer_at(self, p, nullptr);
        if (tok.tag == ValueTag::Obj && tok.v.o && self)
          self->fields["progress"] = Value::make_int(p + 1);
        return tok;
      });
  jvm.natives().register_method(
      "java.util.StringTokenizer", "nextToken",
      "(Ljava.lang.String;)Ljava.lang.String;",
      [tokenizer_at, tokenizer_progress](Object* self,
                                         const std::vector<Value>& args) {
        Object* delim =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        if (self && delim) self->fields["delimiters"] = Value::make_obj(delim);
        const int32_t p = tokenizer_progress(self);
        Value tok = tokenizer_at(self, p, delim);
        if (tok.tag == ValueTag::Obj && tok.v.o && self)
          self->fields["progress"] = Value::make_int(p + 1);
        return tok;
      });
  jvm.natives().register_method(
      "java.util.StringTokenizer", "countTokens", "()I",
      [tokenizer_at, tokenizer_progress](Object* self,
                                         const std::vector<Value>&) {
        int32_t p = tokenizer_progress(self);
        int32_t n = 0;
        while (n < 64) {
          Value tok = tokenizer_at(self, p, nullptr);
          if (tok.tag != ValueTag::Obj || !tok.v.o) break;
          ++n;
          ++p;
        }
        return Value::make_int(n);
      });

  // Stock OptionsDialog enumerates modes before building menus.
  const auto modes = display_modes();
  jvm.natives().register_method(
      "java.render.GfxEngine", "numDisplayModes", "()I",
      [modes](Object*, const std::vector<Value>&) {
        return Value::make_int(static_cast<int>(modes.size()));
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "currDisplayMode", "()I",
      [modes](Object*, const std::vector<Value>&) {
        const auto current = host_game_options().video.mode;
        const auto it = std::find_if(modes.begin(), modes.end(), [&](const auto& mode) {
          return mode.width == current.width && mode.height == current.height;
        });
        return Value::make_int(it == modes.end() ? 0 : static_cast<int>(it - modes.begin()));
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "displayModeName", "(I)Ljava.lang.String;",
      [&jvm, modes](Object*, const std::vector<Value>& args) {
        const int i = args.empty() ? -1 : args[0].v.i;
        if (i < 0 || static_cast<size_t>(i) >= modes.size()) return Value::make_obj(nullptr);
        const auto& mode = modes[i];
        return Value::make_obj(jvm.new_string(std::to_string(mode.width) + " " +
            std::to_string(mode.height) + " " + std::to_string(mode.depth)));
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "changeVideoMode", "(III)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        if (args.size() == 3) {
          const auto windowed = jvm.get_static("java.util.Config", "video_windowed");
          host_set_video_mode(jvm, {args[0].v.i, args[1].v.i, args[2].v.i,
                                   windowed.tag != ValueTag::Int || windowed.v.i != 0});
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "openVideo", "(Ljava.lang.String;II)I",
      [](Object*, const std::vector<Value>& args) {
        const char* path = nullptr;
        std::string tmp;
        if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o) {
          tmp = obj_str(args[0].v.o);
          path = tmp.c_str();
        }
        const int32_t non_excl =
            (args.size() >= 2 && args[1].tag == ValueTag::Int) ? args[1].v.i
                                                               : 0;
        const int32_t loop =
            (args.size() >= 3 && args[2].tag == ValueTag::Int) ? args[2].v.i
                                                               : 0;
        return Value::make_int(video_fmv_open(path, non_excl, loop));
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "closeVideo", "()V",
      [](Object*, const std::vector<Value>&) {
        video_fmv_close();
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "isPlayingVideo", "()I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(video_fmv_is_playing());
      });
  // LoadingDialog.show calls this after the intro videos. Unbound, it aborts
  // Init before GameLogic and the main loop exits with no section. The host
  // already presents a frame each pump.
  jvm.natives().register_method(
      "java.render.GfxEngine", "forceRendering", "()V",
      [](Object*, const std::vector<Value>&) {
        host_force_present(); return Value::make_void();
      });
  jvm.natives().register_method("java.render.GfxEngine", "flush", "()V",
      [](Object*, const std::vector<Value>&) {
        host_force_present(); return Value::make_void();
      });
  jvm.natives().register_method("java.render.GfxEngine", "printScreen", "(Ljava.lang.String;)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o) {
          const auto path = under_game(jvm.game_root(), args[0].v.o->str);
          std::error_code ec;
          std::filesystem::create_directories(path.parent_path(), ec);
          if (!ec && host_print_screen(path.string())) host_force_present();
          else std::fprintf(stderr, "[gfx] screenshot unavailable: %s\n", path.string().c_str());
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "setGlobalEnvmap", "(Ljava.lang.Object;)V",
      [](Object*,const std::vector<Value>& args){
        auto* ref=!args.empty()&&args[0].tag==ValueTag::Obj?args[0].v.o:nullptr;
        scene_instances_set_envmap(resource_id_of(ref));return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.GfxEngine", "setGlobalEnvmap",
      "(Ljava.util.resource.ResourceRef;)V", [](Object*,const std::vector<Value>& args){
        auto* ref=!args.empty()&&args[0].tag==ValueTag::Obj?args[0].v.o:nullptr;
        scene_instances_set_envmap(resource_id_of(ref));return Value::make_void();
      });
  jvm.natives().register_method(
      "java.sound.Sound", "changeMusicSet", "(I)V",
      [](Object*, const std::vector<Value>& a) {
        host_music_change_set(a.empty() || a[0].tag != ValueTag::Int ? -1 : a[0].v.i);
        return Value::make_void();
      });
  for (int step : {-1, 1})
    jvm.natives().register_method("java.sound.Sound", step > 0 ? "nextTrack" : "prevTrack", "()V",
        [step](Object*, const std::vector<Value>&) { host_music_next(step); return Value::make_void(); });
  jvm.natives().register_method("java.sound.Sound", "getMode", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(host_music_mode()); });
  jvm.natives().register_method("java.sound.Sound", "setMode", "(I)I",
      [](Object*, const std::vector<Value>& a) {
        return Value::make_int(host_music_set_mode(a.empty() || a[0].tag != ValueTag::Int ? 0 : a[0].v.i));
      });
  for (const char* capability : {"has3DHardware", "hasMixHardware"})
    jvm.natives().register_method("java.sound.Sound", capability, "()I",
        [](Object*, const std::vector<Value>&) {
          // Our software XAudio2/MCI backend offers no DirectSound hardware buffers.
          return Value::make_int(0);
        });
  // OptionsDialog.show reads the three channel volumes while building the
  // sound page. Missing the native aborted the rest of show().
  {
    static float volumes[3] = {1.f, 1.f, 1.f};
    auto channel = [](const Value& v) {
      int32_t c = 0;
      if (v.tag == ValueTag::Int) c = v.v.i;
      else if (v.tag == ValueTag::Float) c = static_cast<int32_t>(v.v.f);
      if (c < 0 || c > 2) c = 0;
      return c;
    };
    jvm.natives().register_method(
        "java.sound.Sound", "getVolume", "(I)F",
        [channel](Object*, const std::vector<Value>& args) {
          const int c = args.empty() ? 0 : channel(args[0]);
          return Value::make_float(volumes[c]);
        });
    jvm.natives().register_method(
        "java.sound.Sound", "setVolume", "(IF)V",
        [channel](Object*, const std::vector<Value>& args) {
          const int c = args.empty() ? 0 : channel(args[0]);
          float v = 1.f;
          if (args.size() > 1) {
            if (args[1].tag == ValueTag::Float) v = args[1].v.f;
            else if (args[1].tag == ValueTag::Int)
              v = static_cast<float>(args[1].v.i);
          }
          if (v < 0.f) v = 0.f;
          if (v > 1.f) v = 1.f;
          volumes[c] = v;
          if(c==0)host_sfx_set_volume(v);
          if(c==1)host_music_set_volume(v);
          host_game_options_dirty();
          return Value::make_void();
        });
  }
  // Stock lab build returns 0, so Sound.init skips the debug viewport.
  jvm.natives().register_method(
      "java.sound.Sound", "enableDebugDump",
      "(Ljava.util.resource.ResourceRef;)I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(0); });
  auto object = [](const std::vector<Value>& args, size_t i) {
    return i < args.size() && args[i].tag == ValueTag::Obj ? args[i].v.o : nullptr;
  };
  auto integer = [](const std::vector<Value>& args, size_t i) {
    return i < args.size() && args[i].tag == ValueTag::Int ? args[i].v.i : -1;
  };
  jvm.natives().register_method(
      "java.io.Input", "createHotkey",
      "(IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V",
      [&jvm, object, integer](Object*, const std::vector<Value>& args) {
        host_input_create_hotkey(jvm, object(args, 2), object(args, 3),
            object(args, 4), integer(args, 0), integer(args, 1), integer(args, 5));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "deleteHotkey", "(Ljava.io.Hotkey;)V",
      [object](Object*, const std::vector<Value>& args) {
        host_input_delete_hotkey(object(args, 0));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "checkHotkeys",
      "(Ljava.util.resource.GameRef;Ljava.render.Osd;)V",
      [&jvm, object](Object*, const std::vector<Value>& args) {
        host_input_check_hotkeys(jvm, object(args, 0), object(args, 1));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "flushHotkeys", "()V",
      [&jvm](Object*, const std::vector<Value>&) {
        host_input_flush_hotkeys(host_input_player_controller(jvm));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "getDeviceName", "(I)Ljava.lang.String;",
      [&jvm, integer](Object*, const std::vector<Value>& args) {
        const char* name = host_input_device_name(integer(args, 0));
        return Value::make_obj(name ? jvm.new_string(name) : nullptr);
      });
  jvm.natives().register_method(
      "java.io.Input", "mapAxis",
      "(Ljava.util.resource.GameRef;IIIFFFF)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        if (args.size() == 8 && args[0].tag == ValueTag::Obj && args[0].v.o)
          jvm.natives().invoke("java.io.Controller", "user_Add", "(IIIFFFF)I",
                               args[0].v.o, {args.begin() + 1, args.end()});
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "setAxisSmooth",
      "(Ljava.util.resource.GameRef;IFFFF)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        if (args.size() == 6 && args[0].tag == ValueTag::Obj && args[0].v.o) {
          std::vector<Value> settings(args.begin() + 1, args.end());
          settings.push_back(Value::make_float(1.f));
          jvm.natives().invoke("java.io.Controller", "user_SetAxisSmooth", "(IFFFFF)I",
                               args[0].v.o, settings);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.Input", "activeAxis", "(I)I",
      [integer](Object*, const std::vector<Value>& args) {
        return Value::make_int(host_input_active_axis(integer(args, 0)));
      });
  jvm.natives().register_method(
      "java.io.Input", "axisName", "(II)Ljava.lang.String;",
      [&jvm, integer](Object*, const std::vector<Value>& args) {
        return Value::make_obj(jvm.new_string(
            host_input_axis_name(integer(args, 0), integer(args, 1))));
      });
  jvm.natives().register_method(
      "java.io.Input", "lastKey", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_last_key()); });
  jvm.natives().register_method(
      "java.io.Input", "getAxis", "(II)F",
      [integer](Object*, const std::vector<Value>& args) {
        const int device = integer(args, 0), axis = integer(args, 1);
        if (device == 1 && axis < 0) {
          auto& input = host_input();
          input.cursor_x = input.cursor_y = 0.f;
          input.look_yaw = input.look_pitch = input.look_zoom = 0.f;
          input.mouse_dx = input.mouse_dy = 0.f;
        }
        return Value::make_float(host_input_physical_axis(device, axis));
      });
  // Synthetic input queue: boot --auto-cmd pushes AXIS_CANCEL / MENU pulses.
  jvm.natives().register_method(
      "java.io.Input", "queueAxis", "(IF)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        Object* q = nullptr;
        Value qv = jvm.get_static("java.io.Input", "_hostAxisQueue");
        if (qv.tag == ValueTag::Obj) q = qv.v.o;
        if (!q) {
          q = jvm.new_object("java.lang.Object");
          jvm.set_static("java.io.Input", "_hostAxisQueue",
                         Value::make_obj(q));
        }
        const int32_t axis =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        float val = 1.f;
        if (args.size() >= 2 && args[1].tag == ValueTag::Float) val = args[1].v.f;
        q->fields["axis"] = Value::make_int(axis);
        q->fields["value"] = Value::make_float(val);
        std::printf("[native] Input.queueAxis axis=%d val=%.2f\n", axis, val);
        return Value::make_void();
      });
}

}  // namespace slrr
