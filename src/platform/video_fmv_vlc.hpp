#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

enum class VlcFrame { Waiting, Ready, Stopped };

bool vlc_fmv_open(const std::wstring& path, bool loop);
void vlc_fmv_close();
VlcFrame vlc_fmv_pump(std::vector<uint8_t>& bgra, int& width, int& height);
int vlc_fmv_audio_tracks();
bool vlc_fmv_audio_running();

}  // namespace slrr
