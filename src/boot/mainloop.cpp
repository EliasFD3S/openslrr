#include "slrr/mainloop.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/video_fmv.hpp"

#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/host_input.hpp"
#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/slrr_font.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace slrr {
namespace {

constexpr float kDtClamp = 0.1f;  // PE Engine_dtClamp @ 0x60C8CC

void dispatch_due_timers(Jvm& jvm, float now, int32_t* fired) {
  // Soft PE TickTimers → queueEvent EVENT_TIME (0x80) → handleEvent(ref,I,I).
  // Gate on GameType.event_mask like stock mask test. Only the live
  // section may consume timers (stale Splash from tree smoke).
  // GameLogic is not a section: its 10 s timer is the 4x clock
  // (spendTime(timeRefreshRate * timeFactor), then it re-arms).
  Object* actual = nullptr;
  Value av = jvm.get_static("java.game.GameLogic", "actualState");
  if (av.tag == ValueTag::Obj) actual = av.v.o;
  for (const DueTimer& t : gameref_poll_timers(now)) {
    auto* o = static_cast<Object*>(t.key);
    if (!o) continue;
    const bool game_clock = o->class_name == "java.game.GameLogic";
    if (actual && o != actual && !game_clock) continue;
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

// Mechanic.show starts this thread. hide stops it. The script loop is
// the only writer that replaces Garage infoline after createText.
static Object* g_spinning_mechanic = nullptr;

static Object* field_obj(Object* o, const char* name) {
  if (!o) return nullptr;
  auto it = o->fields.find(name);
  if (it == o->fields.end() || it->second.tag != ValueTag::Obj) return nullptr;
  return it->second.v.o;
}

static int32_t field_int(Object* o, const char* name) {
  if (!o) return 0;
  auto it = o->fields.find(name);
  if (it == o->fields.end()) return 0;
  if (it->second.tag == ValueTag::Int) return it->second.v.i;
  if (it->second.tag == ValueTag::Float)
    return static_cast<int32_t>(it->second.v.f);
  return 0;
}

// Mechanic.run, the branch under actualPanelChanged || visualsUpdated.
// filterInventory sets the flag before show(), and the first tick has
// no selected slot, so the line becomes null. That is why "Welcome!"
// exists in the script and never stays on screen.
static void apply_mechanic_infoline(Jvm& jvm, Object* mechanic) {
  if (!mechanic || mechanic != g_spinning_mechanic) return;
  Object* inv = field_obj(mechanic, "inventory");
  const int32_t changed = field_int(mechanic, "actualPanelChanged");
  const int32_t visuals = field_int(inv, "visualsUpdated");
  if (!changed && !visuals) return;
  mechanic->fields["actualPanelChanged"] = Value::make_int(0);
  if (inv) inv->fields["visualsUpdated"] = Value::make_int(0);

  Object* line = field_obj(mechanic, "infoline");
  if (!line) return;
  Object* panel = field_obj(mechanic, "actualPanel");
  Object* item = field_obj(panel, "invItem");
  Object* text = nullptr;
  if (panel && item) {
    Value info = jvm.invoke(item->class_name, "getInfo", "()Ljava.lang.String;",
                             {Value::make_obj(item)}, false);
    if (info.tag == ValueTag::Obj) text = info.v.o;
  } else if (panel) {
    text = jvm.new_string("empty slot");
  }
  jvm.invoke("java.render.Text", "changeText", "(Ljava.lang.String;)V",
             {Value::make_obj(line), Value::make_obj(text)}, false);
}

void mainloop_adopt_section(Jvm& jvm, SectionMachine& sections) {
  Value av = jvm.get_static("java.game.GameLogic", "actualState");
  if (av.tag != ValueTag::Obj) return;
  sections.adopt_actual(av.v.o);
}

// GameType.run is the async message thread. Thread.start does not enter
// it, so CarLot.osdCommand's putMessage never reached handleMessage.
// Drain the live section's port the way that loop would, between frames.
void pump_async_messages(Jvm& jvm, Object* game) {
  if (!game) return;
  for (int n = 0; n < 16; ++n) {
    Object* mp = field_obj(game, "mp");
    Object* fifo = field_obj(mp, "fifo");
    if (!fifo || field_int(fifo, "elementCount") <= 0) return;
    Value msg = jvm.invoke("java.util.MessagePort", "getMessage",
                           "()Ljava.util.Message;",
                           {Value::make_obj(mp)}, false);
    if (msg.tag != ValueTag::Obj || !msg.v.o) return;
    const int32_t type = field_int(msg.v.o, "type");
    const int32_t cmd = field_int(msg.v.o, "cmd");
    // MT_SYSTEM + MC_SYS_TERMINATE. leaveAsyncMode posts this on exit.
    if (type == 0 && cmd == 0) {
      jvm.invoke(game->class_name, "leaveAsyncMode_Script_Real", "()V",
                 {Value::make_obj(game)}, false);
      return;
    }
    std::printf("[async] %s cmd=%d\n", game->class_name.c_str(), cmd);
    jvm.invoke(game->class_name, "handleMessage", "(Ljava.util.Message;)V",
               {Value::make_obj(game), msg}, false);
  }
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
  pump_async_messages(jvm, sections.actual());

  // --- Input_tick @ 0x54DDA0 (Soft: live axes + garage orbit) ---
  float frame_dt = dt;
  if (frame_dt < 0.f) frame_dt = 0.f;
  if (frame_dt > kDtClamp) frame_dt = kDtClamp;

  auto& in = host_input();
  const float yaw = host_input_get_axis(7, 0);
  const float pitch = host_input_get_axis(8, 0);
  const float zoom = host_input_get_axis(10, 0);
  // One camera: the script camera. Orbit, spring and Pori all live on it.
  gameref_orbit_world_camera(yaw * 2.5f, pitch * 1.5f, zoom, frame_dt);
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
      osd_set_loading_dialog(true);
      Object* dlg = screen->fields["loadingDialog"].v.o;
      Object* losd = nullptr;
      if (dlg) {
        auto oit = dlg->fields.find("osd");
        if (oit != dlg->fields.end() && oit->second.tag == ValueTag::Obj)
          losd = oit->second.v.o;
      }
      // Dealer show(pic) already built its own plate (New/Used welcome).
      // Covering it with Loading.dds hid that picture. The generic plate
      // is only the stand-in until the dialog has a background.
      bool own_plate = false;
      if (losd) {
        for (const OsdWidget& w : osd_widgets()) {
          if (w.owner == losd && w.background && w.half_w > 0.4f &&
              w.half_h > 0.4f && !w.icon_path.empty()) {
            own_plate = true;
            break;
          }
        }
      }
      if (own_plate) {
        osd_set_loading_overlay({});
      } else {
        std::string pic = jvm.game_root();
        if (!pic.empty() && pic.back() != '\\' && pic.back() != '/')
          pic.push_back('\\');
        pic += "frontend\\textures\\Loading.dds";
        osd_ensure_loading_plate(losd, pic);
        osd_set_loading_overlay(pic);
      }
      const auto now = std::chrono::steady_clock::now();
      if (dlg != load_seen) {
        load_seen = dlg;
        load_until = now + std::chrono::milliseconds(500);
      }
      auto as_float = [&](const char* name) -> float {
        if (!screen) return 0.f;
        auto it = screen->fields.find(name);
        if (it == screen->fields.end()) return 0.f;
        if (it->second.tag == ValueTag::Float) return it->second.v.f;
        if (it->second.tag == ValueTag::Int)
          return static_cast<float>(it->second.v.i);
        return 0.f;
      };
      // userWait(sec) sets waitForUser and then termSig.wait(). SoftTimer
      // is the thread that would notify after sec; it never starts. A
      // click does the same, which is loadingDialog.wait().
      static Object* wait_screen = nullptr;
      static int32_t wait_gen = 0;
      static std::chrono::steady_clock::time_point wait_deadline{};
      const int32_t wait_user = as_int("waitForUser");
      bool skip = false;
#ifdef _WIN32
      {
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                          (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
                          (GetAsyncKeyState(VK_SPACE) & 0x8000) ||
                          (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
        // The click that opened the dealer is still down. Wait until it
        // is released before a new press can skip the plate.
        static bool armed = false;
        static bool held = false;
        static Object* skip_dlg = nullptr;
        if (skip_dlg != dlg) {
          skip_dlg = dlg;
          armed = false;
          held = down;
        }
        if (!down) armed = true;
        skip = armed && down && !held;
        held = down;
      }
#endif
      if (now >= load_until) {
        const int32_t init = as_int("init");
        if (init > 0) {
          screen->fields["init"] = Value::make_int(init - 1);
        } else if (wait_user == 0) {
          Value busy =
              jvm.invoke("java.lang.System", "isLoading", "()I", {}, true);
          const bool loading =
              busy.tag == ValueTag::Int ? busy.v.i != 0 : false;
          if (!loading)
            jvm.invoke("java.render.LoadingScreen", "hide", "()V",
                       {Value::make_obj(screen)}, false);
        } else {
          if (wait_screen != screen || wait_gen != wait_user) {
            wait_screen = screen;
            wait_gen = wait_user;
            // userWait(5): the picture stays, then FlashText asks for a key.
            // userWait(-1): no timer, the prompt is immediate. The timer
            // does not dismiss the plate.
            float limit = as_float("waitForUserTimeLimit");
            if (limit < 0.f) limit = 0.f;
            wait_deadline =
                now + std::chrono::milliseconds(static_cast<int>(limit * 1000.f));
          }
          if (now >= wait_deadline && skip) {
            jvm.invoke("java.render.LoadingScreen", "hide", "()V",
                       {Value::make_obj(screen)}, false);
          } else if (now >= wait_deadline && dlg) {
            Object* label = nullptr;
            auto tit = dlg->fields.find("loadingText");
            if (tit != dlg->fields.end() && tit->second.tag == ValueTag::Obj)
              label = tit->second.v.o;
            static bool prompt_on = true;
            static Object* flash_dlg = nullptr;
            static std::chrono::steady_clock::time_point next_flash{};
            if (flash_dlg != dlg) {
              flash_dlg = dlg;
              prompt_on = true;
              next_flash = now + std::chrono::milliseconds(600);
            } else if (now >= next_flash) {
              prompt_on = !prompt_on;
              next_flash = now + std::chrono::milliseconds(600);
            }
            static bool shown_on = false;
            static Object* shown_label = nullptr;
            if (label && (shown_label != label || shown_on != prompt_on)) {
              Object* text = prompt_on ? jvm.new_string("PRESS ENTER TO CONTINUE...")
                                       : nullptr;
              jvm.invoke("java.render.Text", "changeText",
                         "(Ljava.lang.String;)V",
                         {Value::make_obj(label), Value::make_obj(text)}, false);
              shown_on = prompt_on;
              shown_label = label;
            }
          }
        }
      }
    } else {
      load_seen = nullptr;
      osd_set_loading_dialog(false);
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

  apply_mechanic_infoline(jvm, g_spinning_mechanic);
  part_plant_parked();

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

void mainloop_note_thread(Jvm& jvm, Object* thread, bool started) {
  Object* tgt = field_obj(thread, "target");
  if (!tgt || tgt->class_name != "java.game.Mechanic") return;
  if (!started) {
    if (g_spinning_mechanic == tgt) g_spinning_mechanic = nullptr;
    return;
  }
  g_spinning_mechanic = tgt;
  // Same moment the script thread would take its first 20 ms tick.
  apply_mechanic_infoline(jvm, tgt);
}

}  // namespace slrr
