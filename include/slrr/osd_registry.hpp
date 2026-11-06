#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slrr {

struct Object;

// OSD widgets registered by Osd.createButton / createRectangle (TREE → native).
// Coordinates are stock OSD text-space ≈ fullscreen NDC (y+ down, y=-1 top).
struct OsdWidget {
  int32_t cmd = 0;  // may be 0 (Mechanic.CMD_SCROLL_UP) — not a miss sentinel
  float x = 0.f;
  float y = 0.f;
  float half_w = 0.06f;
  float half_h = 0.06f;
  bool disabled = false;
  bool hidden = false;     // hideGroup or the whole Osd
  // destroy() hid this picture. showGroup must not bring it back.
  bool dead = false;
  // Survives Osd.show(). Dialog.show() activates the viewport after the
  // pages were hidden; that must not bring every options page back.
  bool group_hidden = false;
  bool background = false; // createRectangle plates (draw under icons, no hit)
  // PhysicsRef hit volume. The Rectangle mesh is the picture; this box
  // only receives the click (and carries the icon path for the hover plate).
  bool hit_only = false;
  // Rectangle picture under a button. The physics box is the hit target.
  bool no_hit = false;
  // Modal darken (RRT_DARKEN). Keep the texture alpha so the scene shows
  // through; icon cutout would force it opaque.
  bool tex_alpha = false;
  // Viewport priority. Lower layers draw first (menu, then darken, then dialog).
  int32_t layer = 0;
  int32_t group = 0;
  uint32_t icon_id = 0;  // ResourceRef local/full id, 0 = none
  std::string icon_path;  // resolved game path (.ptx / .dds), empty = white
  std::string tip;
  // Slider/multi row: tip is the title drawn beside the control (HL_LABELCOLOR).
  bool row_label = false;
  // Object the button was created with (osd.globalHandler). Clicks call
  // that instance's osdCommand.
  Object* handler = nullptr;
  std::string handler_class;
  // Osd instance that created this widget. show/hide touch only that instance.
  Object* owner = nullptr;
  // Script Button. Hit-test reads nofocus, which the script sets after create.
  Object* button = nullptr;
  // Exact Rectangle RenderRef, independent of screen position or texture.
  Object* picture_source = nullptr;
  Object* highlight_for = nullptr;
  // Text object whose changeText fills tip. File rows are created as ""
  // and renamed afterwards; hover matches tip against that text.
  void* label_src = nullptr;
  // Fullscreen y of that text. The hit box sits on the button center,
  // which is not the glyph baseline inside a small dialog viewport.
  float label_y = 0.f;
  // createStringInput. A click focuses it; keys write value_str.
  bool string_input = false;
  std::string input;
  Object* value_str = nullptr;
  Object* gadget = nullptr;
  void* text_key = nullptr;
};

void osd_clear_widgets();
bool osd_focus_plate_visible(const OsdWidget& plate);
bool osd_highlight_disabled(const OsdWidget& plate);
// Hide one widget (ResourceRef.destroy of a rectangle instance).
void osd_hide_widget(int32_t index);
OsdWidget* osd_widget_mut(int32_t index);
void osd_add_widget(OsdWidget w);
// Dialog.show sets Osd.OF_MODAL. That plate has to cover the catalog.
bool osd_owner_modal(Object* owner);
// Viewport priority stored by Viewport.create (menu 10, dialog 22, shield 21).
int32_t osd_viewport_pri(Object* osd);
const std::vector<OsdWidget>& osd_widgets();
void osd_sync_button_pictures();
int32_t osd_widget_count();
// Patch hit-rect after PhysicsRef.setMatrix (createButton TREE path).
void osd_set_widget_pos(int32_t index, float x, float y);
void osd_set_widget_cmd(int32_t index, int32_t cmd);
void osd_set_widget_icon(int32_t index, uint32_t icon_id, std::string path);
// changeTexture builds a new rectangle picture. Tie it back to the button
// so a disabled pager can show the forbidden icon, and the color chip can
// stay white while its texture is still the empty placeholder.
void osd_attach_button_picture(Object* render);
// changeTexture already points the button at the new picture. Reuse the
// quad that is on screen instead of stacking a second texture on it.
// Returns true when that quad was retargeted and no new widget is needed.
bool osd_claim_button_icon(Object* render, float x, float y, uint32_t icon_id,
                           const std::string& path);
// Slider title is createText'd after the track button (empty tip, has icon).
// Bind that string so hover can turn it white (Gadget HL_LABELCOLOR).
void osd_attach_row_label(const std::string& label, float y);
// changeLabelText on a row that was created empty. Binds the Text to the
// hit box at the same height so hover can turn the new name white.
void osd_sync_button_label(Object* osd, void* text, float y,
                           const std::string& label);

// Framebuffer size for Osd.vpAspect (W/H). Call after render.open / resize.
void osd_set_framebuffer(int32_t width, int32_t height);
float osd_framebuffer_aspect();  // width/height, default 4/3
int32_t osd_framebuffer_width();
int32_t osd_framebuffer_height();

// Fired when client size actually changes (Frontend.setFonts / icon rescale).
using OsdFramebufferFn = std::function<void(int32_t width, int32_t height)>;
void osd_set_framebuffer_listener(OsdFramebufferFn fn);

// Group ids mirror Osd.beginGroup/endGroup (first group = 0).
// True after hideGroup, until showGroup. Widgets and texts created while
// the group is already closed (car selector, driver list) start hidden.
bool osd_group_is_hidden(int32_t group);
int32_t osd_current_group();
int32_t osd_begin_group();
int32_t osd_end_group();
void osd_hide_group(int32_t group);
void osd_show_group(int32_t group);
// Menu page: hide every other group (widgets + texts), show `group`.
void osd_show_only_group(int32_t group);
// Group that owns a button cmd, or -1.
int32_t osd_group_of_cmd(int32_t cmd);

// Hit-test: widget index or -1 (miss). cmd==0 is valid (scroll-up).
int32_t osd_hit_index(float osd_x, float osd_y);
const OsdWidget* osd_widget_at(int32_t index);
void osd_mark_disabled(int32_t cmd);
void osd_set_cmd_disabled(int32_t cmd, bool disabled);
void osd_hide_cmd(int32_t cmd);
// Dialog.show plants a fullscreen button (cmd 9999999) on a second OSD.
// Closing the plate must drop that shield, or NO leaves the menu covered.
void osd_drop_modal_shield();

// Hover (Gadget.focus / HL_BACKLIGHT). Returns true when pointed widget changes.
bool osd_set_hover_at(float osd_x, float osd_y);
int32_t osd_hover_index();
const OsdWidget* osd_hover_widget();

// HIGHLIGHT_NORMAL = frontend:0x012B (Gadget.java), drawn at ×1.1 under icon.
void osd_set_highlight_path(std::string path);
const std::string& osd_highlight_path();
void osd_set_disabled_highlight_path(std::string path);
const std::string& osd_disabled_highlight_path();
constexpr float kOsdHighlightScale = 1.1f;

using OsdClickFn = std::function<void(const OsdWidget& w)>;
void osd_set_click_handler(OsdClickFn fn);
// Mouse moved onto another gadget (or off the bar). The script highlight
// only moves when Gadget.focus/unfocus run; EC_HOVER is not dispatched.
using OsdHoverFn = std::function<void(const OsdWidget* w)>;
void osd_set_hover_handler(OsdHoverFn fn);
void osd_fire_click_at(float osd_x, float osd_y);
using OsdSliderFn = std::function<bool(const OsdWidget&, float, float)>;
void osd_set_slider_handler(OsdSliderFn fn);
bool osd_update_slider_at(int32_t index, float x, float y);
Object* osd_inventory_drag_source_at(float x, float y);
bool osd_fire_inventory_drop_at(Object* source, float x, float y);

// Show or hide every widget and text that belongs to this Osd instance.
void osd_set_owner_visible(Object* osd, bool visible);
// LoadingScreen.show builds the plate before any frame is pumped, and that
// plate was staying hidden under the menu. Keep Loading.dds on top.
void osd_ensure_loading_plate(Object* osd, const std::string& path);
// Drawn after every OSD widget, so a hidden group cannot cover it.
void osd_set_loading_overlay(const std::string& path);
const std::string& osd_loading_overlay();
// Dealer welcome keeps its own plate, so the overlay path is empty, but the
// cursor is still over the city button that opened it.
void osd_set_loading_dialog(bool on);
bool osd_loading_dialog();
// Last widget is a string field. Focus it so keys edit osd.stringValue.
void osd_mark_string_input(Object* gadget, Object* value, void* text_key,
                           const std::string& input);
// StringInput.key: backspace 8, enter 13, escape 27, length limit 16.
void osd_on_char(int ascii);

// Pixel → OSD text coords (y+ down, origin center).
void osd_pixel_to_text(int32_t px, int32_t py, int32_t fb_w, int32_t fb_h,
                       float* out_x, float* out_y);

// Java putfield on Group / Gadget / Viewport. Assigns host group ids,
// binds Button.cmd onto the physics hit box, and wires string fields.
void osd_on_field_write(Object* recv, const std::string& field);
// Group.setFlags(WORLDTREELEAF) hides that group; clearFlags shows it.
void osd_on_group_leaf(Object* group, bool leaf);

}  // namespace slrr
