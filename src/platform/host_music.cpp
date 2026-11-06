#include "slrr/host_music.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#endif

namespace slrr {
namespace {
struct Playlist { std::vector<std::filesystem::path> tracks; int index=0, mode=3; };
std::array<Playlist,4> sets;
int active=-1;
bool opened=false;
float volume=1.f;
#ifdef _WIN32
struct Chunk { std::vector<BYTE> bytes; std::atomic<bool> busy{false}; };
struct Callback : IXAudio2VoiceCallback {
  void STDMETHODCALLTYPE OnBufferEnd(void* p) override { static_cast<Chunk*>(p)->busy.store(false); }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
  void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT) override {}
};
struct MusicStream {
  IXAudio2* engine=nullptr;
  IXAudio2MasteringVoice* master=nullptr;
  IXAudio2SourceVoice* voice=nullptr;
  IMFSourceReader* reader=nullptr;
  Callback callback;
  std::array<Chunk,24> chunks;
  bool com=false, mf=false, ended=false;
  void close() {
    if(voice){voice->DestroyVoice();voice=nullptr;}
    if(reader){reader->Release();reader=nullptr;}
    for(auto& c:chunks){c.busy=false;c.bytes.clear();}
    ended=false;
  }
  ~MusicStream() {
    close();
    if(master)master->DestroyVoice();
    if(engine)engine->Release();
    if(mf)MFShutdown();
    if(com)CoUninitialize();
  }
  HRESULT init() {
    if(master)return S_OK;
    if(!com){const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
      if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return hr;
      com=SUCCEEDED(hr);}
    HRESULT hr=S_OK;
    if(!mf){hr=MFStartup(MF_VERSION,MFSTARTUP_LITE);if(FAILED(hr))return hr;mf=true;}
    if(!engine){hr=XAudio2Create(&engine);if(FAILED(hr))return hr;}
    return engine->CreateMasteringVoice(&master);
  }
  HRESULT fill() {
    for(auto& c:chunks) {
      if(ended||c.busy.load())continue;
      DWORD flags=0;IMFSample* sample=nullptr;
      HRESULT hr=reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,nullptr,&sample);
      if(FAILED(hr)){if(sample)sample->Release();return hr;}
      if(flags&MF_SOURCE_READERF_ENDOFSTREAM)ended=true;
      if(!sample)continue;
      IMFMediaBuffer* buffer=nullptr;
      hr=sample->ConvertToContiguousBuffer(&buffer);
      if(SUCCEEDED(hr)) {
        BYTE* data=nullptr;DWORD size=0;
        hr=buffer->Lock(&data,nullptr,&size);
        if(SUCCEEDED(hr)) {
          c.bytes.assign(data,data+size);buffer->Unlock();
          if(size){XAUDIO2_BUFFER packet{};packet.AudioBytes=size;packet.pAudioData=c.bytes.data();packet.pContext=&c;
            c.busy=true;hr=voice->SubmitSourceBuffer(&packet);if(FAILED(hr))c.busy=false;}
        }
        buffer->Release();
      }
      sample->Release();
      if(FAILED(hr))return hr;
    }
    return S_OK;
  }
} stream;
#endif
void close_track() {
#ifdef _WIN32
  stream.close();
#endif
  opened=false;
}
void play_track() {
  close_track();
  if(active<0||sets[active].tracks.empty())return;
  auto& p=sets[active];const int count=static_cast<int>(p.tracks.size());
  p.index=(p.index%count+count)%count;
#ifdef _WIN32
  HRESULT hr=stream.init();
  const auto path=std::filesystem::absolute(p.tracks[p.index]).wstring();
  if(SUCCEEDED(hr))hr=MFCreateSourceReaderFromURL(path.c_str(),nullptr,&stream.reader);
  if(SUCCEEDED(hr))hr=stream.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE);
  if(SUCCEEDED(hr))hr=stream.reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE);
  IMFMediaType* type=nullptr;
  if(SUCCEEDED(hr))hr=MFCreateMediaType(&type);
  if(SUCCEEDED(hr))hr=type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);
  if(SUCCEEDED(hr))hr=type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);
  if(SUCCEEDED(hr))hr=stream.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,type);
  if(type){type->Release();type=nullptr;}
  if(SUCCEEDED(hr))hr=stream.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,&type);
  WAVEFORMATEX* format=nullptr;UINT32 bytes=0;
  if(SUCCEEDED(hr))hr=MFCreateWaveFormatExFromMFMediaType(type,&format,&bytes);
  if(type)type->Release();
  if(SUCCEEDED(hr))hr=stream.engine->CreateSourceVoice(&stream.voice,format,0,1.f,&stream.callback);
  if(format)CoTaskMemFree(format);
  if(SUCCEEDED(hr)){stream.voice->SetVolume(volume);hr=stream.fill();}
  if(SUCCEEDED(hr))hr=stream.voice->Start();
  opened=SUCCEEDED(hr);
  if(!opened){std::fprintf(stderr,"[music] playback failed HRESULT=0x%08lx\n",static_cast<unsigned long>(hr));close_track();}
  std::fprintf(stderr,"[music] set=%d track=%d/%d playing=%d %s\n",active,p.index+1,count,opened?1:0,p.tracks[p.index].filename().string().c_str());
#endif
}
}
void host_music_set_root(const std::string& root) {
  host_music_stop();
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
void host_music_next(int direction){if(active<0)return;sets[active].index+=direction<0?-1:1;play_track();}
int host_music_mode(){return active<0?-1:sets[active].mode;}
int host_music_set_mode(int mode){if(active<0)return mode;if(mode>=0&&mode<=3)sets[active].mode=mode;return sets[active].mode;}
void host_music_set_volume(float value){
  volume=std::isfinite(value)?std::clamp(value,0.f,1.f):1.f;
#ifdef _WIN32
  if(stream.voice)stream.voice->SetVolume(volume);
#endif
}
void host_music_update(){
#ifdef _WIN32
  if(!opened)return;
  const HRESULT hr=stream.fill();
  if(FAILED(hr)){std::fprintf(stderr,"[music] stream failed HRESULT=0x%08lx\n",static_cast<unsigned long>(hr));close_track();return;}
  XAUDIO2_VOICE_STATE state{};stream.voice->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
  if(stream.ended&&!state.BuffersQueued)host_music_next();
#endif
}
void host_music_stop(){close_track();active=-1;}
bool host_music_playing(){return opened;}
int host_music_track_count(int set){return set>=0&&set<4?static_cast<int>(sets[set].tracks.size()):0;}
}
