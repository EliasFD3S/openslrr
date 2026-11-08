#include "video_fmv_vlc.hpp"

#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <malloc.h>
#include <mutex>
#include <new>

namespace slrr {
namespace {

// LibVLC 3 ABI. Load it at runtime so the Win32 build and systems using the
// Windows codecs can keep their existing setup.
struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;
using Player = libvlc_media_player_t;
using Lock = void* (*)(void*, void**);
using Unlock = void (*)(void*, void*, void* const*);
using Display = void (*)(void*, void*);
using Format = unsigned (*)(void**, char*, unsigned*, unsigned*, unsigned*,
                            unsigned*);
using Cleanup = void (*)(void*);
enum class PlayerState {
  Idle, Opening, Buffering, Playing, Paused, Stopped, Ended, Error
};

std::wstring environment(const wchar_t* name) {
  const DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
  if (!capacity) return {};
  std::wstring value(capacity, L'\0');
  const DWORD size = GetEnvironmentVariableW(name, value.data(), capacity);
  if (!size || size >= capacity) return {};
  value.resize(size);
  return value;
}

struct VlcApi {
  HMODULE dll = nullptr;
  libvlc_instance_t* instance = nullptr;
  libvlc_instance_t* (*init)(int, const char* const*) = nullptr;
  void (*release)(libvlc_instance_t*) = nullptr;
  libvlc_media_t* (*media_new_path)(libvlc_instance_t*, const char*) = nullptr;
  void (*media_release)(libvlc_media_t*) = nullptr;
  Player* (*player_new)(libvlc_media_t*) = nullptr;
  void (*player_release)(Player*) = nullptr;
  int (*play)(Player*) = nullptr;
  void (*stop)(Player*) = nullptr;
  PlayerState (*state)(Player*) = nullptr;
  void (*video_callbacks)(Player*, Lock, Unlock, Display, void*) = nullptr;
  void (*format_callbacks)(Player*, Format, Cleanup) = nullptr;
  int (*video_size)(Player*, unsigned, unsigned*, unsigned*) = nullptr;
  int (*audio_track_count)(Player*) = nullptr;
  int (*audio_track)(Player*) = nullptr;

  ~VlcApi() {
    if (instance) release(instance);
    if (dll) FreeLibrary(dll);
  }

  template <typename Function>
  bool symbol(Function& function, const char* name) {
    function = reinterpret_cast<Function>(GetProcAddress(dll, name));
    return function != nullptr;
  }

  bool load() {
    namespace fs = std::filesystem;
    fs::path dir = environment(L"SLRR_VLC_DIR");
    if (dir.empty()) {
      dir = environment(L"ProgramFiles");
      if (dir.empty()) return false;
      dir /= L"VideoLAN/VLC";
    }
    if (!dir.is_absolute()) return false;
    dll = LoadLibraryExW((dir / L"libvlc.dll").c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) return false;
    if (!symbol(init, "libvlc_new") ||
        !symbol(release, "libvlc_release") ||
        !symbol(media_new_path, "libvlc_media_new_path") ||
        !symbol(media_release, "libvlc_media_release") ||
        !symbol(player_new, "libvlc_media_player_new_from_media") ||
        !symbol(player_release, "libvlc_media_player_release") ||
        !symbol(play, "libvlc_media_player_play") ||
        !symbol(stop, "libvlc_media_player_stop") ||
        !symbol(state, "libvlc_media_player_get_state") ||
        !symbol(video_callbacks, "libvlc_video_set_callbacks") ||
        !symbol(format_callbacks, "libvlc_video_set_format_callbacks") ||
        !symbol(video_size, "libvlc_video_get_size") ||
        !symbol(audio_track_count, "libvlc_audio_get_track_count") ||
        !symbol(audio_track, "libvlc_audio_get_track"))
      return false;

    const char* options[] = {"--ignore-config", "--no-video-title-show",
                             "--avcodec-hw=none"};
    instance = init(3, options);
    return instance != nullptr;
  }
};

struct VlcPlayback;

struct Picture {
  VlcPlayback& owner;
  unsigned width;
  unsigned height;
  unsigned pitch;
  uint8_t* pixels;

  ~Picture() { _aligned_free(pixels); }
};

struct VlcPlayback {
  VlcApi api;
  bool load_attempted = false;
  Player* player = nullptr;
  bool loop = false;
  ULONGLONG started = 0;
  bool has_frame = false;
  std::mutex frame_mutex;
  std::vector<uint8_t> frame;
  int width = 0;
  int height = 0;
  bool pending = false;

  ~VlcPlayback() { close(); }

  void close() {
    if (player) {
      // Stop joins the video output before clearing shared frame state.
      api.stop(player);
      api.player_release(player);
      player = nullptr;
    }
    frame.clear();
    pending = false;
  }

  static unsigned format(void** opaque, char* chroma, unsigned* width,
                         unsigned* height, unsigned* pitches, unsigned* lines) {
    auto& self = *static_cast<VlcPlayback*>(*opaque);
    unsigned visible_width = 0;
    unsigned visible_height = 0;
    // The callback can report padded decoder dimensions. Use the visible
    // track size when it agrees with the decoded orientation and dimensions.
    if (self.api.video_size(self.player, 0, &visible_width, &visible_height) == 0 &&
        visible_width <= *width && *width - visible_width < 32 &&
        visible_height <= *height && *height - visible_height < 32) {
      *width = visible_width;
      *height = visible_height;
    }
    constexpr unsigned max_dimension = 4096;
    if (*width == 0 || *height == 0 || *width > max_dimension ||
        *height > max_dimension)
      return 0;
    const unsigned pitch = (*width * 4 + 31) & ~31u;
    const unsigned rows = (*height + 31) & ~31u;
    auto* pixels = static_cast<uint8_t*>(_aligned_malloc(
        static_cast<size_t>(pitch) * rows, 32));
    if (!pixels) return 0;
    auto* picture = new (std::nothrow) Picture{
        self, *width, *height, pitch, pixels};
    if (!picture) {
      _aligned_free(pixels);
      return 0;
    }
    *opaque = picture;
    std::memcpy(chroma, "RV32", 4);
    pitches[0] = pitch;
    lines[0] = rows;
    return 1;
  }

  static void cleanup(void* opaque) { delete static_cast<Picture*>(opaque); }

  static void* lock(void* opaque, void** planes) {
    auto& picture = *static_cast<Picture*>(opaque);
    planes[0] = picture.pixels;
    return nullptr;
  }

  static void display(void* opaque, void*) {
    const auto& picture = *static_cast<Picture*>(opaque);
    auto& self = picture.owner;
    std::lock_guard guard(self.frame_mutex);
    const size_t row_size = static_cast<size_t>(picture.width) * 4;
    try {
      self.frame.resize(row_size * picture.height);
    } catch (const std::bad_alloc&) {
      return;
    }
    for (unsigned y = 0; y < picture.height; ++y)
      std::memcpy(self.frame.data() + y * row_size,
                  picture.pixels + y * picture.pitch, row_size);
    self.width = static_cast<int>(picture.width);
    self.height = static_cast<int>(picture.height);
    self.pending = true;
  }
};

VlcPlayback playback;

}  // namespace

bool vlc_fmv_open(const std::wstring& path, bool loop) {
  auto& p = playback;
  if (!p.load_attempted) {
    p.load_attempted = true;
    if (!p.api.load())
      std::printf("[fmv] LibVLC 3 x64 unavailable, trying Windows codecs\n");
  }
  if (!p.api.instance) return false;

  const auto utf8 = std::filesystem::path(path).u8string();
  auto* media = p.api.media_new_path(
      p.api.instance, reinterpret_cast<const char*>(utf8.c_str()));
  if (!media) return false;
  p.player = p.api.player_new(media);
  p.api.media_release(media);
  if (!p.player) return false;
  p.api.video_callbacks(p.player, VlcPlayback::lock, nullptr,
                        VlcPlayback::display, &p);
  p.api.format_callbacks(p.player, VlcPlayback::format, VlcPlayback::cleanup);
  p.loop = loop;
  p.has_frame = false;
  p.started = GetTickCount64();
  if (p.api.play(p.player) == 0) return true;
  p.close();
  return false;
}

void vlc_fmv_close() { playback.close(); }

VlcFrame vlc_fmv_pump(std::vector<uint8_t>& bgra, int& width, int& height) {
  auto& p = playback;
  bool copied = false;
  {
    std::lock_guard guard(p.frame_mutex);
    if (p.pending) {
      bgra.swap(p.frame);
      width = p.width;
      height = p.height;
      p.pending = false;
      copied = p.has_frame = true;
    }
  }
  const auto state = p.api.state(p.player);
  if (state == PlayerState::Ended && p.loop && p.has_frame) {
    p.api.stop(p.player);
    p.started = GetTickCount64();
    p.has_frame = false;
    if (p.api.play(p.player) != 0) return VlcFrame::Stopped;
  } else if (state == PlayerState::Error || state == PlayerState::Ended ||
             state == PlayerState::Stopped ||
             (!p.has_frame && GetTickCount64() - p.started > 5000)) {
    return VlcFrame::Stopped;
  }
  return copied ? VlcFrame::Ready : VlcFrame::Waiting;
}

int vlc_fmv_audio_tracks() {
  const auto& p = playback;
  // LibVLC includes the "Disable" entry in this count.
  return p.player ? std::max(0, p.api.audio_track_count(p.player) - 1) : 0;
}

bool vlc_fmv_audio_running() {
  const auto& p = playback;
  return p.player && p.api.state(p.player) == PlayerState::Playing &&
         p.api.audio_track(p.player) >= 0;
}

}  // namespace slrr
#endif
