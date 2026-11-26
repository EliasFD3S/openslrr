#pragma once
// Tyre temperature (#446). Two lumps per tyre: the tread surface, heated by
// the sliding at the contact patch, and the carcass, heated by the rubber's
// hysteresis as it rolls and by the hot brake through the rim. The surface
// sets the grip, through a curve per compound (the patched Tyre sends it
// with WheelRef.setTyreSpec). Without a spec the tyre stays at ambient and
// keeps its grip.

namespace slrr::physics {

enum class TyreCompound { street = 0, sport = 1, supersport = 2, drag_radial = 3 };

struct TyreThermalSpec {
  bool specified = false;
  TyreCompound compound = TyreCompound::street;
  double radius = .3, width = .205;  // m
};

// From setTyreSpec's compound (Tyre.tcSTREET...tcDRAGRADIAL) and the wheel's
// radius and width (m). A non-positive radius or width leaves it unspecified.
TyreThermalSpec tyre_thermal_spec(double compound, double radius, double width);

struct TyreTemperature {
  double surface = 25, carcass = 25;  // deg C
};

// Heat flows of one tyre for one substep:
//  sliding: the friction power at the patch (W), |Fx slip| + |Fy lateral slip|;
//  rolling: the rolling-resistance power (W);
//  brake: the rotor temperature (deg C) the rim conducts from;
//  speed: the car's speed past the tyre (m/s), for the air and the road.
TyreTemperature step_tyre_temperature(const TyreThermalSpec& tyre, TyreTemperature t,
                                      double sliding, double rolling, double brake,
                                      double speed, double dt);

// Grip at the tread surface's temperature, relative to the tyre's working
// value (the scripted friction): below 1 cold and overheated.
double tyre_grip_factor(const TyreThermalSpec& tyre, double surface);

// Heat capacities (J/K) of the two lumps, for tests and the debug view.
double tyre_surface_capacity(const TyreThermalSpec& tyre);
double tyre_carcass_capacity(const TyreThermalSpec& tyre);

// Hot pressure (#461, OpenSLRR, beyond the original). The Tyre script sets a
// cold gauge pressure (bar, `inflation`, at ambient) and derives the tyre from
// it against `optimal_inflation`. The air inside follows the carcass
// temperature (ideal gas, constant volume). Against the pressure the tyre
// has at its compound's working carcass temperature (where the scripted
// values hold), the patched script's own laws scale the relaxation length
// (pressure^-0.5), the rolling resistance (pressure^-0.5) and the nominal
// load (pressure^0.25); the cornering stiffness grows as pressure^0.3 and
// falls as the tread rubber heats past its working temperature.
struct TyrePressureEffect {
  double pressure = 0;  // hot gauge pressure (bar); 0 when unknown
  double ratio = 1;     // against the pressure at the working temperature
  double relaxation = 1, rolling = 1, nominal_load = 1, stiffness = 1;
};
TyrePressureEffect tyre_pressure_effect(const TyreThermalSpec& tyre, double inflation,
                                        TyreTemperature t);
// The gauge pressure (bar) of a tyre set to `cold` bar at ambient, at the
// air temperature `carcass` (deg C).
double tyre_hot_pressure(double cold, double carcass);
// Working temperatures (deg C) of a compound's carcass and tread.
double tyre_working_carcass(const TyreThermalSpec& tyre);
double tyre_working_tread(const TyreThermalSpec& tyre);

// Tread wear (#461, OpenSLRR). The sliding work wears the tread, faster as
// it overheats (it tears and blisters): each joule counts tyre_wear_weight
// times. A tyre's life in such joules is the script's setMaxWear value
// times kWearJoulesPerUnit. Tyre.SetInflation writes (0.15 + 0.85 x
// rigidity^4) x 50000000000.0 x misinflation, but the original compiler kept
// that literal as 2755359744 (5e10 mod 2^32; the MWM class has the same
// float), so a new street tyre at its optimal pressure gets 2.76e9: about
// 210 MJ, a sport one 150, a soft racing one 88, a drag radial 48. A
// ten-second burnout on a hot sport tyre takes about a tenth of it.
inline constexpr double kWearJoulesPerUnit = .077;
double tyre_wear_weight(double surface);
// Grip and cornering stiffness of a tyre at condition `wear` (1 new, 0 worn
// out): a thinner tread squirms less, so it stiffens; under 15 % the cords
// show and the grip falls to 0.6.
double tyre_wear_grip(double wear);
double tyre_wear_stiffness(double wear);

}  // namespace slrr::physics
