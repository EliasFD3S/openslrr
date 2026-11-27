#include "slrr/road_routes.hpp"
#include "slrr/job_system.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_set>
namespace slrr {
namespace {
RoadPoint sub(const RoadPoint &a, const RoadPoint &b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float dot(const RoadPoint &a, const RoadPoint &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
RoadPoint lerp(const RoadPoint &a, const RoadPoint &b, float t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
          a[2] + (b[2] - a[2]) * t};
}
bool finite(const RoadPoint &p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
RoadPoint edge_at(const RoadRoutes::Edge &e, float at) {
  auto it = std::upper_bound(e.distances.begin(), e.distances.end(), at);
  size_t i =
      std::clamp<size_t>(it - e.distances.begin(), 1, e.points.size() - 1);
  return lerp(e.points[i - 1], e.points[i],
              std::clamp((at - e.distances[i - 1]) /
                             (e.distances[i] - e.distances[i - 1]),
                         0.f, 1.f));
}
} // namespace
float road_distance(const RoadPoint &a, const RoadPoint &b) {
  auto d = sub(a, b);
  return std::sqrt(dot(d, d));
}
RoadPoint road_normalize(RoadPoint p) {
  const auto l = road_distance(p, {});
  if (l > 1e-8f)
    for (auto &v : p)
      v /= l;
  return p;
}
RoadPoint road_at(const RoadRoute &r, float distance) {
  if (r.points.empty())
    return {};
  for (size_t i = 1; i < r.points.size(); ++i) {
    float l = road_distance(r.points[i - 1], r.points[i]);
    if (distance <= l)
      return lerp(r.points[i - 1], r.points[i],
                  l > 0 ? std::clamp(distance / l, 0.f, 1.f) : 0);
    distance -= l;
  }
  return r.points.back();
}
RoadRoutes::RoadRoutes(const RoadNetwork &net,
                       const std::vector<RoadPoint> &fixtures) {
  // SLRR_rev traffic bea8ce0 road-traffic.ts: actual traffic reverses the
  // blue RoadOther records and follows only explicit grey crossing curves.
  auto curve = [](const std::vector<RoadSegment> &segments) {
    Edge e;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
      int steps =
          int(std::clamp(std::ceil(double(it->length) / 1.5), 12., 256.));
      for (int i = 0; i <= steps; ++i) {
        auto point = trc_sample(*it, 1 - float(i) / steps);
        float step =
            e.points.empty() ? 0 : road_distance(e.points.back(), point);
        if (!e.points.empty() && i == 0 && step > .03f)
          return Edge{};
        if (!e.points.empty() && step < 1e-7f)
          continue;
        e.length += step;
        e.points.push_back(point);
        e.distances.push_back(e.length);
      }
    }
    return e;
  };
  std::unordered_map<uint32_t, size_t> lane_index;
  for (const auto &road : net.others) {
    auto e = curve(road.segments);
    if (e.points.size() < 2)
      continue;
    e.id = road.id;
    e.branch = road.refs[0];
    e.roads = {road.refs[0], road.id};
    for (const auto &node : net.nodes)
      for (const auto &link : node.links)
        if (link.target == road.id) {
          if (link.flag)
            e.to = node.id;
          else
            e.from = node.id;
          // Either rural (group 0) endpoint makes the whole road rural.
          e.speed_group = node.group == 0 || e.speed_group == 0 ? 0 : 1;
        }
    lane_index[road.id] = traffic_edges_.size();
    traffic_edges_.push_back(std::move(e));
  }
  for (const auto &node : net.nodes)
    for (const auto &incoming : node.links) {
      auto a = lane_index.find(incoming.target);
      if (a == lane_index.end())
        continue;
      for (const auto &lane : node.lanes) {
        if (road_distance(traffic_edges_[a->second].points.back(),
                          lane.segment.end) > .03f)
          continue;
        for (const auto &outgoing : node.links) {
          auto b = lane_index.find(outgoing.target);
          if (b == lane_index.end() || a == b ||
              road_distance(lane.segment.start,
                            traffic_edges_[b->second].points.front()) > .03f)
            continue;
          auto turn = curve({lane.segment});
          if (turn.points.size() < 2)
            continue;
          turn.crossing = true;
          turn.from = turn.to = node.id;
          turn.speed_group = node.group == 0 ? 0 : 1;
          turn.branch = traffic_edges_[b->second].branch;
          turn.roads = traffic_edges_[a->second].roads;
          turn.next = {b->second};
          traffic_edges_[a->second].next.push_back(traffic_edges_.size());
          traffic_edges_.push_back(std::move(turn));
        }
      }
    }
  // Each worker reads an immutable lane and writes its own index fragment.
  // Merge/publication happens after the pool joins, before the graph is shared.
  struct IndexPart {
    bool complete = true;
    std::vector<std::pair<uint64_t, size_t>> cells;
  };
  std::vector<IndexPart> parts(traffic_edges_.size());
  jobs_parallel_for(parts.size(), [&](size_t edge) {
    auto &part = parts[edge];
    const auto &e = traffic_edges_[edge];
    for (size_t i = 1; i < e.points.size(); ++i) {
      auto a = e.points[i - 1], b = e.points[i];
      double x0 = std::floor(std::min(a[0], b[0]) / 64.),
             x1 = std::floor(std::max(a[0], b[0]) / 64.);
      double z0 = std::floor(std::min(a[2], b[2]) / 64.),
             z1 = std::floor(std::max(a[2], b[2]) / 64.);
      if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(z0) ||
          !std::isfinite(z1) || x0 < INT32_MIN || x1 > INT32_MAX ||
          z0 < INT32_MIN || z1 > INT32_MAX ||
          (x1 - x0 + 1) * (z1 - z0 + 1) > 4096) {
        part.complete = false;
        continue;
      }
      for (int64_t x = int64_t(x0); x <= int64_t(x1); ++x)
        for (int64_t z = int64_t(z0); z <= int64_t(z1); ++z)
          part.cells.push_back(
              {(uint64_t(uint32_t(x)) << 32) | uint32_t(z), i});
    }
  });
  for (size_t edge = 0; edge < parts.size(); ++edge) {
    traffic_complete_grid_ &= parts[edge].complete;
    for (auto [key, segment] : parts[edge].cells)
      traffic_cells_[key].push_back({edge, segment});
  }
  for (const auto &n : net.nodes)
    nodes_.push_back({n.id, n.position});
  auto node = [&](uint32_t id) -> const RoadPoint * {
    for (const auto &n : nodes_)
      if (n.first == id)
        return &n.second;
    return nullptr;
  };
  auto add = [&](uint32_t id, uint32_t from, uint32_t to,
                 const std::vector<RoadSegment> &segments,
                 std::vector<uint32_t> lanes) {
    const auto *a = node(from), *b = node(to);
    if (!a || !b || from == to)
      return;
    std::vector<RoadPoint> samples;
    for (const auto &s : segments) {
      int count = int(std::clamp(std::ceil(double(s.length) / 2), 8., 512.));
      for (int i = 0; i <= count; ++i)
        samples.push_back(trc_sample(s, float(i) / count));
    }
    if (samples.size() < 2)
      return;
    if (road_distance(samples.front(), *a) + road_distance(samples.back(), *b) >
        road_distance(samples.front(), *b) + road_distance(samples.back(), *a))
      std::reverse(samples.begin(), samples.end());
    for (auto pair : {std::pair{from, road_distance(samples.front(), *a)},
                      std::pair{to, road_distance(samples.back(), *b)}}) {
      auto found = radii_.find(pair.first);
      if (found == radii_.end() || pair.second < found->second)
        radii_[pair.first] = pair.second;
    }
    samples.insert(samples.begin(), *a);
    samples.push_back(*b);
    Edge e;
    e.id = id;
    e.from = from;
    e.to = to;
    e.roads = std::move(lanes);
    for (const auto &p : samples) {
      float d = e.points.empty() ? 0 : road_distance(e.points.back(), p);
      if (!e.points.empty() && d < 1e-7f)
        continue;
      e.length += d;
      e.points.push_back(p);
      e.distances.push_back(e.length);
    }
    if (std::isfinite(e.length) && e.length > 1e-6f)
      edges_.push_back(std::move(e));
  };
  auto linked = [&](uint32_t crossing, uint32_t lane, bool departing) {
    for (const auto &n : net.nodes)
      if (n.id == crossing)
        for (const auto &l : n.links)
          if (l.target == lane && ((l.flag == 0) == departing))
            return true;
    return false;
  };
  for (const auto &p : net.paths) {
    std::vector<const RoadOther *> lanes;
    for (const auto &r : net.others)
      if (r.refs[0] == p.id)
        lanes.push_back(&r);
    for (auto pair :
         {std::pair{p.refs[1], p.refs[2]}, std::pair{p.refs[2], p.refs[1]}}) {
      std::vector<uint32_t> ids;
      for (auto *r : lanes)
        if (linked(pair.first, r->id, true) &&
            linked(pair.second, r->id, false))
          ids.push_back(r->id);
      if (!ids.empty())
        add(p.id, pair.first, pair.second, p.segments, std::move(ids));
    }
    if (lanes.empty())
      add(p.id, p.refs[1], p.refs[2], p.segments, {});
  }
  if (edges_.empty())
    for (const auto &r : net.others) {
      uint32_t a = 0, b = 0;
      for (const auto &n : net.nodes) {
        if (linked(n.id, r.id, true) && !a)
          a = n.id;
        if (linked(n.id, r.id, false) && !b)
          b = n.id;
      }
      if (a && b)
        add(r.id, a, b, r.segments, {r.id});
    }
  // SLRR_rev traffic bea8ce06 traffic-control.ts: bind real trlight heads
  // to the nearest crossing and incoming lane endpoint, then cycle branches.
  for (const auto &fixture : fixtures) {
    const RoadNode *cross = nullptr;
    float nearest = INFINITY;
    for (const auto &n : net.nodes) {
      float d = road_distance(fixture, n.position);
      if (d < nearest) {
        nearest = d;
        cross = &n;
      }
    }
    if (!cross)
      continue;
    const RoadOther *incoming = nullptr;
    nearest = INFINITY;
    for (const auto &link : cross->links)
      if (link.flag)
        for (const auto &road : net.others)
          if (road.id == link.target && !road.segments.empty()) {
            float d = road_distance(fixture, road.segments.front().start);
            if (d < nearest) {
              nearest = d;
              incoming = &road;
            }
          }
    if (incoming) {
      auto branch = incoming->refs[0] ? incoming->refs[0] : incoming->id;
      signals_.push_back({fixture, cross->id, branch});
      auto &branches = cycles_[cross->id];
      if (std::find(branches.begin(), branches.end(), branch) == branches.end())
        branches.push_back(branch);
    }
  }
  // v221's 64 m segment grid keeps traffic's repeated projections local.
  for (size_t ei = 0; ei < edges_.size(); ++ei)
    for (size_t i = 1; i < edges_[ei].points.size(); ++i) {
      const auto &a = edges_[ei].points[i - 1], &b = edges_[ei].points[i];
      double x0 = std::floor(std::min(a[0], b[0]) / 64.),
             x1 = std::floor(std::max(a[0], b[0]) / 64.),
             z0 = std::floor(std::min(a[2], b[2]) / 64.),
             z1 = std::floor(std::max(a[2], b[2]) / 64.);
      if (x0 < INT32_MIN || x1 > INT32_MAX || z0 < INT32_MIN ||
          z1 > INT32_MAX || (x1 - x0 + 1) * (z1 - z0 + 1) > 4096) {
        complete_grid_ = false;
        continue;
      }
      for (int64_t x = int64_t(x0); x <= int64_t(x1); ++x)
        for (int64_t z = int64_t(z0); z <= int64_t(z1); ++z)
          cells_[(uint64_t(uint32_t(x)) << 32) | uint32_t(z)].push_back(
              {ei, i});
    }
}
int RoadRoutes::signal(uint32_t crossing, uint32_t branch,
                       double seconds) const {
  auto it = cycles_.find(crossing);
  if (it == cycles_.end() || it->second.empty())
    return -1;
  if (std::find(it->second.begin(), it->second.end(), branch) ==
      it->second.end())
    return -2; // Missing lamp: timed STOP, never a permanent red.
  constexpr double green = 10, phase = 13;
  double clock = std::max(0., seconds);
  auto slot = uint64_t(clock / phase);
  auto active = it->second[slot % it->second.size()];
  return active != branch ? 0 : std::fmod(clock, phase) < green ? 2 : 1;
}
int RoadRoutes::edge_signal(size_t index, double seconds) const {
  if (index >= edges_.size())
    return -1;
  const auto &e = edges_[index];
  return signal(e.to, e.id, seconds);
}
std::optional<RoadPoint> RoadRoutes::nearest_cross(const RoadPoint &p,
                                                   float radius) const {
  if (!finite(p) || !std::isfinite(radius))
    return {};
  float best = std::numeric_limits<float>::infinity();
  std::optional<RoadPoint> result;
  for (const auto &n : nodes_) {
    float d = std::fabs(road_distance(p, n.second) - std::max(0.f, radius));
    if (d < best) {
      best = d;
      result = n.second;
    }
  }
  return result;
}
std::optional<RoadProjection>
RoadRoutes::project(const RoadPoint &p, const RoadPoint *heading) const {
  if (!finite(p))
    return {};
  std::optional<RoadProjection> best;
  float best_score = INFINITY;
  auto visit = [&](size_t e, size_t i) {
    const auto &edge = edges_[e];
    const auto delta = sub(edge.points[i], edge.points[i - 1]);
    float t = std::clamp(
        dot(sub(p, edge.points[i - 1]), delta) / dot(delta, delta), 0.f, 1.f);
    auto near = lerp(edge.points[i - 1], edge.points[i], t);
    float d = road_distance(p, near);
    float score =
        d + (heading ? .25f * (1 - dot(road_normalize(delta), *heading)) : 0);
    if (!best || score < best_score) {
      best_score = score;
      best = RoadProjection{near, road_normalize(delta), d,
                            edge.distances[i - 1] +
                                t * (edge.distances[i] - edge.distances[i - 1]),
                            e};
    }
  };
  if (complete_grid_ && std::fabs(double(p[0])) < 1.e10 &&
      std::fabs(double(p[2])) < 1.e10) {
    int64_t cx = int64_t(std::floor(p[0] / 64.)),
            cz = int64_t(std::floor(p[2] / 64.));
    for (int ring = 0; ring < 4; ++ring) {
      for (int64_t x = cx - ring; x <= cx + ring; ++x)
        for (int64_t z = cz - ring; z <= cz + ring; ++z)
          if (!ring || x == cx - ring || x == cx + ring || z == cz - ring ||
              z == cz + ring) {
            auto cell =
                cells_.find((uint64_t(uint32_t(x)) << 32) | uint32_t(z));
            if (cell != cells_.end())
              for (auto [e, i] : cell->second)
                visit(e, i);
          }
      double boundary =
          std::min({p[0] - (cx - ring) * 64., (cx + ring + 1) * 64. - p[0],
                    p[2] - (cz - ring) * 64., (cz + ring + 1) * 64. - p[2]});
      if (best && best_score < boundary)
        return best;
    }
  }
  for (size_t e = 0; e < edges_.size(); ++e)
    for (size_t i = 1; i < edges_[e].points.size(); ++i)
      visit(e, i);
  return best;
}
RoadRoutes::Anchor RoadRoutes::anchor(const RoadPoint &p) const {
  Anchor a;
  float distance = std::numeric_limits<float>::infinity();
  for (const auto &n : nodes_) {
    float d = road_distance(p, n.second);
    if (d < distance) {
      a.node = n.first;
      distance = d;
    }
  }
  auto r = radii_.find(a.node);
  float radius = r == radii_.end() ? 0 : r->second;
  if (a.node && distance <= std::clamp(radius + 1, 3.f, 15.f)) {
    a.distance = distance;
    return a;
  }
  a.node = 0;
  a.projection = project(p);
  if (a.projection)
    a.distance = a.projection->distance;
  return a;
}
std::optional<RoadRoute> RoadRoutes::query(const RoadPoint &s,
                                           const RoadPoint &f) const {
  if (!finite(s) || !finite(f) || edges_.empty())
    return {};
  if (road_distance(s, f) < 1e-8f)
    return RoadRoute{0, {}, {s}, {}};
  auto choices = [&](Anchor a) {
    std::vector<Anchor> out{a};
    if (a.projection) {
      const auto &e = edges_[a.projection->edge];
      for (size_t i = 0; i < edges_.size(); ++i)
        if (edges_[i].id == e.id && edges_[i].from == e.to &&
            edges_[i].to == e.from) {
          a.projection->edge = i;
          a.projection->along = edges_[i].length - a.projection->along;
          out.push_back(a);
          break;
        }
    }
    return out;
  };
  std::optional<RoadRoute> best;
  for (const auto &a : choices(anchor(s)))
    for (const auto &b : choices(anchor(f))) {
      auto r = between(s, f, a, b);
      if (r && (!best || r->length < best->length))
        best = std::move(r);
    }
  return best;
}
std::optional<RoadRoute> RoadRoutes::between(const RoadPoint &s,
                                             const RoadPoint &f,
                                             const Anchor &a,
                                             const Anchor &b) const {
  if ((!a.node && !a.projection) || (!b.node && !b.projection))
    return {};
  uint32_t source = a.node ? a.node : edges_[a.projection->edge].to,
           target = b.node ? b.node : edges_[b.projection->edge].from;
  std::unordered_map<uint32_t, float> distance;
  std::unordered_map<uint32_t, size_t> previous;
  using Item = std::pair<float, uint32_t>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
  distance[source] = 0;
  queue.push({0.f, source});
  while (!queue.empty()) {
    auto [d, current] = queue.top();
    queue.pop();
    if (d != distance[current])
      continue;
    for (size_t i = 0; i < edges_.size(); ++i) {
      const auto &e = edges_[i];
      if (e.from != current)
        continue;
      float cost = d + e.length;
      auto found = distance.find(e.to);
      if (found == distance.end() || cost < found->second) {
        distance[e.to] = cost;
        previous[e.to] = i;
        queue.push({cost, e.to});
      }
    }
  }
  float length = distance.contains(target)
                     ? distance[target]
                     : std::numeric_limits<float>::infinity();
  if (a.projection)
    length += edges_[a.projection->edge].length - a.projection->along;
  if (b.projection)
    length += b.projection->along;
  std::vector<size_t> route;
  if (a.projection && b.projection &&
      a.projection->edge == b.projection->edge &&
      b.projection->along >= a.projection->along &&
      b.projection->along - a.projection->along <= length) {
    length = b.projection->along - a.projection->along;
    route.push_back(a.projection->edge);
  } else {
    if (!std::isfinite(length))
      return {};
    for (auto id = target; id != source;) {
      auto it = previous.find(id);
      if (it == previous.end())
        return {};
      route.push_back(it->second);
      id = edges_[it->second].from;
    }
    std::reverse(route.begin(), route.end());
    if (a.projection)
      route.insert(route.begin(), a.projection->edge);
    if (b.projection)
      route.push_back(b.projection->edge);
  }
  RoadRoute out;
  out.length = length + a.distance + b.distance;
  out.points.push_back(s);
  std::unordered_set<uint32_t> roads;
  auto push = [&](const RoadPoint &p) {
    if (road_distance(out.points.back(), p) > 1e-6f)
      out.points.push_back(p);
  };
  for (size_t i = 0; i < route.size(); ++i) {
    const auto &e = edges_[route[i]];
    float lo = i == 0 && a.projection ? a.projection->along : 0,
          hi = i + 1 == route.size() && b.projection ? b.projection->along
                                                     : e.length;
    push(edge_at(e, lo));
    for (size_t j = 1; j < e.points.size(); ++j)
      if (e.distances[j] > lo && e.distances[j] < hi)
        push(e.points[j]);
    push(edge_at(e, hi));
    for (auto road : e.roads)
      if (roads.insert(road).second)
        out.roads.push_back(road);
  }
  push(f);
  if (out.points.size() > 1)
    out.direction = road_normalize(sub(out.points[1], s));
  if (!route.empty()) {
    const auto &e = edges_[route.front()];
    float at = a.projection ? a.projection->along : 0;
    auto it =
        std::upper_bound(e.distances.begin(), e.distances.end(), at + 1e-7f);
    size_t i =
        std::clamp<size_t>(it - e.distances.begin(), 1, e.points.size() - 1);
    out.direction = road_normalize(sub(e.points[i], e.points[i - 1]));
  }
  return out;
}
} // namespace slrr

namespace slrr {
std::optional<RoadProjection>
RoadRoutes::traffic_project(const RoadPoint &point, const RoadPoint *heading,
                            bool include_crossings) const {
  if (!finite(point) || (heading && !finite(*heading)))
    return {};
  std::optional<RoadProjection> result;
  float best = INFINITY;
  std::pair<size_t, size_t> chosen{SIZE_MAX, SIZE_MAX};
  auto examine = [&](size_t edge, size_t i) {
    const auto &e = traffic_edges_[edge];
    if ((!include_crossings && e.crossing) || e.next.empty())
      return;
    auto delta = sub(e.points[i], e.points[i - 1]), planar = delta;
    planar[1] = 0;
    float length = dot(planar, planar);
    if (length < 1e-8f)
      return;
    float t =
        std::clamp(dot(sub(point, e.points[i - 1]), planar) / length, 0.f, 1.f);
    auto p = lerp(e.points[i - 1], e.points[i], t),
         direction = road_normalize(delta);
    float distance = road_distance(point, p), score = distance;
    if (heading)
      score += 2 * (1 - dot(direction, *heading));
    auto order = std::pair{edge, i};
    if (score < best || (score == best && order < chosen)) {
      best = score;
      chosen = order;
      result = RoadProjection{
          p, direction, distance,
          e.distances[i - 1] + t * (e.distances[i] - e.distances[i - 1]), edge};
    }
  };
  const double cell_x = std::floor(double(point[0]) / 64),
               cell_z = std::floor(double(point[2]) / 64);
  if (traffic_complete_grid_ && cell_x > INT32_MIN + 16 &&
      cell_x < INT32_MAX - 16 && cell_z > INT32_MIN + 16 &&
      cell_z < INT32_MAX - 16) {
    int64_t cx = int64_t(cell_x), cz = int64_t(cell_z);
    double allowance =
        heading ? std::max(0., 2 * (double(road_distance(*heading, {})) - 1))
                : 0;
    for (int ring = 0; ring <= 16; ++ring) {
      for (int64_t x = cx - ring; x <= cx + ring; ++x)
        for (int64_t z = cz - ring; z <= cz + ring; ++z) {
          if (ring && std::abs(x - cx) != ring && std::abs(z - cz) != ring)
            continue;
          auto found =
              traffic_cells_.find((uint64_t(uint32_t(x)) << 32) | uint32_t(z));
          if (found != traffic_cells_.end())
            for (auto [edge, i] : found->second)
              examine(edge, i);
        }
      // Every unvisited segment lies outside this square. Its planar distance
      // (minus the largest possible heading bonus) bounds its score below.
      double outside = std::min(
          {point[0] - (cx - ring) * 64., (cx + ring + 1) * 64. - point[0],
           point[2] - (cz - ring) * 64., (cz + ring + 1) * 64. - point[2]});
      if (result && best + allowance < outside)
        return result;
    }
  }
  // Far-away points and incomplete/extreme input keep exhaustive semantics.
  for (size_t edge = 0; edge < traffic_edges_.size(); ++edge)
    for (size_t i = 1; i < traffic_edges_[edge].points.size(); ++i)
      examine(edge, i);
  return result;
}
std::array<float, 2> road_speed_band(int group) {
  return group == 0 ? std::array<float, 2>{60, 100} : std::array<float, 2>{15, 35};
}
float RoadRoutes::speed_limit(const RoadPoint &p) const {
  auto near = traffic_project(p, nullptr, true);
  if (!near || near->distance > 30)
    return -1;
  const int group = traffic_edges_[near->edge].speed_group;
  return group < 0 ? -1 : road_speed_band(group)[1] / 3.6f;
}
int RoadRoutes::traffic_signal(size_t edge, double seconds) const {
  if (edge >= traffic_edges_.size() || traffic_edges_[edge].crossing)
    return -1;
  const auto &e = traffic_edges_[edge];
  return signal(e.to, e.branch, seconds);
}
} // namespace slrr
