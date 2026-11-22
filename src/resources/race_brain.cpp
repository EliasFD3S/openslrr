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
float following_speed(Object *car, Object *ignore, const RoadPoint &position,
                      const RoadPoint &forward, float speed, float desired) {
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
    const float along = dx * forward[0] + dz * forward[2];
    if (along <= 0 || along > 60 ||
        std::fabs(peer.position[1] - position[1]) > 2.5f)
      continue;
    const float alignment = std::min(
        1.f, std::fabs(peer.forward[0] * forward[0] + peer.forward[2] * forward[2]));
    const float across = std::sqrt(std::max(0.f, 1 - alignment * alignment));
    const float length =
        2 * (peer.half_length * alignment + peer.half_width * across);
    const float width =
        2 * (peer.half_width * alignment + peer.half_length * across);
    if (std::fabs(dx * forward[2] - dz * forward[0]) >
        (own_width + width) * .5f + .3f)
      continue;
    const float gap = along - (own_length + length) * .5f;
    const float lead = std::max(0.f, peer.velocity[0] * forward[0] +
                                         peer.velocity[2] * forward[2]);
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
void traffic_route(Brain &b, const RoadPoint &position) {
  auto graph =
      race_ground_roads(*b.resources, car_ground(*b.resources, b.car));
  PartMatrix frame;
  RoadPoint heading{};
  if (scene_instances_script_matrix(b.car, frame.data()))
    heading = {-frame[8], -frame[9], -frame[10]};
  auto projection =
      graph ? graph->traffic_project(position, &heading, true) : std::nullopt;
  if (!projection)
    return;
  b.route = {};
  b.route.points.push_back(position);
  size_t edge = projection->edge;
  float at = projection->along;
  // Keep multiple connected curves ahead so the driver never aims at a street
  // centre or changes heading discontinuously at a crossing boundary.
  for (int hop = 0; hop < 6; ++hop) {
    const auto &e = graph->traffic_edges()[edge];
    for (size_t i = 0; i < e.points.size(); ++i)
      if (e.distances[i] > at + .05f)
        b.route.points.push_back(e.points[i]);
    if (e.next.empty())
      break;
    edge = e.next.front();
    at = -1;
  }
  for (size_t i = 1; i < b.route.points.size(); ++i)
    b.route.length += road_distance(b.route.points[i - 1], b.route.points[i]);
  b.progress = 0;
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
        auto r = race_query_route(*b->resources,
                                  car_ground(*b->resources, car), position,
                                  finish);
        b->route = r ? *r : RoadRoute{};
        b->progress = 0;
      }
    }
  }
  if (traffic && (b->route.points.size() < 2 ||
                  road_distance(position, b->route.points.back()) < 8))
    traffic_route(*b, position);
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
  if (!b->night && !follow &&
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
  if (traffic)
    desired = std::min(desired, 8.f);
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
            : following_speed(car, followed, position, forward, speed, desired);
    // Two cars nosed into each other in a junction would wait forever:
    // after a standstill behind another car, edge forwards anyway. A pursuer
    // waits less.
    b->queued_for = limited < desired - .5f && std::fabs(speed) < .3f && !halted
                        ? b->queued_for + dt
                        : 0;
    if (b->queued_for > (follow ? 5.f : 20.f)) {
      b->queued_for = 0;
      b->ignore_peers_for = 3;
    }
    desired = limited;
  }
  out.brake = std::clamp((speed - desired) * .35f, 0.f, 1.f);
  out.throttle =
      out.brake > .05f ? 0 : std::clamp((desired - speed) * .5f, 0.f, 1.f);
  // Hold a stopped car rather than creeping between tiny commands.
  if (desired < .3f && std::fabs(speed) < .5f) {
    out.brake = 1;
    out.throttle = 0;
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
