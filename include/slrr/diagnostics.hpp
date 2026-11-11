#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef SLRR_ENABLE_DIAGNOSTICS
#define SLRR_ENABLE_DIAGNOSTICS 1
#endif

namespace slrr::diagnostics {

enum class Category {
  Runtime,
  Native,
  Section,
  Gc,
  Fiber,
  File,
};

// Context is thread-local. In particular, a media/worker callback never
// inherits or updates the host thread's VM/fiber context.
struct Context {
  std::optional<std::uint64_t> frame;
  std::optional<std::uint64_t> tick;
  std::string section;
  std::optional<std::uint64_t> fiber_id;
};

struct Value {
  std::string name;
  std::int64_t value = 0;
};

struct Event {
  Category category = Category::Runtime;
  std::string name;
  std::optional<std::uint64_t> frame;
  std::optional<std::uint64_t> tick;
  std::string section;
  std::optional<std::uint64_t> fiber_id;
  const void* object = nullptr;
  std::string resource;
  std::string class_name;
  std::string method;
  std::string signature;
  std::string detail;
  std::uint64_t duration_ns = 0;
  std::string result;
  std::string error;
  std::vector<Value> values;
  std::uint64_t timestamp_us = 0;
};

#if SLRR_ENABLE_DIAGNOSTICS

// Starting tracing is always explicit: there is no default output file.
bool start(const std::string& path, std::string& error,
           std::size_t ring_capacity = 1024) noexcept;
void flush() noexcept;
void shutdown() noexcept;
bool enabled() noexcept;

// Never throws across a native/media callback boundary. String lengths,
// numeric metadata, and ring capacity are bounded by the implementation.
void trace(Event event) noexcept;
std::vector<Event> snapshot();
std::size_t dropped_events() noexcept;

void set_context(Context context);
Context current_context();
void set_frame(std::uint64_t frame);
void set_tick(std::uint64_t tick);
void set_section(std::string section);
void set_fiber(std::uint64_t fiber_id);
void clear_fiber();
std::optional<std::uint64_t> current_fiber_id() noexcept;
void clear_context() noexcept;

#else

inline bool start(const std::string&, std::string& error,
                  std::size_t = 1024) noexcept {
  error = "Diagnostics were disabled at compile time";
  return false;
}
inline void flush() noexcept {}
inline void shutdown() noexcept {}
inline constexpr bool enabled() noexcept { return false; }
inline void trace(Event) noexcept {}
inline std::vector<Event> snapshot() { return {}; }
inline std::size_t dropped_events() noexcept { return 0; }
inline void set_context(Context) {}
inline Context current_context() { return {}; }
inline void set_frame(std::uint64_t) {}
inline void set_tick(std::uint64_t) {}
inline void set_section(std::string) {}
inline void set_fiber(std::uint64_t) {}
inline void clear_fiber() {}
inline std::optional<std::uint64_t> current_fiber_id() noexcept {
  return std::nullopt;
}
inline void clear_context() noexcept {}

#endif

}  // namespace slrr::diagnostics
