#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

using RoadPoint = std::array<float, 3>;

struct RoadSegment {
  RoadPoint start{};
  RoadPoint end{};
  RoadPoint start_tangent{};
  RoadPoint end_tangent{};
  float length = 0.f;
  std::vector<uint32_t> raw;
  uint8_t flag = 0;
};

struct RoadLink {
  uint32_t target = 0;
  uint8_t flag = 0;
};

struct RoadLane {
  uint8_t header[12]{};
  RoadSegment segment;
};

struct RoadNode {
  uint32_t id = 0;
  int group = 0;
  RoadPoint position{};
  std::vector<RoadLink> links;
  std::vector<RoadLane> lanes;
  std::vector<uint32_t> raw;
};

struct RoadPath {
  uint32_t id = 0;
  uint32_t refs[3]{};
  std::vector<uint32_t> raw;
  std::vector<RoadSegment> segments;
};

struct RoadOther {
  uint32_t id = 0;
  uint32_t refs[2]{};
  uint8_t flag = 0;
  std::vector<uint32_t> raw;
  uint32_t extra_refs[4]{};
  std::vector<uint32_t> tail;
  std::vector<RoadSegment> segments;
};

struct RoadNetwork {
  uint32_t version = 0;
  std::vector<RoadNode> nodes;
  std::vector<RoadPath> paths;
  std::vector<RoadOther> others;
  size_t bytes = 0;
};

bool trc_read(const uint8_t* data, size_t size, RoadNetwork* out, std::string* error);
RoadPoint trc_sample(const RoadSegment& segment, float t);

}  // namespace slrr
