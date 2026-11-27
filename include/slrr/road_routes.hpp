#pragma once
#include "slrr/format/trc.hpp"
#include <optional>
#include <unordered_map>
namespace slrr {
struct RoadRoute {
  float length = 0;
  RoadPoint direction{};
  std::vector<RoadPoint> points;
  std::vector<uint32_t> roads;
};
struct RoadProjection {
  RoadPoint position{}, direction{};
  float distance = 0, along = 0;
  size_t edge = 0;
};
// Port of SLRR_rev v221 cbb99147: engine/road-route.ts. Sampled host model,
// not recovered original solver. Lane crossing links establish direction.
class RoadRoutes {
public:
  struct Edge {
    uint32_t id = 0, from = 0, to = 0;
    std::vector<RoadPoint> points;
    std::vector<float> distances;
    float length = 0;
    std::vector<uint32_t> roads;
    std::vector<size_t>
        next; // Explicit RoadOther -> grey crossing -> RoadOther joins.
    bool crossing = false;
    uint32_t branch = 0;
    // SLRR_rev road-traffic.ts speed group: 0 rural (a yellow TRC node at
    // either end of the road, or a rural junction), 1 urban, -1 unknown.
    int speed_group = -1;
  };
  struct Signal {
    RoadPoint position{};
    uint32_t crossing = 0, branch = 0;
  };
  explicit RoadRoutes(const RoadNetwork &network,
                      const std::vector<RoadPoint> &signals = {});
  const std::vector<Signal> &signals() const { return signals_; }
  // -2: missing lamp on a regulated approach (timed STOP); -1: unregulated.
  int signal(uint32_t crossing, uint32_t branch, double seconds) const;
  int edge_signal(size_t edge, double seconds) const;

  std::optional<RoadRoute> query(const RoadPoint &, const RoadPoint &) const;
  std::optional<RoadProjection>
  project(const RoadPoint &, const RoadPoint *heading = nullptr) const;
  std::optional<RoadPoint> nearest_cross(const RoadPoint &,
                                         float radius = 0) const;
  const std::vector<Edge> &edges() const { return edges_; }
  const std::vector<Edge> &traffic_edges() const { return traffic_edges_; }
  std::optional<RoadProjection>
  traffic_project(const RoadPoint &, const RoadPoint *heading = nullptr,
                  bool include_crossings = false) const;
  int traffic_signal(size_t edge, double seconds) const;
  // Host speed zone (SLRR_rev road-speed-zones.ts): the upper speed band of
  // the nearest lane or junction curve within 30 m, in m/s; -1 off the roads.
  float speed_limit(const RoadPoint &) const;

private:
  struct Anchor {
    uint32_t node = 0;
    std::optional<RoadProjection> projection;
    float distance = 0;
  };
  std::vector<Edge> edges_, traffic_edges_;
  std::vector<Signal> signals_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> cycles_;
  std::vector<std::pair<uint32_t, RoadPoint>> nodes_;
  std::unordered_map<uint32_t, float> radii_;
  std::unordered_map<uint64_t, std::vector<std::pair<size_t, size_t>>> cells_;
  std::unordered_map<uint64_t, std::vector<std::pair<size_t, size_t>>>
      traffic_cells_;
  bool complete_grid_ = true, traffic_complete_grid_ = true;
  Anchor anchor(const RoadPoint &) const;
  std::optional<RoadRoute> between(const RoadPoint &, const RoadPoint &,
                                   const Anchor &, const Anchor &) const;
};
// SLRR_rev trafficSpeedBand, km/h: rural 60..100, urban (and unknown) 15..35.
std::array<float, 2> road_speed_band(int speed_group);
float road_distance(const RoadPoint &, const RoadPoint &);
RoadPoint road_normalize(RoadPoint);
RoadPoint road_at(const RoadRoute &, float distance);
} // namespace slrr
