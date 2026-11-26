#include "slrr/ground_surface.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace slrr {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool equal_nocase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
           return std::tolower(x) == std::tolower(y);
         });
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string_view first_token(std::string_view s) {
  s = trim(s);
  size_t end = 0;
  while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
  return s.substr(0, end);
}

bool read_float(std::string_view s, float& out) {
  const std::string text(first_token(s));
  if (text.empty()) return false;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(value)) return false;
  out = value;
  return true;
}

bool read_hex(std::string_view s, uint32_t& out) {
  const std::string text(first_token(s));
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 16);
  if (end == text.c_str()) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

// The text SCM, after the original's ground config reader (sl-remastered
// ground_config.cpp): `material i` selects one material, `params name` every
// material of that name, and the value keys apply to the selection.
std::vector<GroundMaterialEntry> parse_text(std::string_view text) {
  std::vector<GroundMaterialEntry> out;
  std::vector<bool> selected;
  while (!text.empty()) {
    const size_t eol = text.find_first_of("\r\n");
    std::string_view line = text.substr(0, eol);
    text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
    if (const size_t comment = line.find("//"); comment != std::string_view::npos)
      line = line.substr(0, comment);
    line = trim(line);
    const std::string_view key_text = first_token(line);
    if (key_text.empty()) continue;
    const std::string key = lower(key_text);
    const std::string_view args = trim(line.substr(key_text.size()));
    if (key == "materials") {
      const long count = std::strtol(std::string(first_token(args)).c_str(), nullptr, 10);
      if (count < 0 || count > 100000) return {};
      out.assign(static_cast<size_t>(count), {});
      selected.assign(out.size(), false);
    } else if (key == "material") {
      const std::string_view index_text = first_token(args);
      char* end = nullptr;
      const std::string index_string(index_text);
      const long index = std::strtol(index_string.c_str(), &end, 10);
      std::fill(selected.begin(), selected.end(), false);
      if (end != index_string.c_str() && index >= 0 && static_cast<size_t>(index) < out.size()) {
        out[static_cast<size_t>(index)].name = std::string(trim(args.substr(index_text.size())));
        selected[static_cast<size_t>(index)] = true;
      }
    } else if (key == "params") {
      // The original stores the first word of each name (%31s).
      const std::string_view name = first_token(args);
      for (size_t i = 0; i < out.size(); ++i)
        selected[i] = !name.empty() && equal_nocase(first_token(out[i].name), name);
    } else if (key == "friction" || key == "rollres" || key == "collcoeff") {
      float value = 0;
      if (!read_float(args, value)) continue;
      for (size_t i = 0; i < out.size(); ++i) {
        if (!selected[i]) continue;
        if (key == "friction") {
          out[i].friction = std::max(0.f, value);
          out[i].has_friction = true;
        } else if (key == "rollres") {
          out[i].rollres = std::max(0.f, value);
          out[i].has_rolling = true;
        } else {
          out[i].collcoeff = std::max(0.f, value);
          out[i].has_bounce = true;
        }
      }
    } else if (key == "skid_smcolor" || key == "skid_sfx") {
      // Hex ids, as the original's %x; skid_SFX adds a volume scale (1).
      uint32_t id = 0;
      if (!read_hex(args, id)) continue;
      float scale = 1.f;
      if (key == "skid_sfx") {
        const std::string_view first = first_token(args);
        if (!read_float(args.substr(first.size()), scale)) scale = 1.f;
      }
      for (size_t i = 0; i < out.size(); ++i) {
        if (!selected[i]) continue;
        if (key == "skid_smcolor") {
          out[i].mark_color = id;
          out[i].has_mark_color = true;
        } else {
          out[i].slide_sound = id;
          out[i].slide_volume = std::max(0.f, scale);
        }
      }
    }
  }
  return out;
}

uint32_t rd32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// The compiled SCM (MP1, Physic_Lab, Caynon): names only, no values.
std::vector<GroundMaterialEntry> parse_binary(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 8) return {};
  const uint32_t count = rd32(bytes.data()), record = rd32(bytes.data() + 4);
  if (record < 8 || record > 256 || count > 100000 ||
      8 + uint64_t(count) * record > bytes.size())
    return {};
  std::vector<GroundMaterialEntry> out(count);
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = reinterpret_cast<const char*>(bytes.data() + 8 + size_t(i) * record + 4);
    out[i].name.assign(name, strnlen(name, record - 4));
  }
  return out;
}

bool has_any(const std::string& text, std::initializer_list<std::string_view> parts) {
  for (auto part : parts)
    if (text.find(part) != std::string::npos) return true;
  return false;
}

std::string file_name(std::string_view name) {
  // The file name only: a folder such as `Grass_Track/` says nothing.
  std::string text = lower(name);
  std::replace(text.begin(), text.end(), '\\', '/');
  if (const size_t slash = text.rfind('/'); slash != std::string::npos) text.erase(0, slash + 1);
  return text;
}

// A name that says paved: its texture is not asked.
bool paved(std::string_view name) {
  return has_any(file_name(name), {"road", "asfalt", "asphalt", "beton", "concrete", "tarmac",
                                   "street", "sidwalk", "sidewalk", "kerb", "curb"});
}

}  // namespace

std::vector<GroundMaterialEntry> parse_ground_materials(const std::vector<uint8_t>& bytes) {
  constexpr std::string_view kText = "materials";
  if (bytes.size() >= kText.size() &&
      equal_nocase({reinterpret_cast<const char*>(bytes.data()), kText.size()}, kText))
    return parse_text({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
  return parse_binary(bytes);
}

GroundSurface ground_surface_for_name(std::string_view name) {
  const std::string text = file_name(name);
  if (paved(text)) return {};  // `desert_road`, `grass_kerb`
  // Typical tyre-on-surface figures against dry asphalt, the extra
  // rolling resistance of the soft ground, and a mark the colour of the
  // ground churned up (OpenSLRR choices; the marks darken by multiplying).
  const auto surface = [](float friction, float rolling, uint32_t mark) {
    GroundSurface out;
    out.friction = friction;
    out.rolling = rolling;
    out.mark_color = mark;
    return out;
  };
  if (has_any(text, {"sand", "homok", "piasek", "desert"})) return surface(.6f, .15f, 0xFF9A8C74);
  if (has_any(text, {"gravel", "grvl", "kavics", "murva", "zwir", "schotter"}))
    return surface(.7f, .03f, 0xFF7C7870);
  // Stems that are grass only as a whole name: the stock city's lawns
  // (maps/textures/area.dds) and SLRR_rev's `fu.dds` (Hungarian grass).
  const std::string stem = text.substr(0, text.rfind('.'));
  if (has_any(text, {"grass", "gras", "trawa", "trafa", "trava", "gyep", "lawn", "turf"}) ||
      stem == "area" || stem == "fu")
    return surface(.65f, .08f, 0xFF6E6A52);
  if (has_any(text, {"dirt", "soil", "mud", "ziemia", "earth", "offroad"}))
    return surface(.7f, .04f, 0xFF6A5C4A);
  return {};
}

std::vector<GroundSurface> ground_surfaces(const std::vector<GroundMaterialEntry>& materials,
                                           const std::vector<std::string>& textures,
                                           float ground_friction) {
  // 00463220: a ground_friction at or below zero means 1.
  if (!(ground_friction > 0) || !std::isfinite(ground_friction)) ground_friction = 1.f;
  std::vector<GroundSurface> out(materials.size());
  for (size_t i = 0; i < materials.size(); ++i) {
    const auto& m = materials[i];
    GroundSurface guess = ground_surface_for_name(m.name);
    if (guess.friction == 1.f && guess.rolling == 0.f && !paved(m.name) && i < textures.size())
      guess = ground_surface_for_name(textures[i]);
    out[i].friction = (m.has_friction ? m.friction : guess.friction) * ground_friction;
    out[i].rolling = m.has_rolling ? m.rollres * kRollresCoefficient : guess.rolling;
    out[i].bounce = m.has_bounce ? m.collcoeff : guess.bounce;
    out[i].mark_color = m.has_mark_color ? m.mark_color : guess.mark_color;
    if (m.slide_sound) {
      out[i].slide_sound = m.slide_sound;
      out[i].slide_volume = m.slide_volume;
    }
  }
  return out;
}

}  // namespace slrr
