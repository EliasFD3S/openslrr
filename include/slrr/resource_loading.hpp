#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace slrr {
// Host analogue of the original 32-sample resource load-work history.
// Values are measured milliseconds of pack loading, not a synthetic delay.
class ResourceLoadingHistory {
 public:
  void record(double milliseconds) {
    if (std::isfinite(milliseconds) && milliseconds > 0)
      pending_ += std::min(milliseconds, 1000.0);
  }
  void reset() {
    samples_.fill(std::max(last_, pending_));
    cursor_ = 0;
  }
  void pump() {
    last_ = pending_;
    samples_[cursor_] = pending_;
    cursor_ = (cursor_ + 1) % samples_.size();
    pending_ = 0;
  }
  bool busy() const {
    double sum = 0;
    for (double value : samples_) sum += value;
    return pending_ > 4.0 || sum / samples_.size() > 4.0;
  }
 private:
  std::array<double, 32> samples_{};
  size_t cursor_ = 0;
  double pending_ = 0, last_ = 0;
};
// Once per outer host frame; querying isLoading never advances the history.
void resource_loading_pump();
}
