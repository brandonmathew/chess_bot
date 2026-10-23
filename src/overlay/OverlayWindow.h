#pragma once
#include <windows.h>

#include <mutex>
#include <string>

#include "core/Image.h"

namespace cm {

// True desktop overlay for the move suggestion (Discord/NVIDIA-style): a
// borderless popup that spans the ENTIRE virtual desktop, sits in the
// topmost band above other applications, is click-through and never takes
// focus, and is composited with per-pixel alpha via UpdateLayeredWindow.
//
// The previous design (a window sized to the board ROI, colour-key
// transparency, WM_PAINT rendering) could end up composited with the main
// window instead of floating over the desktop. This design removes the
// whole class of problems: the window covers the desktop 1:1, arrow
// coordinates are plain screen coordinates, and every update re-asserts
// HWND_TOPMOST and re-composites immediately - no paint-message dependency.
class OverlayWindow {
   public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Create the (hidden) overlay window. Returns false on failure.
    bool create();
    void destroy();

    // Show the overlay. `bounds` is the board ROI in screen coordinates;
    // it only feeds the badge geometry - the window itself always spans
    // the full virtual desktop.
    void show(RectI bounds);
    void hide();
    bool visible() const { return shown_; }

    // Arrow drawing parameters in screen coordinates (thread-safe).
    void setArrow(int fromX, int fromY, int toX, int toY, int rgb, int width, const std::string& evalText);
    void clearArrow();

   private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // (Re)build the back buffer at the current virtual-desktop size.
    bool ensureSurface();
    // Draw the current state into the back buffer and composite it.
    // Caller must hold mutex_. No-op when the window is hidden.
    void renderLocked();

    struct State {
        bool hasArrow = false;
        int fromX = 0, fromY = 0, toX = 0, toY = 0;  // screen coordinates
        int roiW = 512;                              // board ROI width (badge geometry)
        int rgb = 0x0066FF00;  // encoded as r | g<<8 | b<<16 (== #00FF66)
        int width = 6;
        std::string evalText;
    };

    HWND hwnd_ = nullptr;
    bool shown_ = false;

    // Back buffer: 32bpp top-down DIB covering the virtual desktop.
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    void* pixels_ = nullptr;  // BGRA, row-major, w_ * h_ * 4
    int originX_ = 0, originY_ = 0;  // virtual-desktop origin (SM_X/YVIRTUALSCREEN)
    int w_ = 0, h_ = 0;

    mutable std::mutex mutex_;
    State state_;
};

}  // namespace cm
