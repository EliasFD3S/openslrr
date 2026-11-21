#pragma once
#include <string>
#include <string_view>

namespace slrr {
// Windows clipboard as UTF-8 text (CF_UNICODETEXT). The bgfx Dear ImGui
// build defines IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS, so ImGui's
// own clipboard stays inside the process; the debug overlay routes it here.
bool host_clipboard_set_text(std::string_view utf8);
// Empty when the clipboard holds no text or cannot be opened.
std::string host_clipboard_get_text();
} // namespace slrr
