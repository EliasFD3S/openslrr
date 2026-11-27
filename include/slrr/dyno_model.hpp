#pragma once

#include <cstddef>
#include <functional>
#include <numbers>
#include <string>
#include <unordered_map>

namespace slrr {

inline constexpr double engine_rpm_to_rad = std::numbers::pi / 30.0;
// The recovered cubic mechanical loss, shared by dyno and crank integration.
// At WOT the sampled curve is already net of this loss; do not subtract twice.
inline double engine_friction_torque(double rpm) {
  return rpm > 0 ? rpm * rpm * (2e-8 / engine_rpm_to_rad) : 0;
}

// Combustion cycle recovered from SLRR_rev combustion-model.ts
// (codex/physics-reverse-port @ c65675fe), itself adapted from
// sl-remastered dyno_model.cpp. State is float32, not x87 bit parity.
// #203 converts cycle work to SI net crank torque/power; legacy output scales
// are intentionally not retained. Torque is Nm; power samples are watts.
struct DynoStats {
  bool ok = false;
  float rpm_step = 0.f;
  float maximum_rpm = 0.f;
  float displacement = 0.f;
  float compression = 0.f;
  float max_torque = 0.f;
  float max_hp = 0.f;
  float torque_rpm = 0.f;
  float hp_rpm = 0.f;
};

// Sessions are weak native payloads keyed by their DynoData instance; the
// registry does not keep that Java object alive. deleteNative/finalize releases
// one session, while world reinitialization releases the whole registry.
// A failed recalculation leaves the previous curve in place. A failed first
// calculation publishes no session. Missing numeric fields use Java defaults.
bool dyno_calculate(void* key, const std::unordered_map<std::string, float>& fields,
                    float table_size, DynoStats* stats);
float dyno_sample(void* key, float rpm, float nitro, bool watts);
// Net torque (Nm) against rpm from the session's current curves. The function
// keeps its own immutable copy: it stays valid, unchanged, after a
// recalculation or release, so another thread may call it (#203). A missing
// session gives zero torque.
std::function<double(double)> dyno_torque_function(void* key, float nitro);
float dyno_boost_fraction(void* key, float rpm);
void dyno_forget(void* key);
void dyno_clear_sessions();
std::size_t dyno_session_count();

// False when the published low-RPM witness or the turbo/nitro checks diverge.
bool dyno_self_check(char* msg, std::size_t msg_cap);

}  // namespace slrr
