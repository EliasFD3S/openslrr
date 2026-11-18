#include "standalone_test.hpp"
#include "../src/render/osd_text_batch.hpp"
#include <cmath>
using namespace slrr;
namespace {
SlrrFont font() {
  SlrrFont f; f.px_height=20; f.glyphs.resize(256);
  for (int ch : {int('A'), int('B'), 200}) {
    auto& g=f.glyphs[ch]; g.adv_px=10; g.u0=g.v0=.1f; g.u1=g.v1=.9f;
    const float px[4]={-1,8,8,-1}, py[4]={2,2,14,14};
    for(int c=0;c<4;++c){g.px[c]=px[c];g.py[c]=py[c];g.tu[c]=.1f+c*.2f;g.tv[c]=.9f-c*.2f;}
  }
  f.glyphs[' '].adv_px=5; return f;
}
bool near(float a,float b){return std::fabs(a-b)<1e-5f;}
SLRR_TEST(layout_matches_authored_corners_and_alignment) {
  auto f=font();
  for(int align:{0,1,2}) {
    OsdTextRun run(f,"A B",.4f,-.6f,100,200,align,25);
    std::vector<OsdTextVertex> v;std::vector<uint16_t> idx;
    SLRR_REQUIRE(run.next(100,v,idx)&&v.size()==8&&idx.size()==12);
    const float origin=.4f-(align==0?.5f:align==1?.25f:0);
    SLRR_REQUIRE(near(v[0].x,origin-.02f)&&near(v[0].y,-.58f));
    SLRR_REQUIRE(near(v[2].x,origin-.02f)&&near(v[2].y,-.46f));
    SLRR_REQUIRE(near(v[2].u,.7f)&&near(v[2].v,.3f));
    SLRR_REQUIRE(near(v[4].x,origin+.28f));
    const std::vector<uint16_t> expected={0,1,2,0,2,3,4,5,6,4,6,7};
    SLRR_REQUIRE(idx==expected&&!run.next(100,v,idx));
  }
}
SLRR_TEST(missing_and_extended_bytes_keep_legacy_advance) {
  auto f=font();std::string text="A? ";text.push_back(char(200));
  OsdTextRun run(f,text,0,0,100,100,2,0);
  std::vector<OsdTextVertex> v;std::vector<uint16_t> idx;
  SLRR_REQUIRE(!run.next(0,v,idx));
  SLRR_REQUIRE(run.next(1,v,idx)&&v.size()==4);
  SLRR_REQUIRE(run.next(1,v,idx)&&near(v[0].x,.42f));
  SLRR_REQUIRE(!run.next(1,v,idx));
}
SLRR_TEST(long_runs_split_without_wrapped_indices_or_pen_reset) {
  auto f=font();std::string text(17000,'A');
  OsdTextRun run(f,text,0,0,100,100,2,0);
  std::vector<OsdTextVertex> v;std::vector<uint16_t> idx;
  SLRR_REQUIRE(osd_text_capacity(3,100)==0&&osd_text_capacity(100,5)==0);
  SLRR_REQUIRE(osd_text_capacity(20,18)==3);
  SLRR_REQUIRE(run.next(999999,v,idx)&&v.size()==kOsdTextBatchGlyphs*4);
  for(auto i:idx)SLRR_REQUIRE(i<v.size());
  SLRR_REQUIRE(run.next(999999,v,idx)&&v.size()==(17000-kOsdTextBatchGlyphs)*4);
  SLRR_REQUIRE(std::fabs(v[0].x-(kOsdTextBatchGlyphs*.2f-.02f))<1.f);
  SLRR_REQUIRE(idx[0]==0&&!run.next(999999,v,idx));
}
SLRR_TEST(thousand_letters_submit_as_one_batch_with_tight_buffers) {
  auto f=font();std::string text(1000,'A');
  OsdTextRun run(f,text,0,0,1920,1080,2,0);
  std::vector<OsdTextVertex> v;std::vector<uint16_t> idx;int batches=0;
  while(run.next(osd_text_capacity(4000,6000),v,idx)) {
    ++batches;SLRR_REQUIRE(v.size()==4000&&idx.size()==6000);
  }
  SLRR_REQUIRE(batches==1);
  OsdTextRun spaces(f,"   ",0,0,100,100,2,0);
  SLRR_REQUIRE(!spaces.next(100,v,idx));
}
}
int main(){const standalone::Test tests[]={layout_matches_authored_corners_and_alignment,missing_and_extended_bytes_keep_legacy_advance,long_runs_split_without_wrapped_indices_or_pen_reset,thousand_letters_submit_as_one_batch_with_tight_buffers};return standalone::run("osd_text_batch",tests);}
