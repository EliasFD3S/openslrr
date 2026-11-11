#include "slrr/physics_telemetry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace slrr::physics_telemetry {
namespace {

constexpr std::size_t kSummaryReserve = 4096;
constexpr std::size_t kMaxReplayRecords = 400000;
constexpr std::uint64_t kMaxReplayBytes = 256ull * 1024 * 1024;

void json_string(std::string& out, const std::string& value) {
  static constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  const std::size_t size = std::min<std::size_t>(value.size(), 160);
  for (std::size_t i = 0; i < size; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c) {
      case '"': out += "\""; break;
      case '\\': out += "\\"; break;
      case '\b': out += "\b"; break;
      case '\f': out += "\f"; break;
      case '\n': out += "\n"; break;
      case '\r': out += "\r"; break;
      case '\t': out += "\t"; break;
      default:
        if (c < 0x20 || c >= 0x7f) {
          out += "\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 15]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void json_number(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::general,
                                    std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    out += "null";
    return;
  }
  out.append(buffer, result.ptr);
}

void json_u64(std::string& out, std::uint64_t value) {
  out += std::to_string(value);
}

void json_optional_string(std::string& out, const std::optional<std::string>& value) {
  if (value) json_string(out, *value);
  else out += "null";
}

void json_optional_u64(std::string& out, const std::optional<std::uint64_t>& value) {
  if (value) json_u64(out, *value);
  else out += "null";
}

void json_optional_u32(std::string& out, const std::optional<std::uint32_t>& value) {
  if (value) out += std::to_string(*value);
  else out += "null";
}

void json_vec3(std::string& out, const physics::Vec3& v) {
  out.push_back('[');
  json_number(out, v.x);
  out.push_back(',');
  json_number(out, v.y);
  out.push_back(',');
  json_number(out, v.z);
  out.push_back(']');
}

std::string header_json(const Identity& id, const Limits& limits) {
  std::string out = "{\"schema\":\"slrr-physics-telemetry\",\"version\":" +
      std::to_string(schema_version) + ",\"type\":\"run\",\"scenario\":";
  json_string(out, id.scenario);
  out += ",\"vehicle\":";
  json_optional_string(out, id.vehicle);
  out += ",\"content_hash\":";
  json_optional_string(out, id.content_hash);
  out += ",\"config_hash\":";
  json_optional_string(out, id.config_hash);
  out += ",\"limits\":{\"max_frames\":";
  json_u64(out, limits.max_frames);
  out += ",\"max_ticks\":";
  json_u64(out, limits.max_ticks);
  out += ",\"max_input_transitions\":";
  json_u64(out, limits.max_input_transitions);
  out += ",\"max_file_bytes\":";
  json_u64(out, limits.max_file_bytes);
  out += "}}";
  return out;
}

std::string entity_json(const EntityState& entity) {
  std::string out = "{\"entity\":";
  json_u64(out, entity.entity);
  out += ",\"resource\":";
  json_u64(out, entity.resource);
  out += ",\"active\":";
  out += entity.active ? "true" : "false";
  out += ",\"available\":";
  out += entity.available ? "true" : "false";
  if (!entity.available) {
    out.push_back('}');
    return out;
  }
  const auto& v = entity.vehicle;
  out += ",\"spawn_pose\":";
  if (!entity.spawn_pose) {
    out += "null";
  } else {
    out += "{\"position\":";
    json_vec3(out, entity.spawn_pose->position);
    out += ",\"orientation\":[";
    json_number(out, entity.spawn_pose->orientation.x);
    out.push_back(',');
    json_number(out, entity.spawn_pose->orientation.y);
    out.push_back(',');
    json_number(out, entity.spawn_pose->orientation.z);
    out.push_back(',');
    json_number(out, entity.spawn_pose->orientation.w);
    out += "]}";
  }
  out += ",\"chassis\":{\"mass\":";
  json_number(out, v.mass);
  out += ",\"position\":";
  json_vec3(out, v.position);
  out += ",\"velocity\":";
  json_vec3(out, v.velocity);
  out += ",\"momentum\":";
  json_vec3(out, v.momentum);
  out += ",\"omega\":";
  json_vec3(out, v.omega);
  out += ",\"orientation\":[";
  json_number(out, v.orientation.x);
  out.push_back(',');
  json_number(out, v.orientation.y);
  out.push_back(',');
  json_number(out, v.orientation.z);
  out.push_back(',');
  json_number(out, v.orientation.w);
  out += "]}";
  out += ",\"drivetrain\":{\"dynamics_ticks\":";
  json_u64(out, v.dynamics_ticks);
  out += ",\"engine_omega\":";
  json_number(out, v.engine_omega);
  out += ",\"engine_running\":";
  out += v.engine_running ? "true" : "false";
  out += ",\"gear\":";
  out += std::to_string(v.gear);
  out += ",\"throttle\":";
  json_number(out, v.throttle);
  out += ",\"brake\":";
  json_number(out, v.brake);
  out += ",\"handbrake\":";
  json_number(out, v.handbrake);
  out += ",\"clutch\":";
  json_number(out, v.clutch);
  out.push_back('}');
  out += ",\"wheels\":[";
  for (std::size_t i = 0; i < v.wheels.size(); ++i) {
    if (i) out.push_back(',');
    const auto& w = v.wheels[i];
    out += "{\"enabled\":";
    out += w.enabled ? "true" : "false";
    out += ",\"contact\":";
    out += w.contact ? "true" : "false";
    out += ",\"length\":";
    json_number(out, w.length);
    out += ",\"speed\":";
    json_number(out, w.speed);
    out += ",\"force\":";
    json_number(out, w.force);
    out += ",\"spin\":";
    json_number(out, w.spin);
    out += ",\"rotation\":";
    json_number(out, w.rotation);
    out += ",\"slip_speed\":";
    json_number(out, w.slip_speed);
    out += ",\"lateral_speed\":";
    json_number(out, w.lateral_speed);
    out += ",\"tire_longitudinal\":";
    json_number(out, w.tire_longitudinal);
    out += ",\"tire_lateral\":";
    json_number(out, w.tire_lateral);
    out += ",\"contact_impulse\":";
    json_number(out, w.contact_impulse);
    out += ",\"center\":";
    json_vec3(out, w.center);
    out += ",\"normal\":";
    json_vec3(out, w.normal);
    out += ",\"patch\":";
    json_vec3(out, w.patch);
    out.push_back('}');
  }
  out += "],\"ground_impact\":{\"point\":";
  json_vec3(out, v.ground_impact.point);
  out += ",\"normal\":";
  json_vec3(out, v.ground_impact.normal);
  out += ",\"speed\":";
  json_number(out, v.ground_impact.speed);
  out += "},\"chassis_contact_impulse\":";
  json_number(out, v.chassis_contact_impulse);
  out += ",\"contacts\":[";
  const std::size_t contact_count = std::min<std::size_t>(
      v.contact_count, physics::VehicleTelemetry::max_contacts);
  for (std::size_t i = 0; i < contact_count; ++i) {
    if (i) out.push_back(',');
    const auto& contact = v.contacts[i];
    out += "{\"kind\":" + std::to_string(contact.kind);
    out += ",\"wheel\":" + std::to_string(contact.wheel);
    out += ",\"resource\":";
    json_optional_u32(out, contact.identity.resource);
    out += ",\"mesh\":";
    json_optional_u32(out, contact.identity.mesh);
    out += ",\"triangle\":";
    json_optional_u32(out, contact.identity.triangle);
    out += ",\"material\":";
    json_optional_u32(out, contact.identity.material);
    out += ",\"point\":";
    json_vec3(out, contact.point);
    out += ",\"normal\":";
    json_vec3(out, contact.normal);
    out += ",\"penetration\":";
    json_number(out, contact.penetration);
    out += ",\"impulse\":";
    json_number(out, contact.impulse);
    out.push_back('}');
  }
  out += "],\"dropped_contacts\":" + std::to_string(v.dropped_contacts);
  out.push_back('}');
  return out;
}

std::uint64_t monotonic_us() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

struct Session::Impl {
  Impl(const std::string& output_path, Identity id, Limits bound, std::string& error,
       bool memory)
      : path(output_path), identity(std::move(id)), limits(bound), memory(memory) {
    if (limits.max_file_bytes < kSummaryReserve + 1024 ||
        limits.max_frames == 0 || limits.max_ticks == 0 ||
        limits.max_input_transitions == 0) {
      fail("physics telemetry limits are too small or zero", error);
      return;
    }
    if (!memory) {
      file.open(path, std::ios::binary | std::ios::trunc);
      if (!file) fail("cannot open physics telemetry output: " + path, error);
    }
  }

  void fail(const std::string& message, std::string& output_error) {
    if (error.empty()) error = message;
    output_error = error;
    failed = true;
  }

  bool write_line(const std::string& line, bool summary = false) {
    if (failed || closed) return false;
    if (line.size() > max_record_bytes) {
      truncated = true;
      return false;
    }
    if (bytes + line.size() + 1 + (summary ? 0 : kSummaryReserve) > limits.max_file_bytes) {
      truncated = true;
      return false;
    }
    if (memory) {
      capture += line;
      capture.push_back('\n');
    } else {
      file.write(line.data(), static_cast<std::streamsize>(line.size()));
      file.put('\n');
      if (!file) {
        failed = true;
        if (error.empty()) error = "cannot write physics telemetry output: " + path;
        return false;
      }
    }
    bytes += line.size() + 1;
    return true;
  }

  bool ensure_header() {
    if (header_written) return !failed;
    header_written = true;
    return write_line(header_json(identity, limits));
  }

  void drop(const std::string& line, char kind) {
    (void)line;
    truncated = true;
    if (kind == 'f') ++dropped_frames;
    else if (kind == 't') ++dropped_ticks;
    else if (kind == 'i') ++dropped_inputs;
  }

  void emit(std::string&& line, char kind) {
    if (failed || closed) return;
    if (!ensure_header()) return;
    if (!write_line(line)) drop(line, kind);
  }

  std::string summary_json() const {
    std::string out = "{\"type\":\"summary\",\"frames\":";
    json_u64(out, frames);
    out += ",\"ticks\":";
    json_u64(out, ticks);
    out += ",\"inputs\":";
    json_u64(out, inputs);
    out += ",\"dropped_frames\":";
    json_u64(out, dropped_frames);
    out += ",\"dropped_ticks\":";
    json_u64(out, dropped_ticks);
    out += ",\"dropped_inputs\":";
    json_u64(out, dropped_inputs);
    out += ",\"dropped_entities\":";
    json_u64(out, dropped_entities);
    out += ",\"truncated\":";
    out += truncated ? "true" : "false";
    out += ",\"admitted_sim_dt\":";
    json_number(out, admitted_sim);
    out += ",\"discarded_sim_dt\":";
    json_number(out, discarded_sim);
    out += ",\"admitted_physics_dt\":";
    json_number(out, admitted_physics);
    out += ",\"discarded_physics_dt\":";
    json_number(out, discarded_physics);
    out.push_back('}');
    return out;
  }

  bool finish(std::string& output_error) {
    if (closed) {
      output_error = error;
      return !failed;
    }
    if (!failed) {
      ensure_header();
      write_line(summary_json(), true);
      if (memory) {
        // Memory captures are flushed by every append.
      } else {
        file.flush();
        if (!file && error.empty()) {
          failed = true;
          error = "cannot flush physics telemetry output: " + path;
        }
        file.close();
        if (!file && error.empty()) {
          failed = true;
          error = "cannot close physics telemetry output: " + path;
        }
      }
    } else if (file.is_open()) {
      file.close();
    }
    closed = true;
    output_error = error;
    return !failed;
  }

  std::string path;
  Identity identity;
  Limits limits;
  bool memory = false, failed = false, closed = false, header_written = false;
  bool truncated = false, frame_open = false;
  std::ofstream file;
  std::string capture, error;
  std::uint64_t bytes = 0, frames = 0, ticks = 0, inputs = 0;
  std::uint64_t dropped_frames = 0, dropped_ticks = 0, dropped_inputs = 0;
  std::uint64_t dropped_entities = 0, input_sequence = 0;
  std::uint64_t frame = 0;
  std::optional<std::uint64_t> current_tick;
  double render_dt = 0, time_warp = 1;
  double frame_admitted_sim = 0, frame_discarded_sim = 0;
  double frame_admitted_physics = 0, frame_discarded_physics = 0;
  double admitted_sim = 0, discarded_sim = 0;
  double admitted_physics = 0, discarded_physics = 0;
};

Session::Session(const std::string& path, Identity identity, Limits limits,
                 std::string& error) noexcept
try : impl_(std::make_unique<Impl>(path, std::move(identity), limits, error, false)) {
} catch (const std::exception& e) {
  error = std::string("cannot initialize physics telemetry: ") + e.what();
} catch (...) {
  error = "cannot initialize physics telemetry";
}

Session::Session(Identity identity, Limits limits, std::string& error) noexcept
try : impl_(std::make_unique<Impl>(std::string{}, std::move(identity), limits, error, true)) {
} catch (const std::exception& e) {
  error = std::string("cannot initialize physics telemetry: ") + e.what();
} catch (...) {
  error = "cannot initialize physics telemetry";
}

Session::~Session() noexcept {
  if (impl_) {
    std::string ignored;
    impl_->finish(ignored);
  }
}

bool Session::good() const noexcept { return impl_ && !impl_->failed && !impl_->closed; }
std::string Session::output() const { return impl_ ? impl_->capture : std::string{}; }

void Session::note_vehicle(std::uint32_t resource,
                           std::optional<std::string> config_hash) noexcept {
  if (!impl_ || impl_->header_written) return;
  try {
    if (!impl_->identity.vehicle) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "resource:0x%08x", resource);
      impl_->identity.vehicle = buffer;
    }
    if (config_hash && !impl_->identity.config_hash)
      impl_->identity.config_hash = std::move(config_hash);
  } catch (...) {
    impl_->failed = true;
    impl_->error = "cannot record vehicle telemetry identity";
  }
}

void Session::note_content_hash(std::optional<std::string> hash) noexcept {
  if (!impl_ || impl_->header_written) return;
  try { impl_->identity.content_hash = std::move(hash); }
  catch (...) { impl_->failed = true; impl_->error = "cannot record content hash"; }
}

void Session::begin_frame(std::uint64_t frame, double render_dt,
                          double time_warp) noexcept {
  if (!impl_ || impl_->failed || impl_->closed) return;
  if (impl_->frame_open) end_frame();
  impl_->frame_open = true;
  impl_->frame = frame;
  impl_->current_tick.reset();
  impl_->render_dt = render_dt;
  impl_->time_warp = time_warp;
  impl_->frame_admitted_sim = impl_->frame_discarded_sim = 0;
  impl_->frame_admitted_physics = impl_->frame_discarded_physics = 0;
}

void Session::set_simulation_time(double admitted, double discarded) noexcept {
  if (!impl_ || !impl_->frame_open) return;
  impl_->frame_admitted_sim = admitted;
  impl_->frame_discarded_sim = discarded;
}

void Session::note_physics_time(double admitted, double discarded) noexcept {
  if (!impl_ || !impl_->frame_open) return;
  impl_->frame_admitted_physics = admitted;
  impl_->frame_discarded_physics = discarded;
}

void Session::record_input(InputTransition transition) noexcept {
  if (!impl_ || impl_->failed || impl_->closed) return;
  try {
    if (impl_->inputs >= impl_->limits.max_input_transitions) {
      ++impl_->dropped_inputs;
      impl_->truncated = true;
      return;
    }
    if (transition.sequence == 0) transition.sequence = ++impl_->input_sequence;
    else impl_->input_sequence = std::max(impl_->input_sequence, transition.sequence);
    if (!transition.frame && impl_->frame_open) transition.frame = impl_->frame;
    if (!transition.tick) transition.tick = impl_->current_tick;
    std::string out = "{\"type\":\"input\",\"timestamp_us\":";
    json_u64(out, transition.timestamp_us);
    out += ",\"frame\":";
    json_optional_u64(out, transition.frame);
    out += ",\"tick\":";
    json_optional_u64(out, transition.tick);
    out += ",\"sequence\":";
    json_u64(out, transition.sequence);
    out += ",\"device\":" + std::to_string(transition.device);
    out += ",\"control\":" + std::to_string(transition.control);
    out += ",\"value\":";
    json_number(out, transition.value);
    out.push_back('}');
    impl_->emit(std::move(out), 'i');
    ++impl_->inputs;
  } catch (...) {
    impl_->failed = true;
    impl_->error = "cannot serialize physics input transition";
  }
}

void Session::record_tick(std::uint64_t tick, double tick_dt,
                          const std::vector<EntityState>& entities) noexcept {
  if (!impl_ || impl_->failed || impl_->closed) return;
  try {
    impl_->current_tick = tick;
    if (impl_->ticks >= impl_->limits.max_ticks) {
      ++impl_->dropped_ticks;
      impl_->truncated = true;
      return;
    }
    std::string out = "{\"type\":\"tick\",\"frame\":";
    if (impl_->frame_open) json_u64(out, impl_->frame);
    else out += "null";
    out += ",\"tick\":";
    json_u64(out, tick);
    out += ",\"tick_dt\":";
    json_number(out, tick_dt);
    out += ",\"entities\":[";
    const std::size_t count = std::min(entities.size(), max_entities_per_tick);
    for (std::size_t i = 0; i < count; ++i) {
      if (i) out.push_back(',');
      out += entity_json(entities[i]);
    }
    if (entities.size() > count) {
      impl_->dropped_entities += entities.size() - count;
      impl_->truncated = true;
    }
    out += "]}";
    impl_->emit(std::move(out), 't');
    ++impl_->ticks;
  } catch (...) {
    impl_->failed = true;
    impl_->error = "cannot serialize physics tick";
  }
}

void Session::end_frame() noexcept {
  if (!impl_ || !impl_->frame_open || impl_->failed || impl_->closed) return;
  try {
    impl_->frame_open = false;
    if (impl_->frames >= impl_->limits.max_frames) {
      ++impl_->dropped_frames;
      impl_->truncated = true;
      return;
    }
    const double admitted_sim = std::isfinite(impl_->frame_admitted_sim)
                                    ? impl_->frame_admitted_sim : 0;
    const double discarded_sim = std::isfinite(impl_->frame_discarded_sim)
                                     ? impl_->frame_discarded_sim : 0;
    const double admitted_physics = std::isfinite(impl_->frame_admitted_physics)
                                        ? impl_->frame_admitted_physics : 0;
    const double discarded_physics = std::isfinite(impl_->frame_discarded_physics)
                                         ? impl_->frame_discarded_physics : 0;
    impl_->admitted_sim += admitted_sim;
    impl_->discarded_sim += discarded_sim;
    impl_->admitted_physics += admitted_physics;
    impl_->discarded_physics += discarded_physics;
    std::string out = "{\"type\":\"frame\",\"frame\":";
    json_u64(out, impl_->frame);
    out += ",\"render_dt\":";
    json_number(out, impl_->render_dt);
    out += ",\"admitted_sim_dt\":";
    json_number(out, admitted_sim);
    out += ",\"discarded_sim_dt\":";
    json_number(out, discarded_sim);
    out += ",\"admitted_physics_dt\":";
    json_number(out, admitted_physics);
    out += ",\"discarded_physics_dt\":";
    json_number(out, discarded_physics);
    out += ",\"time_warp\":";
    json_number(out, impl_->time_warp);
    out.push_back('}');
    impl_->emit(std::move(out), 'f');
    ++impl_->frames;
  } catch (...) {
    impl_->failed = true;
    impl_->error = "cannot serialize physics frame";
  }
}

bool Session::flush(std::string& error) noexcept {
  if (!impl_) return false;
  try {
    if (!impl_->failed && !impl_->memory) {
      impl_->file.flush();
      if (!impl_->file) {
        impl_->failed = true;
        if (impl_->error.empty()) impl_->error = "cannot flush physics telemetry output: " + impl_->path;
      }
    }
    error = impl_->error;
    return !impl_->failed;
  } catch (...) {
    error = "cannot flush physics telemetry output";
    return false;
  }
}

bool Session::shutdown(std::string& error) noexcept {
  if (!impl_) return false;
  try { return impl_->finish(error); }
  catch (...) { error = "cannot close physics telemetry output"; return false; }
}

namespace {
std::unique_ptr<Session> g_session;

template <typename F>
void with_session(F&& fn) noexcept {
  if (!g_session) return;
  try { fn(*g_session); } catch (...) {}
}
} // namespace

bool start(const std::string& path, Identity identity, std::string& error,
           Limits limits) noexcept {
  try {
    if (path.empty()) {
      error = "physics telemetry requires an explicit nonempty output path";
      return false;
    }
    if (g_session) {
      std::string ignored;
      g_session->shutdown(ignored);
      g_session.reset();
    }
    auto session = std::make_unique<Session>(path, std::move(identity), limits, error);
    if (!session->good()) {
      if (error.empty()) error = "cannot initialize physics telemetry output: " + path;
      return false;
    }
    g_session = std::move(session);
    return true;
  } catch (const std::exception& e) {
    error = std::string("cannot start physics telemetry: ") + e.what();
    return false;
  } catch (...) {
    error = "cannot start physics telemetry";
    return false;
  }
}

bool enabled() noexcept { return g_session && g_session->good(); }
void note_vehicle(std::uint32_t resource, std::optional<std::string> hash) noexcept {
  with_session([&](Session& s) { s.note_vehicle(resource, std::move(hash)); });
}
void note_content_hash(std::optional<std::string> hash) noexcept {
  with_session([&](Session& s) { s.note_content_hash(std::move(hash)); });
}
void begin_frame(std::uint64_t frame, double dt, double warp) noexcept {
  with_session([&](Session& s) { s.begin_frame(frame, dt, warp); });
}
void set_simulation_time(double admitted, double discarded) noexcept {
  with_session([&](Session& s) { s.set_simulation_time(admitted, discarded); });
}
void note_physics_time(double admitted, double discarded) noexcept {
  with_session([&](Session& s) { s.note_physics_time(admitted, discarded); });
}
void record_input(int device, int control, double value) noexcept {
  with_session([&](Session& s) {
    InputTransition transition;
    transition.timestamp_us = monotonic_us();
    transition.device = device;
    transition.control = control;
    transition.value = value;
    s.record_input(std::move(transition));
  });
}
void record_input(InputTransition transition) noexcept {
  with_session([&](Session& s) { s.record_input(std::move(transition)); });
}
void record_tick(std::uint64_t tick, double dt, const std::vector<EntityState>& entities) noexcept {
  with_session([&](Session& s) { s.record_tick(tick, dt, entities); });
}
void end_frame() noexcept { with_session([](Session& s) { s.end_frame(); }); }
bool flush(std::string& error) noexcept { return g_session ? g_session->flush(error) : true; }
bool shutdown(std::string& error) noexcept {
  if (!g_session) return true;
  const bool ok = g_session->shutdown(error);
  g_session.reset();
  return ok;
}

std::string fnv1a64(const std::uint8_t* bytes, std::size_t size) {
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "fnv1a64:%016llx",
                static_cast<unsigned long long>(hash));
  return buffer;
}
std::string fnv1a64(const std::string& bytes) {
  return fnv1a64(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}
std::optional<std::string> hash_file(const std::string& path) noexcept {
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::uint64_t hash = 14695981039346656037ull;
    std::array<char, 16384> buffer{};
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto count = input.gcount();
      for (std::streamsize i = 0; i < count; ++i) {
        hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
        hash *= 1099511628211ull;
      }
    }
    if (!input.eof()) return std::nullopt;
    char text[32];
    std::snprintf(text, sizeof(text), "fnv1a64:%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(text);
  } catch (...) {
    return std::nullopt;
  }
}

// ------------------------- Strict bounded JSONL reader ----------------------
namespace {

struct Json {
  enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;
};

class JsonParser {
 public:
  JsonParser(const std::string& source, std::string& error) : source_(source), error_(error) {}
  bool parse(Json& out) {
    if (!value(out, 0)) return false;
    whitespace();
    if (position_ != source_.size()) return fail("trailing JSON content");
    return true;
  }
 private:
  bool fail(const std::string& message) {
    if (error_.empty()) error_ = message + " at byte " + std::to_string(position_);
    return false;
  }
  void whitespace() {
    while (position_ < source_.size()) {
      const char c = source_[position_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      ++position_;
    }
  }
  bool consume(char c) {
    whitespace();
    if (position_ >= source_.size() || source_[position_] != c) return false;
    ++position_;
    return true;
  }
  bool value(Json& out, unsigned depth) {
    if (depth > 16) return fail("JSON nesting limit exceeded");
    whitespace();
    if (position_ >= source_.size()) return fail("missing JSON value");
    const char c = source_[position_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"') { out.kind = Json::Kind::String; return string(out.text); }
    if (c == 't' && source_.compare(position_, 4, "true") == 0) {
      position_ += 4; out.kind = Json::Kind::Bool; out.boolean = true; return true;
    }
    if (c == 'f' && source_.compare(position_, 5, "false") == 0) {
      position_ += 5; out.kind = Json::Kind::Bool; out.boolean = false; return true;
    }
    if (c == 'n' && source_.compare(position_, 4, "null") == 0) {
      position_ += 4; out.kind = Json::Kind::Null; return true;
    }
    return number(out);
  }
  bool object(Json& out, unsigned depth) {
    ++position_;
    out.kind = Json::Kind::Object;
    whitespace();
    if (position_ < source_.size() && source_[position_] == '}') { ++position_; return true; }
    for (;;) {
      whitespace();
      std::string key;
      if (!string(key)) return false;
      for (const auto& member : out.members)
        if (member.first == key) return fail("duplicate JSON member");
      if (!consume(':')) return fail("expected ':'");
      Json child;
      if (!value(child, depth + 1)) return false;
      out.members.emplace_back(std::move(key), std::move(child));
      if (out.members.size() > 128) return fail("too many JSON members");
      whitespace();
      if (position_ < source_.size() && source_[position_] == '}') { ++position_; return true; }
      if (!consume(',')) return fail("expected ',' or '}'");
    }
  }
  bool array(Json& out, unsigned depth) {
    ++position_;
    out.kind = Json::Kind::Array;
    whitespace();
    if (position_ < source_.size() && source_[position_] == ']') { ++position_; return true; }
    for (;;) {
      Json child;
      if (!value(child, depth + 1)) return false;
      out.items.push_back(std::move(child));
      if (out.items.size() > 100000) return fail("too many JSON array entries");
      whitespace();
      if (position_ < source_.size() && source_[position_] == ']') { ++position_; return true; }
      if (!consume(',')) return fail("expected ',' or ']'");
    }
  }
  bool hex4(unsigned& value) {
    if (position_ + 4 > source_.size()) return fail("truncated Unicode escape");
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const char c = source_[position_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value += c - '0';
      else if (c >= 'a' && c <= 'f') value += c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') value += c - 'A' + 10;
      else return fail("invalid Unicode escape");
    }
    return true;
  }
  bool string(std::string& out) {
    whitespace();
    if (position_ >= source_.size() || source_[position_] != '"') return fail("expected JSON string");
    ++position_;
    while (position_ < source_.size()) {
      const unsigned char c = static_cast<unsigned char>(source_[position_++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("unescaped JSON control character");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        if (out.size() > 4096) return fail("JSON string too long");
        continue;
      }
      if (position_ >= source_.size()) return fail("truncated JSON escape");
      const char escaped = source_[position_++];
      switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned code;
          if (!hex4(code)) return false;
          if (code >= 0xd800 && code <= 0xdbff) {
            if (position_ + 2 > source_.size() || source_[position_] != '\\' ||
                source_[position_ + 1] != 'u') return fail("unpaired JSON high surrogate");
            position_ += 2;
            unsigned low;
            if (!hex4(low)) return false;
            if (low < 0xdc00 || low > 0xdfff) return fail("invalid JSON surrogate pair");
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
          } else if (code >= 0xdc00 && code <= 0xdfff) {
            return fail("unpaired JSON low surrogate");
          }
          if (code <= 0x7f) out.push_back(static_cast<char>(code));
          else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          } else if (code <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          }
          break;
        }
        default: return fail("invalid JSON escape");
      }
      if (out.size() > 4096) return fail("JSON string too long");
    }
    return fail("unterminated JSON string");
  }
  bool number(Json& out) {
    const std::size_t start = position_;
    if (position_ < source_.size() && source_[position_] == '-') ++position_;
    if (position_ >= source_.size()) return fail("invalid JSON number");
    if (source_[position_] == '0') ++position_;
    else {
      if (source_[position_] < '1' || source_[position_] > '9') return fail("invalid JSON number");
      while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
    }
    if (position_ < source_.size() && source_[position_] == '.') {
      ++position_;
      const std::size_t digits = position_;
      while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
      if (digits == position_) return fail("missing JSON fraction digits");
    }
    if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
      ++position_;
      if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) ++position_;
      const std::size_t digits = position_;
      while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
      if (digits == position_) return fail("missing JSON exponent digits");
    }
    out.text = source_.substr(start, position_ - start);
    char* end = nullptr;
    out.number = std::strtod(out.text.c_str(), &end);
    if (!end || *end || !std::isfinite(out.number)) return fail("non-finite JSON number");
    out.kind = Json::Kind::Number;
    return true;
  }
  const std::string& source_;
  std::string& error_;
  std::size_t position_ = 0;
};

const Json* member(const Json& object, const char* name) {
  if (object.kind != Json::Kind::Object) return nullptr;
  for (const auto& entry : object.members)
    if (entry.first == name) return &entry.second;
  return nullptr;
}

bool kind(const Json& object, const char* name, Json::Kind expected, std::string& error) {
  const Json* value = member(object, name);
  if (!value || value->kind != expected) {
    error = std::string("replay field '") + name + "' has the wrong type or is missing";
    return false;
  }
  return true;
}

bool text(const Json& object, const char* name, std::string& output, std::string& error) {
  if (!kind(object, name, Json::Kind::String, error)) return false;
  output = member(object, name)->text;
  if (output.size() > 160) { error = std::string("replay string '") + name + "' is too long"; return false; }
  return true;
}

bool boolean(const Json& object, const char* name, std::string& error) {
  return kind(object, name, Json::Kind::Bool, error);
}

bool uint_value(const Json& object, const char* name, std::uint64_t& output, std::string& error) {
  if (!kind(object, name, Json::Kind::Number, error)) return false;
  const auto& source = member(object, name)->text;
  const auto result = std::from_chars(source.data(), source.data() + source.size(), output);
  if (result.ec != std::errc{} || result.ptr != source.data() + source.size()) {
    error = std::string("replay field '") + name + "' is not an unsigned integer";
    return false;
  }
  return true;
}

bool int_value(const Json& object, const char* name, std::string& error) {
  if (!kind(object, name, Json::Kind::Number, error)) return false;
  const auto& source = member(object, name)->text;
  long long value = 0;
  const auto result = std::from_chars(source.data(), source.data() + source.size(), value);
  if (result.ec != std::errc{} || result.ptr != source.data() + source.size()) {
    error = std::string("replay field '") + name + "' is not an integer";
    return false;
  }
  return true;
}

bool number_or_null(const Json& object, const char* name, std::string& error) {
  const Json* value = member(object, name);
  if (!value || (value->kind != Json::Kind::Number && value->kind != Json::Kind::Null)) {
    error = std::string("replay field '") + name + "' must be a finite number or null";
    return false;
  }
  return true;
}

bool finite_number(const Json& object, const char* name, bool nonnegative,
                   std::string& error) {
  if (!kind(object, name, Json::Kind::Number, error)) return false;
  if (nonnegative && member(object, name)->number < 0) {
    error = std::string("replay field '") + name + "' must be nonnegative";
    return false;
  }
  return true;
}

bool optional_uint(const Json& object, const char* name, std::string& error) {
  const Json* value = member(object, name);
  if (!value) { error = std::string("replay field '") + name + "' is missing"; return false; }
  if (value->kind == Json::Kind::Null) return true;
  Json wrapper;
  wrapper.kind = Json::Kind::Object;
  wrapper.members.emplace_back(name, *value);
  std::uint64_t ignored;
  return uint_value(wrapper, name, ignored, error);
}

bool vec(const Json& object, const char* name, std::size_t size, std::string& error) {
  const Json* value = member(object, name);
  if (!value || value->kind != Json::Kind::Array || value->items.size() != size) {
    error = std::string("replay vector '") + name + "' has the wrong shape";
    return false;
  }
  for (const auto& item : value->items)
    if (item.kind != Json::Kind::Number && item.kind != Json::Kind::Null) {
      error = std::string("replay vector '") + name + "' contains a nonnumeric value";
      return false;
    }
  return true;
}

bool validate_entity(const Json& entity, std::string& error) {
  std::uint64_t ignored;
  if (!uint_value(entity, "entity", ignored, error) ||
      !uint_value(entity, "resource", ignored, error) ||
      !boolean(entity, "active", error) || !boolean(entity, "available", error)) return false;
  if (member(entity, "entity")->number == 0) {
    error = "replay entity ID must be positive";
    return false;
  }
  const bool available = member(entity, "available")->boolean;
  if (!available) return true;
  const Json* chassis = member(entity, "chassis");
  const Json* drive = member(entity, "drivetrain");
  const Json* wheels = member(entity, "wheels");
  const Json* impact = member(entity, "ground_impact");
  if (!chassis || chassis->kind != Json::Kind::Object || !drive || drive->kind != Json::Kind::Object ||
      !wheels || wheels->kind != Json::Kind::Array || wheels->items.size() != 4 ||
      !impact || impact->kind != Json::Kind::Object) {
    error = "available replay entity lacks chassis/drivetrain/wheel/contact state";
    return false;
  }
  if (!number_or_null(*chassis, "mass", error) ||
      !vec(*chassis, "position", 3, error) || !vec(*chassis, "velocity", 3, error) ||
      !vec(*chassis, "momentum", 3, error) || !vec(*chassis, "omega", 3, error) ||
      !vec(*chassis, "orientation", 4, error)) return false;
  if (!uint_value(*drive, "dynamics_ticks", ignored, error) ||
      !number_or_null(*drive, "engine_omega", error) ||
      !boolean(*drive, "engine_running", error) || !int_value(*drive, "gear", error) ||
      !number_or_null(*drive, "throttle", error) ||
      !number_or_null(*drive, "brake", error) ||
      !number_or_null(*drive, "handbrake", error) ||
      !number_or_null(*drive, "clutch", error)) return false;
  static constexpr const char* wheel_numbers[] = {
      "length", "speed", "force", "spin", "rotation", "slip_speed", "lateral_speed",
      "tire_longitudinal", "tire_lateral", "contact_impulse"};
  for (const auto& wheel : wheels->items) {
    if (wheel.kind != Json::Kind::Object || !boolean(wheel, "enabled", error) ||
        !boolean(wheel, "contact", error)) return false;
    for (const char* name : wheel_numbers)
      if (!number_or_null(wheel, name, error)) return false;
    if (!vec(wheel, "center", 3, error) || !vec(wheel, "normal", 3, error) ||
        !vec(wheel, "patch", 3, error)) return false;
  }
  if (!vec(*impact, "point", 3, error) || !vec(*impact, "normal", 3, error) ||
      !number_or_null(*impact, "speed", error) ||
      !number_or_null(entity, "chassis_contact_impulse", error)) return false;
  const Json* spawn = member(entity, "spawn_pose");
  if (!spawn || (spawn->kind != Json::Kind::Null && spawn->kind != Json::Kind::Object)) {
    error = "replay entity has an invalid spawn pose";
    return false;
  }
  if (spawn->kind == Json::Kind::Object &&
      (!vec(*spawn, "position", 3, error) || !vec(*spawn, "orientation", 4, error)))
    return false;
  const Json* contacts = member(entity, "contacts");
  if (!contacts || contacts->kind != Json::Kind::Array ||
      contacts->items.size() > physics::VehicleTelemetry::max_contacts ||
      !uint_value(entity, "dropped_contacts", ignored, error)) {
    error = "replay entity has invalid bounded contact telemetry";
    return false;
  }
  for (const auto& contact : contacts->items) {
    if (contact.kind != Json::Kind::Object ||
        !uint_value(contact, "kind", ignored, error) || ignored > 2 ||
        !int_value(contact, "wheel", error)) return false;
    for (const char* name : {"resource", "mesh", "triangle", "material"}) {
      if (!optional_uint(contact, name, error)) return false;
      const Json* id = member(contact, name);
      if (id->kind == Json::Kind::Number && id->number > UINT32_MAX) {
        error = "replay contact identity exceeds uint32";
        return false;
      }
    }
    if (!vec(contact, "point", 3, error) || !vec(contact, "normal", 3, error) ||
        !number_or_null(contact, "penetration", error) ||
        !number_or_null(contact, "impulse", error)) return false;
  }
  return true;
}

struct ParsedReplay {
  std::vector<Json> records;
  const Json* run = nullptr;
  std::vector<const Json*> ticks;
  std::vector<const Json*> inputs;
  ReplayInfo info;
};

bool parse_replay(const std::string& jsonl, ParsedReplay& out, std::string& error) {
  if (jsonl.size() > kMaxReplayBytes) { error = "replay exceeds the 256 MiB input bound"; return false; }
  std::size_t start = 0;
  bool summary = false;
  std::uint64_t previous_tick = 0, previous_frame = 0, previous_sequence = 0;
  bool have_tick = false, have_frame = false, have_sequence = false;
  while (start < jsonl.size()) {
    const std::size_t newline = jsonl.find('\n', start);
    const std::size_t end = newline == std::string::npos ? jsonl.size() : newline;
    std::size_t line_end = end;
    if (line_end > start && jsonl[line_end - 1] == '\r') --line_end;
    if (line_end == start) { error = "replay contains an empty record"; return false; }
    if (line_end - start > max_record_bytes) { error = "replay record exceeds the 1 MiB bound"; return false; }
    if (out.records.size() >= kMaxReplayRecords) { error = "replay has too many records"; return false; }
    if (summary) { error = "replay contains records after its summary"; return false; }
    const std::string line = jsonl.substr(start, line_end - start);
    Json record;
    JsonParser parser(line, error);
    if (!parser.parse(record) || record.kind != Json::Kind::Object) {
      if (error.empty()) error = "replay record is not a JSON object";
      error = "replay line " + std::to_string(out.records.size() + 1) + ": " + error;
      return false;
    }
    std::string type;
    if (!text(record, "type", type, error)) return false;
    if (out.records.empty()) {
      if (type != "run") { error = "replay must start with a run identity record"; return false; }
      std::string schema;
      std::uint64_t version;
      if (!text(record, "schema", schema, error) || schema != "slrr-physics-telemetry" ||
          !uint_value(record, "version", version, error) || version != schema_version ||
          !text(record, "scenario", out.info.identity.scenario, error)) return false;
      const Json* limits = member(record, "limits");
      if (!limits || limits->kind != Json::Kind::Object) { error = "replay run lacks limits"; return false; }
      std::uint64_t ignored;
      for (const char* name : {"max_frames", "max_ticks", "max_input_transitions", "max_file_bytes"})
        if (!uint_value(*limits, name, ignored, error)) return false;
      auto optional_string = [&](const char* name, std::optional<std::string>& target) {
        const Json* value = member(record, name);
        if (!value || (value->kind != Json::Kind::Null && value->kind != Json::Kind::String)) {
          error = std::string("replay identity '") + name + "' must be a string or null";
          return false;
        }
        if (value->kind == Json::Kind::String) {
          if (value->text.size() > 160) { error = "replay identity string is too long"; return false; }
          target = value->text;
        }
        return true;
      };
      if (!optional_string("vehicle", out.info.identity.vehicle) ||
          !optional_string("content_hash", out.info.identity.content_hash) ||
          !optional_string("config_hash", out.info.identity.config_hash)) return false;
    } else if (type == "run") {
      error = "replay contains a duplicate run identity";
      return false;
    } else if (type == "tick") {
      std::uint64_t tick, ignored;
      if (!uint_value(record, "tick", tick, error) || !optional_uint(record, "frame", error) ||
          !finite_number(record, "tick_dt", true, error)) return false;
      if (have_tick && tick <= previous_tick) { error = "replay ticks are not strictly increasing"; return false; }
      have_tick = true; previous_tick = tick;
      const Json* entities = member(record, "entities");
      if (!entities || entities->kind != Json::Kind::Array || entities->items.size() > max_entities_per_tick) {
        error = "replay tick has an invalid or unbounded entity array"; return false;
      }
      std::uint64_t previous_entity = 0;
      for (const auto& entity : entities->items) {
        if (entity.kind != Json::Kind::Object || !validate_entity(entity, error)) return false;
        std::uint64_t id;
        if (!uint_value(entity, "entity", id, error)) return false;
        if (id <= previous_entity) { error = "replay entities are not in stable increasing order"; return false; }
        previous_entity = id;
      }
      (void)ignored;
      ++out.info.ticks;
    } else if (type == "frame") {
      std::uint64_t frame;
      if (!uint_value(record, "frame", frame, error) ||
          !finite_number(record, "render_dt", true, error) ||
          !finite_number(record, "admitted_sim_dt", true, error) ||
          !finite_number(record, "discarded_sim_dt", true, error) ||
          !finite_number(record, "admitted_physics_dt", true, error) ||
          !finite_number(record, "discarded_physics_dt", true, error) ||
          !finite_number(record, "time_warp", true, error)) return false;
      if (have_frame && frame <= previous_frame) { error = "replay frames are not strictly increasing"; return false; }
      have_frame = true; previous_frame = frame;
      ++out.info.frames;
    } else if (type == "input") {
      std::uint64_t timestamp, sequence;
      if (!uint_value(record, "timestamp_us", timestamp, error) ||
          !optional_uint(record, "frame", error) || !optional_uint(record, "tick", error) ||
          !uint_value(record, "sequence", sequence, error) ||
          !int_value(record, "device", error) || !int_value(record, "control", error) ||
          !finite_number(record, "value", false, error)) return false;
      if (sequence == 0 || (have_sequence && sequence <= previous_sequence)) {
        error = "replay input sequence is not strictly increasing"; return false;
      }
      have_sequence = true; previous_sequence = sequence;
      ++out.info.inputs;
    } else if (type == "summary") {
      std::uint64_t ignored;
      for (const char* name : {"frames", "ticks", "inputs", "dropped_frames", "dropped_ticks",
                               "dropped_inputs", "dropped_entities"})
        if (!uint_value(record, name, ignored, error)) return false;
      if (!boolean(record, "truncated", error) ||
          !number_or_null(record, "admitted_sim_dt", error) ||
          !number_or_null(record, "discarded_sim_dt", error) ||
          !number_or_null(record, "admitted_physics_dt", error) ||
          !number_or_null(record, "discarded_physics_dt", error)) return false;
      summary = true;
    } else {
      error = "unknown replay record type: " + type;
      return false;
    }
    out.records.push_back(std::move(record));
    if (newline == std::string::npos) start = jsonl.size();
    else start = newline + 1;
  }
  if (out.records.empty()) { error = "replay is empty"; return false; }
  if (out.records.back().kind != Json::Kind::Object) return false;
  std::string last_type;
  if (!text(out.records.back(), "type", last_type, error)) return false;
  if (last_type != "summary") { error = "replay is missing its final summary"; return false; }
  out.run = &out.records.front();
  for (const auto& record : out.records) {
    const Json* type = member(record, "type");
    if (type && type->kind == Json::Kind::String && type->text == "tick") out.ticks.push_back(&record);
    if (type && type->kind == Json::Kind::String && type->text == "input") out.inputs.push_back(&record);
  }
  return true;
}

std::string scalar_json(const Json& value) {
  switch (value.kind) {
    case Json::Kind::Null: return "null";
    case Json::Kind::Bool: return value.boolean ? "true" : "false";
    case Json::Kind::Number: return value.text;
    case Json::Kind::String: { std::string out; json_string(out, value.text); return out; }
    case Json::Kind::Array: return "\"<array>\"";
    case Json::Kind::Object: return "\"<object>\"";
  }
  return "null";
}

bool exact_path(const std::string& path) {
  static constexpr const char* suffixes[] = {
      ".tick", ".entity", ".resource", ".mesh", ".triangle", ".material",
      ".kind", ".wheel", ".available", ".active", ".dynamics_ticks",
      ".engine_running", ".gear", ".enabled", ".contact", ".sequence", ".device", ".control"};
  for (const char* suffix : suffixes)
    if (path.size() >= std::strlen(suffix) &&
        path.compare(path.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0) return true;
  return false;
}

void mismatch(std::vector<std::string>& rows, const std::string& path,
              const Json& left, const Json& right, double tolerance) {
  if (rows.size() >= 32) return;
  std::string row = "{\"path\":";
  json_string(row, path);
  row += ",\"left\":" + scalar_json(left) + ",\"right\":" + scalar_json(right);
  if (left.kind == Json::Kind::Number && right.kind == Json::Kind::Number) {
    row += ",\"difference\":";
    json_number(row, std::abs(left.number - right.number));
    row += ",\"tolerance\":";
    json_number(row, tolerance);
  }
  row.push_back('}');
  rows.push_back(std::move(row));
}

void compare_node(const Json& left, const Json& right, const std::string& path,
                  double tolerance, std::vector<std::string>& rows) {
  if (rows.size() >= 32) return;
  if (left.kind != right.kind) { mismatch(rows, path, left, right, tolerance); return; }
  switch (left.kind) {
    case Json::Kind::Null: return;
    case Json::Kind::Bool:
      if (left.boolean != right.boolean) mismatch(rows, path, left, right, tolerance);
      return;
    case Json::Kind::String:
      if (left.text != right.text) mismatch(rows, path, left, right, tolerance);
      return;
    case Json::Kind::Number: {
      const bool equal = exact_path(path) ? left.number == right.number
          : std::abs(left.number - right.number) <= tolerance;
      if (!equal) mismatch(rows, path, left, right, tolerance);
      return;
    }
    case Json::Kind::Array: {
      if (left.items.size() != right.items.size()) {
        mismatch(rows, path + ".length", left, right, tolerance);
        return;
      }
      for (std::size_t i = 0; i < left.items.size(); ++i)
        compare_node(left.items[i], right.items[i], path + "[" + std::to_string(i) + "]",
                     tolerance, rows);
      return;
    }
    case Json::Kind::Object: {
      for (const auto& entry : left.members) {
        // The fixed tick is authoritative; its render-frame ownership is not.
        if (entry.first == "frame" && path.rfind("ticks[", 0) == 0) continue;
        const Json* other = member(right, entry.first.c_str());
        if (!other) {
          mismatch(rows, path + "." + entry.first, entry.second, right, tolerance);
          continue;
        }
        compare_node(entry.second, *other, path + "." + entry.first, tolerance, rows);
      }
      return;
    }
  }
}

std::string comparison_json(const ParsedReplay& left, const ParsedReplay& right,
                            const std::vector<std::string>& mismatches, double tolerance) {
  std::string out = "{\"schema\":\"slrr-physics-compare\",\"version\":1,\"equal\":";
  out += mismatches.empty() ? "true" : "false";
  out += ",\"tolerance\":";
  json_number(out, tolerance);
  out += ",\"left_ticks\":" + std::to_string(left.ticks.size());
  out += ",\"right_ticks\":" + std::to_string(right.ticks.size());
  out += ",\"left_inputs\":" + std::to_string(left.inputs.size());
  out += ",\"right_inputs\":" + std::to_string(right.inputs.size());
  out += ",\"mismatches\":[";
  for (std::size_t i = 0; i < mismatches.size(); ++i) {
    if (i) out.push_back(',');
    out += mismatches[i];
  }
  out += "]}";
  return out;
}

bool read_bounded(const std::string& path, std::string& output, std::string& error) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) { error = "cannot open replay input: " + path; return false; }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > kMaxReplayBytes) {
    error = "replay input is empty-positioned or exceeds 256 MiB: " + path;
    return false;
  }
  output.resize(static_cast<std::size_t>(end));
  input.seekg(0);
  if (!output.empty()) input.read(output.data(), static_cast<std::streamsize>(output.size()));
  if (!input) { error = "cannot read replay input: " + path; return false; }
  return true;
}

physics::VehicleDynamics probe_vehicle() {
  physics::VehicleDynamics vehicle;
  vehicle.body.configure(1100, {-0.75, -0.35, -1.7}, {0.75, 0.45, 1.7}, {});
  vehicle.body.position = {0, 0.55, 0};
  for (std::size_t i = 0; i < vehicle.wheels.size(); ++i) {
    auto& wheel = vehicle.wheels[i];
    wheel.enabled = true;
    wheel.center = {i % 2 ? 0.68 : -0.68, -0.05, i < 2 ? -1.15 : 1.15};
    wheel.radius = 0.31;
    wheel.rest = 0.30;
    wheel.minimum = 0.16;
    wheel.maximum = 0.42;
    wheel.spring = 42000;
    wheel.bump_damping = 4200;
    wheel.rebound_damping = 3600;
    vehicle.drive.wheels[i].weight = i >= 2 ? 0.5 : 0;
    vehicle.drive.wheels[i].brake = 1;
    vehicle.drive.wheels[i].bearing = 1;
    vehicle.drive.wheels[i].tyre = {1.4, 15.2, 1.49, 8000, 1, 0.4};
  }
  auto& drive = vehicle.drive;
  drive.ratios = {0, 3.1, 2.0, 1.45, 1.1, 0.9, 0.75, 3.3};
  drive.gears = 6;
  drive.engaged = true;
  drive.inertia = 0.28;
  drive.idle = 80;
  drive.limit = 650;
  drive.brake_scale = drive.handbrake_scale = drive.bearing_scale = 1;
  drive.torque = [](double rpm) { return std::clamp(210.0 - rpm * 0.12, 60.0, 210.0); };
  vehicle.engine_omega = drive.idle;
  vehicle.engine_running = true;
  return vehicle;
}

} // namespace

bool validate_replay(const std::string& jsonl, ReplayInfo* info,
                     std::string& error) noexcept {
  try {
    ParsedReplay parsed;
    if (!parse_replay(jsonl, parsed, error)) return false;
    if (info) *info = parsed.info;
    return true;
  } catch (const std::exception& e) {
    error = std::string("cannot parse replay: ") + e.what();
    return false;
  } catch (...) {
    error = "cannot parse replay";
    return false;
  }
}

ComparisonResult compare_replay(const std::string& left_text,
                                const std::string& right_text,
                                double tolerance) noexcept {
  ComparisonResult result;
  try {
    if (!std::isfinite(tolerance) || tolerance < 0 || tolerance > 1) {
      result.error = "comparison tolerance must be finite and between 0 and 1";
      return result;
    }
    ParsedReplay left, right;
    if (!parse_replay(left_text, left, result.error)) { result.error = "left replay: " + result.error; return result; }
    if (!parse_replay(right_text, right, result.error)) { result.error = "right replay: " + result.error; return result; }
    std::vector<std::string> mismatches;
    for (const char* name : {"scenario", "vehicle", "content_hash", "config_hash"}) {
      const Json* a = member(*left.run, name);
      const Json* b = member(*right.run, name);
      if (a && b) compare_node(*a, *b, std::string("run.") + name, tolerance, mismatches);
    }
    if (left.ticks.size() != right.ticks.size()) {
      Json a, b;
      a.kind = b.kind = Json::Kind::Number;
      a.number = static_cast<double>(left.ticks.size()); a.text = std::to_string(left.ticks.size());
      b.number = static_cast<double>(right.ticks.size()); b.text = std::to_string(right.ticks.size());
      mismatch(mismatches, "ticks.length", a, b, tolerance);
    }
    const std::size_t ticks = std::min(left.ticks.size(), right.ticks.size());
    for (std::size_t i = 0; i < ticks; ++i)
      compare_node(*left.ticks[i], *right.ticks[i], "ticks[" + std::to_string(i) + "]",
                   tolerance, mismatches);
    if (left.inputs.size() != right.inputs.size()) {
      Json a, b;
      a.kind = b.kind = Json::Kind::Number;
      a.number = static_cast<double>(left.inputs.size()); a.text = std::to_string(left.inputs.size());
      b.number = static_cast<double>(right.inputs.size()); b.text = std::to_string(right.inputs.size());
      mismatch(mismatches, "inputs.length", a, b, tolerance);
    }
    const std::size_t inputs = std::min(left.inputs.size(), right.inputs.size());
    for (std::size_t i = 0; i < inputs; ++i) {
      for (const char* name : {"sequence", "device", "control", "value"}) {
        const Json* a = member(*left.inputs[i], name);
        const Json* b = member(*right.inputs[i], name);
        if (a && b) compare_node(*a, *b, "inputs[" + std::to_string(i) + "]." + name,
                                 tolerance, mismatches);
      }
    }
    result.equal = mismatches.empty();
    result.report_json = comparison_json(left, right, mismatches, tolerance);
    return result;
  } catch (const std::exception& e) {
    result.equal = false;
    result.error = std::string("cannot compare replays: ") + e.what();
    result.report_json.clear();
    return result;
  } catch (...) {
    result.equal = false;
    result.error = "cannot compare replays";
    result.report_json.clear();
    return result;
  }
}

ComparisonResult compare_replay_files(const std::string& left_path,
                                      const std::string& right_path,
                                      double tolerance) noexcept {
  ComparisonResult result;
  try {
    std::string left, right;
    if (!read_bounded(left_path, left, result.error) ||
        !read_bounded(right_path, right, result.error)) return result;
    return compare_replay(left, right, tolerance);
  } catch (const std::exception& e) {
    result.error = std::string("cannot read replay: ") + e.what();
    return result;
  } catch (...) {
    result.error = "cannot read replay";
    return result;
  }
}

bool write_comparison_report(const std::string& path, const std::string& json,
                             std::string& error) noexcept {
  try {
    if (path.empty()) { error = "comparison report requires an explicit path"; return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { error = "cannot open comparison report: " + path; return false; }
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.put('\n');
    output.close();
    if (!output) { error = "cannot write comparison report: " + path; return false; }
    return true;
  } catch (const std::exception& e) {
    error = std::string("cannot write comparison report: ") + e.what();
    return false;
  } catch (...) {
    error = "cannot write comparison report";
    return false;
  }
}

std::string deterministic_scenario(int partition) noexcept {
  try {
    Identity identity;
    identity.scenario = "fixed-partition-single-vehicle";
    identity.vehicle = "probe:rigid-four-wheel";
    identity.config_hash = fnv1a64(std::string("slrr-stage4-probe-v1"));
    Limits limits;
    std::string error;
    Session session(identity, limits, error);
    if (!session.good()) return {};
    auto vehicle = probe_vehicle();
    const auto flat = [](double, double, double, physics::GroundHit& ground) {
      ground.height = 0;
      ground.normal = {0, 1, 0};
      return true;
    };
    constexpr double host_tick = 1.0 / physics::VehicleDynamics::update_frequency;
    constexpr double max_admitted = 0.1;
    constexpr unsigned max_updates = 4;
    double elapsed = 0;
    std::uint64_t tick = 0;
    double previous_throttle = -1;
    for (std::uint64_t frame = 0; frame < 120; ++frame) {
      double render_dt;
      if (partition == 0) render_dt = 1.0 / 120.0;
      else if (partition == 1) {
        constexpr double pattern[] = {1.0 / 240.0, 1.0 / 120.0, 1.0 / 60.0, 1.0 / 240.0};
        render_dt = pattern[frame % 4];
      } else {
        // Preserve one second of requested wall time while placing 0.2 s in the
        // first frame. Both production clamps intentionally discard the excess.
        render_dt = frame == 0 ? 0.2
            : (1.0 / 120.0 - (0.2 - 1.0 / 120.0) / 119.0);
      }
      session.begin_frame(frame, render_dt, 1.0);
      const double admitted = std::clamp(render_dt, 0.0, max_admitted);
      session.set_simulation_time(admitted, std::max(0.0, render_dt - admitted));
      const double physics_admitted = std::min(admitted, host_tick * max_updates);
      session.note_physics_time(physics_admitted, admitted - physics_admitted);
      elapsed += physics_admitted;
      constexpr double tolerance = host_tick * 1e-10;
      while (elapsed + tolerance >= host_tick) {
        const double throttle = tick < 20 ? 0.25 : tick < 40 ? 0.55 : 0.10;
        vehicle.drive.throttle = throttle;
        if (throttle != previous_throttle) {
          InputTransition input;
          input.timestamp_us = tick * 16667;
          input.frame = frame;
          input.tick = tick;
          input.device = 99;
          input.control = 1;
          input.value = throttle;
          session.record_input(input);
          previous_throttle = throttle;
        }
        vehicle.advance(host_tick, flat);
        EntityState entity;
        entity.entity = 1;
        entity.resource = 0x10001;
        entity.active = entity.available = true;
        entity.spawn_pose = SpawnPose{{0, 0.55, 0}, {}};
        entity.vehicle = vehicle.telemetry_snapshot();
        session.record_tick(tick, host_tick, {entity});
        ++tick;
        elapsed = std::max(0.0, elapsed - host_tick);
      }
      session.end_frame();
    }
    if (!session.shutdown(error)) return {};
    return session.output();
  } catch (...) {
    return {};
  }
}

bool run_deterministic_probe(const std::string& output_path, ProbeResult& result,
                             std::string& error, double tolerance) noexcept {
  try {
    const std::string fixed = deterministic_scenario(0);
    const std::string bursty = deterministic_scenario(1);
    const std::string stall = deterministic_scenario(2);
    if (fixed.empty() || bursty.empty() || stall.empty()) {
      error = "deterministic physics scenario failed to serialize";
      return false;
    }
    ReplayInfo info;
    if (!validate_replay(fixed, &info, error)) return false;
    result.partition_ticks = info.ticks;
    auto partition = compare_replay(fixed, bursty, tolerance);
    if (!partition.error.empty()) { error = partition.error; return false; }
    result.partition_equal = partition.equal;
    if (!result.partition_equal) {
      error = "equal fixed-tick work diverged across render partitions: " + partition.report_json;
      return false;
    }
    auto lag = compare_replay(fixed, stall, tolerance);
    if (!lag.error.empty()) { error = lag.error; return false; }
    result.lag_equal = lag.equal;
    if (!lag.equal && lag.report_json.find("\"path\"") != std::string::npos) {
      const auto path = lag.report_json.find("\"path\"");
      const auto end = lag.report_json.find('}', path);
      const auto begin = path > 0 ? path - 1 : path;
      result.first_lag_mismatch = lag.report_json.substr(
          begin, end == std::string::npos ? end : end - begin + 1);
    }
    if (result.lag_equal) {
      error = "stall scenario unexpectedly matched; the lag-policy negative control is broken";
      return false;
    }
    if (!output_path.empty()) {
      std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
      if (!output) { error = "cannot open physics telemetry output: " + output_path; return false; }
      output.write(fixed.data(), static_cast<std::streamsize>(fixed.size()));
      output.close();
      if (!output) { error = "cannot write physics telemetry output: " + output_path; return false; }
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("deterministic physics probe failed: ") + e.what();
    return false;
  } catch (...) {
    error = "deterministic physics probe failed";
    return false;
  }
}

} // namespace slrr::physics_telemetry
