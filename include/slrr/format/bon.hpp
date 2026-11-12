#pragma once

#include <string>
#include <string_view>

namespace slrr {

struct Skeleton;

// .bon text. The bone lengths that scale a matrixdata direction.
// Same nine-line record as .skl.
bool bon_read(std::string_view text, Skeleton* out, std::string* error);

}  // namespace slrr
