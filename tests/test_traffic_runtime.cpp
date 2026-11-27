#include "race_fixture.hpp"
#include "slrr/host_game_options.hpp"
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;
void traffic_step(Jvm &vm, float seconds) {
  if (seconds == 0) {
    race_traffic_tick(vm, 0);
    return;
  }
  for (float remaining = seconds; remaining > 1e-6f;) {
    float step = std::min(remaining, 1.f / 60);
    race_traffic_tick(vm, step);
    part_physics_advance(step);
    remaining -= step;
  }
}

Object *vec(Jvm &vm, const RoadPoint &p) {
  auto *v = vm.new_object("java.lang.Vector3");
  for (int i = 0; i < 3; ++i)
    v->fields[i == 0 ? "x" : i == 1 ? "y" : "z"] = Value::make_float(p[i]);
  return v;
}
void lifecycle_and_stream() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && !graph->edges().empty());
  auto start = graph->edges()[0].points.front();
  f.player = f.car();
  gameref_set_matrix(f.player, start[0], start[1], start[2], 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  auto *command = f.vm.new_string("controllable " +
                                  std::to_string(gameref_make_live(f.player)));
  f.registry.invoke("java.util.resource.GameRef", "queueEvent",
                    "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V",
                    &f.controller,
                    {Value::make_obj(nullptr), Value::make_int(0x10),
                     Value::make_obj(command)});
  SLRR_REQUIRE(part_controlled_chassis() == f.player);
  fs::create_directories(f.root / "probe/src");
  std::ofstream(f.root / "probe/src/GameRef.java")
      << "package java.util.resource; public class GameRef {}";
  std::ofstream(f.root / "probe/src/String.java")
      << "package java.lang; public class String {}";
  std::ofstream(f.root / "probe/src/Listener.java")
      << "package probe; import java.util.resource.*; import java.lang.*; "
         "public class Listener { public native void handleEvent(GameRef "
         "source,int event,String param); }";
  auto report = script_compile_all(f.root.string(), "probe");
  SLRR_REQUIRE(report.failed == 0);
  for (const auto &e : fs::recursive_directory_iterator(f.root / "probe"))
    if (e.path().extension() == ".class")
      SLRR_REQUIRE(f.vm.load_class_file(e.path().string()));
  std::vector<std::string> events;
  f.registry.register_method(
      "probe.Listener", "handleEvent",
      "(Ljava.util.resource.GameRef;ILjava.lang.String;)V",
      [&](Object *, const std::vector<Value> &a) {
        SLRR_REQUIRE(a[0].v.o == &f.ground && a[1].v.i == 0x10);
        events.push_back(a[2].v.o->str);
        return Value::make_void();
      });
  auto *listener = f.vm.new_object("probe.Listener");
  gameref_set_event_mask(listener, 0x10);
  f.sections.adopt_actual(listener);
  auto *rival = f.car();
  auto point = start;
  point[0] += 2;
  auto *where = vec(f.vm, point);
  auto add = [&]() {
    return f
        .call("addTrafficCar",
              "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I",
              {Value::make_obj(rival), Value::make_obj(where)})
        .v.i;
  };
  auto id = add();
  SLRR_REQUIRE(id > 0 && add() == id && race_traffic_pointer(rival) == id);
  f.call("notifyTrafficCar", "(II)I",
         {Value::make_int(id), Value::make_int(1)});
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(events.size() == 1 && events[0].starts_with("car_add ") &&
               !gameref_get(rival)->streamed_hidden);
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(events.size() == 1);
  f.call("notifyTrafficCar", "(II)I",
         {Value::make_int(id), Value::make_int(0)});
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(events.size() == 2 && events.back().starts_with("car_rem "));
  f.call("notifyTrafficCar", "(II)I",
         {Value::make_int(id), Value::make_int(1)});
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(events.size() == 3);
  f.call("remTrafficCar", "(I)V", {Value::make_int(id)});
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(gameref_get(rival) && race_traffic_pointer(rival) == 0 &&
               events.size() == 4);
  auto *taxi = f.packs.pack_by_name("cars/traffic/Taxi.rpk");
  if (!taxi)
    taxi = f.packs.pack_by_path("cars/traffic/Taxi.rpk");
  SLRR_REQUIRE(taxi);
  Object type;
  type.fields["id"] = Value::make_int(RpkIndex::make_full_id(taxi->pack_id, 6));
  auto positioned = f.call(
      "addTrafficP", "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I",
      {Value::make_obj(&type), Value::make_obj(where), Value::make_int(1),
       Value::make_float(4), Value::make_float(4), Value::make_float(2.5f)});
  SLRR_REQUIRE(positioned.v.i == 1);
  traffic_step(f.vm, 0);
  SLRR_REQUIRE(events.size() == 4);
  traffic_step(f.vm, .01f);
  SLRR_REQUIRE(events.size() == 5);
  std::istringstream inserted(events.back());
  std::string inserted_verb;
  int32_t inserted_id = 0;
  inserted >> inserted_verb >> inserted_id;
  auto *seed = gameref_find_live(inserted_id);
  SLRR_REQUIRE(seed && road_distance(start, {seed->pos[0], seed->pos[1],
                                             seed->pos[2]}) < 10);
  std::vector<std::array<float, 3>> supports;
  SLRR_REQUIRE(scene_instances_supports(seed->key, supports) &&
               supports.size() >= 5);
  float bottom = std::numeric_limits<float>::infinity();
  for (auto &point : supports)
    bottom = std::min(bottom, point[1]);
  std::printf("TRAFFIC bottom=%.3f road=%.3f origin=%.3f\n", bottom, start[1],
              seed->pos[1]);
  SLRR_REQUIRE(std::fabs(bottom - graph->traffic_project(point)->position[1]) <
               .25f);
  traffic_step(f.vm, .01f);
  SLRR_REQUIRE(events.size() == 5);
  f.call("delTraffic", "()V");
  traffic_step(f.vm, 0);
  SLRR_REQUIRE(events.size() == 6);
  events.clear();
  auto model =
      f.call("addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I",
             {Value::make_obj(&type), Value::make_int(10), Value::make_float(4),
              Value::make_float(4), Value::make_float(2.5f)});
  SLRR_REQUIRE(model.v.i > 0);
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(events.size() > 0);
  std::istringstream first(events[0]);
  std::string verb;
  int32_t car_id = 0, registration = 0;
  first >> verb >> car_id >> registration;
  auto *traffic = gameref_find_live(car_id);
  SLRR_REQUIRE(traffic && registration > 0);
  RoadPoint before{traffic->pos[0], traffic->pos[1], traffic->pos[2]};
  traffic_step(f.vm, 3);
  SLRR_REQUIRE(road_distance(before, {traffic->pos[0], traffic->pos[1],
                                      traffic->pos[2]}) > 1);
  SLRR_REQUIRE(!scene_instances_collect_root(&f.world).empty());
  // Bot.followCar detaches a streamed police car with remTrafficCar before
  // driving it: the registration ends, the car stays.
  f.call("remTrafficCar", "(I)V", {Value::make_int(registration)});
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(gameref_find_live(car_id) == traffic &&
               race_traffic_pointer(static_cast<Object *>(traffic->key)) == 0 &&
               !traffic->streamed_hidden);
  // Bot.reJoinTraffic: back on a lane, the car takes its old ID again.
  race_traffic_request_enter(static_cast<Object *>(traffic->key), &f.ground);
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(race_traffic_pointer(static_cast<Object *>(traffic->key)) ==
               registration);
  // City re-adds its own tracker for a returning scout, so re-entry sends no
  // second car_add; the next detachment's car_rem removes that tracker.
  f.call("remTrafficCar", "(I)V", {Value::make_int(registration)});
  traffic_step(f.vm, .1f);
  const auto detached_again =
      std::find(events.rbegin(), events.rend(), "car_rem " + std::to_string(car_id));
  SLRR_REQUIRE(detached_again != events.rend());
  events.erase(std::next(detached_again).base());
  part_destroy(static_cast<Object *>(traffic->key));
  f.call("delTraffic", "()V");
  traffic_step(f.vm, .1f);
  SLRR_REQUIRE(!gameref_find_live(car_id));
  size_t added = 0, removed = 0;
  for (const auto &e : events) {
    added += e.starts_with("car_add ");
    removed += e.starts_with("car_rem ");
  }
  SLRR_REQUIRE(added == removed);
  SLRR_REQUIRE(gameref_get(rival));
  part_destroy(rival);
  part_destroy(f.player);
  gameref_remove(&type);
  gameref_remove(listener);
  std::printf("PASS traffic IDs, Controller routing, balanced events, real "
              "Taxi movement/rendering and cleanup (%zu adds)\n",
              added);
}
void red_light_stop_and_release() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && graph->signals().size() > 400);
  size_t selected = graph->traffic_edges().size();
  RoadPoint start;
  for (size_t i = 0; i < graph->traffic_edges().size(); ++i) {
    const auto &e = graph->traffic_edges()[i];
    if (e.length < 40 || graph->traffic_signal(i, 0) != 0)
      continue;
    RoadRoute road;
    road.points = e.points;
    auto point = road_at(road, e.length - 3);
    auto projected = graph->traffic_project(point);
    if (projected && projected->edge == i) {
      selected = i;
      start = point;
      break;
    }
  }
  SLRR_REQUIRE(selected < graph->traffic_edges().size());
  f.player = f.car();
  gameref_set_matrix(f.player, start[0], start[1], start[2], 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  Object type;
  auto taxi = f.packs.open_lib("cars/traffic/Taxi.rpk");
  type.fields["id"] = Value::make_int(RpkIndex::make_full_id(taxi, 6));
  auto *where = vec(f.vm, start);
  SLRR_REQUIRE(f.call("addTrafficP",
                      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I",
                      {Value::make_obj(&type), Value::make_obj(where),
                       Value::make_int(1), Value::make_float(4),
                       Value::make_float(4), Value::make_float(2.5f)})
                   .v.i == 1);
  traffic_step(f.vm, .01f);
  Object *car = nullptr;
  for (const auto &p : part_vehicle_positions())
    if (p.chassis != f.player && race_traffic_pointer(p.chassis))
      car = p.chassis;
  SLRR_REQUIRE(car);
  for (int i = 0; i < 180; ++i)
    traffic_step(f.vm, 1.f / 60);
  auto *n = gameref_get(car);
  SLRR_REQUIRE(n);
  RoadPoint stopped{n->pos[0], n->pos[1], n->pos[2]};
  for (int i = 0; i < 120; ++i)
    traffic_step(f.vm, 1.f / 60);
  std::printf("RED drift=%.4f speed=%.4f\n",
              road_distance(stopped, {n->pos[0], n->pos[1], n->pos[2]}),
              std::hypot(n->vel[0], n->vel[2]));
  SLRR_REQUIRE(road_distance(stopped, {n->pos[0], n->pos[1], n->pos[2]}) <
                   .05 &&
               std::hypot(n->vel[0], n->vel[2]) < .01);
  traffic_step(f.vm, 0);
  SLRR_REQUIRE(road_distance(stopped, {n->pos[0], n->pos[1], n->pos[2]}) < .01);
  bool resumed = false;
  for (int i = 0; i < 5000 && !resumed; ++i) {
    traffic_step(f.vm, 1.f / 60);
    resumed = road_distance(stopped, {n->pos[0], n->pos[1], n->pos[2]}) > 3;
  }
  SLRR_REQUIRE(resumed);
  f.call("delTraffic", "()V");
  part_destroy(f.player);
  std::puts("PASS actual City signal approach stops at red, stays still during "
            "pause and resumes on green");
}
void shared_signal_render_cache() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  Object type;
  type.fields["id"] = Value::make_int(
      RpkIndex::make_full_id(f.packs.open_lib("cars/traffic/Taxi.rpk"), 6));
  f.call("addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I",
         {Value::make_obj(&type), Value::make_int(0), Value::make_float(4),
          Value::make_float(4), Value::make_float(2.5f)});
  auto collect = []() {
    std::vector<std::shared_ptr<const ScxGeometry>> meshes;
    for (const auto &instance : scene_instances_collect())
      if (instance.emissive_overlay && !instance.geometry->groups.empty() &&
          instance.geometry->groups.front().name == "traffic signal") {
        SLRR_REQUIRE(instance.geometry->groups.size() <= 3);
        meshes.push_back(instance.geometry);
      }
    return meshes;
  };
  auto before = collect();
  SLRR_REQUIRE(!before.empty() && before.size() < 5);
  size_t triangles = 0;
  for (auto &mesh : before)
    triangles += mesh->indices.size() / 3;
  SLRR_REQUIRE(triangles > 400 * 16);
  auto unchanged = collect();
  SLRR_REQUIRE(unchanged == before);
  traffic_step(f.vm, 0);
  SLRR_REQUIRE(collect() == before);
  traffic_step(f.vm, 13.1f);
  auto after = collect();
  SLRR_REQUIRE(after.size() == before.size() && after != before);
  std::puts("PASS real City signal lenses share cached emission meshes; phases "
            "refresh groups without per-head draws");
}
void actual_city_turns_and_wheels() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  // Real native map resources may be represented by generic GameTypes.
  // TrafficNative owns the map binding; the enum alone is not its identity.
  gameref_get(&f.ground)->kind = GameRefKind::GameType;
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  const RoadRoutes::Edge *lane = nullptr;
  for (const auto &e : graph->traffic_edges())
    if (!e.crossing && e.length > 40 && !e.next.empty()) {
      bool bends = true;
      for (auto index : e.next) {
        const auto &t = graph->traffic_edges()[index];
        auto a = road_normalize({t.points[1][0] - t.points[0][0], 0,
                                 t.points[1][2] - t.points[0][2]});
        auto b = road_normalize(
            {t.points.back()[0] - t.points[t.points.size() - 2][0], 0,
             t.points.back()[2] - t.points[t.points.size() - 2][2]});
        bends &= a[0] * b[0] + a[2] * b[2] < .8f;
      }
      if (bends) {
        lane = &e;
        break;
      }
    }
  SLRR_REQUIRE(lane);
  RoadRoute line;
  line.points = lane->points;
  auto start = road_at(line, lane->length - 8);
  f.player = f.car();
  gameref_set_matrix(f.player, start[0], start[1], start[2], 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  Object type;
  type.fields["id"] = Value::make_int(
      RpkIndex::make_full_id(f.packs.open_lib("cars/traffic/Taxi.rpk"), 6));
  auto *where = vec(f.vm, start);
  SLRR_REQUIRE(f.call("addTrafficP",
                      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I",
                      {Value::make_obj(&type), Value::make_obj(where),
                       Value::make_int(1), Value::make_float(4),
                       Value::make_float(4), Value::make_float(2.5f)})
                   .v.i == 1);
  traffic_step(f.vm, .01f);
  Object *car = nullptr;
  for (const auto &p : part_vehicle_positions())
    if (p.chassis != f.player && race_traffic_pointer(p.chassis))
      car = p.chassis;
  SLRR_REQUIRE(car);
  auto *n = gameref_get(car);
  float start_yaw = n->ypr[0], max_step = 0, max_turn = 0, total_turn = 0,
        max_steer = 0;
  std::vector<std::pair<void *, PartMatrix>> initial;
  for (auto key : n->children)
    if (auto *child = gameref_get(key);
        child && child->kind == GameRefKind::RenderRef) {
      PartMatrix local;
      std::copy(child->local_matrix, child->local_matrix + 16, local.begin());
      initial.push_back({key, local});
    }
  auto simulation_begin = std::chrono::steady_clock::now();
  for (int tick = 0; tick < 3600; ++tick) {
    RoadPoint before{n->pos[0], n->pos[1], n->pos[2]};
    float yaw = n->ypr[0];
    gameref_set_matrix(f.player, before[0] + 15, before[1], before[2] + 15, 0,
                       0, 0);
    traffic_step(f.vm, 1.f / 60);
    SLRR_REQUIRE(gameref_get(car) && !n->streamed_hidden);
    float step = road_distance(before, {n->pos[0], n->pos[1], n->pos[2]});
    float turn = std::fabs(std::remainder(n->ypr[0] - yaw, 6.2831853f));
    max_step = std::max(max_step, step);
    max_turn = std::max(max_turn, turn);
    total_turn =
        std::max(total_turn,
                 std::fabs(std::remainder(n->ypr[0] - start_yaw, 6.2831853f)));
    // Rural roads cruise up to 100 km/h (bot.ts speed bands), .46 m a tick;
    // a jump or teleport is still far larger.
    SLRR_REQUIRE(step < .5f && turn < .3f);
    for (const auto &[key, base] : initial)
      if (auto *w = gameref_get(key)) {
        auto *m = w->local_matrix;
        float angle = std::atan2(base[0] * m[2] - base[2] * m[0],
                                 base[0] * m[0] + base[2] * m[2]);
        max_steer = std::max(max_steer, std::fabs(angle));
      }
  }
  std::printf("TRAFFIC physical benchmark: 3600 ticks %.3f ms\n",
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - simulation_begin)
                  .count());
  int animated = 0;
  for (const auto &[key, base] : initial) {
    auto *wheel = gameref_get(key);
    SLRR_REQUIRE(wheel);
    float difference = 0;
    for (int i = 0; i < 12; ++i)
      difference += std::fabs(wheel->local_matrix[i] - base[i]);
    if (difference > .1f) {
      ++animated;
    }
  }
  std::printf(
      "TRAFFIC turn=%.3f max-step=%.3f max-yaw-step=%.3f animated-wheels=%d\n",
      total_turn, max_step, max_turn, animated);
  std::printf("TRAFFIC wheel steering max=%.3f\n", max_steer);
  PartPhysicsState state;
  SLRR_REQUIRE(part_physics_state(car, state) && state.active &&
               state.ticks > 1000);
  SLRR_REQUIRE(total_turn > 1 && animated == 4 && max_steer > .05f);
  f.call("delTraffic", "()V");
  part_destroy(f.player);
}
void native_day_racer_staging() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  // Real native map resources may be represented by generic GameTypes.
  // TrafficNative owns the map binding; the enum alone is not its identity.
  gameref_get(&f.ground)->kind = GameRefKind::GameType;
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && !graph->traffic_edges().empty());
  auto focus = graph->traffic_edges().front().points.front();
  f.player = f.car();
  gameref_set_matrix(f.player, focus[0], focus[1], focus[2], 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  auto pack = f.packs.open_lib("cars/fake_racers/Teg.rpk");
  uint32_t type_id = 0;
  for (const auto &entry : f.packs.pack_by_id(pack)->entries) {
    auto id = RpkIndex::make_full_id(pack, entry.type_id);
    auto cfg = load_part_config(f.packs, id);
    if (cfg.valid && cfg.native_kind == "car") {
      type_id = id;
      break;
    }
  }
  SLRR_REQUIRE(type_id);
  Object type;
  type.fields["id"] = Value::make_int(type_id);
  auto *dummy = f.vm.new_object("java.util.resource.GameRef");
  f.registry.invoke("java.util.resource.GameRef", "create_native",
                    "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
                    "Ljava.lang.String;Ljava.lang.String;)V",
                    dummy,
                    {Value::make_obj(&f.ground), Value::make_obj(&type),
                     Value::make_obj(f.vm.new_string("0,-10000,0,0,0,0")),
                     Value::make_obj(f.vm.new_string("dummycar"))});
  auto *chassis = part_chassis(dummy);
  SLRR_REQUIRE(chassis && gameref_get(dummy) == gameref_get(chassis) &&
               gameref_get(dummy)->pos[1] < -9999);
  auto id = f.call("addTrafficCar",
                   "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I",
                   {Value::make_obj(dummy), Value::make_obj(nullptr)})
                .v.i;
  SLRR_REQUIRE(id > 0);
  auto *n = gameref_get(dummy);
  for (int tick = 0; tick < 600 && n->streamed_hidden; ++tick)
    traffic_step(f.vm, 1.f / 60);
  std::printf("DAY initial hidden=%d x=%.3f y=%.3f z=%.3f id=%d\n",
              n->streamed_hidden, n->pos[0], n->pos[1], n->pos[2], id);
  SLRR_REQUIRE(n && !n->streamed_hidden && n->pos[1] > -100);
  SceneFloorHit floor;
  SLRR_REQUIRE(scene_instances_sample_floor(n->pos[0], n->pos[2], n->pos[1] + 4,
                                            chassis, floor));
  std::vector<RoadPoint> supports;
  SLRR_REQUIRE(scene_instances_supports(chassis, supports));
  float bottom = INFINITY;
  for (auto p : supports)
    bottom = std::min(bottom, p[1]);
  SLRR_REQUIRE(std::fabs(bottom - floor.height) < .15f);
  RoadPoint initial{n->pos[0], n->pos[1], n->pos[2]};
  for (int tick = 0; tick < 600; ++tick)
    traffic_step(f.vm, 1.f / 60);
  SLRR_REQUIRE(!n->streamed_hidden && n->pos[1] > -100 &&
               road_distance(initial, {n->pos[0], n->pos[1], n->pos[2]}) > 1);
  PartPhysicsState physical;
  SLRR_REQUIRE(part_physics_state(chassis, physical) && physical.active &&
               physical.ticks > 1000);
  std::printf(
      "DAY RACER native staging -10000 -> road %.3f body %.3f bottom %.3f\n",
      floor.height, n->pos[1], bottom);
  f.call("remTrafficCar", "(I)V", {Value::make_int(id)});
  SLRR_REQUIRE(gameref_get(dummy));
  part_destroy(chassis);
  part_destroy(f.player);
}
void inline_wheel_contact_and_population() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  RoadRoute lane;
  lane.points = graph->traffic_edges().front().points;
  auto focus = road_at(lane, 30);
  for (const char *pack_name :
       {"cars/traffic/Wagon.rpk", "cars/fake_racers/Teg.rpk"}) {
    auto pack = f.packs.open_lib(pack_name);
    uint32_t type = 0;
    for (const auto &e : f.packs.pack_by_id(pack)->entries) {
      auto id = RpkIndex::make_full_id(pack, e.type_id);
      auto cfg = load_part_config(f.packs, id);
      if (cfg.valid && cfg.native_kind == "car") {
        type = id;
        break;
      }
    }
    SLRR_REQUIRE(type);
    auto cfg = load_part_config(f.packs, type);
    auto *car = part_create(f.vm, f.packs, &f.ground, type,
                            "wheel contact probe", {}, true);
    SLRR_REQUIRE(car);
    SceneFloorHit hit;
    SLRR_REQUIRE(scene_instances_sample_floor(focus[0], focus[2], focus[1] + 4,
                                              car, hit));
    gameref_set_matrix(car, focus[0], hit.height, focus[2], 0, 0, 0);
    part_park_chassis(car);
    part_command(car, "wakeup");
    for (int tick = 0; tick < 360; ++tick)
      part_physics_advance(1. / 60);
    PartPhysicsState state;
    SLRR_REQUIRE(part_physics_state(car, state) && state.ticks > 0);
    for (size_t corner = 0; corner < 4; ++corner) {
      auto expected = cfg.wheels[corner].position;
      expected[1] += .25f - float(state.length[corner]);
      GameRefState *visual = nullptr;
      float best = INFINITY;
      for (auto key : gameref_get(car)->children) {
        auto *n = gameref_get(key);
        if (!n || n->kind != GameRefKind::RenderRef)
          continue;
        bool wheel = false;
        for (const auto &r : cfg.renders)
          wheel |= r.wheel >= 0 && r.resource == uint32_t(n->type_id);
        if (!wheel)
          continue;
        float d = std::hypot(n->local_matrix[12] - expected[0],
                             n->local_matrix[14] - expected[2]);
        if (d < best) {
          best = d;
          visual = n;
        }
      }
      SLRR_REQUIRE(visual && best < .01f &&
                   std::fabs(visual->local_matrix[13] - expected[1]) < .001f);
      PartMatrix frame;
      SLRR_REQUIRE(scene_instances_script_matrix(visual->key, frame.data()));
      SceneFloorHit floor;
      SLRR_REQUIRE(scene_instances_sample_floor(frame[12], frame[14],
                                                frame[13] + 1, car, floor));
      float gap = frame[13] - cfg.wheel_springs[corner].radius - floor.height;
      std::printf("TYRE %s corner=%zu gap=%.4f force=%.1f\n", pack_name, corner,
                  gap, state.force[corner]);
      auto mesh = scene_instances_geometry(uint32_t(visual->type_id));
      SLRR_REQUIRE(mesh);
      float lowest = INFINITY;
      for (size_t v = 0; v < mesh->positions.size(); v += 3) {
        float y =
            (frame[1] * mesh->positions[v] + frame[5] * mesh->positions[v + 1] +
             frame[9] * mesh->positions[v + 2]) *
                mesh->position_scale +
            frame[13];
        lowest = std::min(lowest, y);
      }
      std::printf("RENDERED TYRE bottom gap=%.4f\n", lowest - floor.height);
      SLRR_REQUIRE(state.contact[corner] && std::fabs(gap) < .04f &&
                   std::fabs(lowest - floor.height) < .05f);
    }
    part_destroy(car);
  }
  f.player = f.car();
  gameref_set_matrix(f.player, focus[0], focus[1], focus[2], 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  Object model;
  model.fields["id"] = Value::make_int(
      RpkIndex::make_full_id(f.packs.open_lib("cars/traffic/Taxi.rpk"), 6));
  f.call("addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I",
         {Value::make_obj(&model), Value::make_int(1000), Value::make_float(2),
          Value::make_float(5), Value::make_float(2)});
  std::vector<Object *> rivals;
  for (int i = 0; i < 100; ++i) {
    auto *car = f.car();
    rivals.push_back(car);
    f.call("addTrafficCar",
           "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I",
           {Value::make_obj(car), Value::make_obj(nullptr)});
  }
  auto count = [&]() {
    size_t total = 0, days = 0;
    for (auto p : part_vehicle_positions())
      if (race_traffic_pointer(p.chassis)) {
        ++total;
        days +=
            std::find(rivals.begin(), rivals.end(), p.chassis) != rivals.end();
      }
    return std::pair{total, days};
  };
  for (float density : {.2f, .1f, .8f, 0.f}) {
    f.vm.set_static("java.util.Config", "trafficDensity",
                    Value::make_float(density));
    host_game_options_load(f.vm);
    for (int tick = 0; tick < 20; ++tick)
      race_traffic_tick(f.vm, 1.f / 60);
    auto [total, days] = count();
    size_t local = 0;
    for (const auto &vehicle : part_vehicle_positions())
      if (race_traffic_pointer(vehicle.chassis) &&
          road_distance(
              {vehicle.position[0], vehicle.position[1], vehicle.position[2]},
              focus) <= 165)
        ++local;
    SLRR_REQUIRE(local == total);
    size_t cap = size_t(std::lround(density * 100));
    std::printf("DENSITY %.1f total=%zu day=%zu normal=%zu\n", density, total,
                days, total - days);
    SLRR_REQUIRE(total <= cap &&
                 days <= size_t(std::lround(cap * 100. / 1100)));
    if (cap)
      SLRR_REQUIRE(total - days >= cap / 2);
    else
      SLRR_REQUIRE(total == 0);
    if (density > .5f)
      SLRR_REQUIRE(total > 50);
    if (density == .2f) {
      auto begin = std::chrono::steady_clock::now();
      traffic_step(f.vm, 1);
      std::printf("TRAFFIC fleet benchmark: %zu cars / 60 ticks %.3f ms\n",
                  total,
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - begin)
                      .count());
    }
  }
  f.vm.set_static("java.util.Config", "trafficDensity", Value::make_float(.2f));
  host_game_options_load(f.vm);
  f.call("delTraffic", "()V");
  for (auto *car : rivals)
    part_destroy(car);
  part_destroy(f.player);
}
void missing_lamp_stop_releases() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  size_t edge = 0;
  for (; edge < graph->traffic_edges().size(); ++edge)
    if (!graph->traffic_edges()[edge].crossing &&
        graph->traffic_signal(edge, 0) == -2 &&
        graph->traffic_edges()[edge].length > 20)
      break;
  SLRR_REQUIRE(edge < graph->traffic_edges().size());
  RoadRoute line;
  line.points = graph->traffic_edges()[edge].points;
  auto start = road_at(line, graph->traffic_edges()[edge].length - 3);
  f.player = f.car();
  gameref_set_matrix(f.player, start[0] + 15, start[1], start[2] + 15, 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  Object type;
  type.fields["id"] = Value::make_int(
      RpkIndex::make_full_id(f.packs.open_lib("cars/traffic/Taxi.rpk"), 6));
  f.call("addTrafficP",
         "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I",
         {Value::make_obj(&type), Value::make_obj(vec(f.vm, start)),
          Value::make_int(1), Value::make_float(2), Value::make_float(5),
          Value::make_float(2)});
  traffic_step(f.vm, .02f);
  Object *car = nullptr;
  for (const auto &p : part_vehicle_positions())
    if (p.chassis != f.player && race_traffic_pointer(p.chassis))
      car = p.chassis;
  SLRR_REQUIRE(car);
  traffic_step(f.vm, 3);
  auto *n = gameref_get(car);
  RoadPoint stopped{n->pos[0], n->pos[1], n->pos[2]};
  SLRR_REQUIRE(std::hypot(n->vel[0], n->vel[2]) < .1f);
  traffic_step(f.vm, 10);
  SLRR_REQUIRE(road_distance(stopped, {n->pos[0], n->pos[1], n->pos[2]}) > 3);
  f.call("delTraffic", "()V");
  part_destroy(f.player);
  std::puts("PASS real missing-lamp approach stops then releases instead of "
            "permanent red");
}
// An unavailable local spawn must not allocate disposable car assemblies.
// This is independent of timing and catches per-frame churn at an empty cap.
void unavailable_spawn_does_not_allocate() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  scene_instances_enable_static_floor(false);
  f.player = f.car();
  gameref_set_matrix(f.player, 100000, 0, 100000, 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  Object type;
  type.fields["id"] = Value::make_int(
      RpkIndex::make_full_id(f.packs.open_lib("cars/traffic/Taxi.rpk"), 6));
  f.call("addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I",
         {Value::make_obj(&type), Value::make_int(1000), Value::make_float(2),
          Value::make_float(5), Value::make_float(2)});
  // Warm graph/resource caches, then require repeated failed placements to
  // leave the VM heap unchanged without relying on a collection.
  race_traffic_tick(f.vm, 1.f / 60);
  const auto objects = f.vm.heap_size();
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i)
    race_traffic_tick(f.vm, 1.f / 60);
  std::printf("UNAVAILABLE SPAWN 10 ticks: %.3f ms, %zu disposable objects\n",
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - begin).count(),
              f.vm.heap_size() - objects);
  SLRR_REQUIRE(f.vm.heap_size() == objects);
  f.call("delTraffic", "()V");
  part_destroy(f.player);
}
void slow_frames_preserve_signal_period() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && !graph->signals().empty());
  // Signal publication belongs to the ground's registered traffic population.
  Object model;
  model.fields["id"] = Value::make_int(f.car_type);
  f.call("addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I",
         {Value::make_obj(&model), Value::make_int(1), Value::make_float(2),
          Value::make_float(5), Value::make_float(2)});
  // Two FPS: physical catch-up stays at four ticks, while each frame accounts
  // for half a second in the shared lamp/driver clock.
  const auto fixture = graph->signals().front();
  for (int frame = 0; frame < 208; ++frame) {
    race_traffic_tick(f.vm, 4.f / 60, .5);
    SLRR_REQUIRE(race_signal_at(fixture.position) ==
                 graph->signal(fixture.crossing, fixture.branch,
                               (frame + 1) * .5));
  }
  const auto revision = race_signal_revision();
  const auto color = race_signal_at(fixture.position);
  race_traffic_tick(f.vm, 0, 0);
  SLRR_REQUIRE(race_signal_revision() == revision &&
               race_signal_at(fixture.position) == color);
}
void native_stock_car_uses_native_drive() {
  RaceFixture f;
  f.packs.open_all();
  f.ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &f.ground);
  gameref_get(&f.ground)->kind = GameRefKind::GameType;
  scene_instances_enable_static_floor(false);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  const RoadRoutes::Edge *lane = nullptr;
  for (const auto &edge : graph->traffic_edges())
    if (!edge.crossing && edge.length > 100 && !edge.next.empty()) {
      lane = &edge;
      break;
    }
  SLRR_REQUIRE(lane);
  RoadRoute line;
  line.points = lane->points;
  const auto start = road_at(line, 10);
  const auto type = RpkIndex::make_full_id(
      f.packs.open_lib("cars/racers/Baiern.rpk"), 6);
  const auto cfg = load_part_config(f.packs, type);
  SLRR_REQUIRE(cfg.valid && !cfg.stock.empty() && cfg.wheels.size() == 4);
  SLRR_REQUIRE(std::none_of(cfg.renders.begin(), cfg.renders.end(),
                             [](const auto &r) { return r.wheel >= 0; }));
  auto *car = part_create(f.vm, f.packs, &f.ground, type,
                          "native assembled racer", {}, true);
  SLRR_REQUIRE(car && part_installed_tree(car).size() > 4);
  SLRR_REQUIRE(!car->fields.contains("ratio"));
  f.player = f.car();
  gameref_set_matrix(f.player, start[0] + 15, start[1], start[2] + 15, 0, 0, 0);
  f.controller.class_name = "java.io.Controller";
  gameref_upsert(&f.controller);
  part_command(&f.controller,
               "controllable " + std::to_string(gameref_make_live(f.player)));
  SLRR_REQUIRE(f.call("addTrafficCar",
      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I",
      {Value::make_obj(car), Value::make_obj(vec(f.vm, start))}).v.i > 0);
  traffic_step(f.vm, .02f);
  auto *node = gameref_get(car);
  const RoadPoint before{node->pos[0], node->pos[1], node->pos[2]};
  traffic_step(f.vm, 5);
  PartPhysicsState state;
  SLRR_REQUIRE(part_physics_state(car, state) && state.active);
  SLRR_REQUIRE(road_distance(before,
      {node->pos[0], node->pos[1], node->pos[2]}) > 5);
  f.call("delTraffic", "()V");
  part_destroy(car);
  part_destroy(f.player);
}
int main() {
  const standalone::Test tests[] = {
      native_stock_car_uses_native_drive, slow_frames_preserve_signal_period, unavailable_spawn_does_not_allocate,
      missing_lamp_stop_releases, inline_wheel_contact_and_population,
      native_day_racer_staging,   actual_city_turns_and_wheels,
      lifecycle_and_stream,       red_light_stop_and_release,
      shared_signal_render_cache};
  return standalone::run("traffic_runtime", tests);
}
