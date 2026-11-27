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

struct TufaClass;
// Declared class identities are independent of the path of their TUFA container.
class ModOverlay {
 public:
  void scan(const std::filesystem::path& game, std::ostream& log);
  std::filesystem::path resolve(const std::filesystem::path& relative) const;
  std::filesystem::path provider(const std::string& fqn) const;
  bool load(const std::filesystem::path& container, std::vector<TufaClass>& blobs,
            std::string& error) const;
  std::filesystem::path logical(const std::filesystem::path& physical) const;
  const std::vector<ModManifest>& mods() const { return mods_; }
 private:
  std::filesystem::path game_;
  std::vector<ModManifest> mods_;
  std::map<std::string, std::filesystem::path> winners_;  // game-relative path -> first mod container
  std::map<std::string, std::filesystem::path> classes_;  // exact FQN -> mod provider
  std::map<std::string, std::filesystem::path> base_siblings_;  // in mirrored containers
};
}  // namespace slrr
