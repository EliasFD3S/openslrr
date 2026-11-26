#include "slrr/skid_marks.hpp"
#include <algorithm>
#include <cmath>
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

// A mark's ends fade in and out over this many metres of track.
constexpr double kFadeMetres = .45;
double smooth_fade(double distance) {
  const double t = std::clamp(distance / kFadeMetres, 0.0, 1.0);
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
  std::deque<Segment> segments;       // the last kFadeMetres, re-faded on finish
};

std::vector<SkidMarkVertex> vertices;
std::vector<uint32_t> versions;
size_t cursor = 0;
uint64_t revision = 0;
std::unordered_map<uintptr_t, std::array<Track, 4>> tracks;

void set_alpha(size_t index, double alpha0, double alpha1) {
  auto *v = &vertices[index * 4];
  v[0].alpha = v[1].alpha = static_cast<float>(alpha0);
  v[2].alpha = v[3].alpha = static_cast<float>(alpha1);
  ++revision;
}

bool push_quad(V3 a, V3 b, V3 normal, V3 forward, double width, double alpha0, double alpha1,
               uint32_t color, Segment &out) {
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
  for (int i = 0; i < 4; ++i)
    v[i] = {static_cast<float>(corners[i][0]), static_cast<float>(corners[i][1]),
            static_cast<float>(corners[i][2]), 0.f, color};
  out.index = cursor;
  out.version = ++versions[cursor];
  set_alpha(cursor, alpha0, alpha1);
  cursor = (cursor + 1) % kSkidMarkQuads;
  return true;
}

void add_mark(Track &track, const SkidWheelSample &wheel, V3 a, V3 b, double width,
              double opacity, bool stationary) {
  const double start = track.u, end = start + length(sub(b, a));
  const double alpha0 = (stationary ? std::max(.3, smooth_fade(start)) : smooth_fade(start)) * opacity;
  const double alpha1 = (stationary ? std::max(.42, smooth_fade(end)) : smooth_fade(end)) * opacity;
  Segment segment;
  if (push_quad(a, b, wheel.normal, wheel.forward, width, alpha0, alpha1, wheel.mark_color,
                segment)) {
    segment.start = start;
    segment.end = end;
    segment.alpha0 = alpha0;
    segment.alpha1 = alpha1;
    track.segments.push_back(segment);
  }
  track.u = end;
  while (!track.segments.empty() && end - track.segments.front().end > kFadeMetres)
    track.segments.pop_front();
}

// Tapers the last metres so a track ends as softly as it starts.
void finish(Track &track) {
  const double end = track.u;
  for (const auto &s : track.segments)
    if (s.index < versions.size() && versions[s.index] == s.version)
      set_alpha(s.index, s.alpha0 * smooth_fade(end - s.start), s.alpha1 * smooth_fade(end - s.end));
  track.segments.clear();
  track.has_prior = false;
  track.timer = track.u = 0;
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
  const double strength = skid_scrub(sample);
  const double opacity = skid_mark_opacity(vehicle_speed);
  // A short hold bridges a frame or two of grip inside one slide.
  track.hold = strength > 0 ? .22 : std::max(0.0, track.hold - dt);
  if (!sample.contact || !(track.hold > 0)) {
    if (track.has_prior || !track.segments.empty()) finish(track);
  } else {
    const V3 point = sample.point;
    const double width = .15 + strength * .055;
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
