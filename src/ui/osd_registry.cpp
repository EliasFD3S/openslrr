#include "slrr/gc_roots.hpp"
#include "slrr/osd_registry.hpp"

#include "slrr/object.hpp"
#include "slrr/slrr_font.hpp"

#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace slrr {
namespace {

std::vector<OsdWidget> g_widgets;
OsdClickFn g_click;
OsdSliderFn g_slider;
OsdHoverFn g_hover;
OsdFramebufferFn g_fb_listener;
int32_t g_group_count = 1;
int32_t g_current_group = 0;
bool g_took_zero = false;
int32_t g_last_closed_group = -1;
std::unordered_set<int32_t> g_hidden_groups;
int32_t g_fb_w = 800;
int32_t g_fb_h = 600;
int32_t g_hover_index = -1;
int32_t g_string_focus = -1;
std::string g_highlight_path;
std::string g_disabled_highlight_path;

bool owner_suppressed(const OsdWidget& w) {
  if (!w.owner) return false;
  auto it = w.owner->fields.find("_host_hidden");
  return it != w.owner->fields.end() && it->second.tag == ValueTag::Int &&
         it->second.v.i != 0;
}

Object* modal_osd() {
  for (int32_t i = static_cast<int32_t>(g_widgets.size()) - 1; i >= 0; --i) {
    const OsdWidget& w = g_widgets[static_cast<size_t>(i)];
    if (w.hidden || w.dead || !w.owner || owner_suppressed(w)) continue;
    auto it = w.owner->fields.find("flags");
    if (it == w.owner->fields.end() || it->second.tag != ValueTag::Int)
      continue;
    if ((it->second.v.i & 1) != 0) return w.owner;
  }
  return nullptr;
}

bool widget_nofocus(const OsdWidget& w) {
  if (!w.button) return false;
  auto it = w.button->fields.find("nofocus");
  return it != w.button->fields.end() && it->second.tag == ValueTag::Int &&
         it->second.v.i != 0;
}

bool widget_slider_track(const OsdWidget& w) {
  if (!w.button) return false;
  auto it = w.button->fields.find("parent");
  Object* parent = it != w.button->fields.end() && it->second.tag == ValueTag::Obj
                       ? it->second.v.o : nullptr;
  return parent && parent->class_name == "java.render.osd.Slider";
}

bool widget_disabled(const OsdWidget& w) {
  if (w.disabled) return true;
  if (!w.button) return false;
  auto it = w.button->fields.find("disabled");
  return it != w.button->fields.end() && it->second.tag == ValueTag::Int &&
         it->second.v.i != 0;
}

// Group.java still points a disabled gadget when highliteMethod is exactly
// HL_BACKLIGHT. That is the pager (Empty.ptx): the forbidden plate is the
// focus rectangle, not a picture that stays up.
bool widget_backlight(const OsdWidget& w) {
  if (!w.button) return false;
  auto it = w.button->fields.find("highliteMethod");
  if (it == w.button->fields.end()) return false;
  int method = 0;
  if (it->second.tag == ValueTag::Int) method = it->second.v.i;
  else if (it->second.tag == ValueTag::Float)
    method = static_cast<int>(it->second.v.f);
  else return false;
  return method == 2;
}

void refresh_string_text(const OsdWidget& w, bool caret) {
  if (!w.text_key) return;
  std::string shown = w.input;
  if (caret) shown.push_back('_');
  slrr_text_set_string(w.text_key, shown.c_str());
  slrr_text_update(w.text_key);
}

void sync_string_value(const OsdWidget& w) {
  if (w.value_str) w.value_str->str = w.input;
  if (w.owner && w.value_str)
    w.owner->fields["stringValue"] = Value::make_obj(w.value_str);
}

}  // namespace

void osd_set_framebuffer(int32_t width, int32_t height) {
  const int32_t old_w = g_fb_w;
  const int32_t old_h = g_fb_h;
  const float old_a = osd_framebuffer_aspect();
  if (width > 0) g_fb_w = width;
  if (height > 0) g_fb_h = height;
  const float new_a = osd_framebuffer_aspect();
  // half_w for icons was baked with old aspect; rescale so they stay square.
  // Background plates use text-space width (w=2 → half_w=1 = full canvas) —
  // never shrink them with aspect or stripes look like fixed-res floating panels.
  if (old_a > 0.1f && new_a > 0.1f &&
      std::fabs(old_a - new_a) > 0.001f && !g_widgets.empty()) {
    const float s = old_a / new_a;
    for (OsdWidget& w : g_widgets) {
      if (w.background) continue;
      w.half_w *= s;
    }
  }
  if ((g_fb_w != old_w || g_fb_h != old_h) && g_fb_listener)
    g_fb_listener(g_fb_w, g_fb_h);
}

float osd_framebuffer_aspect() {
  return g_fb_h > 0 ? static_cast<float>(g_fb_w) / static_cast<float>(g_fb_h)
                    : (4.f / 3.f);
}

int32_t osd_framebuffer_width() { return g_fb_w; }
int32_t osd_framebuffer_height() { return g_fb_h; }

void osd_set_framebuffer_listener(OsdFramebufferFn fn) {
  g_fb_listener = std::move(fn);
}

void osd_hide_widget(int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= g_widgets.size()) return;
  OsdWidget& w = g_widgets[static_cast<size_t>(index)];
  w.hidden = true;
  w.dead = true;
}

OsdWidget* osd_widget_mut(int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= g_widgets.size())
    return nullptr;
  return &g_widgets[static_cast<size_t>(index)];
}

void osd_clear_widgets() {
  g_widgets.clear();
  g_group_count = 1;
  g_current_group = 0;
  g_took_zero = false;
  g_last_closed_group = -1;
  g_hidden_groups.clear();
  g_hover_index = -1;
  g_string_focus = -1;
}

int32_t osd_viewport_pri(Object* osd) {
  if (!osd) return 0;
  auto vp = osd->fields.find("vp");
  if (vp == osd->fields.end() || vp->second.tag != ValueTag::Obj ||
      !vp->second.v.o)
    return 0;
  auto pri = vp->second.v.o->fields.find("pri");
  if (pri == vp->second.v.o->fields.end() || pri->second.tag != ValueTag::Int)
    return 0;
  return pri->second.v.i;
}

bool osd_has_modal() { return modal_osd() != nullptr; }

bool osd_owner_modal(Object* owner) {
  if (!owner) return false;
  auto fit = owner->fields.find("flags");
  return fit != owner->fields.end() && fit->second.tag == ValueTag::Int &&
         (fit->second.v.i & 1) != 0;
}

void osd_add_widget(OsdWidget w) {
  if (w.group < 0) w.group = g_current_group;
  if (g_hidden_groups.count(w.group) != 0) {
    w.group_hidden = true;
    w.hidden = true;
  }
  if (w.layer == 0) w.layer = osd_viewport_pri(w.owner);
  if (owner_suppressed(w)) w.hidden = true;
  g_widgets.push_back(std::move(w));
}

const std::vector<OsdWidget>& osd_widgets() { return g_widgets; }

void osd_set_widget_pos(int32_t index, float x, float y) {
  if (index < 0 || static_cast<size_t>(index) >= g_widgets.size()) return;
  g_widgets[static_cast<size_t>(index)].x = x;
  g_widgets[static_cast<size_t>(index)].y = y;
}

void osd_set_widget_cmd(int32_t index, int32_t cmd) {
  if (index < 0 || static_cast<size_t>(index) >= g_widgets.size()) return;
  g_widgets[static_cast<size_t>(index)].cmd = cmd;
}

void osd_set_widget_icon(int32_t index, uint32_t icon_id, std::string path) {
  if (index < 0 || static_cast<size_t>(index) >= g_widgets.size()) return;
  OsdWidget& w = g_widgets[static_cast<size_t>(index)];
  w.icon_id = icon_id;
  w.icon_path = std::move(path);
}

void osd_sync_button_label(Object* osd, void* text, float y,
                           const std::string& label) {
  if (!text) return;
  for (OsdWidget& w : g_widgets) {
    if (w.label_src != text) continue;
    w.tip = label;
    w.label_y = y;
    // Menu rows: the glyph anchor and the hit volume are built apart
    // (font top vs button center). Growing the volume to cover both
    // made it swallow the next row, so the click loaded that car.
    if (auto* o = static_cast<Object*>(text)) {
      auto yit = o->fields.find("_text_y");
      if (yit != o->fields.end() && yit->second.tag == ValueTag::Float)
        w.y = yit->second.v.f;
    }
    return;
  }
  // Button.label.txt is bound when the script assigns its label.
}

void osd_attach_row_label(const std::string& label, float y) {
  if (label.empty()) return;
  for (int32_t i = static_cast<int32_t>(g_widgets.size()) - 1; i >= 0; --i) {
    OsdWidget& w = g_widgets[static_cast<size_t>(i)];
    if (w.group != g_current_group || w.hidden || w.background) continue;
    if (w.icon_path.empty() || !w.tip.empty() || w.row_label) continue;
    if (std::fabs(w.y - y) > 0.08f) continue;
    w.tip = label;
    w.row_label = true;
    // Track quads are short; the title sits on the same row.
    if (w.half_h < 0.05f) w.half_h = 0.05f;
    return;
  }
}

int32_t osd_widget_count() {
  int32_t n = 0;
  for (const OsdWidget& w : g_widgets)
    if (!w.hidden && !w.background) ++n;
  return n;
}

bool osd_group_is_hidden(int32_t group) {
  return g_hidden_groups.count(group) != 0;
}

int32_t osd_current_group() { return g_current_group; }

int32_t osd_begin_group() {
  g_group_count++;
  g_current_group = g_group_count - 1;
  return g_current_group;
}

int32_t osd_end_group() {
  const int32_t closed = g_current_group;
  g_group_count++;
  g_current_group = g_group_count - 1;
  g_last_closed_group = closed;
  return closed;
}

void osd_hide_group(int32_t group) {
  // TREE often loses the assign-expr value in hideGroup((g = endGroup())),
  // passing 0 instead of the just-closed id. Prefer last closed when that
  // happens and it still has widgets.
  if (group == 0 && g_last_closed_group > 0) {
    bool last_has = false;
    for (const OsdWidget& w : g_widgets) {
      if (w.group == g_last_closed_group) {
        last_has = true;
        break;
      }
    }
    if (last_has) group = g_last_closed_group;
  }
  for (OsdWidget& w : g_widgets) {
    if (w.group == group) {
      w.group_hidden = true;
      w.hidden = true;
    }
  }
  g_hidden_groups.insert(group);
  slrr_text_set_group_visible(group, false);
  std::printf("[osd] hideGroup %d (last_closed=%d)\n", group,
              g_last_closed_group);
}

void osd_show_group(int32_t group) {
  for (OsdWidget& w : g_widgets) {
    if (w.group != group) continue;
    w.group_hidden = false;
    if (!w.dead && !owner_suppressed(w)) w.hidden = false;
  }
  g_hidden_groups.erase(group);
  slrr_text_set_group_visible(group, true);
  std::printf("[osd] showGroup %d\n", group);
}

void osd_show_only_group(int32_t group) {
  for (OsdWidget& w : g_widgets) {
    if (owner_suppressed(w)) {
      w.hidden = true;
      continue;
    }
    w.group_hidden = w.group != group;
    if (w.group_hidden) g_hidden_groups.insert(w.group);
    else g_hidden_groups.erase(w.group);
    w.hidden = w.dead || owner_suppressed(w) || w.group_hidden;
  }
  slrr_text_show_only_group(group);
  int32_t shown = 0;
  for (const OsdWidget& w : g_widgets)
    if (!w.hidden && !w.background) ++shown;
  std::printf("[osd] showOnly %d widgets=%d\n", group, shown);
}

int32_t osd_group_of_cmd(int32_t cmd) {
  for (const OsdWidget& w : g_widgets) {
    if (!w.background && w.cmd == cmd) return w.group;
  }
  return -1;
}

int32_t osd_hit_index(float osd_x, float osd_y) {
  Object* modal = modal_osd();
  // Highest viewport wins (dialog 22, shield 21, menu 10). The shield is
  // created after the file rows, so creation order alone would hide them.
  int32_t best = -1;
  int32_t best_layer = 0;
  bool best_block = false;
  bool have = false;
  for (int32_t i = static_cast<int32_t>(g_widgets.size()) - 1; i >= 0; --i) {
    const OsdWidget& w = g_widgets[static_cast<size_t>(i)];
    // Do NOT skip cmd==0 — Mechanic.CMD_SCROLL_UP is 0.
    if (w.hidden || w.background || w.no_hit) continue;
    // Disabled text rows stay quiet. A disabled backlight button (catalog
    // and car-selector page arrows) is still a hover target.
    if (widget_disabled(w) && (widget_nofocus(w) || !widget_backlight(w)))
      continue;
    // Gadget.focus plants a 0x012B / 0x013C plate on the pointed icon. It is
    // not a button; hitting it keeps the highlight glued to that icon.
    if ((w.icon_id & 0xFFFFu) == 0x012Bu || (w.icon_id & 0xFFFFu) == 0x013Cu)
      continue;
    if (modal && w.owner != modal) continue;
    if (std::fabs(osd_x - w.x) > w.half_w ||
        std::fabs(osd_y - w.y) > w.half_h)
      continue;
    // nofocus is keyboard-only. The modal shield uses it and must still
    // swallow clicks that would otherwise reach the menu.
    // Slider tracks are nofocus because their parent owns focus. They still
    // receive mouse clicks; treating them as shields loses the entire drag.
    const bool block = w.cmd == 9999999 ||
                       (widget_nofocus(w) && !widget_slider_track(w));
    if (!have || w.layer > best_layer ||
        (w.layer == best_layer && best_block && !block)) {
      best = i;
      best_layer = w.layer;
      best_block = block;
      have = true;
    }
  }
  if (!have || best_block) return -1;
  return best;
}

const OsdWidget* osd_widget_at(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(g_widgets.size()))
    return nullptr;
  return &g_widgets[static_cast<size_t>(index)];
}

void osd_mark_disabled(int32_t cmd) { osd_set_cmd_disabled(cmd, true); }

void osd_set_cmd_disabled(int32_t cmd, bool disabled) {
  for (OsdWidget& w : g_widgets) {
    if (w.cmd == cmd) w.disabled = disabled;
  }
}

void osd_hide_cmd(int32_t cmd) {
  for (OsdWidget& w : g_widgets) {
    if (w.cmd == cmd) w.hidden = true;
  }
}

namespace {

bool is_osd_class(Object* o) {
  if (!o) return false;
  const std::string& n = o->class_name;
  return n.size() >= 4 && n.compare(n.size() - 4, 4, ".Osd") == 0;
}

// Cancel used to mark the shield's owner hidden. When that owner was the
// menu OSD, every label disappeared and only prime.avi stayed.
void hide_shield_group(int32_t group) {
  bool foreign = false;
  for (const OsdWidget& o : g_widgets) {
    if (o.group != group || o.cmd == 9999999 || o.background) continue;
    foreign = true;
    break;
  }
  if (foreign) return;
  for (OsdWidget& p : g_widgets) {
    if (p.group == group) p.hidden = true;
  }
}

void restore_other_osds(Object* except) {
  std::vector<Object*> owners;
  for (const OsdWidget& w : g_widgets) {
    if (!w.owner || w.owner == except || !is_osd_class(w.owner)) continue;
    bool seen = false;
    for (Object* o : owners)
      if (o == w.owner) seen = true;
    if (!seen) owners.push_back(w.owner);
  }
  for (Object* o : owners) {
    // A dialog that already called hide stays hidden. Restoring it put the
    // track-selector About box back on the main menu.
    auto hidden = o->fields.find("_host_hidden");
    if (hidden != o->fields.end() && hidden->second.tag == ValueTag::Int &&
        hidden->second.v.i != 0)
      continue;
    bool shield_only = false;
    for (const OsdWidget& w : g_widgets) {
      if (w.owner != o) continue;
      if (w.cmd == 9999999) shield_only = true;
      else if (!w.background) shield_only = false;
    }
    if (shield_only) continue;
    o->fields["_host_hidden"] = Value::make_int(0);
    for (OsdWidget& w : g_widgets) {
      if (w.owner != o || w.cmd == 9999999) continue;
      w.hidden = w.group_hidden;
    }
    slrr_text_set_osd_visible(o, true);
  }
}

}  // namespace

void osd_drop_modal_shield(Object* owner) {
  if(!owner)return;
  for (OsdWidget& w : g_widgets) {
    if(w.owner!=owner)continue;
    if (w.cmd != 9999999 || w.hidden) continue;
    w.hidden = true;
    hide_shield_group(w.group);
    if (!w.owner || !is_osd_class(w.owner)) continue;
    bool other = false;
    for (const OsdWidget& o : g_widgets) {
      if (o.owner != w.owner || o.hidden || o.cmd == 9999999 || o.background)
        continue;
      other = true;
      break;
    }
    if (other) continue;
    w.owner->fields["_host_hidden"] = Value::make_int(1);
    auto fit = w.owner->fields.find("flags");
    if (fit != w.owner->fields.end() && fit->second.tag == ValueTag::Int)
      fit->second.v.i &= ~1;
    slrr_text_set_osd_visible(w.owner, false);
  }
  for (OsdWidget& w : g_widgets) {
    if (w.owner!=owner) continue;
    auto fit = w.owner->fields.find("flags");
    if (fit == w.owner->fields.end() || fit->second.tag != ValueTag::Int)
      continue;
    if ((fit->second.v.i & 1) == 0) continue;
    fit->second.v.i &= ~1;
  }
}

bool osd_widget_is_slider(const OsdWidget& widget) {
  Object* gadget=widget.button;
  for(int depth=0;gadget && depth<4;++depth) {
    if(gadget->class_name=="java.render.osd.Slider")return true;
    const auto parent=gadget->fields.find("parent");
    gadget=parent!=gadget->fields.end() && parent->second.tag==ValueTag::Obj
        ?parent->second.v.o:nullptr;
  }
  return false;
}

bool osd_set_hover_at(float osd_x, float osd_y) {
  const int32_t idx = osd_hit_index(osd_x, osd_y);
  auto target=[](const OsdWidget* w)->Object* {
    if(!w || !w->button)return nullptr;
    auto i=w->button->fields.find("parent");
    if(i!=w->button->fields.end()&&i->second.tag==ValueTag::Obj&&i->second.v.o &&
       i->second.v.o->class_name=="java.render.osd.Slider")return i->second.v.o;
    return w->button;
  };
  const bool changed = target(osd_widget_at(idx)) != target(osd_widget_at(g_hover_index));
  g_hover_index = idx;
  if (g_hover) g_hover(osd_widget_at(idx));
  return changed;
}

int32_t osd_hover_index() { return g_hover_index; }

const OsdWidget* osd_hover_widget() {
  const auto* w = osd_widget_at(g_hover_index);
  return w && !w->hidden && !owner_suppressed(*w) && !widget_disabled(*w) ? w : nullptr;
}
bool osd_focus_plate_visible(const OsdWidget& plate) {
  Object* gadget = plate.highlight_for;
  if (!gadget || plate.hidden || owner_suppressed(plate)) return false;
  auto get_obj = [](Object* o, const char* name) -> Object* {
    if (!o) return nullptr;
    auto it = o->fields.find(name);
    return it != o->fields.end() && it->second.tag == ValueTag::Obj ? it->second.v.o : nullptr;
  };
  Object* osd = get_obj(gadget, "osd");
  if (!osd || get_obj(osd, "pointedGadget") != gadget) return false;
  auto mode = osd->fields.find("controlMode");
  if (mode != osd->fields.end() && mode->second.tag == ValueTag::Int && mode->second.v.i == 1) return true;
  // osd_hover_widget drops disabled buttons, but that is exactly when
  // Gadget.focus builds the forbidden plate.
  const auto* hover = osd_widget_at(osd_hover_index());
  if (!hover || hover->hidden || owner_suppressed(*hover)) return false;
  return hover->button == gadget;
}
bool osd_highlight_disabled(const OsdWidget& plate) {
  if(!plate.highlight_for)return plate.disabled;
  auto it=plate.highlight_for->fields.find("disabled");
  return it!=plate.highlight_for->fields.end()&&it->second.tag==ValueTag::Int&&it->second.v.i!=0;
}

void osd_set_highlight_path(std::string path) {
  g_highlight_path = std::move(path);
  std::printf("[osd] highlight tex=%s\n",
              g_highlight_path.empty() ? "(none)" : g_highlight_path.c_str());
}

const std::string& osd_highlight_path() { return g_highlight_path; }
void osd_set_disabled_highlight_path(std::string path) { g_disabled_highlight_path = std::move(path); }
const std::string& osd_disabled_highlight_path() { return g_disabled_highlight_path; }

void osd_set_click_handler(OsdClickFn fn) { g_click = std::move(fn); }

void osd_set_hover_handler(OsdHoverFn fn) { g_hover = std::move(fn); }

std::string g_loading_overlay;
bool g_loading_dialog = false;

void osd_set_loading_overlay(const std::string& path) { g_loading_overlay = path; }

const std::string& osd_loading_overlay() { return g_loading_overlay; }

void osd_set_loading_dialog(bool on) { g_loading_dialog = on; }

bool osd_loading_dialog() { return g_loading_dialog; }

void osd_ensure_loading_plate(Object* osd, const std::string& path) {
  if (!osd) return;
  bool have = false;
  for (OsdWidget& w : g_widgets) {
    if (w.owner != osd || w.dead) continue;
    w.hidden = false;
    w.group_hidden = false;
    if (w.layer < 100000) w.layer = 100000;
    if (w.background && w.half_w > 0.4f && w.half_h > 0.4f) {
      have = true;
      if (w.icon_path.empty()) w.icon_path = path;
    }
  }
  if (!have && !path.empty()) {
    OsdWidget w;
    w.owner = osd;
    w.group = -1;
    w.half_w = 1.f;
    w.half_h = 1.f;
    w.background = true;
    w.no_hit = true;
    w.layer = 100000;
    w.icon_path = path;
    osd_add_widget(std::move(w));
    // Group 0 is often already hidden. The new plate must stay up.
    g_widgets.back().hidden = false;
    g_widgets.back().group_hidden = false;
    g_widgets.back().group = -1;
  }
  slrr_text_set_osd_visible(osd, true);
}

void osd_set_owner_visible(Object* osd, bool visible) {
  if (!osd) return;
  osd->fields["_host_hidden"] = Value::make_int(visible ? 0 : 1);
  if (!visible) {
    auto fit = osd->fields.find("flags");
    if (fit != osd->fields.end() && fit->second.tag == ValueTag::Int)
      fit->second.v.i &= ~1;
  }
  for (size_t i = 0; i < g_widgets.size(); ++i) {
    OsdWidget& w = g_widgets[i];
    if (w.owner != osd) continue;
    w.hidden = visible ? w.group_hidden : true;
    if (!visible && static_cast<int32_t>(i) == g_string_focus)
      g_string_focus = -1;
  }
  slrr_text_set_osd_visible(osd, visible);
  // The darken button is created on backShieldOsd, not on the plate.
  // Hiding the plate alone left that quad up after NO. Restoring the
  // other OSDs puts the menu labels back if the shield shared an owner.
  if (!visible) {
    osd_drop_modal_shield(osd);
    restore_other_osds(osd);
  }
}

void osd_mark_string_input(Object* gadget, Object* value, void* text_key,
                           const std::string& input) {
  if (g_widgets.empty()) return;
  OsdWidget& w = g_widgets.back();
  w.string_input = true;
  w.gadget = gadget;
  w.value_str = value;
  w.text_key = text_key;
  w.input = input;
  g_string_focus = static_cast<int32_t>(g_widgets.size()) - 1;
  sync_string_value(w);
  if (w.owner && gadget)
    w.owner->fields["actGadget"] = Value::make_obj(gadget);
  refresh_string_text(w, true);
}

void osd_on_char(int ascii) {
  if (g_string_focus < 0 ||
      g_string_focus >= static_cast<int32_t>(g_widgets.size()))
    return;
  OsdWidget& w = g_widgets[static_cast<size_t>(g_string_focus)];
  if (!w.string_input || w.hidden) return;
  if (ascii == 27) {
    w.input.clear();
    sync_string_value(w);
    if (w.owner) w.owner->fields["actGadget"] = Value::make_obj(nullptr);
    refresh_string_text(w, false);
    g_string_focus = -1;
    return;
  }
  if (ascii == 13) {
    sync_string_value(w);
    if (w.owner) w.owner->fields["actGadget"] = Value::make_obj(nullptr);
    refresh_string_text(w, false);
    OsdWidget copy = w;
    g_string_focus = -1;
    if (g_click) g_click(copy);
    return;
  }
  if (ascii == 8) {
    if (!w.input.empty()) w.input.pop_back();
  } else if (ascii >= 32 && ascii < 127) {
    if (w.input.size() >= 16) return;
    w.input.push_back(static_cast<char>(ascii));
  } else {
    return;
  }
  sync_string_value(w);
  refresh_string_text(w, true);
}

Object* osd_inventory_drag_source_at(float x, float y) {
  const auto* w = osd_widget_at(osd_hit_index(x, y));
  if (!w || widget_disabled(*w) || !w->button || !w->handler ||
      w->handler->class_name != "java.game.InventoryPanel") return nullptr;
  auto object_field = [](Object* o, const char* key) -> Object* {
    if (!o) return nullptr;
    auto it = o->fields.find(key);
    return it != o->fields.end() && it->second.tag == ValueTag::Obj
               ? it->second.v.o : nullptr;
  };
  Object* inv = object_field(w->handler, "inv");
  if (!inv || !object_field(w->handler, "invItem")) return nullptr;
  auto it = inv->fields.find("interactive");
  if (it == inv->fields.end() || it->second.tag != ValueTag::Int || !it->second.v.i)
    return nullptr;
  return w->button;
}

bool osd_fire_inventory_drop_at(Object* source, float x, float y) {
  if (!source) return false;
  bool live_source = false;
  for (const auto& w : g_widgets)
    if (w.button == source && !w.hidden && !w.dead && !widget_disabled(w))
      live_source = true;
  const auto* target = osd_widget_at(osd_hit_index(x, y));
  if (!live_source || !target || widget_disabled(*target) ||
      !target->button || target->button == source || !target->owner || !g_click)
    return false;
  const auto enabled = target->button->fields.find("dropEnabled");
  if (enabled == target->button->fields.end() || enabled->second.tag != ValueTag::Int ||
      !enabled->second.v.i) return false;
  const OsdWidget hit = *target; // Script callbacks can rebuild the registry.
  Object* owner = hit.owner;
  owner->fields["dropObject"] = Value::make_obj(nullptr);
  owner->fields["dropGadget"] = Value::make_obj(source);
  g_click(hit);
  owner->fields["dropGadget"] = Value::make_obj(nullptr);
  owner->fields["dropObject"] = Value::make_obj(nullptr);
  return true;
}

void osd_set_slider_handler(OsdSliderFn fn) { g_slider = std::move(fn); }
bool osd_update_slider_at(int32_t index, float x, float y) {
  const auto* w = osd_widget_at(index);
  if (!w || w->dead || w->hidden || owner_suppressed(*w) || widget_disabled(*w) ||
      (modal_osd() && modal_osd() != w->owner) || !g_slider) return false;
  const OsdWidget copy = *w; // setValue can rebuild widget records.
  return g_slider(copy, x, y);
}

void osd_fire_click_at(float osd_x, float osd_y) {
  const int32_t idx = osd_hit_index(osd_x, osd_y);
  if (idx < 0 || idx >= static_cast<int32_t>(g_widgets.size())) return;
  const OsdWidget hit = g_widgets[static_cast<size_t>(idx)];
  // Hover can land here. The command still does not run.
  if (widget_disabled(hit)) return;
  if (osd_update_slider_at(idx, osd_x, osd_y)) return;
  if (hit.string_input) {
    g_string_focus = idx;
    sync_string_value(hit);
    if (hit.owner && hit.gadget)
      hit.owner->fields["actGadget"] = Value::make_obj(hit.gadget);
    refresh_string_text(hit, true);
    return;
  }
  if (g_string_focus >= 0 &&
      g_string_focus < static_cast<int32_t>(g_widgets.size()) &&
      g_string_focus != idx) {
    OsdWidget sw = g_widgets[static_cast<size_t>(g_string_focus)];
    sync_string_value(sw);
    refresh_string_text(sw, false);
    g_string_focus = -1;
    // actGadget stays set: cmd 12345 copies the text and does not notify.
    if (g_click) g_click(sw);
    if (sw.owner) sw.owner->fields["actGadget"] = Value::make_obj(nullptr);
  }
  std::printf("[osd] click cmd=%d handler=%s\n", hit.cmd,
              hit.handler_class.empty() ? "?" : hit.handler_class.c_str());
  if (g_click) g_click(hit);
  osd_set_hover_at(osd_x, osd_y);
}

void osd_pixel_to_text(int32_t px, int32_t py, int32_t fb_w, int32_t fb_h,
                       float* out_x, float* out_y) {
  if (!out_x || !out_y || fb_w <= 0 || fb_h <= 0) return;
  *out_x = (static_cast<float>(px) / static_cast<float>(fb_w)) * 2.f - 1.f;
  *out_y = (static_cast<float>(py) / static_cast<float>(fb_h)) * 2.f - 1.f;
}

namespace {

Object* field_o(Object* o, const char* k) {
  if (!o) return nullptr;
  auto it = o->fields.find(k);
  if (it != o->fields.end() && it->second.tag == ValueTag::Obj) return it->second.v.o;
  return nullptr;
}

void hide_group_exact(int32_t group) {
  for (OsdWidget& w : g_widgets) {
    if (w.group == group) {
      w.group_hidden = true;
      w.hidden = true;
    }
  }
  g_hidden_groups.insert(group);
  slrr_text_set_group_visible(group, false);
}

void show_group_exact(int32_t group) {
  for (OsdWidget& w : g_widgets) {
    if (w.group != group) continue;
    w.group_hidden = false;
    if (!w.dead && !owner_suppressed(w)) w.hidden = false;
  }
  g_hidden_groups.erase(group);
  slrr_text_set_group_visible(group, true);
}

int32_t alloc_group_id() {
  if (!g_took_zero) {
    g_took_zero = true;
    g_current_group = 0;
    return 0;
  }
  return osd_begin_group();
}

void bind_gadget(Object* g) {
  if (!g) return;
  Object* phy = field_o(g, "phy");
  if (!phy) return;
  auto wit = phy->fields.find("_osd_wi");
  if (wit == phy->fields.end() || wit->second.tag != ValueTag::Int) return;
  OsdWidget* w = osd_widget_mut(wit->second.v.i);
  if (!w) return;
  w->button = g;
  w->hit_only = true;
  if (Object* label = field_o(g, "label")) {
    if (Object* text = field_o(label, "txt")) w->label_src = text;
  }
  auto cit = g->fields.find("cmd");
  if (cit != g->fields.end() && cit->second.tag == ValueTag::Int)
    w->cmd = cit->second.v.i;
  if (Object* handler = field_o(g, "handler")) {
    w->handler = handler;
    w->handler_class = handler->class_name;
  }
  if (Object* rect = field_o(g, "rect")) {
    if (Object* ins = field_o(rect, "ins")) {
      auto vit = ins->fields.find("_osd_wi");
      if (vit != ins->fields.end() && vit->second.tag == ValueTag::Int) {
        if (OsdWidget* vis = osd_widget_mut(vit->second.v.i)) {
          vis->no_hit = true;
          vis->background = false;
          vis->button = g;
          vis->cmd = w->cmd;
          auto dit = g->fields.find("disabled");
          vis->disabled = dit != g->fields.end() &&
                          dit->second.tag == ValueTag::Int && dit->second.v.i != 0;
          if (!vis->icon_path.empty()) {
            w->icon_id = vis->icon_id;
            w->icon_path = vis->icon_path;
          }
        }
      }
    }
  }
}

void bind_input(Object* g) {
  if (!g) return;
  Object* phy = field_o(g, "phy");
  Object* text = field_o(g, "inputText");
  if (!phy || !text) return;
  auto wit = phy->fields.find("_osd_wi");
  if (wit == phy->fields.end() || wit->second.tag != ValueTag::Int) return;
  OsdWidget* w = osd_widget_mut(wit->second.v.i);
  if (!w) return;
  Object* val = field_o(g, "inputString");
  w->string_input = true;
  w->gadget = g;
  w->button = g;
  w->hit_only = true;
  w->text_key = text;
  w->value_str = val;
  w->input = val ? val->str : std::string{};
  auto cit = g->fields.find("cmd");
  if (cit != g->fields.end() && cit->second.tag == ValueTag::Int)
    w->cmd = cit->second.v.i;
  if (Object* handler = field_o(g, "handler")) {
    w->handler = handler;
    w->handler_class = handler->class_name;
  }
  g_string_focus = wit->second.v.i;
  sync_string_value(*w);
  if (w->owner) w->owner->fields["actGadget"] = Value::make_obj(g);
}

}  // namespace

bool osd_claim_button_icon(Object* render, float x, float y, uint32_t icon_id,
                           const std::string& path) {
  if (!render) return false;
  // A replacement RenderRef owns a new widget. Reusing an old widget by
  // coordinates aliases two instance lifetimes and can leave an orphan glyph.
  for(OsdWidget& hit:g_widgets) {
    if(!hit.hit_only || hit.dead || !hit.button)continue;
    if(field_o(field_o(hit.button,"rect"),"ins")!=render)continue;
    for(OsdWidget& old:g_widgets) {
      if(old.button!=hit.button || old.hit_only || old.dead)continue;
      if(old.picture_source==render)continue;
      old.hidden=old.dead=true;old.icon_path.clear();
    }
    hit.icon_id=icon_id;hit.icon_path=path;
    break;
  }
  return false;
}

void osd_attach_button_picture(Object* render) {
  if (!render) return;
  auto wit = render->fields.find("_osd_wi");
  if (wit == render->fields.end() || wit->second.tag != ValueTag::Int) return;
  OsdWidget* vis = osd_widget_mut(wit->second.v.i);
  if (!vis) return;
  for (OsdWidget& w : g_widgets) {
    if (!w.button || w.dead || !w.hit_only) continue;
    Object* rect = field_o(w.button, "rect");
    Object* ins = field_o(rect, "ins");
    if (ins != render) continue;
    // changeTexture builds a new rectangle. The previous picture stays in
    // the group, and showGroup would paint it again on top of the new one.
    for (OsdWidget& old : g_widgets) {
      if (&old == vis || &old == &w || old.hit_only || old.dead) continue;
      if (old.button != w.button) continue;
      old.hidden = true;
      old.dead = true;
      old.icon_path.clear();
    }
    if (w.hit_only) {
      w.icon_id = vis->icon_id;
      w.icon_path = vis->icon_path;
    }
    vis->button = w.button;
    vis->cmd = w.cmd;
    vis->no_hit = true;
    vis->background = false;
    auto dit = w.button->fields.find("disabled");
    vis->disabled = dit != w.button->fields.end() &&
                    dit->second.tag == ValueTag::Int && dit->second.v.i != 0;
    return;
  }
}

void osd_sync_button_pictures() {
  std::unordered_set<Object*> seen;
  for(OsdWidget& hit:g_widgets) {
    if(!hit.hit_only || hit.dead || !hit.button || !seen.insert(hit.button).second)continue;
    Object* render=field_o(field_o(hit.button,"rect"),"ins");
    if(!render)continue;
    auto wi=render->fields.find("_osd_wi");
    if(wi==render->fields.end() || wi->second.tag!=ValueTag::Int)continue;
    OsdWidget* current=osd_widget_mut(wi->second.v.i);
    if(!current || current->dead || (current->picture_source && current->picture_source!=render))continue;
    auto previous=hit.button->fields.find("_host_current_picture");
    if(previous==hit.button->fields.end() || previous->second.tag!=ValueTag::Int || previous->second.v.i!=wi->second.v.i) {
      osd_attach_button_picture(render);
      hit.button->fields["_host_current_picture"]=Value::make_int(wi->second.v.i);
    }
    current->background=false;current->no_hit=true;
    hit.icon_id=current->icon_id;hit.icon_path=current->icon_path;
  }
}

void osd_on_field_write(Object* recv, const std::string& field) {
  if (!recv || field.empty()) return;
  if (field == "globalHandler" && is_osd_class(recv)) {
    Object* dialog = field_o(recv, "globalHandler");
    if (field_o(dialog, "osd") != recv) return;
    if (!dialog->fields.contains("controller") || !dialog->fields.contains("flags"))
      return;
    const auto shown = dialog->fields.find("shown");
    if (shown != dialog->fields.end() && shown->second.tag == ValueTag::Int &&
        shown->second.v.i != 0) return;
    // Dialog constructors can build and select a group before show().
    // Keep those widgets hidden until their viewport is activated.
    recv->fields["_host_hidden"] = Value::make_int(1);
    for (OsdWidget& w : g_widgets)
      if (w.owner == recv) w.hidden = true;
    slrr_text_set_osd_visible(recv, false);
    return;
  }
  if(field=="ins" && recv->class_name=="java.render.Rectangle") {
    Object* current=field_o(recv,"ins");
    Object* previous=field_o(recv,"_host_picture_instance");
    if(previous && previous!=current) {
      for(OsdWidget& old:g_widgets)if(old.picture_source==previous) {
        old.hidden=old.dead=true;old.icon_path.clear();
      }
    }
    recv->fields["_host_picture_instance"]=Value::make_obj(current);
    return;
  }
  if (field == "osd" && recv->class_name == "java.render.Group") {
    const int32_t id = alloc_group_id();
    recv->fields["_host_group"] = Value::make_int(id);
    g_current_group = id;
    return;
  }
  if (field == "vp") {
    // Camera.create also stores vp. That write must not replace the Osd
    // that Viewport.deactivate hides, or cancel hides the menu.
    if (!is_osd_class(recv)) return;
    if (Object* vp = field_o(recv, "vp"))
      vp->fields["_osd"] = Value::make_obj(recv);
    return;
  }
  if (field == "disabled") {
    const bool dis = recv->fields["disabled"].tag == ValueTag::Int &&
                     recv->fields["disabled"].v.i != 0;
    for (OsdWidget& w : g_widgets)
      if (w.button == recv) w.disabled = dis;
  }
  if (field == "handler" || field == "cmd" || field == "rect" || field == "label") {
    if (field_o(recv, "phy")) bind_gadget(recv);
  }
  if (field == "hr") {
    if (Object* rectangle = field_o(recv, "hr")) {
      if (Object* instance = field_o(rectangle, "ins")) {
        auto wi = instance->fields.find("_osd_wi");
        if (wi != instance->fields.end() && wi->second.tag == ValueTag::Int)
          if (auto* plate = osd_widget_mut(wi->second.v.i)) plate->highlight_for = recv;
      }
    }
  }
  if (field == "tooltip") {
    if (field_o(recv, "phy")) bind_gadget(recv);
    Object* tip = field_o(recv, "tooltip");
    Object* phy = field_o(recv, "phy");
    if (tip && phy) {
      auto wit = phy->fields.find("_osd_wi");
      if (wit != phy->fields.end() && wit->second.tag == ValueTag::Int)
        if (OsdWidget* w = osd_widget_mut(wit->second.v.i))
          if (w->tip.empty()) w->tip = tip->str;
    }
  }
  if (field == "inputString") bind_input(recv);
}

void osd_on_group_leaf(Object* group, bool leaf) {
  if (!group || group->class_name != "java.render.Group") return;
  auto it = group->fields.find("_host_group");
  if (it == group->fields.end() || it->second.tag != ValueTag::Int) return;
  if (leaf)
    hide_group_exact(it->second.v.i);
  else
    show_group_exact(it->second.v.i);
}


void osd_visit_gc_roots(const GcRootVisitor& visit) {
  // Hidden pages may be shown again. Retain their backing script objects,
  // including retired widgets until the registry itself releases them.
  for (const auto& w : g_widgets) {
    visit(w.handler); visit(w.owner); visit(w.button); visit(w.picture_source);
    visit(w.highlight_for); visit(w.label_src); visit(w.value_str); visit(w.gadget); visit(w.text_key);
  }
}
}  // namespace slrr
