#pragma once
// Recorded drives for repeatable performance runs (#358).
//
// --record-path writes the player chassis' published local matrix every
// frame of normal play, stamped with the simulated time. --replay-path makes
// the benchmark drive move the car along that recording at the fixed 1/60 s
// step instead of driving it, so every build renders the same street, the
// same turns and the same camera path, whatever the physics does.

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace slrr {

struct DrivePathSample {
  double t = 0;                 // seconds since the first sample
  std::array<float, 16> local{};  // column-major chassis local matrix
};

class DrivePath {
 public:
  // Text format: a "# openslrr drive path v1" header line, then one line per
  // sample: t followed by the 16 matrix floats.
  bool load(const std::string& path, std::string* error = nullptr);
  bool empty() const { return samples_.size() < 2; }
  double duration() const { return samples_.empty() ? 0 : samples_.back().t; }
  // Pose at time t (clamped to the recording): position interpolated
  // linearly, axes interpolated and re-orthonormalised.
  std::array<float, 16> at(double t) const;
  const std::vector<DrivePathSample>& samples() const { return samples_; }
  void add(const DrivePathSample& s) { samples_.push_back(s); }

 private:
  std::vector<DrivePathSample> samples_;
};

class DrivePathWriter {
 public:
  ~DrivePathWriter();
  bool open(const std::string& path);
  bool is_open() const { return file_ != nullptr; }
  // Appends a sample; time accumulates from the first call.
  void write(double dt, const float* local16);

 private:
  std::FILE* file_ = nullptr;
  double t_ = 0;
  bool first_ = true;
};

}  // namespace slrr
