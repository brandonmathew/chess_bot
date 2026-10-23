#include "vision/EdgeContour.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cm {

// ── Gaussian blur (separable 5x5 binomial kernel, border-replicated) ────────
static Plane gaussian5(const Plane& src) {
    static const int k[5] = {1, 4, 6, 4, 1};
    static const int ksum = 16;
    const int w = src.w, h = src.h;
    Plane tmp(w, h, 0), out(w, h, 0);
    if (src.empty()) return out;

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int acc = 0;
            for (int i = -2; i <= 2; ++i) {
                const int xx = std::clamp(x + i, 0, w - 1);
                acc += src.at(xx, y) * k[i + 2];
            }
            tmp.at(x, y) = static_cast<std::uint8_t>((acc + ksum / 2) / ksum);
        }
    }
    // Vertical pass
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int acc = 0;
            for (int i = -2; i <= 2; ++i) {
                const int yy = std::clamp(y + i, 0, h - 1);
                acc += tmp.at(x, yy) * k[i + 2];
            }
            out.at(x, y) = static_cast<std::uint8_t>((acc + ksum / 2) / ksum);
        }
    }
    return out;
}

Plane canny(const Plane& gray, double low, double high) {
    Plane out(gray.w, gray.h, 0);
    if (gray.w < 3 || gray.h < 3) return out;

    const Plane blur = gaussian5(gray);
    const int w = blur.w, h = blur.h;
    const std::size_t n = blur.size();

    std::vector<float> mag(n, 0.0f);

    // Sobel gradients + non-maximum suppression in one pass.
    auto at = [&](int x, int y) -> float { return static_cast<float>(blur.at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1))); };

    constexpr double kTan22 = 0.41421356237;   // tan(22.5°)
    constexpr double kTan67 = 2.41421356237;   // tan(67.5°)

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const float gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) +
                             at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1);
            const float gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) +
                             at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1);
            const float m = std::sqrt(gx * gx + gy * gy);
            if (m < static_cast<float>(low)) continue;

            // Quantise gradient direction and compare with the two neighbours.
            const double ax = std::abs(gx), ay = std::abs(gy);
            double n1, n2;
            const double tanx = (ax == 0.0) ? 1e9 : ay / ax;
            if (tanx < kTan22) {           // horizontal gradient -> compare left/right
                n1 = at(x - 1, y), n2 = at(x + 1, y);
            } else if (tanx > kTan67) {    // vertical gradient -> compare up/down
                n1 = at(x, y - 1), n2 = at(x, y + 1);
            } else if ((gx > 0) == (gy > 0)) {  // diagonal "\" (NW-SE)
                n1 = at(x - 1, y - 1), n2 = at(x + 1, y + 1);
            } else {                       // diagonal "/" (NE-SW)
                n1 = at(x + 1, y - 1), n2 = at(x - 1, y + 1);
            }
            const float b = at(x, y);
            if (b >= n1 && b >= n2) mag[static_cast<std::size_t>(y) * w + x] = m;
        }
    }

    // Double threshold + hysteresis (iterative flood fill from strong pixels).
    Plane weak(w, h, 0);
    std::vector<std::size_t> stack;
    stack.reserve(4096);
    for (std::size_t i = 0; i < n; ++i) {
        if (mag[i] >= static_cast<float>(low)) weak.d[i] = 1;
        if (mag[i] >= static_cast<float>(high)) {
            out.d[i] = 255;
            stack.push_back(i);
        }
    }

    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const int x = static_cast<int>(i % w);
        const int y = static_cast<int>(i / w);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const std::size_t ni = static_cast<std::size_t>(ny) * w + nx;
                if (weak.d[ni] && !out.d[ni]) {
                    out.d[ni] = 255;
                    stack.push_back(ni);
                }
            }
        }
    }
    return out;
}

// ── Connected components (8-connectivity, iterative) ────────────────────────
static void labelComponents(const Plane& bin, std::vector<std::int32_t>& labels, std::int32_t& labelCount) {
    const int w = bin.w, h = bin.h;
    const std::size_t n = bin.size();
    labels.assign(n, 0);
    labelCount = 0;
    std::vector<std::size_t> stack;
    stack.reserve(1024);

    for (std::size_t start = 0; start < n; ++start) {
        if (!bin.d[start] || labels[start]) continue;
        ++labelCount;
        const std::int32_t lbl = labelCount;
        labels[start] = lbl;
        stack.push_back(start);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const int x = static_cast<int>(i % w);
            const int y = static_cast<int>(i / w);
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx;
                    if (nx < 0 || nx >= w) continue;
                    const std::size_t ni = static_cast<std::size_t>(ny) * w + nx;
                    if (bin.d[ni] && !labels[ni]) {
                        labels[ni] = lbl;
                        stack.push_back(ni);
                    }
                }
            }
        }
    }
}

// ── Moore-neighbour boundary tracing (clockwise, Jacob stopping criterion) ──
static std::vector<Pt> traceBoundary(const Plane& bin, const std::vector<std::int32_t>& labels,
                                     std::int32_t label, std::size_t startIndex) {
    static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};    // E, SE, S, SW, W, NW, N, NE
    static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

    const int w = bin.w, h = bin.h;
    const Pt start{static_cast<int>(startIndex % w), static_cast<int>(startIndex / w)};

    auto isFg = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return labels[static_cast<std::size_t>(y) * w + x] == label;
    };

    std::vector<Pt> contour;
    contour.push_back(start);

    // Backtrack starts west of the start pixel (guaranteed background, because
    // the start pixel is the topmost/leftmost of its component).
    Pt cur = start;
    Pt back{start.x - 1, start.y};
    int backDir = 4;  // W

    constexpr int kMaxSteps = 1 << 22;
    for (int step = 0; step < kMaxSteps; ++step) {
        int foundIdx = -1;
        for (int i = 1; i <= 8; ++i) {
            const int idx = (backDir + i) % 8;
            const int nx = cur.x + DX[idx];
            const int ny = cur.y + DY[idx];
            if (isFg(nx, ny)) {
                foundIdx = idx;
                break;
            }
        }
        if (foundIdx < 0) break;  // isolated pixel

        const Pt next{cur.x + DX[foundIdx], cur.y + DY[foundIdx]};
        const Pt newBack{cur.x + DX[(foundIdx + 7) % 8], cur.y + DY[(foundIdx + 7) % 8]};

        // Direction from the new pixel back to its backtrack neighbour.
        const int ndx = newBack.x - next.x;
        const int ndy = newBack.y - next.y;
        int ndir = 0;
        for (int i = 0; i < 8; ++i) {
            if (DX[i] == ndx && DY[i] == ndy) {
                ndir = i;
                break;
            }
        }

        cur = next;
        back = newBack;
        backDir = ndir;
        contour.push_back(cur);

        if (cur == start) break;  // completed the loop
        if (step > 0 && cur.x == start.x && cur.y == start.y) break;
    }
    return contour;
}

std::vector<std::vector<Pt>> findContoursBinary(const Plane& binary) {
    std::vector<std::vector<Pt>> out;
    if (binary.empty()) return out;

    std::vector<std::int32_t> labels;
    std::int32_t labelCount = 0;
    labelComponents(binary, labels, labelCount);
    if (labelCount == 0) return out;

    out.reserve(static_cast<std::size_t>(labelCount));

    // First pixel of each label (row-major order -> topmost/leftmost).
    std::vector<std::size_t> first(static_cast<std::size_t>(labelCount) + 1, 0);
    std::vector<bool> found(static_cast<std::size_t>(labelCount) + 1, false);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const std::int32_t l = labels[i];
        if (l > 0 && !found[static_cast<std::size_t>(l)]) {
            found[static_cast<std::size_t>(l)] = true;
            first[static_cast<std::size_t>(l)] = i;
        }
    }

    for (std::int32_t l = 1; l <= labelCount; ++l) {
        out.push_back(traceBoundary(binary, labels, l, first[static_cast<std::size_t>(l)]));
    }
    return out;
}

// ── Ramer–Douglas–Peucker ───────────────────────────────────────────────────
static double perpDist(const Pt& p, const Pt& a, const Pt& b) {
    const double dx = static_cast<double>(b.x) - a.x;
    const double dy = static_cast<double>(b.y) - a.y;
    if (dx == 0.0 && dy == 0.0) {
        const double ex = p.x - a.x, ey = p.y - a.y;
        return std::sqrt(ex * ex + ey * ey);
    }
    const double num = std::abs(dy * p.x - dx * p.y + static_cast<double>(b.x) * a.y -
                                static_cast<double>(b.y) * a.x);
    return num / std::sqrt(dx * dx + dy * dy);
}

std::vector<Pt> approxPolyDP(const std::vector<Pt>& pts, double eps, bool closed) {
    if (pts.size() < 3) return pts;

    std::vector<Pt> poly = pts;
    if (closed && !(pts.front() == pts.back())) poly.push_back(pts.front());

    std::vector<char> keep(poly.size(), 0);
    keep.front() = keep.back() = 1;

    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(0, poly.size() - 1);
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double maxd = -1.0;
        std::size_t idx = a;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double d = perpDist(poly[i], poly[a], poly[b]);
            if (d > maxd) {
                maxd = d;
                idx = i;
            }
        }
        if (maxd > eps) {
            keep[idx] = 1;
            stack.emplace_back(a, idx);
            stack.emplace_back(idx, b);
        }
    }

    std::vector<Pt> result;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        if (keep[i]) result.push_back(poly[i]);
    }
    if (closed && result.size() > 1 && result.front() == result.back()) result.pop_back();
    return result;
}

double arcLength(const std::vector<Pt>& pts, bool closed) {
    if (pts.size() < 2) return 0.0;
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double dx = static_cast<double>(pts[i].x) - pts[i - 1].x;
        const double dy = static_cast<double>(pts[i].y) - pts[i - 1].y;
        total += std::sqrt(dx * dx + dy * dy);
    }
    if (closed && pts.size() > 2) {
        const double dx = static_cast<double>(pts.front().x) - pts.back().x;
        const double dy = static_cast<double>(pts.front().y) - pts.back().y;
        total += std::sqrt(dx * dx + dy * dy);
    }
    return total;
}

RectI boundingRect(const std::vector<Pt>& pts) {
    RectI r;
    if (pts.empty()) return r;
    int minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (const Pt& p : pts) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    r.x = minx;
    r.y = miny;
    r.w = maxx - minx + 1;
    r.h = maxy - miny + 1;
    return r;
}

}  // namespace cm
