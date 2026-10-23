#pragma once
#include <windows.h>

#include <mutex>
#include <string>

#include "core/Image.h"

namespace cm {

// Transparent, click-through suggestion overlay (the tkinter Canvas equivalent).
// Created on the main thread; the tracker thread updates it through
// setSuggestion() + InvalidateRect(), and the main loop's message dispatch
// performs the actual painting.
class OverlayWindow {
   public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Create the (hidden) layered window. Returns false on failure.
    bool create();
    void destroy();

    // Position/size in virtual-desktop coordinates; passes through to ShowWindow.
    void show(RectI bounds);
    void hide();
    bool visible() const;

    // Arrow drawing parameters (thread-safe).
    void setArrow(int fromX, int fromY, int toX, int toY, int rgb, int width, const std::string& evalText);
    void clearArrow();

    // ROI changes invalidate nothing by themselves; arrow colour/width are
    // copied per call, so the tracker simply re-posts its suggestion.

   private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handlePaint(HWND hwnd);

    struct State {
        bool hasArrow = false;
        int fromX = 0, fromY = 0, toX = 0, toY = 0;  // screen coordinates
        int originX = 0, originY = 0;                // overlay window origin (ROI)
        int roiW = 512;                              // overlay/ROI width
        int rgb = 0x0066FF00;  // encoded as r | g<<8 | b<<16 (== #00FF66)
        int width = 6;
        std::string evalText;
    };

    HWND hwnd_ = nullptr;
    mutable std::mutex mutex_;
    State state_;

    static constexpr COLORREF kTransparencyKey = RGB(0xAB, 0xCD, 0xEF);
};

}  // namespace cm
