#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/ptx_image.hpp"

#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
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
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_set>
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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
// Publish a fully written sibling file. Never truncate the destination before
// all bytes are written, flushed to the OS and the handle has closed.
bool publish_save(const std::filesystem::path& destination,
                  const std::vector<uint8_t>& bytes, DWORD& error) {
  namespace fs = std::filesystem;
  error = ERROR_SUCCESS;
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) { error = static_cast<DWORD>(ec.value()); return false; }
  static uint64_t serial = 0;
  fs::path staging;
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    staging = destination;
    staging += L".openslrr-save." + std::to_wstring(GetCurrentProcessId()) +
               L"." + std::to_wstring(++serial) + L".tmp";
    handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) break;
    error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
  }
  if (handle == INVALID_HANDLE_VALUE) return false;
  error = ERROR_SUCCESS;
  size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD count = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, count, &written, nullptr)) {
      error = GetLastError(); break;
    }
    if (!written) { error = ERROR_WRITE_FAULT; break; }
    offset += written;
  }
  if (error == ERROR_SUCCESS && !FlushFileBuffers(handle)) error = GetLastError();
  if (!CloseHandle(handle) && error == ERROR_SUCCESS) error = GetLastError();
  if (error == ERROR_SUCCESS &&
      !MoveFileExW(staging.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    error = GetLastError();
  if (error != ERROR_SUCCESS) DeleteFileW(staging.c_str());
  return error == ERROR_SUCCESS;
}

// The newest in-memory save wins over an older disk copy after publication
// failed (for example while the original game holds CopyCar without sharing
// deletion). Copy/move must see the same bytes as File.open and File.exists.
int transfer_pending_save(const std::filesystem::path& src,
                          const std::filesystem::path& dst, bool move) {
  namespace fs = std::filesystem;
  const auto source_key = save_key(src), destination_key = save_key(dst);
  const auto pending = g_unflushed_saves.find(source_key);
  if (pending == g_unflushed_saves.end()) return 1; // use the normal disk path
  if (source_key == destination_key) return move ? 0 : -1;
  DWORD error = ERROR_SUCCESS;
  if (!publish_save(dst, pending->second, error)) return -1;
  g_unflushed_saves.erase(destination_key);
  if (move) {
    std::error_code ec;
    fs::remove(src, ec);
    if (ec) return -1; // destination is valid; preserve source and its buffer
    g_unflushed_saves.erase(source_key);
  }
  return 0;
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
  if (bytes.size() < 4 || bytes[0] != 'S' || bytes[1] != 'D' ||
      bytes[2] != 'A' || bytes[3] != 'T')
    return 0;
  if (bytes.size() < 8) return kSdatCorrupt;
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

// Script strings are bytes, not UTF-8. U+0000..U+00FF encodes each byte
// losslessly, including stock Windows-1252 text and arbitrary damage strings.
std::string json_string(std::string_view bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : bytes) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32 || c >= 127) {
      out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
    } else out += c;
  }
  return out + '"';
}

// File-local resource word (as written by sdat_wire_resource) to the
// session-independent form stored in version 2 JSON.
save_json::Leaf resource_leaf(const HostFile& file, uint32_t wired) {
  save_json::Leaf leaf;
  leaf.type = save_json::LeafType::Resource;
  if (wired == 0) return leaf;
  const uint32_t hi = wired & 0xFFFF0000u;
  if (hi != 0 && hi != 0xFFFF0000u) {
    const int32_t ordinal = static_cast<int32_t>(hi >> 16);
    for (const SdatPackRef& row : file.sdat_packs)
      if (row.ordinal == ordinal) {
        leaf.res = save_json::ResKind::Pack;
        leaf.pack = row.path;
        leaf.local = wired & 0xFFFFu;
        return leaf;
      }
    return leaf;  // an unknown pack row reads as null
  }
  const int32_t ordinal = static_cast<int32_t>(wired & 0xFFFFu);
  for (const SdatBlobRef& blob : file.sdat_blobs)
    if (blob.ordinal == ordinal) {
      leaf.res = save_json::ResKind::Blob;
      leaf.blob = ordinal;
      return leaf;
    }
  return leaf;
}

bool records_values(const HostFile& file) {
  return file.sdat && (file.writing || (file.convert && !file.convert_broken));
}

void record_leaf(HostFile& file, save_json::Leaf leaf) {
  if (!records_values(file)) return;
  leaf.frame = file.frame_stack.empty() ? -1 : file.frame_stack.back();
  file.doc.leaves.push_back(std::move(leaf));
}

void record_word(Object* self, save_json::LeafType type, uint32_t word) {
  auto it = g_host_files.find(self);
  if (it == g_host_files.end()) return;
  save_json::Leaf leaf;
  leaf.type = type;
  leaf.word = word;
  record_leaf(it->second, std::move(leaf));
}

void record_read(HostFile& file, save_json::LeafType type, uint32_t word) {
  if (!file.convert || file.convert_broken) return;
  save_json::Leaf leaf;
  leaf.type = type;
  leaf.word = word;
  record_leaf(file, std::move(leaf));
}

save_json::Document file_document(const HostFile& file) {
  save_json::Document doc = file.doc;
  for (const SdatBlobRef& blob : file.sdat_blobs)
    doc.blobs.push_back({blob.ordinal, blob.kind, blob.bytes});
  return doc;
}

std::vector<uint8_t> json_frame(const HostFile& file, std::string* kind) {
  const std::string text = save_json::encode(file_document(file), kind);
  return {text.begin(), text.end()};
}

// Script frames (Part.save, Inventory.load, ...) that receive an open save
// File. They let the encoder attribute part data to its script class.
std::vector<Object*> g_frame_files;

bool save_frame_enter(const std::string& owner, const std::string& method, Object* self,
                      const std::vector<Value>& args) {
  if (g_host_files.empty()) return false;
  for (const Value& arg : args) {
    if (arg.tag != ValueTag::Obj || !arg.v.o) continue;
    auto it = g_host_files.find(arg.v.o);
    if (it == g_host_files.end()) continue;
    HostFile& file = it->second;
    if (!records_values(file) || file.doc.frames.size() >= 1000000) return false;
    save_json::Frame frame;
    frame.owner = owner;
    frame.method = method;
    frame.self = self;
    if (self) frame.self_class = self->class_name;
    frame.parent = file.frame_stack.empty() ? -1 : file.frame_stack.back();
    file.doc.frames.push_back(std::move(frame));
    file.frame_stack.push_back(static_cast<int>(file.doc.frames.size() - 1));
    g_frame_files.push_back(arg.v.o);
    return true;
  }
  return false;
}

void save_frame_leave() {
  if (g_frame_files.empty()) return;
  Object* file = g_frame_files.back();
  g_frame_files.pop_back();
  auto it = g_host_files.find(file);
  if (it != g_host_files.end() && !it->second.frame_stack.empty())
    it->second.frame_stack.pop_back();
}

// Version 2 JSON to the script stream and its remount tables.
// Returns 0 when the bytes are not a version 2 save, 1 when decoded,
// -1 when a version 2 save is damaged.
int json2_take_payload(RpkIndex& rpk, HostFile& file) {
  save_json::Document doc;
  std::string error;
  const int result = save_json::decode(
      std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()),
      doc, &error);
  if (result <= 0) {
    if (result < 0)
      std::printf("[native] File.open %s: damaged save (%s)\n", file.path.string().c_str(),
                  error.c_str());
    return result;
  }
  HostFile decoded;
  for (const save_json::Blob& blob : doc.blobs) {
    SdatBlobRef ref;
    ref.ordinal = blob.ordinal;
    ref.kind = blob.kind;
    ref.bytes = blob.bytes;
    decoded.sdat_blobs.push_back(std::move(ref));
  }
  for (const save_json::Leaf& leaf : doc.leaves) {
    switch (leaf.type) {
      case save_json::LeafType::Int:
      case save_json::LeafType::Float:
        push_u32(decoded.bytes, leaf.word);
        break;
      case save_json::LeafType::String:
        if (leaf.null_string) { push_u32(decoded.bytes, 0); break; }
        push_u32(decoded.bytes, static_cast<uint32_t>(leaf.text.size() + 1));
        decoded.bytes.insert(decoded.bytes.end(), leaf.text.begin(), leaf.text.end());
        decoded.bytes.push_back(0);
        break;
      case save_json::LeafType::Resource: {
        uint32_t wired = 0;
        if (leaf.res == save_json::ResKind::Blob) {
          wired = static_cast<uint32_t>(leaf.blob);
        } else if (leaf.res == save_json::ResKind::Pack) {
          const SdatPackRef* row = nullptr;
          for (const SdatPackRef& existing : decoded.sdat_packs)
            if (_stricmp(existing.path.c_str(), leaf.pack.c_str()) == 0) row = &existing;
          if (!row) {
            if (decoded.sdat_packs.size() >= 0xFFFE ||
                under_game(rpk.game_root(), leaf.pack).empty())
              return -1;
            SdatPackRef added;
            added.ordinal = static_cast<int32_t>(decoded.sdat_packs.size() + 1);
            added.path = leaf.pack;
            decoded.sdat_packs.push_back(std::move(added));
            row = &decoded.sdat_packs.back();
          }
          wired = (static_cast<uint32_t>(row->ordinal) << 16) | (leaf.local & 0xFFFFu);
        }
        push_u32(decoded.bytes, wired);
        break;
      }
    }
  }
  for (auto& pack : decoded.sdat_packs) pack.pack_id = sdat_resolve_pack(rpk, pack.path);
  file.bytes = std::move(decoded.bytes);
  file.sdat_packs = std::move(decoded.sdat_packs);
  file.sdat_blobs = std::move(decoded.sdat_blobs);
  return 1;
}

bool under_save_dir(const std::string& root, const std::filesystem::path& path) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path, std::filesystem::path(root), ec);
  if (ec || relative.empty()) return false;
  const std::string first = relative.begin()->string();
  return _stricmp(first.c_str(), "save") == 0;
}

// A schema-specific, nonrecursive JSON reader. No generic object tree or new
// dependency is needed; nesting and all accepted fields are fixed by version 1.
struct SaveJsonReader {
  std::string_view text;
  size_t pos = 0;
  void space() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' ||
        text[pos] == '\r' || text[pos] == '\t')) ++pos;
  }
  bool take(char c) {
    space();
    if (pos == text.size() || text[pos] != c) return false;
    ++pos; return true;
  }
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  bool string(std::string& out) {
    out.clear();
    if (!take('"')) return false;
    while (pos < text.size()) {
      unsigned char c = text[pos++];
      if (c == '"') return true;
      if (c < 32) return false;
      if (c == '\\') {
        if (pos == text.size()) return false;
        c = text[pos++];
        switch (c) {
          case '"': case '\\': case '/': break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'u': {
            if (text.size() - pos < 4) return false;
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const int h = hex(text[pos++]);
              if (h < 0) return false;
              code = (code << 4) | h;
            }
            if (code > 255) return false;
            c = static_cast<unsigned char>(code); break;
          }
          default: return false;
        }
      } else if (c >= 128) {
        // Also accept literal UTF-8 for the byte-valued U+0080..U+00FF.
        if ((c != 0xc2 && c != 0xc3) || pos == text.size()) return false;
        const unsigned char next = text[pos++];
        if ((next & 0xc0) != 0x80) return false;
        c = static_cast<unsigned char>(((c & 3) << 6) | (next & 63));
      }
      out += c;
    }
    return false;
  }
  bool number(std::string_view& out) {
    space();
    const size_t start = pos;
    if (pos < text.size() && text[pos] == '-') ++pos;
    if (pos == text.size()) return false;
    if (text[pos] == '0') ++pos;
    else {
      if (text[pos] < '1' || text[pos] > '9') return false;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    for (const char marker : {'.', 'e'}) {
      if (pos == text.size() || (text[pos] != marker &&
          !(marker == 'e' && text[pos] == 'E'))) continue;
      ++pos;
      if (marker == 'e' && pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
      const size_t digits = pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
      if (digits == pos) return false;
    }
    out = text.substr(start, pos - start); return true;
  }
  bool integer(int64_t& out, int64_t low, int64_t high) {
    std::string_view token;
    if (!number(token)) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size() &&
        out >= low && out <= high;
  }
  template<class Read> bool array(Read read) {
    if (!take('[')) return false;
    if (take(']')) return true;
    do { if (!read()) return false; } while (take(','));
    return take(']');
  }
};

// Returns 0 for legacy input, 1 for JSON, -1 for a damaged/unknown JSON save.
// Rebuild the same binary stream so existing Java readers need no changes.
int json_take_payload(RpkIndex& rpk, HostFile& file) {
  SaveJsonReader in{{reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()}};
  if (in.text.starts_with("\xef\xbb\xbf")) in.pos = 3;
  if (!in.take('{')) return 0;
  in.space();
  // Reserve the schema's opening keys, not every '{'-valued raw integer.
  // Also recognize truncated canonical prefixes so damaged JSON fails closed.
  SaveJsonReader probe = in;
  std::string opening_key;
  const bool complete_key = probe.string(opening_key);
  const auto prefix = in.text.substr(in.pos);
  bool is_json = false;
  for (std::string_view key : {"format", "version", "values", "packs", "blobs"}) {
    const std::string quoted = json_string(key);
    if ((complete_key && opening_key == key) ||
        (!complete_key && std::string_view(quoted).starts_with(prefix))) is_json = true;
  }
  if (!is_json) {
    if (probe.take('}')) { probe.space(); if (probe.pos == probe.text.size()) return -1; }
    return 0;
  }
  HostFile decoded;
  unsigned seen = 0;
  do {
    std::string key;
    if (!in.string(key) || !in.take(':')) return -1;
    const unsigned field = key == "format" ? 1 : key == "version" ? 2 :
        key == "values" ? 4 : key == "packs" ? 8 : key == "blobs" ? 16 : 0;
    if (!field || (seen & field)) return -1;
    seen |= field;
    int64_t n = 0;
    if (field == 1) {
      std::string format;
      if (!in.string(format) || format != "OpenSLRR-save") return -1;
    } else if (field == 2) {
      if (!in.integer(n, 1, 1)) return -1;
    } else if (field == 4) {
      if (!in.array([&]() {
        std::string type;
        if (!in.take('[') || !in.string(type) || !in.take(',')) return false;
        if (type == "string") {
          in.space();
          if (in.text.substr(in.pos, 4) == "null") {
            in.pos += 4; push_u32(decoded.bytes, 0);
          } else {
            std::string value;
            if (!in.string(value) || value.size() >= UINT32_MAX ||
                value.find('\0') != std::string::npos) return false;
            push_u32(decoded.bytes, static_cast<uint32_t>(value.size() + 1));
            decoded.bytes.insert(decoded.bytes.end(), value.begin(), value.end());
            decoded.bytes.push_back(0);
          }
        } else if (type == "float") {
          std::string_view token;
          float value = 0;
          if (!in.number(token)) return false;
          const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
          if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
              !std::isfinite(value)) return false;
          uint32_t bits; std::memcpy(&bits, &value, 4); push_u32(decoded.bytes, bits);
        } else {
          if (type != "int" && type != "resource" && type != "floatBits") return false;
          if (!in.integer(n, type == "floatBits" ? 0 : INT32_MIN,
                          type == "floatBits" ? UINT32_MAX : INT32_MAX)) return false;
          push_u32(decoded.bytes, static_cast<uint32_t>(n));
        }
        return in.take(']');
      })) return -1;
    } else if (field == 8) {
      std::unordered_set<int32_t> ordinals;
      if (!in.array([&]() {
        SdatPackRef pack;
        if (!in.take('[') || !in.integer(n, 1, 0xfffe) || !in.take(',')) return false;
        pack.ordinal = static_cast<int32_t>(n);
        if (!ordinals.insert(pack.ordinal).second || !in.string(pack.path) ||
            pack.path.size() > 1024 || under_game(rpk.game_root(), pack.path).empty() ||
            !in.take(']')) return false;
        decoded.sdat_packs.push_back(std::move(pack)); return true;
      })) return -1;
    } else {
      std::unordered_set<int32_t> ordinals;
      if (!in.array([&]() {
        SdatBlobRef blob;
        std::string hex;
        if (!in.take('[') || !in.integer(n, 1, 0xffff) || !in.take(',')) return false;
        blob.ordinal = static_cast<int32_t>(n);
        if (!ordinals.insert(blob.ordinal).second || !in.integer(n, 0, INT32_MAX) ||
            !in.take(',')) return false;
        blob.kind = static_cast<int32_t>(n);
        if (!in.string(hex) || hex.size() % 2 || !in.take(']')) return false;
        for (size_t i = 0; i < hex.size(); i += 2) {
          const int a = SaveJsonReader::hex(hex[i]), b = SaveJsonReader::hex(hex[i + 1]);
          if (a < 0 || b < 0) return false;
          blob.bytes.push_back(static_cast<uint8_t>((a << 4) | b));
        }
        decoded.sdat_blobs.push_back(std::move(blob)); return true;
      })) return -1;
    }
  } while (in.take(','));
  if (seen != 31 || !in.take('}')) return -1;
  in.space();
  if (in.pos != in.text.size()) return -1;
  for (auto& pack : decoded.sdat_packs) pack.pack_id = sdat_resolve_pack(rpk, pack.path);
  file.bytes = std::move(decoded.bytes);
  file.sdat_packs = std::move(decoded.sdat_packs);
  file.sdat_blobs = std::move(decoded.sdat_blobs);
  return 1;
}

// File-local id for write(ResourceRef). A null ref is 0. A pack resource
// becomes (ordinal << 16) | local. An unsaved id (high word 0xFFFF, type
// 5 or 7) becomes its session ordinal, with a blob in the trailer. A
// Painter texture's blob holds its committed pixels as a PNG; a type-7 id
// that is no longer a live texture keeps an empty blob, which loads as null.
bool sdat_wire_resource(RpkIndex& rpk, HostFile& file, int32_t host_id,
                        int32_t restype, int32_t* wired) {
  if (!wired) return false;
  if (host_id == 0) {
    *wired = 0;
    return true;
  }
  const uint32_t uid = static_cast<uint32_t>(host_id);
  const bool file_texture = restype == 7 && gameref_find_live(host_id);
  if ((uid & 0xFFFF0000u) == 0xFFFF0000u || file_texture) {
    if (restype != 5 && restype != 7) return false;
    for (const SdatBlobRef& blob : file.sdat_blobs) {
      if (blob.abs_id == host_id) {
        *wired = blob.ordinal;
        return true;
      }
    }
    if (file.sdat_blobs.size() >= 0xFFFF) return false;
    SdatBlobRef blob;
    if (restype == 7) {
      PaintImage decoded;
      const PaintImage* image = paint_texture_committed(uid);
      if (!image && file_texture) {
        const auto path = scene_instances_texture(uid);
        if (!path.empty() && paint_image_from_texture(load_texture_file(path), &decoded))
          image = &decoded;
      }
      if (image) {
        blob.bytes = paint_png_encode(*image);
        if (blob.bytes.empty()) return false;
      }
    }
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

// A saved texture blob becomes a Painter texture again. The stock game
// leaves the blob empty and writes the image (JPEG) beside the save as
// "<file>.<ordinal>"; this host embeds a PNG.
int32_t sdat_load_texture_blob(const HostFile& file, SdatBlobRef& blob,
                                const std::string& root) {
  std::vector<uint8_t> bytes = blob.bytes;
  if (bytes.empty() && !file.path.empty()) {
    std::filesystem::path side = file.path;
    side += "." + std::to_string(blob.ordinal);
    side = under_game(root, side.lexically_relative(std::filesystem::path(root)).string());
    if (side.empty()) return 0;
    std::ifstream in(side, std::ios::in | std::ios::binary | std::ios::ate);
    if (in) {
      const auto size = in.tellg();
      if (size <= 0 || size > 64 * 1024 * 1024) return 0;
      in.seekg(0);
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  PaintImage image;
  if (bytes.empty() || !paint_image_decode(bytes.data(), bytes.size(), &image)) return 0;
  blob.abs_id = static_cast<int32_t>(paint_texture_create(std::move(image)));
  return blob.abs_id;
}

int32_t sdat_remap_resource(HostFile& file, uint32_t wired, const std::string& root) {
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
  for (SdatBlobRef& blob : file.sdat_blobs) {
    if (blob.ordinal != ordinal) continue;
    if (blob.abs_id != 0 && (blob.kind != 7 ||
                             paint_texture_exists(static_cast<uint32_t>(blob.abs_id))))
      return blob.abs_id;
    if (blob.kind == 7) return sdat_load_texture_blob(file, blob, root);
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
  set_script_frame_hooks(save_frame_enter, save_frame_leave);
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
                    obj_str(native_arg_object(args, 0)).c_str());
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
                                  native_noop);
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
        // Stock Java deletes contents explicitly before deleting a directory.
        // Refuse nonempty directories instead of recursively erasing a tree.
        fs::remove(p, ec);
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
        const bool ok = !p.empty() &&
            (g_unflushed_saves.count(save_key(p)) != 0 || std::filesystem::exists(p, ec));
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
        const int pending = transfer_pending_save(src, dst, false);
        if (pending != 1) return Value::make_int(pending);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) g_unflushed_saves.erase(save_key(dst));
        return Value::make_int(ec ? -1 : 0);
      });
  jvm.natives().register_method(
      "java.io.File", "move", "(Ljava.lang.String;Ljava.lang.String;)I",
      [two_paths](Object*, const std::vector<Value>& args) {
        namespace fs = std::filesystem;
        fs::path src, dst;
        if (!two_paths(args, &src, &dst)) return Value::make_int(-1);
        const int pending = transfer_pending_save(src, dst, true);
        if (pending != 1) return Value::make_int(pending);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::rename(src, dst, ec);
        if (!ec) g_unflushed_saves.erase(save_key(dst));
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
          // Keep the binary stream for callers; close() publishes typed JSON.
          hf.sdat = true;
        } else {
          // Specifying a mode replaces the ifstream default. Without in,
          // MSVC does not open the file.
          std::ifstream in(p, std::ios::in | std::ios::binary);
          const auto kept = g_unflushed_saves.find(save_key(p));
          const bool pending = kept != g_unflushed_saves.end();
          if (pending)
            hf.bytes = kept->second;
          else if (in)
            hf.bytes.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
          else
            return Value::make_int(0);
          // Version 2 JSON, then version 1 JSON, then SDAT type 1/2, then a
          // raw stream. Version 1 and SDAT saves become version 2 once read.
          int shaped = json2_take_payload(rpk, hf);
          bool legacy = false;
          if (shaped == 0) {
            shaped = json_take_payload(rpk, hf);
            if (shaped == 0)
              shaped = sdat_take_payload(rpk, hf.bytes, hf.sdat_packs, hf.sdat_blobs);
            legacy = shaped > 0;
          }
          if (shaped < 0) return Value::make_int(0);
          hf.sdat = shaped > 0;
          hf.convert = legacy && !pending && under_save_dir(jvm.game_root(), p);
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
          std::string kind = "binary";
          const std::vector<uint8_t> framed =
              it->second.sdat ? json_frame(it->second, &kind) : it->second.bytes;
          DWORD publish_error = ERROR_SUCCESS;
          const bool wrote = publish_save(it->second.path, framed, publish_error);
          const std::string key = save_key(it->second.path);
          if (wrote)
            g_unflushed_saves.erase(key);
          else
            g_unflushed_saves[key] = framed;
          std::printf(
              "[native] File.close %s %zu bytes ok=%d kept=%d json=%s packs=%zu degraded_resources=%u error=%lu\n",
              it->second.path.string().c_str(), framed.size(),
              wrote ? 1 : 0,
              g_unflushed_saves.count(key) ? 1 : 0, kind.c_str(),
              it->second.sdat_packs.size(), it->second.degraded_resources, publish_error);
        } else if (it != g_host_files.end() && it->second.convert) {
          // A legacy save the script read completely: rewrite it as version
          // 2 JSON. A partial read (fileCheck) or misaligned read keeps it.
          HostFile& file = it->second;
          if (!file.convert_broken && file.pos == file.bytes.size() && !file.doc.leaves.empty()) {
            std::string kind;
            const std::vector<uint8_t> framed = json_frame(file, &kind);
            DWORD publish_error = ERROR_SUCCESS;
            const bool wrote = publish_save(file.path, framed, publish_error);
            std::printf("[native] File.close %s converted legacy save to json=%s ok=%d error=%lu\n",
                        file.path.string().c_str(), kind.c_str(), wrote ? 1 : 0, publish_error);
          }
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
        record_word(self, save_json::LeafType::Int, static_cast<uint32_t>(v));
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
        uint32_t bits; std::memcpy(&bits, &v, 4);
        record_word(self, save_json::LeafType::Float, bits);
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
        const bool degraded = it->second.sdat &&
            !sdat_wire_resource(rpk, it->second, id, restype, &wired);
        if (degraded) {
          // readResID always consumes four bytes and stock callers often
          // ignore write's return value. A null is safer than a shifted save.
          wired = 0;
          ++it->second.degraded_resources;
          std::printf("[sdat] resource 0x%08x has no pack; writing null placeholder\n",
                      static_cast<uint32_t>(id));
        }
        if (!file_push(self, &wired, 4)) return Value::make_int(0);
        record_leaf(it->second, resource_leaf(it->second, static_cast<uint32_t>(wired)));
        return Value::make_int(degraded ? 0 : 4);
      });
  jvm.natives().register_method(
      "java.io.File", "write", "(Ljava.lang.String;)I",
      [](Object* self, const std::vector<Value>& args) {
        Object* s =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        // A non-null empty string is length 1 plus its NUL. Length 0 is null.
        const char* c = s ? s->str.c_str() : nullptr;
        const int32_t len = c ? static_cast<int32_t>(std::strlen(c) + 1) : 0;
        if (!file_push(self, &len, 4)) return Value::make_int(0);
        save_json::Leaf leaf;
        leaf.type = save_json::LeafType::String;
        if (len <= 0) {
          leaf.null_string = true;
          record_leaf(g_host_files.at(self), std::move(leaf));
          return Value::make_int(0);
        }
        if (!file_push(self, c, static_cast<size_t>(len)))
          return Value::make_int(-1);
        leaf.text.assign(c, static_cast<size_t>(len - 1));
        record_leaf(g_host_files.at(self), std::move(leaf));
        return Value::make_int(len);
      });
  jvm.natives().register_method(
      "java.io.File", "readInt", "()I",
      [](Object* self, const std::vector<Value>&) {
        uint32_t v = 0;
        auto it = g_host_files.find(self);
        if (it != g_host_files.end()) {
          if (rd_u32(it->second.bytes, it->second.pos, &v))
            record_read(it->second, save_json::LeafType::Int, v);
          else
            it->second.convert_broken = true;
        }
        return Value::make_int(static_cast<int32_t>(v));
      });
  jvm.natives().register_method(
      "java.io.File", "readFloat", "()F",
      [](Object* self, const std::vector<Value>&) {
        float v = 0.f;
        auto it = g_host_files.find(self);
        if (it != g_host_files.end()) {
          if (rd_f32(it->second.bytes, it->second.pos, &v)) {
            uint32_t bits; std::memcpy(&bits, &v, 4);
            record_read(it->second, save_json::LeafType::Float, bits);
          } else {
            it->second.convert_broken = true;
          }
        }
        return Value::make_float(v);
      });
  jvm.natives().register_method(
      "java.io.File", "readResID", "()I",
      [&jvm](Object* self, const std::vector<Value>&) {
        uint32_t v = 0;
        auto it = g_host_files.find(self);
        if (it == g_host_files.end()) return Value::make_int(0);
        if (!rd_u32(it->second.bytes, it->second.pos, &v)) {
          it->second.convert_broken = true;
          return Value::make_int(0);
        }
        if (it->second.convert && !it->second.convert_broken)
          record_leaf(it->second, resource_leaf(it->second, v));
        return Value::make_int(sdat_remap_resource(it->second, v, jvm.game_root()));
      });
  jvm.natives().register_method(
      "java.io.File", "readString", "()Ljava.lang.String;",
      [&jvm](Object* self, const std::vector<Value>&) {
        auto it = g_host_files.find(self);
        if (it == g_host_files.end()) return Value::make_obj(nullptr);
        uint32_t len = 0;
        HostFile& file = it->second;
        save_json::Leaf leaf;
        leaf.type = save_json::LeafType::String;
        if (!rd_u32(file.bytes, file.pos, &len) || len > (1u << 20) ||
            file.pos + len > file.bytes.size()) {
          file.convert_broken = true;
          return Value::make_obj(nullptr);
        }
        if (len == 0) {
          leaf.null_string = true;
          if (file.convert) record_leaf(file, std::move(leaf));
          return Value::make_obj(nullptr);
        }
        std::string s(reinterpret_cast<const char*>(file.bytes.data() + file.pos), len);
        file.pos += len;
        // The version 2 form stores strlen strings; other bytes stay legacy.
        if (s.back() != '\0' || s.find('\0') != len - 1) file.convert_broken = true;
        else if (file.convert) { leaf.text.assign(s, 0, len - 1); record_leaf(file, std::move(leaf)); }
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return Value::make_obj(jvm.new_string(s));
      });
  // OpenSLRR (#488): bytes left to read in an open File, so the patched
  // ControlSet reads its optional MKEY trailer only when one is there (a
  // read past the end marks the file broken for the JSON conversion).
  jvm.natives().register_method(
      "java.io.ControlSet", "bytesLeft", "(Ljava.io.File;)I",
      [](Object*, const std::vector<Value>& args) {
        Object* file = !args.empty() && args[0].tag == ValueTag::Obj ? args[0].v.o : nullptr;
        auto it = g_host_files.find(file);
        if (it == g_host_files.end() || it->second.writing ||
            it->second.pos >= it->second.bytes.size())
          return Value::make_int(0);
        const size_t left = it->second.bytes.size() - it->second.pos;
        return Value::make_int(static_cast<int32_t>(std::min<size_t>(left, INT32_MAX)));
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
        // The legacy pad alias still exposes split-positive SDL channels.
        // Raw SDL peripheral axes remain bipolar. Retain the configured
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
