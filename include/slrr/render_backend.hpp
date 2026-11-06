#pragma once

#include "slrr/scx_mesh.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace slrr {

// Thin GPU backend behind GfxEngine / RenderRef / Viewport / Camera natives.
// Slot name "bgfx" is the product target; Win32 build uses WGL/OpenGL until
// libbgfx is linked (same IRenderBackend API).
class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  virtual const char* name() const = 0;

  virtual bool open(int32_t width, int32_t height, const char* title) = 0;
  virtual void close() = 0;
  virtual bool ready() const = 0;

  virtual void pump(int32_t ms) = 0;
  virtual bool quit_requested() const = 0;
  virtual void request_quit() = 0;

  // GfxEngine.flush() — clear / draw queued work / present.
  virtual void flush() = 0;
  virtual bool request_screenshot(const std::string&) { return false; }

  virtual int32_t width() const = 0;
  virtual int32_t height() const = 0;
};

enum class RenderBackendKind { Null, D3D9, Bgfx };

std::unique_ptr<IRenderBackend> create_render_backend(RenderBackendKind kind);
RenderBackendKind parse_render_backend(const std::string& name);

void bgfx_try_scx_smoke(IRenderBackend& render, const std::string& path);

void bgfx_set_garage_viewport(IRenderBackend& render, bool enabled,
                              int32_t osd_buttons);

void bgfx_set_scx_mesh(IRenderBackend& render, int32_t version, int32_t groups,
                       int32_t vertices, int32_t triangles, const float bbox_min[3],
                       const float bbox_max[3]);

// Upload full SCX geometry (positions/normals/indices) for GPU draw.
// Replaces the whole scene (single-mesh helper).
void bgfx_set_scx_geometry(IRenderBackend& render, ScxGeometry geo);

// Multi-mesh scene (RPK map + stuff, …).
void bgfx_clear_scene(IRenderBackend& render);
void bgfx_add_scx_geometry(IRenderBackend& render, ScxGeometry geo);

// Apply Garage.cameraSetup defaults onto host garage camera.
void bgfx_garage_camera_setup();

// HWND for exclusive FMV (boot intros). Null if no window.
void* bgfx_native_hwnd(IRenderBackend& render);

// Draw OSD even outside garage (MainMenu / Splash).
void bgfx_set_osd_overlay(IRenderBackend& render, bool enabled);

// True once the menu or the garage is allowed to draw OSD. Boot stays false
// until the splash frame, so a loading present cannot flash the clear color.
bool bgfx_osd_draw_enabled(IRenderBackend& render);

}  // namespace slrr
