#include "slrr/script_compiler.hpp"
#include "slrr/tufa.hpp"
#include "install.hpp"
#include "slrrc.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace slrr {
namespace {
std::filesystem::path utf8_path(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
std::string source_text(const slrrc::install::Install& tree,
                        const slrrc::install::SourceFile& source, std::string text) {
  const auto names = slrrc::declared_names(text);
  if (names.empty() || names.front().find('.') != std::string::npos) return text;
  // System scripts often omit `package`; their class identity comes from
  // the installed container or the engine classpath, not a Java SE default.
  std::string package;
  std::vector<TufaClass> previous; std::string error;
  if (tufa_load_file(tree.path(source.output_rel()), &previous, &error) && !previous.empty()) {
    const auto name = previous.front().name();
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) package = name.substr(0, dot);
  }
  if (package.empty()) {
    const auto path = slrrc::install::lower(source.class_dir);
    for (const auto& prefix : {std::string("system/scripts/"), std::string("sl/scripts/"),
                              std::string("parts/scripts")}) {
      if (path.compare(0, prefix.size(), prefix) != 0) continue;
      package = prefix == "parts/scripts" ? "java.game.parts" + path.substr(prefix.size())
                                          : "java." + path.substr(prefix.size());
      std::replace(package.begin(), package.end(), '/', '.');
      break;
    }
  }
  if (!package.empty()) {
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    // Same line preserves diagnostic source line numbers.
    text.insert(0, "package " + package + "; ");
  }
  return text;
}
bool publish(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  auto staging = path; staging += L".openslrr.tmp";
  auto backup = path; backup += L".openslrr.bak";
  std::ofstream out(staging, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  out.close();
  if (!out) { DeleteFileW(staging.c_str()); return false; }
  // Preserve the first on-disk original. A failed compilation never publishes.
  if (std::filesystem::exists(path) && !std::filesystem::exists(backup) &&
      !CopyFileW(path.c_str(), backup.c_str(), TRUE)) {
    DeleteFileW(staging.c_str()); return false;
  }
  const bool ok = MoveFileExW(staging.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(staging.c_str());
  return ok;
}
}
ScriptCompileReport script_compile_all(const std::string& root,
    const std::string& directory, const std::function<void()>& checkpoint) {
  ScriptCompileReport report;
  try {
    // Compiler literals must match the reference under the current FPU state.
    if (!slrrc::floatlit::self_test().empty())
      throw std::runtime_error("compiler floating-point self-test failed");
    slrrc::install::Install tree(root);
    const auto requested = slrrc::install::join_path(tree.root, directory.empty() ? "." : directory);
    if (!slrrc::install::is_inside(requested, tree.root) ||
        !slrrc::install::is_dir(requested))
      throw std::runtime_error("compile directory is missing or outside the game root");
    tree.scan();
    slrrc::install::InstallProvider provider(tree);
    provider.load();
    slrrc::install::InstallPacks packs(tree);
    for (const auto& source : tree.sources) {
      if (!slrrc::install::is_inside(source.path, requested)) continue;
      ++report.attempted;
      try {
        std::string text, why;
        if (!slrrc::install::read_source(source, text, why)) throw std::runtime_error(why);
        text = source_text(tree, source, std::move(text));
        const auto result = slrrc::compile(text, source.rel, provider, packs.table);
        for (const auto& diagnostic : result.diagnostics)
          std::fprintf(stderr, "[compiler] %s\n", diagnostic.format(source.rel).c_str());
        if (!result.ok) throw std::runtime_error("source compilation failed");
        // Validate with the host's independent TUFA reader before replacing a file.
        std::vector<TufaClass> parsed;
        if (!tufa_parse(result.class_file.data(), result.class_file.size(), &parsed, &why) ||
            parsed.size() != result.class_names.size())
          throw std::runtime_error("generated TUFA validation failed: " + why);
        const auto path = utf8_path(tree.path(source.output_rel()));
        if (!publish(path, result.class_file)) throw std::runtime_error("cannot publish compiled file");
        ++report.succeeded;
        report.classes += static_cast<int>(parsed.size());
      } catch (const std::exception& error) {
        ++report.failed;
        std::fprintf(stderr, "[compiler] %s: %s\n", source.rel.c_str(), error.what());
      }
      if (checkpoint) checkpoint();
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[compiler] %s\n", error.what());
  }
  std::printf("[compiler] attempted=%d succeeded=%d failed=%d classes=%d (restart for loaded classes)\n",
              report.attempted, report.succeeded, report.failed, report.classes);
  return report;
}
}
