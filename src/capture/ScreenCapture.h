#pragma once
#include <string>
#include <vector>

#include "core/Image.h"

namespace cm {

struct MonitorInfo {
    int index = 0;  // 1..n (0 in the mss convention was the whole virtual desktop)
    RectI rect;     // virtual-desktop coordinates
    bool primary = false;
};

// Screen capture via GDI BitBlt (the mss equivalent). Coordinates are physical
// pixels of the Windows virtual desktop, matching the per-monitor DPI-aware
// process setting applied at startup.
class ScreenCapture {
   public:
    std::vector<MonitorInfo> monitors() const;

    // Whole virtual desktop (mss monitors[0]).
    Image grabVirtualDesktop() const;

    // One physical monitor (mss monitors[1..n]).
    Image grabMonitor(int monitorIndex) const;

    // A bounding box in virtual-desktop coordinates.
    Image grabRegion(RectI region) const;
};

}  // namespace cm
