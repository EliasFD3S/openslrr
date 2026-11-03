#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// TUFA = Invictus class format (not HotSpot bytecode).
// Spec aligned with repos/SLRR-JVM-decompiler/src/tufa.ts (Build 940 OP_FLAGS).

constexpr uint32_t kTufaNone = 0xffffffffu;

// TREE opcode width table: (flags >> 12) != 0 ⇒ node carries u32 immediate.
extern const uint16_t kTreeOpFlags[48];

enum class TufaConstKind : uint8_t {
  Utf8 = 0,
  Resource = 3,
  Class = 4,
  Reference = 5,
  NameAndType = 7,
};

struct TufaConstant {
  TufaConstKind kind = TufaConstKind::Utf8;
  std::string text;       // Utf8
  uint32_t a = 0;         // pack/name/owner index
  uint32_t b = 0;         // id / descriptor / nameAndType
};

struct TufaMember {
  uint32_t flags = 0;
  uint32_t name = 0;
  uint32_t descriptor = 0;
  uint32_t tree = kTufaNone;
  uint32_t locals = 0;  // methods only
};

struct TufaNode {
  uint8_t op = 0;
  uint16_t line = 0;
  bool wide = false;
  uint32_t imm = 0;
};

struct TufaClass {
  uint32_t build = 0;
  std::vector<TufaConstant> constants;
  std::vector<TufaMember> static_fields;
  std::vector<TufaMember> instance_fields;
  std::vector<TufaMember> static_methods;
  std::vector<TufaMember> instance_methods;
  uint32_t name_index = kTufaNone;
  uint32_t super_index = kTufaNone;
  std::vector<uint32_t> interfaces;
  bool is_interface = false;
  std::vector<std::vector<TufaNode>> trees;

  const std::string& utf8(uint32_t index) const;
  std::string class_name(uint32_t index) const;
  std::string name() const;
  std::string super_name() const;  // empty if none

  struct Ref {
    std::string owner;
    std::string name;
    std::string descriptor;
  };
  Ref reference(uint32_t index) const;

  // CONS kind Resource: pack utf8 path + local type id.
  struct Resource {
    std::string pack;
    uint32_t id = 0;
  };
  Resource resource(uint32_t index) const;
};

// One on-disk .class may contain several concatenated TUFA blobs.
bool tufa_parse(const uint8_t* data, size_t size, std::vector<TufaClass>* out,
                std::string* err);
bool tufa_load_file(const std::string& path, std::vector<TufaClass>* out,
                    std::string* err);

}  // namespace slrr
