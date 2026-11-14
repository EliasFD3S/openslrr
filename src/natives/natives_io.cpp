#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/file_faults.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/metrics.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"
#include "slrr/scene_ambience.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/script_threads.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/resource_loading.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace slrr {
namespace {
// StreetLegal can hold save/temp/CopyCar open. The selector's back-to-first
// page saves, destroys, then loads that path; a failed disk write must still
// be what the following read sees.
std::unordered_map<std::string, std::vector<uint8_t>> g_unflushed_saves;

const bool g_metrics_registered = [] {
  return metrics::register_provider([](void*, std::vector<metrics::Value>& values) {
    values.push_back({"host.files_open", static_cast<std::int64_t>(g_host_files.size()),
                      metrics::Kind::Gauge});
    values.push_back({"host.pending_writes",
                      static_cast<std::int64_t>(g_unflushed_saves.size()),
                      metrics::Kind::Gauge});
  }) != 0;
}();

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

namespace {
std::weak_ptr<ResourceLoadingHistory> loading_history;
}
void resource_loading_pump() {
  if (auto history = loading_history.lock()) history->pump();
}
void register_io_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  struct PackLoading {
    unsigned scans = 0;
    bool high_priority = false;
  };
  const auto loading = std::make_shared<PackLoading>();
  const auto history = std::make_shared<ResourceLoadingHistory>();
  loading_history = history;
  jvm.natives().register_method(
      "java.render.Frontend", "setFonts", "()V",
      [&jvm](Object*, const std::vector<Value>&) {
        frontend_set_fonts(jvm, osd_framebuffer_width(), osd_framebuffer_height());
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
      [&jvm](Object*, const std::vector<Value>&) {
        return Value::make_float(static_cast<float>(jvm.sim_time()));
      });
  jvm.natives().register_method(
      "java.lang.System", "timeWarp", "(F)F",
      [&jvm](Object*, const std::vector<Value>& args) {
        float scale = -1.f;
        if (!args.empty() && args[0].tag == ValueTag::Float) scale = args[0].v.f;
        if (!args.empty() && args[0].tag == ValueTag::Int) scale = static_cast<float>(args[0].v.i);
        const float previous = jvm.time_warp(scale);
        if (std::isfinite(scale) && scale >= 0.f && (previous == 0.f) != (scale == 0.f)) {
          part_pause_engine_audio(scale == 0.f);
          scene_ambience_pause(scale == 0.f);
        }
        return Value::make_float(previous);
      });
  // Compatibility calls in shipped System.java without runtime work.
  for (const char* method : {"netHost", "netJoin", "netLeave"}) {
    jvm.natives().register_method(
        "java.lang.System", method, "()I",
        [](Object*, const std::vector<Value>&) { return Value::make_int(0); });
  }
  for (const char* signature : {"(Ljava.lang.Object;)V", "(F)V", "(I)V"})
    jvm.natives().register_method("java.lang.System", "analyze", signature,
                                  native_noop,
                                  NativeImplementation::ExplicitNoop);
  // All three analyze overloads intentionally discard their argument in PE.
  jvm.natives().register_method(
      "java.lang.System", "compileAll", "(Ljava.lang.String;)I",
      [&jvm](Object*, const std::vector<Value>& args) {
        const auto directory = !args.empty() && args[0].tag == ValueTag::Obj
            ? obj_str(args[0].v.o) : std::string(".");
        const auto report = script_compile_all(jvm.game_root(), directory, [&jvm] {
          if (script_thread_active(jvm)) script_thread_checkpoint(jvm);
        });
        // Original returns the number of source units visited, not class blobs.
        return Value::make_int(report.attempted);
      });
  jvm.natives().register_method(
      "java.lang.System", "info", "(I)Ljava.lang.String;",
      [&jvm](Object*, const std::vector<Value>& args) {
        // Selector 5 is the live VM object count in the original.
        const bool heap_count = !args.empty() && args[0].tag == ValueTag::Int &&
                                args[0].v.i == 5;
        return Value::make_obj(
            jvm.new_string(heap_count ? std::to_string(jvm.heap_size()) : ""));
      });
  jvm.natives().register_method(
      "java.lang.System", "openLib", "(Ljava.lang.String;)I",
      [&rpk, history](Object*, const std::vector<Value>& args) {
        std::string path;
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          path = obj_str(args[0].v.o);
        const auto begin = std::chrono::steady_clock::now();
        const int id = rpk.open_lib(path);
        history->record(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count());
        return Value::make_int(id);
      });
  jvm.natives().register_method(
      "java.lang.System", "rpkScan", "(Ljava.lang.String;)I",
      [&jvm, &rpk, loading, history](Object*, const std::vector<Value>& args) {
        std::string dir;
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          dir = obj_str(args[0].v.o);
        const auto names = rpk.list_rpks(dir);
        struct ScanScope {
          unsigned& scans;
          explicit ScanScope(unsigned& count) : scans(count) { ++scans; }
          ~ScanScope() { --scans; }
        } scan(loading->scans);
        int32_t n = 0;
        unsigned batch = 0;
        for (const auto& name : names) {
          const auto path = (std::filesystem::path(dir) / name).string();
          const bool was_open = rpk.pack_by_path(path) != nullptr;
          const auto begin = std::chrono::steady_clock::now();
          rpk.open_lib(path);
          history->record(std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - begin).count());
          ++n;
          if (was_open || n == names.size() || !script_thread_active(jvm))
            continue;
          if (loading->high_priority)
            script_thread_checkpoint(jvm);
          else if (++batch == 8) {
            batch = 0;
            script_thread_sleep(jvm, 0.0f);
          }
        }
        std::printf("[native] System.rpkScan(%s) -> %d\n", dir.c_str(), n);
        return Value::make_int(n);
      });
  jvm.natives().register_method(
      "java.lang.System", "isLoading", "()I",
      [loading, history](Object*, const std::vector<Value>&) {
        return Value::make_int(loading->scans != 0 || history->busy());
      });
  jvm.natives().register_method(
      "java.lang.System", "setLdPriority", "(I)V",
      [loading](Object*, const std::vector<Value>& args) {
        loading->high_priority = !args.empty() &&
                                 args[0].tag == ValueTag::Int && args[0].v.i != 0;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.lang.System", "isLoadingReset", "()V",
      [history](Object*, const std::vector<Value>&) {
        history->reset();
        return Value::make_void();
      });
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
        native_arraycopy(src, src_pos, dst, dst_pos, length);
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
        const auto fault = file_faults::before(file_faults::Operation::Delete, 0, p);
        if (fault.injected) return Value::make_int(0);
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
        const auto fault =
            file_faults::before(file_faults::Operation::Copy, 0, src, dst);
        if (fault.injected) return Value::make_int(-1);
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
        const auto fault =
            file_faults::before(file_faults::Operation::Rename, 0, src, dst);
        if (fault.injected) return Value::make_int(-1);
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
          const auto open_fault =
              file_faults::before(file_faults::Operation::Open, 0, p);
          if (open_fault.injected) return Value::make_int(0);
          // Specifying a mode replaces the ifstream default. Without in,
          // MSVC does not open the file.
          std::ifstream in(p, std::ios::in | std::ios::binary);
          const auto kept = g_unflushed_saves.find(save_key(p));
          if (kept != g_unflushed_saves.end()) {
            hf.bytes = kept->second;
          } else if (in) {
            hf.bytes.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
            const auto read_fault = file_faults::before(
                file_faults::Operation::Read, hf.bytes.size(), p);
            if (read_fault.failure) return Value::make_int(0);
            if (read_fault.injected) {
              hf.bytes.resize(static_cast<size_t>(std::min<std::uint64_t>(
                  read_fault.allowed_bytes, hf.bytes.size())));
            }
          } else {
            return Value::make_int(0);
          }
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
          const auto open_fault = file_faults::before(
              file_faults::Operation::Open, 0, it->second.path);
          std::ofstream out;
          if (!open_fault.injected) {
            out.open(it->second.path, std::ios::out | std::ios::binary |
                                          std::ios::trunc);
          }
          const std::vector<uint8_t> framed =
              it->second.sdat ? sdat_frame(it->second.bytes, it->second.sdat_packs,
                                           it->second.sdat_blobs)
                              : it->second.bytes;
          const bool wrote = !open_fault.injected && static_cast<bool>(out);
          if (wrote && !framed.empty()) {
            const auto write_fault = file_faults::before(
                file_faults::Operation::Write, framed.size(), it->second.path);
            if (!write_fault.failure && write_fault.allowed_bytes != 0) {
              out.write(reinterpret_cast<const char*>(framed.data()),
                        static_cast<std::streamsize>(write_fault.allowed_bytes));
            }
            if (write_fault.injected) out.setstate(std::ios::badbit);
          }
          const auto flush_fault = file_faults::before(
              file_faults::Operation::Flush, 0, it->second.path);
          if (flush_fault.injected) out.setstate(std::ios::badbit);
          const auto close_fault = file_faults::before(
              file_faults::Operation::Close, 0, it->second.path);
          if (close_fault.injected) out.setstate(std::ios::badbit);
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
        } else if (it != g_host_files.end()) {
          // Read-side HostFile instances have no physical stream left by this
          // point; preserve close()'s void semantics while still counting the
          // logical persistence operation for focused fault probes.
          (void)file_faults::before(file_faults::Operation::Close, 0,
                                    it->second.path);
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
  for (bool picked : {false, true})
    jvm.natives().register_method("java.io.MouseCursor", picked ? "getPickedPos" : "getPos",
        "()Ljava.lang.Vector3;", [&jvm, picked](Object* self, const std::vector<Value>&) {
          if (!self) return Value::make_obj(nullptr);
          auto ci = self->fields.find("cursor");
          if (ci == self->fields.end() || ci->second.tag != ValueTag::Obj || !ci->second.v.o)
            return Value::make_obj(nullptr);
          Object* inner = ci->second.v.o;
          auto& in = host_input();
          float xyz[3]{};
          if (!picked && in.pointer_valid)
            osd_pixel_to_text(in.pointer_x, in.pointer_y, in.view_w, in.view_h, &xyz[0], &xyz[1]);
          if (picked) {
            const char* names[3] = {"_pick_x", "_pick_y", "_pick_z"};
            for (int i = 0; i < 3; ++i) {
              auto f = inner->fields.find(names[i]);
              if (f != inner->fields.end() && f->second.tag == ValueTag::Float) xyz[i] = f->second.v.f;
            }
            float origin[3], direction[3];
            if (in.pointer_valid && scene_instances_pointer_ray(in.pointer_x, in.pointer_y,
                in.view_w, in.view_h, origin, direction) && scene_instances_pick(origin, direction, xyz))
              for (int i = 0; i < 3; ++i) inner->fields[names[i]] = Value::make_float(xyz[i]);
          }
          Object* out = jvm.new_object("java.lang.Vector3");
          out->fields["x"] = Value::make_float(xyz[0]);
          out->fields["y"] = Value::make_float(xyz[1]);
          out->fields["z"] = Value::make_float(xyz[2]);
          return Value::make_obj(out);
        });
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
  jvm.natives().register_method("java.game.Player", "<init>", "()V", native_noop,
                                NativeImplementation::ExplicitNoop);
  for (const bool visible : {true, false}) {
    jvm.natives().register_method("java.game.Player", visible ? "showOsd" : "hideOsd", "()V",
      [&jvm, visible](Object* player, const std::vector<Value>&) {
        if (!player) return Value::make_void();
        player->fields["_hostVehicleHud"] = Value::make_int(visible ? 1 : 0);
        const auto osd = player->fields.find("osd");
        if (osd != player->fields.end() && osd->second.tag == ValueTag::Obj && osd->second.v.o)
          jvm.invoke("java.render.Osd", visible ? "show" : "hide", "()V", {osd->second}, false);
        return Value::make_void();
      });
  }

  const char* controller = "java.io.Controller";
  auto integer = [](const std::vector<Value>& args, size_t i) {
    return i < args.size() && args[i].tag == ValueTag::Int ? args[i].v.i : -1;
  };
  auto real = [](const Value& value) {
    if (value.tag == ValueTag::Float) return value.v.f;
    return value.tag == ValueTag::Int ? float(value.v.i) : 0.f;
  };
  auto state = [](Object* self) -> ControllerInput& {
    if (!self->input) self->input = std::make_unique<ControllerInput>();
    return *self->input;
  };
  jvm.natives().register_method(controller, "<init>", "()V",
      [state](Object* self, const std::vector<Value>&) {
        if (self) state(self);
        return Value::make_void();
      });
  jvm.natives().register_method(controller, "user_Reset", "()V",
      [state](Object* self, const std::vector<Value>&) {
        if (self) state(self) = {};
        return Value::make_void();
      });
  jvm.natives().register_method(controller, "user_Add", "(IIIFFFF)I",
      [integer, real, state](Object* self, const std::vector<Value>& args) {
        const int axis = integer(args, 0), device = integer(args, 1);
        const int physical = integer(args, 2);
        if (!self || args.size() < 7 || axis <= 0 || physical < 0 ||
            !host_input_device_name(device)) return Value::make_int(0);
        auto& bindings = state(self).bindings;
        InputBinding binding{axis, device, physical, real(args[3]), real(args[4]),
                             real(args[5]), real(args[6])};
        if (!std::isfinite(binding.from) || !std::isfinite(binding.to) ||
            !std::isfinite(binding.low) || !std::isfinite(binding.high))
          return Value::make_int(0);
        // Stock remapping assumes a newly detected joystick axis is bipolar.
        // XInput exposes each direction independently. Retain the configured
        // dead zone while mapping the active half to the full output range.
        if (device == 2 && binding.from < 0.f && binding.to == 1.f)
          binding.from = (binding.from + 1.f) * .5f;
        auto same = [&](const InputBinding& other) {
          return other.axis == axis && other.device == device &&
                 other.physical == physical && other.high == binding.high;
        };
        const auto existing = std::find_if(bindings.begin(), bindings.end(), same);
        if (existing == bindings.end()) bindings.push_back(binding);
        else *existing = binding;
        return Value::make_int(1);
      });
  jvm.natives().register_method(controller, "user_Del", "(III)I",
      [integer](Object* self, const std::vector<Value>& args) {
        if (!self || !self->input) return Value::make_int(0);
        const auto count = std::erase_if(self->input->bindings, [&](const InputBinding& b) {
          return b.axis == integer(args, 0) && b.device == integer(args, 1) &&
                 b.physical == integer(args, 2);
        });
        return Value::make_int(count != 0);
      });
  jvm.natives().register_method(controller, "user_SetAxisSmooth", "(IFFFFF)I",
      [integer, real, state](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() < 6) return Value::make_int(0);
        auto& smooth = state(self).smoothing[integer(args, 0)];
        smooth = {real(args[1]), real(args[2]), real(args[3]), real(args[4]),
                  std::max(1.f, real(args[5])), smooth.value, smooth.speed};
        return Value::make_int(1);
      });
  jvm.natives().register_method(controller, "user_SetAxisSpeed", "(IF)V",
      [integer, real, state](Object* self, const std::vector<Value>& args) {
        const int axis = integer(args, 0);
        if (!self || args.size() != 2 || axis <= 0) return Value::make_void();
        const float speed = real(args[1]);
        if (!std::isfinite(speed) || speed < 0.f) return Value::make_void();
        const float current = host_input_controller_axis(self, axis);
        auto& smooth = state(self).smoothing.try_emplace(
            axis, InputSmoothing{0.f, .002f, .004f, .002f, 1.f, current}).first->second;
        smooth.speed = speed;
        return Value::make_void();
      });
  jvm.natives().register_method(controller, "user_GetAxisVal", "(I)F",
      [integer](Object* self, const std::vector<Value>& args) {
        return Value::make_float(host_input_controller_axis(self, integer(args, 0)));
      });
  jvm.natives().register_method(controller, "user_SetAxisForce", "(IFF)V",
      [integer, real, state](Object* self, const std::vector<Value>& args) {
        const int axis = integer(args, 0);
        if (!self || args.size() != 3 || axis <= 0) return Value::make_void();
        const float force = real(args[2]);
        if (!std::isfinite(force)) return Value::make_void();
        auto& forced = state(self).forced_axes;
        if (force == 0.f) forced.erase(axis);
        else forced[axis] = std::clamp(force, -1.f, 1.f);
        return Value::make_void();
      });
}

}  // namespace slrr
