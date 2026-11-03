#include "slrr/host_cursor.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace slrr {
namespace {

std::string g_game_root;
#ifdef _WIN32
// shape 0..9 → HCURSOR from (shape+2).cur; nullptr = not loaded
HCURSOR g_shapes[10]{};
bool g_tried = false;
int32_t g_shape = 0;
char g_letter = 'J';
char g_saved_letter = 'J';
bool g_camera_drag = false;

// Soft PE cursor_shape_of @ jump table 0x4b83d4.
int32_t cursor_shape_of(char c) {
  switch (c) {
    case '1':
    case 'G':
      return 1;
    case '2':
    case 'M':
      return 2;
    case '3':
    case 'W':
      return 3;
    case '4':
    case 'A':
      return 4;
    case '5':
    case 'D':
      return 5;
    case '6':
      return 6;
    case '7':
      return 7;
    case '8':
      return 8;
    case '9':
      return 9;
    default:
      return 0;  // J, B, +, …
  }
}

std::vector<std::string> candidate_dirs() {
  std::vector<std::string> dirs;
  auto add = [&](std::string d) {
    if (d.empty()) return;
    for (char& ch : d)
      if (ch == '/') ch = '\\';
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    dirs.push_back(std::move(d));
  };

  char mod[MAX_PATH]{};
  if (GetModuleFileNameA(nullptr, mod, MAX_PATH)) {
    std::string exe = mod;
    const auto slash = exe.find_last_of("\\/");
    if (slash != std::string::npos) exe.resize(slash);
    add(exe + "\\..\\..\\assets\\StreetLegal_Redline_exe\\Cursor\\0409");
    add(exe + "\\..\\..\\..\\assets\\StreetLegal_Redline_exe\\Cursor\\0409");
  }
  if (!g_game_root.empty()) {
    add(g_game_root + "\\..\\host\\assets\\StreetLegal_Redline_exe\\Cursor\\0409");
  }
  return dirs;
}

void ensure_shapes_loaded() {
  if (g_tried) return;
  g_tried = true;
  const auto dirs = candidate_dirs();
  int loaded = 0;
  for (int shape = 0; shape < 10; ++shape) {
    const int file_id = shape + 2;  // 2.cur .. 11.cur
    for (const std::string& dir : dirs) {
      char path[MAX_PATH]{};
      std::snprintf(path, sizeof(path), "%s\\%d.cur", dir.c_str(), file_id);
      HCURSOR c = LoadCursorFromFileA(path);
      if (c) {
        g_shapes[shape] = c;
        ++loaded;
        break;
      }
    }
  }
  if (loaded > 0)
    std::printf("[cursor] loaded %d/10 shapes (2.cur..11.cur)\n", loaded);
  else
    std::printf("[cursor] shapes miss — IDC_ARROW fallback\n");
}

HCURSOR shape_handle(int32_t shape) {
  ensure_shapes_loaded();
  if (shape < 0 || shape > 9) shape = 0;
  if (g_shapes[shape]) return g_shapes[shape];
  if (g_shapes[0]) return g_shapes[0];
  return LoadCursorA(nullptr, IDC_ARROW);
}
#endif

}  // namespace

void host_cursor_set_search_roots(const std::string& game_root) {
  g_game_root = game_root;
#ifdef _WIN32
  g_tried = false;
  for (HCURSOR& h : g_shapes) h = nullptr;
  g_shape = 0;
  g_letter = 'J';
  g_saved_letter = 'J';
  g_camera_drag = false;
#endif
}

#ifdef _WIN32
HostCursorHandle host_cursor_current() {
  return reinterpret_cast<HostCursorHandle>(shape_handle(g_shape));
}

HostCursorHandle host_cursor_arrow() {
  return reinterpret_cast<HostCursorHandle>(shape_handle(0));
}

void host_cursor_set_pointer(char letter) {
  if (!letter) letter = 'J';
  g_letter = letter;
  if (!g_camera_drag) g_saved_letter = letter;
  g_shape = cursor_shape_of(letter);
  HCURSOR c = shape_handle(g_shape);
  SetCursor(c);
  std::printf("[cursor] pointer='%c' shape=%d file=%d.cur\n", g_letter, g_shape,
              g_shape + 2);
}

char host_cursor_pointer() { return g_letter; }

void host_cursor_begin_camera_drag() {
  if (g_camera_drag) return;
  g_saved_letter = g_letter ? g_letter : 'J';
  g_camera_drag = true;
  host_cursor_set_pointer('M');
}

void host_cursor_end_camera_drag() {
  if (!g_camera_drag) return;
  g_camera_drag = false;
  host_cursor_set_pointer(g_saved_letter ? g_saved_letter : 'J');
}
#endif

}  // namespace slrr
