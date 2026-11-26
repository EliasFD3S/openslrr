#pragma once
#include <array>

namespace slrr {

// Map water (#454), from the stock GroundRef natives: setWater(level,
// density, viscosity) and setWater(point, normal, density, viscosity) set the
// water's surface plane (script metres; the water lies below it, against the
// normal), addWaterLimit(point, normal) takes the water away on the side the
// normal points to. Valocity: setWater((0, -8, -1500), (0, 1, 0), 300, 50)
// and addWaterLimit((0, 0, -500), (0, 0, 1)): the sea below y -8 past
// z -500. One water per map; cleared when the map's actors go.
using WaterVec = std::array<double, 3>;
// owner: the GroundRef that set it; the water goes with that map.
void water_set(const WaterVec &point, const WaterVec &normal, double density, double viscosity,
               const void *owner = nullptr);
const void *water_owner();
void water_add_limit(const WaterVec &point, const WaterVec &normal);
void water_clear();
bool water_present();

// How deep `position` is under the water (metres along the normal), or a
// negative value above it or outside the limits.
double water_depth(const WaterVec &position);
// The water's density and viscosity as the script set them (0 without).
double water_density();
double water_viscosity();

}  // namespace slrr
