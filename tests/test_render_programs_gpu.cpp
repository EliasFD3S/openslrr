// Windows/D3D11 integration: exercise actual bgfx shader-reference ownership.
#include "../src/render/bgfx_lib_backend.hpp"
#include "../src/render/bgfx_program.hpp"
#include "dxbc/fs_mesh.sc.bin.h"
#include "dxbc/fs_osd_text.sc.bin.h"
#include "dxbc/vs_mesh.sc.bin.h"
#include "save_io_fixture.hpp"
using namespace slrr;

// Change an unused interface hash, not executable bytecode, so bgfx cannot
// hide leaked references by deduplicating these against the backend shaders.
static uint32_t serial = 0x10500000;
template <size_t N>
bgfx::ShaderHandle shader(const uint8_t (&bytes)[N], bool vertex) {
  std::vector<uint8_t> unique(bytes, bytes + N);
  const auto tag = ++serial;
  const size_t offset =
      vertex ? 4 : 8; // input hash for VS / output hash for FS.
  std::memcpy(unique.data() + offset, &tag, sizeof(tag));
  return bgfx::createShader(bgfx::copy(unique.data(), uint32_t(unique.size())));
}
int main() {
  LibBgfxBackend backend;
  try {
    SLRR_REQUIRE(backend.open(640, 480, "Shader lifetime regression"));
    SLRR_REQUIRE(bgfx::getRendererType() == bgfx::RendererType::Direct3D11);
    ShowWindow(static_cast<HWND>(backend.native_hwnd()), SW_HIDE);
    auto settle = [&]() {
      for (int i = 0; i < 8; ++i)
        bgfx::frame();
      return bgfx::getStats()->numShaders;
    };
    const auto baseline = settle();
    // Reproduce the original early-rejection leak, then clean it explicitly.
    auto vertex = shader(vs_mesh_dxbc, true);
    auto fragment = shader(fs_osd_text_dxbc, false);
    SLRR_REQUIRE(bgfx::isValid(vertex) && bgfx::isValid(fragment));
    auto old = bgfx::createProgram(vertex, fragment, true);
    SLRR_REQUIRE(!bgfx::isValid(old));
    const auto leaked = settle();
    SLRR_REQUIRE(leaked == baseline + 2);
    bgfx::destroy(vertex);
    bgfx::destroy(fragment);
    SLRR_REQUIRE(settle() == baseline);
    for (int i = 0; i < 32; ++i) {
      auto linked = create_owned_program(shader(vs_mesh_dxbc, true),
                                         shader(fs_mesh_dxbc, false));
      SLRR_REQUIRE(bgfx::isValid(linked));
      bgfx::destroy(linked);
      auto mismatch = create_owned_program(shader(vs_mesh_dxbc, true),
                                           shader(fs_osd_text_dxbc, false));
      SLRR_REQUIRE(!bgfx::isValid(mismatch));
      auto missing_fs =
          create_owned_program(shader(vs_mesh_dxbc, true), BGFX_INVALID_HANDLE);
      auto missing_vs = create_owned_program(BGFX_INVALID_HANDLE,
                                             shader(fs_mesh_dxbc, false));
      SLRR_REQUIRE(!bgfx::isValid(missing_fs) && !bgfx::isValid(missing_vs));
      SLRR_REQUIRE(!bgfx::isValid(
          create_owned_program(BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE)));
    }
    const auto after = settle();
    std::printf(
        "shader ownership: baseline=%u legacy rejected=%u after 32 cycles=%u\n",
        baseline, leaked, after);
    SLRR_REQUIRE(after == baseline);
    backend.close();
    // Reopening must recreate only live rendering programs/resources.
    SLRR_REQUIRE(backend.open(640, 480, "Shader reopen regression"));
    ShowWindow(static_cast<HWND>(backend.native_hwnd()), SW_HIDE);
    SLRR_REQUIRE(settle() == baseline);
    backend.close();
    std::puts("PASS shader success/rejection/missing-stage ownership and "
              "backend reopen");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "render_programs_gpu: %s\n", e.what());
    if (backend.ready())
      backend.close();
    return 1;
  }
}
