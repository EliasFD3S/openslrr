#include "slrr/scene_ambience.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scene_sun.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <unordered_map>

namespace slrr {

namespace {

constexpr int32_t kDay = 24 * 3600;
constexpr size_t kMaxRandoms = 16;  // 0x465418: cmp [obj+0xbc4], 0x10
constexpr float kStartRange = 250.f;
constexpr float kNearSlack = 16.f;

std::string_view trim_left(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  return text;
}

// Reads up to `count` numbers in order and stops at the first token that is
// not one, as the original sscanf does (`300;comment` keeps 300).
size_t read_numbers(std::string_view text, double* out, size_t count) {
  std::string buffer(text.substr(0, std::min<size_t>(text.size(), 256)));
  const char* p = buffer.c_str();
  size_t n = 0;
  for (; n < count; ++n) {
    while (*p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    if (n == 0) {
      // The sound id is written as 0x... in every stock CFG.
      const unsigned long id = std::strtoul(p, &end, 0);
      if (end == p) break;
      out[n] = static_cast<double>(id);
    } else {
      out[n] = std::strtod(p, &end);
      if (end == p || !std::isfinite(out[n])) break;
    }
    p = end;
  }
  return n;
}

int32_t hour_seconds(double hours) {
  // The original truncates hours * 3600 to an int (0x5d6750, chop mode).
  const double seconds = hours * 3600.0;
  if (!std::isfinite(seconds)) return 0;
  return static_cast<int32_t>(std::clamp(seconds, -1e9, 1e9));
}

struct Sound {
  bool ok = false;
  std::string path;
  float volume = 1.f;
  float min_distance = 1.f;
  float max_distance = 1e9f;
};

struct LoopState {
  AmbienceConfig::Loop config;
  Sound sound;
  int32_t voice = 0;
  // A refused start (no free voice, unreadable WAV) waits before reloading.
  double retry_at = 0.0;
};

struct RandomState {
  AmbienceConfig::Random config;
  Sound sound;
};

struct Emitter {
  std::array<float, 3> local{};
  std::optional<LoopState> loop;
  std::vector<RandomState> randoms;
  bool scheduled = false;
  double next_time = 0.0;
  size_t next_index = 0;
};

struct ShellAmbience {
  void* shell = nullptr;
  uint32_t render_id = 0;
  std::vector<Emitter> emitters;
};

std::vector<ShellAmbience> g_shells;
std::unordered_map<uint32_t, Sound> g_sounds;
std::minstd_rand g_rng(0x534c5252u);
double g_clock = 0.0;
bool g_paused = false;

bool log_randoms() {
  static const bool enabled = [] {
    const char* value = std::getenv("SLRR_AMBIENCE_LOG");
    return value && *value && *value != '0';
  }();
  return enabled;
}

// rand() & 0x7fff, as the original draws both delays and entries.
int32_t rand15() { return static_cast<int32_t>(g_rng() & 0x7fffu); }
float unit_random() { return static_cast<float>(rand15()) * (1.f / 32768.f); }
size_t random_index(size_t count) {
  return static_cast<size_t>((static_cast<int64_t>(rand15()) * static_cast<int64_t>(count)) >> 15);
}

std::string config_text(const RpkIndex& rpk, uint32_t type) {
  const auto bytes = rpk.read_payload(type);
  if (bytes.size() < 10 || std::memcmp(bytes.data(), "ICFG", 4) != 0) return {};
  const auto separator = std::find(bytes.begin() + 8, bytes.end(), 0);
  if (separator == bytes.end()) return {};
  const auto offset = static_cast<size_t>(separator - bytes.begin()) + 1;
  return std::string(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
}

float positive_prop(const RpkIndex& rpk, uint32_t id, const char* key, float fallback) {
  const auto text = rpk.payload_prop(id, key);
  if (text.empty()) return fallback;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  return end != text.c_str() && std::isfinite(value) && value >= 0.f ? value : fallback;
}

Sound load_sound(const RpkIndex& rpk, int32_t owner_pack, uint32_t cfg_id) {
  const uint32_t id = rpk.resolve_embedded_id(owner_pack, cfg_id);
  auto [found, added] = g_sounds.try_emplace(id);
  if (!added) return found->second;
  Sound& sound = found->second;
  const RpkEntry* entry = rpk.resolve(id);
  if (!entry || entry->typeof_ != 6) return sound;
  std::string source = rpk.payload_prop(id, "sourcefile");
  if (source.empty()) return sound;
  std::replace(source.begin(), source.end(), '\\', '/');
  sound.path = (std::filesystem::path(rpk.game_root()) / source).string();
  sound.volume = std::min(1.f, positive_prop(rpk, id, "volume", 1.f));
  sound.min_distance = std::max(.01f, positive_prop(rpk, id, "mindist", 1.f));
  sound.max_distance = std::max(sound.min_distance, positive_prop(rpk, id, "maxdist", 1e9f));
  sound.ok = true;
  return sound;
}

void stop_loop(LoopState& loop) {
  if (!loop.voice) return;
  host_sfx_stop(loop.voice);
  loop.voice = 0;
  std::printf("[ambience] loop stop %s\n", loop.sound.path.c_str());
}

void stop_shell(ShellAmbience& shell) {
  for (auto& emitter : shell.emitters)
    if (emitter.loop) stop_loop(*emitter.loop);
}

void update_loop(LoopState& loop, int32_t now, float distance_squared) {
  const auto& c = loop.config;
  const float gain = scene_ambience_gain(loop.sound.volume, loop.sound.min_distance,
                                         loop.sound.max_distance, std::sqrt(distance_squared));
  const bool wanted = scene_ambience_in_window(c.from, c.to, now) &&
                      scene_ambience_audible(distance_squared, c.radius) && gain > 1e-3f;
  if (!wanted) {
    stop_loop(loop);
    return;
  }
  if (loop.voice && host_sfx_is_playing(loop.voice)) {
    host_sfx_update_loop(loop.voice, 1.f, gain);
    return;
  }
  if (g_clock < loop.retry_at) return;
  loop.voice = host_sfx_play_file(loop.sound.path, 1.f, gain, true);
  if (!loop.voice) {
    loop.retry_at = g_clock + 1.0;
    return;
  }
  std::printf("[ambience] loop start %s gain=%.3f dist=%.1f\n", loop.sound.path.c_str(), gain,
              std::sqrt(distance_squared));
}

// One object timer (0x4660f7 schedules it, 0x467983 handles it): the entry
// that fires is drawn at random, and its own delay schedules the next one.
void update_randoms(Emitter& emitter, int32_t now, float distance_squared) {
  if (emitter.randoms.empty()) return;
  const size_t count = emitter.randoms.size();
  if (!emitter.scheduled) {
    const auto& first = emitter.randoms.front().config;
    emitter.next_time = g_clock + std::max(0.f, first.min_delay + unit_random() * first.delay_range);
    emitter.next_index = random_index(count);
    emitter.scheduled = true;
  }
  if (g_clock < emitter.next_time) return;
  RandomState& entry = emitter.randoms[std::min(emitter.next_index, count - 1)];
  const auto& c = entry.config;
  double next = g_clock + c.min_delay + unit_random() * c.delay_range;
  emitter.next_index = random_index(count);
  if (scene_ambience_in_window(c.from, c.to, now)) {
    const float gain = scene_ambience_gain(entry.sound.volume, entry.sound.min_distance,
                                           entry.sound.max_distance, std::sqrt(distance_squared));
    if (scene_ambience_audible(distance_squared, c.radius) && gain > 1e-3f &&
        host_sfx_play_file(entry.sound.path, 1.f, gain, false)) {
      if (log_randoms())
        std::printf("[ambience] random %s gain=%.3f dist=%.1f\n", entry.sound.path.c_str(),
                    gain, std::sqrt(distance_squared));
    }
  } else {
    // Outside its hours the object retries 30-90 s later.
    next = g_clock + 30.0 + unit_random() * 60.0;
  }
  // A zero delay would fire every frame; the original timer needs a tick too.
  emitter.next_time = std::max(next, g_clock + 0.05);
}

}  // namespace

AmbienceConfig scene_ambience_parse(std::string_view cfg) {
  AmbienceConfig out;
  while (!cfg.empty()) {
    const auto eol = cfg.find_first_of("\r\n");
    std::string_view line = cfg.substr(0, eol);
    cfg.remove_prefix(eol == std::string_view::npos ? cfg.size() : eol + 1);
    line = trim_left(line);
    const auto word_end = line.find_first_of(" \t");
    if (word_end == std::string_view::npos) continue;
    const std::string_view word = line.substr(0, word_end);
    const std::string_view rest = line.substr(word_end);
    double v[6] = {};
    if (word == "sfx_loop") {
      const size_t n = read_numbers(rest, v, 4);
      if (n < 1 || v[0] == 0) continue;
      AmbienceConfig::Loop loop;
      loop.sound = static_cast<uint32_t>(v[0]);
      if (n > 1) loop.radius = static_cast<float>(v[1]);
      if (n > 2) loop.from = hour_seconds(v[2]);
      if (n > 3) loop.to = hour_seconds(v[3]);
      out.loop = loop;
    } else if (word == "sfx_random") {
      if (out.randoms.size() >= kMaxRandoms) continue;
      const size_t n = read_numbers(rest, v, 6);
      if (n < 1 || v[0] == 0) continue;
      AmbienceConfig::Random random;
      random.sound = static_cast<uint32_t>(v[0]);
      const double min_delay = n > 2 ? v[2] : 1.0;
      const double max_delay = n > 3 ? v[3] : 1.0;
      if (n > 1) random.radius = static_cast<float>(v[1]);
      random.min_delay = static_cast<float>(min_delay);
      random.delay_range = static_cast<float>(max_delay - min_delay);
      if (n > 4) random.from = hour_seconds(v[4]);
      if (n > 5) random.to = hour_seconds(v[5]);
      out.randoms.push_back(random);
    }
  }
  return out;
}

bool scene_ambience_in_window(int32_t from, int32_t to, int32_t now) {
  if (from < to) return from <= now && now < to;
  return from <= now || now < to;
}

bool scene_ambience_audible(float distance_squared, float radius) {
  float reach = distance_squared;
  if (radius > 0.f) {
    reach -= radius * radius;
    if (reach < kNearSlack) return true;
  }
  return reach <= kStartRange * kStartRange;
}

float scene_ambience_gain(float volume, float min_distance, float max_distance,
                          float distance) {
  if (!(volume > 0.f) || !std::isfinite(distance)) return 0.f;
  min_distance = std::max(min_distance, .01f);
  max_distance = std::max(max_distance, min_distance);
  const float d = std::clamp(distance, min_distance, max_distance);
  return std::min(volume, 1.f) * (min_distance / d);
}

void scene_ambience_attach(void* shell, const RpkIndex& rpk, const GroundMapData& map) {
  if (!shell || !map.render_id || map.placements.empty()) return;
  for (auto it = g_shells.begin(); it != g_shells.end(); ++it) {
    if (it->shell != shell) continue;
    stop_shell(*it);
    g_shells.erase(it);
    break;
  }
  ShellAmbience entry;
  entry.shell = shell;
  entry.render_id = map.render_id;
  std::unordered_map<uint32_t, AmbienceConfig> configs;
  size_t loops = 0, randoms = 0;
  for (const auto& placement : map.placements) {
    auto [found, added] = configs.try_emplace(placement.type);
    if (added) found->second = scene_ambience_parse(config_text(rpk, placement.type));
    const AmbienceConfig& config = found->second;
    if (config.empty()) continue;
    const int32_t owner = RpkIndex::pack_from_full(placement.type);
    Emitter emitter;
    emitter.local = placement.position;
    if (config.loop) {
      LoopState loop{*config.loop, load_sound(rpk, owner, config.loop->sound)};
      if (loop.sound.ok) emitter.loop = std::move(loop);
    }
    for (const auto& random : config.randoms) {
      RandomState state{random, load_sound(rpk, owner, random.sound)};
      if (state.sound.ok) emitter.randoms.push_back(std::move(state));
    }
    if (!emitter.loop && emitter.randoms.empty()) continue;
    loops += emitter.loop ? 1 : 0;
    randoms += emitter.randoms.size();
    entry.emitters.push_back(std::move(emitter));
  }
  if (entry.emitters.empty()) return;
  std::printf("[ambience] shell 0x%x emitters=%zu loops=%zu random=%zu\n", map.render_id,
              entry.emitters.size(), loops, randoms);
  g_shells.push_back(std::move(entry));
}

void scene_ambience_tick(double seconds) {
  if (g_shells.empty() || g_paused) return;
  if (std::isfinite(seconds) && seconds > 0.0) g_clock += std::min(seconds, 1.0);
  float eye[3]{}, at[3]{};
  float fov = 0.f;
  const bool listener = gameref_world_camera(eye, at, &fov, nullptr, nullptr);
  const double day = std::fmod(static_cast<double>(scene_sun_time()), static_cast<double>(kDay));
  const int32_t now = static_cast<int32_t>(day < 0 ? day + kDay : day);
  for (auto it = g_shells.begin(); it != g_shells.end();) {
    const GameRefState* state = gameref_get(it->shell);
    if (!state || static_cast<uint32_t>(state->type_id) != it->render_id) {
      stop_shell(*it);
      it = g_shells.erase(it);
      continue;
    }
    float m[16];
    if (!listener || !scene_instances_script_matrix(it->shell, m)) {
      // Another world (garage, menu) or no camera: nothing of this map sounds.
      stop_shell(*it);
      ++it;
      continue;
    }
    for (auto& emitter : it->emitters) {
      // Most of the ~13,700 city emitters are random timers that are not due.
      if (!emitter.loop && emitter.scheduled && g_clock < emitter.next_time) continue;
      const auto& p = emitter.local;
      const float dx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] - eye[0];
      const float dy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] - eye[1];
      const float dz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] - eye[2];
      const float distance_squared = dx * dx + dy * dy + dz * dz;
      if (emitter.loop) update_loop(*emitter.loop, now, distance_squared);
      update_randoms(emitter, now, distance_squared);
    }
    ++it;
  }
}

void scene_ambience_pause(bool paused) {
  if (g_paused == paused) return;
  g_paused = paused;
  for (auto& shell : g_shells)
    for (auto& emitter : shell.emitters)
      if (emitter.loop && emitter.loop->voice) host_sfx_pause_loop(emitter.loop->voice, paused);
}

void scene_ambience_reset() {
  for (auto& shell : g_shells) stop_shell(shell);
  g_shells.clear();
  g_sounds.clear();
  g_clock = 0.0;
  g_paused = false;
}

}  // namespace slrr
