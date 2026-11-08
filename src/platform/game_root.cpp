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

std::string absolute_root(const fs::path& path) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(path, ec);
  return ec ? std::string{} : absolute.lexically_normal().make_preferred().string();
}

}  // namespace

bool game_root_looks_valid(const std::string& root) {
  if (root.empty()) return false;
  return probe(fs::path(root));
}

std::string find_game_root(const char* explicit_path) {
  if (explicit_path && explicit_path[0]) {
    return probe(explicit_path) ? absolute_root(explicit_path) : std::string{};
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
    if (probe(c)) return absolute_root(c);
  }
  return {};
}

}  // namespace slrr
