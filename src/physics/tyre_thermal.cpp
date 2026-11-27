#include "slrr/tyre_thermal.hpp"

#include "slrr/brake_system.hpp"

#include <algorithm>
#include <cmath>

namespace slrr::physics {
namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kSpecificHeat = 1500;  // J/(kg K), rubber with its cords and belts

struct CurvePoint {
  double temperature, factor;
};

template <size_t N>
double interpolate(const CurvePoint (&curve)[N], double t) {
  if (t <= curve[0].temperature) return curve[0].factor;
  for (size_t i = 1; i < N; ++i)
    if (t <= curve[i].temperature) {
      const auto& a = curve[i - 1];
      const auto& b = curve[i];
      return a.factor + (b.factor - a.factor) * (t - a.temperature) / (b.temperature - a.temperature);
    }
  return curve[N - 1].factor;
}

// Grip against tread temperature: typical published shapes, not one
// product's data. A street tyre works from 40 deg C, a soft racing tyre
// needs 80; both lose grip past their window as the rubber overheats.
constexpr CurvePoint kStreet[] = {{0, .93}, {40, 1}, {90, 1}, {130, .92}, {180, .8}, {250, .7}};
constexpr CurvePoint kSport[] = {{0, .91}, {50, 1}, {100, 1}, {140, .9}, {190, .78}, {250, .68}};
constexpr CurvePoint kSupersport[] = {{0, .84}, {40, .9}, {80, 1}, {115, 1},
                                      {150, .9}, {200, .76}, {260, .65}};
constexpr CurvePoint kDragRadial[] = {{0, .86}, {60, 1}, {110, 1}, {150, .88}, {200, .75}};

// Tread band and the two sidewalls (rim at 0.75 of the radius).
double tread_area(const TyreThermalSpec& t) { return 2 * kPi * t.radius * t.width; }
double sidewall_area(const TyreThermalSpec& t) {
  const double rim = .75 * t.radius;
  return 2 * kPi * (t.radius * t.radius - rim * rim);
}
// The mass #430 gives a tyre: tread 15 kg/m², sidewalls 6 kg/m².
double tyre_mass(const TyreThermalSpec& t) { return 15 * tread_area(t) + 6 * sidewall_area(t); }

}  // namespace

TyreThermalSpec tyre_thermal_spec(double compound, double radius, double width) {
  TyreThermalSpec t;
  if (!(radius > 0) || !(width > 0) || !std::isfinite(compound)) return t;
  t.specified = true;
  t.compound = static_cast<TyreCompound>(std::clamp(static_cast<int>(std::lround(compound)), 0, 3));
  t.radius = std::clamp(radius, .15, .6);
  t.width = std::clamp(width, .1, .5);
  return t;
}

// The tread's outer half millimetre, the skin the slide heats: 3 % of the
// tyre; the rest of the tread band 20 %; the carcass the remaining 77 %.
constexpr double kSkinShare = .03, kTreadShare = .20;
double tyre_surface_capacity(const TyreThermalSpec& t) { return kSkinShare * tyre_mass(t) * kSpecificHeat; }
double tyre_tread_capacity(const TyreThermalSpec& t) { return kTreadShare * tyre_mass(t) * kSpecificHeat; }
double tyre_carcass_capacity(const TyreThermalSpec& t) {
  return (1 - kSkinShare - kTreadShare) * tyre_mass(t) * kSpecificHeat;
}

double tyre_grip_temperature(const TyreTemperature& t) { return .5 * (t.surface + t.tread); }

TyreTemperature step_tyre_temperature(const TyreThermalSpec& tyre, TyreTemperature t,
                                      double sliding, double rolling, double brake,
                                      double speed, double dt) {
  if (!tyre.specified) return {kAmbientTemperature, kAmbientTemperature, kAmbientTemperature};
  const double v = std::pow(std::abs(speed), .8);
  const double ambient = kAmbientTemperature;
  // Half the sliding work heats the skin (the rest goes into the road); all
  // of the hysteresis heats the carcass, and the rim conducts some of a hot
  // rotor's heat into it.
  const double rim = brake > t.carcass ? 1.5 * (brake - t.carcass) : 0;
  // Conduction through the rubber (0.25 W/m K) over the tread band: about
  // 1.5 mm from the skin into the bulk, 5 mm from the bulk to the carcass.
  const double skin_in = 160 * tread_area(tyre) * (t.surface - t.tread);  // W
  const double tread_in = 50 * tread_area(tyre) * (t.tread - t.carcass);  // W
  // The skin sheds heat to the air over most of the band and into the road
  // at each pass through the patch: the shorter the contact, the steeper the
  // gradient, so the conduction grows as the square root of the speed. The
  // carcass sheds heat through its sidewalls.
  const double tread_air = (8 + 5 * v) * .8 * tread_area(tyre) * (t.surface - ambient);
  const double road = 4 * std::sqrt(std::abs(speed)) * (t.surface - ambient);
  const double carcass_air = (6 + 4 * v) * (sidewall_area(tyre) + .2 * tread_area(tyre)) *
                             (t.carcass - ambient);
  TyreTemperature next;
  next.surface = t.surface + (.5 * std::max(0.0, sliding) - skin_in - tread_air - road) * dt /
                                 tyre_surface_capacity(tyre);
  next.tread = t.tread + (skin_in - tread_in) * dt / tyre_tread_capacity(tyre);
  next.carcass = t.carcass + (std::max(0.0, rolling) + rim + tread_in - carcass_air) * dt /
                                 tyre_carcass_capacity(tyre);
  next.surface = std::clamp(next.surface, ambient, 400.0);
  next.tread = std::clamp(next.tread, ambient, 350.0);
  next.carcass = std::clamp(next.carcass, ambient, 300.0);
  return next;
}

namespace {
constexpr double kAtmosphere = 1.013;  // bar
constexpr double kKelvin = 273.15;
// Working temperatures: where each compound's grip window starts (carcass)
// and its middle (tread).
constexpr double kWorkingCarcass[] = {40, 50, 80, 60};
constexpr double kWorkingTread[] = {65, 75, 97, 85};
}  // namespace

double tyre_working_carcass(const TyreThermalSpec& tyre) {
  return kWorkingCarcass[static_cast<int>(tyre.compound)];
}
double tyre_working_tread(const TyreThermalSpec& tyre) {
  return kWorkingTread[static_cast<int>(tyre.compound)];
}

double tyre_hot_pressure(double cold, double carcass) {
  return (cold + kAtmosphere) * (carcass + kKelvin) / (kAmbientTemperature + kKelvin) - kAtmosphere;
}

TyrePressureEffect tyre_pressure_effect(const TyreThermalSpec& tyre, double inflation,
                                        TyreTemperature t) {
  TyrePressureEffect e;
  if (!tyre.specified || !(inflation > 0)) return e;
  e.pressure = std::max(0.0, tyre_hot_pressure(inflation, t.carcass));
  const double working = tyre_hot_pressure(inflation, tyre_working_carcass(tyre));
  e.ratio = std::clamp(e.pressure / working, .5, 1.5);
  e.relaxation = 1 / std::sqrt(e.ratio);
  e.rolling = 1 / std::sqrt(e.ratio);
  e.nominal_load = std::pow(e.ratio, .25);
  const double rubber =
      std::clamp(1 - .002 * (tyre_grip_temperature(t) - tyre_working_tread(tyre)), .85, 1.05);
  e.stiffness = std::pow(e.ratio, .3) * rubber;
  return e;
}

double tyre_wear_weight(double surface) {
  return std::exp(std::max(0.0, surface - 100) / 59);
}

double tyre_wear_grip(double wear) {
  wear = std::clamp(wear, 0.0, 1.0);
  return wear >= .15 ? 1 : .6 + .4 * wear / .15;
}

double tyre_wear_stiffness(double wear) {
  return 1 + .15 * (1 - std::clamp(wear, 0.0, 1.0));
}

double tyre_grip_factor(const TyreThermalSpec& tyre, double temperature) {
  if (!tyre.specified) return 1;
  switch (tyre.compound) {
    case TyreCompound::sport: return interpolate(kSport, temperature);
    case TyreCompound::supersport: return interpolate(kSupersport, temperature);
    case TyreCompound::drag_radial: return interpolate(kDragRadial, temperature);
    default: return interpolate(kStreet, temperature);
  }
}

}  // namespace slrr::physics
