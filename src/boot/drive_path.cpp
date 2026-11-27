#include "slrr/drive_path.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace slrr {
namespace {

constexpr const char* kHeader = "# openslrr drive path v1";

void normalize(float* v) {
  const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n > 1e-8f)
    for (int i = 0; i < 3; ++i) v[i] /= n;
}

}  // namespace

bool DrivePath::load(const std::string& path, std::string* error) {
  samples_.clear();
  std::ifstream in(path);
  if (!in) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::string line;
  if (!std::getline(in, line) || line.rfind(kHeader, 0) != 0) {
    if (error) *error = path + ": not a drive path (missing header)";
    return false;
  }
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    DrivePathSample s;
    fields >> s.t;
    for (float& v : s.local) fields >> v;
    if (!fields || !std::isfinite(s.t)) continue;
    if (!samples_.empty() && s.t <= samples_.back().t) continue;
    samples_.push_back(s);
  }
  if (samples_.size() < 2) {
    if (error) *error = path + ": fewer than two samples";
    return false;
  }
  return true;
}

std::array<float, 16> DrivePath::at(double t) const {
  if (samples_.empty()) return {};
  if (t <= samples_.front().t) return samples_.front().local;
  if (t >= samples_.back().t) return samples_.back().local;
  const auto next = std::upper_bound(
      samples_.begin(), samples_.end(), t,
      [](double value, const DrivePathSample& s) { return value < s.t; });
  const auto& b = *next;
  const auto& a = *(next - 1);
  const float u = static_cast<float>((t - a.t) / (b.t - a.t));
  std::array<float, 16> m{};
  for (int i = 0; i < 16; ++i) m[i] = a.local[i] + (b.local[i] - a.local[i]) * u;
  // Re-orthonormalise the rotation (columns 0..2): x, then z orthogonal to
  // x, then y = z cross x, keeping the handedness of the recording.
  float* x = &m[0];
  float* y = &m[4];
  float* z = &m[8];
  normalize(x);
  const float d = z[0] * x[0] + z[1] * x[1] + z[2] * x[2];
  for (int i = 0; i < 3; ++i) z[i] -= d * x[i];
  normalize(z);
  const float c[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
                      z[0] * x[1] - z[1] * x[0]};
  const float sign = (c[0] * y[0] + c[1] * y[1] + c[2] * y[2]) < 0 ? -1.f : 1.f;
  for (int i = 0; i < 3; ++i) y[i] = sign * c[i];
  return m;
}

DrivePathWriter::~DrivePathWriter() {
  if (file_) std::fclose(file_);
}

bool DrivePathWriter::open(const std::string& path) {
  file_ = std::fopen(path.c_str(), "w");
  if (!file_) return false;
  std::fprintf(file_, "%s\n", kHeader);
  return true;
}

void DrivePathWriter::write(double dt, const float* local16) {
  if (!file_) return;
  if (first_)
    first_ = false;
  else
    t_ += dt;
  std::fprintf(file_, "%.5f", t_);
  for (int i = 0; i < 16; ++i) std::fprintf(file_, " %.6g", local16[i]);
  std::fputc('\n', file_);
}

}  // namespace slrr
