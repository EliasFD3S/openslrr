#include "slrr/host_music.hpp"
#include "slrr/host_audio.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace slrr {
namespace {
struct Playlist { std::vector<std::filesystem::path> tracks; int index=0, mode=3; };
std::array<Playlist,4> sets;
int active=-1;
bool opened=false;
float volume=1.f;
uint64_t track_attempts=0,track_failures=0;
struct MusicStream {
  std::shared_ptr<HostAudio> engine;
  FMOD::Sound* sound=nullptr;
  FMOD::Channel* voice=nullptr;
  void close() {
    if(voice){voice->stop();voice=nullptr;}
    if(sound){sound->release();sound=nullptr;}
  }
  ~MusicStream() { close(); }
  bool init() {
    if(!engine)engine=host_audio_acquire();
    return engine && host_audio_check(engine->music->setVolume(volume), "music volume");
  }
} stream;
void close_track() {
  stream.close();
  opened=false;
}
bool try_track(Playlist& p) {
  close_track();
  ++track_attempts;
  const int count=static_cast<int>(p.tracks.size());
  const auto path=std::filesystem::absolute(p.tracks[p.index]).u8string();
  opened=stream.init() &&
      host_audio_check(stream.engine->system->createStream(
          reinterpret_cast<const char*>(path.c_str()), FMOD_2D | FMOD_LOOP_OFF,
          nullptr, &stream.sound), "open music") &&
      host_audio_check(stream.engine->system->playSound(stream.sound, stream.engine->music,
          true, &stream.voice), "play music") &&
      host_audio_check(stream.voice->setPaused(false), "start music");
  if(!opened)close_track();
  std::fprintf(stderr,"[music] set=%d track=%d/%d playing=%d %s\n",active,p.index+1,count,opened?1:0,p.tracks[p.index].filename().string().c_str());
  if (!opened) ++track_failures;
  return opened;
}
void play_track(int direction=1) {
  close_track();
  if(active<0||sets[active].tracks.empty())return;
  auto& p=sets[active];const int count=static_cast<int>(p.tracks.size());
  p.index=(p.index%count+count)%count;
  // At most one pass. An all-broken set stays stopped instead of spinning.
  for(int attempt=0;attempt<count;++attempt) {
    if(try_track(p))return;
    p.index=(p.index+(direction<0?count-1:1))%count;
  }
}
}
void host_music_set_root(const std::string& root) {
  host_music_stop();track_attempts=track_failures=0;
  const char* folders[4]={"Garage_Shop","Roam_ride","Race_chase","Main_menu"};
  for(size_t i=0;i<sets.size();++i){
    sets[i]=Playlist{};std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(root)/"music"/folders[i],ec)){
      if(!entry.is_regular_file(ec))continue;
      auto ext=entry.path().extension().string();
      std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
      if(ext==".mp3"||ext==".wav"||ext==".wma")sets[i].tracks.push_back(entry.path());
    }
    std::sort(sets[i].tracks.begin(),sets[i].tracks.end());
    std::fprintf(stderr,"[music] scan %s: %zu tracks%s\n",folders[i],sets[i].tracks.size(),ec?" (directory error)":"");
  }
}
void host_music_change_set(int type) {
  const int next=type>=0&&type<4?type:-1;
  if(next==active&&(opened||next<0))return;
  active=next;play_track();
}
void host_music_next(int direction){if(active<0)return;sets[active].index+=direction<0?-1:1;play_track(direction);}
int host_music_mode(){return active<0?-1:sets[active].mode;}
int host_music_set_mode(int mode){if(active<0)return mode;if(mode>=0&&mode<=3)sets[active].mode=mode;return sets[active].mode;}
void host_music_set_volume(float value){
  volume=std::isfinite(value)?std::clamp(value,0.f,1.f):1.f;
  if(stream.engine)stream.engine->music->setVolume(volume);
}
void host_music_update(){
  // This pump runs in modal dialogs too. Update the shared mixer even when
  // music is stopped, since effects/engine loops may still be active.
  host_audio_update();
  if(!opened)return;
  FMOD_OPENSTATE state{};
  const auto result=stream.sound->getOpenState(&state,nullptr,nullptr,nullptr);
  bool playing=false;
  if(result!=FMOD_OK || state==FMOD_OPENSTATE_ERROR ||
      stream.voice->isPlaying(&playing)!=FMOD_OK || !playing)host_music_next();
}
void host_music_stop(){close_track();active=-1;}
bool host_music_playing(){return opened;}
HostMusicStats host_music_stats(){return {track_attempts,track_failures,active<0?-1:sets[active].index};}
int host_music_track_count(int set){return set>=0&&set<4?static_cast<int>(sets[set].tracks.size()):0;}
}
