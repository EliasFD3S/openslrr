#include "slrr/format/trc.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace slrr {
namespace {

struct Cursor {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t at = 0;
  void need(size_t count) const {
    if (at + count > size) throw std::runtime_error("TRC: truncated record at " + std::to_string(at));
  }
  uint32_t u32() {
    need(4);
    uint32_t value = 0;
    std::memcpy(&value, data + at, 4);
    at += 4;
    return value;
  }
  uint8_t u8() {
    need(1);
    return data[at++];
  }
  float word_float(uint32_t word) const {
    float value = 0.f;
    std::memcpy(&value, &word, 4);
    return value;
  }
  uint32_t count(size_t minimum) {
    const uint32_t value = u32();
    const size_t room = minimum == 0 ? 0 : (size - at) / minimum;
    if (value > room) throw std::runtime_error("TRC: invalid count");
    return value;
  }
  std::vector<uint32_t> words(size_t count) {
    std::vector<uint32_t> out(count);
    for (uint32_t& value : out) value = u32();
    return out;
  }
};

RoadPoint point_at(const std::vector<uint32_t>& raw, size_t start) {
  if (start + 3 > raw.size()) throw std::runtime_error("TRC: non-finite geometry");
  RoadPoint point{};
  for (int i = 0; i < 3; ++i) {
    std::memcpy(&point[static_cast<size_t>(i)], &raw[start + static_cast<size_t>(i)], 4);
    if (!std::isfinite(point[static_cast<size_t>(i)]))
      throw std::runtime_error("TRC: non-finite geometry");
  }
  return point;
}

RoadSegment read_segment(Cursor& cursor) {
  RoadSegment segment;
  segment.raw = cursor.words(14);
  segment.flag = cursor.u8();
  std::memcpy(&segment.length, &segment.raw[12], 4);
  if (!std::isfinite(segment.length) || segment.length < 0.f)
    throw std::runtime_error("TRC: invalid segment length");
  segment.start = point_at(segment.raw, 0);
  segment.end = point_at(segment.raw, 3);
  segment.start_tangent = point_at(segment.raw, 6);
  segment.end_tangent = point_at(segment.raw, 9);
  return segment;
}

}  // namespace

bool trc_read(const uint8_t* data, size_t size, RoadNetwork* out, std::string* error) {
  if (!out) return false;
  *out = {};
  if (error) error->clear();
  try {
    Cursor cursor{data, size, 0};
    const uint32_t version = cursor.u32();
    if (version != 1) throw std::runtime_error("TRC: unsupported version " + std::to_string(version));
    uint32_t counts[4];
    uint32_t total = 0;
    for (uint32_t& count : counts) {
      count = cursor.count(4);
      total += count;
    }
    if (total > size / 4) throw std::runtime_error("TRC: invalid record total");
    auto ref = [&]() {
      const uint32_t id = cursor.u32();
      if (id > total) throw std::runtime_error("TRC: reference " + std::to_string(id) + " outside record table");
      return id;
    };
    uint32_t id = 0;
    for (int group = 0; group < 2; ++group) {
      for (uint32_t i = 0; i < counts[group]; ++i) {
        RoadNode node;
        node.id = ++id;
        node.group = group;
        const uint32_t links = cursor.count(5);
        for (uint32_t link = 0; link < links; ++link)
          node.links.push_back({ref(), cursor.u8()});
        const uint32_t lanes = cursor.count(69);
        for (uint32_t lane = 0; lane < lanes; ++lane) {
          cursor.need(12);
          RoadLane item;
          std::memcpy(item.header, cursor.data + cursor.at, 12);
          cursor.at += 12;
          item.segment = read_segment(cursor);
          node.lanes.push_back(std::move(item));
        }
        node.raw = cursor.words(4);
        node.position = point_at(node.raw, 0);
        out->nodes.push_back(std::move(node));
      }
    }
    for (uint32_t i = 0; i < counts[2]; ++i) {
      RoadPath path;
      path.id = ++id;
      for (uint32_t& item : path.refs) item = ref();
      path.raw = cursor.words(7);
      const uint32_t segments = cursor.count(57);
      if (segments == 0) throw std::runtime_error("TRC: empty spline");
      for (uint32_t segment = 0; segment < segments; ++segment)
        path.segments.push_back(read_segment(cursor));
      out->paths.push_back(std::move(path));
    }
    for (uint32_t i = 0; i < counts[3]; ++i) {
      RoadOther other;
      other.id = ++id;
      other.refs[0] = ref();
      other.refs[1] = ref();
      other.flag = cursor.u8();
      other.raw = cursor.words(12);
      for (uint32_t& item : other.extra_refs) item = ref();
      other.tail = cursor.words(3);
      const uint32_t segments = cursor.count(57);
      if (segments == 0) throw std::runtime_error("TRC: empty spline");
      for (uint32_t segment = 0; segment < segments; ++segment)
        other.segments.push_back(read_segment(cursor));
      out->others.push_back(std::move(other));
    }
    if (cursor.at != size)
      throw std::runtime_error("TRC: " + std::to_string(size - cursor.at) + " trailing bytes");
    out->version = version;
    out->bytes = size;
    return true;
  } catch (const std::exception& failure) {
    if (error && error->empty()) *error = failure.what();
    *out = {};
    return false;
  }
}

RoadPoint trc_sample(const RoadSegment& segment, float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  RoadPoint out{};
  for (int i = 0; i < 3; ++i) {
    const size_t axis = static_cast<size_t>(i);
    out[axis] = (2.f * t3 - 3.f * t2 + 1.f) * segment.start[axis] +
                (t3 - 2.f * t2 + t) * segment.start_tangent[axis] +
                (-2.f * t3 + 3.f * t2) * segment.end[axis] +
                (t3 - t2) * segment.end_tangent[axis];
  }
  return out;
}

}  // namespace slrr
