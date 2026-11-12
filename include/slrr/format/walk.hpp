#pragma once

#include <memory>
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

// Kind 21 is a wall quad, 22 a gate, 23 a sidewalk run.
// The layout is the one SLRR_rev reads in formats/pedestrian.ts.
struct WalkRecord {
  int kind = 0;
  std::vector<float> quad;
  int id = 0;
  bool has_id = false;
  std::vector<WalkVec> points;
  std::vector<WalkVec> directions;
  std::unique_ptr<WalkRecord> child;
  std::vector<int> links;
};

struct WalkRow {
  int id = 0;
  float box[6]{};
  float origin[4]{};
  float width = 0.f;
  int tag = 0;
  std::vector<WalkRecord> records;
  std::vector<int> links;
};

bool walk_read(std::string_view text, std::vector<WalkRow>* out, std::string* error);

}  // namespace slrr
