#include "slrr/surface_effects.hpp"

#include "slrr/ground_surface.hpp"
#include "slrr/job_system.hpp"
#include "slrr/particle_cloud.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/skid_marks.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace slrr {
namespace {

// A missing id falls back to the stock default in particles.rpk.
uint32_t effect_id(const RpkIndex &rpk, uint32_t full_id, uint32_t local) {
  if (full_id) return full_id;
  const RpkPack *pack = rpk.pack_by_name("particles.rpk");
  return pack ? RpkIndex::make_full_id(pack->pack_id, local) : 0;
}

std::string read_game_file(const RpkIndex &rpk, std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
  std::ifstream in(std::filesystem::path(rpk.game_root()) / path, std::ios::binary);
  if (!in) return {};
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The entry's `texture` id, in its own pack's slots, as a disk path.
std::string entry_texture(const RpkIndex &rpk, uint32_t id) {
  const std::string text = rpk.payload_prop(id, "texture");
  if (text.empty()) return {};
  const uint32_t local = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
  const uint32_t resolved = rpk.resolve_embedded_id(RpkIndex::pack_from_full(id), local);
  return resolved ? rpk.texture_path_for_id(resolved) : std::string{};
}

}  // namespace

uint16_t surface_particle_cloud(const RpkIndex &rpk, uint32_t full_id) {
  const uint32_t id = effect_id(rpk, full_id, kCloudAsphalt);
  if (!id) return 0xffff;
  if (const uint16_t known = particle_cloud_find(id); known != 0xffff)
    return particle_clouds()[known].ok ? known : 0xffff;
  const std::string cfg = rpk.payload_prop(id, "particle");
  ParticleCloudConfig config;
  if (!cfg.empty()) config = parse_particle_cloud(read_game_file(rpk, cfg));
  if (config.ok) config.texture = entry_texture(rpk, id);
  jobs_printf("[effects] cloud %08x %s: %s rate %.0f type %d texture %s\n", id, cfg.c_str(),
              config.ok ? "ok" : "missing", config.rate, config.type,
              config.texture.empty() ? "-" : std::filesystem::path(config.texture).filename().string().c_str());
  const bool ok = config.ok;
  const uint16_t index = particle_cloud_register(id, std::move(config));
  return ok ? index : 0xffff;
}

uint16_t surface_skid_style(const RpkIndex &rpk, uint32_t full_id) {
  const uint32_t id = effect_id(rpk, full_id, kSkidAsphalt);
  if (!id) return 0;
  if (const uint16_t known = skid_mark_style_find(id); known != 0xffff) return known;
  const std::string cfg = rpk.payload_prop(id, "skidmark");
  const std::string text = cfg.empty() ? std::string{} : read_game_file(rpk, cfg);
  SkidMarkStyle style;
  const bool ok = !text.empty();
  if (ok) {
    style = parse_skid_mark_config(text);
    style.texture = entry_texture(rpk, id);
  }
  jobs_printf("[effects] skid %08x %s: %s width %.2f-%.2f tile %.2f texture %s\n", id,
              cfg.c_str(), ok ? "ok" : "missing", style.width_min, style.width_max,
              style.height,
              style.texture.empty() ? "-" : std::filesystem::path(style.texture).filename().string().c_str());
  // An unreadable mark keeps the untextured road strip.
  return ok ? skid_mark_style_register(id, std::move(style)) : skid_mark_style_register(id, {});
}

}  // namespace slrr
