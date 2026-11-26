#include "slrr/skid_marks.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <unordered_map>

namespace slrr {
namespace {

using V3 = std::array<double, 3>;
V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 scale(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(V3 a, V3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(V3 a) { return std::sqrt(dot(a, a)); }
V3 lerp(V3 a, V3 b, double t) { return add(a, scale(sub(b, a), t)); }

double smooth_fade(double distance, double metres) {
  const double t = std::clamp(distance / std::max(metres, .01), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

struct Segment {
  size_t index = 0;
  uint32_t version = 0;
  double start = 0, end = 0, alpha0 = 0, alpha1 = 0;
};
struct Track {
  bool has_prior = false;
  V3 prior{};
  double u = 0, hold = 0, timer = 0;  // u: track length in metres
  uint16_t style = 0;
  std::deque<Segment> segments;       // the last metres, re-faded on finish
};

std::vector<SkidMarkVertex> vertices;
std::vector<uint32_t> versions;
size_t cursor = 0;
uint64_t revision = 0;
std::unordered_map<uintptr_t, std::array<Track, 4>> tracks;
std::vector<SkidMarkStyle> styles;
std::unordered_map<uint32_t, uint16_t> style_keys;

const SkidMarkStyle &style_of(uint16_t index) {
  if (styles.empty()) styles.emplace_back();
  return styles[index < styles.size() ? index : 0];
}

// The tile length, and the metres a finish may re-fade or re-cap.
double tile_of(const SkidMarkStyle &style) { return std::max(.05, double(style.height)); }
double tail_of(const SkidMarkStyle &style) {
  return std::max(double(style.fade), style.texture.empty() ? 0.0 : tile_of(style));
}

void set_alpha(size_t index, double alpha0, double alpha1) {
  auto *v = &vertices[index * 4];
  v[0].alpha = v[1].alpha = static_cast<float>(alpha0);
  v[2].alpha = v[3].alpha = static_cast<float>(alpha1);
  ++revision;
}

void set_uv(size_t index, const std::array<float, 4> &uv, float v0, float v1) {
  auto *v = &vertices[index * 4];
  v[0].u = v[2].u = uv[0];
  v[1].u = v[3].u = uv[2];
  v[0].v = v[1].v = v0;
  v[2].v = v[3].v = v1;
  ++revision;
}

bool push_quad(V3 a, V3 b, V3 normal, V3 forward, double width, double alpha0, double alpha1,
               uint32_t color, uint16_t style, const std::array<float, 4> &uv, float v0,
               float v1, Segment &out) {
  V3 direction = sub(b, a);
  direction = sub(direction, scale(normal, dot(direction, normal)));
  if (dot(direction, direction) < 1e-5) direction = forward;
  const double dl = length(direction);
  if (!(dl > 1e-9)) return false;
  direction = scale(direction, 1 / dl);
  V3 across = cross(direction, normal);
  const double al = length(across);
  if (!(al > 1e-9)) return false;
  across = scale(across, width * .5 / al);
  // Above the road so depth testing keeps it; under any tyre.
  const V3 lift = scale(normal, .01);
  const V3 corners[4] = {add(sub(a, across), lift), add(add(a, across), lift),
                         add(sub(b, across), lift), add(add(b, across), lift)};
  if (vertices.size() < kSkidMarkQuads * 4) {
    vertices.resize(vertices.size() + 4);
    versions.push_back(0);
  }
  auto *v = &vertices[cursor * 4];
  for (int i = 0; i < 4; ++i) {
    v[i] = {static_cast<float>(corners[i][0]), static_cast<float>(corners[i][1]),
            static_cast<float>(corners[i][2]), 0.f, color};
    v[i].u = (i & 1) ? uv[2] : uv[0];
    v[i].v = i < 2 ? v0 : v1;
    v[i].style = style;
  }
  out.index = cursor;
  out.version = ++versions[cursor];
  set_alpha(cursor, alpha0, alpha1);
  cursor = (cursor + 1) % kSkidMarkQuads;
  return true;
}

float region_v(const std::array<float, 4> &uv, double f) {
  return static_cast<float>(uv[1] + (uv[3] - uv[1]) * std::clamp(f, 0.0, 1.0));
}

// One stretch of track from a to b. A mark splits at its texture tiles: the
// first tile is the end cap (uv_end, faint end first), the rest repeat
// uv_normal every `height` metres.
void add_mark(Track &track, const SkidWheelSample &wheel, V3 a, V3 b, double width,
              double opacity, bool stationary) {
  const auto &style = style_of(track.style);
  const double tile = tile_of(style);
  const double length_ab = length(sub(b, a));
  const double start = track.u, end = start + length_ab;
  double pos = start;
  do {
    const double index = std::floor(pos / tile + 1e-6);
    const double next = length_ab > 0 ? std::min(end, (index + 1) * tile) : end;
    const double t0 = length_ab > 0 ? (pos - start) / length_ab : 0;
    const double t1 = length_ab > 0 ? (next - start) / length_ab : 1;
    const auto &uv = index < .5 ? style.uv_end : style.uv_normal;
    const float v0 = region_v(uv, pos / tile - index);
    const float v1 = region_v(uv, next / tile - index);
    const double fade0 = smooth_fade(pos, style.fade), fade1 = smooth_fade(next, style.fade);
    const double alpha0 = (stationary ? std::max(.3, fade0) : fade0) * opacity;
    const double alpha1 = (stationary ? std::max(.42, fade1) : fade1) * opacity;
    Segment segment;
    if (push_quad(lerp(a, b, t0), lerp(a, b, t1), wheel.normal, wheel.forward, width, alpha0,
                  alpha1, wheel.mark_color, track.style, uv, v0, v1, segment)) {
      segment.start = pos;
      segment.end = next;
      segment.alpha0 = alpha0;
      segment.alpha1 = alpha1;
      track.segments.push_back(segment);
    }
    pos = next;
  } while (pos < end - 1e-6);
  track.u = end;
  const double tail = tail_of(style);
  while (!track.segments.empty() && end - track.segments.front().end > tail)
    track.segments.pop_front();
}

// Tapers the last metres so a track ends as softly as it starts; a textured
// mark closes with its end cap, mirrored.
void finish(Track &track) {
  const auto &style = style_of(track.style);
  const double end = track.u, tile = tile_of(style);
  for (const auto &s : track.segments) {
    if (s.index >= versions.size() || versions[s.index] != s.version) continue;
    set_alpha(s.index, s.alpha0 * smooth_fade(end - s.start, style.fade),
              s.alpha1 * smooth_fade(end - s.end, style.fade));
    if (!style.texture.empty() && s.start >= tile - 1e-6 && end - s.start <= tile + 1e-6)
      set_uv(s.index, style.uv_end, region_v(style.uv_end, (end - s.start) / tile),
            region_v(style.uv_end, (end - s.end) / tile));
  }
  track.segments.clear();
  track.has_prior = false;
  track.timer = track.u = 0;
}

std::vector<float> numbers(std::string_view text) {
  std::vector<float> out;
  const std::string s(text);
  const char *p = s.c_str();
  while (*p) {
    char *end = nullptr;
    const float value = std::strtof(p, &end);
    if (end == p) {
      ++p;
      continue;
    }
    if (std::isfinite(value)) out.push_back(value);
    p = end;
  }
  return out;
}

}  // namespace

double skid_scrub(const SkidWheelSample &wheel) {
  if (!wheel.contact || wheel.force < 300) return 0;
  const double longitudinal = std::abs(wheel.slip_ratio) > .25 ? std::abs(wheel.slip_speed) : 0;
  const double lateral = std::abs(wheel.lateral_speed) > 5.5 ? std::abs(wheel.lateral_speed) : 0;
  return std::clamp((std::max(longitudinal, lateral) - 2) / 3, 0.0, 1.0);
}

double skid_mark_opacity(double speed) {
  return 1 - .68 * std::clamp((std::abs(speed) - 10) / 20, 0.0, 1.0);
}

void skid_marks_update(uintptr_t car, size_t wheel, const SkidWheelSample &sample, double dt,
                       double vehicle_speed) {
  if (wheel >= 4 || !(dt > 0)) return;
  auto &track = tracks[car][wheel];
  // Another ground's mark starts a new track (SLRR_rev's surface change).
  if (track.style != sample.style) {
    if (track.has_prior || !track.segments.empty()) finish(track);
    track.style = sample.style;
  }
  const double strength = skid_scrub(sample);
  const double opacity = skid_mark_opacity(vehicle_speed);
  // A short hold bridges a frame or two of grip inside one slide.
  track.hold = strength > 0 ? .22 : std::max(0.0, track.hold - dt);
  if (!sample.contact || !(track.hold > 0)) {
    if (track.has_prior || !track.segments.empty()) finish(track);
  } else {
    const V3 point = sample.point;
    const auto &style = style_of(track.style);
    // The cfg's width from a light to a full scrub, capped at 0.8 m
    // (MW_Mod's Skid_Grass asks for up to 2 m).
    const double width =
        std::min(.8, style.width_min + (style.width_max - style.width_min) * strength);
    if (!track.has_prior || length(sub(point, track.prior)) >= 2.5) {
      // A first sample, or a jump (teleport, reset): start a new track here.
      if (track.has_prior) finish(track);
      track.prior = point;
      track.has_prior = true;
    } else {
      const double distance = length(sub(point, track.prior));
      if (distance > .075) {
        const int steps = std::max(1, static_cast<int>(std::ceil(distance / .28)));
        for (int j = 1; j <= steps; ++j)
          add_mark(track, sample, lerp(track.prior, point, double(j - 1) / steps),
                   lerp(track.prior, point, double(j) / steps), width, opacity, false);
        track.prior = point;
      } else if (strength > 0 && track.timer <= 0) {
        // A brake-held burnout keeps the patch still while the tread scrubs.
        const V3 half = scale(sample.forward, .07);
        add_mark(track, sample, sub(point, half), add(point, half), width, opacity, true);
        track.timer = .14;
      }
    }
  }
  track.timer -= dt;
}

void skid_marks_forget(uintptr_t car) {
  const auto it = tracks.find(car);
  if (it == tracks.end()) return;
  for (auto &track : it->second) finish(track);
  tracks.erase(it);
}

SkidMarkStyle parse_skid_mark_config(std::string_view text) {
  SkidMarkStyle out;
  // particles/scripts/skids/sliding/asphalt.cfg, for keys a cfg leaves out.
  out.width_min = .3f;
  out.width_max = .4f;
  out.height = .3f;
  out.fade = .1f;
  out.uv_end = {.125f, 0.f, .375f, 1.f};
  out.uv_normal = {.625f, 0.f, .875f, 1.f};
  while (!text.empty()) {
    const size_t eol = text.find_first_of("\r\n");
    std::string_view line = text.substr(0, eol);
    text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
    if (const size_t c = line.find(';'); c != std::string_view::npos) line = line.substr(0, c);
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    size_t k = i;
    while (k < line.size() && !std::isspace(static_cast<unsigned char>(line[k]))) ++k;
    std::string key(line.substr(i, k - i));
    for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto v = numbers(line.substr(k));
    if (key == "maxcount" && !v.empty()) {
      out.maxcount = std::max(1, static_cast<int>(v[0]));
    } else if (key == "width" && !v.empty()) {
      out.width_min = std::max(.01f, v[0]);
      out.width_max = std::max(out.width_min, v.size() > 1 ? v[1] : v[0]);
    } else if (key == "height" && !v.empty()) {
      out.height = std::max(.05f, v[0]);
    } else if ((key == "uv_end" || key == "uv_normal") && v.size() >= 4) {
      auto &uv = key == "uv_end" ? out.uv_end : out.uv_normal;
      std::copy(v.begin(), v.begin() + 4, uv.begin());
    } else if (key == "fade" && !v.empty()) {
      out.fade = std::max(0.f, v[0]);
    }
  }
  return out;
}

uint16_t skid_mark_style_register(uint32_t key, SkidMarkStyle style) {
  if (styles.empty()) styles.emplace_back();
  if (const auto it = style_keys.find(key); it != style_keys.end()) return it->second;
  if (styles.size() >= 0xfffe) return 0;
  const auto index = static_cast<uint16_t>(styles.size());
  styles.push_back(std::move(style));
  style_keys.emplace(key, index);
  return index;
}

uint16_t skid_mark_style_find(uint32_t key) {
  const auto it = style_keys.find(key);
  return it == style_keys.end() ? 0xffff : it->second;
}

const std::vector<SkidMarkStyle> &skid_mark_styles() {
  if (styles.empty()) styles.emplace_back();
  return styles;
}

void skid_marks_clear() {
  tracks.clear();
  vertices.clear();
  versions.clear();
  cursor = 0;
  ++revision;
}

const std::vector<SkidMarkVertex> &skid_mark_vertices() { return vertices; }
uint64_t skid_marks_revision() { return revision; }

}  // namespace slrr
