#include "standalone_test.hpp"
#include "slrr/host_audio.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include "../src/platform/audio_reverb.hpp"
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
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
 MIX_Track* channel=host_sfx_track(id);SLRR_REQUIRE(channel);
 host_sfx_pause_loop(id,true);host_music_update();
 SLRR_REQUIRE(MIX_TrackPaused(channel)&&host_sfx_is_playing(id));
 host_sfx_update_loop(id,1.5f,.4f);
 SLRR_REQUIRE(std::abs(MIX_GetTrackFrequencyRatio(channel)-1.5f)<.001f);
 SLRR_REQUIRE(std::abs(MIX_GetTrackGain(channel)-.4f)<.001f);
 host_music_set_volume(.2f);
 SLRR_REQUIRE(std::abs(MIX_GetMixerGain(audio->music)-.2f)<.001f);
 SLRR_REQUIRE(MIX_GetMixerGain(audio->effects)==0);
 SLRR_REQUIRE(MIX_GetMixerGain(audio->fmv)==1.f);
 host_sfx_pause_loop(id,false);SLRR_REQUIRE(MIX_TrackPlaying(channel)&&!MIX_TrackPaused(channel));
 host_sfx_stop(id);SLRR_REQUIRE(!host_sfx_is_playing(id)&&!host_sfx_track(id));
 host_music_set_volume(0);
 std::puts("PASS SDL_mixer loop pause/resume, pitch/gain, stop and independent mixer groups");
 wave(f.root/"music/Main_menu/short.wav",1);
 host_music_set_root(f.root.string());host_music_change_set(3);
 const auto short_path=(f.root/"music/Main_menu/short.wav").string();
 const auto short_loop=host_sfx_play_file(short_path,1,0,true);SLRR_REQUIRE(short_loop>0);
 const auto short_shot=host_sfx_play_file(short_path,1,0,false);SLRR_REQUIRE(short_shot>0);
 channel=host_sfx_track(short_loop);SLRR_REQUIRE(channel&&MIX_GetTrackLoops(channel)==-1);
 SLRR_REQUIRE(MIX_GetTrackAudio(channel)==MIX_GetTrackAudio(host_sfx_track(short_shot)));
 const auto attempts=host_music_stats().attempts;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while((host_music_stats().attempts==attempts||host_sfx_is_playing(short_shot))&&std::chrono::steady_clock::now()<deadline){
   host_music_update();std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 SLRR_REQUIRE(host_music_stats().attempts>attempts&&host_music_playing());host_music_stop();
 SLRR_REQUIRE(host_sfx_is_playing(short_loop)&&!host_sfx_is_playing(short_shot));
 host_sfx_stop(short_loop);
 std::puts("PASS streamed music advances at EOF; cached loop survives its boundary while one-shot ends");
 {
   // Spatial audio (#390): listener at the origin looking down -Z, so +X is right.
   const float eye[3]={0,0,0},forward[3]={0,0,-1},up[3]={0,1,0};
   host_sfx_set_listener(true,eye,forward,up,1.0/60);
   auto& options=host_sfx_spatial_options();options={};host_sfx_apply_spatial_options();
   HostSfxPlace right;right.position[0]=10;right.reference=2;right.maximum=60;right.rolloff=1;
   auto id=host_sfx_play_at(silence.string(),right,1,1,true);
   auto st=host_sfx_spatial_state(id);
   SLRR_REQUIRE(st.found&&st.placed&&std::abs(st.gain-.2f)<1e-4f&&std::abs(st.pan-1.f)<1e-4f&&std::abs(st.doppler-1.f)<1e-4f);
   MIX_Track* track=host_sfx_track(id);
   SLRR_REQUIRE(std::abs(MIX_GetTrackGain(track)-.2f)<1e-4f);
   HostSfxPlace inside=right;inside.position[0]=0;inside.position[2]=-1;
   host_sfx_set_place(id,inside);st=host_sfx_spatial_state(id);
   SLRR_REQUIRE(std::abs(st.gain-1.f)<1e-4f&&std::abs(st.pan)<1e-4f); // inside the reference distance
   HostSfxPlace distant=right;distant.position[0]=61;
   host_sfx_set_place(id,distant);SLRR_REQUIRE(host_sfx_spatial_state(id).gain==0.f);
   host_sfx_stop(id);
   // A source closing in at 30 m/s: (c)/(c-30) with c=343 m/s.
   HostSfxPlace closing;closing.position[2]=-50;closing.velocity[2]=30;closing.maximum=100;
   id=host_sfx_play_at(silence.string(),closing,1.5f,1,true);
   st=host_sfx_spatial_state(id);
   SLRR_REQUIRE(std::abs(st.doppler-343.f/313.f)<1e-4f);
   SLRR_REQUIRE(std::abs(MIX_GetTrackFrequencyRatio(host_sfx_track(id))-1.5f*343.f/313.f)<1e-3f);
   options.doppler=false;host_sfx_apply_spatial_options();
   SLRR_REQUIRE(host_sfx_spatial_state(id).doppler==1.f&&std::abs(MIX_GetTrackFrequencyRatio(host_sfx_track(id))-1.5f)<1e-3f);
   options.positioning=false;host_sfx_apply_spatial_options();
   st=host_sfx_spatial_state(id);SLRR_REQUIRE(st.gain==1.f&&st.pan==0.f);
   host_sfx_stop(id);options={};host_sfx_apply_spatial_options();
   // Unplaced voices (menus, cockpit) stay centred and dry.
   id=host_sfx_play_file(silence.string(),1,1,true);st=host_sfx_spatial_state(id);
   SLRR_REQUIRE(st.found&&!st.placed&&st.gain==1.f&&st.pan==0.f);host_sfx_stop(id);
   host_sfx_set_listener(false,eye,forward,up,1.0/60);
   std::puts("PASS placed voices: inverse distance gain, listener-space pan, Doppler and option switches");
   // Environment reverb: an impulse rings in every room and stops in None.
   for(int preset=0;preset<=3;++preset){
     EnvironmentReverb reverb;reverb.set_preset(preset);
     const int rate=48000,frames=rate*2;std::vector<float> in(frames*2,0.f),out(frames*2,0.f);
     in[0]=in[1]=1.f;
     reverb.process(in.data(),out.data(),frames,2,rate);
     double early=0,late=0;
     for(int f=0;f<frames;++f){const double e=out[2*f]*out[2*f]+out[2*f+1]*out[2*f+1];(f<rate/2?early:late)+=e;}
     if(preset==0)SLRR_REQUIRE(early==0&&late==0);
     else SLRR_REQUIRE(early>1e-4&&late<early&&std::isfinite(late));
     std::printf("  reverb preset %d: early %.4g, late %.4g\n",preset,early,late);
   }
   std::puts("PASS environment reverb rings per preset and decays; None is dry");
 }
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
