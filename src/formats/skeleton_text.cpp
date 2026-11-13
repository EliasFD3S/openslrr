#include "slrr/format/skl.hpp"
#include "slrr/format/bon.hpp"
#include "slrr/skeleton.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace slrr {
namespace {

void fail(std::string* error, const char* text) {
  if (error && error->empty()) *error = text;
}

bool parse_numbers(std::string_view line, float* out, int count) {
  int found = 0;
  size_t cursor = 0;
  while (found < count) {
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
    if (cursor >= line.size()) return false;
    size_t end = cursor;
    while (end < line.size() && line[end] != ',') ++end;
    std::string_view token = line.substr(cursor, end - cursor);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
    if (token.empty()) return false;
    float value = 0.f;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        !std::isfinite(value))
      return false;
    out[found++] = value;
    cursor = end < line.size() ? end + 1 : end;
  }
  while (cursor < line.size() &&
         (line[cursor] == ' ' || line[cursor] == '\t' || line[cursor] == ','))
    ++cursor;
  return cursor == line.size();
}

bool parse_index(std::string_view line, int* out) {
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
  if (line.empty()) return false;
  int value = 0;
  const auto parsed = std::from_chars(line.data(), line.data() + line.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()) return false;
  *out = value;
  return true;
}

bool hierarchy_ok(Skeleton* skeleton, std::string* error) {
  const int count = static_cast<int>(skeleton->bones.size());
  std::vector<uint8_t> state(static_cast<size_t>(count), 0);
  skeleton->order.clear();
  skeleton->order.reserve(static_cast<size_t>(count));
  struct Frame { int index; bool leave; };
  std::vector<Frame> stack;
  for (int start = 0; start < count; ++start) {
    if (state[static_cast<size_t>(start)]) continue;
    stack.push_back({start, false});
    while (!stack.empty()) {
      const Frame frame = stack.back();
      stack.pop_back();
      if (frame.leave) {
        state[static_cast<size_t>(frame.index)] = 2;
        skeleton->order.push_back(frame.index);
        continue;
      }
      const uint8_t seen = state[static_cast<size_t>(frame.index)];
      if (seen == 2) continue;
      if (seen == 1) {
        fail(error, "cyclic skeleton");
        return false;
      }
      const int parent = skeleton->bones[static_cast<size_t>(frame.index)].parent;
      if (parent >= count) {
        fail(error, "skeleton parent out of range");
        return false;
      }
      state[static_cast<size_t>(frame.index)] = 1;
      stack.push_back({frame.index, true});
      if (parent >= 0) stack.push_back({parent, false});
    }
  }
  return static_cast<int>(skeleton->order.size()) == count;
}

bool read_bones(std::string_view text, Skeleton* out, std::string* error) {
  if (!out) return false;
  *out = {};
  if (error) error->clear();
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
      static_cast<unsigned char>(text[1]) == 0xbb &&
      static_cast<unsigned char>(text[2]) == 0xbf)
    text.remove_prefix(3);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  std::vector<std::string_view> lines;
  size_t cursor = 0;
  while (cursor < text.size()) {
    size_t end = text.find('\n', cursor);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(cursor, end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    lines.push_back(line);
    cursor = end < text.size() ? end + 1 : end;
  }
  if (lines.empty() || lines.size() % 9 != 0 || lines.size() > 9 * 4096) {
    fail(error, "incomplete skeleton");
    return false;
  }
  for (size_t i = 0; i < lines.size(); i += 9) {
    SkeletonBone bone;
    bone.name.assign(lines[i]);
    if (bone.name.empty()) {
      fail(error, "empty bone name");
      return false;
    }
    for (const SkeletonBone& prior : out->bones)
      if (prior.name == bone.name) {
        fail(error, "duplicate bone name");
        return false;
      }
    if (!parse_index(lines[i + 1], &bone.parent) || bone.parent < -1) {
      fail(error, "invalid bone parent");
      return false;
    }
    float rotation[9];
    for (int row = 0; row < 3; ++row)
      if (!parse_numbers(lines[i + 2 + static_cast<size_t>(row)], rotation + row * 3, 3)) {
        fail(error, "invalid bone rotation");
        return false;
      }
    std::memcpy(bone.rotation, rotation, sizeof(rotation));
    if (!parse_numbers(lines[i + 5], bone.position, 3)) {
      fail(error, "invalid bone position");
      return false;
    }
    for (int row = 0; row < 3; ++row) {
      float pair[2];
      if (!parse_numbers(lines[i + 6 + static_cast<size_t>(row)], pair, 2)) {
        fail(error, "invalid bone limits");
        return false;
      }
      bone.limit_min[row] = pair[0];
      bone.limit_max[row] = pair[1];
    }
    out->bones.push_back(std::move(bone));
  }
  return hierarchy_ok(out, error);
}

}  // namespace

bool skl_read(std::string_view text, Skeleton* out, std::string* error) {
  return read_bones(text, out, error);
}

bool bon_read(std::string_view text, Skeleton* out, std::string* error) {
  return read_bones(text, out, error);
}

}  // namespace slrr
