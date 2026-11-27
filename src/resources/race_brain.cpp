// Adapted from SLRR_rev v221 cbb99147 engine/race-driver.ts.
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
namespace slrr {
namespace {
struct Brain {
  void *key = nullptr;
  uint64_t incarnation = 0;
  Object *car = nullptr;
  uint64_t car_incarnation = 0;
  const RpkIndex *resources = nullptr;
  std::string mode;
  RoadRoute route;
  float progress = 0, steering = 0, level = 1, replan = 0;
  float blocked_for = 0, reverse_for = 0, reverse_steer = 0;
  float queued_for = 0, ignore_peers_for = 0;
  // Pedals a traffic driver last applied: they move at a bounded rate, so
  // the body does not pitch on every command change.
  float throttle = 0, brake = 0;
  // Traffic lane curves the driver has committed to, starting with the one
  // it is on. Replanning keeps them, so overlapping crossing curves cannot
  // swap the exit in the middle of a junction (#324).
  std::vector<size_t> plan;
  bool enter_requested = false;
  // SLRR_rev bot.ts cruise: a speed drawn from the road's speed band after
  // each junction, approached at .8 m/s2 up and 1.2 m/s2 down.
  float cruise = 0, cruise_command = -1;
  size_t cruise_edge = SIZE_MAX;
  // Lane change in progress (road-traffic.ts tryChangeLane): the route
  // blends from the source lane onto the target over `length` metres.
  float lane_check = 0;
  size_t lane_source = SIZE_MAX, lane_target = SIZE_MAX;
  float lane_start = 0, lane_length = 0;
  // A queued car holds its brake and reacts .5-1.5 s after the way clears.
  bool holding = false;
  float start_delay = -1;
  // Impact: brake until settled for 1 s, then rejoin the lane. Ram: stuck
  // below 3 km/h for 60 s without a halt, push on at 30 km/h for 10 s.
  bool impact = false;
  float settled_for = 0, idle_for = 0, ram_for = 0;
  int32_t target = 0;
  RoadPoint offset{};
  bool night = false, runout = false;
  RoadPoint finish{}, finish_direction{};
};
std::unordered_map<void *, Brain> brains;
std::vector<RacePeer> peers;
// Port of SLRR_rev v221 engine/bot.ts followingSpeed: keep a time gap to the
// nearest car ahead in a straight corridor as wide as both cars, and never
// close within 3 m of it.
// Where a peer lies along the planned path: its distance along the route
// from the car's own progress, its distance from the path and the path
// direction there. The corridor follows bends and junction curves instead of
// the straight line ahead, which caught oncoming cars on curves and cars
// waiting on a crossing street.
struct PathHit {
  float along = 0, lateral = INFINITY;
  RoadPoint direction{};
};
PathHit along_path(const RoadRoute &route, float progress, const RoadPoint &p,
                   float reach) {
  PathHit hit;
  float along = 0;
  for (size_t i = 1; i < route.points.size(); ++i) {
    const auto &a = route.points[i - 1], &q = route.points[i];
    const float dx = q[0] - a[0], dz = q[2] - a[2];
    const float length = std::hypot(dx, dz);
    if (length < 1e-4f)
      continue;
    if (along + length < progress) {
      along += length;
      continue;
    }
    if (along > progress + reach)
      break;
    const float t = std::clamp(
        ((p[0] - a[0]) * dx + (p[2] - a[2]) * dz) / (length * length), 0.f, 1.f);
    const float d = std::hypot(p[0] - a[0] - t * dx, p[2] - a[2] - t * dz);
    if (d < hit.lateral) {
      hit.lateral = d;
      hit.along = along + t * length - progress;
      hit.direction = {dx / length, 0, dz / length};
    }
    along += length;
  }
  return hit;
}
float following_speed(Object *car, Object *ignore, const RoadPoint &position,
                      const RoadPoint &forward, float speed, float desired,
                      const RoadRoute *route = nullptr, float progress = 0) {
  float own_length = 4.6f, own_width = 1.9f;
  for (const auto &peer : peers)
    if (peer.car == car) {
      own_length = 2 * peer.half_length;
      own_width = 2 * peer.half_width;
      break;
    }
  for (const auto &peer : peers) {
    if (peer.car == car || peer.car == ignore)
      continue;
    const float dx = peer.position[0] - position[0],
                dz = peer.position[2] - position[2];
    if (dx * dx + dz * dz > 70 * 70 ||
        std::fabs(peer.position[1] - position[1]) > 2.5f)
      continue;
    RoadPoint axis = forward;
    float along = dx * forward[0] + dz * forward[2];
    float lateral = std::fabs(dx * forward[2] - dz * forward[0]);
    if (route && route->points.size() > 1) {
      const auto hit = along_path(*route, progress, peer.position, 60);
      if (!std::isfinite(hit.lateral))
        continue;
      along = hit.along;
      lateral = hit.lateral;
      axis = hit.direction;
    }
    if (along <= 0 || along > 60)
      continue;
    const float alignment = std::min(
        1.f, std::fabs(peer.forward[0] * axis[0] + peer.forward[2] * axis[2]));
    const float across = std::sqrt(std::max(0.f, 1 - alignment * alignment));
    const float length =
        2 * (peer.half_length * alignment + peer.half_width * across);
    const float width =
        2 * (peer.half_width * alignment + peer.half_length * across);
    if (lateral > (own_width + width) * .5f + .3f)
      continue;
    const float gap = along - (own_length + length) * .5f;
    const float lead = std::max(0.f, peer.velocity[0] * axis[0] +
                                         peer.velocity[2] * axis[2]);
    desired = std::min(
        {desired, std::max(0.f, lead + (gap - 3 - std::max(0.f, speed) * 1.2f) * .7f),
         std::sqrt(6 * std::max(0.f, gap - 3))});
  }
  return desired;
}
Object *live_car(int32_t id) {
  auto *n = gameref_find_live(id);
  if (!n)
    return nullptr;
  return part_chassis(static_cast<Object *>(n->key));
}
bool read_point(std::istream &in, RoadPoint &p) {
  char a = 0, b = 0;
  return bool(in >> p[0] >> a >> p[1] >> b >> p[2]) && a == ',' && b == ',' &&
         std::all_of(p.begin(), p.end(),
                     [](float x) { return std::isfinite(x); });
}
void *car_ground(const RpkIndex &rpk, Object *car) {
  if (auto *ground = race_traffic_ground(car))
    return ground;
  // Race, night-race and police cars outside traffic reach their map through
  // setParent(map). Nothing tags a script GroundRef or a native map GameType
  // as GameRefKind::GroundRef at run time, so the first ancestor with a road
  // graph is the car's ground.
  auto *n = gameref_get(car);
  for (int i = 0; n && i < 32; ++i) {
    if (n->kind == GameRefKind::GroundRef || race_ground_roads(rpk, n->key))
      return n->key;
    n = gameref_get(n->parent);
  }
  return nullptr;
}
RoadPoint car_position(Object *car) {
  PartMatrix m;
  if (!scene_instances_script_matrix(car, m.data()))
    return {};
  return {m[12], m[13], m[14]};
}
RoadRoute spline(const RpkIndex &rpk, const std::string &file,
                 const RoadPoint &start, Object *car) {
  std::string path = file;
  std::replace(path.begin(), path.end(), '\\', '/');
  std::filesystem::path relative(path);
  if (relative.is_absolute())
    return {};
  for (const auto &p : relative)
    if (p == "..")
      return {};
  std::ifstream in(std::filesystem::path(rpk.game_root()) / relative);
  RoadRoute r;
  RoadPoint p, d;
  std::vector<std::pair<RoadPoint, RoadPoint>> keys;
  std::string line;
  while (std::getline(in, line) && keys.size() < 100000) {
    std::istringstream row(line);
    if (row >> p[0] >> p[1] >> p[2] >> d[0] >> d[1] >> d[2]) {
      if (!std::all_of(p.begin(), p.end(),
                       [](float v) { return std::isfinite(v); }) ||
          !std::all_of(d.begin(), d.end(),
                       [](float v) { return std::isfinite(v); }))
        return {};
      keys.push_back({p, d});
    }
  }
  if (keys.size() < 2)
    return {};
  // A closed spline has no privileged first key. Project onto its complete
  // curve, choose the travel direction matching the car, and rotate the loop.
  std::vector<RoadPoint> points;
  for (size_t i = 0; i + 1 < keys.size(); ++i) {
    RoadSegment segment;
    segment.start = keys[i].first;
    segment.end = keys[i + 1].first;
    segment.length = road_distance(segment.start, segment.end);
    for (int axis = 0; axis < 3; ++axis) {
      segment.start_tangent[axis] = keys[i].second[axis] * segment.length;
      segment.end_tangent[axis] = keys[i + 1].second[axis] * segment.length;
    }
    int count = int(std::clamp(std::ceil(segment.length / 2), 16.f, 256.f));
    for (int j = 0; j < count; ++j)
      points.push_back(trc_sample(segment, float(j) / count));
  }
  points.push_back(keys.back().first);
  bool closed = road_distance(points.front(), points.back()) < 1;
  auto nearest = [&]() {
    size_t index = 1;
    float best = INFINITY, t_best = 0;
    for (size_t i = 1; i < points.size(); ++i) {
      auto a = points[i - 1], b = points[i];
      float dx = b[0] - a[0], dz = b[2] - a[2], square = dx * dx + dz * dz;
      if (square < 1e-8)
        continue;
      float t = std::clamp(
          ((start[0] - a[0]) * dx + (start[2] - a[2]) * dz) / square, 0.f, 1.f);
      float distance =
          std::hypot(start[0] - a[0] - t * dx, start[2] - a[2] - t * dz);
      if (distance < best) {
        best = distance;
        index = i;
        t_best = t;
      }
    }
    return std::pair{index, t_best};
  };
  auto [index, t] = nearest();
  RoadPoint heading{};
  PartMatrix frame;
  if (scene_instances_script_matrix(car, frame.data()))
    heading = {-frame[8], -frame[9], -frame[10]};
  auto delta = RoadPoint{points[index][0] - points[index - 1][0],
                         points[index][1] - points[index - 1][1],
                         points[index][2] - points[index - 1][2]};
  if (heading[0] * delta[0] + heading[2] * delta[2] < 0) {
    std::reverse(points.begin(), points.end());
    auto reversed = nearest();
    index = reversed.first;
    t = reversed.second;
  }
  RoadPoint projected;
  for (int axis = 0; axis < 3; ++axis)
    projected[axis] = points[index - 1][axis] +
                      t * (points[index][axis] - points[index - 1][axis]);
  r.points.push_back(start);
  r.points.push_back(projected);
  r.points.insert(r.points.end(), points.begin() + index, points.end());
  if (closed) {
    r.points.insert(r.points.end(), points.begin() + 1, points.begin() + index);
    r.points.push_back(projected);
  }
  for (size_t i = 1; i < r.points.size(); ++i)
    r.length += road_distance(r.points[i - 1], r.points[i]);
  return r;
}
uint32_t random_state = 0x2545f491;
float random_unit() {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return float(random_state & 0xffffff) / 16777216.f;
}
// The planar distance from a point to one traffic curve, and the arc length
// of its nearest point.
float edge_distance(const RoadRoutes::Edge &e, const RoadPoint &p,
                    const RoadPoint &heading, float &at) {
  float best = INFINITY;
  for (size_t i = 1; i < e.points.size(); ++i) {
    const auto &a = e.points[i - 1], &q = e.points[i];
    const float dx = q[0] - a[0], dz = q[2] - a[2], square = dx * dx + dz * dz;
    if (square < 1e-8f)
      continue;
    const float t =
        std::clamp(((p[0] - a[0]) * dx + (p[2] - a[2]) * dz) / square, 0.f, 1.f);
    // A curve heading away from the car is not the one it drives on.
    const float l = std::sqrt(square);
    if ((dx * heading[0] + dz * heading[2]) / l < .3f)
      continue;
    const float d = std::hypot(p[0] - a[0] - t * dx, p[2] - a[2] - t * dz);
    if (d < best) {
      best = d;
      at = e.distances[i - 1] + t * (e.distances[i] - e.distances[i - 1]);
    }
  }
  return best;
}
// The nearest point of a traffic curve and its arc length.
RoadPoint edge_point(const RoadRoutes::Edge &e, const RoadPoint &p, float &at) {
  float best = INFINITY;
  RoadPoint result = e.points.front();
  at = 0;
  for (size_t i = 1; i < e.points.size(); ++i) {
    const auto &a = e.points[i - 1], &q = e.points[i];
    const float dx = q[0] - a[0], dz = q[2] - a[2], square = dx * dx + dz * dz;
    if (square < 1e-8f)
      continue;
    const float t =
        std::clamp(((p[0] - a[0]) * dx + (p[2] - a[2]) * dz) / square, 0.f, 1.f);
    const float d = std::hypot(p[0] - a[0] - t * dx, p[2] - a[2] - t * dz);
    if (d < best) {
      best = d;
      for (int axis = 0; axis < 3; ++axis)
        result[axis] = a[axis] + t * (q[axis] - a[axis]);
      at = e.distances[i - 1] + t * (e.distances[i] - e.distances[i - 1]);
    }
  }
  return result;
}
RoadPoint edge_at(const RoadRoutes::Edge &e, float at, RoadPoint *direction) {
  RoadRoute line;
  line.points = e.points;
  const auto p = road_at(line, std::clamp(at, 0.f, e.length));
  if (direction) {
    const auto q = road_at(line, std::clamp(at + .5f, 0.f, e.length)),
               o = road_at(line, std::clamp(at - .5f, 0.f, e.length));
    *direction = road_normalize({q[0] - o[0], 0, q[2] - o[2]});
  }
  return p;
}
// Upstream bot.ts chooses a destination branch, then one of its lanes, so a
// wide street is not likelier merely for having more lanes. Closed (race)
// roads are avoided while another exit is open.
size_t choose_exit(const RoadRoutes &graph, void *ground,
                   const std::vector<size_t> &next) {
  std::vector<size_t> open;
  for (auto i : next)
    if (race_traffic_edge_open(ground, i))
      open.push_back(i);
  if (open.empty())
    open = next;
  std::vector<uint32_t> branches;
  for (auto i : open) {
    const auto branch = graph.traffic_edges()[i].branch;
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
      branches.push_back(branch);
  }
  const auto branch = branches[std::min<size_t>(
      size_t(random_unit() * branches.size()), branches.size() - 1)];
  std::vector<size_t> lanes;
  for (auto i : open)
    if (graph.traffic_edges()[i].branch == branch)
      lanes.push_back(i);
  return lanes[std::min<size_t>(size_t(random_unit() * lanes.size()),
                                lanes.size() - 1)];
}
void traffic_route(Brain &b, const RoadPoint &position) {
  void *ground = car_ground(*b.resources, b.car);
  auto graph = race_ground_roads(*b.resources, ground);
  PartMatrix frame;
  RoadPoint heading{};
  if (scene_instances_script_matrix(b.car, frame.data()))
    heading = road_normalize({-frame[8], 0, -frame[10]});
  if (!graph)
    return;
  const auto &edges = graph->traffic_edges();
  std::erase_if(b.plan, [&](size_t e) { return e >= edges.size(); });
  // Stay on the committed curves while the car is still on one of them.
  size_t keep = SIZE_MAX;
  float at = 0;
  // During a lane change the car is still beside its new lane.
  const bool changing = !b.plan.empty() && b.plan.front() == b.lane_target &&
                        b.lane_target < edges.size();
  if (changing) {
    float along = 0;
    if (edge_distance(edges[b.lane_target], position, heading, along) < 6 &&
        along < b.lane_start + b.lane_length) {
      keep = 0;
      at = along;
    }
  }
  if (keep == SIZE_MAX)
    b.lane_source = b.lane_target = SIZE_MAX;
  for (size_t i = 0; keep == SIZE_MAX && i < b.plan.size() && i < 3; ++i) {
    float along = 0;
    if (edge_distance(edges[b.plan[i]], position, heading, along) < 3) {
      keep = i;
      at = along;
      break;
    }
  }
  if (keep == SIZE_MAX) {
    auto projection = graph->traffic_project(position, &heading, true);
    if (!projection)
      return;
    b.plan = {projection->edge};
    at = projection->along;
  } else {
    b.plan.erase(b.plan.begin(), b.plan.begin() + long(keep));
  }
  // Several connected curves ahead, so the driver never aims at a street
  // centre or changes heading discontinuously at a crossing boundary.
  float ahead = edges[b.plan.front()].length - at;
  for (size_t i = 1; i < b.plan.size(); ++i)
    ahead += edges[b.plan[i]].length;
  while (b.plan.size() < 12 && ahead < 160) {
    const auto &e = edges[b.plan.back()];
    if (e.next.empty())
      break;
    const size_t next = choose_exit(*graph, ground, e.next);
    b.plan.push_back(next);
    ahead += edges[next].length;
  }
  b.route = {};
  b.route.points.push_back(position);
  for (size_t i = 0; i < b.plan.size(); ++i) {
    const auto &e = edges[b.plan[i]];
    for (size_t k = 0; k < e.points.size(); ++k)
      if (i || e.distances[k] > at + .05f) {
        auto point = e.points[k];
        // Blend from the old lane with a smoothstep over the change length.
        if (!i && b.lane_target == b.plan[0] && b.lane_source < edges.size()) {
          const float t = std::clamp((e.distances[k] - b.lane_start) /
                                         std::max(1.f, b.lane_length), 0.f, 1.f);
          const float weight = t * t * (3 - 2 * t);
          float ignored = 0;
          const auto source = edge_point(edges[b.lane_source], point, ignored);
          for (int axis = 0; axis < 3; ++axis)
            point[axis] = source[axis] + (point[axis] - source[axis]) * weight;
        }
        b.route.points.push_back(point);
      }
  }
  for (size_t i = 1; i < b.route.points.size(); ++i)
    b.route.length += road_distance(b.route.points[i - 1], b.route.points[i]);
  b.progress = 0;
}
// SLRR_rev road-traffic.ts tryChangeLane: only an adjacent, parallel lane of
// the same road branch, 2-5 m to the side, that stays parallel for the change
// length and is clear of cars beside, ahead and behind.
bool try_change_lane(Brain &b, const RoadPoint &position, float speed,
                     float length) {
  if (b.plan.empty() || b.lane_target != SIZE_MAX)
    return false;
  auto graph = race_ground_roads(*b.resources, car_ground(*b.resources, b.car));
  if (!graph)
    return false;
  const auto &edges = graph->traffic_edges();
  const size_t current = b.plan.front();
  if (current >= edges.size() || edges[current].crossing)
    return false;
  const auto &lane = edges[current];
  float at = 0;
  edge_point(lane, position, at);
  if (at < 15 || lane.length - at < length + 20)
    return false;
  RoadPoint direction{};
  const auto here = edge_at(lane, at, &direction);
  RoadPoint source_direction{};
  const auto source_ahead = edge_at(lane, at + length, &source_direction);
  float own_length = 4.6f, own_width = 1.9f;
  for (const auto &peer : peers)
    if (peer.car == b.car) {
      own_length = 2 * peer.half_length;
      own_width = 2 * peer.half_width;
    }
  struct Choice {
    size_t edge;
    float at, gap;
  };
  std::vector<Choice> sides[2];
  for (size_t i = 0; i < edges.size(); ++i) {
    const auto &other = edges[i];
    if (i == current || other.crossing || other.branch != lane.branch ||
        other.next.empty())
      continue;
    float other_at = 0;
    const auto point = edge_point(other, here, other_at);
    RoadPoint other_direction{};
    edge_at(other, other_at, &other_direction);
    const float dx = point[0] - here[0], dz = point[2] - here[2];
    const float lateral = dx * direction[2] - dz * direction[0];
    const float gap = std::fabs(lateral);
    if (gap < 2 || gap > 5 || std::fabs(point[1] - here[1]) > 1 ||
        std::fabs(dx * direction[0] + dz * direction[2]) > 2 ||
        other_direction[0] * direction[0] + other_direction[2] * direction[2] < .97f ||
        other_at < 15 || other.length - other_at < length + 20)
      continue;
    // Merging, diverging or crossing curves can share a local tangent.
    RoadPoint ahead_direction{};
    const auto ahead = edge_at(other, other_at + length, &ahead_direction);
    if (road_distance(ahead, source_ahead) > 6 ||
        ahead_direction[0] * source_direction[0] +
                ahead_direction[2] * source_direction[2] < .97f)
      continue;
    bool clear = true;
    for (const auto &peer : peers) {
      if (peer.car == b.car)
        continue;
      const float px = peer.position[0] - point[0], pz = peer.position[2] - point[2];
      const float along = px * other_direction[0] + pz * other_direction[2];
      const float side = std::fabs(px * other_direction[2] - pz * other_direction[0]);
      const float rear_speed = std::max(0.f, peer.velocity[0] * other_direction[0] +
                                                 peer.velocity[2] * other_direction[2]);
      const float size = std::max(peer.half_length, peer.half_width) * 2;
      const float lengths = (2 * peer.half_length + own_length) * .5f;
      if (!(std::fabs(peer.position[1] - point[1]) > 2.5f ||
            side > (size + own_width) * .5f + .5f ||
            along > std::max(12.f, speed * 2) + lengths ||
            along < -std::max(12.f, rear_speed * 2) - lengths)) {
        clear = false;
        break;
      }
    }
    if (clear)
      sides[lateral > 0].push_back({i, other_at, gap});
  }
  std::vector<Choice> choices;
  for (auto &side : sides)
    if (!side.empty())
      choices.push_back(*std::min_element(side.begin(), side.end(),
          [](const Choice &x, const Choice &y) { return x.gap < y.gap; }));
  if (choices.empty())
    return false;
  const auto &chosen = choices[std::min<size_t>(
      size_t(random_unit() * choices.size()), choices.size() - 1)];
  b.lane_source = current;
  b.lane_target = chosen.edge;
  b.lane_start = chosen.at;
  b.lane_length = length;
  b.plan = {chosen.edge};
  traffic_route(b, position);
  return true;
}
// A pursuer drives straight at its target when the road network cannot take
// it there: the target (or the pursuer) is off the road, the road detour is
// far longer than the gap, or the two are close.
// Distance a scene ray at bumper-to-roof height travels from a point before
// it enters a mesh (a building, wall or car), or INFINITY. The ray starts
// 3 m ahead so the pursuer's own body is not in its way.
float clear_distance(const RoadPoint &from, const RoadPoint &direction) {
  const float origin[3] = {from[0] + direction[0] * 3, from[1] + 1,
                           from[2] + direction[2] * 3};
  const float ray[3] = {direction[0], 0, direction[2]};
  float hit[3];
  if (!scene_instances_pick(origin, ray, hit))
    return INFINITY;
  return 3 + std::hypot(hit[0] - origin[0], hit[2] - origin[2]);
}
// Straight at the target, or round the first obstacle in the way: the
// nearest heading within 75 degrees whose first 25 m are clear.
RoadRoute direct_route(const RoadPoint &position, const RoadPoint &finish) {
  RoadRoute direct;
  direct.points = {position, finish};
  direct.length = road_distance(position, finish);
  const auto heading = road_normalize({finish[0] - position[0], 0, finish[2] - position[2]});
  const float gap = std::hypot(finish[0] - position[0], finish[2] - position[2]);
  if (gap < 6 || clear_distance(position, heading) > gap - 3)
    return direct;
  for (float degrees : {25.f, -25.f, 50.f, -50.f, 75.f, -75.f}) {
    const float a = degrees * 3.1415927f / 180, c = std::cos(a), s = std::sin(a);
    const RoadPoint turn{heading[0] * c - heading[2] * s, 0,
                         heading[0] * s + heading[2] * c};
    const float reach = std::min(25.f, gap);
    if (clear_distance(position, turn) < reach + 2)
      continue;
    const RoadPoint detour{position[0] + turn[0] * reach, position[1],
                           position[2] + turn[2] * reach};
    RoadRoute around;
    around.points = {position, detour, finish};
    around.length = reach + road_distance(detour, finish);
    return around;
  }
  return direct;
}
RoadRoute pursuit_route(const RpkIndex &rpk, Object *car,
                        const RoadPoint &position, const RoadPoint &finish) {
  const RoadRoute direct = direct_route(position, finish);
  auto graph = race_ground_roads(rpk, car_ground(rpk, car));
  if (!graph || road_distance(position, finish) < 40)
    return direct;
  const auto to = graph->project(finish), from = graph->project(position);
  const bool off_road = !to || !from || to->distance > 12 || from->distance > 12;
  if (off_road && road_distance(position, finish) < 250)
    return direct;
  auto route = graph->query(position, finish);
  if (!route || route->points.size() < 2 ||
      route->length > 3 * road_distance(position, finish) + 60)
    return direct;
  // A target away from the road is reached across country from the road
  // point nearest to it.
  if (road_distance(route->points.back(), finish) > 3) {
    route->length += road_distance(route->points.back(), finish);
    route->points.push_back(finish);
  }
  return *route;
}
} // namespace
void race_brain_forget(void *key) {
  brains.erase(key);
  std::erase_if(brains,
                [&](const auto &entry) { return entry.second.car == key; });
}
void race_brains_reset() {
  brains.clear();
  peers.clear();
}
void race_set_peers(std::vector<RacePeer> list) { peers = std::move(list); }
void race_note_impact(Object *car) {
  auto *n = gameref_get(car);
  if (!n)
    return;
  for (auto &entry : brains)
    if (entry.second.car == n->key && entry.second.car_incarnation == n->incarnation &&
        entry.second.mode == "AI_GoToTraffic") {
      entry.second.impact = true;
      entry.second.settled_for = 0;
      entry.second.ram_for = 0;
    }
}
void *race_car_ground(const RpkIndex &rpk, Object *car) {
  return car_ground(rpk, car);
}
bool race_brain_drives(Object *car) {
  auto *n = gameref_get(car);
  if (!n)
    return false;
  for (const auto &entry : brains)
    if (entry.second.car == n->key && entry.second.car_incarnation == n->incarnation &&
        !entry.second.mode.empty() && entry.second.mode != "AI_stop" &&
        entry.second.mode != "AI_suspend")
      return true;
  return false;
}
bool race_brain_command(const RpkIndex &rpk, void *key,
                        const std::string &command) {
  std::istringstream input(command);
  std::string verb;
  input >> verb;
  if (verb != "controllable" && verb != "renderinstance" && verb != "leave" &&
      verb != "AI_level" && verb != "AI_NightRace" && verb != "AI_BeginRace" &&
      verb != "AI_stop" && verb != "AI_suspend" && verb != "AI_race" &&
      verb != "AI_spline" && verb != "AI_follow" && verb != "AI_GoToTraffic" &&
      verb != "AI_GoToTarget")
    return false;
  auto *n = gameref_get(key);
  if (!n)
    return false;
  auto &b = brains[n->key];
  if (b.incarnation != n->incarnation) {
    b = {};
    b.key = n->key;
    b.incarnation = n->incarnation;
  }
  b.resources = &rpk;
  if (verb == "controllable") {
    int32_t id = 0;
    if (input >> id) {
      b.car = live_car(id);
      auto *car = gameref_get(b.car);
      b.car_incarnation = car ? car->incarnation : 0;
      // A Bot that adopts a streamed traffic car (a woken police scout)
      // replaces the host's traffic driver of that car.
      if (b.car)
        std::erase_if(brains, [&](const auto &entry) {
          return entry.first != n->key && entry.second.car == b.car;
        });
    }
    return true;
  }
  if (verb == "renderinstance")
    return true;
  if (verb == "AI_level") {
    float level = 1;
    if (input >> level && std::isfinite(level))
      b.level = std::clamp(level, .1f, 2.f);
    return true;
  }
  if (verb == "AI_NightRace") {
    b.night = true;
    return true;
  }
  // v221 ab4b494 race-driver.ts: the radius-based finish event may precede
  // the finish plane. A night racer coasts across it before holding brakes.
  b.runout = false;
  if (verb == "AI_stop" && b.night && b.car && b.route.points.size() > 1) {
    auto position = car_position(b.car);
    auto finish = b.route.points.back();
    if (std::hypot(position[0] - finish[0], position[2] - finish[2]) < 40) {
      b.runout = true;
      b.finish = finish;
      auto previous = b.route.points[b.route.points.size() - 2];
      b.finish_direction =
          road_normalize({finish[0] - previous[0], 0, finish[2] - previous[2]});
    }
  }
  if (verb == "AI_race" || verb == "AI_spline")
    b.night = false;
  b.mode = verb;
  b.route = {};
  b.plan.clear();
  if (verb != "AI_GoToTraffic")
    b.enter_requested = false;
  b.progress = 0;
  b.replan = 0;
  b.blocked_for = b.reverse_for = 0;
  if (!b.car)
    return true;
  if (verb == "AI_race" || verb == "AI_GoToTarget") {
    RoadPoint finish;
    if (read_point(input, finish)) {
      auto route = race_query_route(rpk, car_ground(rpk, b.car),
                                    car_position(b.car), finish);
      if (route)
        b.route = *route;
    }
  } else if (verb == "AI_spline") {
    float width = 0;
    std::string path;
    if (input >> width >> path && std::isfinite(width) && width > 0)
      b.route = spline(rpk, path, car_position(b.car), b.car);
  } else if (verb == "AI_follow") {
    if (read_point(input, b.offset))
      input >> b.target;
  } else if (verb == "AI_GoToTraffic")
    traffic_route(b, car_position(b.car));
  if (!b.route.points.empty() || verb == "AI_follow")
    part_command(b.car, "start");
  return true;
}
void race_set_traffic_mode(const RpkIndex &rpk, Object *car) {
  for (auto &entry : brains)
    if (entry.second.car == car) {
      race_brain_command(rpk, entry.first, "AI_GoToTraffic");
      return;
    }
  auto id = gameref_make_live(car);
  race_brain_command(rpk, car, "controllable " + std::to_string(id));
  race_brain_command(rpk, car, "AI_GoToTraffic");
}
bool race_drive_input(Object *car, const RoadPoint &position, float yaw,
                      float speed, float dt, RaceDriveInput &out) {
  Brain *b = nullptr;
  auto *n = gameref_get(car);
  if (!n)
    return false;
  for (auto &entry : brains)
    if (entry.second.car == car &&
        entry.second.car_incarnation == n->incarnation) {
      b = &entry.second;
      break;
    }
  if (!b)
    return false;
  out = {};
  if (b->runout) {
    float along = (position[0] - b->finish[0]) * b->finish_direction[0] +
                  (position[2] - b->finish[2]) * b->finish_direction[2];
    if (speed > 1 && along < 10) {
      out.brake = 0;
      out.start = true;
      return true;
    }
    b->runout = false;
  }
  const bool traffic = b->mode == "AI_GoToTraffic",
             follow = b->mode == "AI_follow";
  const bool driving = traffic || follow || b->mode == "AI_race" ||
                       b->mode == "AI_GoToTarget" || b->mode == "AI_spline";
  // A red light, STOP line or race halt slows the car to its stopping point;
  // it is not a failed driver.
  float stop = 0;
  const bool halted =
      traffic &&
      race_traffic_halted(car_ground(*b->resources, car), position, car, &stop);
  if (halted)
    b->blocked_for = 0;
  // After a collision a traffic driver brakes until the car has settled for a
  // second, then rejoins its lane from where it ended up (bot.ts impact).
  if (traffic && b->impact) {
    b->settled_for = std::fabs(speed) < .6f ? b->settled_for + dt : 0;
    if (b->settled_for < 1) {
      out.brake = .8f;
      out.throttle = 0;
      out.steer = b->steering;
      out.start = true;
      b->brake = out.brake;
      b->throttle = 0;
      b->blocked_for = b->reverse_for = 0;
      return true;
    }
    b->impact = false;
    b->settled_for = 0;
    b->plan.clear();
    b->lane_source = b->lane_target = SIZE_MAX;
    b->holding = false;
    traffic_route(*b, position);
    b->replan = 2;
  }
  if (driving && !halted && b->reverse_for > 0) {
    b->reverse_for = std::max(0.f, b->reverse_for - dt);
    PartMatrix frame;
    RoadPoint forward{};
    if (scene_instances_script_matrix(car, frame.data()))
      forward = {-frame[8], 0, -frame[10]};
    bool clear = true;
    for (const auto &peer : part_vehicle_positions())
      if (peer.chassis != car) {
        float dx = peer.position[0] - position[0],
              dz = peer.position[2] - position[2];
        float along = dx * forward[0] + dz * forward[2],
              side = std::fabs(dx * forward[2] - dz * forward[0]);
        if (along < 0 && along > -10 && side < 2.5 &&
            std::fabs(peer.position[1] - position[1]) < 3)
          clear = false;
      }
    if (clear) {
      out.reverse = true;
      out.throttle = .65f;
      out.brake = speed < -3.3f ? .3f : 0;
      out.steer = b->reverse_steer;
      out.start = true;
    }
    if (b->reverse_for == 0) {
      if (traffic)
        traffic_route(*b, position);
      b->replan = traffic ? 2.f : 0.f;
    }
    return true;
  }
  if (traffic) {
    b->replan -= dt;
    if (b->replan <= 0) {
      traffic_route(*b, position);
      b->replan = 2;
    }
  }
  Object *followed = nullptr;
  float target_speed = 0;
  if (follow) {
    followed = live_car(b->target);
    if (auto *t = gameref_get(followed))
      target_speed = std::hypot(t->vel[0], t->vel[2]);
    b->replan -= dt;
    if (b->replan <= 0) {
      b->replan = .5f;
      PartMatrix frame;
      if (followed && scene_instances_script_matrix(followed, frame.data())) {
        // The offset is in the followed car's frame: +Z is behind it. A
        // stopped car is approached to within 5 m to pull it over.
        auto offset = b->offset;
        const float length = road_distance(offset, {});
        if (target_speed < 3 && length > 5)
          for (auto &v : offset)
            v *= 5 / length;
        RoadPoint finish;
        for (int k = 0; k < 3; ++k)
          finish[k] = frame[12 + k] + offset[0] * frame[k] +
                      offset[1] * frame[4 + k] + offset[2] * frame[8 + k];
        // The pursuer's own query must not replace the route Java last
        // queried (the navigator plots it).
        b->route = pursuit_route(*b->resources, car, position, finish);
        b->progress = 0;
      }
    }
  }
  // Extend the plan well before its end: a driver must never reach the end
  // of its lookahead and coast to a stop in the middle of the road.
  if (traffic && (b->route.points.size() < 2 ||
                  road_distance(position, b->route.points.back()) < 40)) {
    traffic_route(*b, position);
    b->replan = 2;
  }
  // A Bot sent back to traffic (AI_GoToTraffic on its own brain, not the
  // host's traffic driver) joins once it drives along a lane: the host
  // registers the car and tells its listeners ai_info_entertraffic.
  if (traffic && b->key != car && !b->enter_requested &&
      !race_traffic_pointer(car)) {
    void *ground = car_ground(*b->resources, car);
    auto graph = race_ground_roads(*b->resources, ground);
    const RoadPoint heading{-std::sin(yaw), 0, -std::cos(yaw)};
    auto lane = graph ? graph->traffic_project(position, &heading) : std::nullopt;
    if (lane && lane->distance < 2.5f &&
        lane->direction[0] * heading[0] + lane->direction[2] * heading[2] > .9f) {
      b->enter_requested = true;
      race_traffic_request_enter(car, ground);
    }
  }
  if (b->route.points.size() < 2)
    return true;
  const auto &points = b->route.points;
  float along = 0, best = std::numeric_limits<float>::infinity(),
        projected = b->progress;
  for (size_t i = 1; i < points.size(); ++i) {
    auto a = points[i - 1], q = points[i];
    float length = road_distance(a, q);
    if (length < 1e-6f)
      continue;
    if (along + length < b->progress - 2) {
      along += length;
      continue;
    }
    if (along > b->progress + 50)
      break;
    float t = 0;
    for (int k = 0; k < 3; ++k)
      t += (position[k] - a[k]) * (q[k] - a[k]);
    t = std::clamp(t / (length * length), 0.f, 1.f);
    RoadPoint near;
    for (int k = 0; k < 3; ++k)
      near[k] = a[k] + t * (q[k] - a[k]);
    float d = road_distance(position, near);
    if (d < best) {
      best = d;
      projected = along + t * length;
    }
    along += length;
  }
  b->progress = std::max(b->progress, projected);
  auto finish = points.back();
  // A follower keeps chasing a moving target past the end of its last route.
  if (!b->night && !follow && !traffic &&
      b->progress >= std::max(0.f, b->route.length - 12.f) &&
      std::hypot(position[0] - finish[0], position[2] - finish[2]) < 3) {
    b->route = {};
    return true;
  }
  auto target =
      road_at(b->route,
              b->progress + std::clamp(std::fabs(speed) * .8f + 4, 4.f, 18.f));
  if (b->night && b->progress > b->route.length - 30) {
    auto previous = points[points.size() - 2];
    auto direction =
        road_normalize({finish[0] - previous[0], 0, finish[2] - previous[2]});
    float along = (position[0] - finish[0]) * direction[0] +
                  (position[2] - finish[2]) * direction[2];
    if (along >= 0)
      for (int axis = 0; axis < 3; ++axis)
        target[axis] = finish[axis] + 100 * direction[axis];
  }
  RoadPoint error;
  for (int k = 0; k < 3; ++k)
    error[k] = target[k] - position[k];
  float target_yaw = std::atan2(-error[0], -error[2]),
        angle =
            std::atan2(std::sin(target_yaw - yaw), std::cos(target_yaw - yaw));
  float curvature =
      2 * std::sin(angle) / std::max(2.f, road_distance(error, {}));
  float request = std::clamp(std::atan(2.7f * curvature) / .6f, -1.f, 1.f);
  // sin(pi) is zero: pure pursuit alone accelerates straight away from a
  // target behind the car. Recover at walking speed with a real steering lock.
  // A follower at its target point waits there instead of circling it.
  const bool arrived =
      follow && std::hypot(position[0] - finish[0], position[2] - finish[2]) < 2.5f;
  const bool behind = !arrived && std::fabs(angle) > 1.5707963f;
  if (behind)
    request = std::copysign(1.f, angle);
  if (arrived)
    request = 0;
  b->steering += std::clamp(request - b->steering, -dt * 3, dt * 3);
  const bool spline_race = b->mode == "AI_spline";
  const float curvature_floor = (b->night || spline_race) ? 1.e-9f : .005f;
  const float remaining = b->route.length - b->progress;
  // A pursuer (AI_level 0..0.75 for police) outruns the speed limit and
  // arrives at its target's speed.
  float desired = std::min(
      {b->night ? INFINITY
       : follow ? 28 + 12 * std::clamp(b->level, 0.f, 1.f)
                : (spline_race ? 55.f : 32.f) * std::clamp(b->level, .5f, 1.5f),
       std::sqrt(4.5f / std::max(curvature_floor, std::fabs(curvature))),
       b->night ? INFINITY
       : follow ? target_speed + std::sqrt(8 * std::max(0.f, remaining))
                : std::sqrt(8 * std::max(road_distance(position, finish),
                                         remaining))});
  // Brake before a bend, rather than waiting for it to enter the short
  // steering lookahead. Distances and acceleration are host tuning in SI units.
  for (float ahead : {8.f, 16.f, 32.f, 48.f, 80.f, 128.f, 192.f}) {
    auto a = road_at(b->route, b->progress + ahead - 4),
         c = road_at(b->route, b->progress + ahead),
         d = road_at(b->route, b->progress + ahead + 4);
    float ux = c[0] - a[0], uz = c[2] - a[2], vx = d[0] - c[0],
          vz = d[2] - c[2];
    float l1 = std::hypot(ux, uz), l2 = std::hypot(vx, vz),
          chord = std::hypot(d[0] - a[0], d[2] - a[2]);
    if (l1 > .1f && l2 > .1f && chord > .1f) {
      float k = 2 * std::fabs(ux * vz - uz * vx) / (l1 * l2 * chord);
      float corner = 4.5f / std::max(curvature_floor, k);
      desired =
          std::min(desired, std::sqrt(corner + 8 * std::max(0.f, ahead - 8)));
    }
  }
  if (behind)
    desired = std::min(desired, 3.f);
  if (arrived)
    desired = std::min(desired, target_speed);
  // Cruise from the road's speed band, redrawn after each junction.
  int speed_group = -1;
  if (traffic) {
    auto graph = race_ground_roads(*b->resources, car_ground(*b->resources, car));
    const size_t edge = b->plan.empty() ? SIZE_MAX : b->plan.front();
    if (graph && edge < graph->traffic_edges().size()) {
      const auto &e = graph->traffic_edges()[edge];
      speed_group = e.speed_group;
      if (!e.crossing && edge != b->cruise_edge && edge != b->lane_target) {
        const auto band = road_speed_band(e.speed_group);
        b->cruise = (band[0] + random_unit() * (band[1] - band[0])) / 3.6f;
        b->cruise_command = std::max(0.f, speed);
        b->cruise_edge = edge;
      }
    }
    if (b->cruise_command < 0) {
      b->cruise = 8;
      b->cruise_command = std::max(0.f, speed);
    }
    const float delta = b->cruise - b->cruise_command;
    b->cruise_command += std::copysign(
        std::min(std::fabs(delta), (delta > 0 ? .8f : 1.2f) * dt), delta);
    desired = std::min(desired, std::max(b->cruise_command, 2.f));
  }
  const float free_desired = desired;
  // Ease down to the stop line at about 2.5 m/s2 (v221 bot.ts).
  if (halted)
    desired = std::min(desired, stop < .25f ? 0.f : std::sqrt(5 * stop));
  // A car turning round at walking pace is not heading into the corridor
  // ahead of its nose.
  if ((traffic || follow) && !behind) {
    b->ignore_peers_for = std::max(0.f, b->ignore_peers_for - dt);
    const RoadPoint forward{-std::sin(yaw), 0, -std::cos(yaw)};
    const float limited =
        b->ignore_peers_for > 0
            ? desired
            : following_speed(car, followed, position, forward, speed, desired,
                              &b->route, b->progress);
    // Two cars nosed into each other in a junction would wait forever:
    // after a standstill behind another car, edge forwards anyway. A pursuer
    // waits less.
    b->queued_for = limited < desired - .5f && std::fabs(speed) < .3f && !halted
                        ? b->queued_for + dt
                        : 0;
    if (b->queued_for > (follow ? 5.f : 10.f)) {
      b->queued_for = 0;
      b->ignore_peers_for = 3;
    }
    // Rural roads and a slow leader make a lane change likelier (bot.ts).
    if (traffic) {
      b->lane_check += dt;
      const bool rural = speed_group == 0;
      const float period = rural ? 10.f : 20.f;
      const float chance =
          std::max(rural ? .25f : .1f, limited < desired - 1e-3f ? .2f : 0.f);
      if (b->lane_check >= period) {
        b->lane_check = 0;
        if (!halted && speed > .5f && random_unit() < chance)
          try_change_lane(*b, position, speed, std::max(25.f, speed * 3.5f));
      }
    }
    desired = limited;
  }
  if (traffic) {
    // A queued car holds its brake, then reacts .5-1.5 s after the way ahead
    // clears instead of creeping on every gap (bot.ts startDelay).
    if (desired > .8f && b->holding) {
      if (b->start_delay < 0)
        b->start_delay = .5f + random_unit();
      b->start_delay -= dt;
      if (b->start_delay <= 0) {
        b->holding = false;
        b->start_delay = -1;
      }
    } else if (desired <= .8f) {
      b->start_delay = -1;
      if (desired < .5f && std::fabs(speed) < .3f)
        b->holding = true;
    }
    if (b->holding)
      desired = 0;
    // Stuck below 3 km/h for a minute without a signal or halt: push on at
    // 30 km/h for ten seconds, ignoring the cars ahead (bot.ts ram).
    b->idle_for = !halted && std::fabs(speed) < 3 / 3.6f ? b->idle_for + dt : 0;
    if (b->idle_for > 60) {
      b->idle_for = 0;
      b->ram_for = 10;
    }
    if (halted)
      b->ram_for = 0;
    if (b->ram_for > 0) {
      b->ram_for -= dt;
      b->holding = false;
      desired = std::min(free_desired, 30 / 3.6f);
    }
  }
  out.brake = std::clamp((speed - desired) * .35f, 0.f, 1.f);
  out.throttle =
      out.brake > .05f ? 0 : std::clamp((desired - speed) * .5f, 0.f, 1.f);
  // Hold a stopped car rather than creeping between tiny commands. Native
  // traffic holds above half brake.
  if (desired < .3f && std::fabs(speed) < .5f) {
    out.brake = traffic ? .6f : 1.f;
    out.throttle = 0;
  }
  // Traffic drivers press and release smoothly instead of snapping between
  // full brake and throttle, which pitched the soft-sprung bodies up and down
  // like hydraulics (#324). Braking still builds within a quarter second;
  // an emergency stop or a car already at rest brakes at once.
  if (traffic) {
    const bool urgent = out.brake >= .9f || std::fabs(speed) < .5f;
    b->brake = urgent ? out.brake
               : out.brake > b->brake ? std::min(out.brake, b->brake + 4 * dt)
                                      : std::max(out.brake, b->brake - 2 * dt);
    b->throttle = out.throttle > b->throttle
                      ? std::min(out.throttle, b->throttle + 1.5f * dt)
                      : std::max(out.throttle, b->throttle - 3 * dt);
    out.brake = b->brake;
    out.throttle = b->brake > .05f ? 0.f : b->throttle;
  }
  out.steer = b->steering;
  out.start = true;
  if (driving) {
    // Stuck against a car or wall with the throttle open: back off and retry.
    b->blocked_for =
        out.throttle > .45f && std::fabs(speed) < .5f ? b->blocked_for + dt : 0;
    if (b->blocked_for > (traffic ? 2.f : 3.f)) {
      b->blocked_for = 0;
      b->reverse_for = traffic ? 3.5f : 2.5f;
      b->reverse_steer =
          std::fabs(angle) > .1f ? -std::copysign(1.f, angle) : 1.f;
    }
  }
  return true;
}
} // namespace slrr
