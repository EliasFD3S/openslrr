#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "../platform/host_input_capture.hpp"
#include "../platform/host_input_devices.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/mod_api.hpp"
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
void register_mod_settings(Jvm& jvm) {
  static_assert(kModApiVersion >= 1, "ModSettings is part of mod API 1");
  constexpr const char* cls = "java.openslrr.ModSettings";
  auto& registry = jvm.natives();
  registry.register_method(cls, "getInt", "(Ljava.lang.String;I)I",
      [](Object*, const std::vector<Value>& args) {
        const auto* key = native_arg_object(args, 0);
        const int def = native_arg_integer(args, 1);
        return Value::make_int(key ? host_mod_get_int(key->str, def) : def);
      });
  registry.register_method(cls, "setInt", "(Ljava.lang.String;I)V",
      [](Object*, const std::vector<Value>& args) {
        const auto* key = native_arg_object(args, 0);
        if (key && args.size() >= 2 && args[1].tag == ValueTag::Int)
          host_mod_set_number(key->str, args[1].v.i);
        return Value::make_void();
      });
  registry.register_method(cls, "getFloat", "(Ljava.lang.String;F)F",
      [](Object*, const std::vector<Value>& args) {
        const auto* key = native_arg_object(args, 0);
        const float def = args.size() > 1 && args[1].tag == ValueTag::Float
            ? args[1].v.f : native_arg_number(args, 1);
        return Value::make_float(key ? host_mod_get_float(key->str, def) : def);
      });
  registry.register_method(cls, "setFloat", "(Ljava.lang.String;F)V",
      [](Object*, const std::vector<Value>& args) {
        const auto* key = native_arg_object(args, 0);
        if (key && args.size() >= 2) {
          if (args[1].tag == ValueTag::Float) host_mod_set_number(key->str, args[1].v.f);
          else if (args[1].tag == ValueTag::Int)
            host_mod_set_number(key->str, static_cast<float>(args[1].v.i));
        }
        return Value::make_void();
      });
  registry.register_method(cls, "getString", "(Ljava.lang.String;Ljava.lang.String;)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        if (const auto* key = native_arg_object(args, 0))
          if (auto found = host_mod_get_string(key->str)) return Value::make_obj(jvm.new_string(*found));
        return Value::make_obj(native_arg_object(args, 1));
      });
  registry.register_method(cls, "setString", "(Ljava.lang.String;Ljava.lang.String;)V",
      [](Object*, const std::vector<Value>& args) {
        const auto* key = native_arg_object(args, 0);
        const auto* value = native_arg_object(args, 1);
        if (key && value) host_mod_set_string(key->str, value->str);
        return Value::make_void();
      });
}

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
  register_mod_settings(jvm);
  // Host-only options by id: the table in host_game_options.cpp (#405).
  jvm.natives().register_method("java.game.OptionsDialog", "getRendererOption", "(I)I",
      [](Object*, const std::vector<Value>& args) {
        int value = 0;
        host_option_get(native_arg_integer(args, 0, -1), value);
        return Value::make_int(value);
      });
  jvm.natives().register_method("java.game.OptionsDialog", "setRendererOption", "(II)V",
      [](Object*, const std::vector<Value>& args) {
        if (args.size() < 2 ||
            (args[0].tag != ValueTag::Int && args[0].tag != ValueTag::Float) ||
            (args[1].tag != ValueTag::Int && args[1].tag != ValueTag::Float))
          return Value::make_void();
        host_option_set(native_arg_integer(args, 0, -1), native_arg_integer(args, 1));
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
      const float pitch=num(2,1),volume=num(3,1);
      const bool loop=static_cast<int>(num(4,0))&1;
      const int32_t instance=static_cast<int>(num(5,0));
      // A position places the sound in the world (#390): the resource's
      // mindist/maxdist give the distance model, a positive radius caps it.
      Object* where=native_arg_object(args,0);
      float position[3]{};
      bool placed=where!=nullptr;
      const char* axes[3]={"x","y","z"};
      for(int i=0;placed&&i<3;++i){
        const auto f=where->fields.find(axes[i]);
        placed=f!=where->fields.end()&&(f->second.tag==ValueTag::Float?std::isfinite(f->second.v.f):f->second.tag==ValueTag::Int);
        if(placed)position[i]=f->second.tag==ValueTag::Float?f->second.v.f:static_cast<float>(f->second.v.i);
      }
      if(!placed)return Value::make_int(host_sfx_play_file(path.string(),pitch,volume,loop,instance));
      HostSfxPlace place;
      std::copy(position,position+3,place.position);
      const auto prop=[&](const char* key,float fallback){
        const auto text=rpk.payload_prop(resource_id_of(self),key);
        const float value=text.empty()?fallback:std::strtof(text.c_str(),nullptr);
        return std::isfinite(value)&&value>0.f?value:fallback;
      };
      place.reference=prop("mindist",2.5f);
      place.maximum=(std::max)(place.reference,prop("maxdist",60.f));
      const float radius=num(1,0);
      if(std::isfinite(radius)&&radius>0.f)place.maximum=(std::max)(place.reference,radius);
      return Value::make_int(host_sfx_play_at(path.string(),place,pitch,volume,loop,instance));
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
        const int i = native_arg_integer(args, 0, -1);
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
          host_set_video_mode(jvm, {native_arg_integer(args, 0), native_arg_integer(args, 1), native_arg_integer(args, 2),
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
  // java.io.InputDevices (#345): the SDL3 devices for Options > Controls.
  // Index i is ControlSet device i + 3.
  {
    const char* kDevices = "java.io.InputDevices";
    auto info = [integer](const std::vector<Value>& args) {
      return host_input_devices().info(integer(args, 0));
    };
    jvm.natives().register_method(kDevices, "refresh", "()I",
        [](Object*, const std::vector<Value>&) {
          host_input_devices().refresh();
          return Value::make_int(host_input_devices().count());
        });
    jvm.natives().register_method(kDevices, "count", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_devices().count()); });
    jvm.natives().register_method(kDevices, "name", "(I)Ljava.lang.String;",
        [&jvm, info](Object*, const std::vector<Value>& a) { return Value::make_obj(jvm.new_string(info(a).name)); });
    jvm.natives().register_method(kDevices, "type", "(I)Ljava.lang.String;",
        [&jvm, info](Object*, const std::vector<Value>& a) { return Value::make_obj(jvm.new_string(info(a).type)); });
    jvm.natives().register_method(kDevices, "power", "(I)Ljava.lang.String;",
        [&jvm, info](Object*, const std::vector<Value>& a) { return Value::make_obj(jvm.new_string(info(a).power)); });
    jvm.natives().register_method(kDevices, "connected", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).connected); });
    jvm.natives().register_method(kDevices, "axes", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).axes); });
    jvm.natives().register_method(kDevices, "buttons", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).buttons); });
    jvm.natives().register_method(kDevices, "hats", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).hats); });
    jvm.natives().register_method(kDevices, "battery", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).battery); });
    jvm.natives().register_method(kDevices, "canForceFeedback", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).force_feedback); });
    jvm.natives().register_method(kDevices, "testForceFeedback", "(I)I",
        [integer](Object*, const std::vector<Value>& a) {
          return Value::make_int(host_input_devices().test_force(integer(a, 0)));
        });
    jvm.natives().register_method(kDevices, "canRumble", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).rumble); });
    jvm.natives().register_method(kDevices, "calibrated", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).calibrated); });
    jvm.natives().register_method(kDevices, "calibrating", "(I)I",
        [info](Object*, const std::vector<Value>& a) { return Value::make_int(info(a).calibrating); });
    jvm.natives().register_method(kDevices, "activity", "(I)F",
        [integer](Object*, const std::vector<Value>& a) {
          return Value::make_float(host_input_devices().activity(integer(a, 0)));
        });
    jvm.natives().register_method(kDevices, "rumble", "(IFI)I",
        [integer](Object*, const std::vector<Value>& a) {
          float strength = .5f;
          if (a.size() > 1) strength = a[1].tag == ValueTag::Float ? a[1].v.f
                                     : a[1].tag == ValueTag::Int ? float(a[1].v.i) : .5f;
          return Value::make_int(host_input_devices().rumble(integer(a, 0), strength, integer(a, 2)));
        });
    jvm.natives().register_method(kDevices, "calibrateBegin", "(I)V",
        [integer](Object*, const std::vector<Value>& a) {
          host_input_devices().calibrate_begin(integer(a, 0));
          return Value::make_void();
        });
    jvm.natives().register_method(kDevices, "calibrateEnd", "(I)I",
        [integer](Object*, const std::vector<Value>& a) {
          return Value::make_int(host_input_devices().calibrate_end(integer(a, 0)));
        });
    jvm.natives().register_method(kDevices, "calibrateReset", "(I)V",
        [integer](Object*, const std::vector<Value>& a) {
          host_input_devices().calibrate_reset(integer(a, 0));
          return Value::make_void();
        });
    // Device class of a ControlSet device id (InputKind): 0 keyboard,
    // 1 mouse, 2 pad, 3 wheel/sim peripheral, -1 none.
    jvm.natives().register_method(kDevices, "kind", "(I)I",
        [integer](Object*, const std::vector<Value>& a) {
          return Value::make_int(static_cast<int>(host_input_device_kind(integer(a, 0))));
        });
    // The rebind listener (host_input_capture.hpp) over every device at once.
    auto none = [](Object*, const std::vector<Value>&) { return Value::make_void(); };
    jvm.natives().register_method(kDevices, "captureBegin", "()V",
        [none](Object* o, const std::vector<Value>& a) { host_input_capture_begin(); return none(o, a); });
    // Only the device classes in the mask (1 << kind) may be taken (#405).
    jvm.natives().register_method(kDevices, "captureBeginKinds", "(I)V",
        [none, integer](Object* o, const std::vector<Value>& a) {
          host_input_capture_begin(integer(a, 0));
          return none(o, a);
        });
    jvm.natives().register_method(kDevices, "captureCancel", "()V",
        [none](Object* o, const std::vector<Value>& a) { host_input_capture_cancel(); return none(o, a); });
    jvm.natives().register_method(kDevices, "captureState", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_capture().state()); });
    jvm.natives().register_method(kDevices, "captureDevice", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_capture().device()); });
    jvm.natives().register_method(kDevices, "captureAxis", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_capture().axis()); });
    jvm.natives().register_method(kDevices, "captureAnalog", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_capture().analog()); });
    jvm.natives().register_method(kDevices, "captureHeld", "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(host_input_capture().held()); });
    jvm.natives().register_method(kDevices, "captureRest", "()F",
        [](Object*, const std::vector<Value>&) { return Value::make_float(host_input_capture().rest()); });
    jvm.natives().register_method(kDevices, "capturePeak", "()F",
        [](Object*, const std::vector<Value>&) { return Value::make_float(host_input_capture().peak()); });
    jvm.natives().register_method(kDevices, "captureProgress", "()F",
        [](Object*, const std::vector<Value>&) { return Value::make_float(host_input_capture_progress()); });
  }
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
