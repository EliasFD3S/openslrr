#pragma once

#include "slrr/mods.hpp"
#include "provider_index.hpp"
#include <filesystem>
#include <iosfwd>
#include <set>
#include <string_view>
#include <vector>

namespace slrr {
struct ModManifest : ModInfo {
  std::string entry;
};
std::vector<ModManifest> discover_mods(const std::filesystem::path& root, std::ostream* log);
void publish_mod_list(const std::vector<ModManifest>& mods);

struct TufaClass;
// One index from fully qualified class name to the container that provides it,
// over every class container of the base game and the enabled mods (the
// compiler builds the same slrrc::ProviderIndex). Paths only identify
// containers; classes are resolved by name.
class ModOverlay {
 public:
  struct ClassContainer {
    std::filesystem::path path;  // absolute, as walked
    std::string_view primary;    // the class a resource path to this file names
    bool base = false;
  };
  // Indexes all containers, parsing each once on the loading pool.
  void scan(const std::filesystem::path& game, std::ostream& log);
  // The provider of exactly this name; empty when no valid container declares it.
  std::filesystem::path provider(const std::string& fqn) const;
  // The one indexed name with this simple name; empty when none or ambiguous.
  std::string unique_short_name(const std::string& simple, bool* ambiguous = nullptr) const;
  // A resource path (game-relative, logical): the base container there when it
  // declares something, else the highest-priority mod's; nullptr when none.
  const ClassContainer* container(const std::filesystem::path& logical) const;
  // A container's blobs, each replaced by its provider's definition.
  bool load(const std::filesystem::path& container, std::vector<TufaClass>& blobs,
            std::string& error) const;
  // Game-relative logical path of a physical path; an enabled mod's prefix is
  // stripped (*mod_path = true). Outside the game: absolute. Empty for a
  // disabled or unknown mod folder.
  std::filesystem::path logical(const std::filesystem::path& physical, bool* mod_path = nullptr) const;
  const std::set<std::string_view>& packages() const { return index_.packages(); }
  const std::vector<ModManifest>& mods() const { return mods_; }
 private:
  std::filesystem::path game_;
  std::vector<ModManifest> mods_;
  slrrc::ProviderIndex index_;            // layers: mods in load order, then base
  std::vector<ClassContainer> containers_;  // by slrrc::Container::id
};
}  // namespace slrr
