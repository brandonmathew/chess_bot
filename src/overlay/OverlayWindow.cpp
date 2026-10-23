#include "overlay/OverlayWindow.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

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
        if (!RegisterClassExA(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                log::print("[Overlay] RegisterClassEx failed (%lu)", GetLastError());
                return false;
            }
        }
        g_classRegistered = true;
    }

    hwnd_ = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE,
                            kClassName, "ChessMonger Overlay", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        log::print("[Overlay] CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }

    // Same mechanism as tkinter's -transparentcolor #abcdef: everything drawn
    // in the key colour is invisible, everything else is opaque.
    SetLayeredWindowAttributes(hwnd_, kTransparencyKey, 0, LWA_COLORKEY);
    return true;
}

void OverlayWindow::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OverlayWindow::show(RectI bounds) {
    if (!hwnd_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.originX = bounds.x;
        state_.originY = bounds.y;
        state_.roiW = std::max(1, bounds.w);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.x, bounds.y, std::max(1, bounds.w), std::max(1, bounds.h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void OverlayWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool OverlayWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void OverlayWindow::setArrow(int fromX, int fromY, int toX, int toY, int rgb, int width,
                             const std::string& evalText) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.hasArrow = true;
        state_.fromX = fromX;
        state_.fromY = fromY;
        state_.toX = toX;
        state_.toY = toY;
        state_.rgb = rgb;
        state_.width = std::clamp(width, 2, 24);
        state_.evalText = evalText;
    }
    if (hwnd_) {
        // Thread-safe: paint is dispatched on the owning (main) thread.
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void OverlayWindow::clearArrow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.hasArrow = false;
        state_.evalText.clear();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT:
            if (self) return self->handlePaint(hwnd);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT OverlayWindow::handlePaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    // Fill everything with the transparency key colour first.
    HBRUSH keyBrush = CreateSolidBrush(kTransparencyKey);
    FillRect(dc, &client, keyBrush);
    DeleteObject(keyBrush);

    State snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = state_;
    }

    if (snapshot.hasArrow && width > 0 && height > 0) {
        const COLORREF color = RGB(snapshot.rgb & 0xFF, (snapshot.rgb >> 8) & 0xFF, (snapshot.rgb >> 16) & 0xFF);

        // Screen -> overlay-window coordinates (the window covers the ROI).
        const int x1 = snapshot.fromX - snapshot.originX;
        const int y1 = snapshot.fromY - snapshot.originY;
        const int x2 = snapshot.toX - snapshot.originX;
        const int y2 = snapshot.toY - snapshot.originY;

        const int sqW = std::max(1, snapshot.roiW) / 8;
        const int radius = static_cast<int>(sqW * 0.35);

        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

        // Source square: outline circle (tkinter create_oval outline only).
        HPEN sourcePen = CreatePen(PS_SOLID, 3, color);
        HGDIOBJ oldPen = SelectObject(dc, sourcePen);
        Ellipse(dc, x1 - radius, y1 - radius, x1 + radius, y1 + radius);

        // Destination square: light fill + outline.
        HBRUSH destBrush = CreateSolidBrush(RGB((GetRValue(color) + 255) / 2, (GetGValue(color) + 255) / 2,
                                                (GetBValue(color) + 255) / 2));
        HPEN destPen = CreatePen(PS_SOLID, 2, color);
        SelectObject(dc, destPen);
        HGDIOBJ prevDestBrush = SelectObject(dc, destBrush);
        Ellipse(dc, x2 - radius, y2 - radius, x2 + radius, y2 + radius);
        SelectObject(dc, prevDestBrush);

        // Arrow shaft (tkinter arrowshape = (16, 20, 8): 16px long, 20px wide).
        HPEN arrowPen = CreatePen(PS_SOLID | PS_ENDCAP_ROUND, snapshot.width, color);
        SelectObject(dc, arrowPen);

        const double dx = static_cast<double>(x2 - x1);
        const double dy = static_cast<double>(y2 - y1);
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-3) {
            const double ux = dx / len, uy = dy / len;
            const double headLen = 16.0;
            const double halfW = 10.0;
            // Shaft stops where the arrow head begins.
            const double tipX = x2 - ux * (radius * 0.6);
            const double tipY = y2 - uy * (radius * 0.6);
            const double baseX = tipX - ux * headLen;
            const double baseY = tipY - uy * headLen;

            MoveToEx(dc, static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)), nullptr);
            LineTo(dc, static_cast<int>(std::lround(baseX)), static_cast<int>(std::lround(baseY)));

            // Arrow head triangle.
            const double px = -uy, py = ux;  // perpendicular
            POINT head[3] = {
                {static_cast<LONG>(std::lround(tipX)), static_cast<LONG>(std::lround(tipY))},
                {static_cast<LONG>(std::lround(baseX + px * halfW)), static_cast<LONG>(std::lround(baseY + py * halfW))},
                {static_cast<LONG>(std::lround(baseX - px * halfW)), static_cast<LONG>(std::lround(baseY - py * halfW))},
            };
            HBRUSH headBrush = CreateSolidBrush(color);
            HGDIOBJ prevHeadBrush = SelectObject(dc, headBrush);
            Polygon(dc, head, 3);
            SelectObject(dc, prevHeadBrush);
            DeleteObject(headBrush);
        }

        // Evaluation badge (mirrors the tkinter rectangle + text).
        if (!snapshot.evalText.empty()) {
            const int badgeX = std::clamp(x2, 40, std::max(40, width - 40));
            const int badgeY = std::clamp(y2 - static_cast<int>(sqW * 0.45), 20, std::max(20, height - 20));
            RECT badge{badgeX - 30, badgeY - 12, badgeX + 30, badgeY + 12};
            HBRUSH badgeBrush = CreateSolidBrush(RGB(0x1E, 0x1E, 0x1E));
            FillRect(dc, &badge, badgeBrush);
            HPEN badgePen = CreatePen(PS_SOLID, 2, color);
            HGDIOBJ prevBadgePen = SelectObject(dc, badgePen);
            HGDIOBJ prevBadgeBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, badge.left, badge.top, badge.right, badge.bottom);
            SelectObject(dc, prevBadgeBrush);
            SelectObject(dc, prevBadgePen);
            DeleteObject(badgePen);
            DeleteObject(badgeBrush);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, "Helvetica");
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetTextAlign(dc, TA_CENTER | TA_BASELINE);
            TextOutA(dc, badgeX, badgeY, snapshot.evalText.c_str(),
                     static_cast<int>(snapshot.evalText.size()));
            SetTextAlign(dc, TA_LEFT | TA_TOP);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }

        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(sourcePen);
        DeleteObject(destPen);
        DeleteObject(destBrush);
        DeleteObject(arrowPen);
    }

    EndPaint(hwnd, &ps);
    return 0;
}

}  // namespace cm
