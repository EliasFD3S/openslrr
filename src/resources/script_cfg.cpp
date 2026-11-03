#include "slrr/script_cfg.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {
namespace {

struct ScriptCfg {
  bool ok = false;
  std::string name;
  std::string file;
  uint32_t render_id = 0;
  uint32_t body_res = 0;
  float body_r = -1.f;
  float trigger_r = -1.f;
  bool noclick = false;
};

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool parse_hex(const std::string& t, uint32_t* out) {
  if (t.size() < 3 || t[0] != '0' || (t[1] != 'x' && t[1] != 'X')) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(t.c_str(), &end, 16);
  if (end == t.c_str()) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool parse_float(const std::string& t, float* out) {
  char* end = nullptr;
  const float v = std::strtof(t.c_str(), &end);
  if (end == t.c_str()) return false;
  *out = v;
  return true;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ';') break;
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

float sphere_radius(const std::vector<std::string>& tok) {
  for (size_t i = 1; i + 1 < tok.size(); ++i) {
    if (tok[i] == "sphere") {
      float r = 0.f;
      if (parse_float(tok[i + 1], &r)) return r;
    }
  }
  return -1.f;
}

bool looks_like_script(const std::string& text) {
  if (text.size() < 3) return false;
  const size_t n = text.size() < 256 ? text.size() : 256;
  for (size_t i = 0; i < n; ++i)
    if (text[i] == '\0') return false;
  return text.find("eof") != std::string::npos;
}

ScriptCfg parse_script_text(const std::string& text) {
  ScriptCfg cfg;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto tok = tokenize(line);
    if (tok.empty()) continue;
    if (tok[0][0] == '#') continue;
    const std::string& cmd = tok[0];
    if (cmd == "eof") {
      cfg.ok = true;
      break;
    }
    if (cmd == "render") {
      if (cfg.render_id == 0) {
        for (size_t i = 1; i < tok.size(); ++i) {
          uint32_t id = 0;
          if (parse_hex(tok[i], &id)) {
            cfg.render_id = id;
            break;
          }
        }
      }
    } else if (cmd == "body") {
      if (tok.size() == 2) {
        uint32_t id = 0;
        if (parse_hex(tok[1], &id)) cfg.body_res = id;
      }
      const float r = sphere_radius(tok);
      if (r >= 0.f) cfg.body_r = r;
    } else if (cmd == "trigger") {
      const float r = sphere_radius(tok);
      if (r >= 0.f) cfg.trigger_r = r;
    } else if (cmd == "name" && tok.size() >= 2) {
      cfg.name = tok[1];
      for (size_t i = 2; i < tok.size(); ++i) {
        cfg.name.push_back(' ');
        cfg.name += tok[i];
      }
    } else if (cmd == "noclick") {
      cfg.noclick = true;
    }
  }
  if (cfg.render_id || cfg.body_res || cfg.body_r >= 0.f ||
      cfg.trigger_r >= 0.f || !cfg.name.empty())
    cfg.ok = true;
  return cfg;
}

int32_t frontend_pack_id(const RpkIndex& rpk) {
  static int32_t id = 0;
  static bool found = false;
  if (found) return id;
  if (rpk.pack_count() <= 0) return 0;
  found = true;
  for (int32_t i = 1; i <= rpk.pack_count(); ++i) {
    const RpkPack* p = rpk.pack_by_id(i);
    if (!p) continue;
    std::string path = p->path;
    for (char& c : path)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (path.find("frontend") != std::string::npos) {
      id = i;
      return id;
    }
  }
  return 0;
}

const std::unordered_map<uint32_t, ScriptCfg>& frontend_scripts(
    const RpkIndex& rpk) {
  static std::unordered_map<uint32_t, ScriptCfg> map;
  static bool built = false;
  if (built) return map;
  if (rpk.game_root().empty()) return map;
  built = true;
  const std::filesystem::path dir =
      std::filesystem::path(rpk.game_root()) / "frontend" / "scripts";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return map;
  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !ent.is_regular_file()) continue;
    std::string ext = ent.path().extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".cfg") continue;
    ScriptCfg cfg = parse_script_text(read_text_file(ent.path()));
    if (!cfg.ok) continue;
    cfg.file = ent.path().filename().string();
    const uint32_t key = cfg.render_id ? cfg.render_id : cfg.body_res;
    if (!key) continue;
    map.emplace(key & 0xFFFFu, std::move(cfg));
  }
  std::printf("[script] frontend/scripts %zu\n", map.size());
  return map;
}

void commit(GameRefState* st, uint32_t full_id, const ScriptCfg& cfg) {
  st->script_render = static_cast<int32_t>(cfg.render_id & 0xFFFFu);
  st->script_body_res = static_cast<int32_t>(cfg.body_res & 0xFFFFu);
  st->script_body_r = cfg.body_r >= 0.f ? cfg.body_r : 0.f;
  st->script_trigger_r = cfg.trigger_r >= 0.f ? cfg.trigger_r : 0.f;
  st->script_noclick = cfg.noclick;
  st->script_name = cfg.name;
  const char* label = !cfg.file.empty() ? cfg.file.c_str()
                      : !cfg.name.empty() ? cfg.name.c_str()
                                          : "-";
  std::printf(
      "[script] 0x%08x %s render=0x%x bodyRes=0x%x bodyR=%.3f trigR=%.3f "
      "noclick=%d name=%s\n",
      full_id, label, st->script_render, st->script_body_res, st->script_body_r,
      st->script_trigger_r, cfg.noclick ? 1 : 0,
      cfg.name.empty() ? "-" : cfg.name.c_str());
}

}  // namespace

void apply_script_cfg(const RpkIndex& rpk, GameRefState* st, uint32_t full_id) {
  if (!st || st->script_parsed || full_id == 0) return;

  const std::vector<uint8_t> bytes = rpk.read_payload(full_id);
  const std::string text(bytes.begin(), bytes.end());
  if (looks_like_script(text)) {
    const ScriptCfg cfg = parse_script_text(text);
    if (cfg.ok) {
      st->script_parsed = true;
      commit(st, full_id, cfg);
      return;
    }
  }

  if (rpk.game_root().empty()) return;
  const int32_t pack = RpkIndex::pack_from_full(full_id);
  const int32_t fe = frontend_pack_id(rpk);
  if (pack != 0 && fe == 0) return;
  st->script_parsed = true;
  if (pack != 0 && fe != 0 && pack != fe) return;

  const auto& idx = frontend_scripts(rpk);
  const auto it = idx.find(full_id & 0xFFFFu);
  if (it == idx.end()) return;
  commit(st, full_id, it->second);
}

}  // namespace slrr
