#include "slrr/format/walk.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>

namespace slrr {
namespace {

void fail(std::string* error, const char* text) {
  if (error && error->empty()) *error = text;
}

struct Reader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t at = 0;
  bool ok = true;
  std::string* error = nullptr;
  void fail(const char* text) {
    ok = false;
    if (error && error->empty()) *error = text;
  }
  uint32_t word() {
    if (!ok) return 0;
    if (at + 4 > size) {
      fail("Truncated pedestrian record");
      return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, data + at, 4);
    at += 4;
    return value;
  }
  float flt() {
    const uint32_t bits = word();
    if (!ok) return 0.f;
    float value = 0.f;
    std::memcpy(&value, &bits, 4);
    if (!std::isfinite(value)) fail("Non-finite pedestrian coordinate");
    return value;
  }
  bool finish() const { return ok && at == size; }
};

// Record layout of the original reader (0x00586580).
bool read_record(Reader& reader, WalkRecord* out) {
  const uint32_t kind = reader.word();
  if (!reader.ok) return false;
  if (kind != 21 && kind != 22 && kind != 23) {
    reader.fail("Unknown pedestrian record type");
    return false;
  }
  out->kind = static_cast<int>(kind);
  if (kind == 23) {
    const uint32_t count = reader.word();
    // Stock runs hold 3 to 6 spans. A bound keeps a corrupt count from
    // reserving the whole payload.
    if (reader.ok && (count < 1 || count > 64)) {
      reader.fail("Invalid pedestrian waypoint count");
      return false;
    }
    for (uint32_t i = 0; reader.ok && i <= count; ++i) {
      WalkNode node;
      node.point = {reader.flt(), reader.flt(), reader.flt()};
      node.tangent = {reader.flt(), reader.flt(), reader.flt()};
      out->nodes.push_back(node);
    }
  }
  for (float& value : out->quad) value = reader.flt();
  out->id = static_cast<int32_t>(reader.word());
  return reader.ok;
}

bool header(std::string_view line, float box[6], int* id, std::string_view* payload,
            std::string* error) {
  const size_t semi = line.find(';');
  const size_t space = line.rfind(' ', semi == std::string_view::npos ? line.size() : semi);
  if (semi == std::string_view::npos || space == std::string_view::npos || space + 1 >= semi) {
    fail(error, "Invalid pedestrian line header");
    return false;
  }
  const std::string_view box_text = line.substr(0, space);
  size_t at = 0;
  for (int i = 0; i < 6; ++i) {
    size_t stop = 0;
    try {
      box[i] = std::stof(std::string(box_text.substr(at)), &stop);
    } catch (...) {
      fail(error, "Invalid pedestrian bounds");
      return false;
    }
    if (!std::isfinite(box[i])) {
      fail(error, "Invalid pedestrian bounds");
      return false;
    }
    at += stop;
    if (i < 5) {
      if (at >= box_text.size() || box_text[at] != ',') {
        fail(error, "Invalid pedestrian bounds");
        return false;
      }
      ++at;
    }
  }
  if (at != box_text.size() || box[0] > box[3] || box[2] > box[5]) {
    fail(error, "Invalid pedestrian bounds");
    return false;
  }
  try {
    *id = std::stoi(std::string(line.substr(space + 1, semi - space - 1)));
  } catch (...) {
    fail(error, "Invalid pedestrian record ID");
    return false;
  }
  if (*id < 1) {
    fail(error, "Invalid pedestrian record ID");
    return false;
  }
  *payload = line.substr(semi + 1);
  return true;
}

}  // namespace

bool walk_read(std::string_view text, std::vector<WalkRow>* out, std::string* error) {
  if (!out) return false;
  out->clear();
  if (error) error->clear();
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
      static_cast<unsigned char>(text[1]) == 0xbb &&
      static_cast<unsigned char>(text[2]) == 0xbf)
    text.remove_prefix(3);
  size_t cursor = 0;
  while (cursor <= text.size()) {
    if (cursor == text.size()) break;
    size_t end = text.find('\n', cursor);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(cursor, end - cursor);
    cursor = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    if (line.empty()) continue;
    WalkRow row;
    std::string_view payload;
    if (!header(line, row.box, &row.id, &payload, error)) return false;
    if (payload.size() % 2 || payload.empty()) {
      fail(error, "Invalid pedestrian nibble payload");
      return false;
    }
    std::vector<uint8_t> bytes(payload.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
      const unsigned hi = static_cast<unsigned char>(payload[i * 2]);
      const unsigned lo = static_cast<unsigned char>(payload[i * 2 + 1]);
      if (hi < 'A' || hi > 'P' || lo < 'A' || lo > 'P') {
        fail(error, "Invalid pedestrian nibble payload");
        return false;
      }
      bytes[i] = static_cast<uint8_t>((hi - 'A') * 16 + (lo - 'A'));
    }
    Reader reader{bytes.data(), bytes.size(), 0, true, error};
    if (reader.word() != bytes.size() - 4) {
      fail(error, "Pedestrian payload length mismatch");
      return false;
    }
    row.origin_x = reader.flt();
    row.origin_z = reader.flt();
    row.slope_x = reader.flt();
    row.slope_z = reader.flt();
    row.height = reader.flt();
    const uint32_t records = reader.word();
    if (reader.ok && records > 64) {
      fail(error, "Invalid pedestrian record count");
      return false;
    }
    for (uint32_t i = 0; reader.ok && i < records; ++i) {
      row.records.emplace_back();
      if (!read_record(reader, &row.records.back())) break;
    }
    const uint32_t links = reader.word();
    if (reader.ok && links > 64) {
      fail(error, "Invalid pedestrian link count");
      return false;
    }
    for (uint32_t i = 0; reader.ok && i < links; ++i)
      row.links.push_back(static_cast<int32_t>(reader.word()));
    if (!reader.finish()) {
      fail(error, "Unexplained pedestrian record bytes");
      return false;
    }
    out->push_back(std::move(row));
  }
  std::unordered_set<int> ids;
  for (const WalkRow& row : *out)
    if (!ids.insert(row.id).second) {
      fail(error, "Duplicate pedestrian walk ID");
      out->clear();
      return false;
    }
  // Gate and link ids are resolved when rows load; the engine ignores a row
  // that is not there, so an unknown id is not a format error.
  return true;
}

}  // namespace slrr
