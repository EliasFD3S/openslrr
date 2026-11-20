#include "standalone_test.hpp"
#include "slrr/host_audio.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cmath>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace fs = std::filesystem;
struct Fixture {
 fs::path root=fs::temp_directory_path()/("slrr-audio-"+std::to_string(GetCurrentProcessId()));
 Fixture(){SLRR_REQUIRE(!fs::exists(root));fs::create_directories(root);}
 ~Fixture(){slrr::host_music_stop();slrr::host_sfx_set_game_root("");std::error_code ec;fs::remove_all(root,ec);}
};
#include <cstring>
#include <limits>
using namespace slrr;
void wave(const fs::path& path,int seconds=60){
 const uint32_t data=8000*2*seconds;std::vector<uint8_t>b(44+data);
 auto put=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));};
 std::memcpy(b.data(),"RIFF",4);put(4,36+data);std::memcpy(b.data()+8,"WAVEfmt ",8);put(16,16);b[20]=1;b[22]=1;put(24,8000);put(28,16000);b[32]=2;b[34]=16;std::memcpy(b.data()+36,"data",4);put(40,data);
 std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());
}
int main(int argc,char** argv){
 Fixture f;auto silence=f.root/"silence.wav";wave(silence);
 host_sfx_set_game_root(f.root.string());host_sfx_set_volume(0);
 std::vector<int32_t> loops;
 for(int i=0;i<24;++i){auto id=host_sfx_play_file(silence.string(),1,0,true);SLRR_REQUIRE(id>0);loops.push_back(id);}
 auto stats=host_sfx_stats();SLRR_REQUIRE(stats.active==24&&stats.wave_reads==1&&stats.cache_hits==23);
 auto rejected=host_sfx_play_file((f.root/"missing.wav").string(),1,0,true);
 stats=host_sfx_stats();SLRR_REQUIRE(!rejected&&stats.wave_reads==1&&stats.rejected==1);
 for(int i=0;i<9;++i){auto path=f.root/("shot"+std::to_string(i)+".wav");wave(path);SLRR_REQUIRE(host_sfx_play_file(path.string(),1,0,false)>0);}
 SLRR_REQUIRE(host_sfx_stats().active==32);
 for(auto id:loops)SLRR_REQUIRE(host_sfx_is_playing(id));
 host_sfx_update_loop(loops.front(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::quiet_NaN());
 SLRR_REQUIRE(host_sfx_is_playing(loops.front()));
 std::puts("PASS loop budget before I/O, shared PCM and reserved one-shot capacity");
 host_sfx_set_game_root(f.root.string());
 auto retained=host_sfx_play_file(silence.string(),1,0,true);SLRR_REQUIRE(retained>0);
 for(int i=0;i<65;++i){auto path=f.root/("cache"+std::to_string(i)+".wav");wave(path,1);SLRR_REQUIRE(host_sfx_play_file(path.string(),1,0,false)>0);}
 stats=host_sfx_stats();SLRR_REQUIRE(stats.cached_waves==64&&stats.cached_bytes<=16*1024*1024&&host_sfx_is_playing(retained));
 std::puts("PASS bounded LRU cache and playing buffer survives eviction");
 host_sfx_set_game_root("");SLRR_REQUIRE(host_sfx_stats().active==0&&host_sfx_stats().cached_waves==0);
 for(const char* name:{"Garage_Shop","Roam_ride","Race_chase","Main_menu"})fs::create_directories(f.root/"music"/name);
 std::ofstream(f.root/"music/Garage_Shop/00bad.wav")<<"not a wave";
 wave(f.root/"music/Garage_Shop/01good.wav");
 for(const char* name:{"00bad.wav","01bad.wav"})std::ofstream(f.root/"music/Roam_ride"/name)<<"not a wave";
 host_music_set_volume(0);host_music_set_root(f.root.string());host_music_change_set(0);
 auto music=host_music_stats();SLRR_REQUIRE(host_music_playing()&&music.attempts==2&&music.failures==1&&music.track==1);
 host_music_next(-1);music=host_music_stats();SLRR_REQUIRE(host_music_playing()&&music.attempts==4&&music.track==1);
 host_music_change_set(1);music=host_music_stats();SLRR_REQUIRE(!host_music_playing()&&music.attempts==6&&music.failures==4);
 for(int i=0;i<100;++i)host_music_update();SLRR_REQUIRE(host_music_stats().attempts==music.attempts);
 host_music_stop();
 std::puts("PASS unreadable music skipped in both directions; all-broken set stops after one pass");
 auto audio=host_audio_acquire();SLRR_REQUIRE(audio);
 auto id=host_sfx_play_file(silence.string(),1,0,true);SLRR_REQUIRE(id>0);
 FMOD::Channel* channel=nullptr;
 SLRR_REQUIRE(audio->effects->getChannel(0,&channel)==FMOD_OK);
 host_sfx_pause_loop(id,true);host_music_update();
 bool paused=false;SLRR_REQUIRE(channel->getPaused(&paused)==FMOD_OK&&paused&&host_sfx_is_playing(id));
 host_sfx_update_loop(id,1.5f,.4f);
 float value=0;SLRR_REQUIRE(channel->getPitch(&value)==FMOD_OK&&std::abs(value-1.5f)<.001f);
 SLRR_REQUIRE(channel->getVolume(&value)==FMOD_OK&&std::abs(value-.4f)<.001f);
 host_music_set_volume(.2f);
 SLRR_REQUIRE(audio->music->getVolume(&value)==FMOD_OK&&std::abs(value-.2f)<.001f);
 SLRR_REQUIRE(audio->effects->getVolume(&value)==FMOD_OK&&value==0);
 host_sfx_pause_loop(id,false);SLRR_REQUIRE(channel->getPaused(&paused)==FMOD_OK&&!paused);
 host_sfx_stop(id);SLRR_REQUIRE(!host_sfx_is_playing(id));
 host_music_set_volume(0);
 std::puts("PASS FMOD loop pause/resume, pitch/gain, stop and independent mixer groups");
 wave(f.root/"music/Main_menu/short.wav",1);
 host_music_set_root(f.root.string());host_music_change_set(3);
 const auto short_path=(f.root/"music/Main_menu/short.wav").string();
 const auto short_loop=host_sfx_play_file(short_path,1,0,true);SLRR_REQUIRE(short_loop>0);
 const auto short_shot=host_sfx_play_file(short_path,1,0,false);SLRR_REQUIRE(short_shot>0);
 SLRR_REQUIRE(audio->effects->getChannel(0,&channel)==FMOD_OK);
 FMOD::Sound* sample=nullptr;FMOD_MODE mode=0;
 SLRR_REQUIRE(channel->getCurrentSound(&sample)==FMOD_OK&&sample->getMode(&mode)==FMOD_OK&&(mode&FMOD_LOOP_NORMAL));
 const auto attempts=host_music_stats().attempts;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while((host_music_stats().attempts==attempts||host_sfx_is_playing(short_shot))&&std::chrono::steady_clock::now()<deadline){
   host_music_update();std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 SLRR_REQUIRE(host_music_stats().attempts>attempts&&host_music_playing());host_music_stop();
 SLRR_REQUIRE(host_sfx_is_playing(short_loop)&&!host_sfx_is_playing(short_shot));
 host_sfx_stop(short_loop);
 std::puts("PASS streamed music advances at EOF; cached loop survives its boundary while one-shot ends");
 if(argc==3&&std::string(argv[1])=="--root"){
   host_music_set_root(argv[2]);
   for(int set=0;set<4;++set){
     SLRR_REQUIRE(host_music_track_count(set)>0);host_music_change_set(set);SLRR_REQUIRE(host_music_playing());
     for(int frame=0;frame<30;++frame){host_music_update();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
     SLRR_REQUIRE(host_music_playing());
   }
   host_music_stop();std::puts("PASS all four real-game music sets start and stream (muted)");
 }
 std::puts("Playback was muted; generated test samples contain digital silence.");return 0;
}
