#include "race_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/job_system.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/road_routes.hpp"
#include <chrono>
#include <cmath>
using namespace slrr;
RoadSegment straight(RoadPoint a, RoadPoint b) {
  RoadSegment s;
  s.start = a;
  s.end = b;
  for (int i = 0; i < 3; ++i)
    s.start_tangent[i] = s.end_tangent[i] = b[i] - a[i];
  s.length = road_distance(a, b);
  return s;
}
RoadNetwork street(bool reverse) {
  RoadNetwork n;
  RoadNode a, b;
  a.id = 1;
  b.id = 2;
  b.position = {100, 0, 0};
  a.links = {{4, 0}};
  b.links = {{4, 1}};
  if (reverse) {
    a.links.push_back({5, 1});
    b.links.push_back({5, 0});
  }
  n.nodes = {a, b};
  RoadPath p;
  p.id = 3;
  p.refs[1] = 1;
  p.refs[2] = 2;
  p.segments = {straight(a.position, b.position)};
  n.paths = {p};
  RoadOther r;
  r.id = 4;
  r.refs[0] = 3;
  n.others = {r};
  if (reverse) {
    r.id = 5;
    n.others.push_back(r);
  }
  return n;
}
void directions() {
  RoadRoutes one(street(false)), two(street(true));
  auto r = one.query({20, 0, 0}, {80, 0, 0});
  SLRR_REQUIRE(r && std::fabs(r->length - 60) < .1f && r->direction[0] > .99f);
  SLRR_REQUIRE(!one.query({80, 0, 0}, {20, 0, 0}));
  r = two.query({80, 0, 0}, {20, 0, 0});
  SLRR_REQUIRE(r && std::fabs(r->length - 60) < .1f && r->direction[0] < -.99f);
  SLRR_REQUIRE(road_distance(road_at(*r, 30), {50, 0, 0}) < .1f);
  auto aligned = two.project({50, 3, 4});
  SLRR_REQUIRE(aligned && std::fabs(aligned->position[0] - 50) < .1f &&
               std::fabs(aligned->distance - 5) < .1f);
}
void cross_radius() {
  RoadNetwork n;
  RoadNode a, b;
  a.id = 1;
  a.position = {4, 0, 0};
  b.id = 2;
  b.position = {12, 0, 0};
  n.nodes = {a, b};
  RoadRoutes g(n);
  auto p = g.nearest_cross({}, 10);
  SLRR_REQUIRE(p && (*p)[0] == 12);
  p = g.nearest_cross({}, -10);
  SLRR_REQUIRE(p && (*p)[0] == 4);
  SLRR_REQUIRE(!g.query({}, {}));
}
void real_city() {
  Fixture f;
  f.packs.set_root(fs::absolute("game").string());
  f.packs.open_all();
  auto pack = f.packs.open_lib("maps/city.rpk");
  SLRR_REQUIRE(pack);
  Object ground;
  auto *n = gameref_upsert(&ground, GameRefKind::GroundRef);
  n->type_id = RpkIndex::make_full_id(pack, 1);
  auto graph = race_ground_roads(f.packs, &ground);
  SLRR_REQUIRE(graph && !graph->edges().empty());
  const auto &e = graph->edges().front();
  std::printf("City bound signal heads: %zu\n", graph->signals().size());
  SLRR_REQUIRE(graph->signals().size() > 400);
  auto route = graph->query(e.points.front(), e.points.back());
  SLRR_REQUIRE(route && route->length > 0 && std::isfinite(route->length));
  std::printf("City graph: %zu directed edges, first route %.1f metres\n",
              graph->edges().size(), route->length);
  gameref_remove(&ground);
  SLRR_REQUIRE(!race_last_route(&ground));
}
void route_native_contract() {
  RaceFixture f;
  register_renderref_natives(f.vm, f.packs, f.sections);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  const auto &e = graph->edges().front();
  auto vector = [&](RoadPoint p) {
    auto *v = f.vm.new_object("java.lang.Vector3");
    for (int k = 0; k < 3; ++k)
      v->fields[k == 0 ? "x" : k == 1 ? "y" : "z"] = Value::make_float(p[k]);
    return v;
  };
  auto *a = vector(e.points.front()), *b = vector(e.points.back());
  auto length = f.call("findRoute", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F",
                       {Value::make_obj(a), Value::make_obj(b)})
                    .v.f;
  SLRR_REQUIRE(length > 0 && f.call("getRouteLength", "()F").v.f == length);
  auto *aligned =
      f.call("alignToRoad", "(Ljava.lang.Vector3;)[Ljava.lang.Vector3;",
             {Value::make_obj(a)})
          .v.o;
  SLRR_REQUIRE(aligned && aligned->is_array && aligned->slots.size() == 2 &&
               aligned->slots[0].v.o != a &&
               aligned->slots[0].v.o != aligned->slots[1].v.o);
  auto *middle = f.call("getRoutePos", "(F)Ljava.lang.Vector3;",
                        {Value::make_float(length * .5f)})
                     .v.o;
  SLRR_REQUIRE(middle);
  auto at = f.call("getRouteDist", "(Ljava.lang.Vector3;)F",
                   {Value::make_obj(middle)})
                .v.f;
  SLRR_REQUIRE(std::fabs(at - length * .5f) < .1f);
  Object navigator, line;
  gameref_upsert(&navigator, GameRefKind::Dummy);
  auto *scale = vector({1, 1, 1});
  auto plotted =
      f.registry.invoke("java.util.resource.RenderRef", "plotRoute",
                        "(Ljava.util.resource.ResourceRef;Ljava.util.resource."
                        "ResourceRef;IFLjava.lang.Vector3;)I",
                        &line,
                        {Value::make_obj(&navigator), Value::make_obj(nullptr),
                         Value::make_int(0xff00ff00), Value::make_float(2),
                         Value::make_obj(scale)});
  SLRR_REQUIRE(plotted.v.i == 1 && gameref_get(&line)->parent == &navigator &&
               gameref_get(&line)->line_geometry &&
               !gameref_get(&line)->line_geometry->indices.empty());
  f.call("findRoute", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F",
         {Value::make_obj(nullptr), Value::make_obj(b)});
  SLRR_REQUIRE(f.call("getRouteLength", "()F").v.f == -1 && !race_plot_route());
  gameref_remove(&line);
  gameref_remove(&navigator);
  std::puts("PASS route native arrays, metre-based cache queries, navigator "
            "ribbon and failed-query invalidation");
}
void shared_signal_phases() {
  auto n = street(false);
  n.others[0].segments = {straight({90, 0, 0}, {0, 0, 0})};
  RoadNode side;
  side.id = 6;
  side.position = {100, 0, 100};
  side.links = {{8, 0}};
  n.nodes.push_back(side);
  n.nodes[1].links.push_back({8, 1});
  RoadPath p;
  p.id = 7;
  p.refs[1] = 6;
  p.refs[2] = 2;
  p.segments = {straight(side.position, n.nodes[1].position)};
  n.paths.push_back(p);
  RoadOther r;
  r.id = 8;
  r.refs[0] = 7;
  r.segments = {straight({100, 0, 10}, side.position)};
  n.others.push_back(r);
  RoadRoutes roads(n, {{90, 0, 0}, {100, 0, 10}});
  SLRR_REQUIRE(roads.signals().size() == 2);
  SLRR_REQUIRE(roads.signal(2, 3, 0) == 2 && roads.signal(2, 7, 0) == 0);
  SLRR_REQUIRE(roads.signal(2, 3, 10) == 1 && roads.signal(2, 7, 10) == 0);
  SLRR_REQUIRE(roads.signal(2, 3, 13) == 0 && roads.signal(2, 7, 13) == 2);
  SLRR_REQUIRE(roads.signal(2, 7, 23) == 1);
  SLRR_REQUIRE(roads.signal(2, 3, 26) == 2);
  SLRR_REQUIRE(roads.edge_signal(0, 13) == 0);
  std::puts(
      "PASS one shared crossing phase: green, yellow, opposing red and repeat");
}
void traffic_lane_topology() {
  RaceFixture f;
  f.packs.open_all();
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && !graph->traffic_edges().empty());
  size_t lanes = 0, turns = 0, bends = 0;
  for (const auto &e : graph->traffic_edges()) {
    if (e.crossing)
      ++turns;
    else
      ++lanes;
    for (size_t next : e.next) {
      SLRR_REQUIRE(next < graph->traffic_edges().size());
      const auto &out = graph->traffic_edges()[next];
      SLRR_REQUIRE(e.crossing != out.crossing);
      SLRR_REQUIRE(road_distance(e.points.back(), out.points.front()) < .031f);
    }
    if (e.crossing) {
      auto first = road_normalize({e.points[1][0] - e.points[0][0], 0,
                                   e.points[1][2] - e.points[0][2]});
      auto last = road_normalize(
          {e.points.back()[0] - e.points[e.points.size() - 2][0], 0,
           e.points.back()[2] - e.points[e.points.size() - 2][2]});
      if (first[0] * last[0] + first[2] * last[2] < .8f)
        ++bends;
    }
  }
  auto reference = [&](RoadPoint point, const RoadPoint *heading, bool turns) {
    std::optional<RoadProjection> result;
    float best = INFINITY;
    for (size_t edge = 0; edge < graph->traffic_edges().size(); ++edge) {
      const auto &e = graph->traffic_edges()[edge];
      if ((!turns && e.crossing) || e.next.empty())
        continue;
      for (size_t i = 1; i < e.points.size(); ++i) {
        RoadPoint d{e.points[i][0] - e.points[i - 1][0],
                    e.points[i][1] - e.points[i - 1][1],
                    e.points[i][2] - e.points[i - 1][2]};
        float l = d[0] * d[0] + d[2] * d[2];
        if (l < 1e-8f)
          continue;
        float t = std::clamp(((point[0] - e.points[i - 1][0]) * d[0] +
                              (point[2] - e.points[i - 1][2]) * d[2]) /
                                 l,
                             0.f, 1.f);
        RoadPoint p;
        for (int axis = 0; axis < 3; ++axis)
          p[axis] = e.points[i - 1][axis] + t * d[axis];
        auto direction = road_normalize(d);
        float distance = road_distance(point, p), score = distance;
        if (heading)
          score += 2 * (1 - (direction[0] * (*heading)[0] +
                             direction[1] * (*heading)[1] +
                             direction[2] * (*heading)[2]));
        if (score < best) {
          best = score;
          result = RoadProjection{p, direction, distance,
                                  e.distances[i - 1] +
                                      t * (e.distances[i] - e.distances[i - 1]),
                                  edge};
        }
      }
    }
    return result;
  };
  for (size_t i = 0; i < 150; ++i) {
    const auto &e =
        graph->traffic_edges()[(i * 7919) % graph->traffic_edges().size()];
    auto point = e.points[e.points.size() / 2];
    point[0] += (i % 3) * 2;
    point[1] += float(i % 4);
    RoadPoint heading = i % 2 ? RoadPoint{1, 0, 0} : RoadPoint{3, 0, -2};
    bool turns = i % 3;
    auto got = graph->traffic_project(point, &heading, turns),
         want = reference(point, &heading, turns);
    SLRR_REQUIRE(got && want && got->edge == want->edge &&
                 road_distance(got->position, want->position) < .001f &&
                 std::fabs(got->along - want->along) < .001f);
  }
  for (auto point : {RoadPoint{999999, 0, -999999}, RoadPoint{0, 5000, 0},
                     RoadPoint{-64, 0, 64}}) {
    auto got = graph->traffic_project(point, nullptr, true),
         want = reference(point, nullptr, true);
    SLRR_REQUIRE(got && want && got->edge == want->edge &&
                 road_distance(got->position, want->position) < .001f);
  }
  SLRR_REQUIRE(!graph->traffic_project({NAN, 0, 0}, nullptr, true));
  std::vector<std::optional<RoadProjection>> parallel(128);
  jobs_parallel_for(parallel.size(), [&](size_t i) {
    const auto &e =
        graph->traffic_edges()[(i * 7919) % graph->traffic_edges().size()];
    parallel[i] =
        graph->traffic_project(e.points[e.points.size() / 2], nullptr, true);
  });
  for (size_t i = 0; i < parallel.size(); ++i) {
    const auto &e =
        graph->traffic_edges()[(i * 7919) % graph->traffic_edges().size()];
    auto serial =
        graph->traffic_project(e.points[e.points.size() / 2], nullptr, true);
    SLRR_REQUIRE(parallel[i] && serial && parallel[i]->edge == serial->edge &&
                 parallel[i]->along == serial->along);
  }
  std::printf("TRAFFIC grid: exhaustive oracle comparisons pass; worker pool "
              "concurrency=%zu\n",
              jobs_concurrency());
  auto begin = std::chrono::steady_clock::now();
  double check = 0;
  for (size_t i = 0; i < 1000; ++i) {
    const auto &e =
        graph->traffic_edges()[(i * 7919) % graph->traffic_edges().size()];
    auto point = e.points[e.points.size() / 2];
    point[0] += 1.7f;
    RoadPoint heading = {1, 0, 0};
    auto projected = graph->traffic_project(point, &heading, true);
    SLRR_REQUIRE(projected);
    check += projected->distance;
  }
  std::printf(
      "TRAFFIC projection benchmark: 1000 queries %.3f ms checksum %.4f\n",
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin)
          .count(),
      check);
  std::printf("TRAFFIC actual lanes=%zu crossing curves=%zu bends=%zu\n", lanes,
              turns, bends);
  SLRR_REQUIRE(lanes > 100 && turns > 100 && bends > 50);
}
int main() {
  const standalone::Test tests[] = {
      traffic_lane_topology, directions,          cross_radius, real_city,
      route_native_contract, shared_signal_phases};
  return standalone::run("road_routes", tests);
}
