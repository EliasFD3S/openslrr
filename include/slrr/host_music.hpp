#pragma once
#include <string>
#include <cstdint>

namespace slrr {
void host_music_set_root(const std::string& root);
void host_music_change_set(int type);
void host_music_next(int direction = 1);
int host_music_mode();
int host_music_set_mode(int mode);
void host_music_set_volume(float volume);
void host_music_update();
void host_music_stop();
bool host_music_playing();
struct HostMusicStats { uint64_t attempts=0, failures=0; int track=-1; };
HostMusicStats host_music_stats();
int host_music_track_count(int set);
}
