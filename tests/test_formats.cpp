#include "test_harness.hpp"

#include "slrr/format/rest.hpp"
#include "slrr/format/trc.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void append_f32(std::vector<uint8_t>& bytes, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, 4);
  append_u32(bytes, bits);
}

void append_segment(std::vector<uint8_t>& bytes) {
  const float words[13] = {0, 1, 0, 10, 1, 0, 10, 0, 0, 10, 0, 0, 10};
  for (float word : words) append_f32(bytes, word);
  append_u32(bytes, 0xffffffffu);
  bytes.push_back(1);
}

std::vector<uint8_t> sample_trc() {
  std::vector<uint8_t> bytes;
  const uint32_t header[5] = {1, 1, 0, 1, 1};
  for (uint32_t word : header) append_u32(bytes, word);
  append_u32(bytes, 1);
  append_u32(bytes, 3);
  bytes.push_back(1);
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), 12, 0);
  append_segment(bytes);
  append_f32(bytes, 2);
  append_f32(bytes, 1);
  append_f32(bytes, 3);
  append_u32(bytes, 0);
  for (uint32_t ref : {3u, 1u, 0u}) append_u32(bytes, ref);
  for (int i = 0; i < 7; ++i) append_u32(bytes, 0);
  append_u32(bytes, 1);
  append_segment(bytes);
  append_u32(bytes, 1);
  append_u32(bytes, 0);
  bytes.push_back(1);
  for (int i = 0; i < 12; ++i) append_u32(bytes, 0);
  for (int i = 0; i < 4; ++i) append_u32(bytes, 0);
  append_f32(bytes, 13.888889f);
  append_f32(bytes, 0.9f);
  append_u32(bytes, 0xffffffffu);
  append_u32(bytes, 1);
  append_segment(bytes);
  return bytes;
}

}  // namespace

SLRR_TEST(rest_reads_spots_and_rejects_a_duplicate) {
  std::vector<slrr::RestSpot> spots;
  std::string error;
  SLRR_REQUIRE(slrr::rest_read("0,0,0,3,2,3 1;1,0\n0,0,0,1,1,1 2;1,1\n", &spots, &error));
  SLRR_REQUIRE(spots.size() == 2);
  SLRR_REQUIRE(spots[0].area == 1 && spots[0].index == 0 && spots[0].flag == 1);
  SLRR_REQUIRE(spots[1].area == 2 && spots[1].index == 1);
  SLRR_REQUIRE(!slrr::rest_read("0,0,0,3,2,3 1;1,0\n0,0,0,1,1,1 1;1,0\n", &spots, &error));
  SLRR_REQUIRE(!slrr::rest_read("0,0,0,3,2,3 1;0,0\n", &spots, &error));
}

SLRR_TEST(trc_reads_the_sample_network) {
  const std::vector<uint8_t> bytes = sample_trc();
  slrr::RoadNetwork network;
  std::string error;
  SLRR_REQUIRE(slrr::trc_read(bytes.data(), bytes.size(), &network, &error));
  SLRR_REQUIRE(network.nodes.size() == 1);
  SLRR_REQUIRE(network.nodes[0].position[0] == 2.f);
  SLRR_REQUIRE(network.nodes[0].position[1] == 1.f);
  SLRR_REQUIRE(network.nodes[0].position[2] == 3.f);
  SLRR_REQUIRE(network.nodes[0].links.size() == 1);
  SLRR_REQUIRE(network.nodes[0].links[0].target == 3);
  SLRR_REQUIRE(network.paths.size() == 1);
  SLRR_REQUIRE(network.paths[0].id == 2);
  SLRR_REQUIRE(network.paths[0].refs[0] == 3 && network.paths[0].refs[1] == 1 &&
               network.paths[0].refs[2] == 0);
  SLRR_REQUIRE(network.others.size() == 1);
  SLRR_REQUIRE(network.others[0].id == 3);
  SLRR_REQUIRE(network.others[0].tail.size() == 3);
  SLRR_REQUIRE(network.others[0].tail[2] == 0xffffffffu);
  const slrr::RoadSegment& segment = network.paths[0].segments[0];
  const slrr::RoadPoint at_start = slrr::trc_sample(segment, 0.f);
  const slrr::RoadPoint at_end = slrr::trc_sample(segment, 1.f);
  const slrr::RoadPoint at_mid = slrr::trc_sample(segment, 0.5f);
  SLRR_REQUIRE(at_start[0] == 0.f && at_start[1] == 1.f && at_start[2] == 0.f);
  SLRR_REQUIRE(at_end[0] == 10.f && at_end[1] == 1.f && at_end[2] == 0.f);
  SLRR_REQUIRE(std::fabs(at_mid[0] - 5.f) < 1e-4f);
  SLRR_REQUIRE(std::fabs(at_mid[1] - 1.f) < 1e-4f);
  slrr::RoadSegment bent = segment;
  bent.start_tangent[1] = 8.f;
  bent.end_tangent[1] = -8.f;
  const slrr::RoadPoint lifted = slrr::trc_sample(bent, 0.5f);
  SLRR_REQUIRE(std::fabs(lifted[0] - 5.f) < 1e-4f);
  SLRR_REQUIRE(std::fabs(lifted[1] - 3.f) < 1e-4f);
  SLRR_REQUIRE(!slrr::trc_read(bytes.data(), bytes.size() - 1, &network, &error));
  std::vector<uint8_t> version = bytes;
  version[0] = 2;
  SLRR_REQUIRE(!slrr::trc_read(version.data(), version.size(), &network, &error));
  std::vector<uint8_t> trailing = bytes;
  trailing.push_back(0);
  SLRR_REQUIRE(!slrr::trc_read(trailing.data(), trailing.size(), &network, &error));
}
