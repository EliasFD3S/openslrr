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

}  // namespace slrr::physics
