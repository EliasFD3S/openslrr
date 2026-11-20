#include "save_io_fixture.hpp"
#include "slrr/video_cinepak.hpp"
#include "slrr/video_frame.hpp"
#include <fstream>
#include "slrr/video_fmv.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/mainloop.hpp"
#include <vfw.h>
using namespace slrr;
int main() {
  const std::vector<uint8_t> sample={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  std::vector<uint8_t> output;
  SLRR_REQUIRE(video_copy_rgb32(sample,2,-2,output) && output==sample);
  SLRR_REQUIRE(video_copy_rgb32(sample,2,2,output));
  SLRR_REQUIRE(output==std::vector<uint8_t>({9,10,11,12,13,14,15,16,1,2,3,4,5,6,7,8}));
  const auto previous=output;
  SLRR_REQUIRE(!video_copy_rgb32(std::span<const uint8_t>(sample.data(),15),2,2,output));
  SLRR_REQUIRE(output==previous);
  SLRR_REQUIRE(!video_copy_rgb32({},2,2,output) && output==previous);
  for (auto [w,h]:{std::pair{0,2},std::pair{-1,2},std::pair{1,0},std::pair{1,INT32_MIN},std::pair{INT32_MAX,INT32_MAX}})
    SLRR_REQUIRE(!video_rgb32_bytes(w,h) && !video_copy_rgb32(sample,w,h,output));
  auto extended=sample;extended.push_back(99);
  SLRR_REQUIRE(video_copy_rgb32(extended,2,-2,output) && output==sample);
  std::puts("PASS top-down/bottom-up RGB32; short samples preserve output; invalid/overflow dimensions rejected");
  // Cinepak frames of 8x4 pixels: two 4x4 blocks in one strip.
  auto chunk=[](uint8_t id,std::vector<uint8_t> body){
    const auto n=body.size()+4;body.insert(body.begin(),{id,uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)});return body;};
  auto cvid=[&](std::vector<uint8_t> chunks){
    auto strip=chunks;const auto s=strip.size()+12;
    strip.insert(strip.begin(),{0x10,uint8_t(s>>16),uint8_t(s>>8),uint8_t(s),0,0,0,0,0,4,0,8});
    const auto n=strip.size()+10;
    strip.insert(strip.begin(),{0,uint8_t(n>>16),uint8_t(n>>8),uint8_t(n),0,8,0,4,0,1});return strip;};
  auto cat=[](std::vector<uint8_t> a,const std::vector<uint8_t>& b){a.insert(a.end(),b.begin(),b.end());return a;};
  // V1 entries 0 (Y 100, U 10, V -20) and 1 (grey 200 when chroma is 0); all-V1 vectors 0, 1.
  const auto key=cvid(cat(chunk(0x23,{0xC0,0,0,0,100,100,100,100,10,0xEC,200,200,200,200,0,0}),chunk(0x32,{0,1})));
  // Inter: block 0 skipped, block 1 takes V1 entry 0.
  const auto inter=cvid(chunk(0x31,{0x40,0,0,0,0}));
  // Grey V4 entry 0 (10,20,30,40); block 0 uses V4, block 1 V1 entry 1.
  const auto mixed=cvid(cat(chunk(0x25,{0x80,0,0,0,10,20,30,40}),chunk(0x30,{0x80,0,0,0,0,0,0,0,1})));
  auto pixel=[](const std::vector<uint8_t>& bgra,int x,int y){const auto* p=&bgra[(y*8+x)*4];return std::vector<uint8_t>(p,p+4);};
  const std::vector<uint8_t> colour={120,115,60,255},grey={200,200,200,255};
  CinepakDecoder decoder;decoder.reset();std::vector<uint8_t> image;
  SLRR_REQUIRE(decoder.decode(key,8,4,image) && image.size()==8*4*4);
  SLRR_REQUIRE(pixel(image,0,0)==colour && pixel(image,3,3)==colour && pixel(image,4,0)==grey && pixel(image,7,3)==grey);
  SLRR_REQUIRE(decoder.decode(inter,8,4,image) && pixel(image,0,0)==colour && pixel(image,4,0)==colour && pixel(image,7,3)==colour);
  SLRR_REQUIRE(decoder.decode(mixed,8,4,image) && pixel(image,0,0)==std::vector<uint8_t>({10,10,10,255}) &&
               pixel(image,1,0)==std::vector<uint8_t>({20,20,20,255}) && pixel(image,0,1)==std::vector<uint8_t>({30,30,30,255}) &&
               pixel(image,3,3)==std::vector<uint8_t>({40,40,40,255}) && pixel(image,5,2)==grey);
  const auto before_bad=image;
  SLRR_REQUIRE(!decoder.decode(std::span<const uint8_t>(key.data(),9),8,4,image) && image==before_bad);
  SLRR_REQUIRE(!decoder.decode(key,8,0,image));
  std::puts("PASS Cinepak V1/V4 codebooks, YUV colour, skipped inter blocks and malformed frames");
  Fixture f;
  // Silent uncompressed AVI exercises cancellation inside active playback.
  const auto movies=f.root/"Data/FMV";fs::create_directories(movies);
  AVIFileInit();
  PAVIFILE avi=nullptr;PAVISTREAM stream=nullptr;
  SLRR_REQUIRE(AVIFileOpenW(&avi,(movies/"Activision.avi").c_str(),OF_CREATE|OF_WRITE,nullptr)==0);
  AVISTREAMINFOW info{};info.fccType=streamtypeVIDEO;info.dwScale=1;info.dwRate=1;
  info.dwSuggestedBufferSize=16;SetRect(&info.rcFrame,0,0,2,2);
  SLRR_REQUIRE(AVIFileCreateStreamW(avi,&stream,&info)==0);
  BITMAPINFOHEADER bitmap{};bitmap.biSize=sizeof(bitmap);bitmap.biWidth=2;bitmap.biHeight=2;
  bitmap.biPlanes=1;bitmap.biBitCount=32;bitmap.biCompression=BI_RGB;bitmap.biSizeImage=16;
  SLRR_REQUIRE(AVIStreamSetFormat(stream,0,&bitmap,sizeof(bitmap))==0);
  uint8_t pixels[16]{};
  for(int i=0;i<5;++i)SLRR_REQUIRE(AVIStreamWrite(stream,i,1,pixels,16,AVIIF_KEYFRAME,nullptr,nullptr)==0);
  AVIStreamRelease(stream);AVIFileRelease(avi);
  // A Cinepak AVI with PCM audio plays through the built-in decoder.
  const auto cinepak_path=movies/"Cinepak.avi";
  SLRR_REQUIRE(AVIFileOpenW(&avi,cinepak_path.c_str(),OF_CREATE|OF_WRITE,nullptr)==0);
  info={};info.fccType=streamtypeVIDEO;info.fccHandler=mmioFOURCC('c','v','i','d');info.dwScale=1;info.dwRate=20;
  SetRect(&info.rcFrame,0,0,8,4);
  SLRR_REQUIRE(AVIFileCreateStreamW(avi,&stream,&info)==0);
  bitmap.biWidth=8;bitmap.biHeight=4;bitmap.biBitCount=24;bitmap.biCompression=mmioFOURCC('c','v','i','d');bitmap.biSizeImage=0;
  SLRR_REQUIRE(AVIStreamSetFormat(stream,0,&bitmap,sizeof(bitmap))==0);
  SLRR_REQUIRE(AVIStreamWrite(stream,0,1,(void*)key.data(),LONG(key.size()),AVIIF_KEYFRAME,nullptr,nullptr)==0);
  SLRR_REQUIRE(AVIStreamWrite(stream,1,1,(void*)inter.data(),LONG(inter.size()),0,nullptr,nullptr)==0);
  PAVISTREAM sound=nullptr;AVISTREAMINFOW sound_info{};sound_info.fccType=streamtypeAUDIO;sound_info.dwScale=4;
  sound_info.dwRate=4*8000;sound_info.dwSampleSize=4;
  SLRR_REQUIRE(AVIFileCreateStreamW(avi,&sound,&sound_info)==0);
  WAVEFORMATEX wave{};wave.wFormatTag=WAVE_FORMAT_PCM;wave.nChannels=2;wave.nSamplesPerSec=8000;wave.wBitsPerSample=16;
  wave.nBlockAlign=4;wave.nAvgBytesPerSec=32000;
  SLRR_REQUIRE(AVIStreamSetFormat(sound,0,&wave,sizeof(wave))==0);
  std::vector<uint8_t> silence(4*800);
  SLRR_REQUIRE(AVIStreamWrite(sound,0,800,silence.data(),LONG(silence.size()),AVIIF_KEYFRAME,nullptr,nullptr)==0);
  AVIStreamRelease(sound);AVIStreamRelease(stream);AVIFileRelease(avi);AVIFileExit();
  {
    std::ifstream file(cinepak_path,std::ios::binary);AviCinepakInfo index;
    SLRR_REQUIRE(avi_read_cinepak(file,index) && index.width==8 && index.height==4 && index.frames.size()==2 &&
                 index.frame_seconds==0.05 && index.audio_channels==2 && index.audio_rate==8000 && index.audio_bits==16 &&
                 !index.audio.empty());
    std::ifstream plain(movies/"Activision.avi",std::ios::binary);
    SLRR_REQUIRE(!avi_read_cinepak(plain,index));
  }
  video_fmv_set_game_root(f.root.string());
  SLRR_REQUIRE(video_fmv_open("Data\\FMV\\Cinepak.avi",1,0)==0 && video_fmv_is_playing());
  video_fmv_pump();
  SLRR_REQUIRE(video_fmv_frame_ready() && video_fmv_width()==8 && video_fmv_height()==4 && video_fmv_frame_serial()==1);
  SLRR_REQUIRE(std::vector<uint8_t>(video_fmv_bgra(),video_fmv_bgra()+16)==cat(cat(colour,colour),cat(colour,colour)));
  for(int i=0;i<400 && video_fmv_is_playing();++i){video_fmv_pump();Sleep(5);}
  SLRR_REQUIRE(!video_fmv_is_playing());
  video_fmv_close();
  std::puts("PASS Cinepak AVI index, built-in playback and end of stream");
  int playback_ticks=0;
  const auto active_played=video_fmv_play_boot_intros(0,[&](){return ++playback_ticks<2;});
  SLRR_REQUIRE(active_played==1 && playback_ticks==2 && !video_fmv_is_playing() && !video_fmv_block_osd());
  std::puts("PASS cancellation during a silent AVI stops playback and skips later intros");
  auto backend=create_render_backend(RenderBackendKind::Bgfx);
  SLRR_REQUIRE(backend && backend->open(320,240,"OpenSLRR shutdown test"));
  auto hwnd=static_cast<HWND>(bgfx_native_hwnd(*backend));
  SLRR_REQUIRE(hwnd && IsWindow(hwnd));
  ShowWindow(hwnd,SW_HIDE);
  SendMessageW(hwnd,WM_CLOSE,0,0);
  SLRR_REQUIRE(backend->quit_requested() && IsWindow(hwnd));
  const auto frame=mainloop_frame(f.vm,*backend,f.sections,1.0/60.0);
  SLRR_REQUIRE(frame.quit && IsWindow(hwnd));
  int ticks=0;
  const auto played=video_fmv_play_boot_intros(0,[&](){++ticks;return !backend->quit_requested();});
  SLRR_REQUIRE(played==0 && ticks==1 && !video_fmv_block_osd());
  backend->close();
  SLRR_REQUIRE(!IsWindow(hwnd) && !backend->ready());
  backend->close();
  std::puts("PASS WM_CLOSE retains HWND until backend close; quit frame returns; close is repeatable");
}
