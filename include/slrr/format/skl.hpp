#pragma once

#include <string>
#include <string_view>

namespace slrr {

struct Skeleton;

// .skl text. Nine lines per bone, the same record as .bon.
// Fills bones and the parent-before-child order. The inverse bind is not
// part of the file.
bool skl_read(std::string_view text, Skeleton* out, std::string* error);

}  // namespace slrr
