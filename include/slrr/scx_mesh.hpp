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

struct ScxTextureMap {
  uint32_t channel = 0, tiling = 0;
  float scale_u = 1, scale_v = 1, offset_u = 0, offset_v = 0;
};
struct ScxGroup {
  uint32_t index_start = 0;
  uint32_t index_count = 0;
  uint16_t texture_index = 0xffff;
  uint16_t mix_index = 0xffff, reflection_index = 0xffff;
  // Baked light (v4 property 0x06000007) and legacy illumination.
  uint16_t lightmap_index = 0xffff, illumination_index = 0xffff;
  bool mix_vertex_color = false;
  uint32_t material_type = 0;
  float reflection_strength = .5f;
  ScxTextureMap base_map, mix_map, reflection_map, lightmap_map, illumination_map;
  uint16_t emission_index = 0xffff;
  ScxTextureMap emission_map;
  // SCX4 COLOR_EMISSIVE (property 2), independent of an emission mask.
  float emission_color[3] = {0.f, 0.f, 0.f};
  uint32_t material_flags = 0;  // SCX3 bit0 alphaOpacity; SCX4 render/coverage flags
  uint8_t alpha_reference = 0x38;
  float diffuse[4] = {0.6f, 0.65f, 0.7f, 1.f};
  // v4 color 1 and 0x01000000. Paint and chrome use a tight power (~21),
  // rubber a wide one (~2.7). Zero specular adds nothing.
  float specular[3] = {0.f, 0.f, 0.f};
  float shininess = 1.f;
  std::string name;
};

// Function words select colour operations; the SCX4 render flags independently
// select surface coverage. ColorMixTex must never override authored holes.
struct ScxSurface {
  bool blend = false;
  bool cutout = false;
  bool color_mix = false;
};
inline ScxSurface scx_surface(int version, const ScxGroup& group) {
  ScxSurface surface;
  const auto flags = group.material_flags;
  if (version == 4) {
    surface.blend = (flags & 0x40000000u) != 0;
    surface.cutout = !surface.blend && (flags & 0x20000000u) != 0;
    surface.color_mix = group.material_type == 0x03u && !surface.blend && !surface.cutout;
  } else if (version == 3) {
    surface.blend = (flags & 0x101u) == 0x101u;
    surface.cutout = !surface.blend && (flags & 1u) != 0;
  }
  return surface;
}

// Blended SCX4 surfaces still carry material opacity. A deliberately absent
// diffuse map uses COLOR_DIFFUSE, not an opaque white fallback. Textured Tex
// families may have black COLOR_DIFFUSE and must keep their sampled RGB.
inline void scx_modulate_blend(int version, const ScxGroup& group, float (&tint)[4],
                               bool premultiplied = false) {
  if (version != 4 || !scx_surface(version, group).blend) return;
  tint[3] *= group.diffuse[3];
  if (group.texture_index == 0xffff)
    for (int i = 0; i < 3; ++i) tint[i] *= group.diffuse[i];
  if (premultiplied)
    for (int i = 0; i < 3; ++i) tint[i] *= group.diffuse[3];
}

// CPU mesh for GPU upload. Positions/normals/uv in original SCX units
// (CFG world uses ×0.01 → meters — apply at draw).
struct ScxGeometry {
  ScxMeshInfo info;
  bool ground_polygons = false;
  std::vector<float> positions;  // xyz * N
  std::vector<float> normals;    // xyz * N
  std::vector<float> uvs;        // uv * N (UV1)
  std::vector<float> uv2, uv3;
  std::vector<uint32_t> vertex_colors; // RPK POLY A8R8G8B8
  std::vector<uint32_t> indices; // triangles * 3
  // Four influences per vertex, parallel to positions / 3. Empty when the
  // mesh has no bone palette. Bone ids are indices into the skeleton the
  // render descriptor names. A zero weight leaves that influence unused.
  std::vector<uint16_t> skin_bones;
  std::vector<float> skin_weights;
  std::vector<ScxGroup> groups;
  // RPK `texture` property order (or .tex). Index == ScxGroup::texture_index.
  std::vector<std::string> textures;
  // SCX positions are centimetres. POLY / script positions are already metres.
  float position_scale = 0.01f;
  // Bumped when skinned positions change so the GPU buffer can be replaced
  // without treating the mesh as a new object.
  uint32_t revision = 0;
};

ScxMeshInfo parse_scx_file(const std::string& path);
ScxGeometry load_scx_geometry(const std::string& path);
void scx_resolve_baked_lightmaps(ScxGeometry& geometry);

}  // namespace slrr
