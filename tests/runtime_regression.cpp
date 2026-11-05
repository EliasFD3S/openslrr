#include "slrr/gameref.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/jvm.hpp"
#include "slrr/mainloop.hpp"
#include "slrr/natives_boot.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unordered_set>
using namespace slrr;
std::string integration_root;
int run_sfx_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine s(&j);
  register_boot_natives(j, r, s);
  r.open_all();
  host_sfx_set_game_root(root);
  Object *sfx = j.new_object("java.util.resource.SfxRef");
  auto rid = j.get_static("java.game.GameLogic", "SFX_ENTERGARAGE");
  check(rid.tag == ValueTag::Int && rid.v.i != 0,
        "garage resource constant resolved");
  j.invoke_by_name(sfx->class_name, "<init>", {Value::make_obj(sfx), rid},
                   false);
  auto played =
      j.invoke_by_name(sfx->class_name, "play", {Value::make_obj(sfx)}, false);
  int garage = played.tag == ValueTag::Int ? played.v.i : 0;
  check(garage > 0 && host_sfx_is_playing(garage),
        "real SfxRef.play starts garage sound through TUFA and RPK");
  host_sfx_play(HostSfx::MenuMove);
  check(host_sfx_is_playing(garage),
        "menu hover does not interrupt garage sound");
  j.invoke_by_name(sfx->class_name, "stop",
                   {Value::make_obj(sfx), Value::make_int(garage)}, false);
  check(!host_sfx_is_playing(garage), "SfxRef.stop stops requested instance");
  for (auto &e :
       std::filesystem::directory_iterator(root + "/frontend/sounds")) {
    int id = host_sfx_play_file(e.path().string(), 1.f, 0.f);
    check(id > 0, e.path().filename().string().c_str());
    host_sfx_stop(id);
  }
  int loop = host_sfx_play_file(root + "/frontend/sounds/entergarage.wav", 1.2f,
                                0.f, true);
  check(loop > 0 && host_sfx_is_playing(loop), "loop/pitch/volume accepted");
  check(host_sfx_play_file(root + "/frontend/sounds/entergarage.wav", 1.f, 0.f,
                           false, loop) == loop,
        "existing live instance reused");
  host_sfx_stop(loop);
  check(!host_sfx_is_playing(loop), "loop stops without stopping other voices");
  check(host_sfx_play_file(root + "/missing.wav") == 0,
        "missing sound returns failure");
  host_sfx_set_volume(0.f);
  host_sfx_set_volume(1.f);
  return failures ? 1 : 0;
}

int run_regression_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine sections(&j);
  register_boot_natives(j, r, sections);
  r.open_all();
  Object *player = sections.seed_player("UIProbe");
  Object *garage = j.new_object("java.game.Garage");
  j.invoke_by_name(garage->class_name, "<init>", {Value::make_obj(garage)},
                   false);
  j.invoke_by_name(garage->class_name, "enter",
                   {Value::make_obj(garage), Value::make_obj(nullptr)}, false);
  auto mech = garage->fields["mechanic"];
  check(mech.tag == ValueTag::Obj && mech.v.o,
        "Garage retains Mechanic object after static flags update");
  check(garage->fields["mode"].tag == ValueTag::Int &&
            garage->fields["mode"].v.i == 1,
        "Garage enters mechanic mode");
  bool bottom = false;
  for (auto &w : osd_widgets())
    if (!w.hidden && w.tip == "Repair Part")
      bottom = true;
  check(bottom, "Mechanic bottom controls visible");
  j.invoke_by_name(garage->class_name, "osdCommand",
                   {Value::make_obj(garage), Value::make_int(124)}, false);
  check(garage->fields["mode"].tag == ValueTag::Int &&
            garage->fields["mode"].v.i == 4,
        "Tune click enters MODE_TUNE");
  auto money = j.get_static("java.render.Frontend", "SFX_MONEY");
  printf("MONEY RID=%x\n", money.v.i);
  check(money.tag == ValueTag::Int && (money.v.i & 0xffff0000),
        "early Frontend resource qualified after packs open");
  Object *effect = j.new_object("java.util.resource.SfxRef");
  j.invoke_by_name(effect->class_name, "<init>",
                   {Value::make_obj(effect), money}, false);
  auto voice = j.invoke_by_name(effect->class_name, "play",
                                {Value::make_obj(effect)}, false);
  check(voice.tag == ValueTag::Int && host_sfx_is_playing(voice.v.i),
        "Frontend.SFX_MONEY plays through real script resource");
  host_sfx_stop(voice.v.i);
  for (const char *avi : {"Activision.avi", "Invictus.avi", "Prime.avi"}) {
    check(video_fmv_open((integration_root + "/Data/Fmv/" + avi).c_str(), 1,
                         0) == 0,
          "intro opens");
    if (std::string(avi) != "Prime.avi")
      check(video_fmv_audio_stream_count() == 1 && video_fmv_audio_running(),
            "real intro audio output is connected and running");
    video_fmv_close();
    check(video_fmv_audio_stream_count() == 0 && !video_fmv_audio_running(),
          "closing FMV releases its audio output");
  }
  Object *codes = j.new_object("java.lang.Object");
  codes->is_array = true;
  for (const char *code : {"mfunfspd", "cfhgpsnpofz", "gbtuqspnpujpo",
                           "jmjlfdifbujoh", "ujnfnbdijof"})
    codes->slots.push_back(Value::make_obj(j.new_string(code)));
  j.set_static("java.game.GameLogic", "kismajom", Value::make_obj(codes));
  for (char ch : std::string("ilikecheating"))
    host_input_char(ch);
  j.invoke_by_name(garage->class_name, "osdCommand",
                   {Value::make_obj(garage), Value::make_int(124)}, false);
  auto cheats = j.get_static("java.game.GameLogic", "cheats");
  check(cheats.tag == ValueTag::Int && cheats.v.i == 1,
        "Tune switch selects case 3 and skips preceding cases");
  for (char ch : std::string("timemachine"))
    host_input_char(ch);
  j.invoke_by_name(garage->class_name, "osdCommand",
                   {Value::make_obj(garage), Value::make_int(124)}, false);
  auto day = j.get_static("java.game.GameLogic", "day");
  check(day.tag == ValueTag::Int && day.v.i == 180,
        "Tune switch selects final case 4");
  return failures ? 1 : 0;
}

int run_gap_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine s(&j);
  register_boot_natives(j, r, s);
  r.open_all();
  frontend_set_fonts(j, 800, 600);
  Object *osd = j.new_object("java.render.Osd");
  j.invoke_by_name(osd->class_name, "<init>",
                   {Value::make_obj(osd), Value::make_float(1.f),
                    Value::make_float(0.f), Value::make_int(10)},
                   false);
  auto font = j.get_static("java.render.Frontend", "smallFont");
  auto makebox = [&](const char *txt) {
    return j.invoke_by_name(osd->class_name, "createTextBox",
                            {Value::make_obj(osd),
                             Value::make_obj(j.new_string(txt)), font,
                             Value::make_int(1), Value::make_float(.1f),
                             Value::make_float(.1f), Value::make_float(.85f)},
                            false);
  };
  auto size = j.invoke_by_name("java.render.Text", "getFontSize", {font}, true);
  printf("METRIC size=%d id=%x constant=%x\n", size.v.i,
         font.v.o->fields["id"].v.i,
         j.get_static("java.render.Text", "RID_SLII11").v.i);
  auto value =
      makebox("OLD description that wraps over several lines so every old line "
              "must be removed when another track is selected.");
  Object *box = value.tag == ValueTag::Obj ? value.v.o : nullptr;
  check(box != nullptr, "real Osd.createTextBox returns textbox");
  check(slrr_osd_texts().size() > 1,
        "initial description creates multiple rendered lines");
  std::unordered_set<void *> old;
  for (auto &t : slrr_osd_texts())
    old.insert(t.text_key);
  auto change = [&](const char *txt) {
    j.invoke_by_name("java.render.osd.TextBox", "changeText",
                     {Value::make_obj(box),
                      Value::make_obj(txt ? j.new_string(txt) : nullptr)},
                     false);
  };
  change("NEW description");
  bool stale = false;
  for (auto &t : slrr_osd_texts())
    if (old.count(t.text_key))
      stale = true;
  check(!stale, "Text.finalize removes every previous render instance");
  check(slrr_osd_texts().size() == 1 &&
            slrr_osd_texts()[0].text == "NEW description",
        "only replacement description remains");
  for (int i = 0; i < 20; ++i)
    change(i % 2 ? "NEW description" : "Other track");
  check(slrr_osd_texts().size() == 1,
        "20 track changes do not accumulate lines");
  change(nullptr);
  check(slrr_osd_texts().empty(), "null description removes all old lines");
  change("Restored");
  check(slrr_osd_texts().size() == 1,
        "description can be recreated after clearing");
  if (!slrr_osd_texts().empty()) {
    Object *t = static_cast<Object *>(slrr_osd_texts()[0].text_key);
    j.invoke_by_name("java.render.Text", "finalize", {Value::make_obj(t)},
                     false);
    j.invoke_by_name("java.render.Text", "finalize", {Value::make_obj(t)},
                     false);
    check(slrr_osd_texts().empty(), "repeated finalize is harmless");
  }
  change("\n \n \n First line \n Second line");
  const auto &lines = slrr_osd_texts();
  check(lines.size() == 2,
        "leading blank lines produce only two visible lines");
  if (lines.size() == 2) {
    check(std::fabs((lines[1].y - lines[0].y) * 300.f - 12.f) < .1f,
          "small font line spacing is 12 pixels");
    check(std::fabs(lines[0].y - .22f) < .001f,
          "three leading blank lines use 36 pixels");
  }
  slrr_text_clear_all();
  const char *catalog =
      "This is the basic Baiern model, a classical but modren looking 5th "
      "generation CoupeSport. This coupe was meant to be a comfortable sporty "
      "car with good performance. The factory developed this car for more than "
      "4 years, so it is. It's in production since 1997, but this autumn, a "
      "new model will come, so the dealers lowered its price for the summer "
      "until production ends. Because of the incorporation between Baiern Cars "
      "Gmbh and Emer Japanese-German Automobiles it can be easily modified and "
      "tuned with lots of parts from Emer and EMD engines and running gears or "
      "component kits.";
  auto boxed = j.invoke_by_name(
      osd->class_name, "createTextBox",
      {Value::make_obj(osd), Value::make_obj(j.new_string(catalog)), font,
       Value::make_int(1), Value::make_float(0.09f), Value::make_float(0.21f),
       Value::make_float(0.85f), Value::make_obj(nullptr), Value::make_int(10)},
      false);
  check(boxed.tag == ValueTag::Obj && boxed.v.o, "10-line text box is created");
  int nlines = 0;
  bool ellipsis = false;
  for (const auto &t : slrr_osd_texts()) {
    ++nlines;
    if (t.text.find("(...)") != std::string::npos) ellipsis = true;
  }
  check(nlines > 0 && nlines <= 10 && ellipsis,
        "catalog description stops at 10 lines with (...)");
  for (const char *slot : {"smallFont", "mediumFont", "largeFont"}) {
    auto f = j.get_static("java.render.Frontend", slot);
    auto measured =
        j.invoke_by_name("java.render.Text", "getFontSize", {f}, true);
    auto *loaded = slrr_font_get(f.v.o);
    check(loaded && measured.tag == ValueTag::Int &&
              measured.v.i == loaded->px_height,
          slot);
  }
  return failures ? 1 : 0;
}

int run_hud_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine sections(&j);
  register_boot_natives(j, r, sections);
  r.open_all();
  Object *player = sections.seed_player("UIProbe");
  Object *garage = j.new_object("java.game.Garage");
  j.invoke_by_name(garage->class_name, "<init>", {Value::make_obj(garage)},
                   false);
  j.invoke_by_name(garage->class_name, "enter",
                   {Value::make_obj(garage), Value::make_obj(nullptr)}, false);
  auto mech = garage->fields["mechanic"];
  check(mech.tag == ValueTag::Obj && mech.v.o,
        "Garage retains Mechanic object after static flags update");
  check(garage->fields["mode"].tag == ValueTag::Int &&
            garage->fields["mode"].v.i == 1,
        "Garage enters mechanic mode");
  bool bottom = false;
  for (auto &w : osd_widgets())
    if (!w.hidden && w.tip == "Repair Part")
      bottom = true;
  check(bottom, "Mechanic bottom controls visible");
  for (const char *k : {"daytxt", "timetxt"}) {
    auto v = garage->fields[k];
    printf("HUDOBJ %s tag=%d ptr=%p\n", k, (int)v.tag,
           v.tag == ValueTag::Obj ? v.v.o : nullptr);
  }
  for (const char *k : {"dayTxt", "timeTxt"}) {
    auto v = j.get_static("java.game.GameLogic", k);
    printf("HUDSTATIC %s tag=%d ptr=%p\n", k, (int)v.tag,
           v.tag == ValueTag::Obj ? v.v.o : nullptr);
  }
  auto render = create_render_backend(RenderBackendKind::Null);
  sections.adopt_actual(garage);
  j.set_static("java.game.GameLogic", "actualState", Value::make_obj(garage));
  mainloop_frame(j, *render, sections, 1.f / 60.f);
  for (auto &t : slrr_osd_texts())
    if (t.y < -.9f)
      printf("HUDTEXT '%s' ptr=%p visible=%d group=%d\n", t.text.c_str(),
             t.text_key, t.visible, t.group);
  auto published = [&](Object *obj, const char *expected) {
    for (auto &t : slrr_osd_texts())
      if (t.text_key == obj)
        return t.visible && t.text == expected;
    return false;
  };
  check(published(garage->fields["daytxt"].v.o, "1"), "DAY value visible");
  check(published(garage->fields["timetxt"].v.o, "12:00pm"),
        "TIME value visible");
  Object *osd = garage->fields["osd"].v.o;
  auto font = j.get_static("java.render.Frontend", "smallFont");
  auto make = [&](float x) {
    return j.invoke_by_name("java.render.Osd", "createText",
                            {Value::make_obj(osd), Value::make_obj(nullptr),
                             font, Value::make_int(2), Value::make_float(x),
                             Value::make_float(-.98f)},
                            false);
  };
  auto nd = make(.32f), nt = make(.50f);
  j.invoke_by_name("java.game.GameLogic", "setTimerText", {nd, nt}, true);
  mainloop_frame(j, *render, sections, 1.f / 60.f);
  check(published(nd.v.o, "1") && published(nt.v.o, "12:00pm"),
        "recreated HUD initializes without day/minute change");
  for (auto tc : {std::pair<float, const char *>{0.f, "12:00am"},
                  {43140.f, "11:59am"},
                  {43200.f, "12:00pm"},
                  {46860.f, "1:01pm"}}) {
    auto v =
        j.invoke("java.lang.String", "timeToString", "(FI)Ljava.lang.String;",
                 {Value::make_float(tc.first), Value::make_int(0x21)}, true);
    check(v.tag == ValueTag::Obj && v.v.o && v.v.o->str == tc.second,
          tc.second);
  }
  return failures ? 1 : 0;
}

int run_career_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine s(&j);
  register_boot_natives(j, r, s);
  r.open_all();
  host_sfx_set_game_root(root);
  j.load_class("java.game.MainMenu");
  s.seed_player("UIProbe");
  Object *menu = j.new_object("java.game.MainMenuDialog");
  j.invoke_by_name(menu->class_name, "<init>", {Value::make_obj(menu)}, false);
  j.invoke_by_name(menu->class_name, "show", {Value::make_obj(menu)}, false);
  host_set_script_pump([&]() {
    int rows = 0;
    for (const auto &w : osd_widgets())
      if (!w.hidden && w.hit_only && w.handler &&
          w.handler->class_name == "java.game.CarrerFileReqDialog" &&
          w.cmd >= 100 && w.cmd < 105)
        ++rows;
    check(rows == 5,
          "Load Career command reaches modal wait with five visible rows");
    return false;
  });
  j.invoke_by_name(menu->class_name, "osdCommand",
                   {Value::make_obj(menu), Value::make_int(52)}, false);
  Object *d = j.new_object("java.game.CarrerFileReqDialog");
  j.invoke_by_name(d->class_name, "<init>",
                   {Value::make_obj(d), Value::make_obj(nullptr),
                    Value::make_int(3),
                    Value::make_obj(j.new_string("LOAD CAREER")),
                    Value::make_obj(j.new_string("LOAD CAREER")),
                    Value::make_obj(j.new_string("save/carrer/")),
                    Value::make_obj(j.new_string("*")), Value::make_int(2)},
                   false);
  auto list = d->fields["fileNameList"].v.o;
  j.invoke_by_name(
      list->class_name, "addElement",
      {Value::make_obj(list), Value::make_obj(j.new_string("TestCareer"))},
      false);
  j.invoke_by_name(d->class_name, "show", {Value::make_obj(d)}, false);
  auto buttons = d->fields["fileButtons"];
  check(buttons.tag == ValueTag::Obj && buttons.v.o &&
            buttons.v.o->slots.size() == 5,
        "five career buttons created");
  if (buttons.tag == ValueTag::Obj && buttons.v.o)
    for (auto &b : buttons.v.o->slots)
      check(b.tag == ValueTag::Obj && b.v.o &&
                b.v.o->class_name == "java.render.osd.Button",
            "career row contains Button receiver");
  if (buttons.tag == ValueTag::Obj && buttons.v.o) {
    auto first = buttons.v.o->slots[0];
    auto label =
        j.invoke_by_name(first.v.o->class_name, "getLabelText", {first}, false);
    check(label.tag == ValueTag::Obj && label.v.o &&
              label.v.o->str ==
                  j.invoke_by_name(list->class_name, "elementAt",
                                   {Value::make_obj(list), Value::make_int(0)},
                                   false)
                      .v.o->str,
          "saved career name reaches button text");
  }
  video_fmv_close();
  return failures ? 1 : 0;
}

int run_filedialog_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine sections(&j);
  register_boot_natives(j, r, sections);
  r.open_all();
  Object *player = sections.seed_player("UIProbe");
  Object *garage = j.new_object("java.game.Garage");
  j.invoke_by_name(garage->class_name, "<init>", {Value::make_obj(garage)},
                   false);
  j.invoke_by_name(garage->class_name, "enter",
                   {Value::make_obj(garage), Value::make_obj(nullptr)}, false);
  auto mech = garage->fields["mechanic"];
  check(mech.tag == ValueTag::Obj && mech.v.o,
        "Garage retains Mechanic object after static flags update");
  check(garage->fields["mode"].tag == ValueTag::Int &&
            garage->fields["mode"].v.i == 1,
        "Garage enters mechanic mode");
  bool bottom = false;
  for (auto &w : osd_widgets())
    if (!w.hidden && w.tip == "Repair Part")
      bottom = true;
  check(bottom, "Mechanic bottom controls visible");
  Object *dialog = j.new_object("java.game.CarrerFileReqDialog");
  j.invoke_by_name(dialog->class_name, "<init>",
                   {Value::make_obj(dialog), Value::make_obj(nullptr),
                    Value::make_int(0),
                    Value::make_obj(j.new_string("Load career")),
                    Value::make_obj(j.new_string("Load")),
                    Value::make_obj(j.new_string("save/carrer/")),
                    Value::make_obj(j.new_string("*")), Value::make_int(2)},
                   false);
  for (const char *name :
       {"java.game.CarrerFileReqDialog",
        "java.render.osd.dialog.FileRequesterDialog",
        "java.render.osd.dialog.OKCancelDialog",
        "java.render.osd.dialog.TextDialog", "java.render.osd.dialog.Dialog"}) {
    j.load_class(name);
    auto *c = j.find_class(name);
    if (c)
      for (auto &m : c->instance_methods)
        if (c->utf8(m.name) == "<init>")
          printf("CTOR %s %s\n", name, c->utf8(m.descriptor).c_str());
  }
  for (auto &[k, v] : dialog->fields)
    if (k == "osd" || k == "path" || k == "title")
      printf("FIELD %s tag=%d obj=%s\n", k.c_str(), (int)v.tag,
             v.tag == ValueTag::Obj && v.v.o ? v.v.o->class_name.c_str()
                                             : "null");
  j.invoke_by_name(dialog->class_name, "show", {Value::make_obj(dialog)},
                   false);
  auto buttons = dialog->fields["fileButtons"];
  check(buttons.tag == ValueTag::Obj && buttons.v.o &&
            buttons.v.o->slots.size() == 5,
        "file requester creates five buttons");
  if (buttons.tag == ValueTag::Obj && buttons.v.o)
    for (auto &b : buttons.v.o->slots)
      printf("BUTTON %s\n", b.tag == ValueTag::Obj && b.v.o
                                ? b.v.o->class_name.c_str()
                                : "null");
  auto list = dialog->fields["fileNameList"];
  j.invoke_by_name("java.util.Vector", "removeAllElements", {list}, false);
  for (int i = 0; i < 7; i++)
    j.invoke_by_name(
        "java.util.Vector", "addElement",
        {list, Value::make_obj(j.new_string("Career " + std::to_string(i)))},
        false);
  j.invoke_by_name(dialog->class_name, "setButtonNames",
                   {Value::make_obj(dialog)}, false);
  if (buttons.tag == ValueTag::Obj && buttons.v.o)
    for (int i = 0; i < 5; i++) {
      auto b = buttons.v.o->slots[i];
      auto label = j.invoke_by_name("java.render.osd.Button", "getLabelText",
                                    {b}, false);
      printf("LABEL tag=%d text=%s\n", (int)label.tag,
             label.tag == ValueTag::Obj && label.v.o ? label.v.o->str.c_str()
                                                     : "null");
      check(label.tag == ValueTag::Obj && label.v.o &&
                label.v.o->str == "Career " + std::to_string(i),
            "file label updated on actual button");
    }
  j.invoke_by_name(dialog->class_name, "osdCommand",
                   {Value::make_obj(dialog), Value::make_int(202)}, false);
  if (buttons.tag == ValueTag::Obj && buttons.v.o)
    for (int i = 0; i < 5; i++) {
      auto label = j.invoke_by_name("java.render.osd.Button", "getLabelText",
                                    {buttons.v.o->slots[i]}, false);
      check(i < 2 ? (label.tag == ValueTag::Obj && label.v.o &&
                     label.v.o->str == "Career " + std::to_string(i + 5))
                  : (label.tag == ValueTag::Obj &&
                     (!label.v.o || label.v.o->str.empty())),
            "scroll updates labels and clears empty rows");
    }
  return failures ? 1 : 0;
}

int run_scene_script_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine sections(&j);
  register_boot_natives(j, r, sections);
  r.open_all();
  Object *map = j.new_object("java.util.resource.GroundRef");
  auto groundType = j.get_static("java.game.Garage", "RID_GARAGE1");
  j.invoke_by_name(map->class_name, "<init>",
                   {Value::make_obj(map), groundType}, false);
  check(gameref_get(map) != nullptr,
        "original GroundRef constructor creates world node");
  Object *scene = j.new_object("java.game.Garage");
  scene->fields["map"] = Value::make_obj(map);
  scene->fields["internalScene"] = Value::make_int(1);
  scene->fields["MotionB"] = Value::make_int(0);
  for (int config : {0, 7, 14}) {
    auto old = scene->fields.find("skydome");
    if (old != scene->fields.end() && old->second.tag == ValueTag::Obj &&
        old->second.v.o)
      j.invoke_by_name("java.util.resource.ResourceRef", "destroy",
                       {old->second}, false);
    scene->fields["lastConfig"] = Value::make_int(-1);
    j.invoke("java.game.Scene", "addSceneElements", "(I)V",
             {Value::make_obj(scene), Value::make_int(config)}, false);
    auto sky = scene->fields["skydome"];
    check(sky.tag == ValueTag::Obj && sky.v.o, "Scene.java creates skydome");
    scene_instances_select(map, sky.tag == ValueTag::Obj ? sky.v.o : nullptr);
    auto live = scene_instances_collect();
    printf("CONFIG %d INSTANCES %zu\n", config, live.size());
    bool found = false;
    for (auto &i : live)
      if (i.sky) {
        found = true;
        printf("SKY %s TEXTURE %s\n", i.geometry->info.path.c_str(),
               i.geometry->textures.empty() ? "NONE"
                                            : i.geometry->textures[0].c_str());
      }
    check(found, "script skydome reaches render scene");
  }
  scene_instances_set_resources(nullptr);
  return failures ? 1 : 0;
}

int run_icon_probe() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) {
    printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
      ++failures;
  };
  NativeRegistry n;
  Jvm j(&n);
  std::string root = integration_root;
  j.set_game_root(root);
  slrr_font_set_game_root(root);
  j.bootstrap_classpath();
  RpkIndex r;
  r.set_root(root);
  SectionMachine s(&j);
  register_boot_natives(j, r, s);
  r.open_all();
  frontend_set_fonts(j, 800, 600);
  Object *osd = j.new_object("java.render.Osd");
  j.invoke_by_name(osd->class_name, "<init>",
                   {Value::make_obj(osd), Value::make_float(1.f),
                    Value::make_float(0.f), Value::make_int(10)},
                   false);

  auto font = j.get_static("java.render.Frontend", "mediumFont");
  Object *icon = j.new_object("java.util.resource.ResourceRef");
  j.invoke_by_name(icon->class_name, "<init>",
                   {Value::make_obj(icon),
                    Value::make_int(rpk_resolve_resource("frontend", 0x122))},
                   false);
  Object *sty = j.new_object("java.render.osd.Style");
  j.invoke_by_name(sty->class_name, "<init>",
                   {Value::make_obj(sty), Value::make_float(.12f),
                    Value::make_float(.12f), font, Value::make_int(1),
                    Value::make_obj(icon)},
                   false);
  auto b = j.invoke_by_name(osd->class_name, "createButton",
                            {Value::make_obj(osd), Value::make_obj(sty),
                             Value::make_float(0.f), Value::make_float(0.f),
                             Value::make_int(50),
                             Value::make_obj(j.new_string("Buy new cars"))},
                            false);
  Object *btn = b.v.o;
  check(b.tag == ValueTag::Obj && btn, "icon button created");
  j.invoke_by_name(btn->class_name, "disable", {Value::make_obj(btn)}, false);
  j.invoke_by_name(btn->class_name, "focus", {Value::make_obj(btn)}, false);
  int pi = -1;
  for (size_t i = 0; i < osd_widgets().size(); ++i)
    if (!osd_widgets()[i].hidden &&
        (osd_widgets()[i].icon_id & 0xffff) == 0x13c)
      pi = (int)i;
  check(pi >= 0, "disabled focus creates forbidden plate");
  if (pi >= 0) {
    check(osd_widget_at(pi)->highlight_for == btn,
          "focus plate is tied to its gadget");
    printf("DEBUG disabled tag=%d value=%d\n", (int)btn->fields["disabled"].tag,
           btn->fields["disabled"].v.i);
    check(osd_highlight_disabled(*osd_widget_at(pi)),
          "disabled button retains forbidden state");
    j.invoke_by_name(btn->class_name, "enable", {Value::make_obj(btn)}, false);
    check(!osd_highlight_disabled(*osd_widget_at(pi)),
          "enable switches existing plate to orange without refocus");
    check(osd_hit_index(0.f, 0.f) >= 0 &&
              osd_widget_at(osd_hit_index(0.f, 0.f))->button == btn,
          "enabled button remains clickable under focus plate");
    j.invoke_by_name(btn->class_name, "disable", {Value::make_obj(btn)}, false);
    check(osd_highlight_disabled(*osd_widget_at(pi)),
          "disable switches back to forbidden");
    check(osd_hit_index(0.f, 0.f) == -1, "disabled button stays unclickable");
    j.invoke_by_name(btn->class_name, "unfocus", {Value::make_obj(btn)}, false);
    check(osd_widget_at(pi)->hidden, "unfocus removes old plate");
  }
  j.invoke_by_name(btn->class_name, "enable", {Value::make_obj(btn)}, false);
  for (int i = 0; i < 20; ++i) {
    j.invoke_by_name(btn->class_name, "focus", {Value::make_obj(btn)}, false);
    j.invoke_by_name(btn->class_name, "unfocus", {Value::make_obj(btn)}, false);
  }
  check(btn->fields["disabled"].tag == ValueTag::Int &&
            btn->fields["disabled"].v.i == 0,
        "20 hovers preserve enabled integer state");
  return failures ? 1 : 0;
}

int run_paint_inventory() {
  int failures = 0;
  auto check = [&](bool ok, const char *message) {
    std::printf("CHECK %s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok)
      ++failures;
  };
  NativeRegistry natives;
  Jvm j(&natives);
  j.set_game_root(integration_root);
  slrr_font_set_game_root(integration_root);
  j.bootstrap_classpath();
  RpkIndex resources;
  resources.set_root(integration_root);
  SectionMachine sections(&j);
  register_boot_natives(j, resources, sections);
  resources.open_all();
  sections.seed_player("UIProbe");
  Object *garage = j.new_object("java.game.Garage");
  j.invoke_by_name(garage->class_name, "<init>", {Value::make_obj(garage)},
                   false);
  j.invoke_by_name(garage->class_name, "enter",
                   {Value::make_obj(garage), Value::make_obj(nullptr)}, false);
  j.invoke_by_name(garage->class_name, "osdCommand",
                   {Value::make_obj(garage), Value::make_int(118)}, false);
  auto collect = [&] {
    std::vector<SceneInstance> cans;
    for (auto *camera : gameref_active_cameras()) {
      auto *viewport = gameref_get(camera->viewport);
      if (!viewport || !viewport->vp_active ||
          (viewport->vp_w >= .99f && viewport->vp_h >= .99f))
        continue;
      for (const auto &instance :
           scene_instances_collect_root(camera->parent)) {
        auto *state = gameref_get(instance.key);
        if (state && (state->type_id & 0xffff) == 0x21)
          cans.push_back(instance);
      }
    }
    return cans;
  };
  auto cans = collect();
  check(cans.size() == 5, "Painter scripts create five visible 3D paint cans");
  auto colors = [&](const std::vector<SceneInstance> &items) {
    std::vector<uint32_t> out;
    for (const auto &item : items)
      out.push_back(item.color);
    std::sort(out.begin(), out.end());
    return out;
  };
  check(colors(cans) == std::vector<uint32_t>(
                            {0x1d1d1d, 0x505050, 0x7f7f7f, 0xb9b9b9, 0xf5f5f5}),
        "first row uses the original five gray colors");
  if (cans.size() == 5)
    check(std::all_of(cans.begin(), cans.end(),
                      [&](const auto &can) {
                        return can.geometry == cans[0].geometry;
                      }),
          "five cameras share one original spray mesh");
  Object *painter = garage->fields["painter"].v.o;
  j.invoke_by_name(painter->class_name, "osdCommand",
                   {Value::make_obj(painter), Value::make_int(1)}, false);
  check(colors(collect()) ==
            std::vector<uint32_t>(
                {0x250505, 0x530a0a, 0x800f0f, 0xae1515, 0xdc191a}),
        "scroll replaces the gray previews with five red paint cans");
  j.invoke_by_name(garage->class_name, "osdCommand",
                   {Value::make_obj(garage), Value::make_int(124)}, false);
  check(collect().empty(), "leaving Painter removes all inventory previews");
  video_fmv_close();
  return failures ? 1 : 0;
}

int run_vehicle_assembly() {
  int failures=0;
  auto check=[&](bool ok,const char* message){std::printf("CHECK %s %s\n",ok?"PASS":"FAIL",message);if(!ok)++failures;};
  NativeRegistry native;Jvm j(&native);j.set_game_root(integration_root);j.bootstrap_classpath();
  RpkIndex resources;resources.set_root(integration_root);SectionMachine sections(&j);register_boot_natives(j,resources,sections);resources.open_all();
  auto* pack=resources.pack_by_name("cars/racers/Baiern.rpk");if(!pack)return 1;
  Object* world=j.new_object("java.util.resource.Dummy");gameref_upsert(world,GameRefKind::Dummy);gameref_set_flags(world,kWorldTreeRoot);
  uint32_t chassis_id=RpkIndex::make_full_id(pack->pack_id,6),hood_id=RpkIndex::make_full_id(pack->pack_id,0x10d);
  Object* chassis=part_create(j,resources,world,chassis_id,"assembly test");
  check(chassis&&chassis->class_name=="java.game.cars.Baiern_CoupeSport","original chassis class is instantiated");
  if(!chassis)return 1;
  Object* hood=part_create(j,resources,world,hood_id,"hood test");
  check(hood&&part_install(hood,chassis),"hood attaches through compatible original CFG slots");
  auto tree=part_installed_tree(chassis);
  check(tree.size()==2&&tree[1].parent_slot==601&&tree[1].own_slot==601,"attachment keeps both slot identities");
  scene_instances_select(world,nullptr);
  check(scene_instances_collect().size()>=2,"installed body pieces reach actual SCX scene rendering");
  auto* alias=j.new_object("java.util.resource.GameRef");int live=gameref_make_live(chassis);
  j.invoke_by_name(alias->class_name,"<init>",{Value::make_obj(alias),Value::make_int(live)},false);
  check(gameref_get(alias)==gameref_get(chassis)&&live!=static_cast<int>(chassis_id),"live references share an instance without losing its RPK type");
  auto script=j.invoke_by_name(alias->class_name,"getScriptInstance",{Value::make_obj(alias)},false);
  check(script.tag==ValueTag::Obj&&script.v.o==chassis,"reference returns the canonical scripted chassis");
  part_destroy(chassis);check(scene_instances_collect().empty(),"assembly destruction removes its child render instances");
  Object* vehicle=vehicle_spawn_stock(j,resources,world,chassis_id);
  check(vehicle!=nullptr,"stock vehicle factory returns a vehicle");
  if(vehicle){
    auto installed=part_installed_tree(vehicle);std::printf("VEHICLE parts=%zu renders=%zu\n",installed.size(),scene_instances_collect().size());
    check(installed.size()>=70,"original addStockParts assembles body, engine and running gear through slot aliases");
    check(scene_instances_collect().size()>15,"complete vehicle contributes visible part geometry");
    auto instances=scene_instances_collect();
    bool layered=false, masked=false, uv_channels=true;
    for(const auto& instance:instances) {
      const auto& mesh=*instance.geometry;
      uv_channels=uv_channels && mesh.uv2.size()==mesh.uvs.size() && mesh.uv3.size()==mesh.uvs.size();
      for(const auto& group:mesh.groups) {
        layered=layered || (group.mix_index!=0xffff && group.reflection_index!=0xffff);
        masked=masked || group.material_type==0x15;
      }
    }
    check(layered&&masked&&uv_channels,"original SCX preserves mix, masked reflection type and authored UV channels");
    if(auto* colors=resources.pack_by_name("cars.rpk")) {
      auto red=RpkIndex::make_full_id(colors->pack_id,0x2a);
      Object* panel=nullptr;
      for(const auto& p:installed)if(p.resource==hood_id)panel=p.object;
      if(panel)j.invoke_by_name(panel->class_name,"setTexture",{Value::make_obj(panel),Value::make_int(red)},false);
      bool updated=false;
      for(const auto& instance:scene_instances_collect())
        if(instance.paint_slot>=0 && instance.paint_texture==resources.texture_path_for_id(red))updated=true;
      check(panel&&updated,"Part.setTexture replaces the CFG paint slot in the live render instance");
    }
    part_destroy(vehicle);
  }
  return failures?1:0;
}

int run_catalog() {
  NativeRegistry native;
  Jvm j(&native); j.set_game_root(integration_root); j.bootstrap_classpath();
  RpkIndex resources; resources.set_root(integration_root);
  SectionMachine sections(&j); register_boot_natives(j,resources,sections); resources.open_all();
  slrr_font_set_game_root(integration_root);
  auto* player=sections.seed_player("Catalog test");
  auto* pack=resources.pack_by_name("cars/racers/Baiern.rpk");
  auto* vehicle=vehicle_spawn_stock(j,resources,player,RpkIndex::make_full_id(pack->pack_id,6));
  player->fields["car"]=Value::make_obj(vehicle);
  Object* catalog=j.new_object("java.game.Catalog");
  j.invoke_by_name(catalog->class_name,"<init>",{Value::make_obj(catalog)},false);
  j.invoke_by_name(catalog->class_name,"enter",{Value::make_obj(catalog),Value::make_obj(nullptr)},false);
  j.invoke_by_name(catalog->class_name,"osdCommand",{Value::make_obj(catalog),Value::make_int(27)},false);
  for(int i=0;i<8;++i)
    j.invoke_by_name(catalog->class_name,"collectObjectsStep",{Value::make_obj(catalog),Value::make_int(8)},false);
  auto inv=catalog->fields["inventory"];
  auto* inventory=inv.tag==ValueTag::Obj?inv.v.o:nullptr;
  auto items=inventory?inventory->fields["items"]:Value::make_void();
  auto* vector=items.tag==ValueTag::Obj?items.v.o:nullptr;
  auto size=vector?j.invoke_by_name(vector->class_name,"size",{Value::make_obj(vector)},false):Value::make_int(0);
  printf("CATALOG body panels=%d\n",size.tag==ValueTag::Int?size.v.i:0);
  int valid=0;
  for(int i=0;i<4 && vector;++i) {
    auto item=j.invoke_by_name(vector->class_name,"elementAt",{Value::make_obj(vector),Value::make_int(i)},false);
    if(item.tag!=ValueTag::Obj || !item.v.o)continue;
    auto part=j.invoke_by_name(item.v.o->class_name,"getPart",{item},false);
    auto price=j.invoke_by_name(item.v.o->class_name,"getPrice",{item},false);
    printf("PREVIEW %d part=%s priceTag=%d price=%g\n",i,part.tag==ValueTag::Obj&&part.v.o?part.v.o->class_name.c_str():"null",static_cast<int>(price.tag),price.tag==ValueTag::Float?price.v.f:price.tag==ValueTag::Int?static_cast<float>(price.v.i):0);
    if(part.tag==ValueTag::Obj && part.v.o && j.tree_instance_of(part.v.o,"java.game.parts.Part")
       && ((price.tag==ValueTag::Int && price.v.i>0) || (price.tag==ValueTag::Float && price.v.f>0)))++valid;
  }
  printf("CATALOG scripted previews with prices=%d\n",valid);
  return size.tag==ValueTag::Int&&size.v.i>5&&valid==4?0:1;
}

int run_vehicle_physics() {
  int failures=0;
  auto check=[&](bool ok,const char* label){printf("CHECK %s %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures;};
  NativeRegistry natives;Jvm j(&natives);j.set_game_root(integration_root);j.bootstrap_classpath();
  RpkIndex resources;resources.set_root(integration_root);SectionMachine sections(&j);
  register_boot_natives(j,resources,sections);resources.open_all();
  Object* world=j.new_object("java.util.resource.Dummy");gameref_upsert(world,GameRefKind::Dummy);gameref_set_flags(world,kWorldTreeRoot);
  scene_instances_select(world,nullptr);
  ScxGeometry floor;floor.info.ok=true;floor.position_scale=1;
  floor.positions={-10,0,-10,10,0,-10,10,0,10,-10,0,10};floor.indices={0,1,2,0,2,3};
  scene_instances_add_static_mesh(floor);
  const auto* pack=resources.pack_by_name("cars/racers/Baiern.rpk");
  if(!pack)return 1;
  const auto rid=RpkIndex::make_full_id(pack->pack_id,6);
  auto* vehicle=vehicle_spawn_stock(j,resources,world,rid);
  if(!vehicle)return 1;
  gameref_set_matrix(vehicle,0,2,0,.3f,0,0);
  for(int i=0;i<9;++i)part_physics_advance(1.0/60);
  PartPhysicsState falling;part_physics_state(vehicle,falling);
  printf("DYNAMIC air ticks=%llu y=%.6f vy=%.6f mass=%.2f\n",static_cast<unsigned long long>(falling.ticks),falling.height,falling.vertical_speed,falling.mass);
  check(falling.active&&falling.ticks==36&&falling.height<1.95&&falling.vertical_speed<-1,"stock scripted vehicle falls instead of being planted on its first frame");
  for(int i=0;i<480;++i)part_physics_advance(1.0/60);
  PartPhysicsState rest;part_physics_state(vehicle,rest);
  int contacts=0;double force=0;for(size_t i=0;i<4;++i){contacts+=rest.contact[i];force+=rest.force[i];}
  printf("DYNAMIC rest y=%.6f vy=%.6f mass=%.2f contacts=%d force=%.3f lengths=%.6f %.6f %.6f %.6f\n",rest.height,rest.vertical_speed,rest.mass,contacts,force,rest.length[0],rest.length[1],rest.length[2],rest.length[3]);
  check(contacts==4&&std::fabs(rest.vertical_speed)<.04,"original CFG and WheelRef springs support all four stock wheels");
  check(std::fabs(force-rest.mass*9.81)<rest.mass*9.81*.03,"native spring forces balance the assembled CFG body mass");
  auto* node=gameref_get(vehicle);
  check(node&&node->matrix_override&&std::fabs(node->local_matrix[13]-rest.height)<1e-5&&std::fabs(node->pos[1]-rest.height)<1e-5,"physics pose is the rendered pose and the script-visible position");
  Object* preview_root=j.new_object("java.util.resource.Dummy");gameref_upsert(preview_root,GameRefKind::Dummy);gameref_set_flags(preview_root,kWorldTreeRoot);gameref_set_parent(preview_root,world);
  auto* preview=vehicle_spawn_stock(j,resources,preview_root,rid);
  part_physics_advance(1.0/60);PartPhysicsState hidden;part_physics_state(preview,hidden);
  check(!hidden.active,"inventory worlds are excluded from live vehicle simulation");part_destroy(preview);
  part_command(vehicle,"suspend");part_physics_advance(1.0/60);PartPhysicsState paused;part_physics_state(vehicle,paused);
  check(!paused.active&&paused.ticks==rest.ticks+4,"suspended script vehicles do not accrue simulation time");
  part_command(vehicle,"wakeup");gameref_set_matrix(vehicle,0,3,0,-.4f,0,0);part_physics_advance(1.0/240);
  PartPhysicsState reset;part_physics_state(vehicle,reset);
  check(reset.ticks==1&&reset.height>2.99&&reset.vertical_speed<0,"script teleport resets physics once and retains the new frame");
  auto* position=j.new_object("java.lang.Vector3");position->fields["x"]=Value::make_float(0);position->fields["y"]=Value::make_float(4);position->fields["z"]=Value::make_float(0);
  j.invoke_by_name(vehicle->class_name,"setPos",{Value::make_obj(vehicle),Value::make_obj(position)},false);
  part_physics_advance(1.0/240);PartPhysicsState native_move;part_physics_state(vehicle,native_move);
  check(native_move.ticks==1&&native_move.height>3.99,"GameRef.setPos through a Vehicle alias updates the canonical dynamic body");
  // Real garage shell, not an implicit floor plane or the visual car meshes.
  scene_instances_set_resources(&resources);scene_instances_select(world,nullptr);
  const auto* garage=resources.pack_by_name("misc/garage.rpk");int meshes=0;
  if(garage)for(auto id:resources.garage_render_ids(garage->pack_id,0)) {
    auto model=resources.model_for_resource(garage->pack_id,id);auto path=std::filesystem::path(integration_root)/model.source;
    auto ext=path.extension().string();for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(ext==".scy")path.replace_extension(".scx");
    auto geometry=load_scx_geometry(path.string());if(geometry.info.ok){
      scene_instances_add_static_mesh(geometry);++meshes;
      auto* terrain=j.new_object("java.render.RenderRef");
      const uint32_t terrain_id=0xfffe0000u+meshes;
      gameref_upsert(terrain,GameRefKind::RenderRef)->type_id=static_cast<int>(terrain_id);
      gameref_set_parent(terrain,world);
      scene_instances_override_mesh(terrain_id,std::make_shared<ScxGeometry>(std::move(geometry)));
    }
  }
  gameref_set_matrix(vehicle,0,2,0,0,0,0);
  for(int i=0;i<480;++i)part_physics_advance(1.0/60);
  PartPhysicsState actual;part_physics_state(vehicle,actual);contacts=0;for(bool c:actual.contact)contacts+=c;
  printf("DYNAMIC garage meshes=%d contacts=%d y=%.6f vy=%.6f\n",meshes,contacts,actual.height,actual.vertical_speed);
  check(meshes>0&&contacts==4&&std::fabs(actual.vertical_speed)<.05,"stock vehicle settles on the actual preloaded garage triangles");
  gameref_set_matrix(vehicle,0,0,0,0,0,0);part_command(vehicle,"reset");
  for(int i=0;i<240;++i)part_physics_advance(1.0/60);
  PartPhysicsState overlap;part_physics_state(vehicle,overlap);contacts=0;for(bool c:overlap.contact)contacts+=c;
  printf("DYNAMIC initial-overlap contacts=%d y=%.6f vy=%.6f\n",contacts,overlap.height,overlap.vertical_speed);
  check(contacts==4&&std::fabs(overlap.vertical_speed)<.05,"original zero-height garage reset recovers overlap through physical contacts");
  part_destroy(vehicle);
  if(const auto* duhen=resources.pack_by_name("cars/racers/Duhen.rpk")) {
    std::vector<uint32_t> variants{RpkIndex::make_full_id(duhen->pack_id,0x126)};
    for(const auto& entry:duhen->entries) {
      const auto id=RpkIndex::make_full_id(duhen->pack_id,entry.type_id);
      if(resources.payload_prop(id,"native").find("_main_SunStrip_1_5_DVC.cfg")!=std::string::npos)variants.push_back(id);
    }
    check(variants.size()==2,"both Duhen 1.8 and the screenshot's 1.5 variant resolve from RPK");
    for(auto variant:variants) {
    auto* other=vehicle_spawn_stock(j,resources,world,variant);

    check(other!=nullptr,"second vehicle is built from the original Duhen DVC scripts");
    if(other) {
      scene_instances_enable_static_floor(false);
      gameref_set_matrix(other,0,2,0,0,0,0);
      scene_instances_prepare_physics();
      const auto terrain_builds=scene_instances_floor_builds();
      for(int i=0;i<480;++i)part_physics_advance(1.0/60);
      PartPhysicsState state;part_physics_state(other,state);contacts=0;force=0;
      for(size_t i=0;i<4;++i){contacts+=state.contact[i];force+=state.force[i];}
      printf("DYNAMIC Duhen mass=%.3f contacts=%d y=%.6f vy=%.6f support=%.3f\n",state.mass,contacts,state.height,state.vertical_speed,force);
      check(state.active&&contacts==4&&std::fabs(state.vertical_speed)<.05,"Duhen uses its own native springs to settle on the real garage");
      check(std::fabs(force-state.mass*9.81)<state.mass*9.81*.03,"Duhen load is not replaced by Baiern tuning");
      const double front_track=std::fabs(state.wheel_center[0][0]-state.wheel_center[1][0]);
      const double rear_track=std::fabs(state.wheel_center[2][0]-state.wheel_center[3][0]);
      printf("DYNAMIC Duhen front-track=%.6f rear-track=%.6f\n",front_track,rear_track);
      auto tree=part_installed_tree(other);
      bool belt=false;
      for(const auto& installed:tree)
        if(installed.object->class_name.find("camshaft_drive_belt")!=std::string::npos &&
           installed.parent_slot==4)
          belt=true;
      check(belt,"Duhen stock engine installs the camshaft drive belt on crankshaft slot 4");
      std::array<PartMatrix,4> straight{};
      std::array<Object*,4> rims{};
      for(const auto& installed:tree) {
        if(installed.object->class_name.find(".rims.")!=std::string::npos && installed.parent_slot>=101 && installed.parent_slot<=104) {
          const int corner=installed.parent_slot-101;rims[corner]=installed.object;
          scene_instances_script_matrix(installed.object,straight[corner].data());
        }
      }
      int tyre_count=0;bool tyre_centers=true;
      for(bool solved:state.geometry_solved)tyre_centers=tyre_centers&&solved;
      PartMatrix chassis;scene_instances_script_matrix(other,chassis.data());
      const auto inverse=part_matrix_inverse_rigid(chassis);
      for(const auto& installed:tree)if(installed.object->class_name.find(".tyres.")!=std::string::npos) {
        const auto rim=std::find(rims.begin(),rims.end(),installed.parent);
        if(rim==rims.end())continue;
        const size_t corner=rim-rims.begin();
        for(const auto& mesh:scene_instances_collect_root(installed.object)) {
          PartMatrix world;if(!scene_instances_script_matrix(mesh.key,world.data()))continue;
          const auto local=part_matrix_multiply(inverse,world);const auto& g=*mesh.geometry;
          const float x=(g.info.bbox_min[0]+g.info.bbox_max[0])*.5f*g.position_scale;
          const float y=(g.info.bbox_min[1]+g.info.bbox_max[1])*.5f*g.position_scale;
          const float z=(g.info.bbox_min[2]+g.info.bbox_max[2])*.5f*g.position_scale;
          const float actual_x=local[0]*x+local[4]*y+local[8]*z+local[12];
          float shadow_frame[16];
          tyre_centers=tyre_centers&&part_shadow_source(mesh.key,shadow_frame)==gameref_get(other)->key;
          printf("TYRE center corner=%zu actual=%.6f constrained=%.6f\n",corner,actual_x,state.wheel_center[corner][0]);
          tyre_centers=tyre_centers&&std::fabs(actual_x)>std::fabs(state.wheel_center[corner][0])+.01 && std::fabs(actual_x)<std::fabs(state.wheel_center[corner][0])+.12;++tyre_count;
        }
      }
      check(tyre_count==4&&tyre_centers,"native arm/hub geometry places rendered tyres independently of template track");
      auto template_positions=[&]() {
        auto* chassis=static_cast<Object*>(gameref_get(other)->key);bool valid=true;
        for(int i=0;i<4;++i) {
          auto wheel=j.invoke_by_name(chassis->class_name,"getWheel",{Value::make_obj(chassis),Value::make_int(i)},false);
          if(wheel.tag!=ValueTag::Obj||!wheel.v.o)return false;
          auto position=j.invoke_by_name("java.game.parts.WheelRef","getPos",{wheel},false);
          if(position.tag!=ValueTag::Obj||!position.v.o)return false;
          const auto x=position.v.o->fields["x"],z=position.v.o->fields["z"];
          valid=valid&&x.tag==ValueTag::Float&&z.tag==ValueTag::Float&&
            std::fabs(x.v.f-state.wheel_center[i][0])<1e-6&&std::fabs(z.v.f-state.wheel_center[i][2])<1e-6;
        }
        return valid;
      };
      check(template_positions(),"WheelRef.getPos preserves native template X/Z independently of rendered tyre position");
      check(part_command(other,"setsteer -0.7"),"original setsteer command is handled");
      bool fronts=true,rears=true;
      double front_angles[2]{};
      for(size_t i=0;i<4;++i) {
        PartMatrix turned;if(!rims[i]||!scene_instances_script_matrix(rims[i],turned.data())){fronts=rears=false;continue;}
        const double rotation=std::fabs(turned[0]-straight[i][0])+std::fabs(turned[2]-straight[i][2]);
        if(i<2){const auto delta=part_matrix_multiply(part_matrix_inverse_rigid(straight[i]),turned);front_angles[i]=std::atan2(delta[8],delta[10]);}
        if(i<2)fronts=fronts&&rotation>.1;else rears=rears&&rotation<1e-5;
      }
      check(fronts&&rears,"steering turns both front wheel meshes and leaves the unsteered rear axle aligned");
      check(front_angles[0]*front_angles[1]>0&&std::fabs(front_angles[0]-front_angles[1])>.02,"Ackermann steering distinguishes inner and outer wheel angles");
      check(template_positions(),"steering leaves native Car Information track measurements unchanged");
      part_physics_advance(1.0/60);
      part_command(other,"setsteer 0");
      bool restored=true;
      for(size_t i=0;i<4;++i){PartMatrix current;if(!rims[i]||!scene_instances_script_matrix(rims[i],current.data())){restored=false;continue;}for(int k=0;k<12;++k)restored=restored&&std::fabs(current[k]-straight[i][k])<1e-4;}
      check(restored,"clearing steering restores native alignment without accumulating rotations");
      check(std::fabs(front_track-1.366)<.0001&&std::fabs(rear_track-1.320)<.0001,
        "Duhen track matches original-game evidence: 1366 mm front and 1320 mm rear");
      check(std::fabs(std::fabs(state.wheel_center[0][2]-state.wheel_center[2][2])-2.547)<.002,
        "Duhen wheelbase matches original-game 2547 mm within displayed rounding");
      check(state.height<.82&&state.height>.65,"Duhen ride height uses CFG anchor length independently of tuned spring rest");
      check(scene_instances_floor_builds()==terrain_builds,"real RenderRef terrain indices are reused across 480 physics frames");
      // Garage.enter/lockCar issues these commands before the first host tick.
      // Select the map afterwards, as section/render synchronisation does.
      scene_instances_select(nullptr,nullptr);
      gameref_set_matrix(other,0,0,0,0,0,0);
      part_command(other,"reset");part_command(other,"setsteer -0.7");
      part_command(other,"stop");part_command(other,"wakeup");part_command(other,"suspend");
      part_physics_advance(1.0/60);
      scene_instances_select(world,nullptr);
      for(int i=0;i<360;++i)part_physics_advance(1.0/60);
      PartPhysicsState parked;part_physics_state(other,parked);int supported=0;for(bool c:parked.contact)supported+=c;
      printf("GARAGE suspended-reset y=%.6f ticks=%llu contacts=%d\n",parked.height,static_cast<unsigned long long>(parked.ticks),supported);
      check(!parked.active&&supported==4&&std::fabs(parked.height-state.height)<.01,
        "garage reset resolves ground placement before script suspend freezes the vehicle");
      for(int i=0;i<60;++i)part_physics_advance(1.0/60);
      PartPhysicsState frozen;part_physics_state(other,frozen);
      check(frozen.ticks==parked.ticks&&std::fabs(frozen.height-parked.height)<1e-7,
        "settled suspended garage car stops consuming physics ticks");
      gameref_set_matrix(other,0,3,0,0,0,0);
      for(int i=0;i<30;++i)part_physics_advance(1.0/60);
      check(std::fabs(gameref_get(other)->pos[1]-3)<1e-6,
        "scripted lift pose remains frozen when no ground reset was requested");
      part_destroy(other);
    }
    }
  } else check(false,"original Duhen pack is present");
  return failures?1:0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: host_runtime_test CASE GAME_ROOT\n");
    return 2;
  }
  integration_root = std::filesystem::absolute(argv[2]).string();
  if(std::string(argv[1])=="vehicle_physics")return run_vehicle_physics();
  if(std::string(argv[1])=="catalog")return run_catalog();
  if(std::string(argv[1])=="vehicle_assembly")return run_vehicle_assembly();
  if (std::string(argv[1]) == "paint_inventory")
    return run_paint_inventory();
  if (std::string(argv[1]) == "sfx_probe")
    return run_sfx_probe();
  if (std::string(argv[1]) == "regression_probe")
    return run_regression_probe();
  if (std::string(argv[1]) == "gap_probe")
    return run_gap_probe();
  if (std::string(argv[1]) == "hud_probe")
    return run_hud_probe();
  if (std::string(argv[1]) == "career_probe")
    return run_career_probe();
  if (std::string(argv[1]) == "filedialog_probe")
    return run_filedialog_probe();
  if (std::string(argv[1]) == "scene_script_probe")
    return run_scene_script_probe();
  if (std::string(argv[1]) == "icon_probe")
    return run_icon_probe();
  return 2;
}
