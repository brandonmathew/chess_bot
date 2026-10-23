#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace cm {

// BGR 8-bit image (the same channel order mss/OpenCV produced in the Python
// implementation so every threshold constant ports unchanged).
struct Image {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> data;  // w * h * 3, row-major

    Image() = default;
    Image(int w_, int h_) { resize(w_, h_); }

    void resize(int w_, int h_) {
        w = w_;
        h = h_;
        data.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 0);
    }

    bool empty() const { return data.empty(); }

    std::uint8_t* px(int x, int y) { return data.data() + (static_cast<std::size_t>(y) * w + x) * 3; }
    const std::uint8_t* px(int x, int y) const {
        return data.data() + (static_cast<std::size_t>(y) * w + x) * 3;
    }

    // Channel access: c = 0 -> B, 1 -> G, 2 -> R
    std::uint8_t& at(int y, int x, int c) { return data[(static_cast<std::size_t>(y) * w + x) * 3 + c]; }
    std::uint8_t at(int y, int x, int c) const { return data[(static_cast<std::size_t>(y) * w + x) * 3 + c]; }

    Image clone() const {
        Image i;
        i.w = w;
        i.h = h;
        i.data = data;
        return i;
    }

    // Deep copy of a rectangle (inclusive of left/top, exclusive of right/bottom)
    Image crop(int x, int y, int cw, int ch) const {
        Image out(cw, ch);
        if (cw <= 0 || ch <= 0) return out;
        for (int yy = 0; yy < ch; ++yy) {
            int sy = y + yy;
            if (sy < 0 || sy >= h) continue;
            for (int xx = 0; xx < cw; ++xx) {
                int sx = x + xx;
                if (sx < 0 || sx >= w) continue;
                std::memcpy(out.px(xx, yy), px(sx, sy), 3);
            }
        }
        return out;
    }
};

// Single-channel 8-bit plane.
struct Plane {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> d;

    Plane() = default;
    Plane(int w_, int h_, std::uint8_t v = 0) { resize(w_, h_, v); }

    void resize(int w_, int h_, std::uint8_t v = 0) {
        w = w_;
        h = h_;
        d.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), v);
    }

    bool empty() const { return d.empty(); }
    std::size_t size() const { return d.size(); }

    std::uint8_t& operator[](std::size_t i) { return d[i]; }
    std::uint8_t operator[](std::size_t i) const { return d[i]; }

    std::uint8_t at(int x, int y) const { return d[static_cast<std::size_t>(y) * w + x]; }
    std::uint8_t& at(int x, int y) { return d[static_cast<std::size_t>(y) * w + x]; }

    // Deep copy of a rectangle (inclusive of left/top, exclusive of right/bottom)
    Plane crop(int x, int y, int cw, int ch) const {
        Plane out(cw, ch, 0);
        if (cw <= 0 || ch <= 0) return out;
        for (int yy = 0; yy < ch; ++yy) {
            const int sy = y + yy;
            if (sy < 0 || sy >= h) continue;
            for (int xx = 0; xx < cw; ++xx) {
                const int sx = x + xx;
                if (sx < 0 || sx >= w) continue;
                out.at(xx, yy) = at(sx, sy);
            }
        }
        return out;
    }
};

// HSV planes: h in [0,179], s in [0,255], v in [0,255] (OpenCV convention)
struct Hsv {
    Plane h, s, v;
};

struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// "Area" resampling (equivalent of OpenCV INTER_AREA): every destination
// pixel is the weighted mean of the source pixels it covers.
Image resizeArea(const Image& src, int dw, int dh);
Plane resizeArea(const Plane& src, int dw, int dh);

}  // namespace cm
