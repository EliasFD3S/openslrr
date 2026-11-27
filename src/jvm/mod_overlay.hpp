#pragma once

#include "slrr/mods.hpp"
#include <filesystem>
#include <iosfwd>
#include <map>

namespace slrr {
struct ModManifest : ModInfo {
  std::string entry;
};
std::vector<ModManifest> discover_mods(const std::filesystem::path& root, std::ostream* log);
void publish_mod_list(const std::vector<ModManifest>& mods);

// For both FQN lookup and direct RPK script paths. Only mod classes are
// indexed; the base game (including resource-discovered roots) stays lazy.
class ModOverlay {
 public:
  void scan(const std::filesystem::path& game, std::ostream& log);
  std::filesystem::path resolve(const std::filesystem::path& relative) const;
  std::filesystem::path logical(const std::filesystem::path& physical) const;
  const std::vector<ModManifest>& mods() const { return mods_; }
 private:
  std::filesystem::path game_;
  std::vector<ModManifest> mods_;
  std::map<std::string, std::filesystem::path> winners_;
};
}  // namespace slrr
