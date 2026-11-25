#include "slrr/script_compiler.hpp"
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>

namespace {
bool parse_float_mode(const wchar_t* text, slrr::FloatLiteralMode& mode) {
  using slrr::FloatLiteralMode;
  if (!std::wcscmp(text, L"auto")) { mode = FloatLiteralMode::Auto; return true; }
  if (text[0] && !text[1]) {
    switch (text[0]) {
    case L'A': mode = FloatLiteralMode::A; return true;
    case L'B': mode = FloatLiteralMode::B; return true;
    case L'C': mode = FloatLiteralMode::C; return true;
    case L'D': mode = FloatLiteralMode::D; return true;
    }
  }
  return false;
}
}

int wmain(int argc, wchar_t** argv) {
  // Offline rebuilds of stock classes default to auto: each unit keeps the
  // float conversion of the class it replaces (#420). The game's own
  // System.compileAll stays on B, the exe's conversion.
  slrr::ScriptCompileOptions options;
  options.float_mode = slrr::FloatLiteralMode::Auto;
  int first = 1;
  if (argc >= 2 && !std::wcscmp(argv[1], L"--float-mode")) {
    if (argc < 3 || !parse_float_mode(argv[2], options.float_mode)) {
      std::fprintf(stderr, "[compiler] --float-mode takes A, B, C, D or auto\n");
      return 2;
    }
    first = 3;
  }
  if (argc - first != 2) {
    std::fprintf(stderr, "Usage: compile_game_sources [--float-mode A|B|C|D|auto] <game-root> <directory>\n"
                         "Compiles sources in directory relative to game-root.\n"
                         "Use a copy: compiled classes are written into game-root.\n"
                         "--float-mode: float literal conversion (default auto: per unit, the\n"
                         "one that reproduces the existing class; B is the game's own).\n");
    return 2;
  }

  slrr::ScriptCompileReport report;
  try {
    const auto utf8 = [](const wchar_t* value) {
      const auto encoded = std::filesystem::path(value).u8string();
      return std::string(encoded.begin(), encoded.end());
    };
    report = slrr::script_compile_all(utf8(argv[first]), utf8(argv[first + 1]), options);
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
