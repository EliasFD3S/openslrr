#include "slrr/osd_registry.hpp"
#include "slrr/object.hpp"
#include "slrr/ui_time.hpp"
#include <cstdio>
using namespace slrr;
int main() {
  int failed = 0;
  auto check = [&](bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failed += !ok;
  };
  Object osd, first, second;
  osd.class_name = "java.render.Osd";
  for (auto* button : {&first, &second}) {
    button->fields["osd"] = Value::make_obj(&osd);
    button->fields["disabled"] = Value::make_int(0);
  }
  osd_clear_widgets();
  OsdWidget a;
  a.button = &first; a.owner = &osd; a.hit_only = true;
  a.x = -.5f; a.half_w = .1f;
  osd_add_widget(a);
  OsdWidget b = a;
  b.button = &second; b.x = .5f;
  osd_add_widget(b);
  OsdWidget old_plate, current_plate;
  old_plate.icon_id = current_plate.icon_id = 0x12b;
  old_plate.highlight_for = &first;
  current_plate.highlight_for = &second;
  osd.fields["controlMode"] = Value::make_int(0);
  osd.fields["pointedGadget"] = Value::make_obj(&second);
  osd_set_hover_at(.5f, 0.f);
  check(!osd_focus_plate_visible(old_plate), "old mode's focus plate is excluded");
  check(osd_focus_plate_visible(current_plate), "current mouse target's plate is visible");
  check(!osd_focus_plate_visible(OsdWidget{}), "orphaned focus plate is excluded");
  osd_hide_widget(1);
  check(!osd_hover_widget() && !osd_focus_plate_visible(current_plate),
        "hidden target cannot retain mouse hover");
  osd.fields["controlMode"] = Value::make_int(1);
  check(osd_focus_plate_visible(current_plate), "keyboard focus remains supported");
  current_plate.hidden = true;
  check(!osd_focus_plate_visible(current_plate), "hidden keyboard plate is excluded");
  osd_widget_mut(1)->hidden = false;
  osd.fields["pointedGadget"] = Value::make_obj(&first);
  osd_set_hover_handler([&](const OsdWidget* w) {
    osd.fields["pointedGadget"] = Value::make_obj(w ? w->button : nullptr);
  });
  const bool changed = osd_set_hover_at(.5f, 0.f);
  check(!changed && osd.fields["pointedGadget"].v.o == &second,
        "same mouse target restores focus after mode selection without another move sound");
  osd_set_hover_handler({});
  check(format_game_time(0.f, 0x21) == "12:00am", "clock formats midnight");
  check(format_game_time(43140.f, 0x21) == "11:59am", "clock formats before noon");
  check(format_game_time(43200.f, 0x21) == "12:00pm", "clock formats noon");
  check(format_game_time(46860.f, 0x21) == "1:01pm", "clock formats afternoon");
  check(format_game_time(3661.f, 0) == "01:01:01", "24 hour clock with seconds");
  osd_clear_widgets();
  return failed ? 1 : 0;
}
