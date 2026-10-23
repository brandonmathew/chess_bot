#pragma once
#include <windows.h>

#include <functional>

#include "core/Image.h"

namespace cm {

// Fullscreen rubber-band region selector (the tkinter selection window
// equivalent). Runs its own window on the main thread, so it participates in
// the normal message loop without blocking ImGui rendering.
class RegionSniper {
   public:
    using DoneFn = std::function<void(RectI)>;

    RegionSniper() = default;
    ~RegionSniper();

    RegionSniper(const RegionSniper&) = delete;
    RegionSniper& operator=(const RegionSniper&) = delete;

    // Show the selection window over the whole virtual desktop.
    // onDone receives virtual-desktop coordinates; it is invoked from the
    // message loop on release.
    bool open(DoneFn onDone);
    void close();

    bool active() const { return hwnd_ != nullptr; }

   private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void paint(HWND hwnd);

    HWND hwnd_ = nullptr;
    DoneFn onDone_;
    POINT start_{};
    POINT current_{};
    bool dragging_ = false;
};

}  // namespace cm
