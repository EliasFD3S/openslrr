#include "slrr/metrics.hpp"

#include "slrr/diagnostics.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace slrr::metrics {
namespace {

constexpr std::size_t kMaximumNameBytes = 96;
constexpr std::size_t kMaximumValues = 128;
constexpr std::size_t kMaximumProviders = 32;
constexpr std::size_t kMaximumSectionSnapshots = 64;

struct ProviderEntry {
  std::uint64_t id = 0;
  Provider provider = nullptr;
  void* context = nullptr;
};

struct State {
  std::mutex mutex;
  std::map<std::string, Value, std::less<>> values;
  std::vector<ProviderEntry> providers;
  std::uint64_t next_provider_id = 1;
  std::deque<Snapshot> sections;
  std::string last_section;
};

State& state() {
  static State instance;
  return instance;
}

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > kMaximumNameBytes) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

const char* kind_name(Kind kind) {
  return kind == Kind::Counter ? "counter" : "gauge";
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

void append_optional(std::string& out,
                     const std::optional<std::uint64_t>& value) {
  if (value)
    out += std::to_string(*value);
  else
    out += "null";
}

void append_snapshot(std::string& out, const Snapshot& current) {
  out += "{\"frame\":";
  append_optional(out, current.frame);
  out += ",\"tick\":";
  append_optional(out, current.tick);
  out += ",\"section\":";
  append_json_string(out, current.section);
  out += ",\"values\":{";
  bool first = true;
  for (const auto& value : current.values) {
    if (!valid_name(value.name)) continue;
    if (!first) out.push_back(',');
    first = false;
    append_json_string(out, value.name);
    out.push_back(':');
    out += std::to_string(value.value);
  }
  out += "},\"kinds\":{";
  first = true;
  for (const auto& value : current.values) {
    if (!valid_name(value.name)) continue;
    if (!first) out.push_back(',');
    first = false;
    append_json_string(out, value.name);
    out.push_back(':');
    append_json_string(out, kind_name(value.kind));
  }
  out += "}}";
}

void append_provider_value(Snapshot& output, const Value& value) {
  if (!valid_name(value.name) || output.values.size() >= kMaximumValues) return;
  const auto duplicate = std::find_if(
      output.values.begin(), output.values.end(), [&](const Value& existing) {
        return existing.name == value.name;
      });
  if (duplicate == output.values.end()) output.values.push_back(value);
}

}  // namespace

bool register_metric(std::string_view name, Kind kind) noexcept {
  if (!valid_name(name)) return false;
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto existing = s.values.find(name);
    if (existing != s.values.end()) return existing->second.kind == kind;
    if (s.values.size() >= kMaximumValues) return false;
    s.values.emplace(std::string(name), Value{std::string(name), 0, kind});
    return true;
  } catch (...) {
    return false;
  }
}

bool set(std::string_view name, std::int64_t value) noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.values.find(name);
    if (it == s.values.end()) return false;
    it->second.value = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool add(std::string_view name, std::int64_t delta) noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.values.find(name);
    if (it == s.values.end() || it->second.kind != Kind::Counter) return false;
    auto& value = it->second.value;
    if ((delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta)) {
      value = delta > 0 ? std::numeric_limits<std::int64_t>::max()
                        : std::numeric_limits<std::int64_t>::min();
    } else {
      value += delta;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::uint64_t register_provider(Provider provider, void* context) noexcept {
  if (!provider) return 0;
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.providers.size() >= kMaximumProviders) return 0;
    const auto id = s.next_provider_id++;
    if (s.next_provider_id == 0) s.next_provider_id = 1;
    s.providers.push_back({id, provider, context});
    return id;
  } catch (...) {
    return 0;
  }
}

void unregister_provider(std::uint64_t id) noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.providers,
                  [&](const ProviderEntry& entry) { return entry.id == id; });
  } catch (...) {
  }
}

Snapshot snapshot() noexcept {
  Snapshot output;
  try {
    const auto context = diagnostics::current_context();
    output.frame = context.frame;
    output.tick = context.tick;
    output.section = context.section;

    std::vector<ProviderEntry> providers;
    auto& s = state();
    {
      std::lock_guard lock(s.mutex);
      output.values.reserve(s.values.size());
      for (const auto& [name, value] : s.values) output.values.push_back(value);
      providers = s.providers;
    }
    for (const auto& entry : providers) {
      try {
        std::vector<Value> provided;
        entry.provider(entry.context, provided);
        for (const auto& value : provided) append_provider_value(output, value);
      } catch (...) {
        // Instrumentation cannot cross a native callback boundary with an
        // exception or change the subsystem being measured.
      }
    }
  } catch (...) {
  }
  return output;
}

void capture_section_transition() noexcept {
  try {
    auto current = snapshot();
    if (current.section.empty()) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (current.section == s.last_section) return;
    s.last_section = current.section;
    if (s.sections.size() == kMaximumSectionSnapshots) s.sections.pop_front();
    s.sections.push_back(std::move(current));
  } catch (...) {
  }
}

std::vector<Snapshot> section_snapshots() noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return {s.sections.begin(), s.sections.end()};
  } catch (...) {
    return {};
  }
}

void reset() noexcept {
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.values.clear();
    s.sections.clear();
    s.last_section.clear();
  } catch (...) {
  }
}

std::string to_json(const Snapshot& current) {
  std::string out;
  append_snapshot(out, current);
  return out;
}

std::string snapshot_json() {
  std::string out = "{\"version\":1,\"snapshot\":";
  append_snapshot(out, snapshot());
  out += ",\"section_snapshots\":[";
  const auto sections = section_snapshots();
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (i != 0) out.push_back(',');
    append_snapshot(out, sections[i]);
  }
  out += "]}";
  return out;
}

bool write_snapshot(const std::string& path, std::string& error) noexcept {
  try {
    error.clear();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      error = "Unable to open metrics file: " + path;
      return false;
    }
    output << snapshot_json() << '\n';
    output.flush();
    if (!output) {
      error = "Unable to write metrics file: " + path;
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    try {
      error = exception.what();
    } catch (...) {
    }
    return false;
  } catch (...) {
    return false;
  }
}

}  // namespace slrr::metrics
