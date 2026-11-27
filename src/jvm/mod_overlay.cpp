#include "mod_overlay.hpp"
#include "slrr/mod_api.hpp"
#include "slrr/job_system.hpp"
#include "slrr/tufa.hpp"
#include "slrr/utf8_path.hpp"
#include "tree_walk.hpp"
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

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
std::string folded(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = fold(c);
  return out;
}

std::string key(const fs::path& rel) { return folded(path_to_utf8(rel.lexically_normal())); }

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
  const int base = static_cast<int>(mods_.size());
  struct File {
    fs::path path;
    std::string rel;
    int layer;
  };
  std::vector<File> files;
  // The walk the compiler shares: links to directories followed, cycles not.
  auto walk_layer = [&](const fs::path& root, int layer) {
    slrrc::tree::walk(root, [&](const slrrc::tree::Entry& entry) {
      if (entry.directory) return layer != base || entry.depth != 0 || folded(entry.name) != "mods";
      if (entry.name.size() > 6 && folded(entry.name).ends_with(".class"))
        files.push_back({entry.path, entry.rel, layer});
      return true;
    }, [&](const slrrc::tree::Problem& problem) { log << "[mods] " << slrrc::tree::describe(problem) << '\n'; });
  };
  for (size_t i = 0; i < mods_.size(); ++i) walk_layer(game_ / "mods" / path_from_utf8(mods_[i].folder), static_cast<int>(i));
  walk_layer(game_, base);
  // Parse every container once on the loading pool, keeping names only.
  struct Declarations {
    std::vector<std::string> names;
    std::string error;
  };
  std::vector<Declarations> declarations(files.size());
  jobs_parallel_for(files.size(), [&](size_t i) {
    auto& result = declarations[i];
    try {
      std::vector<TufaClass> blobs;
      if (tufa_load_file(path_to_utf8(files[i].path), &blobs, &result.error))
        for (const auto& cls : blobs) result.names.push_back(cls.name());
    } catch (const std::exception& e) {
      result.names.clear();
      result.error = e.what();
    }
  });
  std::vector<slrrc::Container> list;
  list.reserve(files.size());
  containers_.assign(files.size(), {});
  std::vector<size_t> primary(files.size());
  size_t classes = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    auto& file = files[i];
    auto& names = declarations[i].names;
    if (names.empty() && file.layer != base)
      log << "[mods] " << path_to_utf8(file.path.lexically_relative(game_)) << ": "
          << (declarations[i].error.empty() ? "empty class container" : declarations[i].error) << '\n';
    // The class a resource path names: the blob named like the file, exactly,
    // else ignoring ASCII case, else the first.
    const auto stem = path_to_utf8(file.path.stem());
    bool caseless = false;
    for (size_t k = 0; k < names.size(); ++k) {
      const std::string_view simple = std::string_view(names[k]).substr(names[k].rfind('.') + 1);
      if (simple == stem) { primary[i] = k; break; }
      if (!caseless && std::equal(simple.begin(), simple.end(), stem.begin(), stem.end(),
                                  [](char x, char y) { return fold(x) == fold(y); })) {
        primary[i] = k;
        caseless = true;
      }
    }
    classes += names.size();
    containers_[i].path = std::move(file.path);
    containers_[i].base = file.layer == base;
    list.push_back({i, file.layer, std::move(file.rel), std::move(names)});
  }
  index_.build(std::move(list));
  for (const auto& c : index_.containers())
    if (!c.classes.empty()) containers_[c.id].primary = c.classes[primary[c.id]];
  log << "[mods] indexed " << files.size() << " class containers (" << classes << " classes)\n";
  for (const auto& [name, providers] : index_.shared()) {
    const auto* winner = providers.front();
    if (winner->layer == base) continue;  // stock duplicates are not mod conflicts
    std::vector<int> layers;
    std::vector<const slrrc::Container*> files_of_mods;
    bool overrides_base = false;
    for (const auto* c : providers) {
      if (c->layer == base) { overrides_base = true; continue; }
      files_of_mods.push_back(c);
      if (std::find(layers.begin(), layers.end(), c->layer) == layers.end()) layers.push_back(c->layer);
    }
    log << "[mods] " << name << ": mods/" << mods_[winner->layer].folder << " (";
    if (overrides_base) log << "overrides base";
    if (layers.size() > 1) {
      if (overrides_base) log << "; ";
      log << "also in mods/" << mods_[layers[1]].folder;
      for (size_t i = 2; i < layers.size(); ++i) log << ", mods/" << mods_[layers[i]].folder;
    }
    if (files_of_mods.size() != layers.size()) {
      if (overrides_base || layers.size() > 1) log << "; ";
      log << "using " << path_to_utf8(containers_[winner->id].path.lexically_relative(game_));
      for (const auto* c : files_of_mods)
        if (c != winner) log << ", not " << path_to_utf8(containers_[c->id].path.lexically_relative(game_));
    }
    log << ")\n";
  }
}

fs::path ModOverlay::provider(const std::string& fqn) const {
  const auto* c = index_.provider(fqn);
  return c ? containers_[c->id].path : fs::path{};
}

std::string ModOverlay::unique_short_name(const std::string& simple, bool* ambiguous) const {
  return std::string(index_.unique_simple(simple, ambiguous));
}

const ModOverlay::ClassContainer* ModOverlay::container(const fs::path& logical) const {
  std::string rel;
  try {
    rel = path_to_utf8(logical.lexically_normal());
  } catch (const std::exception&) {
    return nullptr;
  }
  const auto* c = index_.at(rel, static_cast<int>(mods_.size()));
  return c ? &containers_[c->id] : nullptr;
}

bool ModOverlay::load(const fs::path& container, std::vector<TufaClass>& blobs, std::string& error) const {
  if (!tufa_load_file(path_to_utf8(container), &blobs, &error)) return false;
  // A file supplies its class identities (and hence its primary class); each
  // blob another container provides is replaced by that definition.
  std::map<size_t, std::vector<TufaClass>> replacements;
  for (auto& blob : blobs) {
    const auto name = blob.name();
    const auto* winner = index_.provider(name);
    if (!winner || containers_[winner->id].path == container) continue;
    const auto& path = containers_[winner->id].path;
    auto [it, fresh] = replacements.try_emplace(winner->id);
    if (fresh && !tufa_load_file(path_to_utf8(path), &it->second, &error)) return false;
    auto chosen = std::find_if(it->second.begin(), it->second.end(), [&](const auto& c) { return c.name() == name; });
    if (chosen == it->second.end()) {
      error = path_to_utf8(path) + " no longer provides " + name;
      return false;
    }
    blob = *chosen;
  }
  return true;
}

fs::path ModOverlay::logical(const fs::path& physical, bool* mod_path) const {
  if (mod_path) *mod_path = false;
  const auto absolute = fs::absolute(physical).lexically_normal();
  auto rel = absolute.lexically_relative(game_);
  if (rel.empty() || *rel.begin() == "..") return absolute;  // external tool input
  auto it = rel.begin();
  if (key(*it++) == "mods" && it != rel.end()) {
    const auto folder = *it++;
    for (const auto& mod : mods_) if (compare_names(folder, path_from_utf8(mod.folder)) == CSTR_EQUAL) {
      fs::path tail;
      for (; it != rel.end(); ++it) tail /= *it;
      if (mod_path) *mod_path = true;
      return tail;
    }
    return {};  // direct path to a disabled or invalid mod
  }
  return rel;
}
}  // namespace slrr
