#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {

struct RpkDep {
  uint32_t id = 0;
  std::string path;
  uint32_t trailer = 0;
};

struct RpkEntry {
  uint32_t super_id = 0;
  uint32_t type_id = 0;  // local resource id (lo16 meaningful)
  uint8_t typeof_ = 0;
  uint8_t flags = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  std::string alias;
};

struct RpkPack {
  int32_t pack_id = 0;
  std::string path;  // path as opened (relative or absolute)
  uint32_t version = 0;
  std::vector<RpkDep> deps;
  std::vector<RpkEntry> entries;
  // type_id (full u32 as stored) → index into entries
  std::unordered_map<uint32_t, size_t> by_type_id;
};

// openLib / rpkScan + RPAK TOC parse (SLRR_rev rpk.md).
class RpkIndex {
 public:
  void set_root(std::string root);

  // Non-recursive *.rpk basenames in dir (relative to game root or absolute).
  std::vector<std::string> list_rpks(const std::string& dir) const;

  // Every *.rpk under the game root, any folder. No fixed pack list.
  // Already-open paths keep their pack id. Returns how many files were opened.
  int32_t open_all();

  // Register pack path → id; parse RPAK TOC when file exists. Returns pack id (>0).
  int32_t open_lib(const std::string& path);

  int32_t pack_count() const { return static_cast<int32_t>(packs_.size()); }

  const RpkPack* pack_by_id(int32_t id) const;
  const RpkPack* pack_by_path(const std::string& path) const;
  // "frontend.rpk", "MW_Mod.rpk", or the same name in a subfolder.
  const RpkPack* pack_by_name(const std::string& pack) const;

  // Lookup entry in pack by local type_id (exact u32 match, then lo16).
  const RpkEntry* find_entry(int32_t pack_id, uint32_t type_id) const;

  // Runtime full id = (pack_id << 16) | (type_id & 0xFFFF).
  static uint32_t make_full_id(int32_t pack_id, uint32_t local) {
    return (static_cast<uint32_t>(pack_id & 0xFFFF) << 16) |
           (local & 0xFFFFu);
  }
  static int32_t pack_from_full(uint32_t full) {
    return static_cast<int32_t>((full >> 16) & 0xFFFF);
  }
  static uint32_t local_from_full(uint32_t full) { return full & 0xFFFFu; }

  // Resolve ResourceRef full id → entry (nullptr if unknown).
  const RpkEntry* resolve(uint32_t full_id) const;

  // Read entry payload bytes (RSD text or binary). Empty on miss.
  std::vector<uint8_t> read_payload(uint32_t full_id) const;

  // First `sourcefile` / `texture` property from text RSD payload.
  std::string payload_prop(uint32_t full_id, const char* key) const;

  // Absolute disk path for an OSD/texture resource (follows sourcefile / texture).
  // Accepts local id (lo16) or packed full id. Empty on miss.
  std::string texture_path_for_id(uint32_t id) const;

  // Id written inside an RPK payload (`texture 0x00SSLLLL`). SS is a dependency
  // slot of owner_pack (0 = that pack), not the host pack id.
  uint32_t resolve_embedded_id(int32_t owner_pack, uint32_t rid) const;

  // All values for a repeated RSD key (preserves order — critical for texture).
  std::vector<std::string> payload_props(uint32_t full_id,
                                         const char* key) const;

  // Follow mesh/render resource → `texture` refs → sourcefile paths (RSD order).
  // Matches SCX/SCY basename under game root. Empty if not found in open packs.
  std::vector<std::string> textures_for_scx(const std::string& scx_path) const;

  // Resolve a type-14 (or mesh) resource → SCX path + texture slot list.
  struct ModelRef {
    bool ok = false;
    uint32_t type_id = 0;
    std::string alias;
    std::string source;  // game-relative, may be .scY
    std::vector<std::string> textures;
  };
  ModelRef model_for_resource(int32_t pack_id, uint32_t local_id) const;

  // Club garage type-14 renders under folder alias g1/g2/g3/g4 (map + stuff).
  // garage_index 0..3 → g1..g4. Empty if pack/folder missing.
  std::vector<uint32_t> garage_render_ids(int32_t pack_id,
                                          int garage_index) const;

  // Dependency-qualified children across all packs, in stable pack/TOC order.
  std::vector<uint32_t> children_of(uint32_t parent) const;

  int32_t total_entries() const;

  std::string disk_path_for_pack(int32_t pack_id) const;

  const std::string& game_root() const { return root_; }

 private:
  std::string resolve_disk_path(const std::string& path) const;
  bool parse_rpak(const std::string& disk, RpkPack& pack) const;

  std::string root_;
  std::unordered_map<std::string, int32_t> by_path_;
  std::vector<RpkPack> packs_;  // index 0 unused; pack_id == index
  int32_t next_id_ = 1;
};

// Resource constants are pack path + local id. open_all publishes the index
// so the interpreter can keep the pack instead of a bare local id.
void rpk_set_active(const RpkIndex* index);
uint32_t rpk_resolve_resource(const std::string& pack, uint32_t local);

}  // namespace slrr
