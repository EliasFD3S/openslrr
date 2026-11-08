#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace slrr {

// GfxEngine.openVideo uses the host framebuffer with LibVLC, or the
// legacy Windows codec path. Non-exclusive clips can serve as UI backdrops.

void video_fmv_set_game_root(const std::string& root);
void video_fmv_set_hwnd(void* hwnd);

int32_t video_fmv_open(const char* path, int32_t non_exclusive, int32_t loop);
void video_fmv_close();
int32_t video_fmv_is_playing();
int32_t video_fmv_is_non_exclusive();
int32_t video_fmv_audio_stream_count();
bool video_fmv_audio_running();
// Boot intros: OSD must not draw or take clicks over the movie.
void video_fmv_set_block_osd(bool block);
bool video_fmv_block_osd();

// Pump EC_COMPLETE + grab latest sample (call once per frame before blit).
void video_fmv_pump();

int32_t video_fmv_width();
int32_t video_fmv_height();
// BGRA8 top-left, size width*height*4. Null if exclusive HWND or no frame yet.
const uint8_t* video_fmv_bgra();
bool video_fmv_frame_ready();
// A nonzero serial changes only when a new decoded frame is available.
uint64_t video_fmv_frame_serial();

// Letterbox rect in pixel space for current framebuffer (stock UpdateQuad).
void video_fmv_letterbox(int32_t screen_w, int32_t screen_h, float* out_left,
                         float* out_top, float* out_w, float* out_h);

// Engine_boot intros: Activision → Invictus (StreetLegal optional).
// pump: pump+flush each wait tick; max_frames_each 0 = until end/ESC.
int32_t video_fmv_play_boot_intros(int32_t max_frames_each,
                                   const std::function<void()>& pump);

}  // namespace slrr
