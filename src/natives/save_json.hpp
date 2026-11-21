#pragma once

// Version 2 save JSON: a named, hierarchical view of the script's save
// stream (career, car, skin) with a typed `values` fallback for any other
// file. Platform independent: natives_io.cpp owns files, packs and blobs.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slrr::save_json {

enum class LeafType : uint8_t { Int, Float, String, Resource };
enum class ResKind : uint8_t { Null, Pack, Blob };

// One File.write / File.read* value, in stream order.
struct Leaf {
  LeafType type = LeafType::Int;
  uint32_t word = 0;         // Int value or Float bits
  bool null_string = false;  // String only
  std::string text;          // String bytes, without the trailing NUL
  ResKind res = ResKind::Null;
  std::string pack;          // game-relative pack path, '\' separators
  uint32_t local = 0;        // pack-local resource id (16 bits)
  int32_t blob = 0;          // blob ordinal
  int frame = -1;            // innermost script frame that wrote/read it
};

// A script method that received the open File as an argument.
struct Frame {
  std::string owner;       // fully qualified class declaring the method
  std::string method;
  const void* self = nullptr;
  std::string self_class;  // runtime class of self
  int parent = -1;
};

struct Blob {
  int32_t ordinal = 0;
  int32_t kind = 0;
  std::vector<uint8_t> bytes;
};

struct Document {
  std::vector<Leaf> leaves;
  std::vector<Frame> frames;
  std::vector<Blob> blobs;
};

// Version 2 JSON text. A named layout is used only when it decodes back to
// the same stream; otherwise the file is written as a typed `values` list.
// kind receives "career", "vehicle", "skin" or "raw".
std::string encode(const Document& doc, std::string* kind = nullptr);

// 0: not a version 2 save (legacy or version 1 input); 1: decoded;
// -1: a version 2 save that is damaged or unsupported.
int decode(std::string_view text, Document& out, std::string* error = nullptr);

// True when both documents describe the same script stream: identical
// words and string bytes, resources compared by pack path/id or blob.
bool same_stream(const Document& a, const Document& b);

// Script string bytes as a JSON string (U+0000..U+00FF per byte).
std::string json_string(std::string_view bytes);

}  // namespace slrr::save_json
