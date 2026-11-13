#include "standalone_test.hpp"

#include "slrr/vehicle_dynamics.hpp"

#include <cmath>

namespace {

struct Result {
  double car_speed = 0;
  double prop_speed = 0;
  double momentum = 0;
  double car_angular = 0;
  double prop_angular = 0;
};

Result impact(double prop_mass, double contact_x = 0.0) {
  slrr::physics::RigidBody car, prop;
  car.configure(1200.0, {-1.0, -0.5, -2.0}, {1.0, 0.5, 2.0}, {0.0, 0.0, 0.0});
  prop.configure(prop_mass, {-0.2, -0.2, -0.2}, {0.2, 0.2, 0.2}, {0.0, 0.0, 0.0});
  car.position = {0.0, 0.0, -3.0};
  car.velocity = {0.0, 0.0, 10.0};
  prop.position = {0.0, 0.0, 0.0};
  const slrr::physics::Vec3 point{contact_x, 0.0, -1.0};
  const slrr::physics::Vec3 normal{0.0, 0.0, -1.0};
  car.contact(point, normal, 0.0, 0.0, &prop);
  return {car.velocity.z, prop.velocity.z,
          car.mass * car.velocity.z + prop.mass * prop.velocity.z,
          car.omega().length(), prop.omega().length()};
}

}  // namespace

SLRR_TEST(contact_preserves_linear_momentum_and_scales_car_slowdown_with_mass) {
  const auto light = impact(5.0);
  const auto medium = impact(40.0);
  const auto heavy = impact(1000.0);
  SLRR_REQUIRE(std::fabs(light.momentum - 12000.0) < 1e-6);
  SLRR_REQUIRE(std::fabs(medium.momentum - 12000.0) < 1e-6);
  SLRR_REQUIRE(std::fabs(heavy.momentum - 12000.0) < 1e-6);

  const double light_slowdown = 10.0 - light.car_speed;
  const double medium_slowdown = 10.0 - medium.car_speed;
  const double heavy_slowdown = 10.0 - heavy.car_speed;
  SLRR_REQUIRE(light_slowdown < medium_slowdown);
  SLRR_REQUIRE(medium_slowdown < heavy_slowdown);
  SLRR_REQUIRE(light.prop_speed > 0.0);
  SLRR_REQUIRE(medium.prop_speed > 0.0);
  SLRR_REQUIRE(heavy.car_speed < 10.0);
  SLRR_REQUIRE(heavy.prop_speed > 0.0);
}

SLRR_TEST(off_center_contact_produces_opposite_angular_response) {
  const auto result = impact(40.0, 0.5);
  SLRR_REQUIRE(result.car_angular > 0.0);
  SLRR_REQUIRE(result.prop_angular > 0.0);
}

int main() {
  const slrr::standalone::Test tests[] = {
      contact_preserves_linear_momentum_and_scales_car_slowdown_with_mass,
      off_center_contact_produces_opposite_angular_response,
  };
  return slrr::standalone::run("dynamic_response", tests);
}
