#include "slrr/render_backend.hpp"

#include "slrr/host_cursor.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace slrr {
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
}  // namespace slrr

#ifndef SLRR_HAS_LIBBGFX
#include "slrr/dds_image.hpp"
#include "slrr/garage_camera.hpp"
#include "slrr/host_input.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <gl/GL.h>

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F2
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

using PFNGLCOMPRESSEDTEXIMAGE2DPROC = void(APIENTRY*)(GLenum, GLint, GLenum,
                                                      GLsizei, GLsizei, GLint,
                                                      GLsizei, const void*);
#endif

namespace slrr {

namespace {

#ifdef _WIN32

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

class BgfxBackend final : public IRenderBackend {
 public:
  const char* name() const override { return "bgfx"; }

  bool open(int32_t width, int32_t height, const char* title) override {
    width_ = width > 0 ? width : 800;
    height_ = height > 0 ? height : 600;

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = &BgfxBackend::wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hIcon = LoadIconA(wc.hInstance, "IDI_APP");
    if (!wc.hIcon) wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = reinterpret_cast<HCURSOR>(host_cursor_current());
    wc.lpszClassName = "slrr_host_bgfx";
    RegisterClassExA(&wc);

    hwnd_ = CreateWindowExA(
        0, wc.lpszClassName, title ? title : "SLRR host",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        width_, height_, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
      std::fprintf(stderr, "[bgfx] CreateWindow failed\n");
      return false;
    }

    hdc_ = GetDC(hwnd_);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    const int pf = ChoosePixelFormat(hdc_, &pfd);
    if (!pf || !SetPixelFormat(hdc_, pf, &pfd)) {
      std::fprintf(stderr, "[bgfx] SetPixelFormat failed — GDI fallback\n");
      gl_ok_ = false;
    } else {
      hglrc_ = wglCreateContext(hdc_);
      if (!hglrc_ || !wglMakeCurrent(hdc_, hglrc_)) {
        std::fprintf(stderr, "[bgfx] wglCreateContext failed — GDI fallback\n");
        gl_ok_ = false;
      } else {
        gl_ok_ = true;
        glEnable(GL_DEPTH_TEST);
        // Garage SCX is two-sided — stock does not cull map faces.
        glDisable(GL_CULL_FACE);
        glShadeModel(GL_SMOOTH);
        glClearColor(0.05f, 0.07f, 0.10f, 1.f);
        std::printf("[bgfx] open %dx%d GPU=WGL/OpenGL (libbgfx pending)\n",
                    width_, height_);
      }
    }

    ready_ = true;
    quit_ = false;
    if (!gl_ok_)
      std::printf("[bgfx] open %dx%d (Win32 GDI path)\n", width_, height_);
    return true;
  }

  void close() override {
    destroy_textures();
    if (hglrc_) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(hglrc_);
      hglrc_ = nullptr;
    }
    if (hdc_ && hwnd_) {
      ReleaseDC(hwnd_, hdc_);
      hdc_ = nullptr;
    }
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
    ready_ = false;
  }

  bool ready() const override { return ready_; }

  void pump(int32_t /*ms*/) override {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        quit_ = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    host_input_poll_keys();
  }

  bool quit_requested() const override { return quit_; }
  void request_quit() override { quit_ = true; }

  void flush() override {
    ++frames_;
    if (!hwnd_) return;
    if (gl_ok_ && hglrc_) {
      flush_gl();
      return;
    }
    flush_gdi();
  }

  int32_t width() const override { return width_; }
  int32_t height() const override { return height_; }

  void set_scx_smoke(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      std::printf("[bgfx] SCX smoke miss %s\n", path.c_str());
      scx_ok_ = false;
      return;
    }
    const auto sz = static_cast<long long>(in.tellg());
    scx_ok_ = sz > 0;
    std::printf("[bgfx] SCX smoke %s bytes=%lld ok=%d\n", path.c_str(), sz,
                scx_ok_ ? 1 : 0);
  }

  void* native_hwnd() { return hwnd_; }
  void set_osd_overlay(bool enabled) { osd_overlay_ = enabled; }

  void set_garage_viewport(bool enabled, int32_t osd_buttons) {
    garage_ = enabled;
    osd_buttons_ = osd_buttons;
    if (enabled && !garage_camera().active) garage_camera_setup_defaults();
    std::printf("[bgfx] garage viewport=%d osd_buttons=%d gpu=%d cam=%d\n",
                enabled ? 1 : 0, osd_buttons, gl_ok_ ? 1 : 0,
                garage_camera().active ? 1 : 0);
  }

  void set_scx_mesh(int32_t version, int32_t groups, int32_t vertices,
                    int32_t triangles, const float bbox_min[3],
                    const float bbox_max[3]) {
    mesh_ok_ = vertices > 0 || triangles > 0;
    mesh_ver_ = version;
    mesh_groups_ = groups;
    mesh_verts_ = vertices;
    mesh_tris_ = triangles;
    if (bbox_min && bbox_max) {
      for (int i = 0; i < 3; ++i) {
        mesh_min_[i] = bbox_min[i];
        mesh_max_[i] = bbox_max[i];
      }
    }
    scx_ok_ = scx_ok_ || mesh_ok_;
    std::printf(
        "[bgfx] SCX mesh v=%d groups=%d verts=%d tris=%d ok=%d\n", version,
        groups, vertices, triangles, mesh_ok_ ? 1 : 0);
  }

  void set_scx_geometry(ScxGeometry geo) {
    destroy_textures();
    geo_ = std::move(geo);
    if (geo_.info.ok) {
      set_scx_mesh(static_cast<int32_t>(geo_.info.version), geo_.info.groups,
                   geo_.info.vertices, geo_.info.triangles, geo_.info.bbox_min,
                   geo_.info.bbox_max);
      std::printf(
          "[bgfx] SCX geometry uploaded cpu verts=%zu idx=%zu groups=%zu\n",
          geo_.positions.size() / 3, geo_.indices.size(), geo_.groups.size());
      load_textures_for_scx();
    }
  }

  static BgfxBackend* as_bgfx(IRenderBackend& r) {
    if (std::strcmp(r.name(), "bgfx") != 0) return nullptr;
    return static_cast<BgfxBackend*>(&r);
  }

 private:
  void flush_gl() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right > 0 ? rc.right : width_;
    const int h = rc.bottom > 0 ? rc.bottom : height_;
    width_ = w;
    height_ = h;

    wglMakeCurrent(hdc_, hglrc_);
    glViewport(0, 0, w, h);
    if (garage_)
      glClearColor(0.11f, 0.09f, 0.08f, 1.f);
    else
      glClearColor(0.05f, 0.07f, 0.10f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!geo_.positions.empty() && !geo_.indices.empty()) {
      draw_mesh_gl(w, h);
    } else if (mesh_ok_ || scx_ok_) {
      // Placeholder box if summary only.
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      glFrustum(-0.5, 0.5, -0.5, 0.5, 1.0, 100.0);
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glTranslatef(0, 0, -4);
      glColor3f(0.35f, 0.55f, 0.75f);
      glBegin(GL_QUADS);
      glVertex3f(-1, -0.5f, 0);
      glVertex3f(1, -0.5f, 0);
      glVertex3f(1, 0.5f, 0);
      glVertex3f(-1, 0.5f, 0);
      glEnd();
    }

    if (garage_ || osd_overlay_) draw_osd_gl(w, h);

    SwapBuffers(hdc_);
  }

  void draw_mesh_gl(int w, int h) {
    constexpr float kScale = 0.01f;
    float cx = 0.5f * (mesh_min_[0] + mesh_max_[0]) * kScale;
    float cy = 0.5f * (mesh_min_[1] + mesh_max_[1]) * kScale;
    float cz = 0.5f * (mesh_min_[2] + mesh_max_[2]) * kScale;
    float dx = (mesh_max_[0] - mesh_min_[0]) * kScale;
    float dy = (mesh_max_[1] - mesh_min_[1]) * kScale;
    float dz = (mesh_max_[2] - mesh_min_[2]) * kScale;
    float radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
    if (radius < 0.5f) radius = 0.5f;
    const float dist = radius * 2.2f;
    const float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h)
                               : 1.333f;

    float eye_x = cx + dist * 0.55f;
    float eye_y = cy + dist * 0.35f;
    float eye_z = cz + dist * 0.75f;

    if (garage_camera().active) {
      float view[16];
      float proj[16];
      garage_camera_matrices(view, proj, aspect, false);
      glMatrixMode(GL_PROJECTION);
      glLoadMatrixf(proj);
      glMatrixMode(GL_MODELVIEW);
      glLoadMatrixf(view);
      const auto& cam = garage_camera();
      const float cyaw = std::cos(cam.yaw);
      const float syaw = std::sin(cam.yaw);
      const float cp = std::cos(cam.pitch);
      const float sp = std::sin(cam.pitch);
      eye_x = cam.look_x + cam.dist * cp * syaw;
      eye_y = cam.look_y + cam.dist * sp;
      eye_z = cam.look_z + cam.dist * cp * cyaw;
    } else {
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      const float near_z = 0.1f;
      const float far_z = dist + radius * 4.f;
      const float fovy = 45.f * 3.14159265f / 180.f;
      const float top = near_z * std::tan(fovy * 0.5f);
      const float right = top * aspect;
      glFrustum(-right, right, -top, top, near_z, far_z);
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      look_at(eye_x, eye_y, eye_z, cx, cy, cz, 0.f, 1.f, 0.f);
    }

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    const float light_pos[] = {eye_x, eye_y + radius, eye_z, 1.f};
    const float light_diff[] = {0.9f, 0.9f, 0.95f, 1.f};
    const float light_amb[] = {0.35f, 0.32f, 0.3f, 1.f};
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diff);
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_amb);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    if (!geo_.uvs.empty()) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glPushMatrix();
    glScalef(kScale, kScale, kScale);
    glVertexPointer(3, GL_FLOAT, 0, geo_.positions.data());
    glNormalPointer(GL_FLOAT, 0, geo_.normals.data());
    if (!geo_.uvs.empty())
      glTexCoordPointer(2, GL_FLOAT, 0, geo_.uvs.data());

    if (!geo_.groups.empty()) {
      for (const ScxGroup& g : geo_.groups) {
        GLuint tex = 0;
        if (g.texture_index != 0xffff &&
            g.texture_index < tex_ids_.size())
          tex = tex_ids_[g.texture_index];
        else if (!tex_ids_.empty())
          tex = tex_ids_[0];
        if (tex) {
          glBindTexture(GL_TEXTURE_2D, tex);
          glColor4f(1.f, 1.f, 1.f, 1.f);
        } else {
          glBindTexture(GL_TEXTURE_2D, 0);
          glColor4fv(g.diffuse);
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g.index_count),
                       GL_UNSIGNED_INT,
                       geo_.indices.data() + g.index_start);
      }
    } else {
      glBindTexture(GL_TEXTURE_2D, tex_ids_.empty() ? 0 : tex_ids_[0]);
      glColor4f(1.f, 1.f, 1.f, 1.f);
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(geo_.indices.size()),
                     GL_UNSIGNED_INT, geo_.indices.data());
    }

    glPopMatrix();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!geo_.uvs.empty()) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);

    // Floor plane under mesh (meters).
    glColor3f(0.22f, 0.19f, 0.16f);
    const float fy = mesh_min_[1] * kScale - 0.02f;
    const float hs = radius * 1.5f;
    glBegin(GL_QUADS);
    glVertex3f(cx - hs, fy, cz - hs);
    glVertex3f(cx + hs, fy, cz - hs);
    glVertex3f(cx + hs, fy, cz + hs);
    glVertex3f(cx - hs, fy, cz + hs);
    glEnd();
  }

  void draw_osd_gl(int w, int h) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    glColor3f(0.07f, 0.07f, 0.09f);
    glBegin(GL_QUADS);
    glVertex2i(0, 0);
    glVertex2i(w, 0);
    glVertex2i(w, (h * 12) / 100);
    glVertex2i(0, (h * 12) / 100);
    glEnd();

    const int n = osd_buttons_ > 0 ? osd_buttons_ : 8;
    const int bw = w / (n + 2);
    glColor3f(0.22f, 0.35f, 0.55f);
    for (int i = 0; i < n; ++i) {
      const int x0 = (i + 1) * bw;
      const int y0 = (h * 2) / 100;
      const int x1 = x0 + bw - 4;
      const int y1 = (h * 10) / 100;
      glBegin(GL_QUADS);
      glVertex2i(x0, y0);
      glVertex2i(x1, y0);
      glVertex2i(x1, y1);
      glVertex2i(x0, y1);
      glEnd();
    }
    glEnable(GL_DEPTH_TEST);
  }

  void destroy_textures() {
    if (!tex_ids_.empty() && hglrc_ && hdc_) {
      wglMakeCurrent(hdc_, hglrc_);
      glDeleteTextures(static_cast<GLsizei>(tex_ids_.size()), tex_ids_.data());
    }
    tex_ids_.clear();
  }

  void load_textures_for_scx() {
    if (!gl_ok_ || !hglrc_ || geo_.info.path.empty()) return;
    wglMakeCurrent(hdc_, hglrc_);
    if (!glCompressedTexImage2D_) {
      glCompressedTexImage2D_ = reinterpret_cast<PFNGLCOMPRESSEDTEXIMAGE2DPROC>(
          wglGetProcAddress("glCompressedTexImage2D"));
    }

    std::vector<std::string> paths = geo_.textures;
    if (paths.empty()) {
      std::string dir = geo_.info.path;
      for (char& c : dir)
        if (c == '/') c = '\\';
      const auto slash = dir.find_last_of('\\');
      if (slash == std::string::npos) return;
      dir.resize(slash + 1);
#ifdef _WIN32
      WIN32_FIND_DATAA fd{};
      HANDLE h = FindFirstFileA((dir + "*.dds").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
          paths.push_back(dir + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
      }
#endif
      std::sort(paths.begin(), paths.end());
    }
    std::printf("[bgfx] DDS slots=%zu (RPK=%d)\n", paths.size(),
                geo_.textures.empty() ? 0 : 1);
    tex_ids_.assign(paths.size(), 0);
    int loaded = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
      std::string path = paths[i];
      for (char& c : path)
        if (c == '/') c = '\\';
      if (path.size() > 1 && path[1] != ':') {
        std::string base = geo_.info.path;
        for (char& c : base)
          if (c == '/') c = '\\';
        const auto gpos = base.find("\\game\\");
        if (gpos != std::string::npos)
          path = base.substr(0, gpos + 6) + path;
      }
      DdsImage img = load_dds_file(path);
      if (!img.ok) {
        std::printf("[bgfx] DDS miss %s (%s)\n", path.c_str(),
                    img.error.c_str());
        continue;
      }
      GLuint id = 0;
      glGenTextures(1, &id);
      glBindTexture(GL_TEXTURE_2D, id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
      bool ok_upload = false;
      if (img.format == DdsFormat::DXT1 || img.format == DdsFormat::DXT3 ||
          img.format == DdsFormat::DXT5) {
        if (!glCompressedTexImage2D_) {
          std::printf("[bgfx] no glCompressedTexImage2D\n");
        } else {
          GLenum ifmt = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
          if (img.format == DdsFormat::DXT3)
            ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
          if (img.format == DdsFormat::DXT5)
            ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
          glCompressedTexImage2D_(GL_TEXTURE_2D, 0, ifmt,
                                  static_cast<GLsizei>(img.width),
                                  static_cast<GLsizei>(img.height), 0,
                                  static_cast<GLsizei>(img.mip0_size),
                                  img.data.data());
          ok_upload = true;
        }
      } else if (img.format == DdsFormat::RGBA8) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     static_cast<GLsizei>(img.width),
                     static_cast<GLsizei>(img.height), 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, img.data.data());
        ok_upload = true;
      } else if (img.format == DdsFormat::RGB8) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     static_cast<GLsizei>(img.width),
                     static_cast<GLsizei>(img.height), 0, GL_BGR,
                     GL_UNSIGNED_BYTE, img.data.data());
        ok_upload = true;
      }
      glBindTexture(GL_TEXTURE_2D, 0);
      if (ok_upload) {
        tex_ids_[i] = id;
        ++loaded;
      } else {
        glDeleteTextures(1, &id);
      }
    }
    std::printf("[bgfx] DDS uploaded %d / %zu\n", loaded, paths.size());
  }

  static void look_at(float ex, float ey, float ez, float cx, float cy,
                      float cz, float ux, float uy, float uz) {
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) fl = 1.f;
    fx /= fl;
    fy /= fl;
    fz /= fl;
    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6f) sl = 1.f;
    sx /= sl;
    sy /= sl;
    sz /= sl;
    float ux2 = sy * fz - sz * fy;
    float uy2 = sz * fx - sx * fz;
    float uz2 = sx * fy - sy * fx;
    float m[16] = {sx, ux2, -fx, 0, sy, uy2, -fy, 0, sz, uz2, -fz, 0, 0, 0, 0,
                   1};
    glMultMatrixf(m);
    glTranslatef(-ex, -ey, -ez);
  }

  void flush_gdi() {
    HDC hdc = GetDC(hwnd_);
    if (!hdc) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    HBRUSH br = CreateSolidBrush(garage_ ? RGB(28, 24, 20) : RGB(12, 18, 28));
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    if (garage_) {
      RECT floor{0, (h * 55) / 100, w, h};
      HBRUSH fb = CreateSolidBrush(RGB(48, 42, 36));
      FillRect(hdc, &floor, fb);
      DeleteObject(fb);
      RECT mesh_rc{(w * 20) / 100, (h * 22) / 100, (w * 80) / 100,
                   (h * 58) / 100};
      HBRUSH mb = CreateSolidBrush(mesh_ok_ ? RGB(90, 110, 130) : RGB(70, 70, 80));
      FillRect(hdc, &mesh_rc, mb);
      DeleteObject(mb);
    }
    ReleaseDC(hwnd_, hdc);
  }

  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
      SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self =
        reinterpret_cast<BgfxBackend*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    auto& in = host_input();
    if (msg == WM_SETCURSOR) {
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(reinterpret_cast<HCURSOR>(host_cursor_current()));
        return TRUE;
      }
      return DefWindowProcA(hwnd, msg, wp, lp);
    }
    if (msg == WM_RBUTTONDOWN) {
      SetCapture(hwnd);
      in.rmb_down = true;
      host_cursor_begin_camera_drag();
      return 0;
    }
    if (msg == WM_RBUTTONUP) {
      in.rmb_down = false;
      host_cursor_end_camera_drag();
      ReleaseCapture();
      return 0;
    }
    if (msg == WM_MOUSEMOVE) {
      const int x = static_cast<short>(LOWORD(lp));
      const int y = static_cast<short>(HIWORD(lp));
      if (self && self->have_mouse_) {
        const float dx = static_cast<float>(x - self->mouse_x_) / 200.f;
        const float dy = static_cast<float>(y - self->mouse_y_) / 200.f;
        if (in.rmb_down) {
          in.look_yaw -= dx;
          in.look_pitch -= dy;
        }
      }
      if (self) {
        self->mouse_x_ = x;
        self->mouse_y_ = y;
        self->have_mouse_ = true;
      }
      return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
      in.look_zoom +=
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
          static_cast<float>(WHEEL_DELTA);
      return 0;
    }
    if (msg == WM_CLOSE) {
      if (self) self->quit_ = true;
      DestroyWindow(hwnd);
      return 0;
    }
    if (msg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
  }

  HWND hwnd_ = nullptr;
  HDC hdc_ = nullptr;
  HGLRC hglrc_ = nullptr;
  bool gl_ok_ = false;
  bool ready_ = false;
  bool quit_ = false;
  int mouse_x_ = 0;
  int mouse_y_ = 0;
  bool have_mouse_ = false;
  bool scx_ok_ = false;
  bool mesh_ok_ = false;
  bool garage_ = false;
  bool osd_overlay_ = false;
  int32_t osd_buttons_ = 0;
  int32_t mesh_ver_ = 0;
  int32_t mesh_groups_ = 0;
  int32_t mesh_verts_ = 0;
  int32_t mesh_tris_ = 0;
  float mesh_min_[3] = {0, 0, 0};
  float mesh_max_[3] = {0, 0, 0};
  ScxGeometry geo_;
  std::vector<GLuint> tex_ids_;
  PFNGLCOMPRESSEDTEXIMAGE2DPROC glCompressedTexImage2D_ = nullptr;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t frames_ = 0;
};

#else  // !_WIN32

class BgfxBackend final : public IRenderBackend {
 public:
  const char* name() const override { return "bgfx"; }
  bool open(int32_t w, int32_t h, const char*) override {
    width_ = w;
    height_ = h;
    ready_ = true;
    return true;
  }
  void close() override { ready_ = false; }
  bool ready() const override { return ready_; }
  void pump(int32_t) override {}
  bool quit_requested() const override { return quit_; }
  void request_quit() override { quit_ = true; }
  void flush() override { ++frames_; }
  int32_t width() const override { return width_; }
  int32_t height() const override { return height_; }
  void set_scx_smoke(const std::string&) {}
  void set_garage_viewport(bool, int32_t) {}
  void set_osd_overlay(bool) {}
  void* native_hwnd() { return nullptr; }
  void set_scx_mesh(int32_t, int32_t, int32_t, int32_t, const float*,
                    const float*) {}
  void set_scx_geometry(ScxGeometry) {}
  static BgfxBackend* as_bgfx(IRenderBackend& r) {
    return std::strcmp(r.name(), "bgfx") == 0 ? static_cast<BgfxBackend*>(&r)
                                              : nullptr;
  }

 private:
  bool ready_ = false;
  bool quit_ = false;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t frames_ = 0;
};

#endif  // _WIN32 else stub BgfxBackend

}  // namespace

void bgfx_try_scx_smoke(IRenderBackend& render, const std::string& path) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render)) b->set_scx_smoke(path);
}

void bgfx_set_garage_viewport(IRenderBackend& render, bool enabled,
                              int32_t osd_buttons) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render))
    b->set_garage_viewport(enabled, osd_buttons);
}

void bgfx_set_scx_mesh(IRenderBackend& render, int32_t version, int32_t groups,
                       int32_t vertices, int32_t triangles,
                       const float bbox_min[3], const float bbox_max[3]) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render))
    b->set_scx_mesh(version, groups, vertices, triangles, bbox_min, bbox_max);
}

void bgfx_set_scx_geometry(IRenderBackend& render, ScxGeometry geo) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render))
    b->set_scx_geometry(std::move(geo));
}

void bgfx_clear_scene(IRenderBackend& /*render*/) {}

void bgfx_add_scx_geometry(IRenderBackend& render, ScxGeometry geo) {
  // WGL path: last add wins (single mesh). Prefer libbgfx for multi-mesh RPK.
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render))
    b->set_scx_geometry(std::move(geo));
}

void bgfx_garage_camera_setup() { garage_camera_setup_defaults(); }

void* bgfx_native_hwnd(IRenderBackend& render) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render)) return b->native_hwnd();
  return nullptr;
}

void bgfx_set_osd_overlay(IRenderBackend& render, bool enabled) {
  if (BgfxBackend* b = BgfxBackend::as_bgfx(render))
    b->set_osd_overlay(enabled);
}

}  // namespace slrr

#endif  // !SLRR_HAS_LIBBGFX

#ifdef SLRR_HAS_LIBBGFX
namespace slrr {
std::unique_ptr<IRenderBackend> create_lib_bgfx_backend();
}  // namespace slrr
#endif

namespace slrr {

RenderBackendKind parse_render_backend(const std::string& name) {
  if (name == "d3d9" || name == "D3D9") return RenderBackendKind::D3D9;
  if (name == "bgfx" || name == "Bgfx" || name == "gl" || name == "opengl")
    return RenderBackendKind::Bgfx;
  return RenderBackendKind::Null;
}

std::unique_ptr<IRenderBackend> create_render_backend(RenderBackendKind kind) {
  switch (kind) {
    case RenderBackendKind::Null:
      return std::make_unique<NullBackend>();
    case RenderBackendKind::D3D9:
      std::fprintf(stderr, "[render] d3d9 not wired; using null\n");
      return std::make_unique<NullBackend>();
    case RenderBackendKind::Bgfx:
#ifdef SLRR_HAS_LIBBGFX
      return create_lib_bgfx_backend();
#else
      return std::make_unique<BgfxBackend>();
#endif
  }
  return std::make_unique<NullBackend>();
}

}  // namespace slrr
