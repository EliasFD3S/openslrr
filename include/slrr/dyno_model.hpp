#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace slrr {

// Combustion cycle recovered from SLRR_rev combustion-model.ts
// (codex/physics-reverse-port @ c65675fe), itself adapted from
// sl-remastered dyno_model.cpp. State is stored as float32; the
// 437 RPM witness is the acceptance check, not x87 bit parity.
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

// One session per DynoData instance. A failed calculate leaves the
// previous curve in place. Missing numeric fields use the Java defaults.
bool dyno_calculate(void* key, const std::unordered_map<std::string, float>& fields,
                    float table_size, DynoStats* stats);
float dyno_sample(void* key, float rpm, float nitro, bool watts);
float dyno_boost_fraction(void* key, float rpm);
void dyno_forget(void* key);

// False when the published low-RPM witness or the turbo/nitro checks diverge.
bool dyno_self_check(char* msg, std::size_t msg_cap);

}  // namespace slrr
