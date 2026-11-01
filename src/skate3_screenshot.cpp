#include "skate3_screenshot.h"

#include <rex/logging.h>

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

// gdiplus.h uses unqualified min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace skate3::screenshot {
namespace {

bool EnsureGdiplus() {
  static bool initialized = [] {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }();
  return initialized;
}

bool PngEncoderClsid(CLSID* clsid) {
  UINT count = 0;
  UINT bytes = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) {
    return false;
  }
  std::string storage(bytes, '\0');
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
  if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) {
    return false;
  }
  for (UINT i = 0; i < count; ++i) {
    if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
      *clsid = codecs[i].Clsid;
      return true;
    }
  }
  return false;
}

// Last timestamp handed out, in ms since the epoch. Strictly increasing, so
// two captures in the same millisecond (or after a clock step back) still
// get distinct names, in capture order. After such a burst or step back the
// stamp leads the clock until the clock catches up.
std::atomic<int64_t> g_last_shot_ms{0};

int64_t NextShotMs() {
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  int64_t last = g_last_shot_ms.load(std::memory_order_relaxed);
  int64_t next;
  do {
    next = std::max(now_ms, last + 1);
  } while (!g_last_shot_ms.compare_exchange_weak(last, next, std::memory_order_relaxed));
  return next;
}

// shot_YYYYMMDD_HHMMSS_mmm[_tag].png: the millisecond field keeps names
// unique within a second and, being fixed width, keeps name order equal to
// capture order.
std::filesystem::path ScreenshotPath(const char* tag) {
  const int64_t ms = NextShotMs();
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  const int millis = static_cast<int>(ms % 1000);
  std::tm tm{};
  localtime_s(&tm, &t);
  char name[96];
  if (tag != nullptr && tag[0] != '\0') {
    std::snprintf(name, sizeof(name), "shot_%04d%02d%02d_%02d%02d%02d_%03d_%s.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, millis, tag);
  } else {
    std::snprintf(name, sizeof(name), "shot_%04d%02d%02d_%02d%02d%02d_%03d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, millis);
  }
  return std::filesystem::absolute(std::filesystem::path("screenshots") / name);
}

void EncodeAndSave(HBITMAP bitmap, int crop_x, int crop_y, int crop_w, int crop_h,
                   std::filesystem::path path) {
  if (!EnsureGdiplus()) {
    REXLOG_ERROR("screenshot: GDI+ startup failed");
    DeleteObject(bitmap);
    return;
  }
  CLSID png;
  if (!PngEncoderClsid(&png)) {
    REXLOG_ERROR("screenshot: no PNG encoder");
    DeleteObject(bitmap);
    return;
  }
  Gdiplus::Bitmap full(bitmap, nullptr);
  std::unique_ptr<Gdiplus::Bitmap> crop(
      full.Clone(crop_x, crop_y, crop_w, crop_h, PixelFormat32bppRGB));
  DeleteObject(bitmap);
  if (!crop || crop->GetLastStatus() != Gdiplus::Ok) {
    REXLOG_ERROR("screenshot: client-area crop failed");
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (crop->Save(path.wstring().c_str(), &png, nullptr) != Gdiplus::Ok) {
    REXLOG_ERROR("screenshot: PNG save failed for {}", path.string());
    return;
  }
  REXLOG_INFO("screenshot: saved {} ({}x{})", path.string(), crop_w, crop_h);
}

std::atomic<void*> g_remembered_window{nullptr};

}  // namespace

void RememberWindow(void* hwnd) {
  g_remembered_window.store(hwnd, std::memory_order_relaxed);
}

void* RememberedWindow() { return g_remembered_window.load(std::memory_order_relaxed); }

void CaptureWindow(void* window_handle, const char* tag) {
  HWND hwnd = static_cast<HWND>(window_handle);
  if (hwnd == nullptr) {
    REXLOG_WARN("screenshot: no window handle");
    return;
  }
  RECT wr{};
  RECT cr{};
  if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) {
    REXLOG_WARN("screenshot: window rect query failed");
    return;
  }
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;
  const int crop_w = cr.right - cr.left;
  const int crop_h = cr.bottom - cr.top;
  if (win_w <= 0 || win_h <= 0 || crop_w <= 0 || crop_h <= 0) {
    REXLOG_WARN("screenshot: window is zero-sized (minimized?)");
    return;
  }
  POINT client_origin{0, 0};
  ClientToScreen(hwnd, &client_origin);

  HDC screen_dc = GetDC(nullptr);
  HDC mem_dc = CreateCompatibleDC(screen_dc);
  HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, win_w, win_h);
  HGDIOBJ old = SelectObject(mem_dc, bitmap);
  const BOOL printed = PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT);
  SelectObject(mem_dc, old);
  DeleteDC(mem_dc);
  ReleaseDC(nullptr, screen_dc);
  if (!printed) {
    DeleteObject(bitmap);
    REXLOG_WARN("screenshot: PrintWindow failed");
    return;
  }

  std::thread(EncodeAndSave, bitmap, int(client_origin.x - wr.left),
              int(client_origin.y - wr.top), crop_w, crop_h, ScreenshotPath(tag))
      .detach();
}

}  // namespace skate3::screenshot

#else  // !_WIN32

namespace skate3::screenshot {
void CaptureWindow(void* /*window_handle*/, const char* /*tag*/) {
  REXLOG_WARN("screenshot: only implemented on Windows");
}
void RememberWindow(void* /*hwnd*/) {}
void* RememberedWindow() { return nullptr; }
}  // namespace skate3::screenshot

#endif  // _WIN32
