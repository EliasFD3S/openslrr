#pragma once

#include "slrr/dds_image.hpp"

#include <string>

namespace slrr {

// Soft PE CRT_TEXTURE_PTX: extract primary JPEG → RGBA8 (via stb_image when
// libbgfx is linked). Falls back to empty on miss / unsupported.
DdsImage load_ptx_file(const std::string& path);

// Load .ptx or .dds by extension.
DdsImage load_texture_file(const std::string& path);

}  // namespace slrr
