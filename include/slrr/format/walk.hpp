#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace slrr {

// One point of a city.walk record, in the file's metres.
struct WalkVec {
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
};

// A sidewalk run node: a spline point and its stored tangent.
struct WalkNode {
  WalkVec point;
  WalkVec tangent;
};

// One edge of a walk row. Layout read by the original at 0x00586580:
// u32 kind; for kind 23, u32 N and N + 1 nodes; then x0, z0, x1, z1 and an
// int id. The engine derives the role from the id: -1 is a wall (kind 21,
// or the run itself for kind 23), any other value is a gate into that row.
struct WalkRecord {
  int kind = 0;
  std::vector<WalkNode> nodes;
  float quad[4]{};
  int id = -1;
};

// One line of the file. Junction rows store their edges relative to
// (origin_x, origin_z) and their floor as the plane
// y = slope_x * (x - origin_x) + slope_z * (z - origin_z) + height.
// Corridor rows leave the header at zero and use world coordinates.
struct WalkRow {
  int id = 0;
  float box[6]{};
  float origin_x = 0.f;
  float origin_z = 0.f;
  float slope_x = 0.f;
  float slope_z = 0.f;
  float height = 0.f;
  std::vector<WalkRecord> records;
  // Rows whose edges and walkers this row interacts with, itself included.
  std::vector<int> links;
};

bool walk_read(std::string_view text, std::vector<WalkRow>* out, std::string* error);

}  // namespace slrr
