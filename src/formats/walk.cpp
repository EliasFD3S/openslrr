#include "slrr/format/walk.hpp"

#include <cmath>
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

bool read_record(Reader& reader, WalkRecord* out, int depth) {
  if (depth > 8) {
    reader.fail("Pedestrian link nesting too deep");
    return false;
  }
  const uint32_t kind = reader.word();
  if (!reader.ok) return false;
  out->kind = static_cast<int>(kind);
  if (kind == 21) {
    for (int i = 0; i < 4; ++i) out->quad.push_back(reader.flt());
    return reader.ok;
  }
  if (kind == 23) {
    const uint32_t count = reader.word();
    if (reader.ok && (count < 3 || count > 6)) {
      reader.fail("Invalid pedestrian waypoint count");
      return false;
    }
    for (uint32_t i = 0; reader.ok && i <= count; ++i) {
      WalkVec point{reader.flt(), reader.flt(), reader.flt()};
      WalkVec direction{reader.flt(), reader.flt(), reader.flt()};
      out->points.push_back(point);
      out->directions.push_back(direction);
    }
    for (int i = 0; i < 4; ++i) out->quad.push_back(reader.flt());
    if (!reader.ok) return false;
    const WalkVec& first = out->points.front();
    const WalkVec& last = out->points.back();
    const float ends[4] = {first.x, first.z, last.x, last.z};
    for (int i = 0; i < 4; ++i)
      if (std::fabs(out->quad[static_cast<size_t>(i)] - ends[i]) > 0.01f) {
        reader.fail("Pedestrian sidewalk endpoints disagree");
        return false;
      }
    return true;
  }
  if (kind != 22) {
    reader.fail("Unknown pedestrian record type");
    return false;
  }
  for (int i = 0; i < 4; ++i) out->quad.push_back(reader.flt());
  out->id = static_cast<int>(reader.word());
  out->has_id = reader.ok;
  if (!reader.ok) return false;
  if (reader.at + 4 <= reader.size) {
    uint32_t type = 0;
    std::memcpy(&type, reader.data + reader.at, 4);
    if (type >= 21 && type <= 23) {
      out->child = std::make_unique<WalkRecord>();
      return read_record(reader, out->child.get(), depth + 1);
    }
  }
  while (reader.ok && reader.at + 4 <= reader.size)
    out->links.push_back(static_cast<int>(reader.word()));
  if (!reader.ok) return false;
  if (out->links.size() < 4 || out->links.size() > 6) {
    reader.fail("Invalid pedestrian leaf count");
    return false;
  }
  return true;
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
    Reader whole{bytes.data(), bytes.size(), 0, true, error};
    if (whole.word() != bytes.size() - 4) {
      fail(error, "Pedestrian payload length mismatch");
      return false;
    }
    std::vector<std::pair<size_t, size_t>> sections;
    size_t section = 4;
    for (size_t at = 4; at + 4 <= bytes.size(); at += 4) {
      uint32_t word = 0;
      std::memcpy(&word, bytes.data() + at, 4);
      if (word == 0xffffffffu) {
        sections.emplace_back(section, at);
        section = at + 4;
      }
    }
    if (sections.empty()) {
      fail(error, "Pedestrian first record has no terminator");
      return false;
    }
    {
      Reader first{bytes.data() + sections[0].first, sections[0].second - sections[0].first, 0,
                   true, error};
      for (float& value : row.origin) value = first.flt();
      row.width = first.flt();
      const uint32_t tag = first.word();
      row.tag = static_cast<int>(tag);
      if (first.ok && tag != 3 && tag != 4) {
        fail(error, "Invalid pedestrian first tag");
        return false;
      }
      row.records.emplace_back();
      if (!read_record(first, &row.records.back(), 0) || !first.finish()) {
        if (error && error->empty()) fail(error, "Unexplained pedestrian record bytes");
        return false;
      }
    }
    for (size_t i = 1; i < sections.size(); ++i) {
      Reader mid{bytes.data() + sections[i].first, sections[i].second - sections[i].first, 0,
                 true, error};
      row.records.emplace_back();
      if (!read_record(mid, &row.records.back(), 0) || !mid.finish()) {
        if (error && error->empty()) fail(error, "Unexplained pedestrian record bytes");
        return false;
      }
    }
    Reader tail{bytes.data() + section, bytes.size() - section, 0, true, error};
    const uint32_t type = tail.word();
    tail.at = 0;
    if (type >= 1 && type <= 7 && tail.size == 4u + type * 4u) {
      tail.word();
      for (uint32_t i = 0; tail.ok && i < type; ++i)
        row.links.push_back(static_cast<int>(tail.word()));
    } else if (type == 22) {
      row.records.emplace_back();
      if (!read_record(tail, &row.records.back(), 0)) return false;
    } else {
      fail(error, "Invalid pedestrian tail");
      return false;
    }
    if (!tail.finish()) {
      if (error && error->empty()) fail(error, "Unexplained pedestrian record bytes");
      return false;
    }
    auto collect = [&](auto& self, const WalkRecord& record) -> void {
      row.links.insert(row.links.end(), record.links.begin(), record.links.end());
      if (record.child) self(self, *record.child);
    };
    for (const WalkRecord& record : row.records) collect(collect, record);
    out->push_back(std::move(row));
  }
  std::unordered_set<int> ids;
  for (const WalkRow& row : *out)
    if (!ids.insert(row.id).second) {
      fail(error, "Duplicate pedestrian walk ID");
      out->clear();
      return false;
    }
  for (const WalkRow& row : *out)
    for (int link : row.links)
      if (!ids.count(link)) {
        fail(error, "Pedestrian link outside walk table");
        out->clear();
        return false;
      }
  return true;
}

}  // namespace slrr
