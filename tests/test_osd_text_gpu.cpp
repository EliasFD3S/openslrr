// Opt-in Windows/GPU integration test, using read-only local font assets.
#include "save_io_fixture.hpp"
#include "slrr/scene_presentation.hpp"
#include "../src/render/bgfx_lib_backend.hpp"
using namespace slrr;
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  LibBgfxBackend backend;
  try {
    osd_clear_widgets();slrr_text_clear_all();
    OsdWidget sentinel; sentinel.hidden=true; osd_add_widget(sentinel); // Same touched OSD views in empty/full frames.
    slrr_font_set_game_root(fs::absolute("game").string());
    Object font;
    SLRR_REQUIRE(slrr_font_load(&font,"slii24"));
    SLRR_REQUIRE(backend.open(1280,800,"OSD text batch regression"));
    backend.set_osd_overlay(true);
    backend.set_garage_viewport(true,0); // Keep the normal presentation gate open.
    SetWindowPos(static_cast<HWND>(backend.native_hwnd()),nullptr,-32000,-32000,0,0,
                 SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    auto settle=[&](){for(int i=0;i<8;++i){backend.flush();ShowWindow(static_cast<HWND>(backend.native_hwnd()),SW_HIDE);}return bgfx::getStats()->numDraw;};
    std::printf("ready=%d overlay=%d transition=%d\n",backend.ready(),backend.osd_draw_enabled(),scene_transition_blocks_presentation());
    const auto empty=settle();
    std::vector<Object> lines(20);
    Object owners[3], viewports[3];
    for(int i=0;i<3;++i){viewports[i].fields["pri"]=Value::make_int(i);owners[i].fields["vp"]=Value::make_obj(&viewports[i]);}
    for(int i=0;i<20;++i) {
      slrr_text_create(&lines[i],&font,-.95f,-.95f+i*.09f);
      slrr_text_set_string(&lines[i],std::string(50,'A').c_str());
      slrr_text_set_color(&lines[i],i%2?0x80ffd040u:0xff60a0ffu);
      slrr_text_set_osd(&lines[i],&owners[i%3]);
      slrr_text_update(&lines[i]);
    }
    std::printf("registered=%zu font=%d ready=%d\n",slrr_osd_texts().size(),slrr_font_ready(&font),backend.ready());
    const auto full=settle();
    const auto path=fs::path(argv[0]).replace_extension(".tga");
    SLRR_REQUIRE(backend.request_screenshot(fs::absolute(path).string()));settle();
    for(auto& line:lines)slrr_text_destroy(&line);
    const auto hidden=settle();backend.close();slrr_font_forget(&font);
    std::printf("GPU text: 20 lines / 1000 glyphs; empty=%u full=%u hidden=%u text_draws=%u\n",empty,full,hidden,full-empty);
    SLRR_REQUIRE(full>=empty&&full-empty==20&&hidden==empty);
    return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"osd_text_gpu: %s\n",e.what());if(backend.ready())backend.close();return 1;}
}
