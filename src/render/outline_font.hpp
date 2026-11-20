#pragma once

#include "slrr/slrr_font.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// The pinned bgfx dependency already supplies stb. Keep its implementation local
// to the font loader (ImGui has its own private copy).
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

namespace slrr {
namespace {
// SDF alpha = 128 at the contour, increasing by 16 per pixel inside.
// Eight pixels of padding cover the bounded outline and shadow options.
constexpr int kFontSdfPadding = 8;
constexpr float kFontSdfScale = 16.f;

// Reject incomplete SFNT files before passing trusted local font assets to stb.
// This is not a sandbox for hostile fonts; stb's internal parser is unbounded.
bool font_sfnt_complete(const std::vector<uint8_t>& bytes) {
  auto u16 = [&](size_t p) { return (uint32_t(bytes[p]) << 8) | bytes[p + 1]; };
  auto u32 = [&](size_t p) { return (u16(p) << 16) | u16(p + 2); };
  if (bytes.size() < 12) return false;
  const auto signature = u32(0);
  if (signature != 0x00010000 && signature != 0x4f54544f) return false;
  const auto count = u16(4);
  if (!count || count > (bytes.size() - 12) / 16) return false;
  for (size_t i = 0; i < count; ++i) {
    const size_t p = 12 + i * 16;
    const size_t offset = u32(p + 8), length = u32(p + 12);
    if (offset > bytes.size() || length > bytes.size() - offset) return false;
  }
  return true;
}

struct FontPoint { float x, y; };
struct FontEdge { FontPoint a, b; };
FontPoint midpoint(FontPoint a, FontPoint b) { return {(a.x+b.x)*.5f, (a.y+b.y)*.5f}; }
float edge_distance_sq(FontPoint p, FontPoint a, FontPoint b) {
  const float dx=b.x-a.x, dy=b.y-a.y, length=dx*dx+dy*dy;
  const float t=length > 0.f ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/length,0.f,1.f) : 0.f;
  const float x=p.x-a.x-t*dx, y=p.y-a.y-t*dy;
  return x*x+y*y;
}
void flatten_font_curve(std::vector<FontEdge>& edges, FontPoint a, FontPoint b,
                        FontPoint c, FontPoint d, int depth=0) {
  // Cubic De Casteljau subdivision also handles converted quadratic curves.
  if (depth == 10 || (edge_distance_sq(b,a,d)<.0225f && edge_distance_sq(c,a,d)<.0225f)) {
    edges.push_back({a,d}); return;
  }
  const auto ab=midpoint(a,b), bc=midpoint(b,c), cd=midpoint(c,d);
  const auto abc=midpoint(ab,bc), bcd=midpoint(bc,cd), split=midpoint(abc,bcd);
  flatten_font_curve(edges,a,ab,abc,split,depth+1);
  flatten_font_curve(edges,split,bcd,cd,d,depth+1);
}

int font_winding(const std::vector<FontEdge>& edges, double x, double y) {
  int winding=0;
  for (const auto& e:edges) {
    const double cross=(e.b.x-e.a.x)*(y-e.a.y)-(x-e.a.x)*(e.b.y-e.a.y);
    if (e.a.y<=y && e.b.y>y && cross>0) ++winding;
    else if (e.b.y<=y && e.a.y>y && cross<0) --winding;
  }
  return winding;
}

// Overlapping components can have edges inside the filled union. Split at
// crossings first, then retain only edges separating filled and empty space;
// otherwise SDF distance zero would create faded seams inside solid glyphs.
bool font_boundary_edges(const std::vector<FontEdge>& edges, std::vector<FontEdge>& boundary) {
  for (const auto& e:edges) {
    const double dx=e.b.x-e.a.x, dy=e.b.y-e.a.y, length_sq=dx*dx+dy*dy;
    if (length_sq<1e-12) continue;
    std::vector<double> cuts{0,1};
    auto cut=[&](double t){if(t>0 && t<1) cuts.push_back(t);};
    for (const auto& other:edges) {
      const double sx=other.b.x-other.a.x, sy=other.b.y-other.a.y;
      const double px=other.a.x-e.a.x, py=other.a.y-e.a.y;
      const double denominator=dx*sy-dy*sx;
      if (std::fabs(denominator)>1e-10) {
        const double t=(px*sy-py*sx)/denominator, u=(px*dy-py*dx)/denominator;
        if (u>=0 && u<=1) cut(t);
      } else if (std::fabs(px*dy-py*dx)<1e-10) {
        cut((px*dx+py*dy)/length_sq);
        cut(((other.b.x-e.a.x)*dx+(other.b.y-e.a.y)*dy)/length_sq);
      }
    }
    std::sort(cuts.begin(),cuts.end());
    cuts.erase(std::unique(cuts.begin(),cuts.end(),[](double a,double b){return b-a<1e-7;}),cuts.end());
    const double nx=-dy/std::sqrt(length_sq)*1e-4, ny=dx/std::sqrt(length_sq)*1e-4;
    for (size_t i=1;i<cuts.size();++i) {
      const double mid=(cuts[i-1]+cuts[i])*.5, x=e.a.x+dx*mid, y=e.a.y+dy*mid;
      if ((font_winding(edges,x+nx,y+ny)!=0)==(font_winding(edges,x-nx,y-ny)!=0)) continue;
      boundary.push_back({{float(e.a.x+dx*cuts[i-1]),float(e.a.y+dy*cuts[i-1])},
                          {float(e.a.x+dx*cuts[i]),float(e.a.y+dy*cuts[i])}});
      if (boundary.size()>16384) return false;
    }
  }
  return true;
}

// stb's SDF routine ignores CFF cubic curves. Flatten both outline formats to
// pixel-space edges instead, then evaluate signed distance with nonzero winding.
// This keeps holes and gives TTF and CFF the same distance/coverage convention.
bool font_sdf(const stbtt_fontinfo& info, int glyph, float scale, int left, int top,
              int w, int h, std::vector<uint8_t>& sdf) {
  stbtt_vertex* shape=nullptr;
  const int count=stbtt_GetGlyphShape(&info,glyph,&shape);
  if (!shape || count<=0) return false;
  std::vector<FontEdge> edges;
  FontPoint pen{}, start{};
  auto point=[&](int x,int y){return FontPoint{x*scale,-y*scale};};
  for (int i=0;i<count;++i) {
    const auto& v=shape[i]; const auto end=point(v.x,v.y);
    if (v.type==STBTT_vmove) {
      if (pen.x!=start.x || pen.y!=start.y) edges.push_back({pen,start});
      start=end;
    } else if (v.type==STBTT_vline) edges.push_back({pen,end});
    else if (v.type==STBTT_vcurve) {
      const auto control=point(v.cx,v.cy);
      const FontPoint b{pen.x+(control.x-pen.x)*2.f/3.f,pen.y+(control.y-pen.y)*2.f/3.f};
      const FontPoint c{end.x+(control.x-end.x)*2.f/3.f,end.y+(control.y-end.y)*2.f/3.f};
      flatten_font_curve(edges,pen,b,c,end);
    } else if (v.type==STBTT_vcubic)
      flatten_font_curve(edges,pen,point(v.cx,v.cy),point(v.cx1,v.cy1),end);
    pen=end;
    if (edges.size()>4096) { stbtt_FreeShape(&info,shape); return false; }
  }
  if (pen.x!=start.x || pen.y!=start.y) edges.push_back({pen,start});
  stbtt_FreeShape(&info,shape);
  if (edges.empty()) return false;
  std::vector<FontEdge> boundary;
  if (!font_boundary_edges(edges,boundary)) return false;
  sdf.resize(size_t(w)*h);
  for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
    const FontPoint p{left+x+.5f,top+y+.5f};
    float distance_sq=float(kFontSdfPadding*kFontSdfPadding);
    for (const auto& e:boundary)
      distance_sq=std::min(distance_sq,edge_distance_sq(p,e.a,e.b));
    const float distance=std::sqrt(distance_sq)*(font_winding(edges,p.x,p.y) ? 1.f : -1.f);
    sdf[size_t(y)*w+x]=uint8_t(std::clamp(128.f+distance*kFontSdfScale,0.f,255.f));
  }
  return true;
}

bool rasterize_outline_font(const std::vector<uint8_t>& bytes, SlrrFont& font) {
  if (!font_sfnt_complete(bytes)) return false;
  stbtt_fontinfo info{};
  if (!stbtt_InitFont(&info, bytes.data(), 0)) return false;
  const float scale = stbtt_ScaleForMappingEmToPixels(&info, float(font.px_height));
  if (!std::isfinite(scale) || scale <= 0.f) return false;
  int ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
  const float baseline = ascent * scale;
  constexpr int atlas_width = 1024, max_height = 4096;
  // Bound allocation and reject oversized replacement glyphs.
  std::vector<uint8_t> alpha(atlas_width * max_height, 0);
  font.glyphs.assign(256, {});
  int x = 1, y = 1, row_height = 0;
  for (int ch = 0; ch < 256; ++ch) {
    auto& g = font.glyphs[ch];
    const int index = stbtt_FindGlyphIndex(&info, ch);
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
    g.adv_px = std::max(0.f, advance * scale);
    if (!index || stbtt_IsGlyphEmpty(&info, index)) continue;
    int left = 0, top = 0, right = 0, bottom = 0;
    stbtt_GetGlyphBitmapBox(&info, index, scale, scale, &left, &top, &right, &bottom);
    if (right - left > 256 || bottom - top > 256) return false;
    const int ox = left - kFontSdfPadding, oy = top - kFontSdfPadding;
    const int w = right - left + 2*kFontSdfPadding, h = bottom - top + 2*kFontSdfPadding;
    if (x + w + 1 > atlas_width) { x = 1; y += row_height + 1; row_height = 0; }
    if (w <= 0 || h <= 0 || x + w + 1 > atlas_width || y + h + 1 > max_height) return false;
    std::vector<uint8_t> sdf;
    if (!font_sdf(info,index,scale,ox,oy,w,h,sdf)) return false;
    for (int row = 0; row < h; ++row)
      std::memcpy(alpha.data() + (y + row) * atlas_width + x, sdf.data() + row * w, w);
    g.px[0] = g.px[2] = float(ox); g.px[1] = g.px[3] = float(ox + w);
    g.py[0] = g.py[1] = float(oy) + baseline;
    g.py[2] = g.py[3] = float(oy + h) + baseline;
    // Store pixel coordinates until the final atlas height is known.
    g.u0 = float(x); g.u1 = float(x + w); g.v0 = float(y); g.v1 = float(y + h);
    x += w + 1; row_height = std::max(row_height, h);
  }
  font.atlas_w = atlas_width;
  font.atlas_h = 1;
  while (font.atlas_h < y + row_height + 1) font.atlas_h *= 2;
  font.rgba.assign(size_t(font.atlas_w) * font.atlas_h * 4, 255);
  for (size_t i = 0; i < font.rgba.size() / 4; ++i) font.rgba[i * 4 + 3] = alpha[i];
  for (auto& g : font.glyphs) {
    g.u0 /= font.atlas_w; g.u1 /= font.atlas_w;
    g.v0 /= font.atlas_h; g.v1 /= font.atlas_h;
    g.tu[0] = g.tu[2] = g.u0; g.tu[1] = g.tu[3] = g.u1;
    g.tv[0] = g.tv[1] = g.v0; g.tv[2] = g.tv[3] = g.v1;
  }
  return true;
}
} // namespace
} // namespace slrr
