#include "slrr/gc_roots.hpp"
#include "slrr/slrr_font.hpp"

#include "slrr/object.hpp"
#include "slrr/osd_registry.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#if defined(SLRR_HAS_LIBBGFX)
// Implementation lives in ptx_image.cpp (STB_IMAGE_IMPLEMENTATION).
#include <stb/stb_image.h>
#endif

namespace slrr {
namespace {

std::string g_root;
std::unordered_map<void*, SlrrFont> g_fonts;

struct TextState {
  void* font = nullptr;
  float x = 0.f;
  float y = 0.f;
  uint32_t color = 0xFFFFFFFFu;
  int32_t align = 2;
  int32_t group = 0;
  SlrrFontSlot font_slot = SlrrFontSlot::None;
  std::string text;
  bool alive = false;
  bool visible = true;
  bool pinned = false;
  bool behind_ui = false;
  void* osd = nullptr;
  bool osd_suppressed = false;
  bool group_hidden = false;
  int32_t layer = 0;
};
std::unordered_map<void*, TextState> g_texts;
std::vector<SlrrOsdText> g_osd_text;

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto sz = f.tellg();
  if (sz <= 0 || sz > 32 * 1024 * 1024) return false;
  f.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(out->data()), sz);
  return static_cast<bool>(f) || f.eof();
}

std::string join_root(const std::string& rel) {
  if (g_root.empty()) return rel;
  std::string r = g_root;
  for (char& c : r)
    if (c == '/') c = '\\';
  while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
  std::string p = rel;
  for (char& c : p)
    if (c == '/') c = '\\';
  return r + "\\" + p;
}

int32_t font_px_height_from_name(std::string n) {
  for (char& c : n) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (n.find("simple40") != std::string::npos) return 40;
  if (n.find("slii24") != std::string::npos) return 24;
  if (n.find("simple20") != std::string::npos) return 20;
  if (n.find("slii17") != std::string::npos) return 15;
  if (n.find("slii11") != std::string::npos) return 10;
  if (n.find("console10") != std::string::npos) return 10;
  if (n.find("console5") != std::string::npos) return 7;
  if (n.find("sl28") != std::string::npos) return 28;
  if (n.find("sl14") != std::string::npos) return 14;
  return 20;
}

// INVO v3 charset mesh. Glyph c = vertices c*4..c*4+3. Position is tenths of
// a pixel (x * 0.1), UV at +24. Advance is vertex 1's x. Index by byte code.
bool parse_font_glyphs(const uint8_t* scx, size_t scx_size,
                       std::vector<SlrrFontGlyph>* out) {
  if (!scx || !out || scx_size < 0x48) return false;
  out->clear();
  if (std::memcmp(scx, "INVO", 4) != 0) return false;
  uint32_t ver = 0, hdr = 0, vcount = 0;
  std::memcpy(&ver, scx + 4, 4);
  std::memcpy(&hdr, scx + 8, 4);
  if (ver != 3 || hdr < 0x2c || hdr > 0x1000) return false;
  if (8u + static_cast<size_t>(hdr) + 4 > scx_size) return false;
  size_t stride = 44;
  if (hdr > 0x3c) {
    uint32_t s = 0;
    std::memcpy(&s, scx + 8 + 0x3c, 4);
    stride = s;
  }
  if (stride < 32 || stride > 256) return false;
  std::memcpy(&vcount, scx + 8 + hdr, 4);
  if (vcount < 4 || (vcount % 4) != 0 || vcount > 65536) return false;
  const size_t vstart = 8u + static_cast<size_t>(hdr) + 4;
  const uint64_t need = static_cast<uint64_t>(vstart) +
                        static_cast<uint64_t>(vcount) * stride;
  if (need > scx_size) return false;

  const size_t nglyphs = static_cast<size_t>(vcount / 4);
  out->reserve(nglyphs);
  int ok = 0;
  auto finite = [](float f) { return f == f && f > -1e9f && f < 1e9f; };
  for (size_t g = 0; g < nglyphs; ++g) {
    SlrrFontGlyph glyph;
    for (int i = 0; i < 4; ++i) {
      const size_t off = vstart + (g * 4 + static_cast<size_t>(i)) * stride;
      float x = 0, y = 0, u = 0, v = 0;
      std::memcpy(&x, scx + off, 4);
      std::memcpy(&y, scx + off + 4, 4);
      std::memcpy(&u, scx + off + 24, 4);
      std::memcpy(&v, scx + off + 28, 4);
      glyph.px[i] = finite(x) ? x * 0.1f : 0.f;
      glyph.py[i] = finite(y) ? y * 0.1f : 0.f;
      glyph.tu[i] = finite(u) ? u : 0.f;
      glyph.tv[i] = finite(v) ? v : 0.f;
    }
    glyph.adv_px = glyph.px[1];
    float umin = 2.f, umax = -1.f, vmin = 2.f, vmax = -1.f;
    for (int i = 0; i < 4; ++i) {
      const float u = glyph.tu[i], v = glyph.tv[i];
      if (!(u >= 0.f && u <= 1.01f && v >= 0.f && v <= 1.01f)) {
        umin = umax = vmin = vmax = 0.f;
        break;
      }
      if (u < umin) umin = u;
      if (u > umax) umax = u;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    glyph.u0 = umin;
    glyph.v0 = vmin;
    glyph.u1 = umax;
    glyph.v1 = vmax;
    out->push_back(glyph);
    if ((umax - umin) > 0.002f && (vmax - vmin) > 0.015f) ++ok;
  }
  return ok >= 32;
}

bool decode_tga_font_atlas(const uint8_t* data, size_t size, SlrrFont* font) {
  if (!data || !font || size < 18) return false;
  const uint8_t id_len = data[0];
  const uint8_t cmap_type = data[1];
  const uint8_t img_type = data[2];
  const uint16_t cmap_first =
      static_cast<uint16_t>(data[3] | (data[4] << 8));
  const uint16_t cmap_len =
      static_cast<uint16_t>(data[5] | (data[6] << 8));
  const uint8_t cmap_entry_bits = data[7];
  const uint16_t w = static_cast<uint16_t>(data[12] | (data[13] << 8));
  const uint16_t h = static_cast<uint16_t>(data[14] | (data[15] << 8));
  const uint8_t bpp = data[16];
  const uint8_t desc = data[17];
  if (w == 0 || h == 0 || bpp != 8) return false;
  if (img_type != 3 && img_type != 1) return false;
  if (img_type == 1 && (cmap_type != 1 || cmap_len == 0 || cmap_entry_bits < 24))
    return false;

  const size_t cmap_bytes =
      img_type == 1 ? static_cast<size_t>(cmap_len) * (cmap_entry_bits / 8) : 0;
  const size_t header = 18u + id_len + cmap_bytes;
  const size_t need = header + static_cast<size_t>(w) * h;
  if (size < need) return false;

  uint8_t alpha_lut[256];
  for (int i = 0; i < 256; ++i) alpha_lut[i] = static_cast<uint8_t>(i);
  if (img_type == 1) {
    std::memset(alpha_lut, 0, sizeof(alpha_lut));
    const uint8_t* pal = data + 18u + id_len;
    const int epb = cmap_entry_bits / 8;
    for (uint16_t i = 0; i < cmap_len && i < 256; ++i) {
      const uint8_t* e = pal + static_cast<size_t>(i) * epb;
      const int a = (static_cast<int>(e[2]) + e[1] + e[0]) / 3;
      const int idx = static_cast<int>(cmap_first) + i;
      if (idx >= 0 && idx < 256) alpha_lut[idx] = static_cast<uint8_t>(a);
    }
  }

  font->atlas_w = w;
  font->atlas_h = h;
  font->rgba.assign(static_cast<size_t>(w) * h * 4, 0);
  const bool top_left = (desc & 0x20) != 0;
  for (uint16_t y = 0; y < h; ++y) {
    const uint16_t src_y = top_left ? y : static_cast<uint16_t>(h - 1 - y);
    const uint8_t* src = data + header + static_cast<size_t>(src_y) * w;
    uint8_t* dst = font->rgba.data() + static_cast<size_t>(y) * w * 4;
    for (uint16_t x = 0; x < w; ++x) {
      const uint8_t a = alpha_lut[src[x]];
      dst[x * 4 + 0] = 0xff;
      dst[x * 4 + 1] = 0xff;
      dst[x * 4 + 2] = 0xff;
      dst[x * 4 + 3] = a;
    }
  }
  return true;
}

bool decode_png_font_atlas(const uint8_t* data, size_t size, SlrrFont* font) {
#if defined(SLRR_HAS_LIBBGFX)
  if (!data || !font || size < 8) return false;
  int w = 0, h = 0, comp = 0;
  stbi_uc* img =
      stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
  if (!img || w <= 0 || h <= 0) {
    if (img) stbi_image_free(img);
    return false;
  }
  font->atlas_w = w;
  font->atlas_h = h;
  font->rgba.resize(static_cast<size_t>(w) * h * 4);

  // SLII*.png = white fill + black outline, coverage in alpha — keep RGB.
  // simple20/40.png = white RGB, the letter is the alpha. Using luma as
  // alpha paints the whole cell (luma is 255), which is the white blocks
  // where the build line should be.
  bool outline_atlas = false;
  bool alpha_coverage = false;
  const int n = w * h;
  int white_clear = 0, white_ink = 0;
  for (int i = 0; i < n; ++i) {
    const uint8_t r = img[i * 4 + 0];
    const uint8_t g = img[i * 4 + 1];
    const uint8_t b = img[i * 4 + 2];
    const uint8_t a = img[i * 4 + 3];
    const int luma = (static_cast<int>(r) + g + b) / 3;
    if (luma < 40 && a > 16) outline_atlas = true;
    if (luma > 240 && a < 16) ++white_clear;
    else if (luma > 240 && a > 16) ++white_ink;
  }
  if (!outline_atlas && white_clear > 64 && white_ink > 64)
    alpha_coverage = true;

  for (int i = 0; i < n; ++i) {
    uint8_t r = img[i * 4 + 0];
    uint8_t g = img[i * 4 + 1];
    uint8_t b = img[i * 4 + 2];
    uint8_t a = img[i * 4 + 3];
    if (alpha_coverage) {
      r = g = b = 0xff;
    } else if (!outline_atlas) {
      // Greyscale coverage → white RGB + luma alpha (tintable via u_color).
      a = static_cast<uint8_t>((static_cast<int>(r) + g + b) / 3);
      r = g = b = 0xff;
    }
    font->rgba[static_cast<size_t>(i) * 4 + 0] = r;
    font->rgba[static_cast<size_t>(i) * 4 + 1] = g;
    font->rgba[static_cast<size_t>(i) * 4 + 2] = b;
    font->rgba[static_cast<size_t>(i) * 4 + 3] = a;
  }
  stbi_image_free(img);
  std::printf("[font] png atlas %dx%d outline=%d alpha=%d\n", w, h,
              outline_atlas ? 1 : 0, alpha_coverage ? 1 : 0);
  return true;
#else
  (void)data;
  (void)size;
  (void)font;
  return false;
#endif
}

}  // namespace

namespace {
void* g_ui_small = nullptr;
void* g_ui_medium = nullptr;
void* g_ui_large = nullptr;
}  // namespace

void slrr_font_set_game_root(const std::string& root) { g_root = root; }

void slrr_font_set_ui_keys(void* small, void* medium, void* large) {
  g_ui_small = small;
  g_ui_medium = medium;
  g_ui_large = large;
}

void* slrr_font_ui_small() { return g_ui_small; }
void* slrr_font_ui_medium() { return g_ui_medium; }
void* slrr_font_ui_large() { return g_ui_large; }

const char* slrr_font_name_for_rid(int32_t rid) {
  // Text.java RID_* → stem under frontend/meshes|textures/Fonts/
  const uint32_t local = static_cast<uint32_t>(rid) & 0xFFFFu;
  switch (local) {
    case 0x0020:
      return "simple20";
    case 0x0022:
      return "simple40";
    case 0x0023:
      return "console10";
    case 0x0025:
      return "slii24";
    case 0x002E:
      return "sl28";
    case 0x008C:
      return "sl14";
    case 0x00B2:
      return "console5";
    case 0x0138:
      return "slii11";
    case 0x0139:
      return "slii17";
    case 0x003C:
      return "symbol_pointers";
    default:
      return nullptr;
  }
}

bool slrr_font_load(void* key, const char* name) {
  if (!key || !name || !name[0]) return false;
  std::string n = name;
  {
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
  }
  std::string up = n;
  for (char& c : up) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }

  std::vector<uint8_t> scx_bytes, atlas_bytes;
  const std::string scx_path =
      join_root("frontend\\meshes\\Fonts\\" + n + ".scx");
  if (!read_file(scx_path, &scx_bytes)) {
    std::printf("[font] scx miss %s\n", scx_path.c_str());
    return false;
  }

  std::string atlas_path;
  auto try_atlas = [&](const std::string& rel) -> bool {
    atlas_bytes.clear();
    const std::string p = join_root(rel);
    if (!read_file(p, &atlas_bytes) || atlas_bytes.empty()) return false;
    atlas_path = p;
    return true;
  };
  // The game names the PNG. A sibling .tga is not the charset.
  const bool got_atlas =
      try_atlas("frontend\\textures\\Fonts\\" + n + ".png") ||
      try_atlas("frontend\\textures\\Fonts\\" + up + ".png") ||
      try_atlas("frontend\\textures\\Fonts\\" + n + ".tga") ||
      try_atlas("frontend\\textures\\Fonts\\" + up + ".tga");
  if (!got_atlas) {
    std::printf("[font] atlas miss for %s\n", n.c_str());
    return false;
  }

  SlrrFont st;
  st.name = n;
  st.px_height = font_px_height_from_name(n);
  const bool png =
      atlas_path.size() >= 4 &&
      (atlas_path[atlas_path.size() - 4] == '.') &&
      (atlas_path[atlas_path.size() - 3] == 'p' ||
       atlas_path[atlas_path.size() - 3] == 'P') &&
      (atlas_path[atlas_path.size() - 2] == 'n' ||
       atlas_path[atlas_path.size() - 2] == 'N') &&
      (atlas_path[atlas_path.size() - 1] == 'g' ||
       atlas_path[atlas_path.size() - 1] == 'G');
  const bool atlas_ok =
      png ? decode_png_font_atlas(atlas_bytes.data(), atlas_bytes.size(), &st)
          : decode_tga_font_atlas(atlas_bytes.data(), atlas_bytes.size(), &st);
  if (!atlas_ok) {
    std::printf("[font] atlas decode fail %s\n", atlas_path.c_str());
    return false;
  }
  if (!parse_font_glyphs(scx_bytes.data(), scx_bytes.size(), &st.glyphs)) {
    std::printf("[font] glyph parse fail %s\n", scx_path.c_str());
    return false;
  }
  for (SlrrFontGlyph& g : st.glyphs) {
    if (g.adv_px < 0.f) g.adv_px = 0.f;
  }
  st.ready = !st.glyphs.empty();
  g_fonts[key] = std::move(st);
  std::printf("[font] %s atlas=%s glyphs=%zu px=%d\n", n.c_str(),
              atlas_path.c_str(), g_fonts[key].glyphs.size(),
              g_fonts[key].px_height);
  return g_fonts[key].ready;
}

bool slrr_font_load_from_rid(void* key, int32_t rid) {
  const char* name = slrr_font_name_for_rid(rid);
  if (!name) return false;
  return slrr_font_load(key, name);
}

bool slrr_font_ready(void* key) {
  auto it = g_fonts.find(key);
  return it != g_fonts.end() && it->second.ready;
}

const SlrrFont* slrr_font_get(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return nullptr;
  return &it->second;
}

float slrr_font_measure_px(void* key, const char* text) {
  const SlrrFont* font = slrr_font_get(key);
  if (!font || !text) return 0.f;
  float w = 0.f;
  for (const char* p = text; *p; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    const size_t gi = static_cast<size_t>(ch);
    if (gi < font->glyphs.size() && font->glyphs[gi].adv_px > 0.f)
      w += font->glyphs[gi].adv_px;
    else if (ch == ' ')
      w += static_cast<float>(font->px_height) * 0.35f;
  }
  return w;
}

void slrr_text_create(void* key, void* font, float x, float y) {
  if (!key) return;
  TextState& t = g_texts[key];
  t.font = font;
  t.x = x;
  t.y = y;
  t.color = 0xFFFFFFFFu;
  t.align = 2;
  t.group = 0;
  t.font_slot = SlrrFontSlot::None;
  t.text.clear();
  t.alive = true;
  t.visible = true;
}

void slrr_text_set_group(void* key, int32_t group) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.group = group;
  const bool closed = !it->second.pinned && osd_group_is_hidden(group);
  if (closed) {
    it->second.group_hidden = true;
    it->second.visible = false;
  }
  for (auto& ot : g_osd_text) {
    if (ot.text_key != key) continue;
    ot.group = group;
    if (closed) {
      ot.group_hidden = true;
      ot.visible = false;
    }
  }
}

void slrr_text_set_pinned(void* key, bool pinned) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.pinned = pinned;
  if (pinned) it->second.visible = true;
  for (auto& ot : g_osd_text) {
    if (ot.text_key != key) continue;
    ot.pinned = pinned;
    if (pinned) ot.visible = true;
  }
}

void slrr_text_set_visible(void* key, bool visible) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.visible = visible;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.visible = visible;
  }
}

int32_t text_osd_layer(void* osd) {
  auto* o = static_cast<Object*>(osd);
  if (!o) return 0;
  auto vp = o->fields.find("vp");
  if (vp == o->fields.end() || vp->second.tag != ValueTag::Obj || !vp->second.v.o)
    return 0;
  auto pri = vp->second.v.o->fields.find("pri");
  if (pri == vp->second.v.o->fields.end() || pri->second.tag != ValueTag::Int)
    return 0;
  return pri->second.v.i;
}

void slrr_text_set_osd(void* key, void* osd) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.osd = osd;
  it->second.layer = text_osd_layer(osd);
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) {
      ot.osd = osd;
      ot.layer = it->second.layer;
    }
  }
}

void slrr_text_set_osd_visible(void* osd, bool visible) {
  if (!osd) return;
  for (auto& kv : g_texts) {
    if (kv.second.osd != osd) continue;
    kv.second.osd_suppressed = !visible;
    if (kv.second.pinned) {
      // Pinned survives hideGroup (menu version lines). Hiding the whole
      // OSD must still drop them, or the build line stays in the garage
      // and DAY/TIME stay on the main menu.
      kv.second.visible = visible;
      continue;
    }
    kv.second.visible = visible && !kv.second.group_hidden;
  }
  for (auto& ot : g_osd_text) {
    if (ot.osd != osd) continue;
    ot.osd_suppressed = !visible;
    if (ot.pinned) {
      ot.visible = visible;
      continue;
    }
    ot.visible = visible && !ot.group_hidden;
  }
}

void slrr_text_set_group_visible(int32_t group, bool visible) {
  for (auto& kv : g_texts) {
    if (kv.second.group != group || kv.second.pinned) continue;
    kv.second.group_hidden = !visible;
    if (visible && kv.second.osd_suppressed) continue;
    kv.second.visible = visible && !kv.second.osd_suppressed;
  }
  for (auto& ot : g_osd_text) {
    if (ot.group != group || ot.pinned) continue;
    ot.group_hidden = !visible;
    if (visible && ot.osd_suppressed) continue;
    ot.visible = visible && !ot.osd_suppressed;
  }
}

void slrr_text_show_only_group(int32_t group) {
  for (auto& kv : g_texts) {
    if (kv.second.pinned) continue;
    kv.second.group_hidden = kv.second.group != group;
    if (kv.second.osd_suppressed) {
      kv.second.visible = false;
      continue;
    }
    kv.second.visible = !kv.second.group_hidden;
  }
  for (auto& ot : g_osd_text) {
    if (ot.pinned) continue;
    ot.group_hidden = ot.group != group;
    if (ot.osd_suppressed) {
      ot.visible = false;
      continue;
    }
    ot.visible = !ot.group_hidden;
  }
}

void slrr_text_destroy(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  g_texts.erase(key);
}

void slrr_text_set_color(void* key, uint32_t argb) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.color = argb;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.color = argb;
  }
}

void slrr_text_set_align(void* key, int32_t align) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.align = align;
  // changeText publishes the line before changeAlign. The drawn copy
  // kept ALIGN_LEFT, so a right-aligned column started at the anchor
  // and ran into the other column.
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.align = align;
  }
}

void slrr_text_set_behind_ui(void* key, bool behind) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.behind_ui = behind;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.behind_ui = behind;
  }
}

void slrr_text_set_pos(void* key, float x, float y) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.x = x;
  it->second.y = y;
  for (auto& ot : g_osd_text) {
    if (ot.text_key != key) continue;
    ot.x = x;
    ot.y = y;
  }
}

void slrr_text_set_string(void* key, const char* utf8) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.text = utf8 ? utf8 : "";
}

void slrr_text_set_font(void* key, void* font) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.font = font;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.font_key = font;
  }
}

void slrr_text_set_font_slot(void* key, SlrrFontSlot slot) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.font_slot = slot;
}

SlrrFontSlot slrr_text_font_slot(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return SlrrFontSlot::None;
  return it->second.font_slot;
}

void slrr_text_rebind_frontend_fonts(void* small, void* medium, void* large) {
  for (auto& kv : g_texts) {
    TextState& t = kv.second;
    void* font = nullptr;
    if (t.font_slot == SlrrFontSlot::Small)
      font = small;
    else if (t.font_slot == SlrrFontSlot::Medium)
      font = medium;
    else if (t.font_slot == SlrrFontSlot::Large)
      font = large;
    else
      continue;
    if (!font || font == t.font) continue;
    t.font = font;
    for (auto& ot : g_osd_text) {
      if (ot.text_key == kv.first) ot.font_key = font;
    }
  }
}

void slrr_text_update(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return;
  const TextState& t = it->second;
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  if (t.text.empty() || !t.font) return;
  SlrrOsdText ot;
  ot.text_key = key;
  ot.font_key = t.font;
  ot.x = t.x;
  ot.y = t.y;
  ot.color = t.color;
  ot.align = t.align;
  ot.group = t.group;
  ot.text = t.text;
  ot.visible = t.visible;
  ot.pinned = t.pinned;
  ot.behind_ui = t.behind_ui;
  ot.osd = t.osd;
  ot.osd_suppressed = t.osd_suppressed;
  ot.group_hidden = t.group_hidden;
  ot.layer = t.layer;
  g_osd_text.push_back(std::move(ot));
}

void slrr_text_clear_all() {
  g_osd_text.clear();
  g_texts.clear();
}

const std::vector<SlrrOsdText>& slrr_osd_texts() { return g_osd_text; }


void font_visit_gc_roots(const GcRootVisitor& visit) {
  visit(g_ui_small); visit(g_ui_medium); visit(g_ui_large);
  for (const auto& entry : g_fonts) visit(entry.first);
  for (const auto& entry : g_texts) {
    visit(entry.first); visit(entry.second.font); visit(entry.second.osd);
  }
  for (const auto& line : g_osd_text) {
    visit(line.text_key); visit(line.font_key); visit(line.osd);
  }
}
}  // namespace slrr
