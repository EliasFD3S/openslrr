#include "slrr/format/matrixdata.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slrr {
namespace {

void fail(std::string* error, const char* text) {
  if (error && error->empty()) *error = text;
}

}  // namespace

bool matrixdata_read(const uint8_t* data, size_t size, Clip* out, std::string* error) {
  if (!out) return false;
  *out = {};
  if (error) error->clear();
  auto word = [&](uint32_t* value) {
    if (!data || size < 4) return false;
    std::memcpy(value, data, 4);
    data += 4;
    size -= 4;
    return true;
  };
  auto floats = [&](float* dest, int count) {
    const size_t bytes = static_cast<size_t>(count) * 4;
    if (size < bytes) return false;
    std::memcpy(dest, data, bytes);
    data += bytes;
    size -= bytes;
    return true;
  };
  uint32_t count = 0;
  if (!word(&count) || count < 1 || count > 4096) {
    fail(error, "Invalid matrixdata bone count");
    return false;
  }
  uint32_t keys = 0;
  out->tracks.resize(count);
  for (uint32_t bone = 0; bone < count; ++bone) {
    uint32_t declared = 0;
    if (!word(&declared) || declared < 1) {
      fail(error, "Invalid matrixdata key count");
      return false;
    }
    if (bone == 0) {
      keys = declared;
      out->times.resize(keys);
    } else if (declared != keys) {
      fail(error, "Inconsistent matrixdata key counts");
      return false;
    }
    ClipTrack& track = out->tracks[bone];
    track.rotation.resize(static_cast<size_t>(keys) * 4);
    track.direction.resize(static_cast<size_t>(keys) * 3);
    for (uint32_t key = 0; key < keys; ++key) {
      float values[8];
      if (!floats(values, 8)) {
        fail(error, "Truncated matrixdata keys");
        return false;
      }
      for (float value : values)
        if (!std::isfinite(value)) {
          fail(error, "Non-finite matrixdata key");
          return false;
        }
      if (bone == 0) {
        if (values[0] < 0.f || (key > 0 && values[0] <= out->times[key - 1])) {
          fail(error, "Invalid matrixdata key times");
          return false;
        }
        out->times[key] = values[0];
      } else if (std::fabs(out->times[key] - values[0]) > 1e-5f) {
        fail(error, "Inconsistent matrixdata key times");
        return false;
      }
      const float norm = std::sqrt(values[1] * values[1] + values[2] * values[2] +
                                   values[3] * values[3] + values[4] * values[4]);
      if (std::fabs(norm - 1.f) > 0.01f) {
        fail(error, "Invalid matrixdata quaternion");
        return false;
      }
      std::memcpy(track.rotation.data() + static_cast<size_t>(key) * 4, values + 1,
                  sizeof(float) * 4);
      std::memcpy(track.direction.data() + static_cast<size_t>(key) * 3, values + 5,
                  sizeof(float) * 3);
    }
  }
  std::string names(reinterpret_cast<const char*>(data), size);
  names.erase(std::remove(names.begin(), names.end(), '\r'), names.end());
  if (!names.empty() && names.back() == '\n') names.pop_back();
  size_t cursor = 0;
  for (uint32_t bone = 0; bone < count; ++bone) {
    const size_t end = names.find('\n', cursor);
    const size_t stop = end == std::string::npos ? names.size() : end;
    std::string name = names.substr(cursor, stop - cursor);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t')) ++start;
    if (start) name.erase(0, start);
    if (name.empty()) {
      fail(error, "Invalid matrixdata bone names");
      return false;
    }
    for (uint32_t prior = 0; prior < bone; ++prior)
      if (out->tracks[prior].name == name) {
        fail(error, "Invalid matrixdata bone names");
        return false;
      }
    out->tracks[bone].name = std::move(name);
    if (end == std::string::npos) {
      if (bone + 1 != count) {
        fail(error, "Invalid matrixdata bone names");
        return false;
      }
      return true;
    }
    cursor = end + 1;
  }
  if (cursor < names.size()) {
    const std::string tail = names.substr(cursor);
    if (tail.find_first_not_of(" \t\n") != std::string::npos) {
      fail(error, "Invalid matrixdata bone names");
      return false;
    }
  }
  return true;
}

}  // namespace slrr
