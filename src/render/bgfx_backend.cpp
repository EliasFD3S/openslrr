#include "slrr/render_backend.hpp"

#include <cstdio>
#include <memory>

namespace slrr {
std::unique_ptr<IRenderBackend> create_lib_bgfx_backend();

namespace {

class NullBackend final : public IRenderBackend {
 public:
  const char* name() const override { return "null"; }

  bool open(int32_t width, int32_t height, const char* /*title*/) override {
    width_ = width > 0 ? width : 800;
    height_ = height > 0 ? height : 600;
    ready_ = true;
    quit_ = false;
    return true;
  }

  void close() override { ready_ = false; }
  bool ready() const override { return ready_; }
  void pump(int32_t /*ms*/) override {}
  bool quit_requested() const override { return quit_; }
  void request_quit() override { quit_ = true; }
  void flush() override { ++frames_; }
  int32_t width() const override { return width_; }
  int32_t height() const override { return height_; }

 private:
  bool ready_ = false;
  bool quit_ = false;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t frames_ = 0;
};

}  // namespace

std::unique_ptr<IRenderBackend> create_render_backend(RenderBackendKind kind) {
  switch (kind) {
    case RenderBackendKind::Null:
      return std::make_unique<NullBackend>();
    case RenderBackendKind::D3D9:
      std::fprintf(stderr, "[render] d3d9 not wired; using null\n");
      return std::make_unique<NullBackend>();
    case RenderBackendKind::Bgfx:
      return create_lib_bgfx_backend();
  }
  return std::make_unique<NullBackend>();
}

}  // namespace slrr
