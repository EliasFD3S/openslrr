#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slrr::metrics {

enum class Kind {
  Gauge,
  Counter,
};

struct Value {
  std::string name;
  std::int64_t value = 0;
  Kind kind = Kind::Gauge;
};

struct Snapshot {
  std::optional<std::uint64_t> frame;
  std::optional<std::uint64_t> tick;
  std::string section;
  std::vector<Value> values;
};

// Explicit metrics are registered before use. Names contain at most 96 ASCII
// alphanumeric, '.', '_' or '-' bytes. The registry retains at most 128 values.
bool register_metric(std::string_view name, Kind kind) noexcept;
bool set(std::string_view name, std::int64_t value) noexcept;
// Counters saturate at the signed 64-bit limits instead of overflowing.
bool add(std::string_view name, std::int64_t delta) noexcept;

// Providers add live registry-backed values to every snapshot. The context is
// never serialized. Registration is bounded and returns zero on failure.
using Provider = void (*)(void* context, std::vector<Value>& values);
std::uint64_t register_provider(Provider provider, void* context = nullptr) noexcept;
void unregister_provider(std::uint64_t id) noexcept;

// Snapshot uses the current thread's diagnostics context. Providers run on the
// calling thread; VM/fiber snapshots must therefore remain on the host thread.
Snapshot snapshot() noexcept;

// Record a context transition in a bounded 64-entry history. Repeated identical
// section names are coalesced. This is independent from the trace sink.
void capture_section_transition() noexcept;
std::vector<Snapshot> section_snapshots() noexcept;

// Clear explicit values and transition history without unregistering providers.
// Primarily useful for isolated probes and tests.
void reset() noexcept;

// A single snapshot has frame/tick/section, values, and kinds. snapshot_json()
// also includes section_snapshots for before/after cycle comparisons.
std::string to_json(const Snapshot& snapshot);
std::string snapshot_json();
bool write_snapshot(const std::string& path, std::string& error) noexcept;

}  // namespace slrr::metrics
