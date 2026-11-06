#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace slrr {
namespace {
// StreetLegal can hold save/temp/CopyCar open. The selector's back-to-first
// page saves, destroys, then loads that path; a failed disk write must still
// be what the following read sees.
std::unordered_map<std::string, std::vector<uint8_t>> g_unflushed_saves;

std::string save_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto canon = std::filesystem::weakly_canonical(path, ec);
  return (ec ? path : canon).lexically_normal().string();
}
}  // namespace

namespace {

void push_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

std::string sdat_pack_path(const RpkIndex& rpk, std::string path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path as_path(path);
  if (as_path.is_absolute()) {
    const fs::path rel = fs::relative(as_path, fs::path(rpk.game_root()), ec);
    if (!ec) path = rel.string();
  }
  while (path.size() >= 2 && path[0] == '.' &&
         (path[1] == '/' || path[1] == '\\'))
    path.erase(0, 2);
  for (char& c : path)
    if (c == '/') c = '\\';
  return path;
}

int32_t sdat_resolve_pack(RpkIndex& rpk, const std::string& path) {
  if (path.empty()) return 0;
  if (const RpkPack* pack = rpk.pack_by_name(path)) return pack->pack_id;
  return rpk.open_lib(path);
}

// SDAT magic with a trailer that does not parse. Open fails closed.
constexpr int kSdatCorrupt = -1;

// Replace an SDAT file with its Java payload and the remount tables.
// Returns 1 when the envelope was stripped, 0 when the bytes are a raw
// payload, and kSdatCorrupt when the magic is SDAT but the trailer is not.
int sdat_take_payload(RpkIndex& rpk, std::vector<uint8_t>& bytes,
                      std::vector<SdatPackRef>& packs,
                      std::vector<SdatBlobRef>& blobs) {
  packs.clear();
  blobs.clear();
  if (bytes.size() < 8 || bytes[0] != 'S' || bytes[1] != 'D' ||
      bytes[2] != 'A' || bytes[3] != 'T')
    return 0;
  const uint16_t version = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
  const uint8_t type = bytes[6];
  if (version != 0x0100 || (type != 1 && type != 2)) return kSdatCorrupt;

  auto fail = [&]() {
    bytes.clear();
    packs.clear();
    blobs.clear();
    return kSdatCorrupt;
  };
  size_t start = 0;
  if (type == 1) {
    if (bytes.size() < 16) return fail();
    size_t tail = bytes.size() - 8;
    uint32_t span = 0, count = 0;
    if (!rd_u32(bytes, tail, &span) || !rd_u32(bytes, tail, &count))
      return fail();
    if (span > bytes.size() - 8) return fail();
    start = bytes.size() - 8 - span;
    if (start < 8) return fail();
    size_t cursor = start;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t ordinal = 0, plen = 0;
      if (!rd_u32(bytes, cursor, &ordinal) || !rd_u32(bytes, cursor, &plen) ||
          plen > 1024 || cursor + plen > start + span)
        return fail();
      std::string path(reinterpret_cast<const char*>(bytes.data() + cursor),
                       plen);
      cursor += plen;
      while (!path.empty() && path.back() == '\0') path.pop_back();
      packs.push_back(
          {static_cast<int32_t>(ordinal), sdat_resolve_pack(rpk, path),
           std::move(path)});
    }
    if (cursor != start + span) return fail();
  } else {
    if (bytes.size() < 12) return fail();
    size_t tail = bytes.size() - 4;
    uint32_t span = 0;
    if (!rd_u32(bytes, tail, &span) || span < 8 || span > bytes.size() - 4)
      return fail();
    start = bytes.size() - 4 - span;
    if (start < 8) return fail();
    size_t cursor = start;
    uint32_t unsaved = 0;
    if (!rd_u32(bytes, cursor, &unsaved) || unsaved > 100000) return fail();
    for (uint32_t i = 0; i < unsaved; ++i) {
      uint32_t ordinal = 0, kind = 0, size = 0;
      if (!rd_u32(bytes, cursor, &ordinal) || !rd_u32(bytes, cursor, &kind) ||
          !rd_u32(bytes, cursor, &size) || cursor > start + span ||
          size > start + span - cursor)
        return fail();
      SdatBlobRef blob;
      blob.ordinal = static_cast<int32_t>(ordinal);
      blob.kind = static_cast<int32_t>(kind);
      blob.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                        bytes.begin() +
                            static_cast<std::ptrdiff_t>(cursor + size));
      cursor += size;
      blobs.push_back(std::move(blob));
    }
    uint32_t count = 0;
    if (!rd_u32(bytes, cursor, &count) || count > 100000) return fail();
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t ordinal = 0, plen = 0;
      if (!rd_u32(bytes, cursor, &ordinal) || !rd_u32(bytes, cursor, &plen) ||
          plen > 1024 || cursor + plen > start + span)
        return fail();
      std::string path(reinterpret_cast<const char*>(bytes.data() + cursor),
                       plen);
      cursor += plen;
      while (!path.empty() && path.back() == '\0') path.pop_back();
      packs.push_back(
          {static_cast<int32_t>(ordinal), sdat_resolve_pack(rpk, path),
           std::move(path)});
    }
    if (cursor != start + span) return fail();
  }
  bytes.assign(bytes.begin() + 8, bytes.begin() + static_cast<std::ptrdiff_t>(start));
  return 1;
}

std::vector<uint8_t> sdat_frame(const std::vector<uint8_t>& payload,
                                const std::vector<SdatPackRef>& packs,
                                const std::vector<SdatBlobRef>& blobs) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + 64);
  const uint8_t header[8] = {'S', 'D', 'A', 'T', 0x00, 0x01, 0x02, 0x00};
  out.insert(out.end(), header, header + 8);
  out.insert(out.end(), payload.begin(), payload.end());
  const size_t mark = out.size();
  push_u32(out, static_cast<uint32_t>(blobs.size()));
  for (const SdatBlobRef& blob : blobs) {
    push_u32(out, static_cast<uint32_t>(blob.ordinal));
    push_u32(out, static_cast<uint32_t>(blob.kind));
    push_u32(out, static_cast<uint32_t>(blob.bytes.size()));
    out.insert(out.end(), blob.bytes.begin(), blob.bytes.end());
  }
  push_u32(out, static_cast<uint32_t>(packs.size()));
  for (const SdatPackRef& pack : packs) {
    push_u32(out, static_cast<uint32_t>(pack.ordinal));
    const uint32_t plen =
        pack.path.empty() ? 0u : static_cast<uint32_t>(pack.path.size() + 1);
    push_u32(out, plen);
    if (plen > 0) {
      out.insert(out.end(), pack.path.begin(), pack.path.end());
      out.push_back(0);
    }
  }
  push_u32(out, static_cast<uint32_t>(out.size() - mark));
  return out;
}

// File-local id for write(ResourceRef). A null ref is 0. A pack resource
// becomes (ordinal << 16) | local. An unsaved id (high word 0xFFFF, type
// 5 or 7) becomes its session ordinal, with an empty blob in the trailer.
bool sdat_wire_resource(RpkIndex& rpk, HostFile& file, int32_t host_id,
                        int32_t restype, int32_t* wired) {
  if (!wired) return false;
  if (host_id == 0) {
    *wired = 0;
    return true;
  }
  const uint32_t uid = static_cast<uint32_t>(host_id);
  if ((uid & 0xFFFF0000u) == 0xFFFF0000u) {
    if (restype != 5 && restype != 7) return false;
    for (const SdatBlobRef& blob : file.sdat_blobs) {
      if (blob.abs_id == host_id) {
        *wired = blob.ordinal;
        return true;
      }
    }
    SdatBlobRef blob;
    blob.ordinal = static_cast<int32_t>(file.sdat_blobs.size() + 1);
    blob.kind = restype;
    blob.abs_id = host_id;
    *wired = blob.ordinal;
    file.sdat_blobs.push_back(std::move(blob));
    return true;
  }
  const int32_t pack_id = static_cast<int32_t>((uid >> 16) & 0xFFFF);
  const RpkPack* pack = pack_id > 0 ? rpk.pack_by_id(pack_id) : nullptr;
  if (!pack) return false;
  for (const SdatPackRef& row : file.sdat_packs) {
    if (row.pack_id == pack->pack_id) {
      *wired = (row.ordinal << 16) | static_cast<int32_t>(uid & 0xFFFF);
      return true;
    }
  }
  SdatPackRef row;
  row.ordinal = static_cast<int32_t>(file.sdat_packs.size() + 1);
  row.pack_id = pack->pack_id;
  row.path = sdat_pack_path(rpk, pack->path);
  if (row.path.empty() || row.ordinal >= 0xFFFF) return false;
  *wired = (row.ordinal << 16) | static_cast<int32_t>(uid & 0xFFFF);
  file.sdat_packs.push_back(std::move(row));
  return true;
}

int32_t sdat_remap_resource(const HostFile& file, uint32_t wired) {
  if (!file.sdat) return static_cast<int32_t>(wired);
  if (wired == 0) return 0;
  const uint32_t hi = wired & 0xFFFF0000u;
  if (hi != 0 && hi != 0xFFFF0000u) {
    const int32_t ordinal = static_cast<int32_t>(hi >> 16);
    for (const SdatPackRef& row : file.sdat_packs) {
      if (row.ordinal == ordinal && row.pack_id > 0)
        return static_cast<int32_t>(wired & 0xFFFF) | (row.pack_id << 16);
    }
    return 0;
  }
  const int32_t ordinal = static_cast<int32_t>(wired & 0xFFFF);
  for (const SdatBlobRef& blob : file.sdat_blobs) {
    if (blob.ordinal == ordinal && blob.abs_id != 0) return blob.abs_id;
  }
  return 0;
}

}  // namespace

void register_io_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  jvm.natives().register_method(
      "java.render.Frontend", "setFonts", "()V",
      [&jvm](Object*, const std::vector<Value>&) {
        // Stock does not rescale live: a new mode is stored and applied on
        // the next launch. Rebanding for Config.video_* while the window is
        // still the old size breaks the menus.
        const int32_t fb_w = osd_framebuffer_width();
        const int32_t fb_h = osd_framebuffer_height();
        Value vx = jvm.get_static("java.game.Config", "video_x");
        Value vy = jvm.get_static("java.game.Config", "video_y");
        const int32_t cfg_x = vx.tag == ValueTag::Int ? vx.v.i : 0;
        const int32_t cfg_y = vy.tag == ValueTag::Int ? vy.v.i : 0;
        if (fb_w > 2 && fb_h > 2 && (cfg_x != fb_w || cfg_y != fb_h) &&
            cfg_x > 0 && cfg_y > 0) {
          frontend_set_fonts(jvm, fb_w, fb_h);
          jvm.set_static("java.game.Config", "video_x", Value::make_int(cfg_x));
          jvm.set_static("java.game.Config", "video_y", Value::make_int(cfg_y));
          jvm.set_static("java.util.Config", "video_x", Value::make_int(cfg_x));
          jvm.set_static("java.util.Config", "video_y", Value::make_int(cfg_y));
          return Value::make_void();
        }
        const int32_t x = fb_w > 2 ? fb_w : (cfg_x > 0 ? cfg_x : 800);
        const int32_t y = fb_h > 2 ? fb_h : (cfg_y > 0 ? cfg_y : 600);
        frontend_set_fonts(jvm, x, y);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.System", "buildNumber", "()I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(606);
      });
  jvm.natives().register_method(
      "java.lang.System", "log", "(Ljava.lang.String;)V",
      [](Object*, const std::vector<Value>& args) {
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          std::printf("[script] %s\n", obj_str(args[0].v.o).c_str());
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.System", "exit", "()V",
      [&jvm](Object*, const std::vector<Value>&) {
        std::printf("[native] System.exit\n");
        jvm.set_static("java.lang.System", "_hostQuit", Value::make_int(1));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.System", "exit", "(Ljava.lang.String;)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        std::printf("[native] System.exit(%s)\n",
                    args.empty() ? "" : obj_str(args[0].v.o).c_str());
        jvm.set_static("java.lang.System", "_hostQuit", Value::make_int(1));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.System", "currentTime", "()F",
      [](Object*, const std::vector<Value>&) {
        return Value::make_float(now_seconds());
      });
  jvm.natives().register_method(
      "java.lang.System", "simTime", "()F",
      [](Object*, const std::vector<Value>&) {
        return Value::make_float(now_seconds());
      });
  jvm.natives().register_method(
      "java.lang.System", "timeWarp", "(F)F",
      [](Object*, const std::vector<Value>& args) {
        if (!args.empty() && args[0].tag == ValueTag::Float) return args[0];
        return Value::make_float(1.f);
      });
  jvm.natives().register_method(
      "java.lang.System", "openLib", "(Ljava.lang.String;)I",
      [&rpk](Object*, const std::vector<Value>& args) {
        std::string path;
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          path = obj_str(args[0].v.o);
        return Value::make_int(rpk.open_lib(path));
      });
  jvm.natives().register_method(
      "java.lang.System", "rpkScan", "(Ljava.lang.String;)I",
      [&jvm, &rpk](Object*, const std::vector<Value>& args) {
        std::string dir;
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          dir = obj_str(args[0].v.o);
        const auto names = rpk.list_rpks(dir);
        int32_t n = 0;
        for (const auto& name : names) {
          rpk.open_lib(dir + name);
          ++n;
        }
        std::printf("[native] System.rpkScan(%s) -> %d\n", dir.c_str(), n);
        (void)jvm;
        return Value::make_int(n);
      });
  jvm.natives().register_method(
      "java.lang.System", "isLoading", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(0); });
  // Loader-thread priority. Init calls it from LoadingScreen.show; the host
  // has one thread, so the hint is a no-op. Leaving it unbound aborted Init
  // after the boot intros and the window closed with no section.
  jvm.natives().register_method(
      "java.lang.System", "setLdPriority", "(I)V", native_noop);
  jvm.natives().register_method(
      "java.lang.System", "isLoadingReset", "()V", native_noop);
  // PE System.arraycopy: copy `length` slots. Same array uses a temp so
  // overlapping ranges (Vector.removeElementAt) stay intact.
  jvm.natives().register_method(
      "java.lang.System", "arraycopy",
      "(Ljava.lang.Object;ILjava.lang.Object;II)V",
      [](Object*, const std::vector<Value>& args) {
        auto as_obj = [](const Value& v) -> Object* {
          return v.tag == ValueTag::Obj ? v.v.o : nullptr;
        };
        auto as_i = [](const Value& v) -> int32_t {
          if (v.tag == ValueTag::Int) return v.v.i;
          if (v.tag == ValueTag::Float) return static_cast<int32_t>(v.v.f);
          return 0;
        };
        if (args.size() < 5) return Value::make_void();
        Object* src = as_obj(args[0]);
        const int32_t src_pos = as_i(args[1]);
        Object* dst = as_obj(args[2]);
        const int32_t dst_pos = as_i(args[3]);
        const int32_t length = as_i(args[4]);
        if (!src || !dst || !src->is_array || !dst->is_array || length <= 0)
          return Value::make_void();
        if (src_pos < 0 || dst_pos < 0) return Value::make_void();
        if (src_pos > static_cast<int32_t>(src->slots.size()) ||
            length > static_cast<int32_t>(src->slots.size()) - src_pos)
          return Value::make_void();
        const int32_t need = dst_pos + length;
        if (static_cast<int32_t>(dst->slots.size()) < need) {
          dst->slots.resize(static_cast<size_t>(need), Value::make_obj(nullptr));
          dst->fields["length"] = Value::make_int(need);
        }
        std::vector<Value> tmp(
            src->slots.begin() + src_pos,
            src->slots.begin() + src_pos + length);
        for (int32_t i = 0; i < length; ++i)
          dst->slots[static_cast<size_t>(dst_pos + i)] = tmp[static_cast<size_t>(i)];
        return Value::make_void();
      });

  auto findfile_first = [&jvm](Object* self, const std::vector<Value>& args,
                               int32_t flags) -> Value {
    if (!self) return Value::make_obj(nullptr);
    std::string pattern;
    if (!args.empty() && args[0].tag == ValueTag::Obj)
      pattern = obj_str(args[0].v.o);
    if (args.size() >= 2) {
      if (args[1].tag == ValueTag::Int) flags = args[1].v.i;
      else if (args[1].tag == ValueTag::Float)
        flags = static_cast<int32_t>(args[1].v.f);
    }
    findfile_start(jvm, self, pattern, flags);
    if (self->find_entries.empty()) return Value::make_obj(nullptr);
    return Value::make_obj(jvm.new_string(self->find_entries[0]));
  };
  // Java first(String) forwards to first(path, FILES_DIRS). MWM calls the
  // 2-arg native directly (FILES_ONLY).
  jvm.natives().register_method(
      "java.io.FindFile", "first", "(Ljava.lang.String;)Ljava.lang.String;",
      [findfile_first](Object* self, const std::vector<Value>& args) {
        return findfile_first(self, args, 0);
      });
  jvm.natives().register_method(
      "java.io.FindFile", "first",
      "(Ljava.lang.String;I)Ljava.lang.String;",
      [findfile_first](Object* self, const std::vector<Value>& args) {
        return findfile_first(self, args, 0);
      });
  jvm.natives().register_method(
      "java.io.FindFile", "next", "()Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_obj(nullptr);
        ++self->find_index;
        if (self->find_index >= self->find_entries.size())
          return Value::make_obj(nullptr);
        return Value::make_obj(
            jvm.new_string(self->find_entries[self->find_index]));
      });
  jvm.natives().register_method(
      "java.io.FindFile", "close", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (self) {
          self->find_entries.clear();
          self->find_index = 0;
        }
        return Value::make_void();
      });

  jvm.natives().register_method(
      "java.io.File", "delete", "(Ljava.lang.String;)I",
      [&jvm](Object*, const std::vector<Value>& args) {
        namespace fs = std::filesystem;
        std::string rel;
        if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o)
          rel = args[0].v.o->str;
        while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\'))
          rel.pop_back();
        const fs::path p = under_game(jvm.game_root(), rel);
        if (p.empty()) return Value::make_int(0);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
          g_unflushed_saves.erase(save_key(p));
          return Value::make_int(1);
        }
        fs::remove_all(p, ec);
        if (!ec) g_unflushed_saves.erase(save_key(p));
        std::printf("[native] File.delete %s ok=%d\n", rel.c_str(), ec ? 0 : 1);
        return Value::make_int(ec ? 0 : 1);
      });
  jvm.natives().register_method(
      "java.io.File", "exists", "(Ljava.lang.String;)I",
      [&jvm](Object*, const std::vector<Value>& args) {
        std::string rel;
        if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o)
          rel = args[0].v.o->str;
        const auto p = under_game(jvm.game_root(), rel);
        std::error_code ec;
        const bool ok = !p.empty() && std::filesystem::exists(p, ec);
        return Value::make_int(ok ? 1 : 0);
      });
  // PE File.copy / File.move: 0 on success, -1 on failure. Paths are
  // game-relative, same root as exists/delete.
  auto two_paths = [&jvm](const std::vector<Value>& args, std::filesystem::path* a,
                          std::filesystem::path* b) {
    std::string sa, sb;
    if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o)
      sa = args[0].v.o->str;
    if (args.size() > 1 && args[1].tag == ValueTag::Obj && args[1].v.o)
      sb = args[1].v.o->str;
    *a = under_game(jvm.game_root(), sa);
    *b = under_game(jvm.game_root(), sb);
    return !a->empty() && !b->empty();
  };
  jvm.natives().register_method(
      "java.io.File", "copy", "(Ljava.lang.String;Ljava.lang.String;)I",
      [two_paths](Object*, const std::vector<Value>& args) {
        namespace fs = std::filesystem;
        fs::path src, dst;
        if (!two_paths(args, &src, &dst)) return Value::make_int(-1);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return Value::make_int(ec ? -1 : 0);
      });
  jvm.natives().register_method(
      "java.io.File", "move", "(Ljava.lang.String;Ljava.lang.String;)I",
      [two_paths](Object*, const std::vector<Value>& args) {
        namespace fs = std::filesystem;
        fs::path src, dst;
        if (!two_paths(args, &src, &dst)) return Value::make_int(-1);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::rename(src, dst, ec);
        return Value::make_int(ec ? -1 : 0);
      });
  jvm.natives().register_method(
      "java.io.File", "open", "(I)I",
      [&jvm, &rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_int(0);
        const int mode =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        const auto p = under_game(jvm.game_root(), file_name_of(self));
        if (p.empty()) return Value::make_int(0);
        HostFile hf;
        hf.path = p;
        hf.writing = mode != 0;
        if (hf.writing) {
          // Payload only. close() writes the SDAT type 2 envelope.
          hf.sdat = true;
        } else {
          // Specifying a mode replaces the ifstream default. Without in,
          // MSVC does not open the file.
          std::ifstream in(p, std::ios::in | std::ios::binary);
          const auto kept = g_unflushed_saves.find(save_key(p));
          if (kept != g_unflushed_saves.end())
            hf.bytes = kept->second;
          else if (in)
            hf.bytes.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
          else
            return Value::make_int(0);
          const int shaped =
              sdat_take_payload(rpk, hf.bytes, hf.sdat_packs, hf.sdat_blobs);
          if (shaped < 0) return Value::make_int(0);
          hf.sdat = shaped > 0;
        }
        g_host_files[self] = std::move(hf);
        return Value::make_int(1);
      });
  jvm.natives().register_method(
      "java.io.File", "close", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_void();
        auto it = g_host_files.find(self);
        if (it != g_host_files.end() && it->second.writing &&
            !it->second.path.empty()) {
          namespace fs = std::filesystem;
          std::error_code ec;
          fs::create_directories(it->second.path.parent_path(), ec);
          // Mode replaces the ofstream default. Without out, MSVC does not
          // open the file and the career buffer never reaches disk.
          std::ofstream out(it->second.path, std::ios::out | std::ios::binary |
                                                 std::ios::trunc);
          const std::vector<uint8_t> framed =
              it->second.sdat ? sdat_frame(it->second.bytes, it->second.sdat_packs,
                                           it->second.sdat_blobs)
                              : it->second.bytes;
          const bool wrote = static_cast<bool>(out);
          if (wrote && !framed.empty())
            out.write(reinterpret_cast<const char*>(framed.data()),
                      static_cast<std::streamsize>(framed.size()));
          const std::string key = save_key(it->second.path);
          if (wrote && out)
            g_unflushed_saves.erase(key);
          else
            g_unflushed_saves[key] = framed;
          std::printf(
              "[native] File.close %s %zu bytes ok=%d kept=%d sdat=%d packs=%zu\n",
              it->second.path.string().c_str(), framed.size(),
              wrote && static_cast<bool>(out) ? 1 : 0,
              g_unflushed_saves.count(key) ? 1 : 0, it->second.sdat ? 1 : 0,
              it->second.sdat_packs.size());
          part_note_live_save(it->second.path.string());
        }
        g_host_files.erase(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.io.File", "write", "(I)I",
      [](Object* self, const std::vector<Value>& args) {
        int32_t v = 0;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Int) v = args[0].v.i;
          else if (args[0].tag == ValueTag::Float)
            v = static_cast<int32_t>(args[0].v.f);
        }
        if (!file_push(self, &v, 4)) return Value::make_int(0);
        return Value::make_int(4);
      });
  jvm.natives().register_method(
      "java.io.File", "write", "(F)I",
      [](Object* self, const std::vector<Value>& args) {
        float v = 0.f;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Float) v = args[0].v.f;
          else if (args[0].tag == ValueTag::Int)
            v = static_cast<float>(args[0].v.i);
        }
        if (!file_push(self, &v, 4)) return Value::make_int(0);
        return Value::make_int(4);
      });
  jvm.natives().register_method(
      "java.io.File", "write", "(Ljava.util.resource.ResourceRef;)I",
      [&rpk](Object* self, const std::vector<Value>& args) {
        Object* ref =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        const int32_t id = res_id_of(ref);
        auto it = g_host_files.find(self);
        if (it == g_host_files.end() || !it->second.writing)
          return Value::make_int(0);
        int32_t restype = 0;
        if (ref) {
          auto type = ref->fields.find("typeof");
          if (type != ref->fields.end() && type->second.tag == ValueTag::Int)
            restype = type->second.v.i;
        }
        int32_t wired = id;
        if (it->second.sdat &&
            !sdat_wire_resource(rpk, it->second, id, restype, &wired)) {
          std::printf("[sdat] resource 0x%08x has no pack\n",
                      static_cast<uint32_t>(id));
          return Value::make_int(0);
        }
        if (!file_push(self, &wired, 4)) return Value::make_int(0);
        return Value::make_int(4);
      });
  jvm.natives().register_method(
      "java.io.File", "write", "(Ljava.lang.String;)I",
      [](Object* self, const std::vector<Value>& args) {
        Object* s =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        const char* c = (s && !s->str.empty()) ? s->str.c_str() : nullptr;
        const int32_t len = c ? static_cast<int32_t>(std::strlen(c) + 1) : 0;
        if (!file_push(self, &len, 4)) return Value::make_int(0);
        if (len <= 0) return Value::make_int(0);
        if (!file_push(self, c, static_cast<size_t>(len)))
          return Value::make_int(-1);
        return Value::make_int(len);
      });
  jvm.natives().register_method(
      "java.io.File", "readInt", "()I",
      [](Object* self, const std::vector<Value>&) {
        uint32_t v = 0;
        auto it = g_host_files.find(self);
        if (it != g_host_files.end()) rd_u32(it->second.bytes, it->second.pos, &v);
        return Value::make_int(static_cast<int32_t>(v));
      });
  jvm.natives().register_method(
      "java.io.File", "readFloat", "()F",
      [](Object* self, const std::vector<Value>&) {
        float v = 0.f;
        auto it = g_host_files.find(self);
        if (it != g_host_files.end()) rd_f32(it->second.bytes, it->second.pos, &v);
        return Value::make_float(v);
      });
  jvm.natives().register_method(
      "java.io.File", "readResID", "()I",
      [](Object* self, const std::vector<Value>&) {
        uint32_t v = 0;
        auto it = g_host_files.find(self);
        if (it == g_host_files.end() ||
            !rd_u32(it->second.bytes, it->second.pos, &v))
          return Value::make_int(0);
        return Value::make_int(sdat_remap_resource(it->second, v));
      });
  jvm.natives().register_method(
      "java.io.File", "readString", "()Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>&) {
        auto it = g_host_files.find(self);
        if (it == g_host_files.end()) return Value::make_obj(nullptr);
        uint32_t len = 0;
        if (!rd_u32(it->second.bytes, it->second.pos, &len) || len == 0 ||
            len > (1u << 20))
          return Value::make_obj(nullptr);
        if (it->second.pos + len > it->second.bytes.size())
          return Value::make_obj(nullptr);
        std::string s(
            reinterpret_cast<const char*>(it->second.bytes.data() +
                                          it->second.pos),
            len);
        it->second.pos += len;
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return Value::make_obj(jvm.new_string(s));
      });
  jvm.natives().register_method(
      "java.lang.System", "syncGameTime", "(F)V",
      [&jvm](Object*, const std::vector<Value>& args) {
        float t = 0.f;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Float) t = args[0].v.f;
          else if (args[0].tag == ValueTag::Int) t = static_cast<float>(args[0].v.i);
        }
        jvm.set_static("java.game.GameLogic", "time", Value::make_float(t));
        return Value::make_void();
      });

  // Platform cursor. The script queues "mode 0 <letter>"; the host applies it.
  jvm.natives().register_method(
      "java.io.MouseCursor", "setPointer",
      "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",
      [](Object* self, const std::vector<Value>& args) {
#ifdef _WIN32
        char letter = 'J';
        if (args.size() >= 2 && args[1].tag == ValueTag::Obj && args[1].v.o &&
            !args[1].v.o->str.empty())
          letter = args[1].v.o->str[0];
        if (self && args.size() >= 2) self->fields["pointer"] = args[1];
        host_cursor_set_pointer(letter);
#else
        (void)self;
        (void)args;
#endif
        return Value::make_void();
      });

  // Player.<init> during start_init is a long TREE. A miss there aborts boot.
  // Career creation fills the existing player from the menu script.
  jvm.natives().register_method("java.game.Player", "<init>", "()V", native_noop);

  // Controller.reset() calls these. Unbound, Init stops and the window closes.
  auto ret_int0 = [](Object*, const std::vector<Value>&) {
    return Value::make_int(0);
  };
  jvm.natives().register_method("java.io.Controller", "<init>", "()V", native_noop);
  jvm.natives().register_method("java.io.Controller", "user_Reset", "()V",
                                native_noop);
  jvm.natives().register_method(
      "java.io.Controller", "user_Add", "(IIIFFFF)I", ret_int0);
  jvm.natives().register_method("java.io.Controller", "user_Del", "(III)I",
                                ret_int0);
  jvm.natives().register_method(
      "java.io.Controller", "user_SetAxisSmooth", "(IFFFFF)I", ret_int0);
  jvm.natives().register_method(
      "java.io.Controller", "user_SetAxisSpeed", "(IF)V", native_noop);
  jvm.natives().register_method(
      "java.io.Controller", "user_GetAxisVal", "(I)F",
      [](Object*, const std::vector<Value>&) {
        return Value::make_float(0.f);
      });
  jvm.natives().register_method(
      "java.io.Controller", "user_SetAxisForce", "(IFF)V", native_noop);
}

}  // namespace slrr
