#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// One .matrixdata clip: a time column, then a quaternion and a direction
// per bone per key, then the bone names.
struct ClipTrack {
  std::string name;
  std::vector<float> rotation;
  std::vector<float> direction;
};

struct Clip {
  std::vector<float> times;
  std::vector<ClipTrack> tracks;
};

bool matrixdata_read(const uint8_t* data, size_t size, Clip* out, std::string* error);

}  // namespace slrr
