#pragma once
#include "slrr/gc_roots.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace slrr {
// Lights created by scripts through java.render.LightSource (#235). Each entry
// is keyed by the Java light object and attached to a parent GameRef (any
// ResourceRef with a GameRef state, e.g. a RenderRef). Position and direction
// are in the parent's local script space (metres, script Z); the renderer
// converts them with the parent's drawn matrix.
enum class ScriptLightKind : int32_t { Point = 0, Spot = 1, Rect = 2 };

struct ScriptLight {
  void *key = nullptr;    // the Java LightSource object
  void *parent = nullptr; // canonical GameRef key of the parent
  ScriptLightKind kind = ScriptLightKind::Point;
  std::array<float, 3> position{}, direction{0, -1, 0}, color{1, 1, 1};
  float intensity = 24.f, range = 20.f;
  float inner_degrees = 65.f, outer_degrees = 80.f; // half-angles
  float width = 0.f, height = 0.f;                  // RectLight only
  bool shadows = true, enabled = true;
};

// Limits applied by the natives. Inputs outside them are clamped; non-finite
// values are ignored (the previous value stays).
inline constexpr float kScriptLightMaxIntensity = 1000.f;
inline constexpr float kScriptLightMinRange = .2f, kScriptLightMaxRange = 500.f;
inline constexpr float kScriptLightMaxSize = 100.f;
inline constexpr float kScriptLightMaxPosition = 1.0e5f;

// Returns false when parent has no GameRef state; the light is not created.
bool script_lights_create(void *key, void *parent, ScriptLightKind kind);
ScriptLight *script_lights_get(void *key);
// Removes one light (LightSource.destroy). Unknown keys are ignored.
void script_lights_destroy(void *key);
// Called by gameref_remove: every light attached to that parent goes too.
void script_lights_forget_parent(void *parent);
// World teardown (gameref_clear, native re-registration).
void script_lights_clear();
size_t script_lights_count();
// Snapshot in creation order, so the renderer's light budget is stable.
std::vector<const ScriptLight *> script_lights_all();
// Keeps registered Java light objects alive until destroy or parent removal.
void script_lights_visit_gc_roots(const GcRootVisitor &visit);

// Scenery lights (#237): type-13 light resources placed as `render` children
// of map objects (street lamps). They have no Java object; a list is owned by
// one parent GameRef, the ground shell, and goes with it (gameref_remove,
// gameref_clear). Positions and directions are in the parent's local script
// space (metres, script Z), like ScriptLight.
inline constexpr uint32_t kLightGroupCity = 4; // cfg `flags 4`: night lights
struct SceneryLight {
  std::array<float, 3> position{}, direction{}, color{1, 1, 1};
  float range = 0.f;                               // authored, metres
  std::array<float, 3> attenuation{1.f, 0.f, 0.f}; // 1/(c + l*d + q*d*d)
  uint32_t group = 0;        // cfg `flags`
  uint32_t resource = 0;     // type-13 full id
  bool lights_scene = false; // point/spot; fake* are glow only
  bool spot = false;
};
using SceneryLightList = std::shared_ptr<const std::vector<SceneryLight>>;
// Replaces the parent's list. A parent without GameRef state, or an empty
// list, attaches nothing.
bool scenery_lights_attach(void *parent, SceneryLightList lights);
size_t scenery_lights_count();
// (canonical parent key, list) pairs; order is unspecified.
std::vector<std::pair<void *, SceneryLightList>> scenery_lights_all();

// Renderer selection for scenery lights outside garages. Eye, frustum planes
// and output positions are render space (script Z flipped). `matrix` returns
// a parent's drawn script matrix (scene_instances_render_matrix); parents
// without one are skipped. Only lights that light the scene, whose group is
// enabled (group & groups_on) and, with a frustum, whose light sphere (capped
// range) meets it are candidates: a lamp whose sphere is outside the view
// lights no visible pixel. The nearest `budget` within
// kSceneryLightMaxDistance win. Intensity fades to 0 over
// min(kSceneryLightFadeBand, 40 % of the cut distance, at least 20 m) before
// the first excluded candidate or the distance limit, so a lamp enters and
// leaves the set with no contribution while near lamps stay at full power.
inline constexpr float kSceneryLightMaxDistance = 500.f;
inline constexpr float kSceneryLightFadeBand = 150.f;
// Lighting range cap: authored street ranges (500 m) would make each lamp
// light the whole view; the shader window ends the light at this distance.
inline constexpr float kSceneryLightMaxRange = 45.f;
inline constexpr float kSceneryLightIntensity = .8f;
struct SceneryLightPick {
  std::array<float, 3> position{}, color{};
  std::array<float, 3> attenuation{1.f, 0.f, 0.f};
  float range = 0.f;     // capped
  float intensity = 0.f; // kSceneryLightIntensity * fade
  float distance = 0.f;  // to the eye
  const SceneryLight *source = nullptr;
};
// Lamps whose distance to `focus` (render space) is below `reach`, nearest
// first, full intensity, no frustum and no budget: the shadow candidates,
// which must not depend on where the camera looks.
std::vector<SceneryLightPick>
scenery_lights_near(const std::array<float, 3> &focus, float reach,
                    uint32_t groups_on,
                    const std::function<bool(void *, float *)> &matrix);
using SceneryLightFrustum = std::array<std::array<float, 4>, 6>;
std::vector<SceneryLightPick>
scenery_lights_select(const std::array<float, 3> &eye, size_t budget,
                      uint32_t groups_on,
                      const std::function<bool(void *, float *)> &matrix,
                      const SceneryLightFrustum *frustum = nullptr,
                      float max_distance = kSceneryLightMaxDistance);
} // namespace slrr
