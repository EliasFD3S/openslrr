#pragma once
#include <bgfx/bgfx.h>

namespace slrr {
// Consume both caller-owned shader handles even when linking rejects a pair.
// createProgram(..., true) does not take ownership on every failure path in
// the pinned bgfx. A successful program holds its own shader references.
inline bgfx::ProgramHandle create_owned_program(bgfx::ShaderHandle vertex,
                                                bgfx::ShaderHandle fragment) {
  bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
  if (bgfx::isValid(vertex) && bgfx::isValid(fragment))
    program = bgfx::createProgram(vertex, fragment, false);
  if (bgfx::isValid(vertex))
    bgfx::destroy(vertex);
  if (bgfx::isValid(fragment))
    bgfx::destroy(fragment);
  return program;
}
} // namespace slrr
