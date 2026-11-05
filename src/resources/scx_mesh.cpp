#include "slrr/scx_mesh.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace slrr {
namespace {

uint32_t rd_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float rd_f32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

void expand_bbox(ScxMeshInfo& info, float x, float y, float z, bool& init) {
  if (!init) {
    info.bbox_min[0] = info.bbox_max[0] = x;
    info.bbox_min[1] = info.bbox_max[1] = y;
    info.bbox_min[2] = info.bbox_max[2] = z;
    init = true;
    return;
  }
  info.bbox_min[0] = std::min(info.bbox_min[0], x);
  info.bbox_min[1] = std::min(info.bbox_min[1], y);
  info.bbox_min[2] = std::min(info.bbox_min[2], z);
  info.bbox_max[0] = std::max(info.bbox_max[0], x);
  info.bbox_max[1] = std::max(info.bbox_max[1], y);
  info.bbox_max[2] = std::max(info.bbox_max[2], z);
}

bool parse_v3(const std::vector<uint8_t>& buf, ScxGeometry& geo, bool want_geo) {
  ScxMeshInfo& info = geo.info;
  size_t off = 8;
  bool bbox_init = false;
  while (off + 4 <= buf.size()) {
    const size_t mat_start = off;
    const uint32_t size = rd_u32(buf.data() + off);
    if (size == 0) break;
    if (size != 56 && size != 104 && size != 136) {
      info.error = "v3 unsupported material size";
      return false;
    }
    if (off + size > buf.size()) {
      info.error = "v3 material overrun";
      return false;
    }
    ScxGroup group;
    // io_invo_scx SCX3Material: diffuse RGB+opacity @+4, tex indices @+44
    group.diffuse[0] = rd_f32(buf.data() + mat_start + 4);
    group.diffuse[1] = rd_f32(buf.data() + mat_start + 8);
    group.diffuse[2] = rd_f32(buf.data() + mat_start + 12);
    group.diffuse[3] = rd_f32(buf.data() + mat_start + 16);
    group.specular[0] = rd_f32(buf.data() + mat_start + 20);
    group.specular[1] = rd_f32(buf.data() + mat_start + 24);
    group.specular[2] = rd_f32(buf.data() + mat_start + 28);
    group.shininess = rd_f32(buf.data() + mat_start + 36);
    group.material_flags = rd_u32(buf.data() + mat_start + 40);
    group.reflection_strength = rd_f32(buf.data() + mat_start + 32);
    group.reflection_index = static_cast<uint16_t>(buf[mat_start+50] | (buf[mat_start+51]<<8));
    group.mix_index = static_cast<uint16_t>(buf[mat_start+52] | (buf[mat_start+53]<<8));
    if (size > 56) {
      group.base_map.channel = static_cast<uint16_t>(buf[mat_start+70] | (buf[mat_start+71]<<8));
      group.mix_map.channel = static_cast<uint16_t>(buf[mat_start+72] | (buf[mat_start+73]<<8));
      // +0x38 is the lightmap slot. The exe always samples it on uv channel 1.
      const int16_t lm = static_cast<int16_t>(
          buf[mat_start + 0x38] | (buf[mat_start + 0x39] << 8));
      if (lm >= 0) {
        group.lightmap_index = static_cast<uint16_t>(lm);
        group.lightmap_map.channel = 1;
      }
    }
    group.texture_index = static_cast<uint16_t>(
        buf[mat_start + 44] | (buf[mat_start + 45] << 8));
    if (size == 136) {
      group.name.assign(reinterpret_cast<const char*>(buf.data() + mat_start + 104),
                       32);
      const auto nul = group.name.find('\0');
      if (nul != std::string::npos) group.name.resize(nul);
    }
    off += size;
    if (off + 4 > buf.size()) {
      info.error = "v3 missing vertex count";
      return false;
    }
    const uint32_t vcount = rd_u32(buf.data() + off);
    off += 4;
    const size_t stride = size > 56 ? 64u : 44u;
    if (off + static_cast<size_t>(vcount) * stride > buf.size()) {
      info.error = "v3 vertex overrun";
      return false;
    }
    const uint32_t base = static_cast<uint32_t>(geo.positions.size() / 3);
    for (uint32_t i = 0; i < vcount; ++i) {
      const uint8_t* v = buf.data() + off + static_cast<size_t>(i) * stride;
      const float x = rd_f32(v);
      const float y = rd_f32(v + 4);
      const float z = rd_f32(v + 8);
      expand_bbox(info, x, y, z, bbox_init);
      if (want_geo) {
        geo.positions.push_back(x);
        geo.positions.push_back(y);
        geo.positions.push_back(z);
        geo.normals.push_back(rd_f32(v + 12));
        geo.normals.push_back(rd_f32(v + 16));
        geo.normals.push_back(rd_f32(v + 20));
        geo.uvs.push_back(rd_f32(v + 24));
        geo.uvs.push_back(rd_f32(v + 28));
        geo.uv2.push_back(rd_f32(v + 32));
        geo.uv2.push_back(rd_f32(v + 36));
        geo.uv3.push_back(size > 56 ? rd_f32(v + 52) : 0);
        geo.uv3.push_back(size > 56 ? rd_f32(v + 56) : 0);
      }
    }
    off += static_cast<size_t>(vcount) * stride;
    if (off + 4 > buf.size()) {
      info.error = "v3 missing face count";
      return false;
    }
    const uint32_t face_count = rd_u32(buf.data() + off);
    off += 4;
    const size_t idx_bytes = static_cast<size_t>(face_count) * 12;
    if (off + idx_bytes > buf.size()) {
      info.error = "v3 index overrun";
      return false;
    }
    if (want_geo) {
      group.index_start = static_cast<uint32_t>(geo.indices.size());
      group.index_count = face_count * 3;
      for (uint32_t t = 0; t < face_count; ++t) {
        const uint8_t* ix = buf.data() + off + static_cast<size_t>(t) * 12;
        geo.indices.push_back(base + rd_u32(ix));
        geo.indices.push_back(base + rd_u32(ix + 4));
        geo.indices.push_back(base + rd_u32(ix + 8));
      }
      geo.groups.push_back(std::move(group));
    }
    off += idx_bytes;
    info.groups += 1;
    info.vertices += static_cast<int32_t>(vcount);
    info.triangles += static_cast<int32_t>(face_count);
  }
  info.ok = info.groups > 0;
  if (!info.ok && info.error.empty()) info.error = "v3 no groups";
  return info.ok;
}

size_t v4_weight_bytes(uint32_t flags) {
  // Influence group bits are mutually exclusive (SLRR_rev scx.md).
  if (flags & 0x4) return 4;
  if (flags & 0x8) return 8;
  if (flags & 0x10) return 12;
  return 0;  // 0x2 = one influence, no stored weights
}

size_t v4_vertex_stride(uint32_t flags) {
  size_t stride = 0;
  if (flags & 0x1) stride += 12;
  stride += v4_weight_bytes(flags);
  if (flags & 0x20) stride += 4;
  if (flags & 0x40) stride += 12;
  if (flags & 0x80) stride += 4;
  if (flags & 0x100) stride += 4;
  if (flags & 0x200) stride += 8;
  if (flags & 0x400) stride += 8;
  if (flags & 0x800) stride += 8;
  if (flags & 0x40000) stride += 12;
  return stride;
}

bool parse_v4(const std::vector<uint8_t>& buf, ScxGeometry& geo, bool want_geo) {
  ScxMeshInfo& info = geo.info;
  if (buf.size() < 12) {
    info.error = "v4 too small";
    return false;
  }
  const uint32_t dir_n = rd_u32(buf.data() + 8);
  size_t off = 12;
  if (off + static_cast<size_t>(dir_n) * 8 > buf.size()) {
    info.error = "v4 directory overrun";
    return false;
  }
  struct Dir {
    uint32_t type;
    uint32_t offset;
  };
  std::vector<Dir> dir;
  dir.reserve(dir_n);
  for (uint32_t i = 0; i < dir_n; ++i) {
    dir.push_back({rd_u32(buf.data() + off), rd_u32(buf.data() + off + 4)});
    off += 8;
  }

  const bool physics_only = !dir.empty() && dir[0].type == 1;

  bool bbox_init = false;
  ScxGroup mat{};
  bool have_mat = false;
  bool have_verts = false;
  uint32_t base = 0;
  uint32_t last_vcount = 0;
  uint32_t group_index_start = 0;

  auto finish_group = [&]() {
    if (!have_verts) return;
    if (want_geo) {
      ScxGroup g = mat;
      g.index_start = group_index_start;
      g.index_count =
          static_cast<uint32_t>(geo.indices.size()) - group_index_start;
      if (g.index_count > 0) geo.groups.push_back(std::move(g));
    }
    info.groups += 1;
    have_verts = false;
    last_vcount = 0;
    group_index_start = static_cast<uint32_t>(geo.indices.size());
  };

  for (const Dir& d : dir) {
    if (static_cast<size_t>(d.offset) + 8 > buf.size()) continue;
    const uint32_t type = rd_u32(buf.data() + d.offset);
    const uint32_t size = rd_u32(buf.data() + d.offset + 4);
    if (type != d.type || size < 8) continue;
    if (static_cast<size_t>(d.offset) + size > buf.size()) continue;
    const size_t chunk_end = static_cast<size_t>(d.offset) + size;
    size_t p = static_cast<size_t>(d.offset) + 8;

    if (type == 0 || (type == 1 && physics_only)) {
      finish_group();
      mat = ScxGroup{};
      have_mat = true;
      if (type == 0 && p + 12 <= chunk_end) {
        // flags u32, function u32, property count u32
        mat.material_type = rd_u32(buf.data() + p);
        mat.material_flags = rd_u32(buf.data() + p + 4);
        p += 8;  // flags + function word
        const uint32_t nprop = rd_u32(buf.data() + p);
        p += 4;
        for (uint32_t j = 0; j < nprop && p + 4 <= chunk_end; ++j) {
          const uint32_t prop = rd_u32(buf.data() + p);
          p += 4;
          if (prop <= 2) {
            if (p + 4 > chunk_end) break;
            if (want_geo && prop == 0) {
              mat.diffuse[0] = buf[p + 0] / 255.f;
              mat.diffuse[1] = buf[p + 1] / 255.f;
              mat.diffuse[2] = buf[p + 2] / 255.f;
              mat.diffuse[3] = buf[p + 3] / 255.f;
            } else if (want_geo && prop == 1) {
              mat.specular[0] = buf[p + 0] / 255.f;
              mat.specular[1] = buf[p + 1] / 255.f;
              mat.specular[2] = buf[p + 2] / 255.f;
            }
            p += 4;
          } else if (prop >= 0x01000000u && prop <= 0x01000002u) {
            if (p + 4 > chunk_end) break;
            if (prop == 0x01000000u) mat.shininess = rd_f32(buf.data() + p);
            if (prop == 0x01000001u) mat.reflection_strength = rd_f32(buf.data()+p);
            p += 4;
          } else if (prop == 0x05000000u || prop == 0x07000000u) {
            if (p + 4 > chunk_end) break;
            p += 4;
          } else if (prop >= 0x06000000u && prop <= 0x06000007u) {
            // tex index, channel, tiling, scaleUV, offsetUV
            if (p + 28 > chunk_end) break;
            const uint32_t tex_i = rd_u32(buf.data() + p);
            ScxTextureMap map;
            map.channel = rd_u32(buf.data()+p+4);
            map.tiling = rd_u32(buf.data()+p+8);
            map.scale_u = rd_f32(buf.data()+p+12);
            map.scale_v = rd_f32(buf.data()+p+16);
            map.offset_u = rd_f32(buf.data()+p+20);
            map.offset_v = rd_f32(buf.data()+p+24);
            // Prefer DiffuseMap (0x06000000); fall back to DiffuseMixSecond.
            if (prop == 0x06000000u ||
                (prop == 0x06000001u && mat.texture_index == 0xffff)) {
              mat.texture_index = static_cast<uint16_t>(tex_i);
              mat.base_map = map;
            }
            if (prop == 0x06000001u) { mat.mix_index=static_cast<uint16_t>(tex_i); mat.mix_map=map; }
            if (prop == 0x06000003u) { mat.reflection_index=static_cast<uint16_t>(tex_i); mat.reflection_map=map; }
            if (prop == 0x06000007u && tex_i != 0xffffffffu) {
              mat.lightmap_index = static_cast<uint16_t>(tex_i);
              mat.lightmap_map = map;
            }
            p += 28;
          } else if (prop == 0x08000000u) {
            if (p + 32 > chunk_end) break;
            if (want_geo) {
              mat.name.assign(reinterpret_cast<const char*>(buf.data() + p),
                              32);
              const auto nul = mat.name.find('\0');
              if (nul != std::string::npos) mat.name.resize(nul);
            }
            p += 32;
          } else {
            break;  // unknown — stop material props
          }
        }
      }
    } else if (type == 4) {
      if (!have_mat) continue;
      if (have_verts) finish_group();
      if (p + 8 > chunk_end) continue;
      const uint32_t count = rd_u32(buf.data() + p);
      const uint32_t flags = rd_u32(buf.data() + p + 4);
      p += 8;
      if (!(flags & 0x1)) continue;
      const size_t stride = v4_vertex_stride(flags);
      if (stride == 0 ||
          p + static_cast<size_t>(count) * stride > chunk_end)
        continue;
      base = static_cast<uint32_t>(geo.positions.size() / 3);
      last_vcount = count;
      info.vertices += static_cast<int32_t>(count);
      have_verts = true;
      group_index_start = static_cast<uint32_t>(geo.indices.size());
      for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* v = buf.data() + p + static_cast<size_t>(i) * stride;
        size_t o = 0;
        const float x = rd_f32(v + o);
        const float y = rd_f32(v + o + 4);
        const float z = rd_f32(v + o + 8);
        o += 12;
        expand_bbox(info, x, y, z, bbox_init);
        o += v4_weight_bytes(flags);
        if (flags & 0x20) o += 4;
        float nx = 0.f, ny = 1.f, nz = 0.f;
        if (flags & 0x40) {
          nx = rd_f32(v + o);
          ny = rd_f32(v + o + 4);
          nz = rd_f32(v + o + 8);
          o += 12;
        }
        if (flags & 0x80) o += 4;
        if (flags & 0x100) o += 4;
        float u = 0.f, vv = 0.f;
        if (flags & 0x200) {
          u = rd_f32(v + o);
          vv = rd_f32(v + o + 4);
          o += 8;
        }
        float u2=0,v2=0,u3=0,v3=0;
        if(flags&0x400) {u2=rd_f32(v+o);v2=rd_f32(v+o+4);o+=8;}
        if(flags&0x800) {u3=rd_f32(v+o);v3=rd_f32(v+o+4);o+=8;}
        if (want_geo) {
          geo.positions.push_back(x);
          geo.positions.push_back(y);
          geo.positions.push_back(z);
          geo.normals.push_back(nx);
          geo.normals.push_back(ny);
          geo.normals.push_back(nz);
          geo.uvs.push_back(u);
          geo.uvs.push_back(vv);
          geo.uv2.push_back(u2); geo.uv2.push_back(v2);
          geo.uv3.push_back(u3); geo.uv3.push_back(v3);
        }
      }
    } else if (type == 5) {
      if (!have_mat || !have_verts) continue;
      if (p + 4 > chunk_end) continue;
      const uint32_t icount = rd_u32(buf.data() + p);
      p += 4;
      if (p + static_cast<size_t>(icount) * 2 > chunk_end) continue;
      info.triangles += static_cast<int32_t>(icount / 3);
      if (want_geo) {
        for (uint32_t i = 0; i + 2 < icount; i += 3) {
          const uint16_t a = static_cast<uint16_t>(
              buf[p + i * 2] | (buf[p + i * 2 + 1] << 8));
          const uint16_t b = static_cast<uint16_t>(
              buf[p + (i + 1) * 2] | (buf[p + (i + 1) * 2 + 1] << 8));
          const uint16_t c = static_cast<uint16_t>(
              buf[p + (i + 2) * 2] | (buf[p + (i + 2) * 2 + 1] << 8));
          if (a < last_vcount && b < last_vcount && c < last_vcount) {
            geo.indices.push_back(base + a);
            geo.indices.push_back(base + b);
            geo.indices.push_back(base + c);
          }
        }
      }
    }
  }
  finish_group();
  info.ok = info.vertices > 0 || info.triangles > 0;
  if (!info.ok) info.error = "v4 no geometry";
  return info.ok;
}

bool load_bytes(const std::string& path, std::vector<uint8_t>& buf,
                ScxMeshInfo& info) {
  info.path = path;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    info.error = "open failed";
    return false;
  }
  const auto sz = static_cast<size_t>(in.tellg());
  if (sz < 8) {
    info.error = "too small";
    return false;
  }
  in.seekg(0);
  buf.resize(sz);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(sz));
  if (!in || std::memcmp(buf.data(), "INVO", 4) != 0) {
    info.error = "not INVO";
    return false;
  }
  info.version = rd_u32(buf.data() + 4);
  return true;
}

}  // namespace

ScxMeshInfo parse_scx_file(const std::string& path) {
  ScxGeometry geo;
  std::vector<uint8_t> buf;
  if (!load_bytes(path, buf, geo.info)) return geo.info;
  if (geo.info.version == 3) {
    parse_v3(buf, geo, false);
  } else if (geo.info.version == 4) {
    parse_v4(buf, geo, false);
  } else {
    geo.info.error = "unsupported version";
  }
  return geo.info;
}

ScxGeometry load_scx_geometry(const std::string& path) {
  ScxGeometry geo;
  std::vector<uint8_t> buf;
  if (!load_bytes(path, buf, geo.info)) return geo;
  if (geo.info.version == 3) {
    parse_v3(buf, geo, true);
  } else if (geo.info.version == 4) {
    parse_v4(buf, geo, true);
  } else {
    geo.info.error = "unsupported version";
  }
  if (geo.info.ok) {
    std::printf("[scx] geometry %s v=%u verts=%zu tris=%zu groups=%zu uvs=%zu\n",
                path.c_str(), geo.info.version, geo.positions.size() / 3,
                geo.indices.size() / 3, geo.groups.size(),
                geo.uvs.size() / 2);
    for (size_t i = 0; i < geo.groups.size() && i < 12; ++i) {
      const ScxGroup& g = geo.groups[i];
      std::printf("[scx]   group[%zu] tex=%u idx=%u..+%u '%s'\n", i,
                  g.texture_index, g.index_start, g.index_count,
                  g.name.c_str());
    }
  }
  return geo;
}

}  // namespace slrr
