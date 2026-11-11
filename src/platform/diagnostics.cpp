#include "slrr/diagnostics.hpp"

#if SLRR_ENABLE_DIAGNOSTICS

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>

namespace slrr::diagnostics {
namespace {

constexpr std::size_t kMaximumStringBytes = 512;
constexpr std::size_t kMaximumValues = 16;
constexpr std::size_t kMaximumRingCapacity = 65536;

std::atomic_bool g_enabled{false};
std::mutex g_mutex;
std::ofstream g_output;
std::deque<Event> g_ring;
std::size_t g_ring_capacity = 1024;
std::size_t g_dropped = 0;
thread_local Context g_context;

std::string bounded(std::string_view text) {
  return std::string(text.substr(0, kMaximumStringBytes));
}

const char* category_name(Category category) {
  switch (category) {
    case Category::Native: return "native";
    case Category::Section: return "section";
    case Category::Gc: return "gc";
    case Category::Fiber: return "fiber";
    case Category::Runtime: return "runtime";
  }
  return "runtime";
}

void append_json_string(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    switch (byte) {
      case '"': out += "\\\""; ++i; continue;
      case '\\': out += "\\\\"; ++i; continue;
      case '\b': out += "\\b"; ++i; continue;
      case '\f': out += "\\f"; ++i; continue;
      case '\n': out += "\\n"; ++i; continue;
      case '\r': out += "\\r"; ++i; continue;
      case '\t': out += "\\t"; ++i; continue;
      default: break;
    }
    if (byte < 0x20) {
      out += "\\u00";
      out.push_back(kHex[(byte >> 4) & 0xf]);
      out.push_back(kHex[byte & 0xf]);
      ++i;
      continue;
    }
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
      ++i;
      continue;
    }

    // Preserve valid UTF-8. Replace malformed/truncated sequences so every
    // JSONL record remains valid even when an exception carries arbitrary bytes.
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (byte >= 0xc2 && byte <= 0xdf) {
      length = 2;
      codepoint = byte & 0x1f;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      length = 3;
      codepoint = byte & 0x0f;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      length = 4;
      codepoint = byte & 0x07;
    }
    bool valid = length != 0 && i + length <= text.size();
    for (std::size_t j = 1; valid && j < length; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if ((continuation & 0xc0) != 0x80) {
        valid = false;
      } else {
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
    }
    if (valid && ((length == 3 && codepoint < 0x800) ||
                  (length == 4 && codepoint < 0x10000) ||
                  (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                  codepoint > 0x10ffff)) {
      valid = false;
    }
    if (valid) {
      out.append(text, i, length);
      i += length;
    } else {
      out += "\\ufffd";
      ++i;
    }
  }
  out.push_back('"');
}

void append_member_name(std::string& out, const char* name) {
  if (out.back() != '{') out.push_back(',');
  append_json_string(out, name);
  out.push_back(':');
}

void append_optional(std::string& out,
                     const std::optional<std::uint64_t>& value) {
  if (value) {
    out += std::to_string(*value);
  } else {
    out += "null";
  }
}

void append_object(std::string& out, const void* object) {
  if (!object) {
    out += "null";
    return;
  }
  char address[2 + sizeof(std::uintptr_t) * 2] = {'0', 'x'};
  const auto value = reinterpret_cast<std::uintptr_t>(object);
  const auto converted = std::to_chars(address + 2, std::end(address), value, 16);
  out.push_back('"');
  out.append(address, converted.ptr);
  out.push_back('"');
}

std::string serialize(const Event& event) {
  std::string out;
  out.reserve(512);
  out.push_back('{');
  append_member_name(out, "timestamp_us");
  out += std::to_string(event.timestamp_us);
  append_member_name(out, "frame");
  append_optional(out, event.frame);
  append_member_name(out, "tick");
  append_optional(out, event.tick);
  append_member_name(out, "section");
  append_json_string(out, event.section);
  append_member_name(out, "fiber_id");
  append_optional(out, event.fiber_id);
  append_member_name(out, "category");
  append_json_string(out, category_name(event.category));
  append_member_name(out, "event");
  append_json_string(out, event.name);
  append_member_name(out, "object");
  append_object(out, event.object);
  append_member_name(out, "resource");
  append_json_string(out, event.resource);
  append_member_name(out, "class");
  append_json_string(out, event.class_name);
  append_member_name(out, "method");
  append_json_string(out, event.method);
  append_member_name(out, "signature");
  append_json_string(out, event.signature);
  append_member_name(out, "detail");
  append_json_string(out, event.detail);
  append_member_name(out, "duration_ns");
  out += std::to_string(event.duration_ns);
  append_member_name(out, "result");
  append_json_string(out, event.result);
  append_member_name(out, "error");
  append_json_string(out, event.error);
  append_member_name(out, "values");
  out.push_back('{');
  bool first = true;
  for (const auto& value : event.values) {
    if (!first) out.push_back(',');
    first = false;
    append_json_string(out, value.name);
    out.push_back(':');
    out += std::to_string(value.value);
  }
  out.push_back('}');
  out.push_back('}');
  return out;
}

void bound_event(Event& event) {
  event.name = bounded(event.name);
  event.section = bounded(event.section);
  event.resource = bounded(event.resource);
  event.class_name = bounded(event.class_name);
  event.method = bounded(event.method);
  event.signature = bounded(event.signature);
  event.detail = bounded(event.detail);
  event.result = bounded(event.result);
  event.error = bounded(event.error);
  if (event.values.size() > kMaximumValues) event.values.resize(kMaximumValues);
  for (auto& value : event.values) value.name = bounded(value.name);
}

}  // namespace

bool start(const std::string& path, std::string& error,
           std::size_t ring_capacity) noexcept {
  try {
    std::lock_guard lock(g_mutex);
    g_enabled.store(false, std::memory_order_release);
    if (g_output.is_open()) {
      g_output.flush();
      g_output.close();
    }
    g_output.clear();
    g_ring.clear();
    g_dropped = 0;
    error.clear();
    g_output.open(path, std::ios::binary | std::ios::trunc);
    if (!g_output.is_open()) {
      error = "Unable to open trace file: " + path;
      return false;
    }
    g_ring_capacity = std::max<std::size_t>(
        1, std::min(ring_capacity, kMaximumRingCapacity));
    g_enabled.store(true, std::memory_order_release);
    return true;
  } catch (const std::exception& exception) {
    g_enabled.store(false, std::memory_order_release);
    try {
      error = exception.what();
    } catch (...) {
    }
    return false;
  } catch (...) {
    g_enabled.store(false, std::memory_order_release);
    return false;
  }
}

void flush() noexcept {
  try {
    std::lock_guard lock(g_mutex);
    if (g_output.is_open()) g_output.flush();
  } catch (...) {
  }
}

void shutdown() noexcept {
  try {
    std::lock_guard lock(g_mutex);
    g_enabled.store(false, std::memory_order_release);
    if (g_output.is_open()) {
      g_output.flush();
      g_output.close();
    }
    g_output.clear();
    g_ring.clear();
    g_dropped = 0;
  } catch (...) {
    g_enabled.store(false, std::memory_order_release);
  }
  clear_context();
}

bool enabled() noexcept {
  return g_enabled.load(std::memory_order_acquire);
}

void trace(Event event) noexcept {
  if (!enabled()) return;
  try {
    if (!event.frame) event.frame = g_context.frame;
    if (!event.tick) event.tick = g_context.tick;
    if (event.section.empty()) event.section = g_context.section;
    if (!event.fiber_id) event.fiber_id = g_context.fiber_id;
    event.timestamp_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    bound_event(event);

    std::lock_guard lock(g_mutex);
    if (!enabled()) return;
    if (g_ring.size() == g_ring_capacity) {
      g_ring.pop_front();
      ++g_dropped;
    }
    g_ring.push_back(event);
    if (g_output.is_open()) g_output << serialize(event) << '\n';
  } catch (...) {
    // Diagnostics must never alter script, native, or callback behavior.
  }
}

std::vector<Event> snapshot() {
  std::lock_guard lock(g_mutex);
  return {g_ring.begin(), g_ring.end()};
}

std::size_t dropped_events() noexcept {
  try {
    std::lock_guard lock(g_mutex);
    return g_dropped;
  } catch (...) {
    return 0;
  }
}

void set_context(Context context) {
  if (enabled()) g_context = std::move(context);
}

Context current_context() {
  return g_context;
}

void set_frame(std::uint64_t frame) {
  if (enabled()) g_context.frame = frame;
}

void set_tick(std::uint64_t tick) {
  if (enabled()) g_context.tick = tick;
}

void set_section(std::string section) {
  if (enabled()) g_context.section = std::move(section);
}

void set_fiber(std::uint64_t fiber_id) {
  if (enabled()) g_context.fiber_id = fiber_id;
}

void clear_fiber() {
  g_context.fiber_id.reset();
}

std::optional<std::uint64_t> current_fiber_id() noexcept {
  return g_context.fiber_id;
}

void clear_context() noexcept {
  g_context = Context{};
}

}  // namespace slrr::diagnostics

#endif
