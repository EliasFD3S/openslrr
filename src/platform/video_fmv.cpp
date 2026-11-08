#include "slrr/video_fmv.hpp"

#ifdef _WIN64
#include "video_fmv_vlc.hpp"
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <dshow.h>
#include <vfw.h>
#include <windows.h>

struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample *) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE *, long) = 0;
};
struct __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *, long *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *, long) = 0;
};

static const GUID kClsidSampleGrabber = {
    0xC1F400A0,
    0x3F08,
    0x11d3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kClsidNullRenderer = {
    0xC1F400A4,
    0x3F08,
    0x11d3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kIidISampleGrabber = {
    0x6B652FFF,
    0x11FE,
    0x4fce,
    {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
static constexpr long kFmvExclusiveWindowStyle = 0x46000000L;
static constexpr long kFmvNotifyMsg = 0x40D;
#endif

namespace slrr {
namespace {

std::string g_root;
int g_audio_streams = 0;
#ifdef _WIN32
HWND g_hwnd = nullptr;
IGraphBuilder *g_graph = nullptr;
IMediaControl *g_control = nullptr;
IMediaSeeking *g_seeking = nullptr;
IMediaEventEx *g_event = nullptr;
IVideoWindow *g_vwin = nullptr;
IBasicVideo *g_bvideo = nullptr;
ISampleGrabber *g_grabber = nullptr;
bool g_com = false;
bool g_ds_ready = false;
bool g_playing = false;
bool g_loop = false;
bool g_non_exclusive = false;
bool g_frame_ready = false;
#ifdef _WIN64
bool g_use_vlc = false;
uint64_t g_vlc_frame_serial = 0;
#endif
int g_w = 0;
int g_h = 0;
char g_path[MAX_PATH] = {};
std::vector<uint8_t> g_scratch;
std::vector<uint8_t> g_bgra;
bool g_use_avi = false;
PAVISTREAM g_avi_stream = nullptr;
PGETFRAME g_avi_get = nullptr;
LONG g_avi_frame = 0;
LONG g_avi_start = 0;
LONG g_avi_end = 0;
DWORD g_avi_ms = 33;
DWORD g_avi_next = 0;

std::wstring to_wide(const char *path) {
  if (!path)
    return {};
  const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_ACP, 0, path, -1, out.data(), n);
  return out;
}

std::string resolve_path(const char *path) {
  if (!path || !path[0])
    return {};
  // Absolute or already rooted.
  if (path[0] == '\\' || path[0] == '/' ||
      (std::strlen(path) > 2 && path[1] == ':'))
    return path;
  if (g_root.empty())
    return path;
  std::string full = g_root;
  if (full.back() != '\\' && full.back() != '/')
    full += '\\';
  full += path;
  for (char &c : full)
    if (c == '/')
      c = '\\';
  return full;
}

HRESULT get_pin(IBaseFilter *filter, PIN_DIRECTION dir, IPin **out) {
  *out = nullptr;
  IEnumPins *en = nullptr;
  if (FAILED(filter->EnumPins(&en)) || !en)
    return E_FAIL;
  IPin *pin = nullptr;
  while (en->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) {
      *out = pin;
      en->Release();
      return S_OK;
    }
    pin->Release();
  }
  en->Release();
  return E_FAIL;
}

bool pin_major_is(IPin *pin, const GUID &major) {
  if (!pin)
    return false;
  IEnumMediaTypes *em = nullptr;
  if (FAILED(pin->EnumMediaTypes(&em)) || !em)
    return false;
  AM_MEDIA_TYPE *mt = nullptr;
  bool match = false;
  if (em->Next(1, &mt, nullptr) == S_OK && mt) {
    match = mt->majortype == major;
    if (mt->pbFormat)
      CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk)
      mt->pUnk->Release();
    CoTaskMemFree(mt);
  }
  em->Release();
  return match;
}

// SampleGrabber graph only renders the video pin. AVI audio stays loose,
// so boot intros and prime.avi are silent. Render those pins into the
// default DirectSound renderer.
int render_audio_pins() {
  if (!g_graph)
    return 0;
  IEnumFilters *filters = nullptr;
  if (FAILED(g_graph->EnumFilters(&filters)) || !filters)
    return 0;
  int linked = 0;
  IBaseFilter *filter = nullptr;
  while (filters->Next(1, &filter, nullptr) == S_OK) {
    IEnumPins *pins = nullptr;
    if (SUCCEEDED(filter->EnumPins(&pins)) && pins) {
      IPin *pin = nullptr;
      while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir;
        IPin *peer = nullptr;
        const bool out =
            SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT;
        const bool loose = out && pin->ConnectedTo(&peer) != S_OK;
        if (peer)
          peer->Release();
        if (loose && pin_major_is(pin, MEDIATYPE_Audio)) {
          IBaseFilter *output = nullptr;
          HRESULT hr = CoCreateInstance(CLSID_DSoundRender, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                                        reinterpret_cast<void **>(&output));
          if (SUCCEEDED(hr) && output) {
            hr = g_graph->AddFilter(output, L"AVI sound output");
            IPin *input = nullptr;
            if (SUCCEEDED(hr))
              hr = get_pin(output, PINDIR_INPUT, &input);
            if (SUCCEEDED(hr) && input)
              hr = g_graph->Connect(pin, input);
            if (input)
              input->Release();
            output->Release();
          }
          if (SUCCEEDED(hr))
            ++linked;
        }
        pin->Release();
      }
      pins->Release();
    }
    filter->Release();
  }
  filters->Release();
  return linked;
}

HRESULT find_pin_named(IBaseFilter *filter, const wchar_t *name, IPin **out) {
  *out = nullptr;
  if (!filter || !name)
    return E_POINTER;
  if (SUCCEEDED(filter->FindPin(name, out)) && *out)
    return S_OK;
  if (*out) {
    (*out)->Release();
    *out = nullptr;
  }
  return get_pin(filter, PINDIR_OUTPUT, out);
}

void release_graph() {
  g_audio_streams = 0;
  g_playing = false;
  g_frame_ready = false;
  if (g_control) {
    g_control->Stop();
  }
  if (g_event)
    g_event->SetNotifyWindow(0, 0, 0);
  if (g_vwin)
    g_vwin->put_Visible(OAFALSE);
  auto rel = [](auto *&p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  };
  rel(g_grabber);
  rel(g_seeking);
  rel(g_event);
  rel(g_control);
  rel(g_graph);
  rel(g_vwin);
  rel(g_bvideo);
  g_non_exclusive = false;
  g_w = g_h = 0;
  g_path[0] = '\0';
  g_scratch.clear();
  g_bgra.clear();
}

bool ensure_com() {
  if (g_com && g_ds_ready)
    return true;
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
    g_com = true;
    g_ds_ready = true;
    return true;
  }
  g_ds_ready = false;
  return false;
}

HRESULT build_graph_texture(const wchar_t *wpath) {
  IBaseFilter *grab_f = nullptr;
  HRESULT hr =
      CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IBaseFilter, reinterpret_cast<void **>(&grab_f));
  if (FAILED(hr) || !grab_f)
    return E_FAIL;
  hr = g_graph->AddFilter(grab_f, L"TEXTURERENDERER");
  if (FAILED(hr)) {
    grab_f->Release();
    return hr;
  }
  grab_f->QueryInterface(kIidISampleGrabber,
                         reinterpret_cast<void **>(&g_grabber));
  if (!g_grabber) {
    grab_f->Release();
    return E_FAIL;
  }
  AM_MEDIA_TYPE mt{};
  mt.majortype = MEDIATYPE_Video;
  mt.subtype = MEDIASUBTYPE_RGB32;
  mt.formattype = FORMAT_VideoInfo;
  g_grabber->SetMediaType(&mt);
  g_grabber->SetBufferSamples(TRUE);
  g_grabber->SetOneShot(FALSE);

  IBaseFilter *null_f = nullptr;
  hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, reinterpret_cast<void **>(&null_f));
  if (FAILED(hr) || !null_f) {
    grab_f->Release();
    return E_FAIL;
  }
  hr = g_graph->AddFilter(null_f, L"Null");
  if (FAILED(hr)) {
    null_f->Release();
    grab_f->Release();
    return hr;
  }

  IBaseFilter *src = nullptr;
  hr = g_graph->AddSourceFilter(wpath, L"SOURCE", &src);
  if (FAILED(hr) || !src) {
    null_f->Release();
    grab_f->Release();
    return E_FAIL;
  }
  IPin *src_out = nullptr;
  hr = find_pin_named(src, L"Output", &src_out);
  if (FAILED(hr) || !src_out) {
    src->Release();
    null_f->Release();
    grab_f->Release();
    return E_FAIL;
  }
  hr = g_graph->Render(src_out);
  bool grab_linked = false;
  if (SUCCEEDED(hr) && g_grabber) {
    AM_MEDIA_TYPE probe{};
    if (SUCCEEDED(g_grabber->GetConnectedMediaType(&probe))) {
      grab_linked = true;
      if (probe.pbFormat)
        CoTaskMemFree(probe.pbFormat);
      if (probe.pUnk)
        probe.pUnk->Release();
    }
  }
  if (!grab_linked) {
    IPin *peer = nullptr;
    if (SUCCEEDED(src_out->ConnectedTo(&peer)) && peer) {
      g_graph->Disconnect(src_out);
      g_graph->Disconnect(peer);
      peer->Release();
    }
    IPin *grab_in = nullptr;
    IPin *grab_out = nullptr;
    IPin *null_in = nullptr;
    get_pin(grab_f, PINDIR_INPUT, &grab_in);
    get_pin(grab_f, PINDIR_OUTPUT, &grab_out);
    get_pin(null_f, PINDIR_INPUT, &null_in);
    if (grab_in && grab_out && null_in &&
        SUCCEEDED(g_graph->Connect(src_out, grab_in)) &&
        SUCCEEDED(g_graph->Connect(grab_out, null_in))) {
      hr = S_OK;
      grab_linked = true;
    } else {
      hr = E_FAIL;
    }
    if (grab_in)
      grab_in->Release();
    if (grab_out)
      grab_out->Release();
    if (null_in)
      null_in->Release();
  }
  src_out->Release();
  null_f->Release();
  grab_f->Release();
  src->Release();
  if (FAILED(hr) || !grab_linked)
    return E_FAIL;

  AM_MEDIA_TYPE connected{};
  if (SUCCEEDED(g_grabber->GetConnectedMediaType(&connected))) {
    if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat) {
      auto *vih = reinterpret_cast<VIDEOINFOHEADER *>(connected.pbFormat);
      g_w = vih->bmiHeader.biWidth;
      g_h = vih->bmiHeader.biHeight;
      if (g_h < 0)
        g_h = -g_h;
    }
    if (connected.pbFormat)
      CoTaskMemFree(connected.pbFormat);
    if (connected.pUnk)
      connected.pUnk->Release();
  }
  const int audio = render_audio_pins();
  g_audio_streams = audio;
  std::printf("[fmv] audio render pins=%d\n", audio);
  return S_OK;
}

HRESULT build_graph_exclusive(const wchar_t *wpath) {
  return FAILED(g_graph->RenderFile(wpath, nullptr)) ? E_FAIL : S_OK;
}

bool qi_open_interfaces(bool require_video_ifaces) {
  if (FAILED(g_graph->QueryInterface(IID_IMediaControl,
                                     reinterpret_cast<void **>(&g_control))) ||
      !g_control)
    return false;
  if (FAILED(g_graph->QueryInterface(IID_IMediaSeeking,
                                     reinterpret_cast<void **>(&g_seeking))) ||
      !g_seeking)
    return false;
  if (FAILED(g_graph->QueryInterface(IID_IMediaEventEx,
                                     reinterpret_cast<void **>(&g_event))) ||
      !g_event)
    return false;
  g_graph->QueryInterface(IID_IVideoWindow, reinterpret_cast<void **>(&g_vwin));
  g_graph->QueryInterface(IID_IBasicVideo,
                          reinterpret_cast<void **>(&g_bvideo));
  if (require_video_ifaces && (!g_vwin || !g_bvideo))
    return false;
  return true;
}

HRESULT setup_exclusive_hwnd() {
  if (!g_vwin || !g_hwnd)
    return S_OK;
  HRESULT hr = g_vwin->put_Owner(reinterpret_cast<OAHWND>(g_hwnd));
  if (FAILED(hr))
    return hr;
  hr = g_vwin->put_WindowStyle(kFmvExclusiveWindowStyle);
  if (FAILED(hr))
    return hr;
  if (g_bvideo) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    hr = g_vwin->SetWindowPosition(rc.left, rc.top, rc.right, rc.bottom);
    long vw = 0, vh = 0;
    if (SUCCEEDED(g_bvideo->GetVideoSize(&vw, &vh)) && vw > 0 && vh > 0) {
      g_w = static_cast<int>(vw);
      g_h = static_cast<int>(vh);
    }
  }
  g_vwin->put_Visible(OATRUE);
  return hr;
}

void handle_events() {
  if (!g_event)
    return;
  long code = 0;
  LONG_PTR p1 = 0, p2 = 0;
  while (g_event->GetEvent(&code, &p1, &p2, 0) == S_OK) {
    if (code == EC_COMPLETE) {
      if (g_loop && g_seeking && g_control) {
        LONGLONG zero = 0;
        g_seeking->SetPositions(&zero, AM_SEEKING_AbsolutePositioning, nullptr,
                                AM_SEEKING_NoPositioning);
        g_control->Run();
      } else {
        g_playing = false;
      }
    }
    g_event->FreeEventParams(code, p1, p2);
  }
}

void grab_frame() {
  g_frame_ready = false;
  if (!g_grabber || g_w <= 0 || g_h <= 0)
    return;
  long sz = 0;
  if (FAILED(g_grabber->GetCurrentBuffer(&sz, nullptr)) || sz <= 0)
    return;
  if (static_cast<long>(g_scratch.size()) < sz)
    g_scratch.resize(static_cast<size_t>(sz));
  if (FAILED(g_grabber->GetCurrentBuffer(
          &sz, reinterpret_cast<long *>(g_scratch.data()))))
    return;
  const size_t need = static_cast<size_t>(g_w) * static_cast<size_t>(g_h) * 4u;
  if (g_bgra.size() < need)
    g_bgra.resize(need);
  // DS RGB32 is often bottom-up; flip to top-left for bgfx.
  const int pitch = g_w * 4;
  for (int y = 0; y < g_h; ++y) {
    const uint8_t *src =
        g_scratch.data() + static_cast<size_t>(g_h - 1 - y) * pitch;
    uint8_t *dst = g_bgra.data() + static_cast<size_t>(y) * pitch;
    std::memcpy(dst, src, static_cast<size_t>(pitch));
  }
  g_frame_ready = true;
}
#endif

} // namespace

void video_fmv_set_game_root(const std::string &root) { g_root = root; }

void video_fmv_set_hwnd(void *hwnd) {
#ifdef _WIN32
  g_hwnd = static_cast<HWND>(hwnd);
#else
  (void)hwnd;
#endif
}

#ifdef _WIN32
void avi_release() {
  if (g_avi_get) {
    AVIStreamGetFrameClose(g_avi_get);
    g_avi_get = nullptr;
  }
  if (g_avi_stream) {
    AVIStreamRelease(g_avi_stream);
    g_avi_stream = nullptr;
  }
  g_use_avi = false;
}

bool avi_copy_frame(LONG frame) {
  if (!g_avi_get)
    return false;
  void *p = AVIStreamGetFrame(g_avi_get, frame);
  if (!p)
    return false;
  auto *bih = static_cast<BITMAPINFOHEADER *>(p);
  const int w = bih->biWidth;
  const int h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return false;
  const int bit = bih->biBitCount;
  if (bit != 24 && bit != 32)
    return false;
  const bool bottom_up = bih->biHeight > 0;
  size_t off = bih->biSize;
  if (bit <= 8 || bih->biClrUsed != 0) {
    const DWORD colors = bih->biClrUsed != 0 ? bih->biClrUsed : (1u << bit);
    off += static_cast<size_t>(colors) * 4u;
  }
  const uint8_t *pixels = reinterpret_cast<const uint8_t *>(p) + off;
  const int bpp = bit / 8;
  const int src_stride = (w * bpp + 3) & ~3;
  g_w = w;
  g_h = h;
  g_bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
  for (int y = 0; y < h; ++y) {
    const int sy = bottom_up ? (h - 1 - y) : y;
    const uint8_t *src = pixels + static_cast<size_t>(sy) * src_stride;
    uint8_t *dst =
        g_bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
    if (bit == 32) {
      for (int x = 0; x < w; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 0];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 2];
        dst[x * 4 + 3] = 255;
      }
    } else {
      for (int x = 0; x < w; ++x) {
        dst[x * 4 + 0] = src[x * 3 + 0];
        dst[x * 4 + 1] = src[x * 3 + 1];
        dst[x * 4 + 2] = src[x * 3 + 2];
        dst[x * 4 + 3] = 255;
      }
    }
  }
  g_frame_ready = true;
  return true;
}

bool avi_try_open(const wchar_t *path) {
  avi_release();
  static bool inited = false;
  if (!inited) {
    AVIFileInit();
    inited = true;
  }
  if (AVIStreamOpenFromFileW(&g_avi_stream, path, streamtypeVIDEO, 0, OF_READ,
                             nullptr) != 0) {
    g_avi_stream = nullptr;
    return false;
  }
  AVISTREAMINFOW info{};
  if (AVIStreamInfoW(g_avi_stream, &info, sizeof(info)) != 0) {
    avi_release();
    return false;
  }
  g_avi_start = static_cast<LONG>(info.dwStart);
  g_avi_end = g_avi_start + static_cast<LONG>(info.dwLength);
  if (g_avi_end <= g_avi_start) {
    avi_release();
    return false;
  }
  if (info.dwRate > 0) {
    g_avi_ms = static_cast<DWORD>((1000.0 * static_cast<double>(info.dwScale)) /
                                  static_cast<double>(info.dwRate));
  }
  if (g_avi_ms < 1 || g_avi_ms > 1000)
    g_avi_ms = 33;

  BITMAPINFOHEADER want{};
  want.biSize = sizeof(want);
  want.biWidth = info.rcFrame.right - info.rcFrame.left;
  want.biHeight = info.rcFrame.bottom - info.rcFrame.top;
  want.biPlanes = 1;
  want.biBitCount = 24;
  want.biCompression = BI_RGB;
  g_avi_get = AVIStreamGetFrameOpen(g_avi_stream, &want);
  if (!g_avi_get) {
    g_avi_get = AVIStreamGetFrameOpen(
        g_avi_stream,
        reinterpret_cast<LPBITMAPINFOHEADER>(AVIGETFRAMEF_BESTDISPLAYFMT));
  }
  if (!g_avi_get) {
    avi_release();
    return false;
  }
  g_avi_frame = g_avi_start;
  if (!avi_copy_frame(g_avi_frame)) {
    avi_release();
    return false;
  }
  g_use_avi = true;
  g_avi_next = GetTickCount() + g_avi_ms;
  g_playing = true;
  g_non_exclusive = true;
  return true;
}

void avi_pump() {
  if (!g_use_avi || !g_playing || !g_avi_get)
    return;
  const DWORD now = GetTickCount();
  int guard = 0;
  while (static_cast<int32_t>(now - g_avi_next) >= 0 && guard++ < 8) {
    ++g_avi_frame;
    if (g_avi_frame >= g_avi_end) {
      if (g_loop) {
        g_avi_frame = g_avi_start;
        if (g_seeking) {
          LONGLONG zero = 0;
          g_seeking->SetPositions(&zero, AM_SEEKING_AbsolutePositioning,
                                  nullptr, AM_SEEKING_NoPositioning);
        }
      } else {
        g_playing = false;
        if (g_control)
          g_control->Stop();
        break;
      }
    }
    g_avi_next += g_avi_ms;
  }
  if (g_playing)
    avi_copy_frame(g_avi_frame);
}
#endif

int32_t video_fmv_open(const char *path, int32_t non_exclusive, int32_t loop) {
#ifdef _WIN32
  if (!path || !path[0])
    return -1;
  video_fmv_close();

  const std::string resolved = resolve_path(path);
  const DWORD attr = GetFileAttributesA(resolved.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    std::printf("[fmv] missing %s\n", resolved.c_str());
    return -1;
  }
  const std::wstring wpath = to_wide(resolved.c_str());
  if (wpath.empty())
    return -1;

#ifdef _WIN64
  if (vlc_fmv_open(wpath, loop != 0)) {
    g_use_vlc = true;
    g_non_exclusive = non_exclusive != 0;
    g_playing = true;
    std::printf("[fmv] LibVLC open %s loop=%d\n", resolved.c_str(), loop != 0);
    return 0;
  }
#endif

  if (!ensure_com())
    return -1;
  HRESULT hr =
      CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IGraphBuilder, reinterpret_cast<void **>(&g_graph));
  if (FAILED(hr) || !g_graph)
    return -1;

  g_non_exclusive = non_exclusive != 0;
  g_loop = loop != 0;
  std::snprintf(g_path, sizeof(g_path), "%s", resolved.c_str());
  // prime.avi is Cinepak (cvid). The Sample Grabber graph never inserts
  // iccvid, then soft-ok reports success with zero frames. AVIFile uses the
  // system codec and yields a real picture before the menu is drawn.
  if (non_exclusive) {
    g_non_exclusive = true;
    g_loop = loop != 0;
    if (avi_try_open(wpath.c_str())) {
      IBaseFilter *source = nullptr;
      IBaseFilter *splitter = nullptr;
      int audio = 0;
      if (SUCCEEDED(g_graph->AddSourceFilter(wpath.c_str(), L"AVI audio source",
                                             &source)) &&
          SUCCEEDED(CoCreateInstance(CLSID_AviSplitter, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                                     reinterpret_cast<void **>(&splitter)))) {
        if (SUCCEEDED(g_graph->AddFilter(splitter, L"AVI audio splitter"))) {
          IPin *output = nullptr;
          IPin *input = nullptr;
          get_pin(source, PINDIR_OUTPUT, &output);
          get_pin(splitter, PINDIR_INPUT, &input);
          if (output && input && SUCCEEDED(g_graph->Connect(output, input)))
            audio = render_audio_pins();
          if (output)
            output->Release();
          if (input)
            input->Release();
        }
      }
      if (source)
        source->Release();
      if (splitter)
        splitter->Release();
      HRESULT audio_hr = E_FAIL;
      if (audio > 0 && qi_open_interfaces(false))
        audio_hr = g_control->Run();
      g_audio_streams = SUCCEEDED(audio_hr) ? audio : 0;
      g_avi_next = GetTickCount() + g_avi_ms;
      std::printf("[fmv] AVI audio streams=%d running=%d hr=0x%lx\n", audio,
                  SUCCEEDED(audio_hr), static_cast<unsigned long>(audio_hr));
      std::snprintf(g_path, sizeof(g_path), "%s", resolved.c_str());
      std::printf("[fmv] avi %s loop=%d %dx%d\n", g_path, g_loop ? 1 : 0, g_w,
                  g_h);
      return 0;
    }
    std::printf("[fmv] avi fail %s — trying DirectShow\n", resolved.c_str());
  }

  hr = g_non_exclusive ? build_graph_texture(wpath.c_str())
                       : build_graph_exclusive(wpath.c_str());
  if (FAILED(hr) || !qi_open_interfaces(!g_non_exclusive)) {
    release_graph();
#ifndef _WIN64
    // Menu bg (non_exclusive): Soft Java treats openVideo==0 as success and
    // skips the Dummy/Vehicle fallback. Prefer that over a hard fail when the
    // file exists but DirectShow cannot build a texture graph yet.
    if (g_non_exclusive) {
      std::printf("[fmv] DS graph fail — soft-ok non_exclusive %s\n",
                  resolved.c_str());
      g_playing = true;
      g_loop = loop != 0;
      g_non_exclusive = true;
      std::snprintf(g_path, sizeof(g_path), "%s", resolved.c_str());
      return 0;
    }
#endif
    return -1;
  }
  if (!g_non_exclusive) {
    if (FAILED(setup_exclusive_hwnd())) {
      release_graph();
      return -1;
    }
  }
  if (g_event && g_hwnd)
    g_event->SetNotifyWindow(reinterpret_cast<OAHWND>(g_hwnd), kFmvNotifyMsg,
                             0);
  IBasicAudio *basic_audio = nullptr;
  if (SUCCEEDED(g_graph->QueryInterface(
          IID_IBasicAudio, reinterpret_cast<void **>(&basic_audio))) &&
      basic_audio) {
    // 0 = 0 dB (full). -10000 is silence.
    basic_audio->put_Volume(0);
    basic_audio->Release();
  }
  hr = g_control->Run();
  g_playing = SUCCEEDED(hr);
  std::printf("[fmv] open %s non_excl=%d loop=%d playing=%d %dx%d\n", g_path,
              g_non_exclusive ? 1 : 0, g_loop ? 1 : 0, g_playing ? 1 : 0, g_w,
              g_h);
  return 0;
#else
  (void)path;
  (void)non_exclusive;
  (void)loop;
  return -1;
#endif
}

void video_fmv_close() {
  g_audio_streams = 0;
#ifdef _WIN32
#ifdef _WIN64
  if (g_use_vlc) {
    vlc_fmv_close();
    g_use_vlc = false;
  }
#endif
  avi_release();
  release_graph();
#endif
}

int32_t video_fmv_audio_stream_count() {
#ifdef _WIN64
  if (g_use_vlc) return vlc_fmv_audio_tracks();
#endif
  return g_audio_streams;
}
bool video_fmv_audio_running() {
#ifdef _WIN32
#ifdef _WIN64
  if (g_use_vlc) return vlc_fmv_audio_running();
#endif
  OAFilterState state = State_Stopped;
  return g_audio_streams > 0 && g_control &&
         SUCCEEDED(g_control->GetState(500, &state)) && state == State_Running;
#else
  return false;
#endif
}
int32_t video_fmv_is_playing() {
#ifdef _WIN32
  return g_playing ? 1 : 0;
#else
  return 0;
#endif
}

bool g_block_osd = false;

void video_fmv_set_block_osd(bool block) { g_block_osd = block; }

bool video_fmv_block_osd() {
#ifdef _WIN64
  if (g_use_vlc && g_playing && !g_non_exclusive) return true;
#endif
  return g_block_osd;
}

int32_t video_fmv_is_non_exclusive() {
#ifdef _WIN32
  return g_non_exclusive ? 1 : 0;
#else
  return 0;
#endif
}

void video_fmv_pump() {
#ifdef _WIN32
  if (!g_playing)
    return;
#ifdef _WIN64
  if (g_use_vlc) {
    const auto frame = vlc_fmv_pump(g_bgra, g_w, g_h);
    if (frame == VlcFrame::Stopped) {
      video_fmv_close();
    } else if (frame == VlcFrame::Ready) {
      if (!g_frame_ready)
        std::printf("[fmv] LibVLC frame %dx%d audio=%d\n",
                    g_w, g_h, vlc_fmv_audio_tracks());
      g_frame_ready = true;
      ++g_vlc_frame_serial;
    }
    return;
  }
#endif
  if (g_use_avi) {
    avi_pump();
    return;
  }
  handle_events();
  if (g_non_exclusive)
    grab_frame();
#endif
}

int32_t video_fmv_width() {
#ifdef _WIN32
  return g_w;
#else
  return 0;
#endif
}

int32_t video_fmv_height() {
#ifdef _WIN32
  return g_h;
#else
  return 0;
#endif
}

const uint8_t *video_fmv_bgra() {
#ifdef _WIN32
  return g_frame_ready && !g_bgra.empty() ? g_bgra.data() : nullptr;
#else
  return nullptr;
#endif
}

bool video_fmv_frame_ready() {
#ifdef _WIN32
  return g_frame_ready;
#else
  return false;
#endif
}

uint64_t video_fmv_frame_serial() {
#ifdef _WIN64
  if (g_use_vlc) return g_vlc_frame_serial;
#endif
  return 0;
}

void video_fmv_letterbox(int32_t screen_w, int32_t screen_h, float *out_left,
                         float *out_top, float *out_w, float *out_h) {
  if (!out_left || !out_top || !out_w || !out_h)
    return;
  *out_left = 0.f;
  *out_top = 0.f;
  *out_w = static_cast<float>(screen_w);
  *out_h = static_cast<float>(screen_h);
#ifdef _WIN32
  if (screen_w <= 0 || screen_h <= 0 || g_w <= 0 || g_h <= 0)
    return;
  const float sw = static_cast<float>(screen_w);
  const float sh = static_cast<float>(screen_h);
  const float vw = static_cast<float>(g_w);
  const float vh = static_cast<float>(g_h);
  const float va = vw / vh;
  const float sa = sw / sh;
  if (va <= sa) {
    *out_w = sh * va;
    *out_left = (sw - *out_w) * 0.5f;
    *out_h = sh;
    *out_top = 0.f;
  } else {
    *out_h = sw / va;
    *out_top = (sh - *out_h) * 0.5f;
    *out_w = sw;
    *out_left = 0.f;
  }
#else
  (void)screen_w;
  (void)screen_h;
#endif
}

int32_t video_fmv_play_boot_intros(int32_t max_frames_each,
                                   const std::function<void()> &pump) {
  static const char *kPaths[] = {
      "Data\\FMV\\Activision.avi",
      "Data\\FMV\\Invictus.avi",
      "Data\\FMV\\StreetLegal.avi",
  };
  int32_t played = 0;
#ifdef _WIN32
  auto esc = []() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; };
  if (g_hwnd) {
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
  }
  // ESC skips one intro at a time (edge), then must release before the next.
  auto wait_esc_up = [&]() {
    while (esc()) {
      if (pump)
        pump();
      Sleep(1);
    }
  };
  wait_esc_up();
  video_fmv_set_block_osd(true);
  for (const char *path : kPaths) {
    // bgfx always Presents — exclusive HWND is covered. Use SampleGrabber
    // path (non_exclusive=1) so boot intros blit through draw_fmv.
    if (video_fmv_open(path, /*non_exclusive=*/1, /*loop=*/0) != 0)
      continue;
    ++played;
    int32_t frames = 0;
    bool skipped = false;
    while (video_fmv_is_playing()) {
      if (pump)
        pump();
      video_fmv_pump();
      if (esc()) {
        std::printf("[fmv] skip ESC %s\n", path);
        skipped = true;
        break;
      }
      ++frames;
      if (max_frames_each > 0 && frames >= max_frames_each)
        break;
      Sleep(1);
    }
    video_fmv_close();
    if (skipped)
      wait_esc_up();
  }
  video_fmv_set_block_osd(false);
  wait_esc_up();
#else
  (void)max_frames_each;
  (void)pump;
#endif
  std::printf("[fmv] boot intros played=%d\n", played);
  return played;
}

} // namespace slrr
