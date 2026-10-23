#pragma once
#include <vector>

#include "core/Image.h"

namespace cm {

struct Pt {
    int x = 0;
    int y = 0;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
};

// ── Edge detection (Canny: blur -> Sobel -> NMS -> hysteresis) ──────────────
Plane canny(const Plane& gray, double low, double high);

// ── Contours of a binary image ──────────────────────────────────────────────
// Outer boundary of every 8-connected foreground component (the original code
// called cv2.findContours(..., RETR_TREE) but discarded the hierarchy, so only
// the outer polygons were ever used).
std::vector<std::vector<Pt>> findContoursBinary(const Plane& binary);

// ── Polygon utilities (OpenCV equivalents) ──────────────────────────────────
std::vector<Pt> approxPolyDP(const std::vector<Pt>& pts, double eps, bool closed);
double arcLength(const std::vector<Pt>& pts, bool closed);
RectI boundingRect(const std::vector<Pt>& pts);

}  // namespace cm
