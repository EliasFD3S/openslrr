#include "test_harness.hpp"

#include "slrr/pedestrian_lines.hpp"
#include "slrr/format/walk.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void append_run(std::vector<uint8_t>& bytes, float x, float z0, float z1) {
  append_u32(bytes, 23);
  append_u32(bytes, 4);
  for (int i = 0; i < 5; ++i) {
    const float z = z0 + (z1 - z0) * static_cast<float>(i) / 4.f;
    append_f32(bytes, x);
    append_f32(bytes, 0);
    append_f32(bytes, z);
    append_f32(bytes, 0);
    append_f32(bytes, 0);
    append_f32(bytes, z1 >= z0 ? 1.f : -1.f);
  }
  append_f32(bytes, x);
  append_f32(bytes, z0);
  append_f32(bytes, x);
  append_f32(bytes, z1);
}

void append_span(std::vector<uint8_t>& bytes, float z) {
  append_u32(bytes, 22);
  append_f32(bytes, 0);
  append_f32(bytes, z);
  append_f32(bytes, 2);
  append_f32(bytes, z);
  append_u32(bytes, 1);
  for (int i = 0; i < 4; ++i) append_u32(bytes, 1);
}

std::string nibble(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.resize(bytes.size() * 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = static_cast<char>('A' + (bytes[i] >> 4));
    out[i * 2 + 1] = static_cast<char>('A' + (bytes[i] & 15));
  }
  return out;
}

std::string corridor() {
  std::vector<uint8_t> bytes;
  append_u32(bytes, 0);
  for (int i = 0; i < 4; ++i) append_f32(bytes, 0);
  append_f32(bytes, 2);
  append_u32(bytes, 3);
  append_run(bytes, 0, 0, 80);
  append_u32(bytes, 0xffffffffu);
  append_run(bytes, 2, 0, 80);
  append_u32(bytes, 0xffffffffu);
  append_span(bytes, 0);
  append_u32(bytes, 0xffffffffu);
  append_span(bytes, 80);
  const uint32_t length = static_cast<uint32_t>(bytes.size() - 4);
  bytes[0] = static_cast<uint8_t>(length);
  bytes[1] = static_cast<uint8_t>(length >> 8);
  bytes[2] = static_cast<uint8_t>(length >> 16);
  bytes[3] = static_cast<uint8_t>(length >> 24);
  return "0,-1,0,3,1,80 1;" + nibble(bytes);
}

std::filesystem::path find_game_file(const std::filesystem::path& relative) {
  std::filesystem::path cursor = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const std::filesystem::path candidate = cursor / "game" / relative;
    if (std::filesystem::exists(candidate)) return candidate;
    if (!cursor.has_parent_path() || cursor == cursor.parent_path()) break;
    cursor = cursor.parent_path();
  }
  return {};
}

}  // namespace

SLRR_TEST(pedestrian_follows_corridor) {
  slrr::PedestrianLines lines;
  SLRR_REQUIRE(lines.load(corridor()));
  SLRR_REQUIRE(lines.zones() == 1);
  std::vector<slrr::WalkRow> rows;
  std::string error;
  SLRR_REQUIRE(slrr::walk_read(corridor(), &rows, &error));
  SLRR_REQUIRE(rows.size() == 1);
  SLRR_REQUIRE(rows[0].records[0].directions.size() == 5);
  SLRR_REQUIRE(rows[0].records[0].directions[0].z == 1.f);
  uint32_t rng = 1;
  slrr::PedestrianWalker walker;
  SLRR_REQUIRE(lines.spawn(1.f, 0.f, 40.f, 50.f, 0.f, rng, walker));
  SLRR_REQUIRE(walker.x > 0.05f && walker.x < 1.95f);
  const float z0 = walker.z;
  const int sign = walker.sign;
  for (int i = 0; i < 20; ++i) lines.step(walker, 0.05f, 1000.f, 0.f, 1000.f, rng);
  SLRR_REQUIRE(walker.x > 0.05f && walker.x < 1.95f);
  SLRR_REQUIRE((walker.z - z0) * static_cast<float>(sign) > 0.4f);
  SLRR_REQUIRE(!walker.transitioning);
  const float blocked[2] = {walker.x, walker.z};
  slrr::PedestrianWalker other;
  SLRR_REQUIRE(lines.spawn(1.f, 0.f, 40.f, 50.f, 0.f, rng, other, blocked, 1));
  SLRR_REQUIRE(std::hypot(other.x - walker.x, other.z - walker.z) >= 2.f);
}

SLRR_TEST(pedestrian_city_walk_has_corridors) {
  const std::filesystem::path path = find_game_file("maps/city/ai/city.walk");
  if (path.empty()) return;
  std::ifstream in(path, std::ios::binary);
  SLRR_REQUIRE(in.good());
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  slrr::PedestrianLines lines;
  SLRR_REQUIRE(lines.load(text));
  SLRR_REQUIRE(lines.zones() == 788);
  uint32_t rng = 2;
  slrr::PedestrianWalker walker;
  SLRR_REQUIRE(lines.spawn(-38.f, 5.f, -125.f, 80.f, 0.f, rng, walker));
  const float x0 = walker.x, z0 = walker.z;
  for (int i = 0; i < 20; ++i) lines.step(walker, 0.05f, 1000.f, 0.f, 1000.f, rng);
  SLRR_REQUIRE(std::hypot(walker.x - x0, walker.z - z0) > 0.3f);
}
