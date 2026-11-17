#include "save_io_fixture.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/host_music.hpp"
#include <cstring>
#include <limits>
using namespace slrr;
void wave(const fs::path& path,int seconds=60){
 const uint32_t data=8000*2*seconds;std::vector<uint8_t>b(44+data);
 auto put=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));};
 std::memcpy(b.data(),"RIFF",4);put(4,36+data);std::memcpy(b.data()+8,"WAVEfmt ",8);put(16,16);b[20]=1;b[22]=1;put(24,8000);put(28,16000);b[32]=2;b[34]=16;std::memcpy(b.data()+36,"data",4);put(40,data);
 std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());
}
int main(){
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
 std::puts("All test samples contain digital silence; playback was muted.");return 0;
}
