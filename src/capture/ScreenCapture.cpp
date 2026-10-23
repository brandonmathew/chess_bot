#include "capture/ScreenCapture.h"

#include <windows.h>

#include <algorithm>

namespace cm {

namespace {

// Grab a rectangle of the screen with GDI and return it as a BGR image.
Image grabRect(int left, int top, int w, int h) {
    Image img;
    if (w <= 0 || h <= 0) return img;

    HDC screen = GetDC(nullptr);
    if (!screen) return img;

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ previous = SelectObject(mem, bmp);
    const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, left, top, SRCCOPY);

    // The bitmap must not be selected into a DC while GetDIBits runs.
    SelectObject(mem, previous);

    if (ok) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4, 0);
        if (GetDIBits(screen, bmp, 0, static_cast<UINT>(h), buf.data(), &bmi, DIB_RGB_COLORS)) {
            img.resize(w, h);
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* src = buf.data() + static_cast<std::size_t>(y) * w * 4;
                std::uint8_t* dst = img.data.data() + static_cast<std::size_t>(y) * w * 3;
                for (int x = 0; x < w; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];  // B
                    dst[x * 3 + 1] = src[x * 4 + 1];  // G
                    dst[x * 3 + 2] = src[x * 4 + 2];  // R
                }
            }
        }
    }

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

struct EnumCtx {
    std::vector<MonitorInfo>* out;
    int nextIndex;
};

BOOL CALLBACK monitorEnum(HMONITOR, HDC, LPRECT rect, LPARAM data) {
    auto* ctx = reinterpret_cast<EnumCtx*>(data);
    MonitorInfo info;
    info.index = ctx->nextIndex++;
    info.rect = RectI{rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top};
    info.primary = (rect->left == 0 && rect->top == 0);
    ctx->out->push_back(info);
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> ScreenCapture::monitors() const {
    std::vector<MonitorInfo> result;
    EnumCtx ctx{&result, 1};
    EnumDisplayMonitors(nullptr, nullptr, monitorEnum, reinterpret_cast<LPARAM>(&ctx));

    // Primary monitor first (matches mss, which lists the primary after the
    // virtual-desktop entry).
    std::stable_sort(result.begin(), result.end(),
                     [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary && !b.primary; });

    // Re-index after sorting so grabMonitor() stays consistent.
    for (std::size_t i = 0; i < result.size(); ++i) result[i].index = static_cast<int>(i) + 1;
    return result;
}

Image ScreenCapture::grabVirtualDesktop() const {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return grabRect(left, top, w, h);
}

Image ScreenCapture::grabMonitor(int monitorIndex) const {
    if (monitorIndex <= 0) return grabVirtualDesktop();
    const auto list = monitors();
    if (static_cast<std::size_t>(monitorIndex) > list.size()) return Image();
    const RectI r = list[static_cast<std::size_t>(monitorIndex - 1)].rect;
    return grabRect(r.x, r.y, r.w, r.h);
}

Image ScreenCapture::grabRegion(RectI region) const {
    return grabRect(region.x, region.y, region.w, region.h);
}

}  // namespace cm
