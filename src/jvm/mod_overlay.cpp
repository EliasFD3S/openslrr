#include "mod_overlay.hpp"
#include "slrr/mod_api.hpp"
#include "slrr/tufa.hpp"
#include "../platform/host_options_json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ostream>
#include <set>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace slrr {
namespace fs = std::filesystem;
namespace {
std::mutex list_mutex;
std::vector<ModInfo> active;

std::string key(const fs::path& rel) {
  auto text = rel.lexically_normal().generic_string();
  for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}

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
  if (!options_json::parse(bytes, fields, &error, true)) return "invalid mod.json: " + error;
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
  std::set<std::string> remaining;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    if (it->is_directory(ec)) remaining.insert(it->path().filename().string());
  std::vector<std::string> ordered;
  std::ifstream order(dir / "load_order.txt");
  for (std::string line; std::getline(order, line);) {
    if (line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
    line = line.substr(0, line.find('#'));
    auto start = line.find_first_not_of(" \t\r");
    if (start == line.npos) continue;
    line = line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
    if (remaining.erase(line)) ordered.push_back(line);
  }
  ordered.insert(ordered.end(), remaining.begin(), remaining.end());
  std::vector<ModManifest> result;
  for (const auto& folder : ordered) {
    ModManifest mod;
    mod.folder = folder;
    const auto reason = read_manifest(dir / folder / "mod.json", mod);
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
  std::map<std::string, std::vector<std::string>> providers;
  std::error_code ec;
  for (const auto& mod : mods_) {
    const auto base = game_ / "mods" / mod.folder;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec) || key(it->path().extension()) != ".class") continue;
      const auto rel = key(it->path().lexically_relative(base));
      winners_.try_emplace(rel, it->path());
      providers[rel].push_back(mod.folder);
    }
  }
  for (const auto& [rel, folders] : providers) {
    const bool overrides = fs::is_regular_file(game_ / rel, ec);
    if (!overrides && folders.size() == 1) continue;
    std::vector<TufaClass> classes;
    std::string error;
    const auto& path = winners_.at(rel);
    tufa_load_file(path.string(), &classes, &error);
    auto report = [&](const std::string& name) {
      log << "[mods] " << name << ": mods/" << folders.front() << " (";
      if (overrides) log << "overrides base";
      if (folders.size() > 1) {
        if (overrides) log << "; ";
        log << "also in mods/" << folders[1];
        for (size_t i = 2; i < folders.size(); ++i) log << ", mods/" << folders[i];
      }
      log << ")\n";
    };
    if (classes.empty()) report(rel);
    for (const auto& cls : classes) report(cls.name());
  }
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
    for (const auto& mod : mods_) if (key(folder) == key(mod.folder)) {
      fs::path tail;
      for (; it != rel.end(); ++it) tail /= *it;
      return tail;
    }
    return {};  // direct path to a disabled or invalid mod
  }
  return rel;
}
}  // namespace slrr
