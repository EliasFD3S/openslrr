// Stock type-13 light resources, map placement registration and renderer
// selection for street lamps (#237). Uses the local game assets read-only.
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/light_resource.hpp"
#include "slrr/part_config.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/script_lights.hpp"
#include "standalone_test.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;

namespace {
bool close_to(float a, float b, float eps = 1e-4f) { return std::abs(a - b) < eps; }

SLRR_TEST(parser_keywords_and_comments) {
  const auto light = parse_light_resource(
      "type spot // thatis\r\n"
      "// type point\r\n"
      "diffuse 1.000 0.5 0.25 0.125 // warm\n"
      "  ambient 1 .1 .2 .3\n"
      "position 0 3 0\n"
      "direction 0.000 -1.000 0.000\n"
      "range 26.5 ; trailing\n"
      "attenuation 1.000 .03 0\n"
      "theta 1.896\nphi 2.02\nlit_dynamic 1\n"
      "attH_texture 0x80000000\n"
      "lf_ray_maxlenght 70.000\n"
      "unknown_keyword 1 2 3\n");
  SLRR_REQUIRE(light);
  SLRR_REQUIRE(light->type == LightResourceType::Spot);
  SLRR_REQUIRE(close_to(light->diffuse[0], .5f) && close_to(light->diffuse[1], .25f) &&
               close_to(light->diffuse[2], .125f));
  SLRR_REQUIRE(close_to(light->ambient[0], .1f) && close_to(light->ambient[2], .3f));
  SLRR_REQUIRE(close_to(light->position[1], 3.f) && close_to(light->direction[1], -1.f));
  SLRR_REQUIRE(close_to(light->range, 26.5f));
  SLRR_REQUIRE(close_to(light->attenuation[0], 1.f) &&
               close_to(light->attenuation[1], .03f) && light->attenuation[2] == 0.f);
  SLRR_REQUIRE(close_to(light->theta, 1.896f) && close_to(light->phi, 2.02f));
  SLRR_REQUIRE(light->lit_dynamic && light->has_flare);
  SLRR_REQUIRE(light_resource_lights_scene(light->type));

  const auto fake = parse_light_resource("type fake point\nrange 200\n");
  SLRR_REQUIRE(fake && fake->type == LightResourceType::FakePoint);
  SLRR_REQUIRE(!light_resource_lights_scene(fake->type));
  SLRR_REQUIRE(parse_light_resource("type fakepoint")->type ==
               LightResourceType::FakePoint);
  SLRR_REQUIRE(parse_light_resource("TYPE FakeSpot")->type ==
               LightResourceType::FakeSpot);
  SLRR_REQUIRE(!light_resource_lights_scene(LightResourceType::Directional));
  // A base class without a type line still parses its values.
  const auto base = parse_light_resource("diffuse 1 1 0 0\nrange 10\n");
  SLRR_REQUIRE(base && base->type == LightResourceType::None);
  SLRR_REQUIRE(!light_resource_lights_scene(base->type));
  SLRR_REQUIRE(parse_light_resource("type lava")->type == LightResourceType::Unknown);
}

SLRR_TEST(parser_rejects_malformed_values) {
  SLRR_REQUIRE(!parse_light_resource(""));
  SLRR_REQUIRE(!parse_light_resource("// type point\n; range 5\n"));
  SLRR_REQUIRE(!parse_light_resource(std::string_view("\0type point", 11)));
  const auto light = parse_light_resource(
      "type point\n"
      "range nan\nrange -4\nrange 12x\n"
      "attenuation 1 -1 0\nattenuation 1 2\n"
      "diffuse 1 2\nposition 1 2 inf\n");
  SLRR_REQUIRE(light && light->type == LightResourceType::Point);
  SLRR_REQUIRE(light->range == 0.f);
  SLRR_REQUIRE(light->attenuation == (std::array<float, 3>{1.f, 0.f, 0.f}));
  SLRR_REQUIRE(light->diffuse == (std::array<float, 3>{1.f, 1.f, 1.f}));
  SLRR_REQUIRE(light->position == (std::array<float, 3>{}));
  // Three values are r g b.
  SLRR_REQUIRE(close_to(parse_light_resource("diffuse .1 .2 .3")->diffuse[2], .3f));
}

struct Game {
  Fixture f;
  int32_t objects = 0;
  Game() {
    const auto root = fs::absolute("game");
    f.vm.set_game_root(root.string());
    f.packs.set_root(root.string());
    f.packs.open_all();
    const auto *pack = f.packs.pack_by_path("objects.rpk");
    SLRR_REQUIRE(pack);
    objects = pack->pack_id;
  }
  uint32_t object(uint32_t local) const {
    return RpkIndex::make_full_id(objects, local);
  }
};

SLRR_TEST(stock_light_resources) {
  Game g;
  SLRR_REQUIRE(is_light_resource(g.f.packs, g.object(0x1c7)));
  SLRR_REQUIRE(!is_light_resource(g.f.packs, g.object(0x18c))); // kislampa, typeof 8
  SLRR_REQUIRE(!load_light_resource(g.f.packs, g.object(0x18c)));
  const auto street = load_light_resource(g.f.packs, g.object(0x1c7));
  SLRR_REQUIRE(street && street->type == LightResourceType::Point);
  SLRR_REQUIRE(close_to(street->range, 500.f) && close_to(street->attenuation[1], .03f));
  SLRR_REQUIRE(close_to(street->diffuse[0], .972f) && close_to(street->diffuse[1], .888f) &&
               close_to(street->diffuse[2], .615f));
  SLRR_REQUIRE(street->has_flare);
  const auto fake = load_light_resource(g.f.packs, g.object(0x16));
  SLRR_REQUIRE(fake && fake->type == LightResourceType::FakePoint);
  // Every stock type-13 payload parses, and the types match the #235 survey.
  std::map<LightResourceType, int> types;
  int total = 0;
  for (int32_t id = 1; id <= g.f.packs.pack_count(); ++id) {
    const auto *pack = g.f.packs.pack_by_id(id);
    if (!pack) continue;
    for (const auto &entry : pack->entries) {
      if (entry.typeof_ != 13) continue;
      const auto parsed =
          load_light_resource(g.f.packs, RpkIndex::make_full_id(id, entry.type_id));
      SLRR_REQUIRE(parsed);
      ++types[parsed->type];
      ++total;
    }
  }
  std::printf("light resources=%d point=%d spot=%d fakepoint=%d fakespot=%d "
              "directional=%d untyped=%d unknown=%d\n",
              total, types[LightResourceType::Point], types[LightResourceType::Spot],
              types[LightResourceType::FakePoint], types[LightResourceType::FakeSpot],
              types[LightResourceType::Directional], types[LightResourceType::None],
              types[LightResourceType::Unknown]);
  SLRR_REQUIRE(total >= 89 && types[LightResourceType::Unknown] == 0);
  SLRR_REQUIRE(types[LightResourceType::Point] >= 2 &&
               types[LightResourceType::FakePoint] >= 6);
}

// Light `render` children counted straight from the ICFG text, independent
// of parse_part_config: `render <id>` lines whose RPK entry has typeof 13.
size_t light_children(const RpkIndex &rpk, uint32_t type) {
  const auto bytes = rpk.read_payload(type);
  if (bytes.size() < 8 || std::memcmp(bytes.data(), "ICFG", 4) != 0) return 0;
  std::string text(bytes.begin() + 8, bytes.end());
  size_t count = 0, at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line.compare(i, 6, "render") != 0) continue;
    i += 6;
    if (i >= line.size() || (line[i] != ' ' && line[i] != '\t')) continue;
    const auto id = static_cast<uint32_t>(std::strtoul(line.c_str() + i, nullptr, 0));
    if (is_light_resource(rpk, rpk.resolve_embedded_id(RpkIndex::pack_from_full(type), id)))
      ++count;
  }
  return count;
}

SLRR_TEST(valo_city_lamps_registered_with_their_shell) {
  Game g;
  scene_instances_set_resources(&g.f.packs);
  const auto pack = g.f.packs.open_lib("maps/city.rpk");
  const auto ground_id = RpkIndex::make_full_id(pack, 1);
  // Expected count: walk the same ground graph spawn_ground_shell walks.
  size_t expected = 0, city = 0, fake = 0, placements = 0;
  std::unordered_map<uint32_t, size_t> per_type;
  std::unordered_set<uint32_t> visited;
  std::vector<uint32_t> pending{ground_id};
  std::vector<GroundMapData::Placement> kislampa;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!visited.insert(id).second) continue;
    const auto map = scene_instances_ground_map(g.f.packs, id);
    pending.insert(pending.end(), map.exterior_maps.begin(), map.exterior_maps.end());
    for (const auto &placement : map.placements) {
      ++placements;
      auto [found, unseen] = per_type.try_emplace(placement.type);
      if (unseen) found->second = light_children(g.f.packs, placement.type);
      expected += found->second;
      if (placement.type == g.object(0x18c)) kislampa.push_back(placement);
    }
  }
  Object *root = g.f.vm.new_object("java.util.resource.GroundRef");
  gameref_upsert(root, GameRefKind::GroundRef)->type_id = static_cast<int32_t>(ground_id);
  spawn_ground_shell(g.f.vm, g.f.packs, root);
  const auto shells = gameref_get(root)->children;
  size_t registered = scenery_lights_count();
  std::array<float, 3> sample{};
  bool found_kislampa = kislampa.empty();
  for (const auto &[parent, list] : scenery_lights_all()) {
    SLRR_REQUIRE(std::find(shells.begin(), shells.end(), parent) != shells.end());
    for (const auto &light : *list) {
      if (light.lights_scene) {
        SLRR_REQUIRE(light.resource == g.object(0x1c7));
        SLRR_REQUIRE(light.group & kLightGroupCity);
        SLRR_REQUIRE(close_to(light.range, 500.f));
        ++city;
      } else {
        ++fake;
      }
      if (!found_kislampa && light.resource == g.object(0x1c7)) {
        // kislampa: `render 0x1c7 0.0 4.3 0.0` under the placement pose.
        const auto pose = part_pose_matrix({kislampa[0].position, kislampa[0].rotation});
        sample = {pose[4] * 4.3f + pose[12], pose[5] * 4.3f + pose[13],
                  pose[6] * 4.3f + pose[14]};
        found_kislampa = close_to(light.position[0], sample[0], 1e-3f) &&
                         close_to(light.position[1], sample[1], 1e-3f) &&
                         close_to(light.position[2], sample[2], 1e-3f);
      }
    }
  }
  std::printf("Valo City placements=%zu shells=%zu scenery lights=%zu expected=%zu "
              "scene-lighting=%zu glow-only=%zu kislampa=%zu\n",
              placements, shells.size(), registered, expected, city, fake,
              kislampa.size());
  SLRR_REQUIRE(registered == expected && registered > 0 && city > 0);
  SLRR_REQUIRE(found_kislampa);
  {
    // CPU cost of the per-frame selection over the whole city, eye at a
    // lamp (shell matrices as identity; the renderer passes the drawn ones).
    const auto all = scenery_lights_all();
    const auto &first = all.front().second->front();
    const std::array<float, 3> eye{first.position[0], first.position[1] - 3.f,
                                   -first.position[2]};
    const auto matrix = [](void *, float *m) {
      for (int i = 0; i < 16; ++i) m[i] = i % 5 == 0 ? 1.f : 0.f;
      return true;
    };
    size_t picked = 0;
    const auto start = std::chrono::steady_clock::now();
    constexpr int runs = 500;
    for (int i = 0; i < runs; ++i)
      picked += scenery_lights_select(eye, 64, kLightGroupCity, matrix).size();
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - start).count() / runs;
    std::printf("Valo City selection: %.1f us per frame, %zu lights picked\n", us,
                picked / runs);
    SLRR_REQUIRE(picked / runs > 12 && picked / runs <= 64);
  }
  // A second visit (cached props, new shell) attaches the same lights again.
  for (auto *child : shells) gameref_remove(child);
  SLRR_REQUIRE(scenery_lights_count() == 0);
  gameref_remove(root);
  Object *again = g.f.vm.new_object("java.util.resource.GroundRef");
  gameref_upsert(again, GameRefKind::GroundRef)->type_id = static_cast<int32_t>(ground_id);
  spawn_ground_shell(g.f.vm, g.f.packs, again);
  SLRR_REQUIRE(scenery_lights_count() == registered);
  // The ground shell owns its lights: removing it (or a reset) clears them.
  for (auto *child : std::vector<void *>(gameref_get(again)->children)) gameref_remove(child);
  gameref_remove(again);
  SLRR_REQUIRE(scenery_lights_count() == 0);
  scene_instances_set_resources(nullptr);
}

struct Registry {
  Object parent, hidden;
  Registry() {
    gameref_clear();
    gameref_upsert(&parent, GameRefKind::RenderRef);
    gameref_upsert(&hidden, GameRefKind::RenderRef);
  }
  ~Registry() { gameref_clear(); }
};

bool identity(void *parent, float *m, void *hidden) {
  if (parent == hidden) return false;
  for (int i = 0; i < 16; ++i) m[i] = i % 5 == 0 ? 1.f : 0.f;
  return true;
}

SLRR_TEST(selection_budget_groups_and_fade) {
  Registry r;
  auto lights = std::make_shared<std::vector<SceneryLight>>();
  for (int i = 0; i < 20; ++i) {
    SceneryLight light;
    light.position = {10.f * i, 5.f, -3.f}; // script Z
    light.range = 500.f;
    light.group = kLightGroupCity;
    light.lights_scene = true;
    lights->push_back(light);
  }
  SceneryLight fake = lights->front();
  fake.lights_scene = false;
  lights->push_back(fake);
  SceneryLight brake = lights->front();
  brake.group = 1;
  lights->push_back(brake);
  SceneryLight distant = lights->front();
  distant.position = {0.f, 5.f, -700.f};
  lights->push_back(distant);
  SLRR_REQUIRE(!scenery_lights_attach(&r.parent, nullptr));
  SLRR_REQUIRE(!scenery_lights_attach(nullptr, lights));
  SLRR_REQUIRE(scenery_lights_attach(&r.parent, lights));
  SLRR_REQUIRE(scenery_lights_attach(&r.hidden, lights));
  SLRR_REQUIRE(scenery_lights_count() == 2 * lights->size());
  const auto matrix = [&](void *parent, float *m) { return identity(parent, m, &r.hidden); };
  // Day: the city group is off.
  SLRR_REQUIRE(scenery_lights_select({0, 0, 0}, 12, 0, matrix).empty());
  const auto picks = scenery_lights_select({0, 0, 0}, 12, kLightGroupCity, matrix);
  SLRR_REQUIRE(picks.size() <= 12 && picks.size() >= 9);
  for (size_t i = 0; i < picks.size(); ++i) {
    // Nearest first; render space flips script Z.
    SLRR_REQUIRE(close_to(picks[i].position[0], 10.f * i) && close_to(picks[i].position[2], 3.f));
    SLRR_REQUIRE(picks[i].source->lights_scene && picks[i].source->group == kLightGroupCity);
    SLRR_REQUIRE(close_to(picks[i].range, kSceneryLightMaxRange));
    if (i) SLRR_REQUIRE(picks[i].intensity <= picks[i - 1].intensity);
  }
  SLRR_REQUIRE(close_to(picks.front().intensity, kSceneryLightIntensity));
  // The 13th light (120 m) is the cut: the 12th (110 m) has faded part way.
  SLRR_REQUIRE(picks.back().intensity < kSceneryLightIntensity);
  // Budget zero or no matrix: nothing.
  SLRR_REQUIRE(scenery_lights_select({0, 0, 0}, 0, kLightGroupCity, matrix).empty());
  SLRR_REQUIRE(scenery_lights_select({0, 0, 0}, 12, kLightGroupCity, nullptr).empty());
  // Nothing beyond the distance limit.
  for (const auto &pick :
       scenery_lights_select({0, 0, 350}, 12, kLightGroupCity, matrix))
    SLRR_REQUIRE(pick.distance < kSceneryLightMaxDistance);
  // Moving the eye changes intensities continuously: a light never pops.
  float worst = 0.f;
  std::map<const SceneryLight *, float> previous;
  for (float x = 0.f; x <= 200.f; x += .05f) {
    std::map<const SceneryLight *, float> now;
    for (const auto &pick : scenery_lights_select({x, 0, 0}, 4, kLightGroupCity, matrix))
      now[pick.source] = pick.intensity;
    if (x > 0.f) {
      for (const auto &[light, value] : now)
        worst = std::max(worst, std::abs(value - (previous.contains(light) ? previous[light] : 0.f)));
      for (const auto &[light, value] : previous)
        if (!now.contains(light)) worst = std::max(worst, value);
    }
    previous = std::move(now);
  }
  std::printf("largest per-step intensity change over 200 m: %.4f\n", worst);
  SLRR_REQUIRE(worst < .01f);
  // Removing the parent removes its lights.
  gameref_remove(&r.parent);
  SLRR_REQUIRE(scenery_lights_count() == lights->size());
  script_lights_clear();
  SLRR_REQUIRE(scenery_lights_count() == 0);
}
SLRR_TEST(selection_frustum_and_distance) {
  Registry r;
  auto lights = std::make_shared<std::vector<SceneryLight>>();
  // A straight street: a lamp every 30 m on both sides, 1.2 km long, centred
  // on the eye, so half of it is behind the camera.
  for (int i = -20; i <= 20; ++i)
    for (float side : {-8.f, 8.f}) {
      SceneryLight light;
      light.position = {side, 7.8f, -30.f * i}; // render z = 30 * i
      light.range = 500.f;
      light.group = kLightGroupCity;
      light.lights_scene = true;
      lights->push_back(light);
    }
  SLRR_REQUIRE(scenery_lights_attach(&r.parent, lights));
  const auto matrix = [&](void *parent, float *m) { return identity(parent, m, &r.hidden); };
  // Looking down +z (render): one plane z >= 0, the others accept everything.
  SceneryLightFrustum frustum{};
  for (auto &plane : frustum) plane = {0.f, 0.f, 0.f, 1.f};
  frustum[0] = {0.f, 0.f, 2.f, 0.f}; // not normalised on purpose
  const auto all = scenery_lights_select({0, 2, 0}, 64, kLightGroupCity, matrix);
  const auto ahead = scenery_lights_select({0, 2, 0}, 64, kLightGroupCity, matrix, &frustum);
  size_t behind_spheres = 0;
  for (const auto &pick : ahead) {
    // Only lamps whose 45 m sphere reaches the visible half-space.
    SLRR_REQUIRE(pick.position[2] >= -kSceneryLightMaxRange);
    if (pick.position[2] < 0.f) ++behind_spheres;
  }
  std::printf("street: %zu lamps without frustum, %zu with; farthest ahead %.0f m\n",
              all.size(), ahead.size(), ahead.empty() ? 0.f : ahead.back().distance);
  // Without the frustum the budget is spent on both directions (lamps tied
  // with the cut fade to 0 and are dropped).
  SLRR_REQUIRE(all.size() > 56 && all.size() <= 64 && ahead.size() < all.size());
  SLRR_REQUIRE(behind_spheres == 2 * 1); // the pair at z = -30 still reaches z >= 0
  // The whole visible street fits: every lamp ahead out to the distance limit.
  size_t expected = 0;
  for (const auto &light : *lights)
    if (-light.position[2] >= -kSceneryLightMaxRange &&
        std::hypot(light.position[0], light.position[1] - 2.f, light.position[2]) <
            kSceneryLightMaxDistance)
      ++expected;
  SLRR_REQUIRE(ahead.size() == expected);
  // Near lamps are at full power; the last ones fade to ~0 before 500 m.
  SLRR_REQUIRE(close_to(ahead.front().intensity, kSceneryLightIntensity));
  SLRR_REQUIRE(ahead.back().distance > 400.f && ahead.back().intensity < .05f);
  // Driving along the street: a lamp entering from far ahead has no power.
  std::map<const SceneryLight *, float> previous;
  float entering = 0.f;
  for (float z = -100.f; z <= 100.f; z += .1f) {
    auto moved = frustum;
    moved[0][3] = -2.f * z;
    std::map<const SceneryLight *, float> now;
    for (const auto &pick : scenery_lights_select({0, 2, z}, 64, kLightGroupCity, matrix, &moved))
      now[pick.source] = pick.intensity;
    if (z > -100.f)
      for (const auto &[light, value] : now)
        if (!previous.contains(light)) {
          // Entering from far ahead must be faded out; entering from behind
          // (sphere crossing the view plane) lights nothing visible yet.
          if (light->position[2] * -1.f > z + kSceneryLightMaxRange)
            entering = std::max(entering, value);
        }
    previous = std::move(now);
  }
  std::printf("largest power of a lamp entering far ahead: %.4f\n", entering);
  SLRR_REQUIRE(entering < .01f);
  script_lights_clear();
}
} // namespace

int main() {
  const standalone::Test tests[] = {
      parser_keywords_and_comments, parser_rejects_malformed_values,
      stock_light_resources, valo_city_lamps_registered_with_their_shell,
      selection_budget_groups_and_fade, selection_frustum_and_distance};
  return standalone::run("scenery_lights", tests);
}
