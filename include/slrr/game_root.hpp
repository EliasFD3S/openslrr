#pragma once

#include <string>

namespace slrr {

// Locate an SLRR install (system/Scripts/lang/System.class present).
// Candidates: explicit path, cwd, data/game, ./game, ../game, common Steam layout.
std::string find_game_root(const char* explicit_path = nullptr);

bool game_root_looks_valid(const std::string& root);

}  // namespace slrr
