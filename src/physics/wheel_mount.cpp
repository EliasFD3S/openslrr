#include "slrr/wheel_mount.hpp"

#include <algorithm>
#include <cmath>

namespace slrr::physics {

double contact_impulse_energy(const RigidBody& body, Vec3 point, Vec3 normal,
                              double closing_speed, double restitution) {
  if (!(closing_speed > 0) || !(body.mass > 0) || !point.finite() || !normal.finite())
    return 0;
  const Vec3 n = normal.normalized();
  const Vec3 r = point - body.position;
  const double inverse_mass =
      1 / body.mass + n.dot(body.inverse_world_inertia(r.cross(n)).cross(r));
  if (!(inverse_mass > 0) || !std::isfinite(inverse_mass)) return 0;
  const double impulse = (1 + restitution) * closing_speed / inverse_mass;
  return impulse * impulse / (2 * body.mass);
}

Vec3 pre_contact_velocity(const RigidBody& body, Vec3 contact, Vec3 normal,
                          double closing_speed, Vec3 point, double restitution) {
  const Vec3 current = body.point_velocity(point);
  if (!(closing_speed > 0) || !(body.mass > 0) || !contact.finite() || !normal.finite() ||
      !point.finite())
    return current;
  const Vec3 n = normal.normalized();
  const Vec3 r = contact - body.position;
  const double inverse_mass =
      1 / body.mass + n.dot(body.inverse_world_inertia(r.cross(n)).cross(r));
  if (!(inverse_mass > 0) || !std::isfinite(inverse_mass)) return current;
  const Vec3 impulse = n * ((1 + restitution) * closing_speed / inverse_mass);
  const Vec3 spin = body.inverse_world_inertia(r.cross(impulse));
  return current - impulse / body.mass - spin.cross(point - body.position);
}

float wheel_mount_condition(float condition, double shift, double energy, double distance,
                            const WheelMountTuning& tuning) {
  double result = condition;
  if (std::isfinite(shift) && shift > 0)
    result = std::min(result, 1 - shift / tuning.travel);
  if (std::isfinite(energy) && energy > 0 && std::isfinite(distance))
    result -= energy * std::exp(-(distance * distance) / (tuning.reach * tuning.reach)) /
              tuning.energy;
  return static_cast<float>(result);
}

}  // namespace slrr::physics
