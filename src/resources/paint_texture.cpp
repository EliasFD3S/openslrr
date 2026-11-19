#include "slrr/paint_texture.hpp"

#include "slrr/dds_image.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/scx_mesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace slrr {
namespace {

struct Entry {
  PaintImage committed, shown;
  bool temp = false;
  uint32_t revision = 1;
};
std::unordered_map<uint32_t, Entry> g_textures;
std::vector<uint32_t> g_released;
uint32_t g_next_local = 1;

Entry* entry(uint32_t id) {
  auto it = g_textures.find(id);
  return it == g_textures.end() ? nullptr : &it->second;
}

// ---- BC1/BC2/BC3 ----------------------------------------------------------

void rgb565(uint16_t c, uint8_t* out) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
  out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
  out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

void decode_color_block(const uint8_t* block, bool bc1, uint8_t texels[16][4]) {
  const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
  const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
  uint8_t palette[4][4]{};
  rgb565(c0, palette[0]);
  rgb565(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255;
  if (!bc1 || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i)
      palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
    palette[2][3] = 255;
    palette[3][3] = 0;
  }
  const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) |
                        (static_cast<uint32_t>(block[7]) << 24);
  for (int i = 0; i < 16; ++i)
    std::memcpy(texels[i], palette[(bits >> (2 * i)) & 3], 4);
}

bool decode_bc(const DdsImage& texture, PaintImage* out) {
  const bool bc1 = texture.format == DdsFormat::DXT1;
  const size_t block_size = bc1 ? 8 : 16;
  const uint32_t bw = (texture.width + 3) / 4, bh = (texture.height + 3) / 4;
  if (texture.data.size() < size_t(bw) * bh * block_size) return false;
  out->width = texture.width;
  out->height = texture.height;
  out->rgba.assign(size_t(out->width) * out->height * 4, 0);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* block = texture.data.data() + (size_t(by) * bw + bx) * block_size;
      uint8_t texels[16][4];
      decode_color_block(bc1 ? block : block + 8, bc1, texels);
      if (texture.format == DdsFormat::DXT3) {
        for (int i = 0; i < 16; ++i) {
          const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 15;
          texels[i][3] = static_cast<uint8_t>(nibble * 17);
        }
      } else if (texture.format == DdsFormat::DXT5) {
        const uint8_t a0 = block[0], a1 = block[1];
        uint8_t alpha[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 1; i < 7; ++i)
            alpha[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
        } else {
          for (int i = 1; i < 5; ++i)
            alpha[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
          alpha[6] = 0;
          alpha[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) texels[i][3] = alpha[(bits >> (3 * i)) & 7];
      }
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= out->width || y >= out->height) continue;
        std::memcpy(&out->rgba[(size_t(y) * out->width + x) * 4], texels[i], 4);
      }
    }
  return true;
}

// ---- PNG ------------------------------------------------------------------

struct BitWriter {
  std::vector<uint8_t>& out;
  uint32_t buffer = 0;
  int count = 0;
  void bits(uint32_t value, int n) {  // LSB first
    buffer |= value << count;
    count += n;
    while (count >= 8) {
      out.push_back(static_cast<uint8_t>(buffer & 0xff));
      buffer >>= 8;
      count -= 8;
    }
  }
  void huffman(uint32_t code, int n) {  // Huffman codes go MSB first
    uint32_t reversed = 0;
    for (int i = 0; i < n; ++i) reversed |= ((code >> i) & 1u) << (n - 1 - i);
    bits(reversed, n);
  }
  void flush() {
    if (count > 0) out.push_back(static_cast<uint8_t>(buffer & 0xff));
    buffer = 0;
    count = 0;
  }
};

constexpr std::array<uint16_t, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> kDistanceBase = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void literal(BitWriter& w, uint32_t symbol) {
  if (symbol < 144) w.huffman(0x30 + symbol, 8);
  else if (symbol < 256) w.huffman(0x190 + symbol - 144, 9);
  else if (symbol < 280) w.huffman(symbol - 256, 7);
  else w.huffman(0xc0 + symbol - 280, 8);
}

void match(BitWriter& w, uint32_t length, uint32_t distance) {
  size_t l = kLengthBase.size() - 1;
  while (kLengthBase[l] > length) --l;
  literal(w, 257 + static_cast<uint32_t>(l));
  if (kLengthExtra[l]) w.bits(length - kLengthBase[l], kLengthExtra[l]);
  size_t d = kDistanceBase.size() - 1;
  while (kDistanceBase[d] > distance) --d;
  w.huffman(static_cast<uint32_t>(d), 5);
  if (kDistanceExtra[d]) w.bits(distance - kDistanceBase[d], kDistanceExtra[d]);
}

// One fixed-Huffman block with greedy LZ77 over a 32 KiB window. Painted
// textures are mostly flat, so the Sub filter plus long matches keep them
// small; this is not meant to compete with zlib on photographs.
std::vector<uint8_t> deflate_fixed(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  BitWriter w{out};
  w.bits(1, 1);  // BFINAL
  w.bits(1, 2);  // BTYPE 01
  constexpr uint32_t kWindow = 32768, kHashSize = 1 << 15;
  constexpr int kChain = 24;
  std::vector<int32_t> head(kHashSize, -1), previous(data.size(), -1);
  auto hash = [&](size_t i) {
    return ((uint32_t(data[i]) << 10) ^ (uint32_t(data[i + 1]) << 5) ^ data[i + 2]) &
           (kHashSize - 1);
  };
  auto insert = [&](size_t i) {
    if (i + 2 >= data.size()) return;
    const uint32_t h = hash(i);
    previous[i] = head[h];
    head[h] = static_cast<int32_t>(i);
  };
  size_t i = 0;
  while (i < data.size()) {
    uint32_t best_length = 0, best_distance = 0;
    if (i + 2 < data.size()) {
      int32_t candidate = head[hash(i)];
      for (int chain = 0; candidate >= 0 && chain < kChain; ++chain) {
        const size_t distance = i - static_cast<size_t>(candidate);
        if (distance > kWindow) break;
        const size_t limit = std::min<size_t>(258, data.size() - i);
        size_t length = 0;
        while (length < limit && data[candidate + length] == data[i + length]) ++length;
        if (length > best_length) {
          best_length = static_cast<uint32_t>(length);
          best_distance = static_cast<uint32_t>(distance);
          if (length == limit) break;
        }
        candidate = previous[static_cast<size_t>(candidate)];
      }
    }
    if (best_length >= 3) {
      match(w, best_length, best_distance);
      for (uint32_t k = 0; k < best_length; ++k) insert(i + k);
      i += best_length;
    } else {
      literal(w, data[i]);
      insert(i);
      ++i;
    }
  }
  literal(w, 256);
  w.flush();
  return out;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
  put_be32(png, static_cast<uint32_t>(body.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), body.begin(), body.end());
  put_be32(png, crc32(png.data() + start, png.size() - start));
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
bool normalize3(float* v) {
  const float l = std::sqrt(dot3(v, v));
  if (!(l > 1e-8f) || !std::isfinite(l)) return false;
  for (int i = 0; i < 3; ++i) v[i] /= l;
  return true;
}

void sample(const PaintImage& image, float s, float t, float* rgba) {
  const float x = std::clamp(s * image.width - .5f, 0.f, float(image.width - 1));
  const float y = std::clamp(t * image.height - .5f, 0.f, float(image.height - 1));
  const uint32_t x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
  const uint32_t x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
  const float fx = x - x0, fy = y - y0;
  auto at = [&](uint32_t px, uint32_t py, int c) {
    return float(image.rgba[(size_t(py) * image.width + px) * 4 + c]);
  };
  for (int c = 0; c < 4; ++c)
    rgba[c] = (at(x0, y0, c) * (1 - fx) + at(x1, y0, c) * fx) * (1 - fy) +
              (at(x0, y1, c) * (1 - fx) + at(x1, y1, c) * fx) * fy;
}

}  // namespace

bool paint_texture_exists(uint32_t id) {
  return (id & 0xFFFF0000u) == kPaintTextureHigh && g_textures.count(id) != 0;
}

uint32_t paint_texture_create(PaintImage image) {
  if (!image.valid()) return 0;
  for (uint32_t tries = 0; tries < 0xFFFEu; ++tries) {
    const uint32_t local = g_next_local;
    g_next_local = g_next_local >= 0xFFFEu ? 1 : g_next_local + 1;
    const uint32_t id = kPaintTextureHigh | local;
    if (g_textures.count(id)) continue;
    Entry e;
    e.shown = image;
    e.committed = std::move(image);
    g_textures.emplace(id, std::move(e));
    return id;
  }
  return 0;
}

const PaintImage* paint_texture_image(uint32_t id) {
  const Entry* e = entry(id);
  return e ? &e->shown : nullptr;
}

const PaintImage* paint_texture_committed(uint32_t id) {
  const Entry* e = entry(id);
  return e ? &e->committed : nullptr;
}

uint32_t paint_texture_revision(uint32_t id) {
  const Entry* e = entry(id);
  return e ? e->revision : 0;
}

bool paint_texture_draw(uint32_t id, bool temp,
                        const std::function<void(PaintImage&)>& draw) {
  Entry* e = entry(id);
  if (!e) return false;
  if (temp) {
    e->shown = e->committed;
    draw(e->shown);
    e->temp = true;
  } else {
    draw(e->committed);
    e->shown = e->committed;
    e->temp = false;
  }
  ++e->revision;
  return true;
}

bool paint_texture_clear_temp(uint32_t id) {
  Entry* e = entry(id);
  if (!e || !e->temp) return false;
  e->shown = e->committed;
  e->temp = false;
  ++e->revision;
  return true;
}

bool paint_texture_has_temp(uint32_t id) {
  const Entry* e = entry(id);
  return e && e->temp;
}

void paint_texture_release(uint32_t id) {
  if (g_textures.erase(id)) g_released.push_back(id);
}

std::vector<uint32_t> paint_texture_take_released() {
  std::vector<uint32_t> out;
  out.swap(g_released);
  return out;
}

void paint_texture_clear_all() {
  for (const auto& [id, e] : g_textures) g_released.push_back(id);
  g_textures.clear();
}

size_t paint_texture_count() { return g_textures.size(); }

std::string paint_texture_path(uint32_t id) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "paint:%08x", id);
  return buf;
}

uint32_t paint_texture_from_path(const std::string& path) {
  if (path.size() != 14 || path.compare(0, 6, "paint:") != 0) return 0;
  uint32_t id = 0;
  for (size_t i = 6; i < path.size(); ++i) {
    const char c = path[i];
    const int v = c >= '0' && c <= '9' ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                         : -1;
    if (v < 0) return 0;
    id = (id << 4) | static_cast<uint32_t>(v);
  }
  return (id & 0xFFFF0000u) == kPaintTextureHigh ? id : 0;
}

bool paint_image_from_texture(const DdsImage& texture, PaintImage* out) {
  if (!out || !texture.ok || !texture.width || !texture.height) return false;
  if (texture.width > 4096 || texture.height > 4096) return false;
  if (texture.format == DdsFormat::DXT1 || texture.format == DdsFormat::DXT3 ||
      texture.format == DdsFormat::DXT5)
    return decode_bc(texture, out);
  std::string ext = texture.path.size() >= 4 ? texture.path.substr(texture.path.size() - 4) : "";
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const bool dds = ext == ".dds";
  const size_t n = size_t(texture.width) * texture.height;
  if (texture.format == DdsFormat::RGB8) {
    if (texture.data.size() < n * 3) return false;
    out->width = texture.width;
    out->height = texture.height;
    out->rgba.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* p = &texture.data[i * 3];
      uint8_t* q = &out->rgba[i * 4];
      q[0] = dds ? p[2] : p[0];
      q[1] = p[1];
      q[2] = dds ? p[0] : p[2];
      q[3] = 255;
    }
    return true;
  }
  if (texture.format != DdsFormat::RGBA8 || texture.data.size() < n * 4) return false;
  out->width = texture.width;
  out->height = texture.height;
  out->rgba.assign(texture.data.begin(), texture.data.begin() + static_cast<std::ptrdiff_t>(n * 4));
  for (size_t i = 0; i < n; ++i) {
    uint8_t* q = &out->rgba[i * 4];
    if (dds) {
      std::swap(q[0], q[2]);
    } else if (q[3] && q[3] < 255) {
      // load_texture_file premultiplies PNG/PTX for the OSD.
      for (int c = 0; c < 3; ++c)
        q[c] = static_cast<uint8_t>(std::min(255, (q[c] * 255 + q[3] / 2) / q[3]));
    }
  }
  return true;
}

std::vector<uint8_t> paint_png_encode(const PaintImage& image) {
  if (!image.valid()) return {};
  const size_t stride = size_t(image.width) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * image.height);
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(1);  // Sub
    const uint8_t* row = &image.rgba[y * stride];
    for (size_t x = 0; x < stride; ++x)
      raw.push_back(static_cast<uint8_t>(row[x] - (x >= 4 ? row[x - 4] : 0)));
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  const auto body = deflate_fixed(raw);
  z.insert(z.end(), body.begin(), body.end());
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<uint8_t> header;
  put_be32(header, image.width);
  put_be32(header, image.height);
  header.insert(header.end(), {8, 6, 0, 0, 0});
  chunk(png, "IHDR", header);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  return png;
}

bool paint_image_decode(const uint8_t* data, size_t size, PaintImage* out) {
  if (!data || !size || !out) return false;
  PaintImage image;
  if (!decode_image_rgba(data, size, &image.width, &image.height, &image.rgba) ||
      !image.valid() || image.width > 4096 || image.height > 4096)
    return false;
  *out = std::move(image);
  return true;
}

void paint_fill(PaintImage& image, uint32_t argb) {
  const uint8_t px[4] = {static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
                         static_cast<uint8_t>(argb), static_cast<uint8_t>(argb >> 24)};
  for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) std::memcpy(&image.rgba[i], px, 4);
}

size_t paint_stamp_mesh(PaintImage& target, const ScxGeometry& geometry, int paint_slot,
                        const PaintImage& brush, const PaintStamp& stamp) {
  if (!target.valid() || !brush.valid() || paint_slot < 0 || !(stamp.size > 1e-4f) ||
      !std::isfinite(stamp.size) || !std::isfinite(stamp.rotation))
    return 0;
  float n[3] = {stamp.normal[0], stamp.normal[1], stamp.normal[2]};
  if (!normalize3(n)) return 0;
  // In-plane axes: "up" is the mesh's +Y projected onto the surface, or +Z
  // on a roof or floor. The script's rotation turns them about the normal.
  float up[3] = {0.f, 1.f, 0.f};
  if (std::fabs(n[1]) > .9f) up[1] = 0.f, up[2] = 1.f;
  const float d = dot3(up, n);
  float b0[3] = {up[0] - n[0] * d, up[1] - n[1] * d, up[2] - n[2] * d};
  if (!normalize3(b0)) return 0;
  float t0[3];
  cross3(b0, n, t0);
  const float c = std::cos(stamp.rotation), s = std::sin(stamp.rotation);
  float t[3], b[3];
  for (int i = 0; i < 3; ++i) {
    t[i] = t0[i] * c + b0[i] * s;
    b[i] = b0[i] * c - t0[i] * s;
  }
  const float depth = std::max(stamp.size * .5f, .05f);
  const float color[4] = {float((stamp.argb >> 16) & 255), float((stamp.argb >> 8) & 255),
                          float(stamp.argb & 255), float(stamp.argb >> 24)};
  if (color[3] <= 0.f) return 0;

  const float scale = geometry.position_scale;
  const auto& pos = geometry.positions;
  const auto& normals = geometry.normals;
  const bool vertex_normals = normals.size() == pos.size();
  const size_t vertices = pos.size() / 3;
  const float w = float(target.width), h = float(target.height);
  std::vector<uint8_t> covered(size_t(target.width) * target.height, 0);
  size_t changed = 0;

  for (const ScxGroup& group : geometry.groups) {
    if (group.texture_index != static_cast<uint32_t>(paint_slot)) continue;
    const auto& uvs = group.base_map.channel == 2 ? geometry.uv3
                      : group.base_map.channel == 1 ? geometry.uv2
                                                    : geometry.uvs;
    if (uvs.size() < vertices * 2) continue;
    const uint32_t end = std::min<uint32_t>(group.index_start + group.index_count,
                                            static_cast<uint32_t>(geometry.indices.size()));
    for (uint32_t tri = group.index_start; tri + 2 < end; tri += 3) {
      uint32_t vi[3];
      bool bad = false;
      for (int k = 0; k < 3; ++k) {
        vi[k] = geometry.indices[tri + k];
        bad |= vi[k] >= vertices;
      }
      if (bad) continue;
      float p[3][3], decal[3][2], dist[3], uv[3][2];
      for (int k = 0; k < 3; ++k) {
        for (int a = 0; a < 3; ++a) p[k][a] = pos[vi[k] * 3 + a] * scale - stamp.hit[a];
        decal[k][0] = dot3(p[k], t) / stamp.size + .5f;
        decal[k][1] = .5f - dot3(p[k], b) / stamp.size;
        if (stamp.flip) decal[k][0] = 1.f - decal[k][0];
        dist[k] = dot3(p[k], n);
        uv[k][0] = (uvs[vi[k] * 2] * group.base_map.scale_u + group.base_map.offset_u) * w;
        uv[k][1] = (uvs[vi[k] * 2 + 1] * group.base_map.scale_v + group.base_map.offset_v) * h;
      }
      auto all = [&](auto pred) { return pred(0) && pred(1) && pred(2); };
      if (all([&](int k) { return dist[k] > depth; }) ||
          all([&](int k) { return dist[k] < -depth; }) ||
          all([&](int k) { return decal[k][0] < 0.f; }) ||
          all([&](int k) { return decal[k][0] > 1.f; }) ||
          all([&](int k) { return decal[k][1] < 0.f; }) ||
          all([&](int k) { return decal[k][1] > 1.f; }))
        continue;
      // Facing: authored normals when present, else either side of the
      // face (the depth slab still keeps the far side of the body out).
      float facing = 0.f;
      if (vertex_normals) {
        for (int k = 0; k < 3; ++k) facing += dot3(&normals[vi[k] * 3], n);
        facing /= 3.f;
      } else {
        float e1[3], e2[3], fn[3];
        for (int a = 0; a < 3; ++a) e1[a] = p[1][a] - p[0][a], e2[a] = p[2][a] - p[0][a];
        cross3(e1, e2, fn);
        facing = normalize3(fn) ? std::fabs(dot3(fn, n)) : 0.f;
      }
      if (facing < .2f) continue;

      const float area = (uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) -
                         (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]);
      if (!(std::fabs(area) > 1e-6f)) continue;
      float lo[2], hi[2];
      for (int a = 0; a < 2; ++a) {
        lo[a] = std::min({uv[0][a], uv[1][a], uv[2][a]}) - 1.f;
        hi[a] = std::max({uv[0][a], uv[1][a], uv[2][a]}) + 1.f;
      }
      if (hi[0] - lo[0] > 4.f * w || hi[1] - lo[1] > 4.f * h) continue;
      // Edge distances may be 0.7 px outside, so neighbouring UV islands
      // meet without a seam; the coverage mask keeps shared texels single.
      float slack[3];
      for (int k = 0; k < 3; ++k) {
        const float* a0 = uv[(k + 1) % 3];
        const float* a1 = uv[(k + 2) % 3];
        slack[k] = -.7f * std::hypot(a1[0] - a0[0], a1[1] - a0[1]) / std::fabs(area);
      }
      for (int y = int(std::floor(lo[1])); y <= int(std::ceil(hi[1])); ++y)
        for (int x = int(std::floor(lo[0])); x <= int(std::ceil(hi[0])); ++x) {
          const float cx = x + .5f, cy = y + .5f;
          float bary[3];
          for (int k = 0; k < 3; ++k) {
            const float* a0 = uv[(k + 1) % 3];
            const float* a1 = uv[(k + 2) % 3];
            bary[k] = ((a1[0] - a0[0]) * (cy - a0[1]) - (cx - a0[0]) * (a1[1] - a0[1])) / area;
          }
          if (bary[0] < slack[0] || bary[1] < slack[1] || bary[2] < slack[2]) continue;
          const float sd = bary[0] * decal[0][0] + bary[1] * decal[1][0] + bary[2] * decal[2][0];
          const float td = bary[0] * decal[0][1] + bary[1] * decal[1][1] + bary[2] * decal[2][1];
          if (sd < 0.f || sd > 1.f || td < 0.f || td > 1.f) continue;
          const int px = ((x % int(target.width)) + int(target.width)) % int(target.width);
          const int py = ((y % int(target.height)) + int(target.height)) % int(target.height);
          const size_t index = size_t(py) * target.width + size_t(px);
          if (covered[index]) continue;
          float src[4];
          sample(brush, sd, td, src);
          const float alpha = src[3] / 255.f * color[3] / 255.f;
          if (alpha <= 0.f) continue;
          covered[index] = 1;
          uint8_t* dst = &target.rgba[index * 4];
          for (int k = 0; k < 3; ++k) {
            const float value = src[k] * color[k] / 255.f;
            dst[k] = static_cast<uint8_t>(std::clamp(dst[k] + (value - dst[k]) * alpha + .5f, 0.f, 255.f));
          }
          dst[3] = static_cast<uint8_t>(std::clamp(dst[3] + (255.f - dst[3]) * alpha + .5f, 0.f, 255.f));
          ++changed;
        }
    }
  }
  return changed;
}

}  // namespace slrr
