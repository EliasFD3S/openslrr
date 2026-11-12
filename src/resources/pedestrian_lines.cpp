#include "slrr/pedestrian_lines.hpp"
#include "slrr/format/walk.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace slrr {
namespace {

constexpr float kClearance = 0.3f;
constexpr int kCell = 64;

using V = WalkVec;

V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V lerp(V a, V b, float t) { return add(a, scale(sub(b, a), t)); }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float flat(V a, V b) { return std::hypot(a.x - b.x, a.z - b.z); }
float dist(V a, V b) { return std::sqrt(dot(sub(a, b), sub(a, b))); }
float cross_xz(V a, V b, V c) {
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
V norm_xz(V a) {
  a.y = 0;
  const float l = std::hypot(a.x, a.z);
  if (l < 1e-8f) return {0, 0, 1};
  return {a.x / l, 0, a.z / l};
}
float unit(uint32_t& rng) {
  rng = rng * 1664525u + 1013904223u;
  return (rng >> 8) * (1.f / 16777216.f);
}

struct Walk : WalkRow {
  Walk() = default;
  Walk(Walk&&) = default;
  Walk& operator=(Walk&&) = default;
  explicit Walk(WalkRow&& row) : WalkRow(std::move(row)) {}
  mutable bool poly_ready = false;
  mutable bool poly_ok = false;
  mutable std::vector<V> polygon;
  mutable bool walls_ready = false;
  mutable std::vector<std::pair<V, V>> walls;
};
using Rec = WalkRecord;

struct Boundary {
  std::vector<V> points;
  std::vector<float> lengths;
  float length = 0;
};

struct Gate {
  int junction = 0;
  V point;
};

struct Zone {
  int id = 0;
  Boundary sides[2];
  Gate gates[2];
  float length = 0;
};

struct Sample {
  int zone = 0;
  float t = 0;
  float lateral = 0;
  V position;
};

Boundary make_boundary(std::vector<V> points) {
  Boundary out;
  out.points = std::move(points);
  out.lengths.push_back(0);
  for (size_t i = 1; i < out.points.size(); ++i)
    out.lengths.push_back(out.lengths.back() + dist(out.points[i - 1], out.points[i]));
  out.length = out.lengths.back();
  return out;
}

V sample_boundary(const Boundary& side, float t) {
  if (side.points.empty()) return {};
  if (side.points.size() == 1) return side.points[0];
  const float d = std::clamp(t, 0.f, 1.f) * side.length;
  size_t i = 1;
  while (i + 1 < side.lengths.size() && side.lengths[i] < d) ++i;
  const float span = std::max(1e-6f, side.lengths[i] - side.lengths[i - 1]);
  return lerp(side.points[i - 1], side.points[i], (d - side.lengths[i - 1]) / span);
}

V zone_point(const Zone& zone, float t, float lateral) {
  const V a = sample_boundary(zone.sides[0], t);
  const V b = sample_boundary(zone.sides[1], t);
  const float width = flat(a, b);
  const float margin = std::min(0.5f, std::max(0.15f, kClearance / std::max(1e-6f, width)));
  const float blend = std::min(1.f, std::min(t, 1.f - t) * zone.length / 4.f);
  const float offset = std::clamp(0.5f + (lateral - 0.5f) * blend, margin, 1.f - margin);
  return lerp(a, b, offset);
}

V quad_end(const Walk& row, const std::vector<float>& q, int end) {
  if (end == 0) return {q[0] + row.origin[0], 0, q[1] + row.origin[1]};
  return {q[2] + row.origin[0], 0, q[3] + row.origin[1]};
}

const std::vector<V>* polygon_of(const Walk& row) {
  if (row.poly_ready) return row.poly_ok ? &row.polygon : nullptr;
  row.poly_ready = true;
  std::vector<std::pair<V, V>> edges;
  auto go = [&](auto& self, const Rec& rec) -> void {
    if (rec.kind == 23) return;
    if (rec.quad.size() == 4)
      edges.emplace_back(quad_end(row, rec.quad, 0), quad_end(row, rec.quad, 1));
    if (rec.child) self(self, *rec.child);
  };
  for (const Rec& rec : row.records) go(go, rec);
  if (edges.size() < 3) return nullptr;
  auto edge = edges[0];
  edges.erase(edges.begin());
  row.polygon = {edge.first, edge.second};
  while (!edges.empty()) {
    const V& tip = row.polygon.back();
    const auto found = std::find_if(edges.begin(), edges.end(), [&](const std::pair<V, V>& item) {
      return flat(item.first, tip) < 0.02f || flat(item.second, tip) < 0.02f;
    });
    if (found == edges.end()) return nullptr;
    row.polygon.push_back(flat(found->first, tip) < 0.02f ? found->second : found->first);
    edges.erase(found);
  }
  if (flat(row.polygon.front(), row.polygon.back()) > 0.02f) {
    row.polygon.clear();
    return nullptr;
  }
  row.polygon.pop_back();
  row.poly_ok = true;
  return &row.polygon;
}

const std::vector<std::pair<V, V>>& walls_of(const Walk& row) {
  if (row.walls_ready) return row.walls;
  row.walls_ready = true;
  auto go = [&](auto& self, const Rec& rec) -> void {
    if (rec.kind == 21 && rec.quad.size() == 4)
      row.walls.emplace_back(quad_end(row, rec.quad, 0), quad_end(row, rec.quad, 1));
    if (rec.child) self(self, *rec.child);
  };
  for (const Rec& rec : row.records) go(go, rec);
  return row.walls;
}

struct GateRef {
  int id = 0;
  V point;
};

std::vector<GateRef> gates_of(const Walk& row) {
  std::vector<GateRef> gates;
  auto go = [&](auto& self, const Rec& rec) -> void {
    if (rec.kind == 22 && rec.has_id && rec.quad.size() == 4) {
      const V a = quad_end(row, rec.quad, 0);
      const V b = quad_end(row, rec.quad, 1);
      gates.push_back({rec.id, {(a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f}});
    }
    if (rec.child) self(self, *rec.child);
  };
  for (const Rec& rec : row.records) go(go, rec);
  return gates;
}

bool inside_polygon(V point, const std::vector<V>& polygon) {
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const V& a = polygon[j];
    const V& b = polygon[i];
    const float length = flat(a, b);
    if (length > 1e-8f && std::fabs(cross_xz(a, b, point)) / length < 0.015f &&
        point.x >= std::min(a.x, b.x) - 0.015f && point.x <= std::max(a.x, b.x) + 0.015f &&
        point.z >= std::min(a.z, b.z) - 0.015f && point.z <= std::max(a.z, b.z) + 0.015f)
      return true;
    if ((a.z > point.z) != (b.z > point.z) &&
        point.x < (b.x - a.x) * (point.z - a.z) / (b.z - a.z) + a.x)
      inside = !inside;
  }
  return inside;
}

float segment_distance(V p, V a, V b) {
  const V d = sub(b, a);
  const float denom = std::max(1e-9f, d.x * d.x + d.z * d.z);
  const float t = std::clamp(((p.x - a.x) * d.x + (p.z - a.z) * d.z) / denom, 0.f, 1.f);
  return flat(p, {a.x + d.x * t, 0, a.z + d.z * t});
}

bool clear_of_walls(V a, V b, const std::vector<std::pair<V, V>>& walls) {
  for (const auto& wall : walls) {
    const float gap = std::min(std::min(segment_distance(a, wall.first, wall.second),
                                        segment_distance(b, wall.first, wall.second)),
                               std::min(segment_distance(wall.first, a, b),
                                        segment_distance(wall.second, a, b)));
    if (gap < kClearance - 1e-6f) return false;
  }
  return true;
}

bool segment_inside(V a, V b, const std::vector<V>& polygon) {
  if (!inside_polygon(a, polygon) || !inside_polygon(b, polygon)) return false;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const V& c = polygon[i];
    const V& d = polygon[(i + 1) % polygon.size()];
    if (cross_xz(a, b, c) * cross_xz(a, b, d) < -1e-8f &&
        cross_xz(c, d, a) * cross_xz(c, d, b) < -1e-8f) {
      const float dx = b.x - a.x, dz = b.z - a.z, ex = d.x - c.x, ez = d.z - c.z;
      const float t = ((c.x - a.x) * ez - (c.z - a.z) * ex) / (dx * ez - dz * ex);
      if (std::min(t, 1.f - t) * flat(a, b) > 0.02f) return false;
    }
  }
  for (float t : {0.25f, 0.5f, 0.75f})
    if (!inside_polygon(lerp(a, b, t), polygon)) return false;
  return true;
}

std::vector<V> curve(V a, V control, V b) {
  const int count = std::clamp(
      static_cast<int>(std::ceil((flat(a, control) + flat(control, b)) * 5.f)), 16, 256);
  std::vector<V> points;
  points.reserve(static_cast<size_t>(count) + 1);
  for (int i = 0; i <= count; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(count);
    const float u = 1.f - t;
    points.push_back(add(add(scale(a, u * u), scale(control, 2.f * t * u)), scale(b, t * t)));
  }
  return points;
}

bool path_valid(const std::vector<V>& points, const std::vector<V>& polygon,
                const std::vector<std::pair<V, V>>& walls) {
  for (size_t i = 0; i < points.size(); ++i) {
    if (!inside_polygon(points[i], polygon) || !clear_of_walls(points[i], points[i], walls))
      return false;
    if (i && (!segment_inside(points[i - 1], points[i], polygon) ||
              !clear_of_walls(points[i - 1], points[i], walls)))
      return false;
  }
  return true;
}

std::vector<V> constrained_transition(const Walk& row, V from, V to) {
  const std::vector<V>* polygon = polygon_of(row);
  const auto& walls = walls_of(row);
  if (!polygon || !inside_polygon(from, *polygon) || !inside_polygon(to, *polygon) ||
      !clear_of_walls(from, from, walls) || !clear_of_walls(to, to, walls))
    return {};
  const V control{row.origin[0], (from.y + to.y) * 0.5f, row.origin[1]};
  std::vector<V> points = curve(from, control, to);
  if (!path_valid(points, *polygon, walls)) {
    std::vector<V> candidates = *polygon;
    for (const auto& wall : walls)
      for (const V& end : {wall.first, wall.second})
        for (int i = 0; i < 24; ++i) {
          const float angle = static_cast<float>(i) * 3.14159265f / 12.f;
          candidates.push_back({end.x + std::cos(angle) * 0.31f, 0,
                                end.z + std::sin(angle) * 0.31f});
        }
    std::vector<V> nodes{from, to};
    for (const V& candidate : candidates)
      if (inside_polygon(candidate, *polygon) && clear_of_walls(candidate, candidate, walls))
        nodes.push_back(candidate);
    std::vector<float> cost(nodes.size(), 1e30f);
    std::vector<int> previous(nodes.size(), -1);
    std::vector<char> done(nodes.size(), 0);
    cost[0] = 0;
    for (size_t step = 0; step < nodes.size(); ++step) {
      int current = -1;
      for (size_t i = 0; i < nodes.size(); ++i)
        if (!done[i] && (current < 0 || cost[i] < cost[static_cast<size_t>(current)]))
          current = static_cast<int>(i);
      if (current < 0 || cost[static_cast<size_t>(current)] > 1e29f) break;
      if (current == 1) break;
      done[static_cast<size_t>(current)] = 1;
      for (size_t i = 0; i < nodes.size(); ++i) {
        if (static_cast<int>(i) == current || done[i]) continue;
        if (!segment_inside(nodes[static_cast<size_t>(current)], nodes[i], *polygon) ||
            !clear_of_walls(nodes[static_cast<size_t>(current)], nodes[i], walls))
          continue;
        const float next = cost[static_cast<size_t>(current)] +
                           flat(nodes[static_cast<size_t>(current)], nodes[i]);
        if (next < cost[i]) {
          cost[i] = next;
          previous[i] = current;
        }
      }
    }
    if (previous[1] < 0) return {};
    std::vector<V> path;
    for (int i = 1; i >= 0;) {
      path.push_back(nodes[static_cast<size_t>(i)]);
      if (i == 0) break;
      i = previous[static_cast<size_t>(i)];
      if (path.size() > nodes.size()) return {};
    }
    std::reverse(path.begin(), path.end());
    points = {from};
    for (size_t i = 1; i + 1 < path.size(); ++i) {
      const V& corner = path[i];
      const float radius = std::min(0.75f, std::min(flat(path[i - 1], corner) * 0.3f,
                                                    flat(corner, path[i + 1]) * 0.3f));
      const V a = lerp(corner, path[i - 1], radius / std::max(1e-6f, flat(corner, path[i - 1])));
      const V b = lerp(corner, path[i + 1], radius / std::max(1e-6f, flat(corner, path[i + 1])));
      const std::vector<V> rounded = curve(a, corner, b);
      if (path_valid(rounded, *polygon, walls))
        points.insert(points.end(), rounded.begin(), rounded.end());
      else
        points.push_back(corner);
    }
    points.push_back(to);
    if (!path_valid(points, *polygon, walls)) return {};
  }
  std::vector<float> lengths{0};
  for (size_t i = 1; i < points.size(); ++i)
    lengths.push_back(lengths.back() + flat(points[i - 1], points[i]));
  const float length = lengths.back();
  for (size_t i = 0; i < points.size(); ++i)
    points[i].y = from.y + (to.y - from.y) * (length > 1e-8f ? lengths[i] / length : 0);
  return points;
}

struct Route {
  int target = 0;
  int entry = 0;
  std::vector<V> points;
  float score = 0;
};

struct Lines {
  std::vector<Walk> rows;
  std::unordered_map<int, int> row_of;
  std::vector<Zone> zones;
  std::unordered_map<int, int> zone_of;
  std::map<std::pair<int, int>, std::vector<Sample>> cells;
};

V point_of(const Lines& lines, int zone, float t) {
  return zone_point(lines.zones[static_cast<size_t>(zone)], t, 0.5f);
}

std::vector<Route> forward_routes(const Lines& lines, int zone, int exit, V from, V forward) {
  const Zone& source = lines.zones[static_cast<size_t>(zone)];
  const auto first_row = lines.row_of.find(source.gates[exit].junction);
  if (first_row == lines.row_of.end()) return {};
  struct Item {
    int row = 0;
    int previous = 0;
    V from;
    std::vector<V> points;
    std::vector<int> via;
  };
  std::vector<Item> queue{{first_row->second, source.id, from, {from}, {}}};
  std::vector<Route> result;
  for (int processed = 0; !queue.empty() && processed < 128; ++processed) {
    Item current = std::move(queue.front());
    queue.erase(queue.begin());
    const Walk& row = lines.rows[static_cast<size_t>(current.row)];
    if (std::find(current.via.begin(), current.via.end(), row.id) != current.via.end() ||
        current.via.size() >= 12)
      continue;
    current.via.push_back(row.id);
    for (const GateRef& gate : gates_of(row)) {
      if (gate.id == current.previous || gate.id == source.id) continue;
      const auto target_index = lines.zone_of.find(gate.id);
      if (target_index != lines.zone_of.end()) {
        const Zone& target = lines.zones[static_cast<size_t>(target_index->second)];
        int entry = -1;
        if (target.gates[0].junction == row.id) entry = 0;
        else if (target.gates[1].junction == row.id) entry = 1;
        if (entry < 0) continue;
        const V to = point_of(lines, target_index->second, static_cast<float>(entry));
        std::vector<V> part = constrained_transition(row, current.from, to);
        if (part.empty()) continue;
        const V direction = norm_xz(sub(to, from));
        const V outgoing = norm_xz(sub(
            point_of(lines, target_index->second, entry == 0 ? 0.02f : 0.98f), to));
        std::vector<V> points = current.points;
        if (part.size() > 1) points.insert(points.end(), part.begin() + 1, part.end());
        float length = 0;
        for (size_t i = 1; i < points.size(); ++i) length += dist(points[i - 1], points[i]);
        result.push_back({target_index->second, entry, std::move(points),
                          dot(direction, forward) * 0.7f + dot(outgoing, forward) * 0.3f -
                              length * 0.001f});
      } else {
        const auto next = lines.row_of.find(gate.id);
        if (next == lines.row_of.end() ||
            std::find(current.via.begin(), current.via.end(), gate.id) != current.via.end() ||
            !polygon_of(lines.rows[static_cast<size_t>(next->second)]))
          continue;
        const Walk& next_row = lines.rows[static_cast<size_t>(next->second)];
        bool opposite = false;
        for (const GateRef& back : gates_of(next_row))
          if (back.id == row.id && flat(back.point, gate.point) < 0.1f) opposite = true;
        if (!opposite) continue;
        const V to{gate.point.x, current.from.y, gate.point.z};
        std::vector<V> part = constrained_transition(row, current.from, to);
        if (part.empty()) continue;
        Item onward{next->second, row.id, to, current.points, current.via};
        if (part.size() > 1) onward.points.insert(onward.points.end(), part.begin() + 1, part.end());
        queue.push_back(std::move(onward));
      }
    }
  }
  if (result.empty()) return {};
  float best = -1e30f;
  for (const Route& route : result) best = std::max(best, route.score);
  result.erase(std::remove_if(result.begin(), result.end(),
                              [&](const Route& route) { return route.score < best - 0.05f; }),
               result.end());
  return result;
}

void build(Lines& lines) {
  lines.row_of.clear();
  lines.zones.clear();
  lines.zone_of.clear();
  lines.cells.clear();
  for (int i = 0; i < static_cast<int>(lines.rows.size()); ++i)
    lines.row_of.emplace(lines.rows[static_cast<size_t>(i)].id, i);
  for (const Walk& row : lines.rows) {
    std::vector<const Rec*> runs;
    std::vector<const Rec*> spans;
    auto go = [&](auto& self, const Rec& rec) -> void {
      if (rec.kind == 23 && !rec.points.empty()) runs.push_back(&rec);
      if (rec.kind == 22 && rec.has_id && rec.quad.size() == 4) spans.push_back(&rec);
      if (rec.child) self(self, *rec.child);
    };
    for (const Rec& rec : row.records) go(go, rec);
    if (runs.size() != 2 || spans.size() != 2) continue;
    Boundary a = make_boundary(runs[0]->points);
    std::vector<V> raw = runs[1]->points;
    if (flat(a.points.front(), raw.front()) > flat(a.points.front(), raw.back()))
      std::reverse(raw.begin(), raw.end());
    Boundary b = make_boundary(std::move(raw));
    if (a.length < 1.f || b.length < 1.f) continue;
    const V ends[2] = {lerp(sample_boundary(a, 0), sample_boundary(b, 0), 0.5f),
                       lerp(sample_boundary(a, 1), sample_boundary(b, 1), 0.5f)};
    auto mid = [](const Rec* rec) {
      const auto& q = rec->quad;
      return V{(q[0] + q[2]) * 0.5f, 0, (q[1] + q[3]) * 0.5f};
    };
    if (flat(ends[0], mid(spans[0])) > flat(ends[0], mid(spans[1])))
      std::swap(spans[0], spans[1]);
    if (flat(ends[0], mid(spans[0])) > 12.f || flat(ends[1], mid(spans[1])) > 12.f) continue;
    Zone zone;
    zone.id = row.id;
    zone.sides[0] = std::move(a);
    zone.sides[1] = std::move(b);
    for (int i = 0; i < 2; ++i) zone.gates[i] = {spans[static_cast<size_t>(i)]->id, ends[i]};
    zone.length = (zone.sides[0].length + zone.sides[1].length) * 0.5f;
    lines.zone_of.emplace(zone.id, static_cast<int>(lines.zones.size()));
    lines.zones.push_back(std::move(zone));
  }
  for (int z = 0; z < static_cast<int>(lines.zones.size()); ++z) {
    const Zone& zone = lines.zones[static_cast<size_t>(z)];
    const int steps = std::max(1, static_cast<int>(std::ceil(zone.length / 8.f)));
    for (int i = 0; i < steps; ++i)
      for (float lateral : {0.25f, 0.5f, 0.75f}) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
        const V position = zone_point(zone, t, lateral);
        const auto key = std::make_pair(static_cast<int>(std::floor(position.x / kCell)),
                                         static_cast<int>(std::floor(position.z / kCell)));
        lines.cells[key].push_back({z, t, lateral, position});
      }
  }
}

Boundary path_boundary(const std::vector<std::array<float, 3>>& path) {
  std::vector<V> points;
  points.reserve(path.size());
  for (const auto& p : path) points.push_back({p[0], p[1], p[2]});
  return make_boundary(std::move(points));
}

void remember(std::vector<std::array<float, 3>>* path, const std::vector<V>& points) {
  path->clear();
  for (const V& p : points) path->push_back({p.x, p.y, p.z});
}

// SLRR_rev PedestrianSpawnIndex.choose: a moving body is tested at its
// closest approach within two seconds. A pedestrian obstacle has radius 2.
bool inside_block(float x, float y, float z, const PedestrianBlock& block) {
  if (std::fabs(y - block.y) > 4.f) return false;
  const float dx = x - block.x;
  const float dy = y - block.y;
  const float dz = z - block.z;
  const float speed2 = block.vx * block.vx + block.vy * block.vy + block.vz * block.vz;
  const float approach = speed2 > 0.001f
                             ? std::clamp((dx * block.vx + dy * block.vy + dz * block.vz) / speed2,
                                          0.f, 2.f)
                             : 0.f;
  const float px = dx - block.vx * approach;
  const float py = dy - block.vy * approach;
  const float pz = dz - block.vz * approach;
  const float limit = block.radius + 1.5f;
  return px * px + py * py + pz * pz < limit * limit;
}

}  // namespace

struct PedestrianLines::Data : Lines {};

PedestrianLines::PedestrianLines() : data_(new Data) {}
PedestrianLines::~PedestrianLines() { delete data_; }

bool PedestrianLines::load(const std::string& text) {
  data_->rows.clear();
  data_->zones.clear();
  data_->cells.clear();
  data_->row_of.clear();
  data_->zone_of.clear();
  std::vector<WalkRow> parsed;
  std::string error;
  if (!walk_read(text, &parsed, &error)) {
    if (!error.empty()) std::fprintf(stderr, "[ped] %s\n", error.c_str());
    data_->rows.clear();
    data_->zones.clear();
    data_->cells.clear();
    return false;
  }
  for (auto& row : parsed) data_->rows.push_back(Walk(std::move(row)));
  build(*data_);
  return true;
}

int PedestrianLines::zones() const { return static_cast<int>(data_->zones.size()); }

float PedestrianLines::area_within(float x, float y, float z, float radius) const {
  if (radius <= 0.f) return 0.f;
  const V focus{x, y, z};
  float total = 0.f;
  for (const Zone& zone : data_->zones) {
    const int steps = std::max(1, static_cast<int>(std::ceil(zone.length / 2.f)));
    V previous = zone_point(zone, 0.f, 0.5f);
    float previous_width = flat(sample_boundary(zone.sides[0], 0.f), sample_boundary(zone.sides[1], 0.f));
    for (int i = 1; i <= steps; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(steps);
      const V middle = zone_point(zone, t, 0.5f);
      const float width = flat(sample_boundary(zone.sides[0], t), sample_boundary(zone.sides[1], t));
      const V sample = lerp(previous, middle, 0.5f);
      if (std::fabs(sample.y - y) <= 20.f && dist(sample, focus) <= radius)
        total += (previous_width + width) * 0.5f * flat(previous, middle);
      previous = middle;
      previous_width = width;
    }
  }
  return total;
}

bool PedestrianLines::spawn(float x, float y, float z, float radius, float min_distance,
                            uint32_t& rng, PedestrianWalker& out, const PedestrianBlock* blocked,
                            int blocked_count) const {
  if (data_->zones.empty() || radius <= 0.f) return false;
  const V focus{x, y, z};
  const Sample* choice = nullptr;
  int count = 0;
  const int x0 = static_cast<int>(std::floor((x - radius) / kCell));
  const int x1 = static_cast<int>(std::floor((x + radius) / kCell));
  const int z0 = static_cast<int>(std::floor((z - radius) / kCell));
  const int z1 = static_cast<int>(std::floor((z + radius) / kCell));
  for (int cx = x0; cx <= x1; ++cx)
    for (int cz = z0; cz <= z1; ++cz) {
      const auto found = data_->cells.find({cx, cz});
      if (found == data_->cells.end()) continue;
      for (const Sample& sample : found->second) {
        const float distance = dist(sample.position, focus);
        if (distance > radius || distance < min_distance) continue;
        if (std::fabs(sample.position.y - y) > 20.f) continue;
        bool taken = false;
        for (int i = 0; blocked && i < blocked_count; ++i)
          if (inside_block(sample.position.x, sample.position.y, sample.position.z, blocked[i])) {
            taken = true;
            break;
          }
        if (taken) continue;
        if (unit(rng) < 1.f / static_cast<float>(++count)) choice = &sample;
      }
    }
  if (!choice) return false;
  const Zone& zone = data_->zones[static_cast<size_t>(choice->zone)];
  out.zone = choice->zone;
  out.t = choice->t;
  out.lateral = choice->lateral;
  out.sign = choice->t < 0.5f ? -1 : 1;
  out.base_speed = 0.9f + unit(rng) * 0.6f;
  out.flee_speed = 1.8f + unit(rng) * 1.2f;
  const V position = zone_point(zone, out.t, out.lateral);
  const V ahead = zone_point(zone, std::clamp(out.t + static_cast<float>(out.sign) * 0.02f, 0.f, 1.f),
                             out.lateral);
  out.x = position.x;
  out.y = position.y;
  out.z = position.z;
  out.yaw = std::atan2(ahead.x - position.x, ahead.z - position.z);
  out.ok = true;
  return true;
}

void PedestrianLines::step(PedestrianWalker& walker, float dt, float car_x, float car_y,
                           float car_z, uint32_t& rng) const {
  if (!walker.ok || walker.zone < 0 ||
      walker.zone >= static_cast<int>(data_->zones.size())) {
    walker.ok = false;
    return;
  }
  dt = std::clamp(dt, 0.f, 0.1f);
  walker.cooldown = std::max(0.f, walker.cooldown - dt);
  const V car{car_x, car_y, car_z};
  const V here{walker.x, walker.y, walker.z};
  const bool threat = std::fabs(car.y - here.y) < 4.f && flat(car, here) < 12.f;
  walker.fleeing = threat;
  const float target_speed = threat ? walker.flee_speed : walker.base_speed;
  walker.speed += std::clamp(target_speed - walker.speed, -dt * 3.f, dt * 3.f);
  const V before = here;
  const Zone& zone = data_->zones[static_cast<size_t>(walker.zone)];
  if (walker.transitioning && walker.path.size() >= 2 && walker.target >= 0 &&
      walker.target < static_cast<int>(data_->zones.size())) {
    const V from{walker.path.front()[0], walker.path.front()[1], walker.path.front()[2]};
    const V to{walker.path.back()[0], walker.path.back()[1], walker.path.back()[2]};
    if (threat && walker.cooldown == 0.f && flat(car, to) + 2.f < flat(car, from)) {
      std::reverse(walker.path.begin(), walker.path.end());
      const int previous = walker.zone;
      const int entry = walker.sign > 0 ? 1 : 0;
      walker.zone = walker.target;
      walker.sign = walker.entry == 0 ? -1 : 1;
      walker.target = previous;
      walker.entry = entry;
      walker.tr_t = 1.f - walker.tr_t;
      walker.cooldown = 2.f;
    }
    const Boundary live = path_boundary(walker.path);
    const float length = std::max(0.01f, live.length);
    walker.tr_t = std::min(1.f, walker.tr_t + walker.speed * dt / length);
    const V position = sample_boundary(live, walker.tr_t);
    walker.x = position.x;
    walker.y = position.y;
    walker.z = position.z;
    if (walker.tr_t >= 1.f) {
      walker.zone = walker.target;
      walker.t = static_cast<float>(walker.entry);
      walker.sign = walker.entry == 0 ? 1 : -1;
      walker.transitioning = false;
      walker.path.clear();
    }
  } else {
    const V ahead = sub(zone_point(zone, std::clamp(walker.t + static_cast<float>(walker.sign) * 0.02f,
                                                    0.f, 1.f),
                                   walker.lateral),
                        here);
    if (threat && walker.cooldown == 0.f && dot(ahead, sub(car, here)) > 0.f) {
      walker.sign = -walker.sign;
      walker.cooldown = 2.f;
    }
    const float length = std::max(0.01f, zone.length);
    walker.t = std::clamp(walker.t + static_cast<float>(walker.sign) * walker.speed * dt / length, 0.f,
                          1.f);
    const V position = zone_point(zone, walker.t, walker.lateral);
    walker.x = position.x;
    walker.y = position.y;
    walker.z = position.z;
    if ((walker.sign > 0 && walker.t == 1.f) || (walker.sign < 0 && walker.t == 0.f)) {
      const int exit = walker.sign > 0 ? 1 : 0;
      const V forward = norm_xz(sub(zone_point(zone, static_cast<float>(exit), 0.5f),
                                    zone_point(zone, exit == 0 ? 0.02f : 0.98f, 0.5f)));
      const std::vector<Route> routes = forward_routes(*data_, walker.zone, exit, position, forward);
      if (routes.empty()) {
        walker.sign = -walker.sign;
      } else {
        const int pick = std::min(static_cast<int>(routes.size()) - 1,
                                  static_cast<int>(unit(rng) * static_cast<float>(routes.size())));
        const Route& chosen = routes[static_cast<size_t>(pick)];
        walker.transitioning = true;
        walker.target = chosen.target;
        walker.entry = chosen.entry;
        walker.tr_t = 0;
        remember(&walker.path, chosen.points);
      }
    }
  }
  const float dx = walker.x - before.x;
  const float dz = walker.z - before.z;
  if (dx * dx + dz * dz > 1e-8f) walker.yaw = std::atan2(dx, dz);
}

}  // namespace slrr
