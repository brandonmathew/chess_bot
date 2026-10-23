#pragma once
#include <cstdint>
#include <vector>

#include "core/Image.h"

namespace cm {
namespace cv {

// ── Colour conversions (OpenCV-compatible ranges) ───────────────────────────
Plane bgr2gray(const Image& img);
Hsv bgr2hsv(const Image& img);

// Inclusive per-channel threshold on HSV planes -> 0/255 mask.
Plane inRange(const Hsv& hsv, const int lo[3], const int hi[3]);

// ── Mask arithmetic ─────────────────────────────────────────────────────────
void bitwiseOrInPlace(Plane& dst, const Plane& src);
void clearWhere(Plane& dst, const Plane& other);  // dst = 0 wherever other != 0
std::size_t countNonzero(const Plane& p);
double fracNonzero(const Plane& p);

// ── Morphology (border-replicated) ──────────────────────────────────────────
Plane dilateRect(const Plane& src, int kw, int kh);
Plane erodeRect(const Plane& src, int kw, int kh);
Plane morphClose(const Plane& src, int kw, int kh);  // dilate then erode

// ── Statistics helpers matching numpy semantics ─────────────────────────────
double meanU8(const std::uint8_t* p, std::size_t n);
double meanPlane(const Plane& p);
double medianU8(std::vector<std::uint8_t> values);  // numpy median (float result)
double stddevU8(const std::uint8_t* p, std::size_t n);  // population (ddof = 0)
double percentileF(const std::vector<float>& values, double q);  // np.percentile linear

// Same statistics over double values (medians of sub-blocks etc.)
double meanD(const std::vector<double>& v);
double medianD(std::vector<double> v);
double stddevD(const std::vector<double>& v);  // population (ddof = 0)

}  // namespace cv
}  // namespace cm
