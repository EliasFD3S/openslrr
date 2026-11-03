#include "slrr/sl2_pacejka.hpp"

#include <cmath>
#include <cstdio>

namespace slrr {

double sl2_pacejka_force(const Sl2PacejkaCoef& p, double slip, double fz,
                         bool clamp_negative) {
  const double peak = fz * p[1] + p[2];
  const double denom = peak * p[0];
  if (denom == 0.0) return 0.0;

  const double stiffness =
      (fz * p[3] + p[4]) * std::exp(-fz * p[5]) / denom;
  const double x = (fz * p[9] + p[10] + slip) * stiffness;
  const double curvature = fz * fz * p[6] + fz * p[7] + p[8];
  const double force =
      peak * fz *
      std::sin(p[0] * std::atan(curvature * (std::atan(x) - x) + x));
  if (clamp_negative && force < 0.0) return 0.0;
  return force;
}

bool sl2_pacejka_self_check(char* msg, std::size_t msg_cap) {
  const double got = sl2_pacejka_force(kBaiernSportPacejka, 0.1, 3500.0);
  constexpr double kExpect = 4512.310131513881;
  const double err = std::fabs(got - kExpect);
  if (err >= 1e-8) {
    if (msg && msg_cap)
      std::snprintf(msg, msg_cap, "sl2_pacejka golden fail got=%.12f want=%.12f",
                    got, kExpect);
    return false;
  }
  if (sl2_pacejka_force(kBaiernSportPacejka, 0.0, 3500.0) != 0.0) {
    if (msg && msg_cap)
      std::snprintf(msg, msg_cap, "sl2_pacejka zero-slip expected 0");
    return false;
  }
  if (sl2_pacejka_force(kBaiernSportPacejka, -0.1, 3500.0, true) != 0.0) {
    if (msg && msg_cap)
      std::snprintf(msg, msg_cap, "sl2_pacejka clamp-negative expected 0");
    return false;
  }
  if (msg && msg_cap)
    std::snprintf(msg, msg_cap, "sl2_pacejka ok F(0.1,3500)=%.6f", got);
  return true;
}

}  // namespace slrr
