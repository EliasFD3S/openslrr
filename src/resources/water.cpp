#include "slrr/water.hpp"

#include <cmath>
#include <vector>

namespace slrr {
namespace {

struct Plane {
  WaterVec point{}, normal{0, 1, 0};
};

bool g_present = false;
Plane g_surface;
std::vector<Plane> g_limits;
double g_density = 0, g_viscosity = 0;
const void *g_owner = nullptr;

WaterVec unit(const WaterVec &v) {
  const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(length > 1e-9) || !std::isfinite(length)) return {0, 1, 0};
  return {v[0] / length, v[1] / length, v[2] / length};
}

double signed_distance(const Plane &plane, const WaterVec &p) {
  return (p[0] - plane.point[0]) * plane.normal[0] + (p[1] - plane.point[1]) * plane.normal[1] +
         (p[2] - plane.point[2]) * plane.normal[2];
}

}  // namespace

void water_set(const WaterVec &point, const WaterVec &normal, double density, double viscosity,
               const void *owner) {
  for (double v : point)
    if (!std::isfinite(v)) return;
  // A new water (a new map) starts without the last one's limits.
  g_limits.clear();
  g_surface = {point, unit(normal)};
  g_density = std::isfinite(density) ? density : 0;
  g_viscosity = std::isfinite(viscosity) ? viscosity : 0;
  g_present = true;
  g_owner = owner;
}

void water_add_limit(const WaterVec &point, const WaterVec &normal) {
  for (double v : point)
    if (!std::isfinite(v)) return;
  g_limits.push_back({point, unit(normal)});
}

void water_clear() {
  g_present = false;
  g_limits.clear();
  g_density = g_viscosity = 0;
  g_owner = nullptr;
}

const void *water_owner() { return g_owner; }

bool water_present() { return g_present; }

double water_depth(const WaterVec &position) {
  if (!g_present) return -1;
  for (const auto &limit : g_limits)
    if (signed_distance(limit, position) >= 0) return -1;
  return -signed_distance(g_surface, position);
}

double water_density() { return g_present ? g_density : 0; }
double water_viscosity() { return g_present ? g_viscosity : 0; }

}  // namespace slrr
