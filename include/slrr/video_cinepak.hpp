#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <span>
#include <vector>

namespace slrr {

// Built-in playback for Cinepak (cvid) AVI files with optional PCM audio, the
// format of every SLRR movie. 64-bit Windows has no Cinepak codec, so this
// path needs neither LibVLC nor a system decoder.

// Decodes Cinepak frames. Codebooks persist between frames, as the format
// requires, so frames must be decoded in order after reset().
class CinepakDecoder {
 public:
  // Writes a width*height top-left BGRA image. Blocks an inter frame skips keep
  // the previous pixels, so pass the same buffer for every frame. Returns
  // false for malformed data; blocks decoded before the error stay written.
  bool decode(std::span<const uint8_t> frame, int width, int height,
              std::vector<uint8_t>& bgra);
  void reset();

  static constexpr int kMaxStrips = 32;

 private:
  struct Strip {
    // Each entry holds four RGB pixels.
    uint8_t v1[256][12];
    uint8_t v4[256][12];
  };
  bool decode_strip(Strip& strip, int x1, int y1, int x2, int y2,
                    const uint8_t* data, size_t size, int width, int height,
                    uint8_t* bgra);
  std::array<Strip, kMaxStrips> strips_{};
};

struct AviChunkRef {
  uint64_t offset = 0;
  uint32_t size = 0;
};

struct AviCinepakInfo {
  int width = 0;
  int height = 0;
  double frame_seconds = 0;
  std::vector<AviChunkRef> frames;
  // Zero channels means no usable PCM audio stream.
  int audio_channels = 0;
  int audio_rate = 0;
  int audio_bits = 0;
  std::vector<AviChunkRef> audio;
};

// Indexes the first video stream and the first audio stream. Returns false
// unless the file is an AVI whose first video stream is Cinepak.
bool avi_read_cinepak(std::istream& in, AviCinepakInfo& out);

enum class CinepakFrame { Waiting, Ready, Stopped };

bool cinepak_fmv_open(const std::filesystem::path& path, bool loop);
void cinepak_fmv_close();
CinepakFrame cinepak_fmv_pump(std::vector<uint8_t>& bgra, int& width,
                              int& height);
int cinepak_fmv_audio_tracks();
bool cinepak_fmv_audio_running();

}  // namespace slrr
