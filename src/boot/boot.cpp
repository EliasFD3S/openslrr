#include "slrr/boot.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/script_threads.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/natives_boot.hpp"
#include "slrr/object.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/section_machine.hpp"
#include "slrr/video_fmv.hpp"
#include "slrr_version.h"

#include <cstdio>
#include <exception>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace slrr {

void start_init(Jvm& jvm);

namespace {

int run_tree_smokes(Jvm& jvm, SectionMachine& sections) {
  if (!jvm.load_class("java.lang.System")) return 4;
  if (!jvm.load_class("java.lang.Object")) return 4;
  if (!jvm.load_class("java.lang.Integer")) return 4;

  const Value bn =
      jvm.invoke("java.lang.System", "buildNumber", "()I", {}, true);
  if (bn.tag != ValueTag::Int || bn.v.i != 606) {
    std::fprintf(stderr, "[boot] FAIL buildNumber\n");
    return 5;
  }

  Object* a = jvm.new_object("java.lang.Object");
  Object* b = jvm.new_object("java.lang.Object");
  Value eq_self = jvm.invoke(
      "java.lang.Object", "equals", "(Ljava.lang.Object;)I",
      {Value::make_obj(a), Value::make_obj(a)}, false);
  Value eq_other = jvm.invoke(
      "java.lang.Object", "equals", "(Ljava.lang.Object;)I",
      {Value::make_obj(a), Value::make_obj(b)}, false);
  Value eq_null = jvm.invoke(
      "java.lang.Object", "equals", "(Ljava.lang.Object;)I",
      {Value::make_obj(a), Value::make_obj(nullptr)}, false);

  const bool eq_ok = eq_self.tag == ValueTag::Int && eq_self.v.i == 1 &&
                     eq_other.tag == ValueTag::Int && eq_other.v.i == 0 &&
                     eq_null.tag == ValueTag::Int && eq_null.v.i == 0;
  std::printf("[boot] Object.equals self=%d other=%d null=%d %s\n",
              eq_self.tag == ValueTag::Int ? eq_self.v.i : -1,
              eq_other.tag == ValueTag::Int ? eq_other.v.i : -1,
              eq_null.tag == ValueTag::Int ? eq_null.v.i : -1,
              eq_ok ? "ok=1" : "ok=0");
  if (!eq_ok) return 6;

  Object* iv = jvm.new_object("java.lang.Integer");
  iv->fields["number"] = Value::make_int(42);
  Value ivv = jvm.invoke("java.lang.Integer", "intValue", "()I",
                         {Value::make_obj(iv)}, false);
  const bool iv_ok = ivv.tag == ValueTag::Int && ivv.v.i == 42;
  std::printf("[boot] Integer.intValue %d %s\n",
              ivv.tag == ValueTag::Int ? ivv.v.i : -1, iv_ok ? "ok=1" : "ok=0");
  if (!iv_ok) return 7;

  if (!jvm.load_class("java.game.SplashScreen")) return 4;
  if (!jvm.load_class("java.game.MainMenu")) return 4;
  if (!jvm.load_class("java.game.GameLogic")) return 4;
  if (!jvm.load_class("java.util.resource.ResourceRef")) return 4;

  // OptionsDialog/MainMenuDialog ctor needs GameLogic.player.controller.
  sections.seed_player("Player");

  // Splash.enter TREE via SectionMachine — ctor runs GameType.createNativeInstance.
  Object* pic = jvm.new_object("java.util.resource.ResourceRef");
  pic->fields["id"] = Value::make_int(0x0005);
  Object* splash = jvm.new_object("java.game.SplashScreen");
  jvm.invoke("java.game.SplashScreen", "<init>",
             "(Ljava.util.resource.ResourceRef;)V",
             {Value::make_obj(splash), Value::make_obj(pic)}, false);
  sections.change_active_section(splash);
  const bool enter_ok =
      sections.actual() == splash && splash->fields.count("osd");
  std::printf("[boot] SplashScreen.enter TREE osd=%d %s\n",
              splash->fields.count("osd") ? 1 : 0, enter_ok ? "ok=1" : "ok=0");
  if (!enter_ok) return 8;

  // osdCommand → MainMenu: Splash.exit TREE + MainMenu.enter TREE (mmd.show TREE).
  jvm.invoke("java.game.SplashScreen", "osdCommand", "(I)V",
             {Value::make_obj(splash), Value::make_int(35)}, false);
  Object* actual = jvm.get_static("java.game.GameLogic", "actualState").v.o;
  Object* mmd = nullptr;
  if (actual) {
    auto mit = actual->fields.find("mmd");
    if (mit != actual->fields.end() && mit->second.tag == ValueTag::Obj)
      mmd = mit->second.v.o;
  }
  const int32_t n_widgets = osd_widget_count();
  const bool menu_ok =
      actual && actual->class_name.find("MainMenu") != std::string::npos && mmd &&
      mmd->fields.count("osd") && n_widgets > 0;
  std::printf("[boot] Splash→MainMenu mmd=%d osd=%d widgets=%d texts=%zu "
              "section=%s %s\n",
              mmd ? 1 : 0, mmd && mmd->fields.count("osd") ? 1 : 0, n_widgets,
              slrr_osd_texts().size(),
              actual ? actual->class_name.c_str() : "(null)",
              menu_ok ? "ok=1" : "ok=0");
  std::fflush(stdout);
  if (!menu_ok) return 9;

  std::printf("[boot] MainMenu.enter ok=1\n");
  std::printf("[boot] probe ok classes=%d buildNumber=%d tree=1\n",
              jvm.classes_loaded(), bn.v.i);
  std::fflush(stdout);
  start_init(jvm);
  return 0;
}

}  // namespace

// Stock entry: Init(int) → Sound, Frontend, controllers, GameLogic.
// The first native with no host binding aborts this chain.
void start_init(Jvm& jvm) {
  if (!jvm.load_class("java.game.Init")) {
    std::fprintf(stderr, "[boot] Init class missing\n");
    return;
  }
  Object* init = jvm.new_object("java.game.Init");
  jvm.clear_unbound();
  jvm.set_halt_on_unbound(true);
  try {
    jvm.invoke("java.game.Init", "<init>", "(I)V",
               {Value::make_obj(init), Value::make_int(0)}, false);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[boot] Init aborted: %s\n", e.what());
  }
  jvm.set_halt_on_unbound(false);
  if (!jvm.unbound_native().empty())
    std::printf("[boot] Init stopped: unbound %s\n",
                jvm.unbound_native().c_str());
  else
    std::printf("[boot] Init.<init> returned\n");
  {
    Value av = jvm.get_static("java.game.GameLogic", "actualState");
    const char* name = "(none)";
    if (av.tag == ValueTag::Obj && av.v.o) name = av.v.o->class_name.c_str();
    std::printf("[boot] Init actualState=%s\n", name);
  }
  std::fflush(stdout);
}

int boot_probe(Jvm& jvm, const BootOptions& opt) {
  jvm.set_game_root(opt.game_root);
  slrr_font_set_game_root(opt.game_root);
  video_fmv_set_game_root(opt.game_root);
  if (!jvm.bootstrap_classpath()) return 2;

  RpkIndex rpk;
  rpk.set_root(opt.game_root);
  SectionMachine sections(&jvm);
  register_boot_natives(jvm, rpk, sections);

  const int32_t opened = rpk.open_all();
  const RpkPack* fe_pack = rpk.pack_by_path("frontend.rpk");
  const RpkPack* garage_pack = rpk.pack_by_path("misc\\garage.rpk");
  const RpkPack* mwm_pack = rpk.pack_by_path("MW_Mod.rpk");
  const int32_t fe = fe_pack ? fe_pack->pack_id : 0;
  std::printf("[boot] rpk open_all=%d packs=%d entries=%d frontend=%d "
              "garage=%d MW_Mod=%d\n",
              opened, rpk.pack_count(), rpk.total_entries(), fe,
              garage_pack ? garage_pack->pack_id : 0,
              mwm_pack ? mwm_pack->pack_id : 0);

  // ResourceRef.resolve smoke: frontend local 0x011D (street button).
  Object* rr = jvm.new_object("java.util.resource.ResourceRef");
  rr->fields["id"] = Value::make_int(
      static_cast<int32_t>(RpkIndex::make_full_id(fe, 0x011D)));
  jvm.invoke("java.util.resource.ResourceRef", "load", "()V",
             {Value::make_obj(rr)}, false);
  const int loaded =
      (rr->fields.count("loaded") && rr->fields["loaded"].tag == ValueTag::Int)
          ? rr->fields["loaded"].v.i
          : 0;
  std::string tex;
  if (rr->fields.count("texture") && rr->fields["texture"].tag == ValueTag::Obj &&
      rr->fields["texture"].v.o)
    tex = rr->fields["texture"].v.o->str;
  std::printf("[boot] ResourceRef frontend:0x011D loaded=%d alias=%s tex=%s\n",
              loaded, rr->str.c_str(), tex.c_str());

  const ScxMeshInfo scx =
      parse_scx_file(opt.game_root + "/misc/garage/g1/garazs_01.scx");
  std::printf("[boot] SCX garazs_01 v=%u groups=%d verts=%d tris=%d ok=%d %s\n",
              scx.version, scx.groups, scx.vertices, scx.triangles,
              scx.ok ? 1 : 0, scx.error.c_str());

  return run_tree_smokes(jvm, sections);
}

int boot_game(Jvm& jvm, IRenderBackend& render, const BootOptions& opt) {
  jvm.set_game_root(opt.game_root);
  if (!jvm.bootstrap_classpath()) return 2;

  host_sfx_set_game_root(opt.game_root);
  host_music_set_root(opt.game_root);
  host_cursor_set_search_roots(opt.game_root);
  slrr_font_set_game_root(opt.game_root);
  video_fmv_set_game_root(opt.game_root);

  RpkIndex rpk;
  rpk.set_root(opt.game_root);
  SectionMachine sections(&jvm);
  register_boot_natives(jvm, rpk, sections);

  host_game_options_load(jvm);

  const int32_t opened = rpk.open_all();
  const RpkPack* fe_found = rpk.pack_by_path("frontend.rpk");
  const RpkPack* garage_found = rpk.pack_by_path("misc\\garage.rpk");
  const RpkPack* mwm_found = rpk.pack_by_path("MW_Mod.rpk");
  const int32_t garage_pack = garage_found ? garage_found->pack_id : 0;
  std::printf("[boot] rpk open_all=%d packs=%d entries=%d frontend=%d "
              "garage=%d MW_Mod=%d\n",
              opened, rpk.pack_count(), rpk.total_entries(),
              fe_found ? fe_found->pack_id : 0, garage_pack,
              mwm_found ? mwm_found->pack_id : 0);

  // Gadget.HIGHLIGHT_NORMAL = frontend:0x012B (icon_highlite).
  {
    std::string hl = rpk.texture_path_for_id(0x012B);
    if (hl.empty())
      hl = opt.game_root + "\\frontend\\textures\\buttons\\Info_holder.ptx";
    osd_set_highlight_path(std::move(hl));
    osd_set_disabled_highlight_path(rpk.texture_path_for_id(0x013c));
  }

  if (!jvm.load_class("java.lang.System")) return 4;
  const Value bn =
      jvm.invoke("java.lang.System", "buildNumber", "()I", {}, true);
  if (bn.tag != ValueTag::Int || bn.v.i != 606) {
    std::fprintf(stderr, "[boot] FAIL buildNumber\n");
    return 5;
  }

  const bool want_window =
      opt.window || opt.render == RenderBackendKind::Bgfx || opt.auto_new;
  if (want_window) {
    if (!render.open(800, 600, "OpenSLRR - " SLRR_VERSION)) {
      std::fprintf(stderr, "[boot] render.open failed (%s)\n", render.name());
      return 3;
    }
    osd_set_framebuffer(render.width(), render.height());
    host_set_render_callbacks([&render]() {
      if (!render.ready() || !bgfx_osd_draw_enabled(render)) return;
      // Both stock natives present seven times. Loads are synchronous here;
      // repeated frames also drain deferred bgfx resource destruction.
      for (int i = 0; i < 7; ++i) render.flush();
    }, [&render](const std::string& path) { return render.request_screenshot(path); });
    // Click → LoadingScreen.show presents this plate before the rest of the
    // handler (garage enter, track load). Skipped until the splash is up,
    // so Init's own show() cannot flash the clear color.
    host_set_loading_present([&jvm, &render]() {
      if (!render.ready() || !bgfx_osd_draw_enabled(render)) return;
      std::string pic = jvm.game_root();
      if (!pic.empty() && pic.back() != '\\' && pic.back() != '/')
        pic.push_back('\\');
      pic += "frontend\\textures\\Loading.dds";
      osd_set_loading_overlay(pic);
      render.pump(0);
      render.flush();
    });
    // HUD text positions assume Frontend.setFonts for this resolution —
    // SLII17 at 800×600 overflows the garage header (stock uses SLII11).
    frontend_set_fonts(jvm, render.width(), render.height());
    // Live resize: stock Frontend.rebuild() — bump SLII11↔17↔24 with video_y.
    osd_set_framebuffer_listener([&jvm](int32_t w, int32_t h) {
      frontend_set_fonts(jvm, w, h);
    });
    // Stock Engine_boot FMV intros (Activision → Invictus). ESC skips.
    // --no-wait / smoke: cap a few frames so CI does not block on AVI.
    {
      // Probe already built the menu OSD. Drop it so intros are just the movie.
      osd_clear_widgets();
      slrr_text_clear_all();
      const int32_t cap = (opt.max_frames > 0) ? 2 : 0;
      if(opt.spawn_car.empty())video_fmv_play_boot_intros(cap, [&render]() {
        render.pump(0);
        render.flush();
      });
    }
  }

  // Stock entry is Init(int), which ends in changeActiveSection(SplashScreen).
  // Do not build a host splash on top of that.
  {
    const std::string pname =
        opt.player_name.empty() ? "Player" : opt.player_name;
    sections.seed_player(pname);
    jvm.set_static("java.game.GameLogic", "_autoNewPlayerName",
                   Value::make_obj(jvm.new_string(pname)));
    start_init(jvm);
    Value gv = jvm.get_static("java.game.GameLogic", "garage");
    if (gv.tag == ValueTag::Obj && gv.v.o) {
      sections.set_garage(gv.v.o);
    } else {
      Object* garage = jvm.new_object("java.game.Garage");
      jvm.invoke("java.game.Garage", "<init>", "()V",
                 {Value::make_obj(garage)}, false);
      sections.set_garage(garage);
    }
    Value av = jvm.get_static("java.game.GameLogic", "actualState");
    if (av.tag == ValueTag::Obj && av.v.o && sections.actual() != av.v.o)
      sections.change_active_section(av.v.o);
    std::printf("[boot] after Init section=%s\n",
                sections.actual_name().c_str());
    if (want_window) {
      bgfx_set_osd_overlay(render, true);
      mainloop_sync_section_render(render, sections);
      if (render.ready()) {
        render.pump(0);
        render.flush();
      }
    }
  }

  if(!opt.spawn_car.empty()) {
    auto colon=opt.spawn_car.find_last_of(':');
    const RpkPack* pack=colon==std::string::npos?nullptr:rpk.pack_by_name(opt.spawn_car.substr(0,colon));
    uint32_t local=0;
    try {size_t used=0;std::string value=opt.spawn_car.substr(colon+1);unsigned long id=std::stoul(value,&used,0);if(used==value.size()&&id>0&&id<=0xffff)local=static_cast<uint32_t>(id);}catch(const std::exception&){}
    Object* player=sections.seed_player(opt.player_name);
    Object* car=pack&&local?vehicle_spawn_stock(jvm,rpk,player,RpkIndex::make_full_id(pack->pack_id,local)):nullptr;
    if(!car){std::fprintf(stderr,"[boot] cannot spawn car %s\n",opt.spawn_car.c_str());if(render.ready())render.close();return 11;}
    player->fields["car"]=Value::make_obj(car);
    // A development session enters the garage directly; it never submits
    // the NEW CAREER dialog or writes a save as a side effect of spawning.
    sections.change_active_section(sections.garage());
    std::printf("[boot] spawned %s parts=%zu\n",opt.spawn_car.c_str(),part_installed_tree(car).size());
  } else if (opt.auto_new) {
    std::fprintf(stderr,
                 "[boot] WARN --auto-new bypasses Splash→MainMenu (stock "
                 "loop). Use --game without --auto-new for Soft path.\n");
    if (!sections.auto_new_career(opt.player_name)) {
      std::fprintf(stderr, "[boot] FAIL auto-new → Garage\n");
      if (render.ready()) render.close();
      return 10;
    }
  }

  // Garage SCX preload (used after NEW CAREER). Flush so splash stays visible.
  bgfx_clear_scene(render);
  int32_t scx_verts = 0;
  int32_t scx_tris = 0;
  const std::vector<uint32_t> garage_renders =
      rpk.garage_render_ids(garage_pack, 0);
  int32_t load_i = 0;
  for (uint32_t rid : garage_renders) {
    RpkIndex::ModelRef model = rpk.model_for_resource(garage_pack, rid);
    if (!model.ok || model.source.empty()) continue;
    std::string path = model.source;
    for (char& c : path)
      if (c == '/') c = '\\';
    if (path.size() > 4) {
      std::string ext = path.substr(path.size() - 4);
      for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext == ".scy") path = path.substr(0, path.size() - 4) + ".scx";
    }
    const std::string abs = opt.game_root + "\\" + path;
    bgfx_try_scx_smoke(render, abs);
    ScxGeometry geo = load_scx_geometry(abs);
    geo.textures = std::move(model.textures);
    std::printf(
        "[boot] RPK garage render 0x%x '%s' v=%u groups=%d verts=%d "
        "tris=%d tex=%zu ok=%d\n",
        rid, model.alias.c_str(), geo.info.version, geo.info.groups,
        geo.info.vertices, geo.info.triangles, geo.textures.size(),
        geo.info.ok ? 1 : 0);
    if (!geo.info.ok) continue;
    scx_verts += geo.info.vertices;
    scx_tris += geo.info.triangles;
    scene_instances_add_static_mesh(geo);
    bgfx_add_scx_geometry(render, std::move(geo));
    if (want_window && render.ready() && (++load_i % 2) == 0) {
      mainloop_sync_section_render(render, sections);
      render.pump(0);
      render.flush();
    }
  }
  if (garage_renders.empty()) {
    std::fprintf(stderr,
                 "[boot] WARN no g1 renders in garage.rpk — fallback SCX\n");
    const std::string garage_scx =
        opt.game_root + "/misc/garage/g1/garazs_01.scx";
    ScxGeometry scx_geo = load_scx_geometry(garage_scx);
    scx_geo.textures = rpk.textures_for_scx(garage_scx);
    scx_verts = scx_geo.info.vertices;
    scx_tris = scx_geo.info.triangles;
    if (scx_geo.info.ok) {
      scene_instances_add_static_mesh(scx_geo);
      bgfx_set_scx_geometry(render, std::move(scx_geo));
    }
  }

  // Garage preload can eat the Splash 3s timer — restart it so MainLoop
  // actually shows the plate for ~3s (stock: splash after intros, then menu).
  if (want_window && !opt.auto_new && sections.actual() &&
      sections.actual()->class_name.find("Splash") != std::string::npos) {
    Object* splash = sections.actual();
    jvm.invoke("java.game.SplashScreen", "removeAllTimers", "()V",
               {Value::make_obj(splash)}, false);
    jvm.invoke("java.game.SplashScreen", "addTimer", "(FI)V",
               {Value::make_obj(splash), Value::make_float(3.f),
                Value::make_int(1)},
               false);
    std::printf("[boot] SplashScreen timer re-armed after garage preload\n");
  }

  if (opt.auto_cmd != 0 && sections.actual() &&
      sections.actual()->class_name.find("Garage") != std::string::npos) {
    jvm.invoke("java.io.Input", "queueAxis", "(IF)V",
               {Value::make_int(35), Value::make_float(1.f)}, true);
    std::printf("[boot] auto-cmd Garage.osdCommand(%d)\n", opt.auto_cmd);
    jvm.invoke("java.game.Garage", "osdCommand", "(I)V",
               {Value::make_obj(sections.actual()),
                Value::make_int(opt.auto_cmd)},
               false);
  }

  int32_t osd_n = osd_widget_count();
  Object* garage_obj = sections.garage();
  if (!garage_obj) garage_obj = sections.actual();
  if (garage_obj) {
    auto it = garage_obj->fields.find("osdButtonCount");
    if (it != garage_obj->fields.end() && it->second.tag == ValueTag::Int)
      osd_n = it->second.v.i;
    if (osd_n <= 0) osd_n = osd_widget_count();
  }
  const bool in_garage =
      sections.actual() &&
      sections.actual()->class_name.find("Garage") != std::string::npos;
  bgfx_set_garage_viewport(render, in_garage, osd_n);
  if (!in_garage) bgfx_set_osd_overlay(render, true);

  // LBUTTON → the handler stored on the button. osdCommand is the TREE.
  // Object.wait (Dialog.display) pumps the same frame until notify.
  host_set_script_pump([&jvm, &render, &sections]() -> bool {
    render.pump(16);
    if (render.quit_requested()) {
      std::printf("[boot] script pump stop window\n");
      return false;
    }
    mainloop_sync_section_render(render, sections);
    const MainLoopFrameResult fr =
        mainloop_frame(jvm, render, sections, 1.f / 60.f);
#ifdef _WIN32
    Sleep(1);
#endif
    if (fr.quit || render.quit_requested()) {
      std::printf("[boot] script pump stop quit=%d window=%d section=%s\n",
                  fr.quit ? 1 : 0, render.quit_requested() ? 1 : 0,
                  sections.actual_name().c_str());
      return false;
    }
    return true;
  });
  osd_set_click_handler([&jvm, &sections, &render](const OsdWidget& w) {
    if (video_fmv_block_osd()) return;
    sections.dispatch_osd_command(w.handler, w.cmd);
    mainloop_adopt_section(jvm, sections);
    mainloop_sync_section_render(render, sections);
  });

  std::printf(
      "[boot] render=%s window=%d auto_new=%d auto_cmd=%d section=%s osd=%d "
      "scx_verts=%d\n",
      render.name(), want_window ? 1 : 0, opt.auto_new ? 1 : 0, opt.auto_cmd,
      sections.actual_name().c_str(), osd_n, scx_verts);
      (void)scx_tris;

  int32_t frames = 0;
  if (render.ready()) {
    const bool until_quit = opt.max_frames <= 0;
    if (until_quit)
      std::printf("[boot] Soft MainLoop until window close\n");
#ifdef _WIN32
    unsigned last_ms = GetTickCount();
#endif
    while (until_quit || frames < opt.max_frames) {
      // Soft PollWindowQuit adjacency — pump before Input/sim like stock
      // neighbors (Input_tick still inside mainloop_frame).
      render.pump(16);
#ifdef _WIN32
      const unsigned now_ms = GetTickCount();
      float wall_dt = 0.f;
      if (last_ms != 0)
        wall_dt = static_cast<float>(now_ms - last_ms) * 0.001f;
      last_ms = now_ms;
#else
      float wall_dt = 1.f / 60.f;
#endif
      mainloop_sync_section_render(render, sections);
      MainLoopFrameResult fr =
          mainloop_frame(jvm, render, sections, wall_dt);
      jvm.gc_safe_point();
      ++frames;
      if (fr.quit) break;
      if (until_quit) {
#ifdef _WIN32
        Sleep(1);
#endif
      }
    }
    host_set_script_pump(nullptr);
    script_threads_shutdown(jvm);
    video_fmv_close();
    host_game_options_update(jvm, true);
    host_music_stop();
    render.close();
    host_set_render_callbacks({}, {});
  }
  std::printf("[boot] game exit=0 frames=%d section=%s\n", frames,
              sections.actual_name().c_str());
  return 0;
}

}  // namespace slrr
