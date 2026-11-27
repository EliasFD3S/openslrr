#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace slrr {
// Host path strings in the class loader, overlay and compiler are UTF-8, never
// the Windows ANSI code page, so folders such as mods/日本 stay usable.
inline std::filesystem::path path_from_utf8(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
// Generic ('/') separators.
inline std::string path_to_utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}
}  // namespace slrr
