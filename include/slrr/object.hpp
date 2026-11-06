#pragma once

#include "slrr/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace slrr {

// Minimal script instance. Identity = pointer (Object.equals).
struct Object {
  bool gc_disabled = false;
  bool gc_finalized = false;
  uint32_t identity_hash = 0;
  std::string class_name;
  std::unordered_map<std::string, Value> fields;
  // java.lang.String payload (Invictus string objects).
  std::string str;
  // java.io.FindFile state.
  std::vector<std::string> find_entries;
  size_t find_index = 0;
  // `new T[n]` (TREE expr 31). `fields["length"]` is the script-visible size.
  bool is_array = false;
  std::vector<Value> slots;
};

}  // namespace slrr
