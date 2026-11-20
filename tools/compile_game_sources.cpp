#include "slrr/script_compiler.hpp"
#include <cstdio>
#include <exception>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: compile_game_sources <game-root> <directory>\n"
                         "Compiles sources in directory relative to game-root.\n"
                         "Use a copy: compiled classes are written into game-root.\n");
    return 2;
  }

  slrr::ScriptCompileReport report;
  try {
    const auto utf8 = [](const wchar_t* value) {
      const auto encoded = std::filesystem::path(value).u8string();
      return std::string(encoded.begin(), encoded.end());
    };
    report = slrr::script_compile_all(utf8(argv[1]), utf8(argv[2]));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[compiler] %s\n", error.what());
    return 1;
  }
  // Setup errors also return an empty report; do not report them as success.
  if (report.attempted == 0) {
    std::fprintf(stderr, "[compiler] no source units compiled\n");
    return 1;
  }
  return report.failed == 0 && report.succeeded == report.attempted ? 0 : 1;
}
