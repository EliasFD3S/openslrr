#include "slrr/light_resource.hpp"
#include "slrr/rpk_index.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

namespace slrr {
namespace {
std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> tokens(std::string_view s) {
  std::vector<std::string_view> out;
  while (true) {
    s = trim(s);
    if (s.empty())
      return out;
    size_t end = 0;
    while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])))
      ++end;
    out.push_back(s.substr(0, end));
    s.remove_prefix(end);
  }
}

bool number(std::string_view token, float &out) {
  if (!token.empty() && token.front() == '+')
    token.remove_prefix(1);
  float value = 0.f;
  const auto parsed =
      std::from_chars(token.data(), token.data() + token.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
      !std::isfinite(value))
    return false;
  out = value;
  return true;
}

// Reads `count` numbers starting at `first`; assigns only when all parse.
template <size_t N>
bool numbers(const std::vector<std::string_view> &t, size_t first,
             std::array<float, N> &out) {
  if (t.size() < first + N)
    return false;
  std::array<float, N> values{};
  for (size_t i = 0; i < N; ++i)
    if (!number(t[first + i], values[i]))
      return false;
  out = values;
  return true;
}

// `diffuse a r g b`: keep r g b. Three values are read as r g b.
bool color(const std::vector<std::string_view> &t, std::array<float, 3> &out) {
  std::array<float, 4> argb{};
  if (numbers(t, 1, argb)) {
    out = {argb[1], argb[2], argb[3]};
    return true;
  }
  return numbers(t, 1, out);
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char &c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
} // namespace

std::optional<LightResource> parse_light_resource(std::string_view text) {
  LightResource out;
  bool any = false;
  while (!text.empty()) {
    size_t end = text.find_first_of("\r\n");
    std::string_view line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (const auto comment = line.find("//"); comment != std::string_view::npos)
      line = line.substr(0, comment);
    if (const auto comment = line.find(';'); comment != std::string_view::npos)
      line = line.substr(0, comment);
    if (const auto nul = line.find('\0'); nul != std::string_view::npos)
      line = line.substr(0, nul);
    const auto t = tokens(line);
    if (t.empty())
      continue;
    const std::string key = lower(t[0]);
    if (key == "type") {
      std::string value;
      for (size_t i = 1; i < t.size(); ++i)
        value += lower(t[i]);  // "fake point" == "fakepoint"
      out.type = value == "directional" ? LightResourceType::Directional
                 : value == "point"     ? LightResourceType::Point
                 : value == "spot"      ? LightResourceType::Spot
                 : value == "fakepoint" ? LightResourceType::FakePoint
                 : value == "fakespot"  ? LightResourceType::FakeSpot
                                        : LightResourceType::Unknown;
      any = true;
    } else if (key == "diffuse") {
      any |= color(t, out.diffuse);
    } else if (key == "specular") {
      any |= color(t, out.specular);
    } else if (key == "ambient") {
      any |= color(t, out.ambient);
    } else if (key == "position") {
      any |= numbers(t, 1, out.position);
    } else if (key == "direction") {
      any |= numbers(t, 1, out.direction);
    } else if (key == "attenuation") {
      std::array<float, 3> a{};
      if (numbers(t, 1, a) && a[0] >= 0.f && a[1] >= 0.f && a[2] >= 0.f) {
        out.attenuation = a;
        any = true;
      }
    } else if (key == "range" || key == "theta" || key == "phi") {
      float value = 0.f;
      if (t.size() > 1 && number(t[1], value) && value >= 0.f) {
        (key == "range" ? out.range : key == "theta" ? out.theta : out.phi) =
            value;
        any = true;
      }
    } else if (key == "lit_dynamic") {
      float value = 0.f;
      if (t.size() > 1 && number(t[1], value))
        out.lit_dynamic = value != 0.f;
    } else if (key.starts_with("lf_")) {
      out.has_flare = true;
    }
  }
  if (!any)
    return std::nullopt;
  return out;
}

bool is_light_resource(const RpkIndex &rpk, uint32_t full_id) {
  const auto *entry = full_id ? rpk.resolve(full_id) : nullptr;
  return entry && entry->typeof_ == 13;
}

std::optional<LightResource> load_light_resource(const RpkIndex &rpk,
                                                 uint32_t full_id) {
  if (!is_light_resource(rpk, full_id))
    return std::nullopt;
  const auto bytes = rpk.read_payload(full_id);
  return parse_light_resource(
      {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}
} // namespace slrr
