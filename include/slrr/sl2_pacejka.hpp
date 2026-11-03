#pragma once

#include <array>
#include <cstddef>

namespace slrr {

// SL2 tyre-force leaf (FUN_0043bb30 / FUN_0043ba80). Double reference — not
// x87 bit-exact. See docs/physics/sl2-pacejka.md.
using Sl2PacejkaCoef = std::array<double, 11>;

// Stock Baiern 205/55 pattern after Tyre.java → WheelRef (indices 0..10).
inline constexpr Sl2PacejkaCoef kBaiernSportPacejka = {
    0.814, 0.0, 1.76863, 0.0, 17.6839, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0};

double sl2_pacejka_force(const Sl2PacejkaCoef& p, double slip, double fz,
                         bool clamp_negative = false);

// Returns false and fills msg if golden sample diverges.
bool sl2_pacejka_self_check(char* msg, std::size_t msg_cap);

}  // namespace slrr
