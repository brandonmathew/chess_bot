#include "capture/RegionSniper.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/Log.h"

namespace cm {

namespace {
constexpr const char* kSniperClass = "ChessMongerRegionSniper";
bool g_sniperClassRegistered = false;
}  // namespace

RegionSniper::~RegionSniper() { close(); }

bool RegionSniper::open(DoneFn onDone) {
    if (hwnd_) return false;
    HINSTANCE instance = GetModuleHandleA(nullptr);

    if (!g_sniperClassRegistered) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RegionSniper::wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSniperClass;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            log::print("[Sniper] RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
        g_sniperClassRegistered = true;
    }

    onDone_ = std::move(onDone);

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwnd_ = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSniperClass,
                            "ChessMonger Select Region", WS_POPUP, x, y, w, h, nullptr, nullptr, instance,
                            this);
    if (!hwnd_) {
        log::print("[Sniper] CreateWindowEx failed (%lu)", GetLastError());
        hwnd_ = nullptr;
        return false;
    }

    // 30% opacity like the tkinter overlay used.
    SetLayeredWindowAttributes(hwnd_, 0, 77, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    log::print("[Sniper] Drag to select the board region, ESC to cancel.");
    return true;
}

void RegionSniper::close() {
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
    onDone_ = nullptr;
    dragging_ = false;
}

LRESULT CALLBACK RegionSniper::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<RegionSniper*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT RegionSniper::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            dragging_ = true;
            start_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            current_ = start_;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_) {
                current_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!dragging_) return 0;
            dragging_ = false;
            ReleaseCapture();
            current_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // The window spans the whole virtual desktop: client coordinates
            // are relative to its origin, so add it back to get screen coords.
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int width = std::abs(current_.x - start_.x);
            int height = std::abs(current_.y - start_.y);
            const int left = std::min(start_.x, current_.x);
            const int top = std::min(start_.y, current_.y);

            auto callback = std::move(onDone_);
            onDone_ = nullptr;
            HWND owner = hwnd_;
            hwnd_ = nullptr;
            DestroyWindow(owner);

            if (width > 80 && height > 80) {
                // Match the original: square off slightly rectangular drags.
                const int dim = (width + height) / 2;
                RectI selection{vx + left, vy + top, dim, dim};
                log::print("[Sniper] Selected region: (%d, %d, %d, %d)", selection.x, selection.y,
                           selection.w, selection.h);
                if (callback) callback(selection);
            } else {
                log::print("[Sniper] Selection too small (%dx%d) - ignored. Drag across the whole board.",
                           width, height);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                log::print("[Sniper] Selection cancelled.");
                close();
                return 0;
            }
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void RegionSniper::paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    HBRUSH bg = CreateSolidBrush(RGB(10, 10, 20));
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    if (dragging_) {
        RECT band{std::min(start_.x, current_.x), std::min(start_.y, current_.y),
                  std::max(start_.x, current_.x), std::max(start_.y, current_.y)};
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(0, 255, 102));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, band.left, band.top, band.right, band.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 255, 204));
    HFONT font = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    const char* hint = "Click and drag a box covering the 8x8 chess board (Press ESC to cancel)";
    TextOutA(dc, (client.right - 620) / 2, 40, hint, static_cast<int>(strlen(hint)));
    SelectObject(dc, oldFont);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}

}  // namespace cm
