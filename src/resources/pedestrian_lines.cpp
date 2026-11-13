#include "slrr/pedestrian_lines.hpp"
#include "slrr/format/walk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <utility>

// Pedestrians in the city.walk rows, after the original engine
// (StreetLegal_Redline.exe 2.2.1, functions cited by address). A walker is a
// free agent with a heading. The row's walls and its two sidewalk runs repel
// it and keep it a body radius away; gates are open edges into the next row.
// Constants marked "original" come from the executable; the rest are noted
// where they are OpenSLRR choices.

namespace slrr {
namespace {

// Original: ped+0x30 (radius) and ped+0x34 (speed), both set to 1.0 at spawn.
constexpr float kRadius = 1.f;
constexpr float kSpeed = 1.f;
// Original 0x00586e40: the current heading counts three times in the sum.
constexpr float kHeadingWeight = 3.f;
// Original car obstacle (0x00463ac0 -> 0x00589860): radius 3 m. A walker
// reacts when very close, or ahead of the car within the car and walker radii
// plus 1 m of its line and 5 s of travel. The flee push weighs 10.
constexpr float kCarRadius = 3.f;
constexpr float kCarSeconds = 5.f;
constexpr float kFleeWeight = 10.f;
constexpr int kCell = 64;

struct V {
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
};

V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V lerp(V a, V b, float t) { return add(a, scale(sub(b, a), t)); }
float flat(V a, V b) { return std::hypot(a.x - b.x, a.z - b.z); }

float unit(uint32_t& rng) {
  rng = rng * 1664525u + 1013904223u;
  return static_cast<float>(rng >> 8) * (1.f / 16777216.f);
}

// Original influence curves (behaviour table built at 0x00588080 through
// 0x005856c0): f(x) = max(0, a / (x + c) - b).
struct Curve {
  float a, b, c;
  float operator()(float x) const { return std::max(0.f, a / std::max(0.05f, x + c) - b); }
};
// Walls and runs: 2 at contact, 0 at 3 m, for both ranking and steering.
constexpr Curve kEdgeCurve{1.875f, 0.5f, 0.75f};
// Other walkers: ranking falls to 0 at 10, steering is 3.5 at contact and 0
// at 4 m.
constexpr Curve kWalkerRank{1.40625f, 0.125f, 1.25f};
constexpr Curve kWalkerSteer{52.5f, 5.25f, 6.f};
// Original 0x00586cd0 keeps the four strongest influences per walker.
constexpr int kSlots = 4;

enum class EdgeType { Wall, Gate, Run };

// A row edge on the ground plane. Points are world metres; edges run so that
// (-dz, dx) points into the row (0x00586a20 / 0x0058a520), which holds for
// every stock row.
// Segments are grouped in chunks with a bounding box, so a query skips the
// far parts of a long run.
constexpr size_t kChunk = 16;
struct Box {
  float x0, z0, x1, z1;
  float distance(V p) const {
    const float dx = std::max({x0 - p.x, 0.f, p.x - x1});
    const float dz = std::max({z0 - p.z, 0.f, p.z - z1});
    return std::hypot(dx, dz);
  }
};
struct Edge {
  EdgeType type = EdgeType::Wall;
  int target_id = -1;
  int target = -1;
  std::vector<V> points;
  std::vector<float> lengths;
  std::vector<Box> chunks;
  float length = 0.f;
};

struct Nearest {
  V point;
  V normal;
  float distance = 0.f;
  float along = 0.f;
  bool interior = false;
};

// Nearest point of an edge. Parts farther than `limit` are skipped; with
// nothing inside it the distance stays at 1e30.
Nearest nearest(const Edge& edge, V p, float limit = 1e30f) {
  Nearest best;
  best.distance = 1e30f;
  for (size_t i = 1; i < edge.points.size(); ++i) {
    if ((i - 1) % kChunk == 0) {
      const size_t chunk = (i - 1) / kChunk;
      if (chunk < edge.chunks.size() &&
          edge.chunks[chunk].distance(p) > std::min(limit, best.distance)) {
        i += kChunk - 1;
        continue;
      }
    }
    const V a = edge.points[i - 1];
    const V b = edge.points[i];
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float span2 = dx * dx + dz * dz;
    if (span2 < 1e-12f) continue;
    const float raw = ((p.x - a.x) * dx + (p.z - a.z) * dz) / span2;
    const float t = std::clamp(raw, 0.f, 1.f);
    const V q = lerp(a, b, t);
    const float d = flat(p, q);
    if (d < best.distance) {
      const float span = std::sqrt(span2);
      best.point = q;
      best.normal = {-dz / span, 0.f, dx / span};
      best.distance = d;
      best.along = edge.lengths[i - 1] + t * span;
      best.interior = best.along > 1e-4f && best.along < edge.length - 1e-4f;
    }
  }
  return best;
}

V point_at(const Edge& edge, float s) {
  if (edge.points.size() == 1) return edge.points[0];
  s = std::clamp(s, 0.f, edge.length);
  size_t i = 1;
  while (i + 1 < edge.lengths.size() && edge.lengths[i] < s) ++i;
  const float span = std::max(1e-6f, edge.lengths[i] - edge.lengths[i - 1]);
  return lerp(edge.points[i - 1], edge.points[i], (s - edge.lengths[i - 1]) / span);
}

void finish_edge(Edge& edge) {
  edge.lengths.assign(1, 0.f);
  for (size_t i = 1; i < edge.points.size(); ++i)
    edge.lengths.push_back(edge.lengths.back() + flat(edge.points[i - 1], edge.points[i]));
  edge.length = edge.lengths.back();
  edge.chunks.clear();
  // Chunk k holds segments k*kChunk+1 .. (k+1)*kChunk, so its box spans
  // points k*kChunk .. (k+1)*kChunk.
  for (size_t start = 0; start + 1 < edge.points.size(); start += kChunk) {
    Box box{1e30f, 1e30f, -1e30f, -1e30f};
    for (size_t i = start; i <= std::min(start + kChunk, edge.points.size() - 1); ++i) {
      box.x0 = std::min(box.x0, edge.points[i].x);
      box.z0 = std::min(box.z0, edge.points[i].z);
      box.x1 = std::max(box.x1, edge.points[i].x);
      box.z1 = std::max(box.z1, edge.points[i].z);
    }
    edge.chunks.push_back(box);
  }
}

float len3(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V unit3(V a) {
  const float l = len3(a);
  return l > 1e-12f ? scale(a, 1.f / l) : a;
}

// One span of a sidewalk run (original 0x0057c590). Only the directions of
// the stored tangents count; both are rescaled to the span's arc length.
struct Span {
  V p0, p1, t0, t1;
  float length = 1.f;
  V at(float s) const {
    const float t = s / length, t2 = t * t, t3 = t2 * t;
    return add(add(scale(p0, 2.f * t3 - 3.f * t2 + 1.f), scale(p1, 3.f * t2 - 2.f * t3)),
               add(scale(t0, t3 - 2.f * t2 + t), scale(t1, t3 - t2)));
  }
  // Original 0x0057ca50: 100 chords with the curve parameter running 0..1.
  float measure() const {
    Span unit = *this;
    unit.length = 1.f;
    float total = 0.f;
    V previous = unit.at(0.f);
    for (int i = 1; i <= 100; ++i) {
      const V next = unit.at(static_cast<float>(i) * 0.01f);
      total += len3(sub(next, previous));
      previous = next;
    }
    return total;
  }
};

Span make_span(V p0, V p1, V t0, V t1) {
  Span span;
  span.p0 = p0;
  span.p1 = p1;
  t0 = unit3(t0);
  t1 = unit3(t1);
  const float chord = std::max(1e-4f, len3(sub(p1, p0)));
  const V d = unit3(sub(p1, p0));
  const float a = std::sqrt(1.f / chord);
  auto half = [](float c) { return std::sqrt(std::max(0.f, (1.f + c) * 0.5f)); };
  const float h0 = half(t0.x * d.x + t0.y * d.y + t0.z * d.z);
  const float h1 = half(t1.x * d.x + t1.y * d.y + t1.z * d.z);
  span.length = chord / std::max(1e-4f, (1.f - a * (1.f - h0)) * (1.f - a * (1.f - h1)));
  span.t0 = scale(t0, span.length);
  span.t1 = scale(t1, span.length);
  for (int i = 0; i < 16; ++i) {
    const float previous = span.length;
    span.length = std::max(1e-4f, span.measure());
    const float ratio = span.length / previous;
    span.t0 = scale(span.t0, ratio);
    span.t1 = scale(span.t1, ratio);
    if (std::fabs(span.length - previous) <= 0.01f * previous) break;
  }
  return span;
}

// A sidewalk run as a fine polyline of its spline (original loader
// 0x0058670e: begin at node 0 with its tangent direction, then one span per
// further node, each starting with the previous span's end direction).
std::vector<V> run_points(const WalkRecord& record, V offset) {
  std::vector<V> out;
  const auto& nodes = record.nodes;
  if (nodes.empty()) return out;
  auto vec = [](const WalkVec& v) { return V{v.x, v.y, v.z}; };
  V previous = vec(nodes[0].point);
  V direction = vec(nodes[0].tangent);
  for (size_t i = 1; i < nodes.size(); ++i) {
    const V next = vec(nodes[i].point);
    if (len3(sub(next, previous)) < 1e-4f) continue;
    const Span span = make_span(previous, next, direction, vec(nodes[i].tangent));
    const int steps = std::clamp(static_cast<int>(std::ceil(span.length)), 4, 512);
    for (int k = 0; k < steps; ++k)
      out.push_back(add(offset, span.at(span.length * static_cast<float>(k) / static_cast<float>(steps))));
    previous = next;
    direction = span.t1;
  }
  out.push_back(add(offset, previous));
  return out;
}

struct Row {
  int id = 0;
  V origin;
  float slope_x = 0.f;
  float slope_z = 0.f;
  float height = 0.f;
  std::vector<Edge> edges;
  std::vector<int> runs;
  std::vector<int> links;
  float area = 0.f;
  V center;
  float reach = 0.f;
};

float row_height(const Row& row, float x, float z) {
  if (row.runs.size() == 2) {
    // Original 0x005862e0: the runs' heights at their nearest points,
    // weighted by the distance to the other run.
    const V p{x, 0.f, z};
    const Nearest a = nearest(row.edges[static_cast<size_t>(row.runs[0])], p);
    const Nearest b = nearest(row.edges[static_cast<size_t>(row.runs[1])], p);
    const float total = a.distance + b.distance;
    if (total < 1e-6f) return a.point.y;
    return (a.point.y * b.distance + b.point.y * a.distance) / total;
  }
  if (row.runs.size() == 1) return nearest(row.edges[static_cast<size_t>(row.runs[0])], {x, 0, z}).point.y;
  // Original 0x005863a2: a plane in the row's local frame.
  return row.slope_x * (x - row.origin.x) + row.slope_z * (z - row.origin.z) + row.height;
}

bool inside_row(const Row& row, V p) {
  bool inside = false;
  for (const Edge& edge : row.edges)
    for (size_t i = 1; i < edge.points.size(); ++i) {
      const V a = edge.points[i - 1];
      const V b = edge.points[i];
      if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
        inside = !inside;
    }
  return inside;
}

bool inside_block(float x, float y, float z, const PedestrianBlock& block) {
  // SLRR_rev PedestrianSpawnIndex.choose: a moving body is tested at its
  // closest approach within two seconds.
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

// Push a walker out of the row's walls and runs (original hard pass in
// 0x00589cb0, run variant 0x005860a0): an edge closer than the body radius
// pushes it back to the radius, and the distance lost to the push is given
// back as a slide along the edge. OpenSLRR addition: inside an edge's span
// a walker found behind the edge is put back on the row side, so one long
// step cannot carry it out of the sidewalk.
V constrain(const Row& row, V p, V previous, float step) {
  for (int pass = 0; pass < 2; ++pass)
    for (const Edge& edge : row.edges) {
      if (edge.type == EdgeType::Gate) continue;
      const Nearest n = nearest(edge, p, kRadius + 2.f);
      if (n.distance >= 1e29f) continue;
      const float side = (p.x - n.point.x) * n.normal.x + (p.z - n.point.z) * n.normal.z;
      float ox = 0.f, oz = 0.f;
      if (n.interior && side <= 0.f) {
        p.x = n.point.x + n.normal.x * kRadius;
        p.z = n.point.z + n.normal.z * kRadius;
        ox = -n.normal.x;
        oz = -n.normal.z;
      } else if (n.distance < kRadius) {
        ox = n.distance > 1e-5f ? (n.point.x - p.x) / n.distance : -n.normal.x;
        oz = n.distance > 1e-5f ? (n.point.z - p.z) / n.distance : -n.normal.z;
        p.x += ox * (n.distance - kRadius);
        p.z += oz * (n.distance - kRadius);
      } else {
        continue;
      }
      if (pass > 0) continue;
      const float dx = p.x - previous.x, dz = p.z - previous.z;
      const float lack = step - std::hypot(dx, dz);
      if (lack <= 0.f) continue;
      float tx = -oz, tz = ox;
      if (dx * tx + dz * tz < 0.f) {
        tx = -tx;
        tz = -tz;
      }
      p.x += tx * lack;
      p.z += tz * lack;
    }
  return p;
}

}  // namespace

struct PedestrianLines::Data {
  std::vector<Row> rows;
  std::unordered_map<int, int> row_of;
  std::map<std::pair<int, int>, std::vector<int>> cells;
};

PedestrianLines::PedestrianLines() : data_(new Data) {}
PedestrianLines::~PedestrianLines() { delete data_; }

bool PedestrianLines::load(const std::string& text) {
  *data_ = Data{};
  std::vector<WalkRow> parsed;
  std::string error;
  if (!walk_read(text, &parsed, &error)) {
    if (!error.empty()) std::fprintf(stderr, "[ped] %s\n", error.c_str());
    return false;
  }
  for (const WalkRow& source : parsed) {
    Row row;
    row.id = source.id;
    row.origin = {source.origin_x, 0.f, source.origin_z};
    row.slope_x = source.slope_x;
    row.slope_z = source.slope_z;
    row.height = source.height;
    for (const WalkRecord& record : source.records) {
      Edge edge;
      if (record.kind == 23 && record.nodes.size() >= 2) {
        edge.type = EdgeType::Run;
        edge.points = run_points(record, row.origin);
      } else {
        edge.type = record.id == -1 ? EdgeType::Wall : EdgeType::Gate;
        edge.target_id = record.id;
        const V a{record.quad[0] + row.origin.x, 0.f, record.quad[1] + row.origin.z};
        const V b{record.quad[2] + row.origin.x, 0.f, record.quad[3] + row.origin.z};
        // Original 0x00585c80 drops edges shorter than 1e-4.
        if (flat(a, b) < 1e-4f) continue;
        edge.points = {a, b};
      }
      finish_edge(edge);
      if (edge.length < 1e-4f) continue;
      if (edge.type == EdgeType::Run) row.runs.push_back(static_cast<int>(row.edges.size()));
      row.edges.push_back(std::move(edge));
    }
    if (row.edges.size() < 3) continue;
    for (Edge& edge : row.edges)
      if (edge.type != EdgeType::Run)
        for (V& point : edge.points) point.y = row_height(row, point.x, point.z);
    // Original row area (0x00586b50): a corridor is its gate width times the
    // mean run length; any other row is its polygon area.
    if (row.runs.size() == 2) {
      const Edge& a = row.edges[static_cast<size_t>(row.runs[0])];
      const Edge& b = row.edges[static_cast<size_t>(row.runs[1])];
      row.area = flat(a.points.front(), b.points.back()) * (a.length + b.length) * 0.5f;
    } else {
      float twice = 0.f;
      for (const Edge& edge : row.edges)
        for (size_t i = 1; i < edge.points.size(); ++i)
          twice += edge.points[i - 1].x * edge.points[i].z - edge.points[i].x * edge.points[i - 1].z;
      row.area = std::fabs(twice) * 0.5f;
    }
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (const Edge& edge : row.edges)
      for (const V& point : edge.points) {
        lo[0] = std::min(lo[0], point.x);
        lo[1] = std::min(lo[1], point.z);
        hi[0] = std::max(hi[0], point.x);
        hi[1] = std::max(hi[1], point.z);
      }
    row.center = {(lo[0] + hi[0]) * 0.5f, 0.f, (lo[1] + hi[1]) * 0.5f};
    row.center.y = row_height(row, row.center.x, row.center.z);
    row.reach = std::hypot(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f;
    data_->row_of.emplace(row.id, static_cast<int>(data_->rows.size()));
    data_->rows.push_back(std::move(row));
  }
  for (size_t i = 0; i < parsed.size(); ++i) {
    const auto own = data_->row_of.find(parsed[i].id);
    if (own == data_->row_of.end()) continue;
    Row& row = data_->rows[static_cast<size_t>(own->second)];
    row.links.push_back(own->second);
    for (int link : parsed[i].links) {
      const auto found = data_->row_of.find(link);
      if (found != data_->row_of.end() &&
          std::find(row.links.begin(), row.links.end(), found->second) == row.links.end())
        row.links.push_back(found->second);
    }
    for (Edge& edge : row.edges)
      if (edge.type == EdgeType::Gate) {
        const auto found = data_->row_of.find(edge.target_id);
        edge.target = found == data_->row_of.end() ? -1 : found->second;
        if (edge.target >= 0 &&
            std::find(row.links.begin(), row.links.end(), edge.target) == row.links.end())
          row.links.push_back(edge.target);
      }
  }
  for (int i = 0; i < static_cast<int>(data_->rows.size()); ++i) {
    const V& c = data_->rows[static_cast<size_t>(i)].center;
    data_->cells[{static_cast<int>(std::floor(c.x / kCell)), static_cast<int>(std::floor(c.z / kCell))}]
        .push_back(i);
  }
  return true;
}

int PedestrianLines::rows() const { return static_cast<int>(data_->rows.size()); }

float PedestrianLines::area_within(float x, float y, float z, float radius) const {
  if (radius <= 0.f) return 0.f;
  float total = 0.f;
  const V focus{x, y, z};
  for (const Row& row : data_->rows)
    if (std::fabs(row.center.y - y) <= 20.f && flat(row.center, focus) <= radius) total += row.area;
  return total;
}

bool PedestrianLines::inside(float x, float y, float z, float clearance) const {
  const V p{x, y, z};
  for (const Row& row : data_->rows) {
    if (flat(row.center, p) > row.reach + 1.f) continue;
    if (!inside_row(row, p) || std::fabs(row_height(row, x, z) - y) > 2.f) continue;
    bool clear = true;
    for (const Edge& edge : row.edges)
      if (edge.type != EdgeType::Gate && nearest(edge, p, clearance).distance < clearance) clear = false;
    if (clear) return true;
  }
  return false;
}

bool PedestrianLines::spawn(float x, float y, float z, float radius, float min_distance,
                            uint32_t& rng, PedestrianWalker& out, const PedestrianBlock* blocked,
                            int blocked_count, bool (*hidden)(float, float, float, void*),
                            void* hidden_user) const {
  if (data_->rows.empty() || radius <= 0.f) return false;
  const V focus{x, y, z};
  std::vector<int> candidates;
  float total = 0.f;
  const int x0 = static_cast<int>(std::floor((x - radius) / kCell));
  const int x1 = static_cast<int>(std::floor((x + radius) / kCell));
  const int z0 = static_cast<int>(std::floor((z - radius) / kCell));
  const int z1 = static_cast<int>(std::floor((z + radius) / kCell));
  for (int cx = x0; cx <= x1; ++cx)
    for (int cz = z0; cz <= z1; ++cz) {
      const auto found = data_->cells.find({cx, cz});
      if (found == data_->cells.end()) continue;
      for (int index : found->second) {
        const Row& row = data_->rows[static_cast<size_t>(index)];
        if (row.area <= 0.f || flat(row.center, focus) > radius + row.reach ||
            std::fabs(row.center.y - y) > 25.f)
          continue;
        candidates.push_back(index);
        total += row.area;
      }
    }
  if (candidates.empty() || total <= 0.f) return false;
  // Rows are drawn by area, as the original fills each row to density x area.
  bool have = false;
  bool have_hidden = false;
  PedestrianWalker best;
  for (int attempt = 0; attempt < 24; ++attempt) {
    float pick = unit(rng) * total;
    int index = candidates.back();
    for (int candidate : candidates) {
      pick -= data_->rows[static_cast<size_t>(candidate)].area;
      if (pick <= 0.f) {
        index = candidate;
        break;
      }
    }
    const Row& row = data_->rows[static_cast<size_t>(index)];
    const float r1 = unit(rng), r2 = unit(rng), r3 = unit(rng);
    V p;
    if (row.runs.size() == 2) {
      // Original 0x00589550, corridor: the same cross-section of both runs
      // (they run in opposite directions), 10-90 % of the way across.
      const Edge& a = row.edges[static_cast<size_t>(row.runs[0])];
      const Edge& b = row.edges[static_cast<size_t>(row.runs[1])];
      const V pa = point_at(a, (a.length - 1.f) * r1 + 0.5f);
      const V pb = point_at(b, (b.length - 1.f) * (1.f - r1) + 0.5f);
      p = lerp(pb, pa, 0.8f * r2 + 0.1f);
    } else {
      // Original 0x00589550, other rows: a triangle of the fan from the first
      // edge's start.
      std::vector<V> corners;
      for (const Edge& edge : row.edges) corners.push_back(edge.points.front());
      if (corners.size() < 3) continue;
      const size_t n = corners.size();
      const size_t i = std::min(n - 2, static_cast<size_t>(r1 * static_cast<float>(n - 2)) + 1);
      const float a = 1.f - r2, b = r2 * r3;
      p.x = corners[i].x * a + corners[(i + 1) % n].x * b + corners[0].x * (1.f - a - b);
      p.z = corners[i].z * a + corners[(i + 1) % n].z * b + corners[0].z * (1.f - a - b);
    }
    p = constrain(row, p, p, 0.f);
    if (!inside_row(row, p)) continue;
    p.y = row_height(row, p.x, p.z);
    if (std::fabs(p.y - y) > 20.f) continue;
    const float distance = flat(p, focus);
    const bool off = hidden && hidden(p.x, p.y, p.z, hidden_user);
    const float nearest_allowed = hidden ? (off ? 12.f : 35.f) : min_distance;
    if (distance > radius || distance < nearest_allowed) continue;
    bool taken = false;
    for (int i = 0; blocked && i < blocked_count && !taken; ++i)
      taken = inside_block(p.x, p.y, p.z, blocked[i]);
    if (taken) continue;
    if (have && (have_hidden || !off)) continue;
    // Original: a random heading.
    const float angle = unit(rng) * 6.2831853f;
    best = PedestrianWalker{};
    best.row = index;
    best.x = p.x;
    best.y = p.y;
    best.z = p.z;
    best.hx = std::sin(angle);
    best.hz = std::cos(angle);
    best.yaw = angle;
    best.ok = true;
    have = true;
    have_hidden = off;
    if (!hidden || off) break;
  }
  if (!have) return false;
  out = best;
  return true;
}

void PedestrianLines::step(std::vector<PedestrianWalker>& walkers, float dt,
                           const PedestrianCar* cars, int car_count) const {
  // The original takes one step of the elapsed time; a stall here would
  // throw walkers through walls, so long frames are capped.
  dt = std::clamp(dt, 0.f, 0.1f);
  if (dt <= 0.f) return;
  const int row_count = static_cast<int>(data_->rows.size());
  std::unordered_map<int, std::vector<int>> by_row;
  for (int i = 0; i < static_cast<int>(walkers.size()); ++i) {
    PedestrianWalker& w = walkers[static_cast<size_t>(i)];
    if (!w.ok) continue;
    if (w.row < 0 || w.row >= row_count) {
      w.ok = false;
      continue;
    }
    by_row[w.row].push_back(i);
  }
  struct Slot {
    float rank = 0.f;
    bool walker = false;
    float d = 0.f;
    float dx = 0.f, dz = 0.f;    // unit direction toward the obstacle
    float ovx = 0.f, ovz = 0.f;  // the other walker's velocity
  };
  struct Move {
    float hx = 0.f, hz = 1.f, vx = 0.f, vz = 0.f;
    bool fleeing = false;
  };
  std::vector<Move> moves(walkers.size());
  for (int i = 0; i < static_cast<int>(walkers.size()); ++i) {
    const PedestrianWalker& w = walkers[static_cast<size_t>(i)];
    if (!w.ok) continue;
    const Row& row = data_->rows[static_cast<size_t>(w.row)];
    const V p{w.x, w.y, w.z};
    // Ranking (0x00586cd0): walls, runs and walkers of this row and of the
    // rows it lists. The original ranks on the squared distance.
    Slot slots[kSlots];
    int used = 0;
    auto offer = [&](const Slot& slot) {
      if (slot.rank <= 0.f) return;
      if (used < kSlots) {
        slots[used++] = slot;
        return;
      }
      int weakest = -1;
      for (int k = 0; k < kSlots; ++k)
        if (slots[k].rank < slot.rank && (weakest < 0 || slots[k].rank < slots[weakest].rank))
          weakest = k;
      if (weakest >= 0) slots[weakest] = slot;
    };
    for (int link : row.links) {
      const Row& other = data_->rows[static_cast<size_t>(link)];
      if (flat(other.center, p) > other.reach + 4.f) continue;
      for (const Edge& edge : other.edges) {
        // Gates have no behaviour entry and exert no force.
        if (edge.type == EdgeType::Gate) continue;
        const Nearest n = nearest(edge, p, 4.f);
        if (n.distance > 4.f || std::fabs(n.point.y - p.y) > 3.f) continue;
        Slot slot;
        slot.d = n.distance;
        slot.rank = kEdgeCurve(n.distance * n.distance - kRadius);
        if (n.distance > 1e-5f) {
          slot.dx = (n.point.x - p.x) / n.distance;
          slot.dz = (n.point.z - p.z) / n.distance;
        } else {
          slot.dx = -n.normal.x;
          slot.dz = -n.normal.z;
        }
        offer(slot);
      }
      const auto there = by_row.find(link);
      if (there == by_row.end()) continue;
      for (int j : there->second) {
        if (j == i) continue;
        const PedestrianWalker& o = walkers[static_cast<size_t>(j)];
        if (std::fabs(o.y - p.y) > 3.f) continue;
        const float ex = o.x - p.x, ez = o.z - p.z;
        const float length = std::hypot(ex, ez);
        if (length > 12.f) continue;
        Slot slot;
        slot.walker = true;
        slot.d = std::max(0.f, length - kRadius);
        slot.rank = kWalkerRank(slot.d * slot.d - kRadius);
        slot.dx = length > 1e-5f ? ex / length : 1.f;
        slot.dz = length > 1e-5f ? ez / length : 0.f;
        slot.ovx = o.vx;
        slot.ovz = o.vz;
        offer(slot);
      }
    }
    // Steering (0x00586e40, deflection 0x005857a0).
    float ax = w.hx * kHeadingWeight, az = w.hz * kHeadingWeight;
    float total = kHeadingWeight;
    float turn_x = 0.f, turn_z = 0.f;
    for (int k = 0; k < used; ++k) {
      const Slot& slot = slots[k];
      float vx = 0.f, vz = 0.f, f = 0.f;
      if (!slot.walker) {
        // Walls and runs behind the motion are ignored.
        if (w.vx * slot.dx + w.vz * slot.dz < 0.f) continue;
        const float bend = 2.f / std::max(1.f, slot.d + 1.f - kRadius);
        vx = w.hx - bend * slot.dx;
        vz = w.hz - bend * slot.dz;
        const float l = std::hypot(vx, vz);
        if (l < 1e-6f) {
          vx = -slot.dz;
          vz = slot.dx;
        } else {
          vx /= l;
          vz /= l;
        }
        f = kEdgeCurve(slot.d - kRadius);
        turn_x += f * vx;
        turn_z += f * vz;
      } else {
        // Pass on the side away from the other walker, judged on the
        // relative velocity.
        float ux = w.hx - slot.ovx, uz = w.hz - slot.ovz;
        const float ul = std::hypot(ux, uz);
        if (ul < 1e-6f) {
          ux = w.hx;
          uz = w.hz;
        } else {
          ux /= ul;
          uz /= ul;
        }
        const float lx = -uz, lz = ux;
        const float side = (slot.d + kRadius) * (slot.dx * lx + slot.dz * lz);
        vx = side < 0.f ? lx : -lx;
        vz = side < 0.f ? lz : -lz;
        if (slot.d - kRadius < 0.f) {
          vx += (slot.d - kRadius) * slot.dx;
          vz += (slot.d - kRadius) * slot.dz;
          const float l = std::hypot(vx, vz);
          if (l > 1e-6f) {
            vx /= l;
            vz /= l;
          }
        }
        const float ahead = std::clamp(
            (slot.d + kRadius) * (ux * slot.dx + uz * slot.dz) + 2.f * kRadius, 0.f, 1.f);
        f = kWalkerSteer(std::max(std::fabs(side) - 2.f * kRadius, 0.f)) * ahead /
            std::max(1.f, slot.d);
      }
      ax += f * vx;
      az += f * vz;
      total += f;
    }
    bool fleeing = false;
    for (int c = 0; cars && c < car_count; ++c) {
      const PedestrianCar& car = cars[c];
      if (std::fabs(car.y - p.y) > 4.f) continue;
      const float rx = p.x - car.x, rz = p.z - car.z;
      const float distance = std::hypot(rx, rz);
      const float reach = kCarRadius + kRadius;
      const float speed = std::hypot(car.vx, car.vz);
      const float fx = speed > 1e-3f ? car.vx / speed : 0.f;
      const float fz = speed > 1e-3f ? car.vz / speed : 0.f;
      float k = -1.f, weight = 1.f, push_x = 0.f, push_z = 0.f;
      // Too close: step straight away. The original compares the squared
      // distance with reach + 1, which only triggers inside about 2.2 m.
      if (distance * distance < reach + 1.f && distance > 1e-4f) {
        k = 1.f;
        push_x = rx / distance;
        push_z = rz / distance;
      }
      // In the car's path within five seconds: move aside, more the nearer
      // the walker is to its line.
      const float lat = rx * -fz + rz * fx;
      const float lon = rx * fx + rz * fz;
      if (speed > 1e-3f && std::fabs(lat) < reach + 1.f && lon > 0.f && lon < kCarSeconds * speed) {
        float q = 1.f - (std::fabs(lon) + 1.f) / (kCarSeconds * speed + 1.f);
        if (std::fabs(lat) - reach > 0.f) q *= 1.f - (std::fabs(lat) - reach);
        if (k < q) {
          // Sideways, away from the side of the car's line the walker is on.
          // The original picks the side with a cross product whose operand
          // order is not settled; this keeps the walker on its own side.
          const float sign = lat >= 0.f ? 1.f : -1.f;
          push_x = (1.f - q) * fx + q * -fz * sign;
          push_z = (1.f - q) * fz + q * fx * sign;
          k = q;
          weight = 7.f * q;
        }
      }
      if (k <= -0.5f) continue;
      const float l = std::hypot(push_x, push_z);
      if (l < 1e-6f) continue;
      fleeing = true;
      ax += push_x / l * weight * kFleeWeight * k;
      az += push_z / l * weight * kFleeWeight * k;
      total += kFleeWeight * k;
    }
    Move& m = moves[static_cast<size_t>(i)];
    m.fleeing = fleeing;
    if (total > 0.f) {
      ax /= total;
      az /= total;
    }
    // A resultant shorter than 1 walks at the square root of its length.
    const float m2 = ax * ax + az * az;
    if (m2 > 0.f && m2 < 1.f) {
      const float lift = 1.f / std::sqrt(std::sqrt(m2));
      ax *= lift;
      az *= lift;
    }
    m.vx = ax * kSpeed;
    m.vz = az * kSpeed;
    // Only walls and runs turn the heading.
    m.hx = w.hx;
    m.hz = w.hz;
    if (turn_x != 0.f || turn_z != 0.f) {
      float hx = w.hx + 0.5f * turn_x / total, hz = w.hz + 0.5f * turn_z / total;
      const float l = std::hypot(hx, hz);
      if (l > 1e-6f) {
        m.hx = hx / l;
        m.hz = hz / l;
      }
    }
  }
  for (int i = 0; i < static_cast<int>(walkers.size()); ++i) {
    PedestrianWalker& w = walkers[static_cast<size_t>(i)];
    if (!w.ok) continue;
    const Move& m = moves[static_cast<size_t>(i)];
    const V before{w.x, w.y, w.z};
    V p{w.x + m.vx * dt, w.y, w.z + m.vz * dt};
    const Row* row = &data_->rows[static_cast<size_t>(w.row)];
    p = constrain(*row, p, before, std::hypot(m.vx, m.vz) * dt);
    // Leave through a gate: the walker belongs to the row behind it from now
    // on (original gate pass in 0x00589cb0). With no row there it is gone.
    // The original tests the gate's whole line. Here the line counts only
    // near the gate or once the walker is outside the row, so a long, bent
    // corridor cannot leak through the line of a distant gate.
    const bool outside = !inside_row(*row, p);
    bool moved_row = false;
    for (const Edge& edge : row->edges) {
      if (edge.type != EdgeType::Gate) continue;
      const V a = edge.points.front();
      const V b = edge.points.back();
      const float len = std::max(1e-5f, edge.length);
      const float nx = -(b.z - a.z) / len, nz = (b.x - a.x) / len;
      const float side = (p.x - a.x) * nx + (p.z - a.z) * nz;
      const float along = ((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / len;
      if (side >= 0.f || (!outside && (along < -kRadius || along > len + kRadius))) continue;
      if (edge.target < 0) {
        w.ok = false;
        break;
      }
      w.row = edge.target;
      row = &data_->rows[static_cast<size_t>(w.row)];
      p = constrain(*row, p, p, 0.f);
      moved_row = true;
      break;
    }
    // Out of the row without passing a gate (round a wall's end): step back.
    if (w.ok && outside && !moved_row) p = before;
    if (!w.ok) continue;
    p.y = row_height(*row, p.x, p.z);
    w.x = p.x;
    w.y = p.y;
    w.z = p.z;
    w.hx = m.hx;
    w.hz = m.hz;
    w.vx = (p.x - before.x) / dt;
    w.vz = (p.z - before.z) / dt;
    w.fleeing = m.fleeing;
    const float moved = flat(p, before);
    w.speed = moved / dt;
    // Facing (0x00587820): toward the travel direction, ignoring wobble
    // under 5 degrees, turning at most pi radians per metre walked.
    float turned = 0.f;
    if (moved > 1e-4f) {
      const float target = std::atan2(p.x - before.x, p.z - before.z);
      float diff = std::remainder(target - w.yaw, 6.2831853f);
      if (std::fabs(diff) < 0.0872665f) diff = 0.f;
      diff = std::clamp(diff, -moved * 3.14159265f, moved * 3.14159265f);
      w.yaw = std::remainder(w.yaw + diff, 6.2831853f);
      turned = std::fabs(diff);
    }
    w.gait = (moved + 0.5f * turned) / dt;
  }
}

}  // namespace slrr
