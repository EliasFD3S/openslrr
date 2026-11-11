#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/physics_telemetry.hpp"
#include "slrr/scene_instances.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

slrr::physics_telemetry::Identity identity() {
  slrr::physics_telemetry::Identity id;
  id.scenario = "hermetic-test";
  id.vehicle = "test-vehicle";
  id.content_hash = "fnv1a64:0123456789abcdef";
  id.config_hash = std::nullopt;
  return id;
}

slrr::physics_telemetry::EntityState entity(std::uint64_t id = 1) {
  slrr::physics_telemetry::EntityState entity;
  entity.entity = id;
  entity.resource = 0x1234;
  entity.active = true;
  entity.available = true;
  entity.vehicle.mass = 1000;
  entity.vehicle.position = {1, 2, 3};
  entity.vehicle.dynamics_ticks = 2;
  return entity;
}

void one_tick(slrr::physics_telemetry::Session& session) {
  session.begin_frame(0, 1.0 / 120.0, 1.0);
  session.set_simulation_time(1.0 / 120.0, 0);
  session.note_physics_time(1.0 / 120.0, 0);
  session.record_tick(0, 1.0 / 60.0, {entity()});
  session.end_frame();
}

} // namespace

SLRR_TEST(physics_telemetry_serialization_nulls_and_bounds) {
  using namespace slrr::physics_telemetry;
  Limits limits;
  limits.max_ticks = 1;
  limits.max_input_transitions = 1;
  std::string error;
  Session session(identity(), limits, error);
  SLRR_REQUIRE(session.good());
  auto value = entity();
  value.vehicle.position.y = std::numeric_limits<double>::quiet_NaN();
  value.vehicle.wheels[0].contact_impulse = std::numeric_limits<double>::infinity();
  std::vector<EntityState> entities(65, value);
  for (std::size_t i = 0; i < entities.size(); ++i) entities[i].entity = i + 1;
  entities[0].available = false;
  entities[0].active = false;
  session.begin_frame(7, 0.01, 2.0);
  session.set_simulation_time(0.02, 0);
  session.note_physics_time(0.02, 0);
  InputTransition input;
  input.timestamp_us = 123;
  input.sequence = 1;
  input.device = 0;
  input.control = 200;
  input.value = 1;
  session.record_input(input);
  session.record_tick(9, 1.0 / 60.0, entities);
  session.record_tick(10, 1.0 / 60.0, {entity()});
  InputTransition extra = input;
  extra.sequence = 2;
  session.record_input(extra);
  session.end_frame();
  SLRR_REQUIRE(session.shutdown(error));
  const std::string json = session.output();
  SLRR_REQUIRE(json.find("\"type\":\"run\"") != std::string::npos);
  SLRR_REQUIRE(json.find("\"position\":[1,null,3]") != std::string::npos);
  SLRR_REQUIRE(json.find("\"contact_impulse\":null") != std::string::npos);
  SLRR_REQUIRE(json.find("\"dropped_ticks\":1") != std::string::npos);
  SLRR_REQUIRE(json.find("\"dropped_inputs\":1") != std::string::npos);
  SLRR_REQUIRE(json.find("\"dropped_entities\":1") != std::string::npos);
  SLRR_REQUIRE(json.find("\"truncated\":true") != std::string::npos);
  SLRR_REQUIRE(json.find("nan") == std::string::npos);
  SLRR_REQUIRE(json.find("inf") == std::string::npos);
  ReplayInfo info;
  SLRR_REQUIRE(validate_replay(json, &info, error));
  SLRR_REQUIRE(info.ticks == 1);
  SLRR_REQUIRE(info.frames == 1);
  SLRR_REQUIRE(info.inputs == 1);
  SLRR_REQUIRE(json.find("{\"entity\":1,\"resource\":4660,\"active\":false,\"available\":false}") != std::string::npos);

  Limits byte_limits;
  byte_limits.max_file_bytes = 5120;
  Session byte_bounded(identity(), byte_limits, error);
  SLRR_REQUIRE(byte_bounded.good());
  byte_bounded.begin_frame(0, 0.1, 1.0);
  for (std::uint64_t tick = 0; tick < 8; ++tick)
    byte_bounded.record_tick(tick, 1.0 / 60.0, entities);
  byte_bounded.end_frame();
  SLRR_REQUIRE(byte_bounded.shutdown(error));
  SLRR_REQUIRE(byte_bounded.output().size() <= byte_limits.max_file_bytes);
  SLRR_REQUIRE(byte_bounded.output().find("\"truncated\":true") != std::string::npos);
  SLRR_REQUIRE(validate_replay(byte_bounded.output(), nullptr, error));
}

SLRR_TEST(physics_telemetry_render_partition_and_lag_comparison) {
  using namespace slrr::physics_telemetry;
  const std::string fixed = deterministic_scenario(0);
  const std::string bursty = deterministic_scenario(1);
  const std::string stall = deterministic_scenario(2);
  SLRR_REQUIRE(!fixed.empty());
  SLRR_REQUIRE(!bursty.empty());
  SLRR_REQUIRE(!stall.empty());
  ReplayInfo a, b, c;
  std::string error;
  SLRR_REQUIRE(validate_replay(fixed, &a, error));
  SLRR_REQUIRE(validate_replay(bursty, &b, error));
  SLRR_REQUIRE(validate_replay(stall, &c, error));
  SLRR_REQUIRE(a.ticks == 60);
  SLRR_REQUIRE(b.ticks == a.ticks);
  SLRR_REQUIRE(c.ticks < a.ticks);
  const auto match = compare_replay(fixed, bursty);
  SLRR_REQUIRE(match.error.empty());
  SLRR_REQUIRE(match.equal);
  const auto lag = compare_replay(fixed, stall);
  SLRR_REQUIRE(lag.error.empty());
  SLRR_REQUIRE(!lag.equal);
  SLRR_REQUIRE(lag.report_json.find("ticks.length") != std::string::npos);
  ProbeResult probe;
  SLRR_REQUIRE(run_deterministic_probe({}, probe, error));
  SLRR_REQUIRE(probe.partition_equal);
  SLRR_REQUIRE(!probe.lag_equal);
  SLRR_REQUIRE(probe.partition_ticks == 60);
}


SLRR_TEST(physics_contact_identity_spawn_serialization_and_missing_data) {
  using namespace slrr::physics_telemetry;
  std::string error;
  Session session(identity(), Limits{}, error);
  SLRR_REQUIRE(session.good());
  auto value = entity();
  value.spawn_pose = SpawnPose{{4, 5, 6}, {0.1, 0.2, 0.3, 0.9}};
  value.vehicle.contact_count = 2;
  auto& contact = value.vehicle.contacts[0];
  contact.kind = 2;
  contact.wheel = -1;
  contact.identity.resource = 0x1020304;
  contact.identity.mesh = 0x5060708;
  contact.identity.triangle = 91;
  contact.identity.material = 17;
  contact.point = {1, 2, 3};
  contact.normal = {0, 1, 0};
  contact.penetration = 0.025;
  contact.impulse = 42.5;
  value.vehicle.contacts[1].kind = 1;
  value.vehicle.contacts[1].wheel = 3;
  value.vehicle.dropped_contacts = 4;
  session.record_tick(1, 1.0 / 60.0, {value});
  SLRR_REQUIRE(session.shutdown(error));
  const auto json = session.output();
  SLRR_REQUIRE(json.find("\"version\":2") != std::string::npos);
  SLRR_REQUIRE(json.find("\"spawn_pose\":{\"position\":[4,5,6]") != std::string::npos);
  SLRR_REQUIRE(json.find("\"resource\":16909060") != std::string::npos);
  SLRR_REQUIRE(json.find("\"mesh\":84281096") != std::string::npos);
  SLRR_REQUIRE(json.find("\"triangle\":91") != std::string::npos);
  SLRR_REQUIRE(json.find("\"material\":17") != std::string::npos);
  SLRR_REQUIRE(json.find("\"penetration\":0.025") != std::string::npos);
  SLRR_REQUIRE(json.find("\"impulse\":42.5") != std::string::npos);
  SLRR_REQUIRE(json.find("\"resource\":null,\"mesh\":null,\"triangle\":null,\"material\":null") != std::string::npos);
  SLRR_REQUIRE(json.find("\"dropped_contacts\":4") != std::string::npos);
  ReplayInfo info;
  SLRR_REQUIRE(validate_replay(json, &info, error));
  const auto same = compare_replay(json, json);
  SLRR_REQUIRE(same.equal);
}

SLRR_TEST(physics_contact_observation_is_bounded_and_materials_are_authoritative) {
  slrr::physics::VehicleDynamics dynamics;
  for (std::uint32_t i = 0; i < 18; ++i) {
    slrr::physics::ContactTelemetry contact;
    contact.kind = 2;
    contact.identity.resource = i;
    dynamics.observe_contact(contact);
  }
  const auto snapshot = dynamics.telemetry_snapshot();
  SLRR_REQUIRE(snapshot.contact_count == slrr::physics::VehicleTelemetry::max_contacts);
  SLRR_REQUIRE(snapshot.dropped_contacts == 2);
  SLRR_REQUIRE(snapshot.contacts[0].identity.resource == 0u);
  SLRR_REQUIRE(snapshot.contacts[15].identity.resource == 15u);

  slrr::ScxGeometry geometry;
  slrr::ScxGroup group;
  group.index_start = 3;
  group.index_count = 6;
  group.material_type = 77;
  geometry.groups.push_back(group);
  SLRR_REQUIRE(slrr::scene_mesh_material(geometry, 1) == 77u);
  SLRR_REQUIRE(slrr::scene_mesh_material(geometry, 2) == 77u);
  SLRR_REQUIRE(!slrr::scene_mesh_material(geometry, 0));
  SLRR_REQUIRE(!slrr::scene_mesh_material(geometry, 3));
  SLRR_REQUIRE(!slrr::scene_mesh_material(geometry, std::nullopt));
}

SLRR_TEST(physics_telemetry_mismatch_and_tolerance_report) {
  using namespace slrr::physics_telemetry;
  const std::string left = deterministic_scenario(0);
  std::string right = left;
  const std::string needle = "\"mass\":1100";
  const auto position = right.find(needle);
  SLRR_REQUIRE(position != std::string::npos);
  right.replace(position, needle.size(), "\"mass\":1101");
  const auto result = compare_replay(left, right);
  SLRR_REQUIRE(result.error.empty());
  SLRR_REQUIRE(!result.equal);
  SLRR_REQUIRE(result.report_json.find("chassis.mass") != std::string::npos);
  SLRR_REQUIRE(result.report_json.find("\"difference\":1") != std::string::npos);
  const auto tolerant = compare_replay(left, right, 1.0);
  SLRR_REQUIRE(tolerant.error.empty());
  SLRR_REQUIRE(tolerant.equal);
}

SLRR_TEST(physics_telemetry_malformed_replay_inputs) {
  using namespace slrr::physics_telemetry;
  const std::string valid = deterministic_scenario(0);
  std::vector<std::string> malformed = {
      {}, "{", "null\n", valid.substr(0, valid.find("\"type\":\"summary\"")),
      valid + "\"type\":\"summary\"}\n"};
  std::string duplicate = valid;
  duplicate.insert(duplicate.find("\"type\":\"tick\""),
                   "\"tick\":0,\"tick\":1,");
  malformed.push_back(duplicate);
  std::string trailing = valid;
  trailing += "true\n";
  malformed.push_back(trailing);
  std::string nonfinite = valid;
  const auto mass = nonfinite.find("\"mass\":1100");
  SLRR_REQUIRE(mass != std::string::npos);
  nonfinite.replace(mass, 11, "\"mass\":NaN");
  malformed.push_back(nonfinite);
  std::string unsorted = valid;
  const auto first_entity = unsorted.find("\"entity\":1");
  SLRR_REQUIRE(first_entity != std::string::npos);
  unsorted.replace(first_entity, 10, "\"entity\":0");
  malformed.push_back(unsorted);
  malformed.emplace_back(max_record_bytes + 1, 'x');
  for (const auto& input : malformed) {
    ReplayInfo info;
    std::string error;
    SLRR_REQUIRE(!validate_replay(input, &info, error));
    SLRR_REQUIRE(!error.empty());
    const auto comparison = compare_replay(input, valid);
    SLRR_REQUIRE(!comparison.error.empty());
  }
}

SLRR_TEST(physics_telemetry_real_file_output_and_open_failure) {
  using namespace slrr::physics_telemetry;
  slrr::test::TemporaryDirectory temporary("physics-telemetry");
  const auto path = temporary.path() / "run.jsonl";
  std::string error;
  {
    Session session(path.string(), identity(), Limits{}, error);
    SLRR_REQUIRE(session.good());
    one_tick(session);
    SLRR_REQUIRE(session.flush(error));
    SLRR_REQUIRE(session.shutdown(error));
  }
  const std::string json = slrr::test::read_text_file(path);
  ReplayInfo info;
  SLRR_REQUIRE(validate_replay(json, &info, error));
  SLRR_REQUIRE(info.ticks == 1);
  SLRR_REQUIRE(json.find(temporary.path().string()) == std::string::npos);
  SLRR_REQUIRE(json.find("secret") == std::string::npos);
  SLRR_REQUIRE(json.find("password") == std::string::npos);
  SLRR_REQUIRE(json.find("0x7ff") == std::string::npos);

  Session failure(temporary.path().string(), identity(), Limits{}, error);
  SLRR_REQUIRE(!failure.good());
  SLRR_REQUIRE(!error.empty());

  const auto comparison_path = temporary.path() / "mismatch.json";
  const auto mismatch = compare_replay(json, deterministic_scenario(2));
  SLRR_REQUIRE(!mismatch.equal);
  SLRR_REQUIRE(write_comparison_report(comparison_path.string(), mismatch.report_json, error));
  const std::string report = slrr::test::read_text_file(comparison_path);
  SLRR_REQUIRE(report.find("\"equal\":false") != std::string::npos);
  SLRR_REQUIRE(report.find("mismatches") != std::string::npos);
}
