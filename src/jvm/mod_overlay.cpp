#include "mod_overlay.hpp"
#include "slrr/mod_api.hpp"
#include "slrr/tufa.hpp"
#include "slrr/utf8_path.hpp"
#include "../platform/host_options_json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <ostream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace slrr {
namespace fs = std::filesystem;
namespace {
std::mutex list_mutex;
std::vector<ModInfo> active;

std::string key(const fs::path& rel) {
  auto text = path_to_utf8(rel.lexically_normal());
  for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}

// Windows folder names compare ordinally without case, for any script.
int compare_names(const fs::path& a, const fs::path& b) {
  const auto& x = a.native();
  const auto& y = b.native();
  return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE);
}
struct NameLess {
  bool operator()(const fs::path& a, const fs::path& b) const { return compare_names(a, b) == CSTR_LESS_THAN; }
};

bool identifier(const std::string& name) {
  bool first = true;
  for (unsigned char c : name) {
    if (c == '.' && !first) { first = true; continue; }
    if (!(std::isalpha(c) || c == '_' || c == '$' || (!first && std::isdigit(c)))) return false;
    first = false;
  }
  return !first && name.find('.') != name.npos;
}

std::string read_manifest(const fs::path& path, ModManifest& mod) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "missing or unreadable mod.json";
  std::string bytes{std::istreambuf_iterator<char>(in), {}};
  if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
  if (bytes.size() > INT_MAX || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
          bytes.data(), static_cast<int>(bytes.size()), nullptr, 0)) return "invalid UTF-8 mod.json";
  using options_json::Scalar;
  std::map<std::string, Scalar> fields;
  std::string error;
  if (!options_json::parse_flat(bytes, fields, &error)) return "invalid mod.json: " + error;
  auto string_field = [&](const char* key, std::string& out) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.kind != Scalar::String || it->second.text.empty()) return false;
    out = it->second.text;
    return std::none_of(out.begin(), out.end(), [](unsigned char c) { return c < 0x20; });
  };
  if (!string_field("name", mod.name) || !string_field("version", mod.version))
    return "invalid mod.json: expected name and version strings";
  const auto api = fields.find("api");
  if (api == fields.end() || api->second.kind != Scalar::Number ||
      api->second.text.find_first_of(".eE-") != std::string::npos)
    return "invalid mod.json: expected nonnegative integer api";
  if (fields.contains("entry") && (!string_field("entry", mod.entry) || !identifier(mod.entry)))
    return "invalid mod.json: expected an entry class name";
  if (const auto it = fields.find("enabled"); it != fields.end()) {
    if (it->second.kind != Scalar::Bool) return "invalid mod.json: expected boolean enabled";
    if (!it->second.boolean) return "disabled";
  }
  if (api->second.number > kModApiVersion) return "requires API " + api->second.text;
  return {};
}
}  // namespace

std::vector<ModInfo> mods_loaded() {
  std::lock_guard lock(list_mutex);
  return active;
}
void publish_mod_list(const std::vector<ModManifest>& mods) {
  std::lock_guard lock(list_mutex);
  active.assign(mods.begin(), mods.end());
}

std::vector<ModManifest> discover_mods(const fs::path& root, std::ostream* log) {
  const fs::path dir = root / "mods";
  // One unusable entry must never abort discovery. The map's order is the
  // case-insensitive fallback order; `listed` marks load_order.txt names.
  std::map<fs::path, bool, NameLess> folders;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    try {
      if (it->is_directory(ec)) folders.emplace(it->path().filename(), false);
    } catch (const std::exception& e) {
      if (log) *log << "[mods] skipped an unreadable entry in mods/: " << e.what() << '\n';
    }
  }
  std::vector<fs::path> ordered;
  std::ifstream order(dir / "load_order.txt");
  int line_number = 0;
  for (std::string line; std::getline(order, line);) {
    ++line_number;
    if (line_number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
    line = line.substr(0, line.find('#'));
    auto start = line.find_first_not_of(" \t\r");
    if (start == line.npos) continue;
    line = line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
    fs::path wanted;
    try {
      if (!line.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(),
                                                static_cast<int>(line.size()), nullptr, 0))
        throw std::runtime_error("invalid UTF-8");
      wanted = path_from_utf8(line);
    } catch (const std::exception&) {
      if (log) *log << "[mods] load_order.txt line " << line_number << ": not valid UTF-8 (ignored)\n";
      continue;
    }
    const auto hit = folders.find(wanted);
    if (hit == folders.end()) {
      if (log) *log << "[mods] load_order.txt line " << line_number << ": no mods/" << line << " folder (ignored)\n";
    } else if (hit->second) {
      if (log) *log << "[mods] load_order.txt line " << line_number << ": mods/" << line << " is listed again (ignored)\n";
    } else {
      hit->second = true;
      ordered.push_back(hit->first);
    }
  }
  for (const auto& [name, listed] : folders) if (!listed) ordered.push_back(name);
  std::vector<ModManifest> result;
  for (const auto& name : ordered) {
    ModManifest mod;
    try {
      mod.folder = path_to_utf8(name);
    } catch (const std::exception& e) {
      if (log) *log << "[mods] skipped a mods/ folder whose name is not convertible: " << e.what() << '\n';
      continue;
    }
    const auto& folder = mod.folder;
    const auto reason = read_manifest(dir / name / "mod.json", mod);
    if (!reason.empty()) {
      if (log) *log << "[mods] mods/" << folder << ": skipped (" << reason << ")\n";
    } else {
      if (log) *log << "[mods] mods/" << folder << ": " << mod.name << ' ' << mod.version << '\n';
      result.push_back(std::move(mod));
    }
  }
  return result;
}

void ModOverlay::scan(const fs::path& game, std::ostream& log) {
  game_ = fs::absolute(game).lexically_normal();
  mods_ = discover_mods(game_, &log);
  winners_.clear();
  classes_.clear();
  base_siblings_.clear();
  struct Conflict {
    std::vector<std::string> folders;
    std::vector<fs::path> files;
    bool base = false;
  };
  std::map<std::string, Conflict> conflicts;
  auto scan_layer = [&](const fs::path& layer, const ModManifest* mod) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(layer, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
      if (!mod && it.depth() == 0 && key(it->path().filename()) == "mods") {
        it.disable_recursion_pending();
        continue;
      }
      if (key(it->path().extension()) != ".class") continue;
      if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    // Stable order for duplicate declarations within one layer. Prefer a
    // container named after the class, as the compiler does at equal priority.
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
      const auto rel = key(path.lexically_relative(layer));
      if (mod) winners_.try_emplace(rel, path);
      std::vector<TufaClass> blobs;
      std::string error;
      if (!tufa_load_file(path_to_utf8(path), &blobs, &error)) {
        if (mod) log << "[mods] " << path_to_utf8(path.lexically_relative(game_)) << ": " << error << '\n';
        continue;
      }
      for (const auto& cls : blobs) {
        const auto name = cls.name();
        if (!mod) {
          if (auto hit = conflicts.find(name); hit != conflicts.end()) hit->second.base = true;
          else if (winners_.contains(rel)) base_siblings_.try_emplace(name, path);
          continue;
        }
        auto& conflict = conflicts[name];
        conflict.files.push_back(path);
        auto& folders = conflict.folders;
        if (folders.empty() || folders.back() != mod->folder) folders.push_back(mod->folder);
        auto [winner, fresh] = classes_.try_emplace(name, path);
        const auto simple = name.substr(name.rfind('.') + 1);
        if (!fresh && folders.front() == mod->folder && path_to_utf8(path.stem()) == simple &&
            path_to_utf8(winner->second.stem()) != simple) winner->second = path;
      }
    }
  };
  for (const auto& mod : mods_) scan_layer(game_ / "mods" / path_from_utf8(mod.folder), &mod);
  // No mods supplying classes: retain the stock lazy boot path. Otherwise
  // base secondary blobs and resource scripts also count as displaced providers.
  if (!classes_.empty()) scan_layer(game_, nullptr);
  for (const auto& [name, conflict] : conflicts) {
    const auto& folders = conflict.folders;
    const bool duplicates = conflict.files.size() != folders.size();
    if (!conflict.base && conflict.files.size() == 1) continue;
    log << "[mods] " << name << ": mods/" << folders.front() << " (";
    if (conflict.base) log << "overrides base";
    if (folders.size() > 1) {
      if (conflict.base) log << "; ";
      log << "also in mods/" << folders[1];
      for (size_t i = 2; i < folders.size(); ++i) log << ", mods/" << folders[i];
    }
    if (duplicates) {
      if (conflict.base || folders.size() > 1) log << "; ";
      const auto& winner = classes_.at(name);
      log << "using " << path_to_utf8(winner.lexically_relative(game_));
      for (const auto& file : conflict.files) if (file != winner)
        log << ", not " << path_to_utf8(file.lexically_relative(game_));
    }
    log << ")\n";
  }
}

fs::path ModOverlay::provider(const std::string& fqn) const {
  const auto hit = classes_.find(fqn);
  if (hit != classes_.end()) return hit->second;
  const auto base = base_siblings_.find(fqn);
  return base != base_siblings_.end() ? base->second : fs::path{};
}

bool ModOverlay::load(const fs::path& container, std::vector<TufaClass>& blobs, std::string& error) const {
  if (!tufa_load_file(path_to_utf8(container), &blobs, &error)) return false;
  // An RPK names a file, not an FQN. Preserve that file's class identities
  // (and hence its primary class), substituting only the displaced blobs.
  std::map<fs::path, std::vector<TufaClass>> replacements;
  for (auto& blob : blobs) {
    const auto name = blob.name();
    const auto hit = classes_.find(name);
    if (hit == classes_.end() || hit->second == container) continue;
    const auto& winner = hit->second;
    auto [it, fresh] = replacements.try_emplace(winner);
    if (fresh && !tufa_load_file(path_to_utf8(winner), &it->second, &error)) return false;
    auto chosen = std::find_if(it->second.begin(), it->second.end(), [&](const auto& c) { return c.name() == name; });
    if (chosen == it->second.end()) {
      error = path_to_utf8(winner) + " no longer provides " + name;
      return false;
    }
    blob = *chosen;
  }
  return true;
}

fs::path ModOverlay::resolve(const fs::path& relative) const {
  if (!winners_.empty()) {
    const auto it = winners_.find(key(relative));
    if (it != winners_.end()) return it->second;
  }
  return game_ / relative;
}

fs::path ModOverlay::logical(const fs::path& physical) const {
  const auto absolute = fs::absolute(physical).lexically_normal();
  auto rel = absolute.lexically_relative(game_);
  if (rel.empty() || *rel.begin() == "..") return absolute;  // external tool input
  auto it = rel.begin();
  if (key(*it++) == "mods" && it != rel.end()) {
    const auto folder = *it++;
    for (const auto& mod : mods_) if (compare_names(folder, path_from_utf8(mod.folder)) == CSTR_EQUAL) {
      fs::path tail;
      for (; it != rel.end(); ++it) tail /= *it;
      return tail;
    }
    return {};  // direct path to a disabled or invalid mod
  }
  return rel;
}
}  // namespace slrr
