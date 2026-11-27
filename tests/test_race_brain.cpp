#include "race_fixture.hpp"
#include <cmath>
using namespace slrr;
void spline_wait_and_arbitration() {
  RaceFixture f;
  auto *car = f.car();
  Object brain;
  gameref_upsert(&brain);
  race_brain_command(f.packs, &brain,
                     "controllable " + std::to_string(gameref_make_live(car)));
  std::ifstream in(fs::absolute("game/maps/ROC_Track/race.spl"));
  RoadPoint start;
  SLRR_REQUIRE(bool(in >> start[0] >> start[1] >> start[2]));
  gameref_set_matrix(car, start[0], start[1], start[2], 0, 0, 0);
  RaceDriveInput input;
  race_brain_command(f.packs, &brain, "AI_BeginRace 0.5");
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle == 0 && input.brake == 1);
  SLRR_REQUIRE(race_brain_command(f.packs, &brain,
                                  "AI_spline 5 maps/ROC_Track/race.spl 0"));
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle > 0 && input.start);
  SLRR_REQUIRE(!race_brain_command(f.packs, &brain, "AI_unknown"));
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle > 0);
  race_brain_command(f.packs, &brain, "AI_suspend");
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle == 0 && input.brake == 1);
  gameref_remove(&brain);
  SLRR_REQUIRE(!race_drive_input(car, start, 0, 0, .02f, input));
  part_destroy(car);
  std::puts("PASS ROC closed spline starts; countdown/suspend hold; command "
            "order and destruction");
}
void city_route_drives_and_halts() {
  RaceFixture f;
  auto *car = f.car();
  Object brain;
  gameref_upsert(&brain);
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph && !graph->edges().empty());
  const auto &e = graph->edges().front();
  auto start = e.points.front(), finish = e.points.back();
  gameref_set_matrix(car, start[0], start[1], start[2], 0, 0, 0);
  race_brain_command(f.packs, &brain,
                     "controllable " + std::to_string(gameref_make_live(car)));
  std::string command = "AI_race " + std::to_string(finish[0]) + "," +
                        std::to_string(finish[1]) + "," +
                        std::to_string(finish[2]) + " 0";
  race_brain_command(f.packs, &brain, command);
  RaceDriveInput input;
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle > 0);
  auto direction = race_last_route(&f.ground)->direction;
  float away = std::atan2(direction[0], direction[2]);
  SLRR_REQUIRE(race_drive_input(car, start, away, 20, .02f, input) &&
               input.throttle == 0 && input.brake > .9f &&
               std::fabs(input.steer) > .05f);
  race_brain_command(f.packs, &brain, "AI_GoToTraffic");
  race_halt_path(f.packs, &f.ground, start, finish);
  // Stopped traffic holds above native traffic's half-brake hold threshold.
  SLRR_REQUIRE(race_drive_input(car, start, 0, 0, .02f, input) &&
               input.throttle == 0 && input.brake > .5f);
  race_clear_halts(&f.ground);
  gameref_remove(&brain);
  part_destroy(car);
  std::puts("PASS city route supplies throttle; traffic respects race path "
            "exclusions");
}
void preview_bend_brakes() {
  RaceFixture f;
  auto *car = f.car();
  gameref_set_matrix(car, 0, 0, 0, 0, 0, 0);
  Object brain;
  gameref_upsert(&brain);
  std::ofstream(f.root / "bend.spl")
      << "0 0 0 0 0 -1\n0 0 -30 0 0 -1\n10 0 -40 1 0 0\n40 0 -40 1 0 0\n";
  RpkIndex routes;
  routes.set_root(f.root.string());
  race_brain_command(routes, &brain,
                     "controllable " + std::to_string(gameref_make_live(car)));
  race_brain_command(routes, &brain, "AI_spline 5 bend.spl 0");
  RaceDriveInput input;
  SLRR_REQUIRE(race_drive_input(car, {0, 0, 0}, 0, 20, .02f, input) &&
               input.throttle == 0 && input.brake > .1f);
  for (int i = 0; i < 10; ++i)
    race_drive_input(car, {0, 0, -28}, 0, 5, .02f, input);
  SLRR_REQUIRE(std::fabs(input.steer) > .1f);
  gameref_remove(&brain);
  part_destroy(car);
  std::puts("PASS anticipation brakes before a sharp spline bend and turns at "
            "the bend");
}
void night_finish_runout() {
  RaceFixture f;
  auto *car = f.car();
  Object brain;
  gameref_upsert(&brain);
  auto graph = race_ground_roads(f.packs, &f.ground);
  const auto &e = graph->edges().front();
  auto start = e.points.front(), finish = e.points.back();
  gameref_set_matrix(car, start[0], start[1], start[2], 0, 0, 0);
  race_brain_command(f.packs, &brain,
                     "controllable " + std::to_string(gameref_make_live(car)));
  race_brain_command(f.packs, &brain,
                     "AI_race " + std::to_string(finish[0]) + "," +
                         std::to_string(finish[1]) + "," +
                         std::to_string(finish[2]) + " 0");
  race_brain_command(f.packs, &brain, "AI_NightRace");
  RoadRoute route = *race_last_route(&f.ground);
  auto approach = road_at(route, route.length - 5);
  gameref_set_matrix(car, approach[0], approach[1], approach[2], 0, 0, 0);
  race_brain_command(f.packs, &brain, "AI_stop");
  RaceDriveInput input;
  SLRR_REQUIRE(race_drive_input(car, approach, 0, 20, .02f, input) &&
               input.brake == 0 && input.throttle == 0);
  auto previous = route.points[route.points.size() - 2];
  auto direction =
      road_normalize({finish[0] - previous[0], 0, finish[2] - previous[2]});
  RoadPoint past = finish;
  for (int k = 0; k < 3; ++k)
    past[k] += 12 * direction[k];
  SLRR_REQUIRE(race_drive_input(car, past, 0, 20, .02f, input) &&
               input.brake == 1);
  gameref_remove(&brain);
  part_destroy(car);
  std::puts("PASS night finish stop event coasts across the finish plane "
            "before braking");
}
void traffic_blocked_recovery() {
  RaceFixture f;
  f.packs.open_all();
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  size_t edge = 0;
  for (; edge < graph->traffic_edges().size(); ++edge)
    if (!graph->traffic_edges()[edge].crossing &&
        graph->traffic_signal(edge, 0) == -1 &&
        !graph->traffic_edges()[edge].next.empty())
      break;
  SLRR_REQUIRE(edge < graph->traffic_edges().size());
  const auto &e = graph->traffic_edges()[edge];
  RoadRoute line;
  line.points = e.points;
  auto start = road_at(line, e.length * .5f),
       next = road_at(line, e.length * .5f + 1);
  float yaw = std::atan2(-(next[0] - start[0]), -(next[2] - start[2]));
  auto *car = f.car();
  gameref_set_matrix(car, start[0], start[1], start[2], yaw, 0, 0);
  race_set_traffic_mode(f.packs, car);
  RaceDriveInput input;
  bool reverse = false;
  for (int tick = 0; tick < 200; ++tick) {
    SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input));
    if (input.reverse) {
      reverse = true;
      SLRR_REQUIRE(input.throttle > 0 && input.brake == 0);
      break;
    }
  }
  SLRR_REQUIRE(reverse);
  auto *rear = f.car();
  gameref_set_matrix(rear, start[0] + std::sin(yaw) * 5, start[1],
                     start[2] + std::cos(yaw) * 5, yaw, 0, 0);
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input) &&
               !input.reverse && input.brake == 1);
  part_destroy(rear);
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input) &&
               input.reverse && input.throttle > 0);
  race_brain_command(f.packs, car, "AI_stop");
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input) &&
               !input.reverse && input.brake == 1);
  part_destroy(car);
  std::puts("PASS blocked traffic physically requests reverse recovery; stop "
            "cancels it");
}
void traffic_follows_peer_and_bot_takes_over() {
  RaceFixture f;
  f.packs.open_all();
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  size_t edge = 0;
  for (; edge < graph->traffic_edges().size(); ++edge)
    if (!graph->traffic_edges()[edge].crossing &&
        graph->traffic_signal(edge, 0) == -1 &&
        !graph->traffic_edges()[edge].next.empty() &&
        graph->traffic_edges()[edge].length > 40)
      break;
  SLRR_REQUIRE(edge < graph->traffic_edges().size());
  const auto &e = graph->traffic_edges()[edge];
  RoadRoute line;
  line.points = e.points;
  auto start = road_at(line, e.length * .25f),
       next = road_at(line, e.length * .25f + 1);
  float yaw = std::atan2(-(next[0] - start[0]), -(next[2] - start[2]));
  RoadPoint forward{-std::sin(yaw), 0, -std::cos(yaw)};
  auto *car = f.car();
  gameref_set_matrix(car, start[0], start[1], start[2], yaw, 0, 0);
  race_set_traffic_mode(f.packs, car);
  RaceDriveInput input;
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 5, .02f, input) &&
               input.throttle > 0);
  // A stopped car 7 m ahead holds the driver; it does not ram it.
  Object lead;
  RacePeer peer;
  peer.car = &lead;
  peer.forward = forward;
  for (int k = 0; k < 3; ++k)
    peer.position[k] = start[k] + 7 * forward[k];
  race_set_peers({peer});
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 5, .02f, input) &&
               input.throttle == 0 && input.brake > .9f);
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input) &&
               input.throttle == 0 && input.brake > .5f && !input.reverse);
  race_set_peers({});
  // The queued driver reacts .5-1.5 s after the way clears (bot.ts).
  int waited = 0;
  while (race_drive_input(car, start, yaw, 0, .02f, input) &&
         input.throttle == 0 && waited < 100)
    ++waited;
  SLRR_REQUIRE(input.throttle > 0 && waited >= 24 && waited < 80);
  // A Bot adopting the streamed car (City's woken police scout) replaces
  // the host traffic driver.
  Object brain;
  gameref_upsert(&brain);
  race_brain_command(f.packs, &brain,
                     "controllable " + std::to_string(gameref_make_live(car)));
  race_brain_command(f.packs, &brain, "AI_suspend");
  SLRR_REQUIRE(race_drive_input(car, start, yaw, 0, .02f, input) &&
               input.throttle == 0 && input.brake == 1);
  gameref_remove(&brain);
  SLRR_REQUIRE(!race_drive_input(car, start, yaw, 0, .02f, input));
  part_destroy(car);
  std::puts("PASS traffic waits behind a stopped car; a Bot's controllable "
            "replaces the traffic driver");
}
void road_speed_zones() {
  RaceFixture f;
  auto graph = race_ground_roads(f.packs, &f.ground);
  SLRR_REQUIRE(graph);
  int groups[2] = {0, 0};
  for (const auto &e : graph->traffic_edges()) {
    if (e.crossing || e.speed_group < 0 || e.next.empty())
      continue;
    ++groups[e.speed_group];
    RoadRoute line;
    line.points = e.points;
    const auto middle = road_at(line, e.length * .5f);
    const float limit = graph->speed_limit(middle);
    // The nearest curve may be a neighbouring lane of the other group.
    SLRR_REQUIRE(std::fabs(limit - 100 / 3.6f) < 1e-3f ||
                 std::fabs(limit - 35 / 3.6f) < 1e-3f);
  }
  SLRR_REQUIRE(groups[0] + groups[1] > 0);
  auto start = graph->traffic_edges().front().points.front();
  SLRR_REQUIRE(graph->speed_limit({start[0], start[1] + 500, start[2]}) == -1);
  std::printf("PASS road speed zones: %d rural and %d urban lanes; off-road -1\n",
              groups[0], groups[1]);
}
int main() {
  const standalone::Test tests[] = {road_speed_zones,
      traffic_blocked_recovery, spline_wait_and_arbitration,
      city_route_drives_and_halts, preview_bend_brakes, night_finish_runout,
      traffic_follows_peer_and_bot_takes_over};
  return standalone::run("race_brain", tests);
}
