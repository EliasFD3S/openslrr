#include "slrr/gc_roots.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/script_threads.hpp"
#include "slrr/script_events.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/video_fmv.hpp"
#include "slrr/host_music.hpp"
#include "slrr/host_game_options.hpp"

#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/host_input.hpp"
#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/slrr_font.hpp"
#include <chrono>
#include <cmath>
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
    script_event_emit(jvm,o,nullptr,kEventTime,Value::make_int(t.timer_id));
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
static Object* g_mechanic_thread = nullptr;
static Object* g_mechanic_hover = nullptr;
static int g_mechanic_pointer_depth = 0;
static float g_inventory_spin = 0.f;

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
  // A car-part hover already wrote the name. The panel flag must not
  // replace it, and must not fire again on the next tick.
  if (g_mechanic_hover) {
    mechanic->fields["actualPanelChanged"] = Value::make_int(0);
    if (inv) inv->fields["visualsUpdated"] = Value::make_int(0);
    return;
  }
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
  if(script_thread_alive(jvm,field_obj(game,"t")))return;
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

// Keep the showroom camera above the garage camera. Its actual pose now
// comes from CarSelect.run; retain the fallback only without a live thread.
void spin_car_select_camera(Object* section) {
  static void* held = nullptr;
  auto release = []() {
    if (!held) return;
    if (GameRefState* old = gameref_get(held)) {
      if (old->cam_seq == 0x7fffffff) {
        old->cam_active = false;
        old->cam_seq = 0;
      }
    }
    held = nullptr;
  };
  const bool select =
      section && section->class_name.find("CarSelect") != std::string::npos;
  if (!select) {
    release();
    return;
  }
  auto it = section->fields.find("Cam");
  if (it == section->fields.end() || it->second.tag != ValueTag::Obj ||
      !it->second.v.o)
    return;
  GameRefState* cam = gameref_get(it->second.v.o);
  if (!cam) return;
  if (held && held != cam->key) release();
  held = cam->key;
  cam->cam_active = true;
  cam->cam_seq = 0x7fffffff;
  if(field_int(field_obj(section,"CamThread"),"_thread_alive"))return;
  cam->aim_look = false;
  cam->aim_move = false;
  const float yaw = -0.01f + gameref_now() * 0.5f;
  const float place = -0.15f;
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sp = std::sin(place), cp = std::cos(place);
  const float arm = 1.5f * sp + 8.5f * cp;
  gameref_set_matrix(cam->key, sy * arm, 1.5f * cp - 8.5f * sp, cy * arm, yaw,
                     -0.23f, 0.f);
}

void mainloop_sync_section_render(IRenderBackend& render,
                                  SectionMachine& sections) {
  Object* actual = sections.actual();
  auto object_field=[&](const char* field)->Object* {
    if(!actual)return nullptr;auto it=actual->fields.find(field);
    return it!=actual->fields.end()&&it->second.tag==ValueTag::Obj?it->second.v.o:nullptr;
  };
  spin_car_select_camera(actual);
  // Garage and city publish a map. The car selector parents the car to
  // CarScene and has no map, so the mesh was never collected.
  void* world = object_field("map");
  if (!world) world = object_field("CarScene");
  if (!world) world = object_field("Scene");
  if (!world)
    if (GameRefState* cam = fullscreen_script_camera()) world = cam->parent;
  scene_instances_select(world, object_field("skydome"));
  const bool in_garage =
      actual && actual->class_name.find("Garage") != std::string::npos;
  scene_instances_enable_static_floor(in_garage);
  // Close the menu movie once, on the way in. CarSelect.enter opens
  // prime.avi and then waits; that wait still pumps with the host
  // section stuck on the garage, and closing every frame killed the
  // video that had just started. Back then showed the selector plate
  // on the brown clear instead of the movie.
  static bool movie_closed_for_garage = false;
  if (in_garage) {
    if (!movie_closed_for_garage && video_fmv_is_non_exclusive())
      video_fmv_close();
    movie_closed_for_garage = true;
    bgfx_set_garage_viewport(render, true, osd_widget_count());
    bgfx_set_osd_overlay(render, false);
  } else {
    movie_closed_for_garage = false;
    bgfx_set_garage_viewport(render, false, 0);
    bgfx_set_osd_overlay(render, true);
  }
}

void spin_mechanic_inventory(Jvm& jvm, float dt) {
  if (!g_spinning_mechanic || g_mechanic_pointer_depth) {
    g_inventory_spin = 0.f;
    return;
  }
  Object* panel = nullptr;
  const auto& in = host_input();
  if (in.pointer_valid && !in.rmb_down)
    if (const OsdWidget* w = osd_hover_widget())
      if (w->handler &&
          w->handler->class_name.find("InventoryPanel") != std::string::npos)
        panel = w->handler;
  Object* current = field_obj(g_spinning_mechanic, "actualPanel");
  if (panel != current) {
    g_spinning_mechanic->fields["actualPanel"] = Value::make_obj(panel);
    g_spinning_mechanic->fields["actualPanelChanged"] = Value::make_int(1);
  }
  if (!panel) {
    g_inventory_spin = 0.f;
    return;
  }
  // Mechanic.run calls focusHook every 20 ms. That walks the slot camera
  // around the part, which is the spin in the inventory cell.
  if (dt < 0.f) dt = 0.f;
  g_inventory_spin += dt;
  while (g_inventory_spin >= 0.02f) {
    g_inventory_spin -= 0.02f;
    jvm.invoke(panel->class_name, "focusHook", "()V",
               {Value::make_obj(panel)}, false);
    if (g_inventory_spin > 0.1f) g_inventory_spin = 0.f;
  }
}

void dispatch_mechanic_pointer(Jvm& jvm) {
  auto& in = host_input();
  if (g_mechanic_pointer_depth) {
    in.part_click = false;
    return;
  }
  if (!g_spinning_mechanic) {
    g_mechanic_hover = nullptr;
    return;
  }
  if (!in.pointer_valid || in.rmb_down || in.view_w <= 0 || in.view_h <= 0)
    return;
  const bool click = in.part_click;
  in.part_click = false;

  auto part_at = [&](int px, int py) -> Object* {
    float eye[3], at[3], fov = 60.f, zn = 0.05f, zf = 500.f;
    if (!gameref_world_camera(eye, at, &fov, &zn, &zf)) return nullptr;
    float fx = at[0] - eye[0], fy = at[1] - eye[1], fz = at[2] - eye[2];
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-4f) return nullptr;
    fx /= fl;
    fy /= fl;
    fz /= fl;
    float rx = -fz, rz = fx;
    const float rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-4f) {
      rx = 1.f;
      rz = 0.f;
    } else {
      rx /= rl;
      rz /= rl;
    }
    const float ux = -rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy;
    const float aspect =
        static_cast<float>(in.view_w) / static_cast<float>(in.view_h);
    const float tany = std::tan(fov * 3.14159265f / 360.f);
    const float ndc_x =
        (2.f * px / static_cast<float>(in.view_w) - 1.f) * tany * aspect;
    const float ndc_y = (1.f - 2.f * py / static_cast<float>(in.view_h)) * tany;
    float dx = fx + rx * ndc_x + ux * ndc_y;
    float dy = fy + uy * ndc_y;
    float dz = fz + rz * ndc_x + uz * ndc_y;
    const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dl < 1e-6f) return nullptr;
    const float origin[3] = {eye[0], eye[1], eye[2]};
    const float dir[3] = {dx / dl, dy / dl, dz / dl};
    return part_render_owner(scene_instances_pick(origin, dir));
  };
  auto cursor = [&](Object* part, const char* param) {
    Object* text = jvm.new_string(param);
    script_event_emit(jvm,part,part,0x00010000,Value::make_obj(text));
    struct Depth {
      int &n;
      explicit Depth(int &n) : n(n) { ++n; }
      ~Depth() { --n; }
    } guard(g_mechanic_pointer_depth);
    jvm.invoke(g_spinning_mechanic->class_name, "handleEvent",
               "(Ljava.util.resource.GameRef;ILjava.lang.String;)V",
               {Value::make_obj(g_spinning_mechanic), Value::make_obj(part),
                Value::make_int(0x00010000), Value::make_obj(text)},
               false);
  };

  if (click)
    if (Object* part = part_at(in.part_click_x, in.part_click_y)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "5 0 %d 0 0 0 0 0 0",
                    gameref_make_live(part));
      cursor(part, buf);
    }
  float ox = 0.f, oy = 0.f;
  osd_pixel_to_text(in.pointer_x, in.pointer_y, in.view_w, in.view_h, &ox, &oy);
  Object* hover =
      osd_hit_index(ox, oy) >= 0 ? nullptr : part_at(in.pointer_x, in.pointer_y);
  if (hover == g_mechanic_hover) return;
  if (g_mechanic_hover) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "16 0 %d",
                  gameref_make_live(g_mechanic_hover));
    cursor(g_mechanic_hover, buf);
  }
  g_mechanic_hover = hover;
  if (!hover) return;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "15 0 %d -1", gameref_make_live(hover));
  cursor(hover, buf);
}

MainLoopFrameResult mainloop_frame(Jvm& jvm, IRenderBackend& render,
                                   SectionMachine& sections, double dt) {
  host_music_update();
  host_game_options_update(jvm);
  script_threads_tick(jvm);
  MainLoopFrameResult out;
  mainloop_adopt_section(jvm, sections);
  pump_async_messages(jvm, sections.actual());

  // --- Input_tick @ 0x54DDA0 (Soft: live axes + garage orbit) ---
  float frame_dt = static_cast<float>(dt);
  if (frame_dt < 0.f) frame_dt = 0.f;
  if (frame_dt > kDtClamp) frame_dt = kDtClamp;
  script_callbacks_tick(jvm,frame_dt);

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
      if(!script_callback_registered(jvm,scroll,28))jvm.invoke(scroll->class_name, "animate", "()V",
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
          if (w.owner == losd && !w.hidden && !w.dead && w.background && w.half_w > 0.4f &&
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
      // userWait(sec) blocks on termSig. LoadingScreen starts SoftTimer,
      // which notifies the dialog after sec, and FlashText during the wait.
      // A click is loadingDialog.osdCommand, the same notify. userWait(-1)
      // starts no timer.
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
          static Object* wait_dlg = nullptr;
          static bool wait_timed = false;
          const float limit = as_float("waitForUserTimeLimit");
          if (wait_screen != screen || wait_gen != wait_user ||
              wait_dlg != dlg) {
            wait_screen = screen;
            wait_gen = wait_user;
            wait_dlg = dlg;
            // Negative limit is userWait(): click only. Zero or more is
            // SoftTimer, which dismisses the plate by itself.
            wait_timed = limit >= 0.f;
            wait_deadline =
                wait_timed
                    ? now + std::chrono::milliseconds(
                               static_cast<int>(limit * 1000.f))
                    : now;
          }
          if ((wait_timed && now >= wait_deadline) || skip) {
            jvm.invoke("java.render.LoadingScreen", "hide", "()V",
                       {Value::make_obj(screen)}, false);
          } else if (dlg) {
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
    }
    // The timed wait can hide the dialog above. Update the presentation
    // state before flushing this frame, including during a section change.
    if (!shown("loadingDialog")) {
      load_seen = nullptr;
      osd_set_loading_dialog(false);
      osd_set_loading_overlay({});
    }
  }

  // Compatibility tick only without a running Java time refresher.
  Value refresher=jvm.get_static("java.game.GameLogic","timeRefresher");
  if(!script_thread_alive(jvm,refresher.tag==ValueTag::Obj?refresher.v.o:nullptr)) {
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

  dispatch_mechanic_pointer(jvm);
  spin_mechanic_inventory(jvm, frame_dt);
  if(!script_thread_alive(jvm,g_mechanic_thread))apply_mechanic_infoline(jvm, g_spinning_mechanic);
  part_physics_advance(dt < 0.0 ? 0.0 : dt);

  // Last write before the view is built. The chase camera must not cover
  // the showroom orbit, and nothing after the earlier spin may put it back.
  spin_car_select_camera(sections.actual());

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
    if (g_spinning_mechanic == tgt) {
      g_spinning_mechanic = nullptr;
        g_mechanic_thread = nullptr;
      g_mechanic_hover = nullptr;
      g_inventory_spin = 0.f;
    }
    return;
  }
  g_spinning_mechanic = tgt;
  g_mechanic_thread = thread;
  // Same moment the script thread would take its first 20 ms tick.
  apply_mechanic_infoline(jvm, tgt);
}


void mainloop_visit_gc_roots(const GcRootVisitor& visit) {
  visit(g_spinning_mechanic); visit(g_mechanic_hover); visit(g_mechanic_thread);
}
}  // namespace slrr
