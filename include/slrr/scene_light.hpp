#pragma once
#include <array>
#include <cmath>
namespace slrr {
// Renderer light contract, independent of legacy meshes and material formats.
// Position is render-space metres; intensity uses engine units (not calibrated
// lumens). This first implementation budgets one shadowed point source per
// interior.
struct PointLight {
  void *owner = nullptr;
  std::array<float, 3> position{}, color{1, 1, 1}, ambient{.04f, .04f, .04f};
  float intensity = 18.f;
  float range = 32.f;
  bool casts_shadows = true;
  bool valid() const {
    if (!std::isfinite(intensity) || intensity < 0 || !std::isfinite(range) ||
        range <= .1f)
      return false;
    for (int i = 0; i < 3; ++i)
      if (!std::isfinite(position[i]) || !std::isfinite(color[i]) ||
          color[i] < 0 || !std::isfinite(ambient[i]) || ambient[i] < 0)
        return false;
    return true;
  }
  bool operator==(const PointLight &) const = default;
};
// Unshadowed omnidirectional scenery lamp (#237): street lamps outside
// garages. Stock D3D falloff 1/(c + l*d + q*d*d) inside a range window.
struct LampLight {
  std::array<float, 3> position{}, color{1, 1, 1}, attenuation{1, 0, 0};
  float intensity = 1.f, range = 45.f;
  // Spot cone (car lights, #279): beam direction (unit, render space) and
  // the cosines of the inner and outer half-angles. An outer cosine below -1
  // is an omnidirectional lamp.
  std::array<float, 3> direction{0, 0, 0};
  float cos_inner = -1.f, cos_outer = -2.f;
  // Car lights (#283): the chassis the light is installed on, whether it is
  // a headlight beam, and its tile in the headlight shadow atlas (-1: none).
  const void *car = nullptr;
  bool headlight = false;
  int shadow_tile = -1;
  const void *source = nullptr; // stable identity (the registry entry)
  bool valid() const {
    for (int i = 0; i < 3; ++i)
      if (!std::isfinite(position[i]) || !std::isfinite(color[i]) ||
          color[i] < 0 || !std::isfinite(attenuation[i]) || attenuation[i] < 0)
        return false;
    return attenuation[0] + attenuation[1] + attenuation[2] > 0 &&
           std::isfinite(intensity) && intensity >= 0 &&
           std::isfinite(range) && range > .1f;
  }
};
// Broad fixture source with an explicit hood/cone, separate from point
// fallback.
struct SpotLight {
  void *owner = nullptr;
  std::array<float, 3> position{}, direction{0, -1, 0}, color{1, 1, 1};
  float intensity = 24.f, range = 20.f;
  float outer_cos =
      .173648f; // Fixed 65-degree inner / default 80-degree outer half-angle
  bool valid() const {
    float n = 0;
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(position[i]) || !std::isfinite(direction[i]) ||
          !std::isfinite(color[i]) || color[i] < 0)
        return false;
      n += direction[i] * direction[i];
    }
    return n > .99f && n < 1.01f && std::isfinite(intensity) &&
           intensity >= 0 && std::isfinite(range) && range > .1f &&
           std::isfinite(outer_cos) && outer_cos > 0 && outer_cos < .422618f;
  }
};
} // namespace slrr
