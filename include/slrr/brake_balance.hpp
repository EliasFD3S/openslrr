#pragma once
// Brake distribution (#400).
//
// Stock Brake.java scales each axle's own disc torque by the chassis
// `brake_balance` b: rear wheels get torque x b, front wheels torque x (1 - b),
// with b = 0.5 unless the chassis has an adjuster (`brake_balance_can_be_set`,
// tuned in the garage). With the stock parts that left about 56 % of the
// braking on the front axle (the Baiern CoupeSport has two front pistons and
// one at the rear), while braking puts about 70 % of the weight there: once
// the brakes no longer lock all four wheels at half pedal, the rears locked
// first and a car braking into a bend spun.
//
// The host reads b as the front share instead: b = 0.5 puts 70 % of the
// braking on the front axle and the adjuster moves it from 100 % (b = 0) to
// 40 % (b = 1). The total stays what the stock split gives at b = 0.5, and
// each wheel keeps its share of its own axle.
#include <algorithm>
#include <array>

namespace slrr {

constexpr double kFrontBrakeShare = .7;

inline double front_brake_share(double balance) {
  return std::clamp(kFrontBrakeShare + .6 * (.5 - balance), .4, 1.0);
}

// brakes: per-wheel torque from the scripts, front wheels first (`front`
// of them). Rewritten in place.
template <size_t N>
void distribute_brakes(std::array<double, N>& brakes, size_t front, double balance) {
  if (!(balance >= 0 && balance <= 1)) balance = .5;
  // Undo the script's split. At an end of the adjuster one axle got nothing:
  // its discs are taken to match the other axle's.
  const double b = std::clamp(balance, .02, .98);
  std::array<double, N> raw{};
  double front_raw = 0, rear_raw = 0;
  for (size_t i = 0; i < N; ++i) {
    raw[i] = brakes[i] / (i < front ? 1 - b : b);
    (i < front ? front_raw : rear_raw) += raw[i];
  }
  if (balance <= .02 || balance >= .98) {
    const bool rear_empty = balance <= .02;
    const double other = rear_empty ? front_raw / std::max<size_t>(front, 1)
                                    : rear_raw / std::max<size_t>(N - front, 1);
    for (size_t i = 0; i < N; ++i)
      if ((i < front) != rear_empty) raw[i] = other;
    front_raw = rear_raw = 0;
    for (size_t i = 0; i < N; ++i) (i < front ? front_raw : rear_raw) += raw[i];
  }
  if (front_raw <= 0 || rear_raw <= 0) return;  // one axle has no brakes
  const double total = .5 * (front_raw + rear_raw);
  const double share = front_brake_share(balance);
  for (size_t i = 0; i < N; ++i)
    brakes[i] = raw[i] * (i < front ? share * total / front_raw
                                    : (1 - share) * total / rear_raw);
}

}  // namespace slrr
