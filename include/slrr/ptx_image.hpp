#pragma once

#include "slrr/dds_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// Soft PE CRT_TEXTURE_PTX: extract primary JPEG → RGBA8 (via stb_image when
// libbgfx is linked). Falls back to empty on miss / unsupported.
DdsImage load_ptx_file(const std::string& path);

// PNG/JPEG/TGA/BMP bytes to straight (not premultiplied) RGBA8, rows
// top-down. False without stb_image (no libbgfx) or for other data.
bool decode_image_rgba(const uint8_t* data, size_t size, uint32_t* width,
                       uint32_t* height, std::vector<uint8_t>* rgba);

// Load .ptx or .dds by extension.
DdsImage load_texture_file(const std::string& path);

}  // namespace slrr
