#include "slrr/mainloop.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/video_fmv.hpp"

#include "slrr/garage_camera.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_input.hpp"
#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/slrr_font.hpp"
#include <chrono>
#include <cstdio>
#include <string>

namespace slrr {
namespace {

constexpr float kDtClamp = 0.1f;  // PE Engine_dtClamp @ 0x60C8CC

void dispatch_due_timers(Jvm& jvm, float now, int32_t* fired) {
  // Soft PE TickTimers → queueEvent EVENT_TIME (0x80) → handleEvent(ref,I,I).
  // Gate on GameType.event_mask like stock mask test. Only the live
  // GameLogic.actualState may consume timers (stale Splash from tree smoke).
  Object* actual = nullptr;
  Value av = jvm.get_static("java.game.GameLogic", "actualState");
  if (av.tag == ValueTag::Obj) actual = av.v.o;
  for (const DueTimer& t : gameref_poll_timers(now)) {
    auto* o = static_cast<Object*>(t.key);
    if (!o) continue;
    if (actual && o != actual) continue;
    GameRefState* st = gameref_get(o);
    if (st && (st->event_mask & kEventTime) == 0) continue;
    jvm.invoke(o->class_name, "handleEvent",
               "(Ljava.util.resource.GameRef;II)V",
               {Value::make_obj(o), Value::make_obj(nullptr),
                Value::make_int(kEventTime), Value::make_int(t.timer_id)},
               false);
    if (fired) ++(*fired);
  }
}

}  // namespace

void mainloop_adopt_section(Jvm& jvm, SectionMachine& sections) {
  Value av = jvm.get_static("java.game.GameLogic", "actualState");
  if (av.tag != ValueTag::Obj) return;
  sections.adopt_actual(av.v.o);
}

void mainloop_sync_section_render(IRenderBackend& render,
                                  SectionMachine& sections) {
  Object* actual = sections.actual();
  auto object_field=[&](const char* field)->Object* {
    if(!actual)return nullptr;auto it=actual->fields.find(field);
    return it!=actual->fields.end()&&it->second.tag==ValueTag::Obj?it->second.v.o:nullptr;
  };
  scene_instances_select(object_field("map"),object_field("skydome"));
  const bool in_garage =
      actual && actual->class_name.find("Garage") != std::string::npos;
  if (in_garage) {
    if(video_fmv_is_non_exclusive())video_fmv_close();
    bgfx_set_garage_viewport(render, true, osd_widget_count());
    bgfx_set_osd_overlay(render, false);
  } else {
    bgfx_set_garage_viewport(render, false, 0);
    bgfx_set_osd_overlay(render, true);
  }
}

MainLoopFrameResult mainloop_frame(Jvm& jvm, IRenderBackend& render,
                                   SectionMachine& sections, float dt) {
  MainLoopFrameResult out;
  mainloop_adopt_section(jvm, sections);

  // --- Input_tick @ 0x54DDA0 (Soft: live axes + garage orbit) ---
  float frame_dt = dt;
  if (frame_dt < 0.f) frame_dt = 0.f;
  if (frame_dt > kDtClamp) frame_dt = kDtClamp;

  auto& in = host_input();
  const float yaw = host_input_get_axis(7, 0);
  const float pitch = host_input_get_axis(8, 0);
  const float zoom = host_input_get_axis(10, 0);
  garage_camera_tick(frame_dt > 0.f ? frame_dt : (1.f / 60.f), yaw, pitch,
                     zoom);
  in.look_yaw = 0.f;
  in.look_pitch = 0.f;
  in.look_zoom = 0.f;

  // --- SimulateFrame slice: GameType timers (phys/CONTROL OOS lean host) ---
  dispatch_due_timers(jvm, gameref_now(), &out.timers_fired);

  // Credits scroll lives on MainMenuDialog (field mmd), not on the
  // section. animate() only while that dialog's credits group is showing.
  if (Object* menu = sections.actual()) {
    Object* scroll = menu;
    auto mit = menu->fields.find("mmd");
    if (mit != menu->fields.end() && mit->second.tag == ValueTag::Obj &&
        mit->second.v.o)
      scroll = mit->second.v.o;
    auto ag = scroll->fields.find("actGroup");
    auto cg = scroll->fields.find("creditsGroup");
    auto as_i = [](const Value& v, bool* ok) -> int32_t {
      if (v.tag == ValueTag::Int) {
        *ok = true;
        return v.v.i;
      }
      if (v.tag == ValueTag::Float) {
        *ok = true;
        return static_cast<int32_t>(v.v.f);
      }
      *ok = false;
      return 0;
    };
    bool ag_ok = false, cg_ok = false;
    const int32_t act =
        (ag != scroll->fields.end()) ? as_i(ag->second, &ag_ok) : 0;
    const int32_t cred =
        (cg != scroll->fields.end()) ? as_i(cg->second, &cg_ok) : 0;
    if (ag_ok && cg_ok && cred != 0 && act == cred) {
      jvm.invoke(scroll->class_name, "animate", "()V",
                 {Value::make_obj(scroll)}, false);
      // animate() writes creditsPos then renderinst.setMatrix. That call
      // does not move the OSD glyph, so place the lines the script has
      // in the scroller window. The menu OSD is fullscreen: script y is
      // the screen y.
      auto field_obj = [&](const char* name) -> Object* {
        auto it = scroll->fields.find(name);
        if (it == scroll->fields.end() || it->second.tag != ValueTag::Obj)
          return nullptr;
        return it->second.v.o;
      };
      Object* txts = field_obj("creditsTxt");
      Object* poss = field_obj("creditsPos");
      bool n_ok = false, in_ok = false, out_ok = false;
      auto nit = scroll->fields.find("creditLines");
      auto iit = scroll->fields.find("lastInLine");
      auto oit = scroll->fields.find("lastOutLine");
      const int32_t nlines =
          (nit != scroll->fields.end()) ? as_i(nit->second, &n_ok) : 0;
      const int32_t last_in =
          (iit != scroll->fields.end()) ? as_i(iit->second, &in_ok) : 0;
      const int32_t last_out =
          (oit != scroll->fields.end()) ? as_i(oit->second, &out_ok) : 0;
      if (txts && poss && txts->is_array && poss->is_array && n_ok &&
          nlines > 0 && in_ok && out_ok) {
        const int32_t nslots =
            static_cast<int32_t>(txts->slots.size()) < nlines
                ? static_cast<int32_t>(txts->slots.size())
                : nlines;
        for (int32_t j = 0; j < nslots; ++j) {
          const Value& tv = txts->slots[static_cast<size_t>(j)];
          if (tv.tag == ValueTag::Obj && tv.v.o)
            slrr_text_set_visible(tv.v.o, false);
        }
        int32_t endc = last_in;
        if (endc < last_out) endc += nlines;
        // The script window is about two screens (spacing 0.1). Cap a
        // runaway lastIn so a stuck exit test cannot draw every line.
        const int32_t limit = nlines < 48 ? nlines : 48;
        for (int32_t n = 0, i = last_out; i <= endc && n < limit; ++i, ++n) {
          const int32_t j = i % nlines;
          if (j < 0) continue;
          const size_t idx = static_cast<size_t>(j);
          if (idx >= txts->slots.size() || idx >= poss->slots.size()) continue;
          const Value& tv = txts->slots[idx];
          const Value& pv = poss->slots[idx];
          if (tv.tag != ValueTag::Obj || !tv.v.o) continue;
          float y = 0.f;
          if (pv.tag == ValueTag::Float)
            y = pv.v.f;
          else if (pv.tag == ValueTag::Int)
            y = static_cast<float>(pv.v.i);
          else
            continue;
          float x = 0.f;
          auto xt = tv.v.o->fields.find("_text_x");
          if (xt != tv.v.o->fields.end() &&
              xt->second.tag == ValueTag::Float)
            x = xt->second.v.f;
          slrr_text_set_visible(tv.v.o, true);
          slrr_text_set_behind_ui(tv.v.o, true);
          slrr_text_set_pos(tv.v.o, x, y);
        }
      }
    }
  }

  // --- Jvm_PumpFrame @ 0x418D10 — no green-thread budget yet ---

  // LoadingScreen.run is a thread the host does not schedule. show() leaves
  // the dialog up; the script hides it once init has ticked and nothing is
  // loading. waitForUser keeps the plate until the script's own wait.
  {
    Value ls = jvm.get_static("java.render.Frontend", "loadingScreen");
    Object* screen = (ls.tag == ValueTag::Obj) ? ls.v.o : nullptr;
    auto shown = [&](const char* name) -> bool {
      if (!screen) return false;
      auto it = screen->fields.find(name);
      return it != screen->fields.end() && it->second.tag == ValueTag::Obj &&
             it->second.v.o;
    };
    auto as_int = [&](const char* name) -> int32_t {
      if (!screen) return 0;
      auto it = screen->fields.find(name);
      if (it == screen->fields.end()) return 0;
      if (it->second.tag == ValueTag::Int) return it->second.v.i;
      if (it->second.tag == ValueTag::Float)
        return static_cast<int32_t>(it->second.v.f);
      return 0;
    };
    // The script builds the next screen before it pumps, then display()
    // only waited one frame. Loading.dds never reached the glass.
    static Object* load_seen = nullptr;
    static std::chrono::steady_clock::time_point load_until{};
    if (shown("loadingDialog")) {
      Object* dlg = screen->fields["loadingDialog"].v.o;
      Object* losd = nullptr;
      if (dlg) {
        auto oit = dlg->fields.find("osd");
        if (oit != dlg->fields.end() && oit->second.tag == ValueTag::Obj)
          losd = oit->second.v.o;
      }
      std::string pic = jvm.game_root();
      if (!pic.empty() && pic.back() != '\\' && pic.back() != '/')
        pic.push_back('\\');
      pic += "frontend\\textures\\Loading.dds";
      osd_ensure_loading_plate(losd, pic);
      osd_set_loading_overlay(pic);
      const auto now = std::chrono::steady_clock::now();
      if (dlg != load_seen) {
        load_seen = dlg;
        load_until = now + std::chrono::milliseconds(500);
      }
      if (now >= load_until) {
        const int32_t init = as_int("init");
        if (init > 0) {
          screen->fields["init"] = Value::make_int(init - 1);
        } else if (as_int("waitForUser") == 0) {
          Value busy =
              jvm.invoke("java.lang.System", "isLoading", "()I", {}, true);
          const bool loading =
              busy.tag == ValueTag::Int ? busy.v.i != 0 : false;
          if (!loading)
            jvm.invoke("java.render.LoadingScreen", "hide", "()V",
                       {Value::make_obj(screen)}, false);
        }
      }
    } else {
      load_seen = nullptr;
      osd_set_loading_overlay({});
    }
  }

  // GameLogic.run is the time thread. Thread.start does not schedule it,
  // so DAY / TIME stayed as empty labels. Same tick the script would do
  // after timeRefresher.notify: timeToString + changeText.
  {
    auto text_of = [](const Value& v) -> Object* {
      return (v.tag == ValueTag::Obj) ? v.v.o : nullptr;
    };
    Object* day_txt = text_of(jvm.get_static("java.game.GameLogic", "dayTxt"));
    Object* time_txt =
        text_of(jvm.get_static("java.game.GameLogic", "timeTxt"));
    if (day_txt || time_txt) {
      int32_t day = 1;
      Value dv = jvm.get_static("java.game.GameLogic", "day");
      if (dv.tag == ValueTag::Int) day = dv.v.i;
      else if (dv.tag == ValueTag::Float)
        day = static_cast<int32_t>(dv.v.f);
      float time = 12.f * 3600.f;
      Value tv = jvm.get_static("java.game.GameLogic", "time");
      if (tv.tag == ValueTag::Float) time = tv.v.f;
      else if (tv.tag == ValueTag::Int) time = static_cast<float>(tv.v.i);
      static int32_t shown_day = -1;
      static std::string shown_time;
      static Object* shown_day_text = nullptr;
      static Object* shown_time_text = nullptr;
      if (day_txt && (day != shown_day || day_txt != shown_day_text)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", day);
        jvm.invoke("java.render.Text", "changeText", "(Ljava.lang.String;)V",
                   {Value::make_obj(day_txt),
                    Value::make_obj(jvm.new_string(buf))},
                   false);
        shown_day = day;
        shown_day_text = day_txt;
      }
      const int minute = static_cast<int>(time) / 60;
      static int shown_minute = -100000;
      if (time_txt && (minute != shown_minute || time_txt != shown_time_text)) {
        // TCF_NOSECONDS | TCF_US
        Value formatted = jvm.invoke(
            "java.lang.String", "timeToString",
            "(FI)Ljava.lang.String;",
            {Value::make_float(time), Value::make_int(0x21)}, true);
        std::string clock;
        if (formatted.tag == ValueTag::Obj && formatted.v.o)
          clock = formatted.v.o->str;
        shown_minute = minute;
        if (!clock.empty() && (clock != shown_time || time_txt != shown_time_text)) {
          jvm.invoke("java.render.Text", "changeText",
                     "(Ljava.lang.String;)V",
                     {Value::make_obj(time_txt), Value::make_obj(formatted.v.o)},
                     false);
          shown_time = clock;
          shown_time_text = time_txt;
        }
      }
    }
  }

  // --- Present: AltPresentGate (FMV) else PresentFrame ---
  // Soft: backend flush pumps FMV SampleGrabber + Present (libbgfx path).
  render.flush();

  // --- PollWindowQuit @ 0x5522A0 ---
  out.quit = render.quit_requested();
  if (!sections.actual()) out.quit = true;
  Value hq = jvm.get_static("java.lang.System", "_hostQuit");
  if (hq.tag == ValueTag::Int && hq.v.i) out.quit = true;

  // --- EndFrame @ 0x554DC0 — FileAsync Soft no-op on lean host ---
  return out;
}

}  // namespace slrr
