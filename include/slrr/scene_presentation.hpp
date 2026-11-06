#pragma once

namespace slrr {
// Cooperative lifecycle calls can yield after destroying the active camera.
// Keep the last complete frame through section changes; loading plates may
// still be presented, and input continues to pump.
inline int scene_transition_depth = 0;
inline bool scene_transition_active() { return scene_transition_depth != 0; }
class SceneTransitionScope {
 public:
  explicit SceneTransitionScope(bool active) : active_(active) {
    if (active_) ++scene_transition_depth;
  }
  ~SceneTransitionScope() { if (active_) --scene_transition_depth; }
  SceneTransitionScope(const SceneTransitionScope&) = delete;
  SceneTransitionScope& operator=(const SceneTransitionScope&) = delete;
 private:
  bool active_;
};
}  // namespace slrr
