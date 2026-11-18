#pragma once
#include "slrr/scx_mesh.hpp"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace slrr {
// Draw metadata only: vertex, index and skin arrays stay in the shared source.
struct SceneMeshData {
  ScxMeshInfo info;
  bool ground_polygons = false;
  std::vector<ScxGroup> groups;
  std::vector<std::string> textures;
  float position_scale = .01f;
  size_t vertex_count = 0, index_count = 0;
  explicit SceneMeshData(const ScxGeometry &source)
      : info(source.info), ground_polygons(source.ground_polygons),
        groups(source.groups), textures(source.textures),
        position_scale(source.position_scale),
        vertex_count(source.positions.size() / 3),
        index_count(source.indices.size()) {}
  SceneMeshData() = default;
};
// Reordered indices are upload scratch, never retained in the GPU cache.
struct SceneMeshUpload {
  SceneMeshData metadata;
  std::vector<uint32_t> merged_indices;
  explicit SceneMeshUpload(const ScxGeometry &source) : metadata(source) {}
  const std::vector<uint32_t> &indices(const ScxGeometry &source) const {
    return merged_indices.empty() ? source.indices : merged_indices;
  }
};
namespace mesh_detail {
inline bool same_map(const ScxTextureMap &a, const ScxTextureMap &b) {
  return a.channel == b.channel && a.tiling == b.tiling &&
         a.scale_u == b.scale_u && a.scale_v == b.scale_v &&
         a.offset_u == b.offset_u && a.offset_v == b.offset_v;
}

// Everything a draw reads from a group, except its index range and name.
inline bool same_material(const ScxGroup &a, const ScxGroup &b) {
  return a.texture_index == b.texture_index && a.mix_index == b.mix_index &&
         a.reflection_index == b.reflection_index &&
         a.lightmap_index == b.lightmap_index &&
         a.illumination_index == b.illumination_index &&
         a.emission_index == b.emission_index &&
         same_map(a.emission_map, b.emission_map) &&
         std::equal(std::begin(a.emission_color), std::end(a.emission_color),
                    std::begin(b.emission_color)) &&
         a.mix_vertex_color == b.mix_vertex_color &&
         a.material_type == b.material_type &&
         a.reflection_strength == b.reflection_strength &&
         same_map(a.base_map, b.base_map) && same_map(a.mix_map, b.mix_map) &&
         same_map(a.reflection_map, b.reflection_map) &&
         same_map(a.lightmap_map, b.lightmap_map) &&
         same_map(a.illumination_map, b.illumination_map) &&
         a.material_flags == b.material_flags &&
         a.alpha_reference == b.alpha_reference &&
         std::equal(std::begin(a.diffuse), std::end(a.diffuse),
                    std::begin(b.diffuse)) &&
         std::equal(std::begin(a.specular), std::end(a.specular),
                    std::begin(b.specular)) &&
         a.shininess == b.shininess;
}
inline void merge_scene_materials(SceneMeshUpload &upload,
                                  const ScxGeometry &source) {
  auto &geo = upload.metadata;
  if (geo.groups.size() < 2)
    return;
  const bool v3 = geo.info.version == 3, v4 = geo.info.version == 4;
  for (const auto &group : geo.groups) {
    // Blended sections are drawn in their stored order. Leave such a mesh as
    // is.
    if ((v4 && (group.material_flags & 0x40000000u)) ||
        (v3 && (group.material_flags & 0x101u) == 0x101u))
      return;
    if (static_cast<size_t>(group.index_start) + group.index_count >
        source.indices.size())
      return;
  }
  // Materials in order of first use; each keeps its sections in stored order.
  std::vector<size_t> material_of(geo.groups.size());
  std::vector<size_t> first;
  for (size_t i = 0; i < geo.groups.size(); ++i) {
    size_t m = 0;
    while (m < first.size() &&
           !same_material(geo.groups[first[m]], geo.groups[i]))
      ++m;
    if (m == first.size())
      first.push_back(i);
    material_of[i] = m;
  }
  if (first.size() == geo.groups.size())
    return;
  std::vector<uint32_t> indices;
  indices.reserve(source.indices.size());
  std::vector<ScxGroup> groups;
  groups.reserve(first.size());
  for (size_t m = 0; m < first.size(); ++m) {
    ScxGroup merged = geo.groups[first[m]];
    merged.index_start = static_cast<uint32_t>(indices.size());
    for (size_t i = 0; i < geo.groups.size(); ++i) {
      if (material_of[i] != m)
        continue;
      const auto begin = source.indices.begin() + geo.groups[i].index_start;
      indices.insert(indices.end(), begin, begin + geo.groups[i].index_count);
    }
    merged.index_count =
        static_cast<uint32_t>(indices.size()) - merged.index_start;
    groups.push_back(std::move(merged));
  }
  upload.merged_indices = std::move(indices);
  geo.index_count = upload.merged_indices.size();
  geo.groups = std::move(groups);
  geo.info.groups = static_cast<int32_t>(geo.groups.size());
}

} // namespace mesh_detail
inline SceneMeshUpload prepare_scene_mesh(const ScxGeometry &source,
                                          bool merge) {
  SceneMeshUpload upload(source);
  if (merge)
    mesh_detail::merge_scene_materials(upload, source);
  return upload;
}
} // namespace slrr
