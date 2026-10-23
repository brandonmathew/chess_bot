#include "overlay/OverlayWindow.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.h"

namespace cm {

namespace {
constexpr const char* kClassName = "ChessMongerOverlayWindow";
bool g_classRegistered = false;
}  // namespace

OverlayWindow::~OverlayWindow() { destroy(); }

bool OverlayWindow::create() {
    if (hwnd_) return true;
    HINSTANCE instance = GetModuleHandleA(nullptr);

    if (!g_classRegistered) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWindow::wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            log::print("[Overlay] RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
        g_classRegistered = true;
    }

    // The window spans the whole virtual desktop from the start; content is
    // composited with UpdateLayeredWindow (per-pixel alpha), never WM_PAINT.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwnd_ = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            kClassName, "ChessMonger Overlay", WS_POPUP, vx, vy, std::max(1, vw),
                            std::max(1, vh), nullptr, nullptr, instance, nullptr);
    if (!hwnd_) {
        log::print("[Overlay] CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }

    // Exclude the overlay from screenshots, recordings and screen shares:
    // it stays visible on the physical display but is absent from every
    // capture (which also keeps the suggestion arrow out of the bot's own
    // board captures). Requires Windows 10 2004+; log and continue if the
    // OS refuses.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        log::print("[Overlay] SetWindowDisplayAffinity failed (%lu) - overlay will appear in captures",
                   GetLastError());
    }
    return true;
}

void OverlayWindow::destroy() {
    shown_ = false;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (memDc_) {
        if (oldBitmap_) SelectObject(memDc_, oldBitmap_);
        oldBitmap_ = nullptr;
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    pixels_ = nullptr;
    w_ = h_ = 0;
}

bool OverlayWindow::ensureSurface() {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (dib_ && vx == originX_ && vy == originY_ && vw == w_ && vh == h_) return true;

    if (memDc_) {
        if (oldBitmap_) SelectObject(memDc_, oldBitmap_);
        oldBitmap_ = nullptr;
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    pixels_ = nullptr;

    memDc_ = CreateCompatibleDC(nullptr);
    if (!memDc_) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;  // top-down: memory row 0 is the top row
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dib_ = CreateDIBSection(memDc_, &bmi, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (!dib_ || !pixels_) {
        log::print("[Overlay] CreateDIBSection failed (%lu)", GetLastError());
        DeleteDC(memDc_);
        memDc_ = nullptr;
        dib_ = nullptr;
        return false;
    }
    oldBitmap_ = SelectObject(memDc_, dib_);
    originX_ = vx;
    originY_ = vy;
    w_ = vw;
    h_ = vh;
    return true;
}

void OverlayWindow::show(RectI bounds) {
    if (!hwnd_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.roiW = std::max(1, bounds.w);
        shown_ = true;
        if (!ensureSurface()) {
            shown_ = false;
            return;
        }
        // Position in the topmost band over the whole desktop, then paint.
        SetWindowPos(hwnd_, HWND_TOPMOST, originX_, originY_, w_, h_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        renderLocked();
    }
    log::print("[Overlay] Desktop overlay shown (window %d,%d %dx%d; board ROI %d,%d %dx%d)", originX_,
               originY_, w_, h_, bounds.x, bounds.y, bounds.w, bounds.h);
}

void OverlayWindow::hide() {
    shown_ = false;
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::setArrow(int fromX, int fromY, int toX, int toY, int rgb, int width,
                             const std::string& evalText) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.hasArrow = true;
    state_.fromX = fromX;
    state_.fromY = fromY;
    state_.toX = toX;
    state_.toY = toY;
    state_.rgb = rgb;
    state_.width = std::clamp(width, 2, 24);
    state_.evalText = evalText;
    if (shown_ && hwnd_) {
        // Re-assert topmost on every suggestion: apps that grabbed the
        // topmost band after us must not bury the overlay.
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        renderLocked();
    }
}

void OverlayWindow::clearArrow() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.hasArrow = false;
    state_.evalText.clear();
    if (shown_ && hwnd_) renderLocked();
}

void OverlayWindow::renderLocked() {
    if (!hwnd_ || !shown_ || !ensureSurface()) return;

    // Fully transparent base (BGRA all-zero).
    std::memset(pixels_, 0, static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4);

    const State& s = state_;
    if (s.hasArrow) {
        const COLORREF color = RGB(s.rgb & 0xFF, (s.rgb >> 8) & 0xFF, (s.rgb >> 16) & 0xFF);

        // Screen -> overlay coordinates (window origin is the desktop origin).
        const int x1 = s.fromX - originX_;
        const int y1 = s.fromY - originY_;
        const int x2 = s.toX - originX_;
        const int y2 = s.toY - originY_;

        const int sqW = std::max(1, s.roiW) / 8;
        const int radius = static_cast<int>(sqW * 0.35);

        HGDIOBJ oldBrush = SelectObject(memDc_, GetStockObject(NULL_BRUSH));

        // Source square: outline circle.
        HPEN sourcePen = CreatePen(PS_SOLID, 3, color);
        HGDIOBJ oldPen = SelectObject(memDc_, sourcePen);
        Ellipse(memDc_, x1 - radius, y1 - radius, x1 + radius, y1 + radius);

        // Destination square: light fill + outline.
        HBRUSH destBrush = CreateSolidBrush(RGB((GetRValue(color) + 255) / 2, (GetGValue(color) + 255) / 2,
                                                (GetBValue(color) + 255) / 2));
        HPEN destPen = CreatePen(PS_SOLID, 2, color);
        SelectObject(memDc_, destPen);
        HGDIOBJ prevDestBrush = SelectObject(memDc_, destBrush);
        Ellipse(memDc_, x2 - radius, y2 - radius, x2 + radius, y2 + radius);
        SelectObject(memDc_, prevDestBrush);

        // Arrow shaft + head.
        HPEN arrowPen = CreatePen(PS_SOLID | PS_ENDCAP_ROUND, s.width, color);
        SelectObject(memDc_, arrowPen);

        const double dx = static_cast<double>(x2 - x1);
        const double dy = static_cast<double>(y2 - y1);
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-3) {
            const double ux = dx / len, uy = dy / len;
            const double headLen = 16.0;
            const double halfW = 10.0;
            const double tipX = x2 - ux * (radius * 0.6);
            const double tipY = y2 - uy * (radius * 0.6);
            const double baseX = tipX - ux * headLen;
            const double baseY = tipY - uy * headLen;

            MoveToEx(memDc_, static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)), nullptr);
            LineTo(memDc_, static_cast<int>(std::lround(baseX)), static_cast<int>(std::lround(baseY)));

            const double px = -uy, py = ux;  // perpendicular
            POINT head[3] = {
                {static_cast<LONG>(std::lround(tipX)), static_cast<LONG>(std::lround(tipY))},
                {static_cast<LONG>(std::lround(baseX + px * halfW)), static_cast<LONG>(std::lround(baseY + py * halfW))},
                {static_cast<LONG>(std::lround(baseX - px * halfW)), static_cast<LONG>(std::lround(baseY - py * halfW))},
            };
            HBRUSH headBrush = CreateSolidBrush(color);
            HGDIOBJ prevHeadBrush = SelectObject(memDc_, headBrush);
            Polygon(memDc_, head, 3);
            SelectObject(memDc_, prevHeadBrush);
            DeleteObject(headBrush);
        }

        // Evaluation badge.
        if (!s.evalText.empty()) {
            const int badgeX = std::clamp(x2, 40, std::max(40, w_ - 40));
            const int badgeY = std::clamp(y2 - static_cast<int>(sqW * 0.45), 20, std::max(20, h_ - 20));
            RECT badge{badgeX - 30, badgeY - 12, badgeX + 30, badgeY + 12};
            HBRUSH badgeBrush = CreateSolidBrush(RGB(0x1E, 0x1E, 0x1E));
            FillRect(memDc_, &badge, badgeBrush);
            DeleteObject(badgeBrush);
            HPEN badgePen = CreatePen(PS_SOLID, 2, color);
            HGDIOBJ prevBadgePen = SelectObject(memDc_, badgePen);
            HGDIOBJ prevBadgeBrush = SelectObject(memDc_, GetStockObject(NULL_BRUSH));
            Rectangle(memDc_, badge.left, badge.top, badge.right, badge.bottom);
            SelectObject(memDc_, prevBadgeBrush);
            SelectObject(memDc_, prevBadgePen);
            DeleteObject(badgePen);

            SetBkMode(memDc_, TRANSPARENT);
            SetTextColor(memDc_, RGB(255, 255, 255));
            HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, "Helvetica");
            HGDIOBJ oldFont = SelectObject(memDc_, font);
            SetTextAlign(memDc_, TA_CENTER | TA_BASELINE);
            TextOutA(memDc_, badgeX, badgeY, s.evalText.c_str(), static_cast<int>(s.evalText.size()));
            SetTextAlign(memDc_, TA_LEFT | TA_TOP);
            SelectObject(memDc_, oldFont);
            DeleteObject(font);
        }

        SelectObject(memDc_, oldPen);
        SelectObject(memDc_, oldBrush);
        DeleteObject(sourcePen);
        DeleteObject(destPen);
        DeleteObject(destBrush);
        DeleteObject(arrowPen);

        // GDI writes RGB but leaves the DIB alpha byte at 0. Everything that
        // was drawn becomes fully opaque; untouched pixels stay transparent.
        auto* px = static_cast<std::uint8_t*>(pixels_);
        const std::size_t count = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t* p = px + i * 4;
            if (p[0] | p[1] | p[2]) p[3] = 255;
        }
        GdiFlush();
    }

    POINT dst{originX_, originY_};
    SIZE size{w_, h_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDc_, &src, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // No WM_PAINT handling: content is composited by UpdateLayeredWindow.
    // WS_EX_TRANSPARENT + WS_EX_NOACTIVATE keep input flowing to the app
    // underneath; anything else goes to the default handler.
    if (msg == WM_DESTROY) return 0;
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

}  // namespace cm
