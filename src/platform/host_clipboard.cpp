#include "slrr/host_clipboard.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace slrr {
namespace {
// OpenClipboard fails while another process holds it; retry briefly.
bool open_clipboard() {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (OpenClipboard(nullptr)) return true;
    Sleep(5);
  }
  return false;
}
} // namespace

bool host_clipboard_set_text(std::string_view utf8) {
  const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  if (wide < 0 || (wide == 0 && !utf8.empty())) return false;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (size_t(wide) + 1) * sizeof(wchar_t));
  if (!memory) return false;
  auto *text = static_cast<wchar_t *>(GlobalLock(memory));
  if (!text) {
    GlobalFree(memory);
    return false;
  }
  if (wide) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), text, wide);
  text[wide] = 0;
  GlobalUnlock(memory);
  if (!open_clipboard()) {
    GlobalFree(memory);
    return false;
  }
  EmptyClipboard();
  // On success the system owns the memory.
  const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
  CloseClipboard();
  if (!ok) GlobalFree(memory);
  return ok;
}

std::string host_clipboard_get_text() {
  std::string out;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard()) return out;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto *text = static_cast<const wchar_t *>(GlobalLock(data))) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
      if (bytes > 1) {
        out.resize(size_t(bytes));
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
        out.resize(size_t(bytes) - 1);
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return out;
}
} // namespace slrr
