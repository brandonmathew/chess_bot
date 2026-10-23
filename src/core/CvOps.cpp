#include "core/CvOps.h"

#include <algorithm>
#include <cmath>

namespace cm {
namespace cv {

Plane bgr2gray(const Image& img) {
    Plane out(img.w, img.h, 0);
    if (img.empty()) return out;
    for (std::size_t i = 0, n = img.data.size(); i + 2 < n; i += 3) {
        const int b = img.data[i], g = img.data[i + 1], r = img.data[i + 2];
        // ITU-R BT.601 weights, rounded the way OpenCV does.
        const int y = (r * 4899 + g * 9617 + b * 1868 + 8192) >> 14;
        out.d[i / 3] = static_cast<std::uint8_t>(std::clamp(y, 0, 255));
    }
    return out;
}

Hsv bgr2hsv(const Image& img) {
    Hsv out;
    out.h.resize(img.w, img.h, 0);
    out.s.resize(img.w, img.h, 0);
    out.v.resize(img.w, img.h, 0);
    if (img.empty()) return out;

    for (std::size_t i = 0, n = img.data.size(); i + 2 < n; i += 3) {
        const int b = img.data[i], g = img.data[i + 1], r = img.data[i + 2];
        const int vmax = std::max({b, g, r});
        const int vmin = std::min({b, g, r});
        const int diff = vmax - vmin;

        const int v = vmax;
        const int s = vmax ? (diff * 255 + vmax / 2) / vmax : 0;

        int h = 0;
        if (diff != 0) {
            double hf;
            if (vmax == r) {
                hf = 60.0 * (static_cast<double>(g - b) / diff);
            } else if (vmax == g) {
                hf = 60.0 * (static_cast<double>(b - r) / diff + 2.0);
            } else {
                hf = 60.0 * (static_cast<double>(r - g) / diff + 4.0);
            }
            if (hf < 0.0) hf += 360.0;
            h = static_cast<int>(std::lround(hf / 2.0)) % 180;
        }

        out.h.d[i / 3] = static_cast<std::uint8_t>(h);
        out.s.d[i / 3] = static_cast<std::uint8_t>(std::clamp(s, 0, 255));
        out.v.d[i / 3] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    }
    return out;
}

Plane inRange(const Hsv& hsv, const int lo[3], const int hi[3]) {
    Plane out(hsv.v.w, hsv.v.h, 0);
    const std::size_t n = out.d.size();
    for (std::size_t i = 0; i < n; ++i) {
        const int h = hsv.h.d[i], s = hsv.s.d[i], v = hsv.v.d[i];
        out.d[i] = (h >= lo[0] && h <= hi[0] && s >= lo[1] && s <= hi[1] && v >= lo[2] && v <= hi[2]) ? 255 : 0;
    }
    return out;
}

void bitwiseOrInPlace(Plane& dst, const Plane& src) {
    const std::size_t n = std::min(dst.d.size(), src.d.size());
    for (std::size_t i = 0; i < n; ++i) dst.d[i] = static_cast<std::uint8_t>(dst.d[i] | src.d[i]);
}

void clearWhere(Plane& dst, const Plane& other) {
    const std::size_t n = std::min(dst.d.size(), other.d.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (other.d[i]) dst.d[i] = 0;
    }
}

std::size_t countNonzero(const Plane& p) {
    std::size_t c = 0;
    for (std::uint8_t v : p.d) c += (v != 0);
    return c;
}

double fracNonzero(const Plane& p) {
    if (p.d.empty()) return 0.0;
    return static_cast<double>(countNonzero(p)) / static_cast<double>(p.d.size());
}

static Plane morphOp(const Plane& src, int kw, int kh, bool isDilate) {
    Plane out(src.w, src.h, 0);
    if (src.empty() || kw <= 0 || kh <= 0) return out;
    const int rx = kw / 2, ry = kh / 2;
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            std::uint8_t acc = isDilate ? 0 : 255;
            for (int dy = -ry; dy <= ry; ++dy) {
                const int yy = std::clamp(y + dy, 0, src.h - 1);
                for (int dx = -rx; dx <= rx; ++dx) {
                    const int xx = std::clamp(x + dx, 0, src.w - 1);
                    const std::uint8_t v = src.at(xx, yy);
                    acc = isDilate ? std::max(acc, v) : std::min(acc, v);
                }
            }
            out.at(x, y) = acc;
        }
    }
    return out;
}

Plane dilateRect(const Plane& src, int kw, int kh) { return morphOp(src, kw, kh, true); }
Plane erodeRect(const Plane& src, int kw, int kh) { return morphOp(src, kw, kh, false); }
Plane morphClose(const Plane& src, int kw, int kh) { return erodeRect(dilateRect(src, kw, kh), kw, kh); }

double meanU8(const std::uint8_t* p, std::size_t n) {
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) acc += p[i];
    return acc / static_cast<double>(n);
}

double meanPlane(const Plane& p) { return meanU8(p.d.data(), p.d.size()); }

double medianU8(std::vector<std::uint8_t> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) return static_cast<double>(values[n / 2]);
    return (static_cast<double>(values[n / 2 - 1]) + static_cast<double>(values[n / 2])) / 2.0;
}

double stddevU8(const std::uint8_t* p, std::size_t n) {
    if (n == 0) return 0.0;
    const double m = meanU8(p, n);
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = p[i] - m;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

double percentileF(const std::vector<float>& values, double q) {
    if (values.empty()) return 0.0;
    std::vector<float> v = values;
    std::sort(v.begin(), v.end());
    const double pos = (q / 100.0) * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return v[lo];
    const double frac = pos - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}

double meanD(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double acc = 0.0;
    for (double x : v) acc += x;
    return acc / static_cast<double>(v.size());
}

double medianD(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double stddevD(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const double m = meanD(v);
    double acc = 0.0;
    for (double x : v) {
        const double d = x - m;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(v.size()));
}

}  // namespace cv
}  // namespace cm
