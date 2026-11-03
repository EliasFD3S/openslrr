#pragma once

#include <string>

namespace slrr {

// Stock cursors from StreetLegal_Redline.exe → host/assets/.../Cursor/0409/{2..11}.cur
// PE: LoadCursor group 116..121,123..126 ↔ files 2..11; shape index n → file (n+2).cur
// Letter→shape: soft PE jump table 0x4b83d4 (NativeTypes_part9 cursor_shape_of).
void host_cursor_set_search_roots(const std::string& game_root);

#ifdef _WIN32
using HostCursorHandle = void*;  // opaque HCURSOR (avoid windows.h vs bx)

HostCursorHandle host_cursor_current();
HostCursorHandle host_cursor_arrow();  // default 'J' → 2.cur

// MouseCursor.setPointer(Frontend.pointers, letter) — single ASCII char.
void host_cursor_set_pointer(char letter);
char host_cursor_pointer();

// Garage EC_RDRAGBEGIN/END: temporary "M" over the mode letter.
void host_cursor_begin_camera_drag();
void host_cursor_end_camera_drag();
#endif

}  // namespace slrr
