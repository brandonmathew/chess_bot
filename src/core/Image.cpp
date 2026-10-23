#include "core/Image.h"

#include <algorithm>
#include <cmath>

namespace cm {

Image resizeArea(const Image& src, int dw, int dh) {
    Image out;
    if (src.empty() || dw <= 0 || dh <= 0) return out;
    out.resize(dw, dh);
    if (src.w == dw && src.h == dh) {
        out.data = src.data;
        return out;
    }

    const double sx = static_cast<double>(src.w) / dw;
    const double sy = static_cast<double>(src.h) / dh;

    for (int y = 0; y < dh; ++y) {
        const double y0f = y * sy;
        const double y1f = (y + 1) * sy;
        int y0 = static_cast<int>(std::floor(y0f));
        int y1 = static_cast<int>(std::ceil(y1f));
        y0 = std::clamp(y0, 0, src.h - 1);
        y1 = std::clamp(y1, y0 + 1, src.h);

        for (int x = 0; x < dw; ++x) {
            const double x0f = x * sx;
            const double x1f = (x + 1) * sx;
            int x0 = static_cast<int>(std::floor(x0f));
            int x1 = static_cast<int>(std::ceil(x1f));
            x0 = std::clamp(x0, 0, src.w - 1);
            x1 = std::clamp(x1, x0 + 1, src.w);

            double acc[3] = {0.0, 0.0, 0.0};
            double wsum = 0.0;
            for (int yy = y0; yy < y1; ++yy) {
                const double wy = std::min(static_cast<double>(yy + 1), y1f) - std::max(static_cast<double>(yy), y0f);
                if (wy <= 0.0) continue;
                for (int xx = x0; xx < x1; ++xx) {
                    const double wx = std::min(static_cast<double>(xx + 1), x1f) - std::max(static_cast<double>(xx), x0f);
                    if (wx <= 0.0) continue;
                    const double wgt = wx * wy;
                    const std::uint8_t* p = src.px(xx, yy);
                    for (int c = 0; c < 3; ++c) acc[c] += wgt * p[c];
                    wsum += wgt;
                }
            }
            if (wsum <= 0.0) wsum = 1.0;
            std::uint8_t* q = out.px(x, y);
            for (int c = 0; c < 3; ++c) {
                q[c] = static_cast<std::uint8_t>(std::lround(std::clamp(acc[c] / wsum, 0.0, 255.0)));
            }
        }
    }
    return out;
}

Plane resizeArea(const Plane& src, int dw, int dh) {
    Plane out;
    if (src.empty() || dw <= 0 || dh <= 0) return out;
    out.resize(dw, dh, 0);
    if (src.w == dw && src.h == dh) {
        out.d = src.d;
        return out;
    }

    const double sx = static_cast<double>(src.w) / dw;
    const double sy = static_cast<double>(src.h) / dh;

    for (int y = 0; y < dh; ++y) {
        const double y0f = y * sy;
        const double y1f = (y + 1) * sy;
        int y0 = std::clamp(static_cast<int>(std::floor(y0f)), 0, src.h - 1);
        int y1 = std::clamp(static_cast<int>(std::ceil(y1f)), y0 + 1, src.h);

        for (int x = 0; x < dw; ++x) {
            const double x0f = x * sx;
            const double x1f = (x + 1) * sx;
            int x0 = std::clamp(static_cast<int>(std::floor(x0f)), 0, src.w - 1);
            int x1 = std::clamp(static_cast<int>(std::ceil(x1f)), x0 + 1, src.w);

            double acc = 0.0, wsum = 0.0;
            for (int yy = y0; yy < y1; ++yy) {
                const double wy = std::min(static_cast<double>(yy + 1), y1f) - std::max(static_cast<double>(yy), y0f);
                if (wy <= 0.0) continue;
                for (int xx = x0; xx < x1; ++xx) {
                    const double wx = std::min(static_cast<double>(xx + 1), x1f) - std::max(static_cast<double>(xx), x0f);
                    if (wx <= 0.0) continue;
                    acc += wx * wy * src.at(xx, yy);
                    wsum += wx * wy;
                }
            }
            if (wsum <= 0.0) wsum = 1.0;
            out.at(x, y) = static_cast<std::uint8_t>(std::lround(std::clamp(acc / wsum, 0.0, 255.0)));
        }
    }
    return out;
}

}  // namespace cm
