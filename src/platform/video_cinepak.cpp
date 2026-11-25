#include "slrr/video_cinepak.hpp"
#include "slrr/host_audio.hpp"
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>


namespace slrr {
namespace {

uint32_t be16(const uint8_t* p) { return (uint32_t{p[0]} << 8) | p[1]; }
uint32_t be24(const uint8_t* p) {
  return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
}
uint32_t be32(const uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }
uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
uint8_t clip(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

// Codebook chunk: bit 2 of the id means 4 luma bytes per entry instead of
// 4 luma + 2 chroma; bit 0 means a 32-bit flag word selects updated entries.
void load_codebook(uint8_t (*book)[12], int id, const uint8_t* data, size_t size) {
  const uint8_t* end = data + size;
  const ptrdiff_t entry_size = (id & 0x04) ? 4 : 6;
  const bool partial = (id & 0x01) != 0;
  uint32_t flag = 0, mask = 0;
  for (int i = 0; i < 256; ++i) {
    if (partial && !(mask >>= 1)) {
      if (end - data < 4) return;
      flag = be32(data);
      data += 4;
      mask = 0x80000000u;
    }
    if (partial && !(flag & mask)) continue;
    if (end - data < entry_size) return;
    uint8_t* entry = book[i];
    if (entry_size == 4) {
      for (int k = 0; k < 4; ++k)
        entry[k * 3] = entry[k * 3 + 1] = entry[k * 3 + 2] = data[k];
    } else {
      const int u = static_cast<int8_t>(data[4]);
      const int v = static_cast<int8_t>(data[5]);
      for (int k = 0; k < 4; ++k) {
        const int y = data[k];
        entry[k * 3] = clip(y + v * 2);
        entry[k * 3 + 1] = clip(y - u / 2 - v);
        entry[k * 3 + 2] = clip(y + u * 2);
      }
    }
    data += entry_size;
  }
}

struct Target {
  uint8_t* bgra;
  int width;
  int height;
  void put(int x, int y, const uint8_t* rgb) const {
    if (x >= width || y >= height) return;
    uint8_t* pixel = bgra + (static_cast<size_t>(y) * width + x) * 4;
    pixel[0] = rgb[2];
    pixel[1] = rgb[1];
    pixel[2] = rgb[0];
    pixel[3] = 255;
  }
};

// Vector chunk 0x30: every 4x4 block is coded, a flag bit picks V1 or V4.
// 0x31: a flag bit first says whether the block changes at all. 0x32: V1 only.
bool decode_vectors(const uint8_t (*v1)[12], const uint8_t (*v4)[12], int id,
                    int x1, int y1, int x2, int y2, const uint8_t* data,
                    size_t size, const Target& target) {
  const uint8_t* end = data + size;
  uint32_t flag = 0, mask = 0;
  auto next_flag = [&]() {
    if (end - data < 4) return false;
    flag = be32(data);
    data += 4;
    mask = 0x80000000u;
    return true;
  };
  for (int y = y1; y < y2; y += 4) {
    for (int x = x1; x < x2; x += 4) {
      if ((id & 0x01) && !(mask >>= 1) && !next_flag()) return false;
      if ((id & 0x01) && !(flag & mask)) continue;
      if (!(id & 0x02) && !(mask >>= 1) && !next_flag()) return false;
      if ((id & 0x02) || !(flag & mask)) {
        // V1: one entry, each of its pixels covers a 2x2 quarter.
        if (end - data < 1) return false;
        const uint8_t* entry = v1[*data++];
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            target.put(x + c, y + r, entry + ((r / 2) * 2 + c / 2) * 3);
      } else {
        // V4: one entry per 2x2 quarter.
        if (end - data < 4) return false;
        const uint8_t* entries[4] = {v4[data[0]], v4[data[1]], v4[data[2]],
                                     v4[data[3]]};
        data += 4;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            target.put(x + c, y + r,
                       entries[(r / 2) * 2 + c / 2] + ((r % 2) * 2 + c % 2) * 3);
      }
    }
  }
  return true;
}

}  // namespace

void CinepakDecoder::reset() { std::memset(strips_.data(), 0, sizeof(strips_)); }

bool CinepakDecoder::decode_strip(Strip& strip, int x1, int y1, int x2, int y2,
                                  const uint8_t* data, size_t size, int width,
                                  int height, uint8_t* bgra) {
  // Blocks are 4x4; a strip may cover the padding of a partial last block.
  if (x2 > ((width + 3) & ~3) || y2 > ((height + 3) & ~3) || x1 >= x2 || y1 >= y2)
    return false;
  const uint8_t* end = data + size;
  while (end - data >= 4) {
    const int id = data[0];
    const uint32_t chunk = be24(data + 1);
    if (chunk < 4) return false;
    data += 4;
    const size_t length = std::min<size_t>(chunk - 4, static_cast<size_t>(end - data));
    switch (id) {
      case 0x20: case 0x21: case 0x24: case 0x25:
        load_codebook(strip.v4, id, data, length);
        break;
      case 0x22: case 0x23: case 0x26: case 0x27:
        load_codebook(strip.v1, id, data, length);
        break;
      case 0x30: case 0x31: case 0x32:
        return decode_vectors(strip.v1, strip.v4, id, x1, y1, x2, y2, data,
                              length, Target{bgra, width, height});
      default:
        break;
    }
    data += length;
  }
  return false;
}

bool CinepakDecoder::decode(std::span<const uint8_t> frame, int width, int height,
                            std::vector<uint8_t>& bgra) {
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  if (bgra.size() != bytes) {
    bgra.assign(bytes, 0);
    for (size_t i = 3; i < bytes; i += 4) bgra[i] = 255;
  }
  if (frame.size() < 10) return false;
  const uint8_t* data = frame.data();
  const uint8_t* end = data + frame.size();
  // Frame header: flags, 24-bit size, width, height, strip count.
  const int flags = data[0];
  const int count = std::min<int>(static_cast<int>(be16(data + 8)), kMaxStrips);
  data += 10;
  int y0 = 0;
  for (int i = 0; i < count; ++i) {
    // Strip header: id, 24-bit size, y1, x1, y2, x2. A zero y1 makes y2 a
    // height below the previous strip.
    if (end - data < 12) return false;
    const uint32_t size = be24(data + 1);
    if (size < 12) return false;
    int y1 = static_cast<int>(be16(data + 4));
    const int x1 = static_cast<int>(be16(data + 6));
    int y2 = static_cast<int>(be16(data + 8));
    const int x2 = static_cast<int>(be16(data + 10));
    if (y1 == 0) {
      y1 = y0;
      y2 += y0;
    }
    data += 12;
    const size_t length = std::min<size_t>(size - 12, static_cast<size_t>(end - data));
    Strip& strip = strips_[i];
    // Flag bit 0 clear: each strip starts from the previous strip's codebooks.
    if (i > 0 && !(flags & 0x01)) {
      std::memcpy(strip.v1, strips_[i - 1].v1, sizeof(strip.v1));
      std::memcpy(strip.v4, strips_[i - 1].v4, sizeof(strip.v4));
    }
    if (!decode_strip(strip, x1, y1, x2, y2, data, length, width, height, bgra.data()))
      return false;
    data += length;
    y0 = y2;
  }
  return true;
}

namespace {

bool is_fourcc(const uint8_t* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

bool is_cinepak(const uint8_t* p) {
  for (int i = 0; i < 4; ++i)
    if (std::tolower(p[i]) != "cvid"[i]) return false;
  return true;
}

struct AviParser {
  std::istream& in;
  uint64_t file_size;
  AviCinepakInfo& out;
  int streams = 0;
  int current = -1;
  int video = -1;
  int audio = -1;
  bool video_cinepak = false;
  double avih_seconds = 0;
  uint64_t audio_bytes = 0;

  bool read(uint64_t offset, void* dst, size_t size) {
    if (offset + size > file_size) return false;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == size;
  }

  void stream_header(uint64_t offset, uint64_t size) {
    uint8_t h[48];
    if (current < 0 || size < sizeof(h) || !read(offset, h, sizeof(h))) return;
    if (is_fourcc(h, "vids") && video < 0) {
      video = current;
      const uint32_t scale = le32(h + 20), rate = le32(h + 24);
      if (scale && rate) out.frame_seconds = static_cast<double>(scale) / rate;
      video_cinepak = is_cinepak(h + 4);
    } else if (is_fourcc(h, "auds") && audio < 0) {
      audio = current;
    }
  }

  void stream_format(uint64_t offset, uint64_t size) {
    if (current >= 0 && current == video) {
      uint8_t b[20];
      if (size < sizeof(b) || !read(offset, b, sizeof(b))) return;
      const auto width = static_cast<int32_t>(le32(b + 4));
      const auto height = static_cast<int32_t>(le32(b + 8));
      out.width = width;
      out.height = height == INT32_MIN ? 0 : (height < 0 ? -height : height);
      // The bitmap compression is authoritative; handlers are often blank.
      video_cinepak = is_cinepak(b + 16);
    } else if (current >= 0 && current == audio) {
      uint8_t w[16];
      if (size < sizeof(w) || !read(offset, w, sizeof(w))) return;
      const uint32_t tag = le16(w), channels = le16(w + 2), rate = le32(w + 4);
      const uint32_t bits = le16(w + 14);
      if (tag == 1 && channels >= 1 && channels <= 2 && (bits == 8 || bits == 16) &&
          rate >= 1000 && rate <= 192000) {
        out.audio_channels = static_cast<int>(channels);
        out.audio_rate = static_cast<int>(rate);
        out.audio_bits = static_cast<int>(bits);
      } else {
        std::printf("[fmv] Cinepak AVI audio format %u is not PCM; playing silently\n",
                    tag);
      }
    }
  }

  void movie_chunk(const uint8_t* id, uint64_t offset, uint64_t size) {
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return;
    const int stream = (id[0] - '0') * 10 + (id[1] - '0');
    constexpr uint64_t max_frame = 16u << 20;
    if (stream == video && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) {
      if (size <= max_frame)
        out.frames.push_back({offset, static_cast<uint32_t>(size)});
    } else if (stream == audio && id[2] == 'w' && id[3] == 'b' && out.audio_channels &&
               size && audio_bytes + size <= (512u << 20)) {
      out.audio.push_back({offset, static_cast<uint32_t>(size)});
      audio_bytes += size;
    }
  }

  void list(uint64_t begin, uint64_t end, bool movie, int depth) {
    uint64_t pos = begin;
    while (pos + 8 <= end) {
      uint8_t h[8];
      if (!read(pos, h, sizeof(h))) return;
      const uint32_t size = le32(h + 4);
      const uint64_t payload = pos + 8;
      const uint64_t stop = std::min<uint64_t>(payload + size, end);
      if (is_fourcc(h, "LIST")) {
        uint8_t type[4];
        if (depth < 4 && stop - payload >= 4 && read(payload, type, 4)) {
          if (is_fourcc(type, "hdrl")) {
            list(payload + 4, stop, false, depth + 1);
          } else if (is_fourcc(type, "strl")) {
            current = streams++;
            list(payload + 4, stop, false, depth + 1);
            current = -1;
          } else if (is_fourcc(type, "movi") || (movie && is_fourcc(type, "rec "))) {
            list(payload + 4, stop, true, depth + 1);
          }
        }
      } else if (movie) {
        movie_chunk(h, payload, stop - payload);
      } else if (is_fourcc(h, "avih")) {
        uint8_t micro[4];
        if (stop - payload >= 4 && read(payload, micro, 4))
          avih_seconds = le32(micro) / 1e6;
      } else if (is_fourcc(h, "strh")) {
        stream_header(payload, stop - payload);
      } else if (is_fourcc(h, "strf")) {
        stream_format(payload, stop - payload);
      }
      pos = payload + size + (size & 1);
    }
  }
};

}  // namespace

bool avi_read_cinepak(std::istream& in, AviCinepakInfo& out) {
  out = {};
  in.clear();
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 12) return false;
  AviParser parser{in, static_cast<uint64_t>(size), out};
  // The first RIFF is "AVI "; OpenDML files continue in "AVIX" RIFFs.
  uint64_t pos = 0;
  while (pos + 12 <= parser.file_size) {
    uint8_t h[12];
    if (!parser.read(pos, h, sizeof(h)) || !is_fourcc(h, "RIFF") ||
        !is_fourcc(h + 8, pos == 0 ? "AVI " : "AVIX"))
      break;
    const uint32_t riff_size = le32(h + 4);
    parser.list(pos + 12, std::min<uint64_t>(pos + 8 + riff_size, parser.file_size),
                false, 0);
    pos += 8 + uint64_t{riff_size} + (riff_size & 1);
  }
  if (parser.video < 0 || !parser.video_cinepak || out.width <= 0 ||
      out.height <= 0 || out.width > 4096 || out.height > 4096 || out.frames.empty())
    return false;
  if (!(out.frame_seconds >= 0.001 && out.frame_seconds <= 1.0))
    out.frame_seconds = parser.avih_seconds >= 0.001 && parser.avih_seconds <= 1.0
                            ? parser.avih_seconds
                            : 1.0 / 15.0;
  if (out.audio.empty()) out.audio_channels = out.audio_rate = out.audio_bits = 0;
  return true;
}

#ifdef _WIN32
namespace {

using Clock = std::chrono::steady_clock;

struct Movie {
  std::shared_ptr<HostAudio> audio;
  MIX_Track* voice = nullptr;  // Holds the only reference to the PCM.
  double audio_length = 0;
  int audio_frame_rate = 0;  // Rate of the loaded PCM, after any upsampling.
  double audio_position = 0;
  double audio_wall = 0;
  bool audio_done = true;

  std::ifstream file;
  AviCinepakInfo info;
  CinepakDecoder decoder;
  std::vector<uint8_t> chunk;
  std::vector<uint8_t> frame;
  std::vector<uint8_t> pcm;
  int current = -1;
  bool open = false;
  bool loop = false;
  bool fresh = false;
  bool reported_error = false;
  Clock::time_point started;
  double wall_offset = 0;

  ~Movie() { close(); }

  void close() {
    if (voice) {
      MIX_DestroyTrack(voice);
      voice = nullptr;
    }
    audio_done = true;
    audio_length = 0;
    file.close();
    file.clear();
    info = {};
    chunk.clear();
    frame.clear();
    pcm.clear();
    current = -1;
    open = false;
  }

  // Decodes the next frame. Returns whether the image changed.
  bool decode_next() {
    const AviChunkRef ref = info.frames[static_cast<size_t>(++current)];
    if (!ref.size) return false;  // An empty chunk repeats the previous frame.
    chunk.resize(ref.size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(ref.offset));
    file.read(reinterpret_cast<char*>(chunk.data()), ref.size);
    if (static_cast<uint32_t>(file.gcount()) != ref.size) return false;
    if (!decoder.decode(chunk, info.width, info.height, frame) && !reported_error) {
      reported_error = true;
      std::printf("[fmv] Cinepak frame %d is malformed\n", current);
    }
    return true;
  }

  bool submit_audio() {
    audio_position = audio_wall = 0;
    // Playing a track again restarts it from the first sample.
    audio_done = !voice || !host_audio_check(MIX_PlayTrack(voice, 0), "play Cinepak PCM");
    return !audio_done;
  }

  void start_audio() {
    if (!info.audio_channels) return;
    for (const AviChunkRef& ref : info.audio) {
      const size_t at = pcm.size();
      pcm.resize(at + ref.size);
      file.clear();
      file.seekg(static_cast<std::streamoff>(ref.offset));
      file.read(reinterpret_cast<char*>(pcm.data() + at), ref.size);
      pcm.resize(at + static_cast<size_t>(file.gcount()));
    }
    const int block = info.audio_channels * info.audio_bits / 8;
    const auto bytes = pcm.size() / block * block;
    if (!bytes) { std::vector<uint8_t>().swap(pcm); return; }
    if (!audio) audio = host_audio_acquire();
    if (!audio) { std::vector<uint8_t>().swap(pcm); return; }
    // AVI PCM keeps WAV's conventions: unsigned 8-bit, signed little-endian 16-bit.
    std::vector<float> samples(bytes / (info.audio_bits / 8));
    for (size_t i = 0; i < samples.size(); ++i)
      samples[i] = info.audio_bits == 8
          ? (static_cast<float>(pcm[i]) - 128.f) / 128.f
          : static_cast<float>(static_cast<int16_t>(pcm[2 * i] | (pcm[2 * i + 1] << 8))) / 32768.f;
    // Free the staging buffer before playback; the load copies the samples.
    std::vector<uint8_t>().swap(pcm);
    MIX_Audio* sound = host_audio_load_pcm(audio->fmv, samples, info.audio_channels,
                                           info.audio_rate, false, &audio_frame_rate);
    if (!sound) return;
    voice = MIX_CreateTrack(audio->fmv);
    const bool attached = host_audio_check(voice != nullptr, "Cinepak track") &&
                          host_audio_check(MIX_SetTrackAudio(voice, sound), "Cinepak audio");
    MIX_DestroyAudio(sound);  // The track keeps its own reference.
    if (!attached) {
      if (voice) MIX_DestroyTrack(voice);
      voice = nullptr;
      return;
    }
    audio_length = static_cast<double>(bytes) / (static_cast<double>(info.audio_rate) * block);
    submit_audio();
  }

  void restart() {
    decoder.reset();
    current = -1;
    decode_next();
    started = Clock::now();
    wall_offset = 0;
    if (voice) submit_audio();
  }

  bool start(const std::filesystem::path& path, bool repeat) {
    close();
    file.open(path, std::ios::binary);
    if (!file || !avi_read_cinepak(file, info)) {
      close();
      return false;
    }
    decoder.reset();
    reported_error = false;
    // Show a real picture at once; another backend gets the file if even the
    // first frame cannot be decoded.
    const AviChunkRef first = info.frames.front();
    chunk.resize(first.size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(first.offset));
    file.read(reinterpret_cast<char*>(chunk.data()), first.size);
    if (!first.size || static_cast<uint32_t>(file.gcount()) != first.size ||
        !decoder.decode(chunk, info.width, info.height, frame)) {
      close();
      return false;
    }
    current = 0;
    loop = repeat;
    fresh = true;
    open = true;
    start_audio();
    started = Clock::now();
    wall_offset = 0;
    std::printf("[fmv] Cinepak %dx%d frames=%zu fps=%.2f audio=%d\n", info.width,
                info.height, info.frames.size(), 1.0 / info.frame_seconds,
                voice ? 1 : 0);
    return true;
  }

  CinepakFrame pump(std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!open) return CinepakFrame::Stopped;
    const double wall = std::chrono::duration<double>(Clock::now() - started).count();
    double t = wall + wall_offset;
    if (voice && !audio_done) {
      // Audio drives the clock while it plays. A stalled device must not
      // freeze the picture, so fall back to wall time when it lags.
      const Sint64 samples = MIX_GetTrackPlaybackPosition(voice);
      const bool running = MIX_TrackPlaying(voice) && samples >= 0;
      if (running) {
        audio_position = std::min(audio_length, static_cast<double>(samples) / audio_frame_rate);
        audio_wall = wall;
      }
      // An exhausted or stopped track stops reporting. On EOF or an early device
      // failure, continue from the last known position instead of jumping the
      // picture to the end of the sample.
      t = running ? audio_position : std::min(audio_length, audio_position + wall - audio_wall);
      if (!running || wall - t > 2.0) {
        audio_done = true;
        wall_offset = t - wall;
      }
    }
    const int count = static_cast<int>(info.frames.size());
    const int target = static_cast<int>(
        std::min<double>(count - 1, std::max(0.0, t / info.frame_seconds)));
    bool changed = fresh;
    fresh = false;
    for (int budget = 30; current < target && budget > 0; --budget)
      changed = decode_next() || changed;
    if (current >= count - 1 && audio_done && t >= count * info.frame_seconds) {
      if (!loop) return CinepakFrame::Stopped;
      restart();
      changed = true;
    }
    if (!changed) return CinepakFrame::Waiting;
    bgra = frame;
    width = info.width;
    height = info.height;
    return CinepakFrame::Ready;
  }
};

Movie movie;

}  // namespace

bool cinepak_fmv_open(const std::filesystem::path& path, bool loop) {
  return movie.start(path, loop);
}
void cinepak_fmv_close() { movie.close(); }
CinepakFrame cinepak_fmv_pump(std::vector<uint8_t>& bgra, int& width, int& height) {
  return movie.pump(bgra, width, height);
}
int cinepak_fmv_audio_tracks() { return movie.voice ? 1 : 0; }
bool cinepak_fmv_audio_running() {
  if (!movie.voice || movie.audio_done) return false;
  return MIX_TrackPlaying(movie.voice);
}
MIX_Track* cinepak_fmv_audio_track() { return movie.voice; }
#else
MIX_Track* cinepak_fmv_audio_track() { return nullptr; }
bool cinepak_fmv_open(const std::filesystem::path&, bool) { return false; }
void cinepak_fmv_close() {}
CinepakFrame cinepak_fmv_pump(std::vector<uint8_t>&, int&, int&) {
  return CinepakFrame::Stopped;
}
int cinepak_fmv_audio_tracks() { return 0; }
bool cinepak_fmv_audio_running() { return false; }
#endif

}  // namespace slrr
