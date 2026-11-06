#include "slrr/host_music.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

namespace slrr {
namespace {
struct Playlist { std::vector<std::filesystem::path> tracks; int index = 0, mode = 3; };
std::array<Playlist, 4> sets;
int active = -1;
bool opened = false;
float volume = 1.f;
std::chrono::steady_clock::time_point last_poll;
#ifdef _WIN32
bool command(const std::wstring& text) {
  const auto error = mciSendStringW(text.c_str(), nullptr, 0, nullptr);
  if (!error) return true;
  char message[256]{};
  mciGetErrorStringA(error, message, sizeof(message));
  std::fprintf(stderr, "[music] MCI: %s\n", message);
  return false;
}
#endif
void close_track() {
#ifdef _WIN32
  if (opened) command(L"close openslrr_music");
#endif
  opened = false;
}
void play_track() {
  close_track();
  if (active < 0 || sets[active].tracks.empty()) return;
  auto& p = sets[active];
  const int count = static_cast<int>(p.tracks.size());
  p.index = (p.index % count + count) % count;
#ifdef _WIN32
  // A separate MCI alias streams MP3 without affecting FMV or XAudio2 SFX.
  const auto path = std::filesystem::absolute(p.tracks[p.index]).wstring();
  if (path.find(L'"') != std::wstring::npos) return;
  opened = command(L"open \"" + path + L"\" type mpegvideo alias openslrr_music");
  if (!opened) return;
  command(L"setaudio openslrr_music volume to " + std::to_wstring(static_cast<int>(volume*1000.f)));
  if (!command(L"play openslrr_music")) close_track();
#endif
  last_poll = std::chrono::steady_clock::now();
}
}
void host_music_set_root(const std::string& root) {
  host_music_stop();
  const char* folders[4] = {"Garage_Shop", "Roam_ride", "Race_chase", "Main_menu"};
  for (size_t i = 0; i < sets.size(); ++i) {
    sets[i] = Playlist{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(root)/"music"/folders[i], ec)) {
      if (!entry.is_regular_file(ec)) continue;
      std::string ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ext == ".mp3" || ext == ".wav" || ext == ".wma") sets[i].tracks.push_back(entry.path());
    }
    std::sort(sets[i].tracks.begin(), sets[i].tracks.end());
  }
}
void host_music_change_set(int type) {
  const int next = type >= 0 && type < 4 ? type : -1;
  if (next == active) return;
  active = next;
  play_track();
}
void host_music_next(int direction) {
  if (active < 0) return;
  sets[active].index += direction < 0 ? -1 : 1;
  play_track();
}
int host_music_mode() { return active < 0 ? -1 : sets[active].mode; }
int host_music_set_mode(int mode) {
  // Reference native: no set returns the argument; invalid values do not store.
  if (active < 0) return mode;
  if (mode >= 0 && mode <= 3) sets[active].mode = mode;
  return sets[active].mode;
}
void host_music_set_volume(float value) {
  volume = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 1.f;
#ifdef _WIN32
  if (opened) command(L"setaudio openslrr_music volume to " + std::to_wstring(static_cast<int>(volume*1000.f)));
#endif
}
void host_music_update() {
#ifdef _WIN32
  const auto now = std::chrono::steady_clock::now();
  if (!opened || now-last_poll < std::chrono::milliseconds(250)) return;
  last_poll = now;
  wchar_t state[32]{};
  if (!mciSendStringW(L"status openslrr_music mode", state, 32, nullptr) &&
      std::wstring(state) == L"stopped") host_music_next();
#endif
}
void host_music_stop() { close_track(); active = -1; }
}
