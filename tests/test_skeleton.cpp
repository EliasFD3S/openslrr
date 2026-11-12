#include "test_harness.hpp"

#include "slrr/scx_mesh.hpp"
#include "slrr/skeleton.hpp"
#include "slrr/format/bon.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<uint8_t>* bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes->push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void append_f32(std::vector<uint8_t>* bytes, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, 4);
  append_u32(bytes, bits);
}

std::filesystem::path find_game_file(const std::filesystem::path& relative) {
  std::filesystem::path cursor = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const std::filesystem::path candidate = cursor / "game" / relative;
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) return candidate;
    if (!cursor.has_parent_path() || cursor == cursor.parent_path()) break;
    cursor = cursor.parent_path();
  }
  return {};
}

const char* kChildFirst =
    "child\n"
    "1\n"
    "1, 0, 0\n"
    "0, 1, 0\n"
    "0, 0, 1\n"
    "4, 0, 0\n"
    "0, 1\n"
    "0, 0\n"
    "2, -1\n"
    "root\n"
    "-1\n"
    "1, 0, 0\n"
    "0, 1, 0\n"
    "0, 0, 1\n"
    "0, 5, 0\n"
    "0, 0\n"
    "0, 0\n"
    "0, 0\n";

}  // namespace

SLRR_TEST(bon_reads_the_same_record_without_a_bind) {
  slrr::Skeleton skeleton;
  std::string error;
  SLRR_REQUIRE(slrr::bon_read(kChildFirst, &skeleton, &error));
  SLRR_REQUIRE(skeleton.bones.size() == 2);
  SLRR_REQUIRE(skeleton.order.size() == 2);
  SLRR_REQUIRE(skeleton.inverse_bind.empty());
}

SLRR_TEST(skeleton_accepts_a_parent_declared_later) {
  slrr::Skeleton skeleton;
  std::string error;
  SLRR_REQUIRE(slrr::skeleton_parse(kChildFirst, &skeleton, &error));
  SLRR_REQUIRE(skeleton.bones.size() == 2);
  SLRR_REQUIRE(skeleton.bones[0].parent == 1);
  SLRR_REQUIRE(skeleton.bones[0].limit_min[2] == 2.f);
  SLRR_REQUIRE(skeleton.bones[0].limit_max[2] == -1.f);
  float locals[32], worlds[32];
  slrr::skeleton_local_bind(skeleton, locals);
  slrr::skeleton_world(skeleton, locals, worlds);
  SLRR_REQUIRE(std::fabs(worlds[12] - 4.f) < 1e-4f);
  SLRR_REQUIRE(std::fabs(worlds[13] - 5.f) < 1e-4f);
  SLRR_REQUIRE(std::fabs(worlds[14]) < 1e-4f);
}

SLRR_TEST(skeleton_rejects_a_cycle_and_a_duplicate_name) {
  slrr::Skeleton skeleton;
  const char* cycle =
      "a\n0\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n"
      "b\n0\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n";
  // Bone 0 parents bone 0? parent index 0 is itself. The second bone also
  // parents 0, which is not a cycle by itself. Make them point at each other.
  const char* mutual =
      "a\n1\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n"
      "b\n0\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n";
  SLRR_REQUIRE(!slrr::skeleton_parse(mutual, &skeleton, nullptr));
  SLRR_REQUIRE(!slrr::skeleton_parse(cycle, &skeleton, nullptr));
  std::string bom;
  bom.push_back(static_cast<char>(0xef));
  bom.push_back(static_cast<char>(0xbb));
  bom.push_back(static_cast<char>(0xbf));
  bom += kChildFirst;
  SLRR_REQUIRE(slrr::skeleton_parse(bom, &skeleton, nullptr));
  const char* duplicate =
      "a\n-1\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n"
      "a\n-1\n1, 0, 0\n0, 1, 0\n0, 0, 1\n1, 0, 0\n0, 0\n0, 0\n0, 0\n";
  SLRR_REQUIRE(!slrr::skeleton_parse(duplicate, &skeleton, nullptr));
}

SLRR_TEST(clip_translation_uses_the_bone_length) {
  slrr::Skeleton skeleton;
  SLRR_REQUIRE(slrr::skeleton_parse(kChildFirst, &skeleton, nullptr));
  std::vector<uint8_t> bytes;
  append_u32(&bytes, 1);
  append_u32(&bytes, 1);
  append_f32(&bytes, 0.f);
  append_f32(&bytes, 0.f);
  append_f32(&bytes, 0.f);
  append_f32(&bytes, 0.f);
  append_f32(&bytes, 1.f);
  append_f32(&bytes, 0.f);
  append_f32(&bytes, 1.f);
  append_f32(&bytes, 0.f);
  bytes.insert(bytes.end(), {'c', 'h', 'i', 'l', 'd', '\n'});
  slrr::Clip clip;
  SLRR_REQUIRE(slrr::clip_parse(bytes.data(), bytes.size(), &clip, nullptr));
  float locals[32]{};
  slrr::skeleton_local_bind(skeleton, locals);
  SLRR_REQUIRE(slrr::clip_sample(skeleton, &skeleton, clip, 0.f, locals));
  SLRR_REQUIRE(std::fabs(locals[12]) < 1e-4f);
  SLRR_REQUIRE(std::fabs(locals[13] - 4.f) < 1e-4f);
  SLRR_REQUIRE(std::fabs(locals[14]) < 1e-4f);
  for (int i = 0; i < 4; ++i) bytes[24 + i] = 0;
  SLRR_REQUIRE(!slrr::clip_parse(bytes.data(), bytes.size(), &clip, nullptr));
}

SLRR_TEST(skin_follows_one_bone) {
  slrr::Skeleton skeleton;
  const char* rest =
      "bone\n-1\n1, 0, 0\n0, 1, 0\n0, 0, 1\n0, 0, 0\n0, 0\n0, 0\n0, 0\n";
  SLRR_REQUIRE(slrr::skeleton_parse(rest, &skeleton, nullptr));
  float worlds[16];
  slrr::skeleton_local_bind(skeleton, worlds);
  worlds[12] = 10.f;
  slrr::ScxGeometry mesh;
  mesh.positions = {1.f, 0.f, 0.f};
  mesh.normals = {0.f, 1.f, 0.f};
  mesh.skin_bones = {0, 0, 0, 0};
  mesh.skin_weights = {1.f, 0.f, 0.f, 0.f};
  std::vector<float> positions, normals;
  slrr::skeleton_skin(mesh, skeleton, worlds, &positions, &normals);
  SLRR_REQUIRE(positions.size() == 3);
  SLRR_REQUIRE(std::fabs(positions[0] - 11.f) < 1e-3f);
  SLRR_REQUIRE(std::fabs(positions[1]) < 1e-3f);
  SLRR_REQUIRE(std::fabs(positions[2]) < 1e-3f);
  SLRR_REQUIRE(std::fabs(normals[1] - 1.f) < 1e-3f);
}

SLRR_TEST(installed_human_files_round_trip_the_bind_pose) {
  const auto skel_path = find_game_file("humans/drivers/vin.skl");
  const auto mesh_path = find_game_file("humans/drivers/vin.scx");
  const auto clip_path = find_game_file("humans/pedestrians/gambler_walk.matrixdata");
  if (skel_path.empty() || mesh_path.empty() || clip_path.empty()) return;

  std::ifstream skel_in(skel_path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(skel_in)), std::istreambuf_iterator<char>());
  slrr::Skeleton skeleton;
  std::string error;
  SLRR_REQUIRE(slrr::skeleton_parse(text, &skeleton, &error));
  SLRR_REQUIRE(skeleton.bones.size() == 20);
  SLRR_REQUIRE(skeleton.bones[0].name == "Bip01 Spine");
  SLRR_REQUIRE(skeleton.bones[0].parent == 3);

  slrr::ScxGeometry mesh = slrr::load_scx_geometry(mesh_path.string());
  SLRR_REQUIRE(mesh.info.ok);
  SLRR_REQUIRE(!mesh.skin_weights.empty());
  const size_t vertices = mesh.positions.size() / 3;
  SLRR_REQUIRE(mesh.skin_weights.size() == vertices * 4);

  std::vector<float> locals(skeleton.bones.size() * 16);
  std::vector<float> worlds(skeleton.bones.size() * 16);
  slrr::skeleton_local_bind(skeleton, locals.data());
  slrr::skeleton_world(skeleton, locals.data(), worlds.data());
  std::vector<float> posed, normals;
  slrr::skeleton_skin(mesh, skeleton, worlds.data(), &posed, &normals);
  SLRR_REQUIRE(posed.size() == mesh.positions.size());
  for (size_t i = 0; i < posed.size(); ++i)
    SLRR_REQUIRE(std::fabs(posed[i] - mesh.positions[i]) < 0.05f);

  std::ifstream clip_in(clip_path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)),
                             std::istreambuf_iterator<char>());
  slrr::Clip clip;
  SLRR_REQUIRE(slrr::clip_parse(bytes.data(), bytes.size(), &clip, &error));
  SLRR_REQUIRE(clip.tracks.size() == 25);
  SLRR_REQUIRE(clip.times.size() == 10);
  SLRR_REQUIRE(std::fabs(clip.times.back() - 0.9f) < 1e-3f);
}
