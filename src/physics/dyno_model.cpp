#include "slrr/gc_roots.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace slrr {
namespace {

enum Word : int {
  kCrank = 0,
  kPiston = 1,
  kRod = 3,
  kVmin = 4,
  kCylinders = 5,
  kBurnTime = 7,
  kBurnSpeed = 8,
  kDensity = 9,
  kHeatCapacity = 10,
  kIntakePressure = 11,
  kExhaustPressure = 12,
  kIntakeTemperature = 13,
  kExhaustTemperature = 14,
  kMixtureHeat = 15,
  kExhaustHeat = 16,
  kIntakeMinimum = 17,
  kExhaustMinimum = 18,
  kIntakeMaximum = 19,
  kExhaustMaximum = 20,
  kThermalTime = 21,
  kRpmLimit = 22,
  kBore = 23,
  kStroke = 24,
  kDisplacement = 25,
  kArea = 26,
  kSweptVolume = 27,
  kVmax = 28,
  kCompression = 29,
  kSpeedLimit = 30,
  kIntakeDensity = 31,
  kExhaustDensity = 32,
  kDt = 33,
  kRpm = 34,
  kMaximumRpm = 35,
  kSpark = 36,
  kIntakeOpen = 37,
  kIntakeClose = 38,
  kExhaustOpen = 39,
  kExhaustClose = 40,
  kIntakeStart = 41,
  kIntakeDuration = 42,
  kIntakeShift = 43,
  kIntakeRpm0 = 44,
  kIntakeRpm1 = 45,
  kExhaustStart = 46,
  kExhaustDuration = 47,
  kExhaustShift = 48,
  kExhaustRpm0 = 49,
  kExhaustRpm1 = 50,
  kSparkStart = 51,
  kSparkShift = 52,
  kSparkRpm0 = 53,
  kSparkRpm1 = 54,
  kTurboMultiplier = 55,
  kTurboOptimum = 56,
  kTurboPlateau = 57,
  kTurboRange = 58,
  kTurboPressure = 59,
  kWastePressure = 60,
  kMixtureRatio = 61,
  kFuelLimit = 62,
  kAirLimit = 63,
  kIntakeValve = 64,
  kExhaustValve = 65,
  kBurned = 66,
  kIgnitionTime = 67,
  kWallTemperature = 68,
  kAmbientTemperature = 69,
  kHeatLoss = 70,
  kOtherHeat = 71,
  kElapsed = 72,
  kPeriod = 73,
  kAngularSpeed = 74,
  kAngle = 75,
  kPistonPosition = 76,
  kVolume = 77,
  kGasDensity = 78,
  kMass = 79,
  kPressure = 80,
  kTemperature = 81,
  kFuelHeat = 82,
  kAirFraction = 83,
  kFuelFraction = 84,
  kEffectiveHeat = 85,
  kEffectivePressure = 86,
  kEffectiveTemperature = 87,
  kEffectiveDensity = 88,
  kEffectiveBurnTime = 89,
  kFuelUsed = 90,
  kAirUsed = 91,
  kPistonSpeed = 92,
  kWork = 94,
  kPreviousWork = 95,
  kFrictionPower = 96,
  kMeanPressure = 97,
  kTorque = 98,
  kHorsepower = 99,
  kGrossPower = 100,
  kNetPower = 101,
  kFuelConsumption = 102,
  kAirConsumption = 103,
  kSpecificConsumption = 104,
  kEfficiency = 105,
  kPeakPressure = 106,
  kPeakTorque = 107,
  kPeakHorsepower = 108,
  kPeakGrossPower = 109,
  kPeakNetPower = 110,
  kPeakPressureRpm = 111,
  kPeakTorqueRpm = 112,
  kPeakHorsepowerRpm = 113,
  kPeakGrossRpm = 114,
  kPeakNetRpm = 115
};

struct Bank {
  float v[116]{};
  double get(int i) const { return v[i]; }
  void set(int i, double x) { v[i] = static_cast<float>(x); }
};

float fpow(double x, double e) {
  const float base = static_cast<float>(x);
  const float exp = static_cast<float>(e);
  return static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exp)));
}

void zero_range(Bank& s, int first, int last) {
  for (int i = first; i <= last; ++i) s.set(i, 0);
}

void mixture(Bank& s) {
  s.set(kFuelFraction, 1.0 / (s.get(kMixtureRatio) + 1.0));
  s.set(kAirFraction, s.get(kFuelFraction) * s.get(kMixtureRatio));
  s.set(kIntakeDensity, s.get(kIntakePressure) * 9.86923259915784e-6 * s.get(kDensity));
  s.set(kExhaustDensity, s.get(kExhaustPressure) * 9.86923259915784e-6 * s.get(kDensity));
}

// The reset volume keeps the opposite grouping from the advancing crank.
void reset_cycle(Bank& s) {
  s.set(kFuelHeat, s.get(kEffectiveHeat));
  s.set(kPressure, s.get(kEffectivePressure));
  for (int field : {kElapsed, kAngle, kIntakeValve, kExhaustValve, kBurned, kHeatLoss,
                    kOtherHeat, kWork, kPreviousWork, kMeanPressure, kGrossPower, kFuelUsed,
                    kAirUsed})
    s.set(field, 0);
  s.set(kTemperature, s.get(kEffectiveTemperature));
  const double position = s.get(kCrank) + std::sqrt(s.get(kRod) * s.get(kRod));
  s.set(kPistonPosition, position);
  s.set(kVolume, ((position - s.get(kRod)) - s.get(kCrank)) * s.get(kArea) + s.get(kVmin));
  s.set(kGasDensity, s.get(kEffectiveDensity));
  s.set(kMass, s.get(kVolume) * s.get(kEffectiveDensity));
}

void reset(Bank& s) {
  s.set(kCrank, 0.05317499861121178);
  s.set(kPiston, 0.04899999871850014);
  s.set(kRod, 0.15);
  s.set(kVmin, 0.0001);
  s.set(kCylinders, 4);
  s.set(kBurnTime, 0.003);
  s.set(kBurnSpeed, 0.00005);
  s.set(2, 1.0290000438690186);
  s.set(6, 1);
  s.set(kDensity, 1.2928999662399292);
  s.set(kMixtureHeat, 1000000);
  s.set(kExhaustHeat, 0);
  s.set(kHeatCapacity, 712);
  s.set(kIntakePressure, 101325);
  s.set(kExhaustPressure, 111457.5);
  s.set(kIntakeTemperature, 293.15);
  s.set(kExhaustTemperature, 293.15);
  s.set(kIntakeMinimum, 0.005);
  s.set(kExhaustMinimum, 0.005);
  s.set(kIntakeMaximum, 0.8);
  s.set(kExhaustMaximum, 0.8);
  s.set(kThermalTime, 95);
  s.set(kWallTemperature, 353.15);
  s.set(kAmbientTemperature, 353.15);
  s.set(kMixtureRatio, 14);
  s.set(kSpark, 0.48);
  s.set(kSparkStart, 0.48);
  s.set(kIntakeOpen, 0.001);
  s.set(kIntakeStart, 0.001);
  s.set(kIntakeClose, 0.25);
  s.set(kExhaustOpen, 0.75);
  s.set(kExhaustStart, 0.75);
  s.set(kExhaustClose, 0.99);
  s.set(kSparkShift, -0.05);
  s.set(kSparkRpm0, 2000);
  s.set(kSparkRpm1, 5000);
  s.set(kIntakeDuration, 0.249);
  s.set(kExhaustDuration, 0.24000000953674316);
  for (int field : {kIntakeShift, kIntakeRpm0, kIntakeRpm1, kExhaustShift, kExhaustRpm0,
                    kExhaustRpm1, kElapsed, kAngle, kTurboPlateau})
    s.set(field, 0);
  s.set(kRpm, 1000);
  s.set(kTurboMultiplier, 20);
  s.set(kTurboOptimum, 85000);
  s.set(kTurboRange, 30000);
  s.set(kTurboPressure, 0);
  s.set(kWastePressure, 3);
  mixture(s);
  reset_cycle(s);
  for (int field : {kPeakGrossPower, kPeakNetPower, kPeakPressure, kPeakGrossRpm, kPeakNetRpm,
                    kPeakPressureRpm, kFuelUsed, kAirUsed})
    s.set(field, 0);
  s.set(kDt, s.get(kPeriod) * 0.005);
  s.set(kAirLimit, 100);
  s.set(kFuelLimit, 100);
}

void geometry(Bank& s) {
  s.set(kEffectiveHeat, s.get(kMixtureHeat));
  s.set(kBore, s.get(kPiston) + s.get(kPiston));
  s.set(kEffectivePressure, s.get(kIntakePressure));
  s.set(kSpeedLimit, 45);
  s.set(kEffectiveTemperature, s.get(kIntakeTemperature));
  s.set(kEffectiveDensity, s.get(kIntakeDensity));
  s.set(kStroke, s.get(kCrank) + s.get(kCrank));
  s.set(kArea, s.get(kPiston) * s.get(kPiston) * 3.1415927410125732);
  s.set(kSweptVolume, s.get(kArea) * s.get(kStroke));
  s.set(kVmax, s.get(kSweptVolume) + s.get(kVmin));
  s.set(kCompression, s.get(kVmax) / s.get(kVmin));
  s.set(kMaximumRpm, (45.0 / s.get(kCrank)) * 9.549296379089355);
  s.set(kPistonSpeed, s.get(kAngularSpeed) * s.get(kCrank));
  s.set(kDisplacement, s.get(kSweptVolume) * s.get(kCylinders));
}

double advance_timing(const Bank& s, int start, int shift, int low, int high) {
  const double span = s.get(high) - s.get(low);
  if (span <= 0) return s.get(start);
  const double fraction = (s.get(kRpm) - s.get(low)) / span;
  if (fraction < 0) return s.get(start);
  if (fraction > 1) return s.get(shift) + s.get(start);
  return fraction * s.get(shift) + s.get(start);
}

double turbo_pressure(const Bank& s, double rpm) {
  const double multiplier = s.get(kTurboMultiplier);
  const double range = s.get(kTurboRange);
  if (multiplier <= 0 || range <= 0) return 0;
  const double distance = std::fabs(rpm * multiplier - s.get(kTurboOptimum));
  const double plateau = s.get(kTurboPlateau);
  const double response = distance < plateau ? 1.0
      : std::max(0.0, 1.0 - (distance - plateau) / (range - plateau));
  return std::min(response * s.get(kTurboPressure), s.get(kWastePressure));
}

void prepare(Bank& s) {
  mixture(s);
  s.set(kSpark, advance_timing(s, kSparkStart, kSparkShift, kSparkRpm0, kSparkRpm1));
  s.set(kIntakeOpen, advance_timing(s, kIntakeStart, kIntakeShift, kIntakeRpm0, kIntakeRpm1));
  s.set(kIntakeClose, s.get(kIntakeDuration) + s.get(kIntakeOpen));
  s.set(kExhaustOpen,
        advance_timing(s, kExhaustStart, kExhaustShift, kExhaustRpm0, kExhaustRpm1));
  s.set(kExhaustClose, s.get(kExhaustDuration) + s.get(kExhaustOpen));
  s.set(kEffectivePressure, s.get(kIntakePressure));
  s.set(kEffectiveHeat, s.get(kMixtureHeat));
  s.set(kEffectiveTemperature, s.get(kIntakeTemperature));
  s.set(kEffectiveDensity, s.get(kIntakeDensity));
  if (s.get(kTurboMultiplier) > 0 && s.get(kTurboRange) > 0) {
    const double boost = std::sqrt(turbo_pressure(s, s.get(kRpm)) + 1.0);
    s.set(kEffectivePressure, boost * boost * s.get(kIntakePressure));
    s.set(kEffectiveTemperature, s.get(kIntakeTemperature) * boost);
    s.set(kEffectiveDensity, s.get(kIntakeDensity) * boost);
  }
  if (s.get(kFuelLimit) > 0 && s.get(kFuelLimit) < s.get(kFuelConsumption))
    s.set(kEffectiveHeat, s.get(kMixtureHeat) / (s.get(kFuelConsumption) / s.get(kFuelLimit)));
  if (s.get(kAirLimit) > 0 && s.get(kAirLimit) < s.get(kAirConsumption))
    s.set(kEffectivePressure, s.get(kEffectivePressure) / (s.get(kAirConsumption) / s.get(kAirLimit)));
  double shortening = s.get(kRpm) * s.get(kBurnSpeed);
  if (shortening >= 0.9) shortening = 0.9;
  s.set(kEffectiveBurnTime, (1.0 - shortening) * s.get(kBurnTime));
}

bool valve_open(const Bank& s, int open, int close) {
  const bool after = s.get(open) * s.get(kPeriod) < s.get(kElapsed);
  if (s.get(close) <= s.get(open))
    return after || s.get(kElapsed) < s.get(close) * s.get(kPeriod);
  return after && s.get(kElapsed) < s.get(close) * s.get(kPeriod);
}

bool step(Bank& s) {
  if (s.get(kVmin) < 0) s.set(kVmin, 0);
  if (s.get(kVmax) < 0) s.set(kVmax, 0);
  const double old_volume = s.get(kVolume) < 0 ? 0.0 : s.get(kVolume);
  const double old_pressure = s.get(kPressure);
  const double old_temperature = s.get(kTemperature);
  const double old_burned = s.get(kBurned);
  s.set(kElapsed, s.get(kDt) + s.get(kElapsed));
  const bool completed = s.get(kPeriod) <= s.get(kElapsed);
  if (completed) {
    s.set(kElapsed, s.get(kElapsed) - s.get(kPeriod));
    s.set(kPreviousWork, s.get(kWork));
    s.set(kWork, 0);
    const double swept = s.get(kVmax) - s.get(kVmin);
    s.set(kMeanPressure, swept <= 1e-8 ? 0.0 : s.get(kPreviousWork) / swept);
    const double pressure_work = s.get(kMeanPressure) * swept;
    s.set(kTorque, s.get(kCylinders) * pressure_work * 0.125);
    s.set(kGrossPower, s.get(kRpm) * pressure_work * 0.01666666753590107 * s.get(kCylinders));
    s.set(kFrictionPower, s.get(kRpm) * s.get(kRpm) * s.get(kRpm) * 2e-8);
    s.set(kNetPower, s.get(kGrossPower) - s.get(kFrictionPower));
    s.set(kHorsepower, s.get(kNetPower) * 0.001341);
  }
  s.set(kAngle, s.get(kAngle) + s.get(kAngularSpeed) * s.get(kDt));
  if (s.get(kAngle) > 12.566370964050293) s.set(kAngle, s.get(kAngle) - 12.566370964050293);
  const double sine = std::sin(s.get(kAngle));
  const double transverse = sine * s.get(kCrank);
  const double position = std::sqrt(s.get(kRod) * s.get(kRod) - transverse * transverse) +
                          std::cos(s.get(kAngle)) * s.get(kCrank);
  s.set(kPistonPosition, position);
  s.set(kVolume, ((s.get(kCrank) + s.get(kRod)) - position) * s.get(kArea) + s.get(kVmin));
  if (s.get(kVolume) < 1e-7) {
    s.set(kVolume, 1e-7);
    return completed;
  }
  const double inverse_volume = 1.0 / s.get(kVolume);
  s.set(kPressure, static_cast<double>(fpow(inverse_volume * old_volume, 1.3)) * old_pressure);
  const double temperature_ratio = inverse_volume * old_volume;
  s.set(kTemperature, static_cast<double>(fpow(temperature_ratio, 0.3)) * old_temperature);
  s.set(kIntakeValve, s.get(valve_open(s, kIntakeOpen, kIntakeClose) ? kIntakeMaximum
                                                                     : kIntakeMinimum));
  s.set(kExhaustValve, s.get(valve_open(s, kExhaustOpen, kExhaustClose) ? kExhaustMaximum
                                                                        : kExhaustMinimum));
  const double ignition = s.get(kSpark) * s.get(kPeriod);
  double end = ignition + s.get(kEffectiveBurnTime);
  s.set(kIgnitionTime, ignition);
  if (end > s.get(kPeriod)) end -= s.get(kPeriod);
  const bool burning = end <= ignition ? ignition < s.get(kElapsed) || s.get(kElapsed) <= end
                                       : ignition < s.get(kElapsed) && s.get(kElapsed) <= end;
  s.set(kBurned, burning ? (s.get(kElapsed) - ignition) / s.get(kEffectiveBurnTime)
                         : (old_burned <= 0 ? 0.0 : 1.0));
  if (s.get(kBurned) > 0) {
    const double old_mass = s.get(kMass);
    if (s.get(kMass) < 1e-7) s.set(kMass, 1e-7);
    if (s.get(kTemperature) < 1e-7) s.set(kTemperature, 1e-7);
    const double burn_energy = s.get(kFuelHeat) * old_mass;
    const double next_temperature =
        ((s.get(kBurned) - old_burned) * burn_energy) / (s.get(kHeatCapacity) * s.get(kMass)) +
        s.get(kTemperature);
    s.set(kPressure, (next_temperature / s.get(kTemperature)) * s.get(kPressure));
    s.set(kTemperature, next_temperature);
    if (s.get(kBurned) >= 1) {
      s.set(kBurned, 0);
      s.set(kFuelHeat, 0);
    }
  }
  if (s.get(kIntakeValve) > 0) {
    double fraction = s.get(kIntakeValve) * s.get(kDt) * 1000.0;
    if (fraction >= 1) fraction = 1;
    double next_pressure =
        fraction * (s.get(kEffectivePressure) - s.get(kPressure)) + s.get(kPressure);
    if (next_pressure <= 1e-6) next_pressure = 1e-6;
    const double displaced = (s.get(kVolume) * s.get(kPressure)) / next_pressure;
    if (s.get(kVolume) <= displaced) {
      s.set(kMass, s.get(kMass) - (displaced - s.get(kVolume)) * inverse_volume * s.get(kMass));
    } else {
      const double added = (next_pressure / s.get(kEffectivePressure)) * s.get(kEffectiveDensity) *
                           (s.get(kVolume) - displaced);
      if (s.get(kMass) < 1e-7) s.set(kMass, 1e-7);
      const double next_mass = s.get(kMass) + added;
      const double inverse_mass = 1.0 / next_mass;
      s.set(kTemperature, (s.get(kEffectiveTemperature) * added + s.get(kTemperature) * s.get(kMass)) *
                             inverse_mass);
      s.set(kFuelHeat,
            (s.get(kEffectiveHeat) * added + s.get(kFuelHeat) * s.get(kMass)) * inverse_mass);
      s.set(kMass, next_mass);
      s.set(kAirUsed, added * s.get(kAirFraction) + s.get(kAirUsed));
      s.set(kFuelUsed, s.get(kFuelFraction) * added + s.get(kFuelUsed));
    }
    s.set(kPressure, next_pressure);
    if (s.get(kMass) < 0) s.set(kMass, 0);
  }
  if (s.get(kExhaustValve) > 0) {
    double fraction = s.get(kExhaustValve) * s.get(kDt) * 1000.0;
    if (fraction >= 1) fraction = 1;
    const double next_pressure =
        fraction * (s.get(kExhaustPressure) - s.get(kPressure)) + s.get(kPressure);
    const double displaced = (s.get(kVolume) * s.get(kPressure)) / next_pressure;
    if (s.get(kVolume) <= displaced) {
      s.set(kMass, s.get(kMass) - (displaced - s.get(kVolume)) * inverse_volume * s.get(kMass));
    } else {
      const double added = (next_pressure / s.get(kExhaustPressure)) * s.get(kExhaustDensity) *
                           (s.get(kVolume) - displaced);
      if (s.get(kMass) < 1e-7) s.set(kMass, 1e-7);
      const double next_mass = s.get(kMass) + added;
      const double inverse_mass = 1.0 / next_mass;
      s.set(kTemperature,
            (s.get(kTemperature) * s.get(kMass) + added * s.get(kExhaustTemperature)) * inverse_mass);
      s.set(kFuelHeat, (s.get(kExhaustHeat) * added + s.get(kFuelHeat) * s.get(kMass)) * inverse_mass);
      s.set(kMass, next_mass);
    }
    s.set(kPressure, next_pressure);
  }
  if (s.get(kMass) < 0) s.set(kMass, 0);
  double fraction = s.get(kDt) / s.get(kThermalTime);
  if (fraction >= 1) fraction = 1;
  const double difference = s.get(kTemperature) - s.get(kWallTemperature);
  const double sign = difference == 0.0 ? 0.0 : (difference < 0 ? -1.0 : 1.0);
  double cooling = difference * difference * sign * fraction;
  if (difference != 0.0 && cooling / difference > 1.0) cooling /= cooling / difference;
  s.set(kHeatLoss, s.get(kHeatLoss) + cooling);
  const double cooled = s.get(kTemperature) - cooling;
  if (s.get(kTemperature) < 1e-7) s.set(kTemperature, 1e-7);
  s.set(kPressure, (cooled / s.get(kTemperature)) * s.get(kPressure));
  s.set(kTemperature, cooled);
  s.set(kWork, (s.get(kPressure) + old_pressure) * 0.5 * (s.get(kVolume) - old_volume) + s.get(kWork));
  return completed;
}

bool cycle(Bank& s) {
  s.set(kFuelUsed, 0);
  s.set(kAirUsed, 0);
  s.set(kHeatLoss, 0);
  s.set(kOtherHeat, 0);
  bool completed = false;
  for (int i = 0; i < 232 && !completed; ++i) completed = step(s);
  if (!completed) return false;
  const double inverse_period = 1.0 / s.get(kPeriod);
  s.set(kFuelConsumption, s.get(kFuelUsed) * inverse_period * s.get(kCylinders));
  s.set(kAirConsumption, s.get(kAirUsed) * inverse_period * s.get(kCylinders));
  s.set(kEfficiency, (s.get(kAirUsed) + s.get(kFuelUsed)) / (s.get(kSweptVolume) * s.get(kDensity)));
  s.set(kSpecificConsumption, (s.get(kFuelConsumption) * 7920.0) / s.get(kHorsepower));
  s.set(kHeatLoss, inverse_period * s.get(kHeatLoss));
  return std::isfinite(s.get(kTorque)) && std::isfinite(s.get(kNetPower));
}

struct Curve {
  float rpm_step = 0.f;
  std::vector<float> torque;
  std::vector<float> watts;
  std::vector<float> nitro_torque;
  std::vector<float> nitro_watts;
};

bool table(Bank& s, int step_rpm, std::vector<float>* torque, std::vector<float>* power) {
  zero_range(s, kPeakPressure, kPeakNetRpm);
  const int count = static_cast<int>(std::trunc(std::trunc(s.get(kMaximumRpm)) / step_rpm));
  if (count < 1 || count > 65536) return false;
  torque->assign(static_cast<size_t>(count), 0.f);
  power->assign(static_cast<size_t>(count), 0.f);
  for (int i = 1; i < count; ++i) {
    s.set(kAngle, 0);
    s.set(kElapsed, 0);
    s.set(kRpm, i * step_rpm);
    s.set(kPeriod, 120.0 / s.get(kRpm));
    s.set(kAngularSpeed, s.get(kRpm) * 0.10471975803375244);
    s.set(kPistonSpeed, s.get(kAngularSpeed) * s.get(kCrank));
    prepare(s);
    s.set(kDt, s.get(kPeriod) * 0.0043290043249726295);
    reset_cycle(s);
    if (!cycle(s)) return false;
    double previous = s.get(kMeanPressure);
    for (int repeat = 0; repeat < 10; ++repeat) {
      if (!cycle(s)) return false;
      if (std::fabs(previous / s.get(kMeanPressure) - 1.0) < 0.05) break;
      previous = previous * 0.4 + s.get(kMeanPressure) * 0.6;
    }
    if (s.get(kPeakGrossPower) < s.get(kGrossPower)) {
      s.set(kPeakGrossPower, s.get(kGrossPower));
      s.set(kPeakGrossRpm, s.get(kRpm));
    }
    if (s.get(kPeakNetPower) < s.get(kNetPower)) {
      s.set(kPeakNetPower, s.get(kNetPower));
      s.set(kPeakHorsepower, s.get(kNetPower) * 0.001341);
      s.set(kPeakNetRpm, s.get(kRpm));
      s.set(kPeakHorsepowerRpm, s.get(kRpm));
    }
    if (s.get(kPeakPressure) < s.get(kMeanPressure)) {
      s.set(kPeakPressure, s.get(kMeanPressure));
      s.set(kPeakPressureRpm, s.get(kRpm));
      s.set(kPeakTorqueRpm, s.get(kRpm));
      s.set(kPeakTorque, s.get(kTorque));
    }
    (*torque)[static_cast<size_t>(i)] = static_cast<float>(s.get(kTorque));
    (*power)[static_cast<size_t>(i)] = static_cast<float>(s.get(kNetPower));
  }
  s.set(kTorque, s.get(kPeakTorque));
  s.set(kGrossPower, s.get(kPeakGrossPower));
  s.set(kNetPower, s.get(kPeakNetPower));
  return true;
}

float field_of(const std::unordered_map<std::string, float>& fields, const char* name,
               float fallback) {
  const auto it = fields.find(name);
  return it == fields.end() ? fallback : it->second;
}

struct Session {
  Bank state;
  Curve curve;
  DynoStats stats;
};

// Weak native payloads: reachability is owned by the DynoData Java object.
// Only synchronous native calculations temporarily promote their receiver to
// a GC root. This preserves the receiver if host code reaches a safe point
// without recreating the permanent registry -> key -> registry cycle.
std::unordered_map<void*, Session> g_sessions;
std::unordered_map<void*, std::size_t> g_calculations_in_flight;

const bool g_metrics_registered = [] {
  return slrr::metrics::register_provider([](void*, std::vector<slrr::metrics::Value>& values) {
    values.push_back({"dyno.sessions", static_cast<std::int64_t>(g_sessions.size()),
                      slrr::metrics::Kind::Gauge});
    std::size_t in_flight = 0;
    for (const auto& [key, count] : g_calculations_in_flight) in_flight += count;
    values.push_back({"dyno.calculations_in_flight",
                      static_cast<std::int64_t>(in_flight), slrr::metrics::Kind::Gauge});
  }) != 0;
}();

class CalculationRoot {
 public:
  explicit CalculationRoot(void* key) : key_(key) { ++g_calculations_in_flight[key_]; }
  ~CalculationRoot() {
    const auto it = g_calculations_in_flight.find(key_);
    if (it == g_calculations_in_flight.end()) return;
    if (--it->second == 0) g_calculations_in_flight.erase(it);
  }
  CalculationRoot(const CalculationRoot&) = delete;
  CalculationRoot& operator=(const CalculationRoot&) = delete;

 private:
  void* key_;
};

bool calculate_into(Session* session, const std::unordered_map<std::string, float>& fields,
                    float table_size, std::string* error) {
  if (!std::isfinite(table_size) || table_size <= 0) {
    if (error) *error = "invalid table size";
    return false;
  }
  auto bad = [&](const char* name) {
    if (error) *error = std::string("non-finite ") + name;
    return false;
  };
  Bank s = session->state;
  reset(s);
  const std::pair<int, const char*> bindings[] = {
      {kCylinders, "cylinders"},
      {kVmin, "Vmin"},
      {kIntakeMinimum, "in_min"},
      {kIntakeMaximum, "in_max"},
      {kExhaustMinimum, "out_min"},
      {kExhaustMaximum, "out_max"},
      {kIntakeStart, "time_in_open"},
      {kIntakeClose, "time_in_close"},
      {kExhaustStart, "time_out_open"},
      {kExhaustClose, "time_out_close"},
      {kBurnTime, "time_burn"},
      {kRpmLimit, "RPM_limit"},
      {kSparkStart, "time_spark_min"},
      {kSparkShift, "time_spark_inc"},
      {kSparkRpm0, "time_spark_RPM0"},
      {kSparkRpm1, "time_spark_RPM1"},
      {kTurboMultiplier, "rpm_turbo_mul"},
      {kTurboOptimum, "rpm_turbo_opt"},
      {kTurboRange, "rpm_turbo_range"},
      {kTurboPressure, "P_turbo_max"},
      {kWastePressure, "P_turbo_waste"},
      {kMixtureHeat, "mixture_H"},
      {kMixtureRatio, "mixture_ratio"},
      {kAirLimit, "max_air_consumption"},
      {kFuelLimit, "max_fuel_consumption"},
  };
  const float fallbacks[] = {4,     0.00005f, 0.005f, 0.8f,  0.005f, 0.8f, 0.001f, 0.25f, 0.75f,
                             0.99f, 0.003f,   7000,   0.48f, -0.05f, 2000,  5000,   16,    85000,
                             20000, 0,        10,     1000000, 14,   0,     0};
  for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
    const float value = field_of(fields, bindings[i].second, fallbacks[i]);
    if (!std::isfinite(value)) return bad(bindings[i].second);
    s.set(bindings[i].first, value);
  }
  const float bore = field_of(fields, "bore", 0.082f);
  const float stroke = field_of(fields, "stroke", 0.09f);
  if (!std::isfinite(bore) || !std::isfinite(stroke)) return bad("bore");
  s.set(kPiston, bore * 0.5f);
  s.set(kCrank, stroke * 0.5f);
  s.set(kIntakeOpen, s.get(kIntakeStart));
  s.set(kExhaustOpen, s.get(kExhaustStart));
  s.set(kIntakeDuration, s.get(kIntakeClose) - s.get(kIntakeOpen));
  s.set(kExhaustDuration, s.get(kExhaustClose) - s.get(kExhaustOpen));
  s.set(kTurboPlateau, 0);
  const float intercooling = field_of(fields, "intercooling", 1.f);
  if (!std::isfinite(intercooling)) return bad("intercooling");
  s.set(kIntakeTemperature, intercooling * 293.15);
  if (!(s.get(kCrank) >= 0 && s.get(kCrank) <= s.get(kRod) && s.get(kPiston) >= 0)) {
    if (error) *error = "unsupported cylinder geometry";
    return false;
  }
  mixture(s);
  geometry(s);
  const float max_rpm = field_of(fields, "maxRPM", 0.f);
  if (!std::isfinite(max_rpm)) return bad("maxRPM");
  if (max_rpm > 0) s.set(kMaximumRpm, max_rpm);
  const double requested = std::max(s.get(kMaximumRpm) / table_size, 1.0);
  if (!std::isfinite(requested) || requested >= 2147483647.0) {
    if (error) *error = "unsupported RPM table range";
    return false;
  }
  const int rpm_step = static_cast<int>(std::trunc(requested));
  // A recalculation is a new configuration snapshot. Never combine its
  // primary bank with nitrous data captured at another resolution/geometry.
  Curve next;
  next.rpm_step = static_cast<float>(rpm_step);
  if (!table(s, rpm_step, &next.torque, &next.watts)) {
    if (error) *error = "non-finite primary combustion cycle";
    return false;
  }
  DynoStats stats;
  stats.ok = true;
  stats.rpm_step = next.rpm_step;
  stats.maximum_rpm = static_cast<float>(s.get(kMaximumRpm));
  stats.displacement = static_cast<float>(s.get(kDisplacement));
  stats.compression = static_cast<float>(s.get(kCompression));
  stats.max_torque = static_cast<float>(s.get(kPeakTorque));
  stats.max_hp = static_cast<float>(s.get(kPeakHorsepower));
  stats.torque_rpm = static_cast<float>(s.get(kPeakTorqueRpm));
  stats.hp_rpm = static_cast<float>(s.get(kPeakHorsepowerRpm));
  s.set(kIntakeTemperature, 293.15);
  const float nitro_h = field_of(fields, "nitro_H", 1.f);
  const float nitro_cooling = field_of(fields, "nitro_cooling", 1.f);
  if (!std::isfinite(nitro_h) || !std::isfinite(nitro_cooling)) return bad("nitro");
  if (nitro_h - 1.f > 0.0001f || nitro_cooling - 1.f > 0.0001f) {
    const double maximum = s.get(kMaximumRpm);
    s.set(kMixtureHeat, s.get(kMixtureHeat) * nitro_h);
    s.set(kIntakeTemperature, s.get(kIntakeTemperature) * nitro_cooling);
    mixture(s);
    geometry(s);
    s.set(kMaximumRpm, maximum);
    if (!table(s, rpm_step, &next.nitro_torque, &next.nitro_watts)) {
      if (error) *error = "invalid nitrous combustion table";
      return false;
    }
  }
  session->state = s;
  session->curve = std::move(next);
  session->stats = stats;
  return true;
}

float sample_curve(const Curve& curve, float rpm, float nitro, bool watts) {
  if (curve.torque.empty() || !std::isfinite(rpm) || !std::isfinite(nitro) || rpm < 0 ||
      !(curve.rpm_step > 0.f))
    return 0.f;
  const auto sample = [&](const std::vector<float>& table) {
    if (table.empty()) return 0.f;
    const double position = static_cast<double>(rpm) / curve.rpm_step;
    const double last = static_cast<double>(table.size() - 1);
    // Bound before converting to int, and distinguish the exact final sample
    // from values beyond it (which have no right-hand interpolation point).
    if (!std::isfinite(position) || position < 0 || position > last) return 0.f;
    const auto index = static_cast<std::size_t>(std::floor(position));
    if (index == table.size() - 1) return table.back();
    const float fraction = static_cast<float>(position - std::floor(position));
    return table[index] * (1.f - fraction) + table[index + 1] * fraction;
  };
  const float primary = sample(watts ? curve.watts : curve.torque);
  const std::vector<float>& secondary = watts ? curve.nitro_watts : curve.nitro_torque;
  if (secondary.empty()) return primary;
  return primary * (1.f - nitro) + sample(secondary) * nitro;
}

bool near(double got, double want, double tolerance) {
  return std::fabs(got - want) < tolerance;
}

}  // namespace

bool dyno_calculate(void* key, const std::unordered_map<std::string, float>& fields,
                    float table_size, DynoStats* stats) {
  if (!key) return false;
  const CalculationRoot in_flight(key);
  auto [it, inserted] = g_sessions.try_emplace(key);
  Session& session = it->second;
  std::string error;
  if (!calculate_into(&session, fields, table_size, &error)) {
    if (inserted) g_sessions.erase(it);
    std::fprintf(stderr, "[dyno] calc failed: %s\n", error.c_str());
    if (stats) *stats = {};
    return false;
  }
  if (stats) *stats = session.stats;
  return true;
}

float dyno_sample(void* key, float rpm, float nitro, bool watts) {
  const auto it = g_sessions.find(key);
  if (it == g_sessions.end()) return 0.f;
  return sample_curve(it->second.curve, rpm, nitro, watts);
}

float dyno_boost_fraction(void* key, float rpm) {
  const auto it = g_sessions.find(key);
  if (it == g_sessions.end() || !std::isfinite(rpm)) return 0.f;
  const auto& state = it->second.state;
  const double maximum = std::min(state.get(kTurboPressure), state.get(kWastePressure));
  return maximum > 0 ? static_cast<float>(std::clamp(turbo_pressure(state, rpm) / maximum, 0.0, 1.0))
                     : 0.f;
}

void dyno_forget(void* key) { g_sessions.erase(key); }

void dyno_clear_sessions() { g_sessions.clear(); }

std::size_t dyno_session_count() { return g_sessions.size(); }

std::size_t dyno_calculations_in_flight() {
  std::size_t total = 0;
  for (const auto& [key, count] : g_calculations_in_flight) total += count;
  return total;
}

bool dyno_self_check(char* msg, std::size_t msg_cap) {
  auto fail = [&](const char* text) {
    if (msg && msg_cap) std::snprintf(msg, msg_cap, "%s", text);
    return false;
  };
  Session fresh;
  std::string error;
  if (!calculate_into(&fresh, {{"maxRPM", 7000.f}}, 16.f, &error))
    return fail(error.c_str());
  const Curve& curve = fresh.curve;
  if (curve.rpm_step != 437.f || curve.torque.size() != 16 || curve.watts.size() != 16)
    return fail("dyno table shape");
  if (!near(fresh.stats.displacement, 0.0019011663971468806, 1e-9) ||
      !near(fresh.stats.compression, 10.50583267211914, 2e-6) ||
      !near(curve.torque[1], 82.08709716796875, 0.002) ||
      !near(curve.watts[1], 4781.27294921875, 0.05)) {
    if (msg && msg_cap)
      std::snprintf(msg, msg_cap,
                    "dyno witness fail torque=%.6f watts=%.4f disp=%.12g comp=%.8g",
                    curve.torque[1], curve.watts[1], fresh.stats.displacement,
                    fresh.stats.compression);
    return false;
  }
  Session tuned;
  if (!calculate_into(&tuned, {{"maxRPM", 7000.f}}, 28.f, &error)) return fail(error.c_str());
  const float base = tuned.curve.torque[14];
  if (!calculate_into(&tuned,
                      {{"maxRPM", 7000.f},
                       {"rpm_turbo_mul", 1.f},
                       {"rpm_turbo_opt", 3500.f},
                       {"rpm_turbo_range", 3000.f},
                       {"P_turbo_max", 0.8f}},
                      28.f, &error))
    return fail(error.c_str());
  if (!(tuned.curve.torque[14] > base)) return fail("dyno turbo did not raise torque");
  if (!calculate_into(&tuned, {{"maxRPM", 7000.f}, {"nitro_H", 1.2f}, {"nitro_cooling", 0.9f}},
                      28.f, &error))
    return fail(error.c_str());
  if (tuned.curve.nitro_torque.size() <= 14 ||
      !(tuned.curve.nitro_torque[14] > tuned.curve.torque[14]))
    return fail("dyno nitrous bank");
  if (!calculate_into(&tuned, {{"maxRPM", 7000.f}}, 28.f, &error)) return fail(error.c_str());
  if (!tuned.curve.nitro_torque.empty() || !tuned.curve.nitro_watts.empty())
    return fail("dyno stale nitrous bank was retained");
  Session bad;
  if (calculate_into(&bad, {{"stroke", 1.f}}, 28.f, &error)) return fail("dyno geometry accepted");
  if (calculate_into(&bad, {}, 0.f, &error)) return fail("dyno empty table accepted");
  if (msg && msg_cap)
    std::snprintf(msg, msg_cap, "dyno ok 437rpm torque=%.4f W=%.2f", curve.torque[1],
                  curve.watts[1]);
  return true;
}


void dyno_visit_gc_roots(const GcRootVisitor& visit) {
  for (const auto& entry : g_calculations_in_flight) visit(entry.first);
}
}  // namespace slrr
