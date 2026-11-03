#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// INVO SCX — layout from nonamesex/io_invo_scx (imhex/scx.hexpat)
// + SLRR_rev/docs/formats/scx.md.
struct ScxMeshInfo {
  bool ok = false;
  uint32_t version = 0;
  int32_t groups = 0;
  int32_t vertices = 0;
  int32_t triangles = 0;
  float bbox_min[3] = {0, 0, 0};
  float bbox_max[3] = {0, 0, 0};
  std::string path;
  std::string error;
};

struct ScxGroup {
  uint32_t index_start = 0;
  uint32_t index_count = 0;
  uint16_t texture_index = 0xffff;
  uint32_t material_flags = 0;  // bit0 = alphaOpacity (scx.md)
  float diffuse[4] = {0.6f, 0.65f, 0.7f, 1.f};
  std::string name;
};

// CPU mesh for GPU upload. Positions/normals/uv in original SCX units
// (CFG world uses ×0.01 → meters — apply at draw).
struct ScxGeometry {
  ScxMeshInfo info;
  std::vector<float> positions;  // xyz * N
  std::vector<float> normals;    // xyz * N
  std::vector<float> uvs;        // uv * N (UV1)
  std::vector<uint32_t> indices; // triangles * 3
  std::vector<ScxGroup> groups;
  // RPK `texture` property order (or .tex). Index == ScxGroup::texture_index.
  std::vector<std::string> textures;
};

ScxMeshInfo parse_scx_file(const std::string& path);
ScxGeometry load_scx_geometry(const std::string& path);

}  // namespace slrr
