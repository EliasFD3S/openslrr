#include "slrr/rpk_index.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace slrr {
namespace {

uint32_t rd_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float rd_f32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

std::string norm_key(std::string p) {
  for (char& c : p)
    if (c == '/') c = '\\';
  for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return p;
}

bool path_ends_cfg(const std::string& path) {
  if (path.size() < 4) return false;
  const char* e = path.c_str() + path.size() - 4;
  return (e[0] == '.' ) &&
         (e[1] == 'c' || e[1] == 'C') &&
         (e[2] == 'f' || e[2] == 'F') &&
         (e[3] == 'g' || e[3] == 'G');
}

// Frontend script icons (dollar.cfg, mask.cfg, …) name a mesh via `render 0x…`.
uint32_t cfg_render_id(const std::string& path) {
  std::ifstream in(path);
  if (!in) return 0;
  std::string line;
  while (std::getline(in, line)) {
    const auto semi = line.find(';');
    if (semi != std::string::npos) line.resize(semi);
    std::string low = line;
    for (char& c : low)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto at = low.find("render");
    if (at == std::string::npos) continue;
    const auto hx = low.find("0x", at);
    if (hx == std::string::npos) continue;
    try {
      return static_cast<uint32_t>(std::stoul(line.substr(hx), nullptr, 16));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

}  // namespace

void RpkIndex::set_root(std::string root) { root_ = std::move(root); }

std::string RpkIndex::resolve_disk_path(const std::string& path) const {
  std::string p = path;
  for (char& c : p)
    if (c == '/') c = '\\';
  if (p.size() >= 2 && p[1] == ':') return p;
  std::string full = root_;
  if (!full.empty() && full.back() != '\\' && full.back() != '/') full.push_back('\\');
  full += p;
  return full;
}

std::vector<std::string> RpkIndex::list_rpks(const std::string& dir) const {
  std::vector<std::string> out;
  std::string path = dir;
  for (char& c : path)
    if (c == '/') c = '\\';
  if (!path.empty() && path.back() != '\\') path.push_back('\\');

  const std::string full = resolve_disk_path(path);

#ifdef _WIN32
  const std::string pattern = full + "*.rpk";
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    out.emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)full;
#endif
  return out;
}

bool RpkIndex::parse_rpak(const std::string& disk, RpkPack& pack) const {
  std::ifstream in(disk, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto sz = static_cast<size_t>(in.tellg());
  if (sz < 16) return false;
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(sz));
  if (!in) return false;

  if (sz < 16 || std::memcmp(buf.data(), "RPAK", 4) != 0) {
    std::printf("[rpk] not RPAK: %s\n", disk.c_str());
    return false;
  }
  const uint32_t version = rd_u32(buf.data() + 4);
  if (version != 0x200) {
    std::printf("[rpk] bad version 0x%x %s\n", version, disk.c_str());
    return false;
  }
  pack.version = version;
  const uint32_t dep_a = rd_u32(buf.data() + 8);
  const uint32_t dep_b = rd_u32(buf.data() + 12);
  const uint32_t dep_n = dep_a + dep_b;
  size_t off = 16;
  const size_t dep_bytes = static_cast<size_t>(dep_n) * 64;
  if (off + dep_bytes + 16 > sz) return false;

  pack.deps.clear();
  pack.deps.reserve(dep_n);
  for (uint32_t i = 0; i < dep_n; ++i) {
    const uint8_t* d = buf.data() + off;
    RpkDep dep;
    dep.id = rd_u32(d);
    dep.path.assign(reinterpret_cast<const char*>(d + 4), 56);
    const auto nul = dep.path.find('\0');
    if (nul != std::string::npos) dep.path.resize(nul);
    dep.trailer = rd_u32(d + 60);
    pack.deps.push_back(std::move(dep));
    off += 64;
  }

  const uint32_t table_size = rd_u32(buf.data() + off);
  const uint32_t count = rd_u32(buf.data() + off + 4);
  off += 16;  // tableSize, count, skip(8)
  const size_t table_end = off + table_size;
  if (table_end > sz) return false;

  pack.entries.clear();
  pack.by_type_id.clear();
  pack.entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (off + 23 > table_end) {
      std::printf("[rpk] TOC overrun entry %u %s\n", i, disk.c_str());
      break;
    }
    const uint8_t* e = buf.data() + off;
    RpkEntry ent;
    ent.super_id = rd_u32(e + 0);
    ent.type_id = rd_u32(e + 4);
    ent.typeof_ = e[8];
    ent.flags = e[9];
    (void)rd_f32(e + 10);
    ent.offset = rd_u32(e + 14);
    ent.size = rd_u32(e + 18);
    const uint8_t alias_len = e[22];
    off += 23;
    if (off + alias_len > table_end) break;
    if (alias_len) {
      ent.alias.assign(reinterpret_cast<const char*>(buf.data() + off),
                       alias_len);
      const auto nul = ent.alias.find('\0');
      if (nul != std::string::npos) ent.alias.resize(nul);
      off += alias_len;
    }
    if (ent.flags & 1) {
      if (off + 48 > table_end) break;
      off += 48;  // f32[12]
    }
    pack.by_type_id[ent.type_id] = pack.entries.size();
    pack.entries.push_back(std::move(ent));
  }

  std::printf("[rpk] parsed %s pack=%d deps=%zu entries=%zu\n",
              pack.path.c_str(), pack.pack_id, pack.deps.size(),
              pack.entries.size());
  return true;
}

int32_t RpkIndex::open_all() {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (root_.empty() || !fs::exists(root_, ec)) return 0;
  int32_t n = 0;
  const fs::directory_options opt =
      fs::directory_options::skip_permission_denied;
  for (fs::recursive_directory_iterator it(fs::path(root_), opt, ec), end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    std::string ext = it->path().extension().string();
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".rpk") continue;
    std::error_code rel_ec;
    fs::path rel = fs::relative(it->path(), fs::path(root_), rel_ec);
    open_lib(rel_ec ? it->path().string() : rel.string());
    ++n;
  }
  rpk_set_active(this);
  std::printf("[rpk] open_all %d files, packs=%d entries=%d\n", n, pack_count(),
              total_entries());
  return n;
}

int32_t RpkIndex::open_lib(const std::string& path) {
  const std::string key = norm_key(path);
  auto it = by_path_.find(key);
  if (it != by_path_.end()) return it->second;

  const int32_t id = next_id_++;
  by_path_[key] = id;
  if (static_cast<size_t>(id) >= packs_.size()) packs_.resize(id + 1);

  RpkPack& pack = packs_[static_cast<size_t>(id)];
  pack.pack_id = id;
  pack.path = path;
  const std::string disk = resolve_disk_path(path);
  if (!parse_rpak(disk, pack)) {
    std::printf("[rpk] openLib %s -> %d (no TOC)\n", path.c_str(), id);
  } else {
    std::printf("[rpk] openLib %s -> %d\n", path.c_str(), id);
  }
  return id;
}

const RpkPack* RpkIndex::pack_by_id(int32_t id) const {
  if (id <= 0 || static_cast<size_t>(id) >= packs_.size()) return nullptr;
  return &packs_[static_cast<size_t>(id)];
}

const RpkPack* RpkIndex::pack_by_path(const std::string& path) const {
  auto it = by_path_.find(norm_key(path));
  if (it == by_path_.end()) return nullptr;
  return pack_by_id(it->second);
}

const RpkPack* RpkIndex::pack_by_name(const std::string& pack) const {
  if (pack.empty()) return nullptr;
  if (const RpkPack* p = pack_by_path(pack)) return p;
  std::string with_ext = pack;
  if (with_ext.size() < 4 ||
      with_ext.substr(with_ext.size() - 4) != ".rpk")
    with_ext += ".rpk";
  if (const RpkPack* p = pack_by_path(with_ext)) return p;
  const std::string want = norm_key(with_ext);
  const auto slash = want.find_last_of('\\');
  const std::string base =
      slash == std::string::npos ? want : want.substr(slash + 1);
  for (size_t i = 1; i < packs_.size(); ++i) {
    if (packs_[i].path.empty()) continue;
    std::string key = norm_key(packs_[i].path);
    const auto s = key.find_last_of('\\');
    const std::string b = s == std::string::npos ? key : key.substr(s + 1);
    if (b == base) return &packs_[i];
  }
  return nullptr;
}

namespace {
const RpkIndex* g_active_rpk = nullptr;
int g_pack_misses = 0;
}  // namespace

void rpk_set_active(const RpkIndex* index) { g_active_rpk = index; }

uint32_t rpk_resolve_resource(const std::string& pack, uint32_t local) {
  const uint32_t loc = local & 0xFFFFu;
  if (!g_active_rpk || pack.empty()) return loc;
  if (const RpkPack* p = g_active_rpk->pack_by_name(pack))
    return RpkIndex::make_full_id(p->pack_id, loc);
  if (g_pack_misses < 8) {
    ++g_pack_misses;
    std::printf("[rpk] resource pack miss '%s' local=0x%x\n", pack.c_str(),
                loc);
  }
  return loc;
}

const RpkEntry* RpkIndex::find_entry(int32_t pack_id, uint32_t type_id) const {
  const RpkPack* p = pack_by_id(pack_id);
  if (!p) return nullptr;
  auto it = p->by_type_id.find(type_id);
  if (it != p->by_type_id.end()) return &p->entries[it->second];
  // Fallback: match lo16 only (compiled frontend:0x011Dr often stores local).
  const uint32_t local = type_id & 0xFFFFu;
  for (const auto& e : p->entries) {
    if ((e.type_id & 0xFFFFu) == local) return &e;
  }
  return nullptr;
}

const RpkEntry* RpkIndex::resolve(uint32_t full_id) const {
  const int32_t pid = pack_from_full(full_id);
  const uint32_t local = local_from_full(full_id);
  if (pid > 0) return find_entry(pid, local);
  // Unscoped local: search all packs (last match wins — debug only).
  const RpkEntry* hit = nullptr;
  for (size_t i = 1; i < packs_.size(); ++i) {
    if (const RpkEntry* e = find_entry(static_cast<int32_t>(i), local))
      hit = e;
  }
  return hit;
}

int32_t RpkIndex::total_entries() const {
  int32_t n = 0;
  for (size_t i = 1; i < packs_.size(); ++i)
    n += static_cast<int32_t>(packs_[i].entries.size());
  return n;
}

std::string RpkIndex::disk_path_for_pack(int32_t pack_id) const {
  const RpkPack* p = pack_by_id(pack_id);
  if (!p) return {};
  return resolve_disk_path(p->path);
}

std::vector<uint8_t> RpkIndex::read_payload(uint32_t full_id) const {
  const RpkEntry* e = resolve(full_id);
  if (!e || e->size == 0) return {};
  int32_t pid = pack_from_full(full_id);
  if (pid <= 0) {
    for (size_t i = 1; i < packs_.size(); ++i) {
      for (const auto& ent : packs_[i].entries) {
        if (&ent == e) {
          pid = static_cast<int32_t>(i);
          break;
        }
      }
      if (pid > 0) break;
    }
  }
  const std::string path = disk_path_for_pack(pid);
  if (path.empty()) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(static_cast<std::streamoff>(e->offset));
  std::vector<uint8_t> out(e->size);
  in.read(reinterpret_cast<char*>(out.data()),
          static_cast<std::streamsize>(e->size));
  if (!in) return {};
  return out;
}

namespace {

bool payload_is_text_rsd(const std::vector<uint8_t>& bytes, size_t& end_out) {
  if (bytes.empty()) return false;
  end_out = bytes.size();
  if (bytes.back() == 0) end_out = bytes.size() - 1;
  for (size_t i = 0; i < end_out; ++i) {
    if (bytes[i] == 0) return false;
  }
  return true;
}

std::vector<std::string> parse_rsd_props(const std::string& text,
                                         const char* key) {
  std::vector<std::string> out;
  if (!key || !*key) return out;
  std::string want_l = key;
  for (char& c : want_l)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) line_end = text.size();
    std::string line = text.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto semi = line.find(';');
    if (semi != std::string::npos) line.resize(semi);
    const auto slash = line.find("//");
    if (slash != std::string::npos) line.resize(slash);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (!line.empty()) {
      size_t sp = 0;
      while (sp < line.size() && line[sp] != ' ' && line[sp] != '\t' &&
             line[sp] != '=')
        ++sp;
      std::string k = line.substr(0, sp);
      for (char& c : k)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (k == want_l) {
        size_t v = sp;
        while (v < line.size() &&
               (line[v] == ' ' || line[v] == '\t' || line[v] == '='))
          ++v;
        out.push_back(line.substr(v));
      }
    }
    line_start = line_end + (line_end < text.size() ? 1 : 0);
  }
  return out;
}

std::string basename_lower(std::string p) {
  for (char& c : p)
    if (c == '/') c = '\\';
  const auto slash = p.find_last_of('\\');
  if (slash != std::string::npos) p = p.substr(slash + 1);
  for (char& c : p)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return p;
}

std::string stem_lower(std::string p) {
  p = basename_lower(std::move(p));
  const auto dot = p.find_last_of('.');
  if (dot != std::string::npos) p.resize(dot);
  return p;
}

}  // namespace

uint32_t RpkIndex::resolve_embedded_id(int32_t owner_pack,
                                       uint32_t rid) const {
  const uint32_t local = rid & 0xFFFFu;
  const uint32_t slot = rid >> 16;
  const RpkPack* owner = pack_by_id(owner_pack);
  if (!owner || slot == 0)
    return owner ? make_full_id(owner->pack_id, local) : local;
  // `texture 0x00030022` in frontend.rpk: slot 3 is cars.rpk (dep id 0x30000),
  // local 0x22. The high half is not the host pack id.
  for (const RpkDep& d : owner->deps) {
    if ((d.id >> 16) != slot) continue;
    if (const RpkPack* p = pack_by_name(d.path))
      return make_full_id(p->pack_id, local);
    break;
  }
  return make_full_id(owner->pack_id, local);
}

std::string RpkIndex::payload_prop(uint32_t full_id, const char* key) const {
  const auto all = payload_props(full_id, key);
  return all.empty() ? std::string{} : all.front();
}

std::string RpkIndex::texture_path_for_id(uint32_t id) const {
  auto abs_under_root = [&](std::string file) -> std::string {
    if (file.empty()) return {};
    for (char& c : file)
      if (c == '/') c = '\\';
    while (!file.empty() && (file.front() == '\\' || file.front() == '/'))
      file.erase(file.begin());
    if (file.size() > 1 && file[1] == ':') return file;
    if (root_.empty()) return file;
    std::string out = root_;
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
    out += file;
    return out;
  };

  std::function<std::string(uint32_t, int)> follow =
      [&](uint32_t fid, int depth) -> std::string {
    if (depth > 4) return {};
    std::string src = payload_prop(fid, "sourcefile");
    if (!src.empty()) {
      std::string path = abs_under_root(std::move(src));
      if (path_ends_cfg(path)) {
        const uint32_t rid = cfg_render_id(path);
        if (rid) {
          const uint32_t next =
              resolve_embedded_id(pack_from_full(fid), rid);
          std::string tex = follow(next, depth + 1);
          if (!tex.empty()) return tex;
        }
      }
      return path;
    }
    const std::string tex = payload_prop(fid, "texture");
    if (tex.empty()) return {};
    if (tex.size() > 2 && tex[0] == '0' && (tex[1] == 'x' || tex[1] == 'X')) {
      uint32_t rid = 0;
      try {
        rid = static_cast<uint32_t>(std::stoul(tex, nullptr, 16));
      } catch (...) {
        return {};
      }
      rid = resolve_embedded_id(pack_from_full(fid), rid);
      return follow(rid, depth + 1);
    }
    return abs_under_root(tex);
  };

  // Bare local ids (OSD icons, stripes) collide across packs — prefer
  // frontend.rpk before a generic resolve (e.g. 0x4C = arrowUp vs garage).
  if ((id >> 16) == 0) {
    if (const RpkPack* fe = pack_by_path("frontend.rpk")) {
      const uint32_t full = make_full_id(fe->pack_id, id & 0xFFFFu);
      if (resolve(full)) {
        std::string path = follow(full, 0);
        if (!path.empty()) return path;
      }
    }
  }
  if (resolve(id)) return follow(id, 0);
  return {};
}

std::vector<std::string> RpkIndex::payload_props(uint32_t full_id,
                                                 const char* key) const {
  const auto bytes = read_payload(full_id);
  size_t end = 0;
  if (!payload_is_text_rsd(bytes, end)) return {};
  return parse_rsd_props(
      std::string(reinterpret_cast<const char*>(bytes.data()), end), key);
}

std::vector<std::string> RpkIndex::textures_for_scx(
    const std::string& scx_path) const {
  const std::string want = stem_lower(scx_path);
  if (want.empty()) return {};

  auto resolve_tex_paths = [&](int32_t pack_id,
                               const std::vector<std::string>& tex_refs) {
    std::vector<std::string> paths;
    paths.reserve(tex_refs.size());
    for (const std::string& ref : tex_refs) {
      if (ref.size() > 2 && ref[0] == '0' &&
          (ref[1] == 'x' || ref[1] == 'X')) {
        uint32_t rid =
            static_cast<uint32_t>(std::strtoul(ref.c_str(), nullptr, 16));
        rid = resolve_embedded_id(pack_id, rid);
        std::string file = payload_prop(rid, "sourcefile");
        // Prefer co-located .dds when RSD points at .png/.tga.
        if (file.size() > 4) {
          std::string dds = file;
          const auto dot = dds.find_last_of('.');
          if (dot != std::string::npos) {
            std::string ext = dds.substr(dot);
            for (char& c : ext)
              c = static_cast<char>(
                  std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".dds") {
              dds.resize(dot);
              dds += ".dds";
              auto png = dds.find("\\png\\");
              if (png == std::string::npos) png = dds.find("/png/");
              if (png != std::string::npos) dds.erase(png + 1, 4);
              if(std::filesystem::exists(std::filesystem::path(root_)/dds))
                file = std::move(dds);
            }
          }
        }
        paths.push_back(std::move(file));
      } else {
        paths.push_back(ref);
      }
    }
    return paths;
  };

  auto entry_source_stem = [&](int32_t pack_id, uint32_t local_or_full) {
    const uint32_t full = resolve_embedded_id(pack_id, local_or_full);
    return stem_lower(payload_prop(full, "sourcefile"));
  };

  for (size_t pi = 1; pi < packs_.size(); ++pi) {
    const int32_t pack_id = static_cast<int32_t>(pi);
    const RpkPack& pack = packs_[pi];
    for (const RpkEntry& ent : pack.entries) {
      const uint32_t full = make_full_id(pack_id, ent.type_id);
      const std::vector<std::string> tex_refs = payload_props(full, "texture");
      if (tex_refs.empty()) continue;

      // Render/parent entries carry `texture` + `mesh`/`render`; mesh leaf
      // carries `sourcefile` only. Match either.
      std::string src_stem = stem_lower(payload_prop(full, "sourcefile"));
      if (src_stem != want) {
        for (const char* link : {"mesh", "render"}) {
          const std::string next = payload_prop(full, link);
          if (next.size() > 2 && next[0] == '0' &&
              (next[1] == 'x' || next[1] == 'X')) {
            const uint32_t rid =
                static_cast<uint32_t>(std::strtoul(next.c_str(), nullptr, 16));
            src_stem = entry_source_stem(pack_id, rid);
            if (src_stem == want) break;
          }
        }
      }
      if (src_stem != want) continue;

      std::vector<std::string> paths = resolve_tex_paths(pack_id, tex_refs);
      if (!paths.empty()) {
        std::printf("[rpk] textures_for_scx %s -> %zu slots (pack %d entry "
                    "0x%x)\n",
                    scx_path.c_str(), paths.size(), pack_id, ent.type_id);
        return paths;
      }
    }
  }
  return {};
}

RpkIndex::ModelRef RpkIndex::model_for_resource(int32_t pack_id,
                                                uint32_t local_id) const {
  ModelRef out;
  const RpkEntry* ent = find_entry(pack_id, local_id);
  if (!ent) return out;
  out.type_id = ent->type_id & 0xffffu;
  out.alias = ent->alias;
  const uint32_t full = make_full_id(pack_id, local_id);

  auto prefer_dds = [this](std::string file) {
    const std::string original=file;
    if (file.size() <= 4) return file;
    const auto dot = file.find_last_of('.');
    if (dot == std::string::npos) return file;
    std::string ext = file.substr(dot);
    for (char& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".dds") return file;
    file.resize(dot);
    file += ".dds";
    auto png = file.find("\\png\\");
    if (png == std::string::npos) png = file.find("/png/");
    if (png != std::string::npos) file.erase(png + 1, 4);
    return std::filesystem::exists(std::filesystem::path(root_)/file)?file:original;
  };

  auto resolve_tex = [&](const std::vector<std::string>& refs) {
    std::vector<std::string> paths;
    for (const std::string& ref : refs) {
      if (ref.size() > 2 && ref[0] == '0' &&
          (ref[1] == 'x' || ref[1] == 'X')) {
        uint32_t rid =
            static_cast<uint32_t>(std::strtoul(ref.c_str(), nullptr, 16));
        rid = resolve_embedded_id(pack_id, rid);
        paths.push_back(prefer_dds(payload_prop(rid, "sourcefile")));
      } else {
        paths.push_back(prefer_dds(ref));
      }
    }
    return paths;
  };

  std::string src = payload_prop(full, "sourcefile");
  std::vector<std::string> tex_refs = payload_props(full, "texture");
  if (src.empty()) {
    for (const char* link : {"mesh", "render"}) {
      const std::string next = payload_prop(full, link);
      if (next.size() > 2 && next[0] == '0' &&
          (next[1] == 'x' || next[1] == 'X')) {
        uint32_t rid =
            static_cast<uint32_t>(std::strtoul(next.c_str(), nullptr, 16));
        rid = resolve_embedded_id(pack_id, rid);
        src = payload_prop(rid, "sourcefile");
        if (tex_refs.empty()) tex_refs = payload_props(rid, "texture");
        if (!src.empty()) break;
      }
    }
  }
  if (src.empty()) return out;
  out.source = src;
  out.textures = resolve_tex(tex_refs);
  out.ok = true;
  std::printf("[rpk] model 0x%x '%s' src=%s tex=%zu\n", out.type_id,
              out.alias.c_str(), out.source.c_str(), out.textures.size());
  return out;
}

std::vector<uint32_t> RpkIndex::garage_render_ids(int32_t pack_id,
                                                  int garage_index) const {
  std::vector<uint32_t> ids;
  const RpkPack* pack = pack_by_id(pack_id);
  if (!pack) return ids;
  static const char* kFolders[] = {"g1", "g2", "g3", "g4"};
  if (garage_index < 0 || garage_index > 3) garage_index = 0;
  const char* folder = kFolders[garage_index];

  // Folder type-14 node (alias g1) → children type-14 with mesh (map + stuff).
  uint32_t folder_id = 0;
  for (const RpkEntry& e : pack->entries) {
    if (e.typeof_ == 14 && e.alias == folder) {
      folder_id = e.type_id;
      break;
    }
  }
  if (!folder_id) return ids;
  for (const RpkEntry& e : pack->entries) {
    if (e.typeof_ != 14 || e.super_id != folder_id) continue;
    const uint32_t full = make_full_id(pack_id, e.type_id);
    const std::string mesh = payload_prop(full, "mesh");
    if (mesh.empty()) continue;
    ids.push_back(e.type_id & 0xffffu);
  }
  std::printf("[rpk] garage[%d] folder=%s renders=%zu\n", garage_index, folder,
              ids.size());
  return ids;
}

}  // namespace slrr
