#include "save_io_fixture.hpp"
#include "slrr/video_frame.hpp"
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
  AVIStreamRelease(stream);AVIFileRelease(avi);AVIFileExit();
  video_fmv_set_game_root(f.root.string());
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
