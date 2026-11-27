#pragma once

#include <string>
#include <vector>

namespace slrr {
struct ModInfo {
  std::string name, version, folder;  // folder is the directory name under mods/
};

// Snapshot of the active game's enabled manifests, highest priority first.
std::vector<ModInfo> mods_loaded();
}  // namespace slrr
