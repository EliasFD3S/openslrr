#pragma once
#include "slrr/scx_mesh.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>

namespace slrr {
// Explicit stock garage adapter for twin-tube ceiling fittings.
// - g1: the `texturak` section of garazs1.dds; its tube faces sample a white
//   atlas swatch, while the shell and fitting use other coordinates in the
//   same material.
// - g2/g3 (#235): the lamp groups carry an authored SCX4 emissive colour and
//   no emission mask (`neon` in garazs2_targyak, `Material #11345` in
//   garazs3targyak). Other emissive groups, such as the `reklam` signs, fail
//   the tube shape test below.
// Only tube-shaped components (2-4 m long, under 0.25 m wide and tall, above
// 3.3 m, paired) become emitters. Do not mark the entire material emissive or
// guess sources from arbitrary bright pixels.
inline bool legacy_neon_candidate_group(const ScxGeometry &mesh,
                                        const ScxGroup &group, bool *swatch) {
  *swatch = group.name == "texturak" &&
            group.texture_index < mesh.textures.size() &&
            std::filesystem::path(mesh.textures[group.texture_index])
                    .filename() == "garazs1.dds";
  const bool emissive = group.emission_index == 0xffff &&
                        (group.emission_color[0] > 0.f ||
                         group.emission_color[1] > 0.f ||
                         group.emission_color[2] > 0.f);
  return *swatch || emissive;
}
inline void adapt_legacy_neon_fixtures(ScxGeometry &mesh) {
  if (!mesh.ceiling_light_anchors.empty())
    return;
  struct Triangle {
    size_t offset;
  };
  std::vector<Triangle> triangles;
  for (size_t g = 0; g < mesh.groups.size(); ++g) {
    const auto &group = mesh.groups[g];
    bool swatch = false;
    if (!legacy_neon_candidate_group(mesh, group, &swatch))
      continue;
    const auto &uv = group.base_map.channel == 2   ? mesh.uv3
                     : group.base_map.channel == 1 ? mesh.uv2
                                                   : mesh.uvs;
    for (size_t i = group.index_start;
         i + 2 < group.index_start + group.index_count; i += 3) {
      bool tube = true;
      for (size_t k = 0; k < 3; ++k) {
        const auto v = mesh.indices[i + k];
        tube &= (!swatch || v * 2 + 1 < uv.size()) &&
                v * 3 + 2 < mesh.positions.size();
        if (!tube)
          break;
        // g1 shares its material with the fitting: keep only the swatch.
        tube &= (!swatch || (std::abs(uv[v * 2] - .188369f) < .00002f &&
                             std::abs(uv[v * 2 + 1] - .139628f) < .00002f)) &&
                mesh.positions[v * 3 + 1] * mesh.position_scale > 3.3f;
      }
      if (tube)
        triangles.push_back({i});
    }
  }
  if (triangles.empty())
    return;
  std::vector<size_t> parent(triangles.size());
  std::iota(parent.begin(), parent.end(), 0);
  auto root = [&](size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  std::map<std::array<int, 3>, size_t> vertices;
  for (size_t t = 0; t < triangles.size(); ++t)
    for (size_t k = 0; k < 3; ++k) {
      const auto v = mesh.indices[triangles[t].offset + k];
      std::array<int, 3> key;
      for (size_t a = 0; a < 3; ++a)
        key[a] = int(std::lround(mesh.positions[v * 3 + a] *
                                 mesh.position_scale * 1000));
      auto [it, inserted] = vertices.emplace(key, t);
      if (!inserted)
        parent[root(t)] = root(it->second);
    }
  struct Bounds {
    std::array<float, 3> low{1e9f, 1e9f, 1e9f}, high{-1e9f, -1e9f, -1e9f};
  };
  std::map<size_t, Bounds> bounds;
  for (size_t t = 0; t < triangles.size(); ++t)
    for (size_t k = 0; k < 3; ++k) {
      auto &b = bounds[root(t)];
      auto v = mesh.indices[triangles[t].offset + k];
      for (size_t a = 0; a < 3; ++a) {
        float x = mesh.positions[v * 3 + a] * mesh.position_scale;
        b.low[a] = std::min(b.low[a], x);
        b.high[a] = std::max(b.high[a], x);
      }
    }
  std::vector<bool> accepted(mesh.indices.size() / 3, false);
  std::vector<std::array<float, 3>> tubes;
  for (const auto &[id, b] : bounds) {
    const float x = b.high[0] - b.low[0], z = b.high[2] - b.low[2],
                y = b.high[1] - b.low[1];
    if (std::max(x, z) < 2 || std::max(x, z) > 4 || std::min(x, z) > .25f ||
        y > .25f)
      continue;
    tubes.push_back({(b.low[0] + b.high[0]) * .5f, b.low[1] - .08f,
                     (b.low[2] + b.high[2]) * .5f});
    for (size_t t = 0; t < triangles.size(); ++t)
      if (root(t) == id)
        accepted[triangles[t].offset / 3] = true;
  }
  // Pair the two adjacent tubes into one broad downward emitter per fitting.
  std::vector<bool> used(tubes.size());
  for (size_t i = 0; i < tubes.size(); ++i) {
    if (used[i])
      continue;
    for (size_t j = i + 1; j < tubes.size(); ++j) {
      if (used[j] ||
          std::hypot(tubes[i][0] - tubes[j][0], tubes[i][2] - tubes[j][2]) >
              .4f ||
          std::abs(tubes[i][1] - tubes[j][1]) > .1f)
        continue;
      mesh.ceiling_light_anchors.push_back({(tubes[i][0] + tubes[j][0]) * .5f,
                                            std::min(tubes[i][1], tubes[j][1]),
                                            (tubes[i][2] + tubes[j][2]) * .5f});
      used[i] = used[j] = true;
      break;
    }
  }
  if (mesh.ceiling_light_anchors.empty())
    return;
  std::vector<uint32_t> indices;
  std::vector<ScxGroup> groups;
  for (auto original : mesh.groups)
    for (bool emitting : {false, true}) {
      auto group = original;
      group.index_start = uint32_t(indices.size());
      for (size_t i = original.index_start;
           i + 2 < original.index_start + original.index_count; i += 3)
        if (accepted[i / 3] == emitting)
          indices.insert(indices.end(), mesh.indices.begin() + i,
                         mesh.indices.begin() + i + 3);
      group.index_count = uint32_t(indices.size()) - group.index_start;
      if (!group.index_count)
        continue;
      group.self_illuminated = emitting;
      groups.push_back(std::move(group));
    }
  mesh.indices = std::move(indices);
  mesh.groups = std::move(groups);
}
} // namespace slrr
