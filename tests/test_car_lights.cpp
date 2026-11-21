// Car lights (#279): each headlight part owns one spot beam and each brake
// light part one brake light, from the part's own cfg `render` light
// children. Lights follow their part: a removed headlight takes its beam
// away. Headlights light at night with the engine running, brake lights
// while the car brakes. Uses the installed game data, read-only.
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include <array>
#include <cmath>
namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;
namespace {
size_t count(const std::vector<CarLight> &lights, CarLightKind kind) {
  size_t n = 0;
  for (const auto &light : lights)
    n += light.kind == kind;
  return n;
}
const CarLight *find(const std::vector<CarLight> &lights, CarLightKind kind, size_t nth = 0) {
  for (const auto &light : lights)
    if (light.kind == kind && nth-- == 0)
      return &light;
  return nullptr;
}
// #284: gaps between each lamp-glass vertex and the lens of its part, in
// metres along the nearest lens triangle's plane (a body triangle facing
// within 60 degrees of the glass normal), for vertices with a lens triangle
// within 3 cm. Also the largest
// distance a vertex moved from the stock mesh.
struct GlassGaps {
  size_t meshes = 0, vertices = 0;
  double smallest = 1e9, largest = -1e9, moved = 0;
};
using V3 = std::array<double, 3>;
V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot3(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross3(V3 a, V3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 place(const GameRefState &node, const ScxGeometry &mesh, size_t v) {
  const float *m = node.local_matrix;
  const double k = mesh.position_scale, x = mesh.positions[v * 3] * k,
               y = mesh.positions[v * 3 + 1] * k, z = mesh.positions[v * 3 + 2] * k;
  return {m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
          m[2] * x + m[6] * y + m[10] * z + m[14]};
}
GlassGaps glass_gaps(Object *car) {
  GlassGaps out;
  for (const auto &item : part_installed_tree(car)) {
    const auto *part = gameref_get(item.object);
    if (!part) continue;
    std::vector<std::array<V3, 3>> body;
    std::vector<const GameRefState *> glass;
    for (void *child : part->children) {
      const auto *node = gameref_get(child);
      if (!node || node->empty || node->kind != GameRefKind::RenderRef) continue;
      if (node->light_flags) { glass.push_back(node); continue; }
      const auto mesh = scene_instances_geometry(uint32_t(node->type_id));
      if (!mesh) continue;
      for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3)
        body.push_back({place(*node, *mesh, mesh->indices[t]), place(*node, *mesh, mesh->indices[t + 1]),
                        place(*node, *mesh, mesh->indices[t + 2])});
    }
    if (body.empty()) continue;
    for (const auto *node : glass) {
      const auto stock = scene_instances_geometry(uint32_t(node->type_id));
      const auto drawn = node->deformed_geometry && node->deformed_type == node->type_id
                             ? node->deformed_geometry : stock;
      if (!stock || !drawn || drawn->positions.size() != stock->positions.size()) continue;
      ++out.meshes;
      for (size_t v = 0; v * 3 + 2 < drawn->positions.size(); ++v) {
        const V3 p = place(*node, *drawn, v), before = place(*node, *stock, v);
        const V3 d = sub(p, before);
        out.moved = std::max(out.moved, std::sqrt(dot3(d, d)));
        const float *m = node->local_matrix;
        const double nx = stock->normals[v * 3], ny = stock->normals[v * 3 + 1], nz = stock->normals[v * 3 + 2];
        const V3 normal{m[0] * nx + m[4] * ny + m[8] * nz, m[1] * nx + m[5] * ny + m[9] * nz,
                        m[2] * nx + m[6] * ny + m[10] * nz};
        double best = .03, gap = 0;
        bool found = false;
        for (const auto &tri : body) {
          const V3 e0 = sub(tri[1], tri[0]), e1 = sub(tri[2], tri[0]);
          V3 n = cross3(e0, e1);
          const double length = std::sqrt(dot3(n, n));
          if (!(length > 1e-10)) continue;
          for (auto &c : n) c /= length;
          if (dot3(n, normal) < 0) for (auto &c : n) c = -c;
          // A lens faces the way the glass does (not a housing wall).
          if (dot3(n, normal) < .5 * std::sqrt(dot3(normal, normal))) continue;
          const double h = dot3(sub(p, tri[0]), n);
          if (std::fabs(h) >= best) continue;
          const V3 q = sub(sub(p, {n[0] * h, n[1] * h, n[2] * h}), tri[0]);
          const double d00 = dot3(e0, e0), d01 = dot3(e0, e1), d11 = dot3(e1, e1);
          const double d20 = dot3(q, e0), d21 = dot3(q, e1), det = d00 * d11 - d01 * d01;
          if (!(std::fabs(det) > 1e-20)) continue;
          const double b1 = (d11 * d20 - d01 * d21) / det, b2 = (d00 * d21 - d01 * d20) / det;
          if (b1 < -.02 || b2 < -.02 || b1 + b2 > 1.04) continue;
          best = std::fabs(h);
          gap = h;
          found = true;
        }
        if (!found) continue;
        ++out.vertices;
        out.smallest = std::min(out.smallest, gap);
        out.largest = std::max(out.largest, gap);
      }
    }
  }
  return out;
}
void set_time(Jvm &vm, float seconds) {
  vm.set_static("java.game.GameLogic", "time", Value::make_float(seconds));
}
} // namespace

void stock_baiern_lights() {
  Fixture f;
  const auto game = fs::absolute("game");
  for (const auto &e : fs::recursive_directory_iterator(game))
    if (e.is_regular_file() && e.path().extension() == ".class") {
      auto out = f.root / fs::relative(e.path(), game);
      fs::create_directories(out.parent_path());
      fs::copy_file(e.path(), out);
    }
  f.packs.set_root(game.string());
  f.packs.open_all();
  rpk_set_active(&f.packs);
  SLRR_REQUIRE(f.vm.bootstrap_classpath());
  gameref_clear();
  register_lang_natives(f.vm, f.packs, f.sections);
  register_gameref_natives(f.vm, f.packs, f.sections);
  register_resref_natives(f.vm, f.packs, f.sections);
  register_renderref_natives(f.vm, f.packs, f.sections);
  register_part_natives(f.vm, f.packs);
  register_race_natives(f.vm, f.packs, f.sections);
  Object world, ground, brain;
  gameref_upsert(&world, GameRefKind::Dummy)->flags = kWorldTreeRoot;
  auto city = f.packs.open_lib("maps/city.rpk");
  auto *g = gameref_upsert(&ground, GameRefKind::GroundRef);
  g->type_id = RpkIndex::make_full_id(city, 1);
  gameref_set_parent(&ground, &world);
  gameref_upsert(&brain);
  scene_instances_set_resources(&f.packs);
  scene_instances_select(&world, nullptr, false);
  ground.class_name = "java.util.resource.GroundRef";
  spawn_ground_shell(f.vm, f.packs, &ground);
  scene_instances_enable_static_floor(false);
  const auto pack = f.packs.open_lib("cars/racers/Baiern.rpk");
  uint32_t type = 0;
  for (const auto &e : f.packs.pack_by_id(pack)->entries) {
    const auto id = RpkIndex::make_full_id(pack, e.type_id);
    const auto cfg = load_part_config(f.packs, id);
    if (cfg.valid && cfg.native_kind == "car") {
      type = id;
      break;
    }
  }
  SLRR_REQUIRE(type);
  auto *vehicle = vehicle_spawn_stock(f.vm, f.packs, &ground, type);
  SLRR_REQUIRE(vehicle);
  auto *car = part_chassis(vehicle);
  SLRR_REQUIRE(car);
  auto graph = race_ground_roads(f.packs, &ground);
  SLRR_REQUIRE(graph);
  const auto &e = graph->edges().front();
  const auto start = e.points.front(), finish = e.points.back();
  const auto toward = e.points[1];
  const float yaw = std::atan2(-(toward[0] - start[0]), -(toward[2] - start[2]));
  gameref_set_matrix(car, start[0], start[1], start[2], yaw, 0, 0);
  part_park_chassis(car);

  // Ownership: the stock Baiern has two headlight parts and two taillight
  // parts; each owns exactly one light of its kind.
  auto all = part_car_lights(false);
  const size_t heads = count(all, CarLightKind::Headlight);
  const size_t brakes = count(all, CarLightKind::Brake);
  std::printf("CAR LIGHTS Baiern: %zu headlights, %zu brake lights (all parts)\n", heads, brakes);
  SLRR_REQUIRE(heads == 2 && brakes == 2);
  const CarLight left = *find(all, CarLightKind::Headlight, 0);
  const CarLight right = *find(all, CarLightKind::Headlight, 1);
  SLRR_REQUIRE(left.part != right.part && left.car == car && right.car == car);
  SLRR_REQUIRE(find(all, CarLightKind::Brake, 0)->part != find(all, CarLightKind::Brake, 1)->part);
  for (const auto &light : all)
    SLRR_REQUIRE(light.part != car);
  // The headlights are the `type spot` resource (diffuse 0.2, range 400),
  // the brake lights the red glow-only `fakespot` (range 5).
  SLRR_REQUIRE(left.range == 400.f && left.theta > 1.8f && left.diffuse[0] < .5f);
  const CarLight rear = *find(all, CarLightKind::Brake);
  SLRR_REQUIRE(rear.diffuse[0] > .9f && rear.diffuse[1] < .1f && rear.range == 5.f);
  // Placement: beams leave the front of the car, forward and level within
  // about 15 degrees; brake lights sit behind the headlights.
  PartMatrix m;
  SLRR_REQUIRE(scene_instances_render_matrix(car, m.data()));
  const std::array<float, 3> centre{m[12], m[13], -m[14]};
  for (const auto *light : {&left, &right}) {
    float out = 0, ahead = 0;
    for (int i = 0; i < 3; ++i) {
      out += (light->position[i] - centre[i]) * light->direction[i];
      ahead += (light->position[i] - rear.position[i]) * light->direction[i];
    }
    std::printf("CAR LIGHTS headlight at %.2f %.2f %.2f dir %.3f %.3f %.3f (%.2f m ahead of the centre, %.2f of the brake light)\n",
                light->position[0], light->position[1], light->position[2],
                light->direction[0], light->direction[1], light->direction[2], out, ahead);
    SLRR_REQUIRE(out > .8f && ahead > 2.f && std::fabs(light->direction[1]) < .26f);
  }
  SLRR_REQUIRE(std::hypot(left.position[0] - right.position[0], left.position[2] - right.position[2]) > .8f);

  // #284: lamp glass sits on its lens: 0 to 1 mm in front of the lamp body,
  // never behind, moved at most a few centimetres from the stock mesh.
  const auto gaps = glass_gaps(car);
  std::printf("LAMP GLASS %zu meshes, %zu vertices over a lens: gap %.2f to %.2f mm, moved at most %.1f mm\n",
              gaps.meshes, gaps.vertices, gaps.smallest * 1000, gaps.largest * 1000, gaps.moved * 1000);
  SLRR_REQUIRE(gaps.meshes >= 8 && gaps.vertices >= 40);
  SLRR_REQUIRE(gaps.smallest >= 0. && gaps.largest < .001 && gaps.moved < .03);

  // Gating: parked, engine off, midday: nothing is lit.
  set_time(f.vm, 12 * 3600.f);
  SLRR_REQUIRE(part_car_lights(true).empty());
  // Driving at night under autonomous control: headlights lit, no brake.
  race_brain_command(f.packs, &brain, "controllable " + std::to_string(gameref_make_live(car)));
  f.vm.set_static("java.util.resource.GameRef", "EVENT_COMMAND", Value::make_int(0x10));
  auto *bot = f.vm.new_object("java.game.Bot");
  bot->fields["car"] = Value::make_obj(vehicle);
  bot->fields["brain"] = Value::make_obj(&brain);
  bot->fields["world"] = Value::make_obj(&ground);
  bot->fields["traffic_id"] = Value::make_int(0);
  auto *racer = f.vm.new_object("java.game.Racer");
  racer->fields["car"] = Value::make_obj(vehicle);
  auto *destination = f.vm.new_object("java.lang.Vector3");
  destination->fields["x"] = Value::make_float(finish[0]);
  destination->fields["y"] = Value::make_float(finish[1]);
  destination->fields["z"] = Value::make_float(finish[2]);
  f.vm.invoke("java.game.Bot", "startRace", "(Ljava.lang.Vector3;Ljava.game.Racer;)V",
              {Value::make_obj(bot), Value::make_obj(destination), Value::make_obj(racer)}, false);
  for (int i = 0; i < 240; ++i)
    part_physics_advance(1. / 60.);
  set_time(f.vm, 12 * 3600.f);
  const auto day = part_car_lights(true);
  set_time(f.vm, 23 * 3600.f);
  auto lit = part_car_lights(true);
  PartPhysicsState state;
  SLRR_REQUIRE(part_physics_state(car, state));
  std::printf("CAR LIGHTS driving %.1f km/h: day %zu lit, night %zu headlights %zu brake lights\n",
              state.speed_kph, day.size(), count(lit, CarLightKind::Headlight),
              count(lit, CarLightKind::Brake));
  SLRR_REQUIRE(count(day, CarLightKind::Headlight) == 0);
  SLRR_REQUIRE(count(lit, CarLightKind::Headlight) == 2 && count(lit, CarLightKind::Brake) == 0);
  // Braking: the brake light parts light up (day or night), and their
  // lamp glass with them.
  race_brain_command(f.packs, &brain, "AI_stop");
  for (int i = 0; i < 20; ++i)
    part_physics_advance(1. / 60.);
  lit = part_car_lights(true);
  std::printf("CAR LIGHTS braking: %zu brake lights, %zu headlights\n",
              count(lit, CarLightKind::Brake), count(lit, CarLightKind::Headlight));
  SLRR_REQUIRE(count(lit, CarLightKind::Brake) == 2 && count(lit, CarLightKind::Headlight) == 2);
  SLRR_REQUIRE(part_lamp_lit(rear.part, 1u, false) && !part_lamp_lit(rear.part, 1u, true));
  set_time(f.vm, 12 * 3600.f);
  SLRR_REQUIRE(count(part_car_lights(true), CarLightKind::Brake) == 2);
  set_time(f.vm, 23 * 3600.f);

  // A lost headlight takes only its own beam away.
  SLRR_REQUIRE(part_destroy(left.part));
  all = part_car_lights(false);
  lit = part_car_lights(true);
  std::printf("CAR LIGHTS one headlight removed: %zu headlights (%zu lit), %zu brake lights\n",
              count(all, CarLightKind::Headlight), count(lit, CarLightKind::Headlight),
              count(all, CarLightKind::Brake));
  SLRR_REQUIRE(count(all, CarLightKind::Headlight) == 1 && count(lit, CarLightKind::Headlight) == 1);
  SLRR_REQUIRE(find(lit, CarLightKind::Headlight)->part == right.part);
  SLRR_REQUIRE(count(all, CarLightKind::Brake) == 2);

  // Wear: condition = wear x tear. Past kLampBrokenCondition the lamp is
  // broken (no beam, no lit glass); between it and kLampFullCondition it
  // dims from 40 %; a repaired lamp is at full power again.
  const auto wear = [&](Object *part, float value) {
    f.vm.natives().invoke("java.game.parts.Part", "setWear", "(F)V", part,
                          {Value::make_float(value)});
  };
  const auto beam = [&]() -> float {
    const auto *head = find(part_car_lights(true), CarLightKind::Headlight);
    return head ? head->power : 0.f;
  };
  SLRR_REQUIRE(beam() == 1.f);
  wear(right.part, .2f);
  const float broken = beam();
  const bool glass = part_lamp_lit(right.part, 4u, false);
  wear(right.part, .4f);
  const float dim = beam();
  wear(right.part, 1.f);
  const float repaired = beam();
  std::printf("CAR LIGHTS headlight wear 0.2 -> power %.2f (glass %d), 0.4 -> %.2f, 1.0 -> %.2f\n",
              broken, int(glass), dim, repaired);
  SLRR_REQUIRE(broken == 0.f && !glass && find(part_car_lights(false), CarLightKind::Headlight));
  SLRR_REQUIRE(std::fabs(dim - part_lamp_power(.4f)) < 1e-6f && dim > .4f && dim < 1.f);
  SLRR_REQUIRE(repaired == 1.f && part_lamp_power(kLampBrokenCondition) == 0.f &&
               part_lamp_power(kLampFullCondition) == 1.f);
  // The same rule for brake lights, through tear.
  const auto *taillight = find(part_car_lights(true), CarLightKind::Brake);
  SLRR_REQUIRE(taillight);
  Object *const torn_tail = taillight->part;
  f.vm.natives().invoke("java.game.parts.Part", "setTear", "(F)V", torn_tail,
                        {Value::make_float(.1f)});
  lit = part_car_lights(true);
  std::printf("CAR LIGHTS taillight torn to 0.1: %zu brake lights\n", count(lit, CarLightKind::Brake));
  SLRR_REQUIRE(count(lit, CarLightKind::Brake) == 1 && find(lit, CarLightKind::Brake)->part != torn_tail);
  SLRR_REQUIRE(!part_lamp_lit(torn_tail, 1u, false));

  // A headlight torn off and lying loose on the ground gives no light.
  SLRR_REQUIRE(part_tear_off(right.part));
  for (int i = 0; i < 60; ++i)
    part_physics_advance(1. / 60.);
  all = part_car_lights(false);
  lit = part_car_lights(true);
  std::printf("CAR LIGHTS remaining headlight torn off: %zu headlights reported (%zu lit)\n",
              count(all, CarLightKind::Headlight), count(lit, CarLightKind::Headlight));
  SLRR_REQUIRE(count(all, CarLightKind::Headlight) == 0 && count(lit, CarLightKind::Headlight) == 0);
  SLRR_REQUIRE(!part_lamp_lit(right.part, 4u, false));
  SLRR_REQUIRE(part_destroy(right.part));  // still a live (loose) part until now
  // A car taken apart reports no light.
  SLRR_REQUIRE(part_destroy(car));
  SLRR_REQUIRE(part_car_lights(false).empty());

  scene_instances_select(nullptr, nullptr, false);
  scene_instances_set_resources(nullptr);
  scene_instances_enable_static_floor(false);
  gameref_clear();
  rpk_set_active(nullptr);
  std::puts("PASS car lights belong to their headlight and brake light parts");
}
int main() {
  const standalone::Test tests[] = {stock_baiern_lights};
  return standalone::run("car_lights", tests);
}
