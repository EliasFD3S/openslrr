#include "slrr/game_root.hpp"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace slrr {
namespace {

bool probe(const fs::path& root) {
  const fs::path marker = root / "system" / "Scripts" / "lang" / "System.class";
  std::error_code ec;
  return fs::is_regular_file(marker, ec);
}

}  // namespace

bool game_root_looks_valid(const std::string& root) {
  if (root.empty()) return false;
  return probe(fs::path(root));
}

std::string find_game_root(const char* explicit_path) {
  if (explicit_path && explicit_path[0]) {
    if (probe(fs::path(explicit_path))) return explicit_path;
    return {};
  }

  static const char* kCandidates[] = {
      ".",
      "game",
      "../game",
      "../../game",
      "Street Legal Racing - Redline",
      "../Street Legal Racing - Redline",
  };

  for (const char* c : kCandidates) {
    if (probe(fs::path(c))) {
      std::error_code ec;
      fs::path abs = fs::absolute(c, ec);
      return ec ? std::string(c) : abs.string();
    }
  }
  return {};
}

}  // namespace slrr
