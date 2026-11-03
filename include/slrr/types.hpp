#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// Opaque script object handle (Invictus instance). Concrete layout lives in jvm/.
struct Object;

enum class ValueTag : uint8_t { Void, Int, Float, Obj };

struct Value {
  ValueTag tag = ValueTag::Void;
  union {
    int32_t i;
    float f;
    Object* o;
  } v{};

  static Value make_void() { return {}; }
  static Value make_int(int32_t x) {
    Value r;
    r.tag = ValueTag::Int;
    r.v.i = x;
    return r;
  }
  static Value make_float(float x) {
    Value r;
    r.tag = ValueTag::Float;
    r.v.f = x;
    return r;
  }
  static Value make_obj(Object* o) {
    Value r;
    r.tag = ValueTag::Obj;
    r.v.o = o;
    return r;
  }
};

}  // namespace slrr
