#pragma once

#include "slrr/vehicle_dynamics.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace slrr::physics_telemetry {

inline constexpr std::uint32_t schema_version = 2;
inline constexpr std::size_t max_entities_per_tick = 64;
inline constexpr std::size_t max_record_bytes = 1024 * 1024;

struct Limits {
  std::uint64_t max_frames = 100000;
  std::uint64_t max_ticks = 250000;
  std::uint64_t max_input_transitions = 100000;
  std::uint64_t max_file_bytes = 256ull * 1024 * 1024;
};

struct Identity {
  std::string scenario;
  // Null means that no vehicle/content/configuration identity was available.
  std::optional<std::string> vehicle;
  std::optional<std::string> content_hash;
  std::optional<std::string> config_hash;
};

struct SpawnPose {
  physics::Vec3 position;
  physics::Quaternion orientation;
};

struct EntityState {
  std::uint64_t entity = 0; // Creation order, never an address.
  std::uint32_t resource = 0;
  bool active = false;
  bool available = false;
  // Captured once from the first successfully configured rigid-body frame.
  std::optional<SpawnPose> spawn_pose;
  physics::VehicleTelemetry vehicle;
};

struct InputTransition {
  std::uint64_t timestamp_us = 0; // Monotonic observation time, not replay time.
  std::optional<std::uint64_t> frame;
  std::optional<std::uint64_t> tick;
  std::uint64_t sequence = 0;
  int device = 0;
  int control = 0;
  double value = 0;
};

// A session is host-thread-only, like the VM and fixed-step scheduler. Writing
// is bounded and best-effort after startup; shutdown reports delayed I/O errors.
class Session {
 public:
  Session(const std::string& path, Identity identity, Limits limits,
          std::string& error) noexcept;
  // In-memory capture for hermetic probes/tests; it obeys the same byte bound.
  Session(Identity identity, Limits limits, std::string& error) noexcept;
  ~Session() noexcept;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool good() const noexcept;
  std::string output() const;
  void note_vehicle(std::uint32_t resource,
                    std::optional<std::string> config_hash = std::nullopt) noexcept;
  void note_content_hash(std::optional<std::string> hash) noexcept;
  void begin_frame(std::uint64_t frame, double render_dt,
                   double time_warp) noexcept;
  void set_simulation_time(double admitted, double discarded) noexcept;
  void note_physics_time(double admitted, double discarded) noexcept;
  void record_input(InputTransition transition) noexcept;
  void record_tick(std::uint64_t tick, double tick_dt,
                   const std::vector<EntityState>& entities) noexcept;
  void end_frame() noexcept;
  bool flush(std::string& error) noexcept;
  bool shutdown(std::string& error) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Process-wide production sink. No file is opened unless start is explicit.
bool start(const std::string& path, Identity identity, std::string& error,
           Limits limits = {}) noexcept;
bool enabled() noexcept;
void note_vehicle(std::uint32_t resource,
                  std::optional<std::string> config_hash = std::nullopt) noexcept;
void note_content_hash(std::optional<std::string> hash) noexcept;
void begin_frame(std::uint64_t frame, double render_dt, double time_warp) noexcept;
void set_simulation_time(double admitted, double discarded) noexcept;
void note_physics_time(double admitted, double discarded) noexcept;
void record_input(int device, int control, double value) noexcept;
void record_input(InputTransition transition) noexcept;
void record_tick(std::uint64_t tick, double tick_dt,
                 const std::vector<EntityState>& entities) noexcept;
void end_frame() noexcept;
bool flush(std::string& error) noexcept;
bool shutdown(std::string& error) noexcept;

// FNV-1a/64 is an identity fingerprint, not a cryptographic authenticity check.
std::string fnv1a64(const std::uint8_t* bytes, std::size_t size);
std::string fnv1a64(const std::string& bytes);
std::optional<std::string> hash_file(const std::string& path) noexcept;

struct ReplayInfo {
  std::uint64_t frames = 0, ticks = 0, inputs = 0;
  Identity identity;
};

struct ComparisonResult {
  bool equal = false;
  std::string report_json;
  std::string error;
};

bool validate_replay(const std::string& jsonl, ReplayInfo* info,
                     std::string& error) noexcept;
ComparisonResult compare_replay(const std::string& left,
                                const std::string& right,
                                double tolerance = 1e-9) noexcept;
ComparisonResult compare_replay_files(const std::string& left_path,
                                      const std::string& right_path,
                                      double tolerance = 1e-9) noexcept;
bool write_comparison_report(const std::string& path, const std::string& json,
                             std::string& error) noexcept;

struct ProbeResult {
  bool partition_equal = false;
  bool lag_equal = true;
  std::uint64_t partition_ticks = 0;
  std::string first_lag_mismatch;
};

// Runs two equal-tick render partitions and an intentionally time-dropping
// stall. When output_path is nonempty, only the baseline run is written there.
bool run_deterministic_probe(const std::string& output_path, ProbeResult& result,
                             std::string& error,
                             double tolerance = 1e-9) noexcept;
std::string deterministic_scenario(int partition) noexcept;

} // namespace slrr::physics_telemetry
