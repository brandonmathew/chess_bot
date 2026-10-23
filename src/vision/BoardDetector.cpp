#include "vision/BoardDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "core/CvOps.h"
#include "vision/EdgeContour.h"

namespace cm {

static double planeMean(const Plane& p, int x1, int y1, int x2, int y2) {
    x1 = std::clamp(x1, 0, p.w);
    x2 = std::clamp(x2, 0, p.w);
    y1 = std::clamp(y1, 0, p.h);
    y2 = std::clamp(y2, 0, p.h);
    if (x2 <= x1 || y2 <= y1) return 0.0;
    double acc = 0.0;
    std::size_t n = 0;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            acc += p.at(x, y);
            ++n;
        }
    }
    return n ? acc / static_cast<double>(n) : 0.0;
}

static std::pair<double, double> lightDarkMeans(const std::array<std::array<double, 8>, 8>& means) {
    double light = 0.0, dark = 0.0;
    int nl = 0, nd = 0;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (((r + c) % 2) == 1) {
                light += means[r][c];
                ++nl;
            } else {
                dark += means[r][c];
                ++nd;
            }
        }
    }
    return {nl ? light / nl : 0.0, nd ? dark / nd : 0.0};
}

double BoardDetector::evaluateBoardCandidate(const Plane& grayCrop) {
    const int ch = grayCrop.h, cw = grayCrop.w;
    if (ch < 8 || cw < 8) return 0.0;

    const double sqH = static_cast<double>(ch) / 8.0;
    const double sqW = static_cast<double>(cw) / 8.0;

    std::array<std::array<double, 8>, 8> means{};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            // Sample the centre 50% of the square to avoid piece edges.
            const int y1 = static_cast<int>((r + 0.25) * sqH);
            const int y2 = static_cast<int>((r + 0.75) * sqH);
            const int x1 = static_cast<int>((c + 0.25) * sqW);
            const int x2 = static_cast<int>((c + 0.75) * sqW);
            means[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = planeMean(grayCrop, x1, y1, x2, y2);
        }
    }

    const auto [lightMean, darkMean] = lightDarkMeans(means);
    return std::abs(lightMean - darkMean) / 255.0;
}

std::optional<RectI> BoardDetector::autoDetectBoard(const Image& screen) const {
    if (screen.empty()) return std::nullopt;
    const Plane gray = cv::bgr2gray(screen);
    const int h = gray.h, w = gray.w;

    const int minDim = static_cast<int>(std::min(h, w) * 0.2);
    const int maxDim = static_cast<int>(std::min(h, w) * 0.95);

    const Plane edges = cv::canny(gray, 40.0, 150.0);
    const auto contours = findContoursBinary(edges);

    std::optional<RectI> bestBox;
    double bestScore = -1.0;

    for (const auto& cnt : contours) {
        const double peri = arcLength(cnt, true);
        const auto approx = approxPolyDP(cnt, 0.02 * peri, true);
        // The Python original required exactly 4 vertices; accepting a few
        // extra keeps jittered Canny outlines in the running - the checkerboard
        // score below still selects the real board.
        if (approx.size() < 4 || approx.size() > 8) continue;

        const RectI box = boundingRect(approx);
        if (box.w <= 0 || box.h <= 0) continue;
        const double aspect = static_cast<double>(box.w) / static_cast<double>(box.h);
        if (aspect < 0.92 || aspect > 1.08) continue;
        if (box.w < minDim || box.w > maxDim || box.h < minDim || box.h > maxDim) continue;

        const Plane crop = gray.crop(box.x, box.y, box.w, box.h);
        const double score = evaluateBoardCandidate(crop);
        if (score > bestScore) {
            bestScore = score;
            bestBox = box;
        }
    }

    if (bestBox && bestScore > 0.3) return refineBoardBox(gray, *bestBox);

    // Some board themes do not expose a continuous outer-board contour.
    // Fall back to finding the two dominant alternating square colours.
    const auto colorBox = detectByDominantColors(screen);
    if (colorBox) return refineBoardBox(gray, *colorBox);
    return std::nullopt;
}

std::optional<RectI> BoardDetector::detectByDominantColors(const Image& image) const {
    const int h = image.h, w = image.w;
    if (image.empty()) return std::nullopt;

    // Quantise to 4 bits per channel and count occurrences (0..4095).
    std::array<std::uint32_t, 4096> counts{};
    const std::size_t n = static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = image.data.data() + i * 3;
        const int code = (p[0] >> 4) + (p[1] >> 4) * 16 + (p[2] >> 4) * 256;
        ++counts[static_cast<std::size_t>(code)];
    }

    std::vector<int> order(4096);
    for (int i = 0; i < 4096; ++i) order[static_cast<std::size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return counts[static_cast<std::size_t>(a)] > counts[static_cast<std::size_t>(b)];
    });

    const auto minCount = static_cast<std::size_t>(n * 0.003);
    std::vector<std::array<std::uint8_t, 3>> colors;
    for (const int code : order) {
        if (counts[static_cast<std::size_t>(code)] < minCount) break;
        const std::array<std::uint8_t, 3> color = {static_cast<std::uint8_t>((code % 16) * 16),
                                                   static_cast<std::uint8_t>(((code / 16) % 16) * 16),
                                                   static_cast<std::uint8_t>(((code / 256) % 16) * 16)};
        Image tmp(1, 1);
        *tmp.px(0, 0) = color[0];
        *(tmp.px(0, 0) + 1) = color[1];
        *(tmp.px(0, 0) + 2) = color[2];
        const Hsv hsv = cv::bgr2hsv(tmp);
        const int s = hsv.s.at(0, 0), v = hsv.v.at(0, 0);
        if (s > 25 && v > 40 && v < 250) colors.push_back(color);
        if (colors.size() >= 10) break;
    }
    if (colors.size() < 2) return std::nullopt;

    // Binary mask per colour: every channel within distance 24.
    std::vector<Plane> masks;
    masks.reserve(colors.size());
    for (const auto& color : colors) {
        Plane mask(w, h, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* p = image.data.data() + i * 3;
            const int d0 = std::abs(static_cast<int>(p[0]) - color[0]);
            const int d1 = std::abs(static_cast<int>(p[1]) - color[1]);
            const int d2 = std::abs(static_cast<int>(p[2]) - color[2]);
            mask.d[i] = (std::max({d0, d1, d2}) <= 24) ? 255 : 0;
        }
        masks.push_back(std::move(mask));
    }

    const Plane gray = cv::bgr2gray(image);
    std::optional<RectI> best;
    double bestScore = -std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < colors.size(); ++i) {
        for (std::size_t j = i + 1; j < colors.size(); ++j) {
            double colorDistance = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double d = static_cast<double>(colors[i][static_cast<std::size_t>(k)]) -
                                 static_cast<double>(colors[j][static_cast<std::size_t>(k)]);
                colorDistance += d * d;
            }
            colorDistance = std::sqrt(colorDistance);
            if (colorDistance < 35.0) continue;

            Plane mask = masks[i];
            cv::bitwiseOrInPlace(mask, masks[j]);
            mask = cv::morphClose(mask, 9, 9);

            const auto contours = findContoursBinary(mask);
            for (const auto& cnt : contours) {
                const RectI box = boundingRect(cnt);
                if (box.w <= 0 || box.h <= 0) continue;
                if (std::min(box.w, box.h) < static_cast<int>(std::min(h, w) * 0.20)) continue;
                const double aspect = static_cast<double>(box.w) / static_cast<double>(box.h);
                if (aspect < 0.90 || aspect > 1.10) continue;

                double covered = 0.0;
                for (int y = box.y; y < box.y + box.h; ++y) {
                    for (int x = box.x; x < box.x + box.w; ++x) {
                        covered += (mask.at(x, y) != 0) ? 1.0 : 0.0;
                    }
                }
                const double coverage = covered / (static_cast<double>(box.w) * box.h);
                if (coverage < 0.50) continue;

                const int side = std::min({std::max(box.w, box.h), w, h});
                const int cx = box.x + box.w / 2;
                const int cy = box.y + box.h / 2;
                const int sx = std::max(0, std::min(w - side, cx - side / 2));
                const int sy = std::max(0, std::min(h - side, cy - side / 2));
                const Plane candidate = gray.crop(sx, sy, side, side);
                const double checkerScore = evaluateBoardCandidate(candidate);
                if (checkerScore < 0.15) continue;

                const double score = coverage * side * side * checkerScore;
                if (score > bestScore) {
                    bestScore = score;
                    best = RectI{sx, sy, side, side};
                }
            }
        }
    }
    return best;
}

// ── Grid alignment scoring ──────────────────────────────────────────────────
double BoardDetector::gridBoundaryScore(const Plane& crop) {
    const int h = crop.h, w = crop.w;
    if (h < 64 || w < 64) return -std::numeric_limits<double>::infinity();

    const Plane small = resizeArea(crop, 64, 64);

    // Median of the four 2x2 corner patches inside each 8x8 block.
    std::array<std::array<double, 8>, 8> medians{};
    std::vector<double> allCornerValues;  // for corner_noise (global mean)
    allCornerValues.reserve(8 * 8 * 16);

    static const int kCornerOffset[4] = {0, 1, 6, 7};
    for (int br = 0; br < 8; ++br) {
        for (int bc = 0; bc < 8; ++bc) {
            std::array<double, 16> corners{};
            int k = 0;
            for (int pr : kCornerOffset) {
                for (int pc : kCornerOffset) {
                    const double val = small.at(bc * 8 + pc, br * 8 + pr);
                    corners[static_cast<std::size_t>(k++)] = val;
                    allCornerValues.push_back(val);
                }
            }
            std::vector<double> tmp(corners.begin(), corners.end());
            medians[static_cast<std::size_t>(br)][static_cast<std::size_t>(bc)] = cv::medianD(std::move(tmp));
        }
    }

    double cornerNoise = 0.0;
    {
        std::size_t k = 0;
        for (int br = 0; br < 8; ++br) {
            for (int bc = 0; bc < 8; ++bc) {
                const double m = medians[static_cast<std::size_t>(br)][static_cast<std::size_t>(bc)];
                for (int i = 0; i < 16; ++i) cornerNoise += std::abs(allCornerValues[k++] - m);
            }
        }
        cornerNoise = allCornerValues.empty() ? 0.0 : cornerNoise / allCornerValues.size();
    }

    std::vector<double> groupA, groupB;
    for (int br = 0; br < 8; ++br) {
        for (int bc = 0; bc < 8; ++bc) {
            const double m = medians[static_cast<std::size_t>(br)][static_cast<std::size_t>(bc)];
            if (((br + bc) % 2) == 0) {
                groupA.push_back(m);
            } else {
                groupB.push_back(m);
            }
        }
    }
    const double contrast = std::abs(cv::meanD(groupA) - cv::meanD(groupB));
    const double inconsistency = cv::stddevD(groupA) + cv::stddevD(groupB);
    return contrast - 2.0 * cornerNoise - inconsistency;
}

RectI BoardDetector::refineBoardBox(const Plane& gray, RectI box) const {
    const int x = box.x, y = box.y, bw = box.w, bh = box.h;
    if (bw <= 0 || bh <= 0) return box;
    const Plane crop = gray.crop(x, y, bw, bh);

    const int maxSide = std::min(bw, bh);
    // Contour detection is normally only a few pixels outside the board;
    // a 4% trim window is enough while keeping calibration responsive.
    const int minSide = std::max(64, static_cast<int>(maxSide * 0.96));

    double bestScore = -std::numeric_limits<double>::infinity();
    int bestOx = 0, bestOy = 0, bestSide = maxSide;

    auto tryCandidate = [&](int side, int ox, int oy) {
        if (side <= 0 || ox < 0 || oy < 0 || ox + side > bw || oy + side > bh) return;
        const Plane cand = crop.crop(ox, oy, side, side);
        const double s = gridBoundaryScore(cand);
        if (s > bestScore) {
            bestScore = s;
            bestOx = ox;
            bestOy = oy;
            bestSide = side;
        }
    };

    // Coarse search first, then refine around the winner: this keeps the more
    // reliable checkerboard score responsive on large boards.
    std::vector<int> coarseSides;
    for (int s = minSide; s <= maxSide; s += 2) coarseSides.push_back(s);
    if (coarseSides.empty() || coarseSides.back() != maxSide) coarseSides.push_back(maxSide);

    for (const int side : coarseSides) {
        const int maxXoff = bw - side, maxYoff = bh - side;
        std::vector<int> yOffs, xOffs;
        for (int o = 0; o <= maxYoff; o += 2) yOffs.push_back(o);
        if (yOffs.empty() || yOffs.back() != maxYoff) yOffs.push_back(maxYoff);
        for (int o = 0; o <= maxXoff; o += 2) xOffs.push_back(o);
        if (xOffs.empty() || xOffs.back() != maxXoff) xOffs.push_back(maxXoff);
        for (const int oy : yOffs)
            for (const int ox : xOffs) tryCandidate(side, ox, oy);
    }

    const int fineLo = std::max(minSide, bestSide - 2);
    const int fineHi = std::min(maxSide, bestSide + 2);
    const int coarseX = bestOx, coarseY = bestOy;
    for (int side = fineLo; side <= fineHi; ++side) {
        const int maxXoff = bw - side, maxYoff = bh - side;
        if (maxXoff < 0 || maxYoff < 0) continue;
        const int oyLo = std::max(0, coarseY - 2), oyHi = std::min(maxYoff, coarseY + 2);
        const int oxLo = std::max(0, coarseX - 2), oxHi = std::min(maxXoff, coarseX + 2);
        for (int oy = oyLo; oy <= oyHi; ++oy)
            for (int ox = oxLo; ox <= oxHi; ++ox) tryCandidate(side, ox, oy);
    }

    return RectI{x + bestOx, y + bestOy, bestSide, bestSide};
}

// ── Slicing & coordinate mapping ────────────────────────────────────────────
std::array<Image, 64> BoardDetector::sliceBoard(const Image& boardImg, bool orientationWhite) const {
    std::array<Image, 64> squares;
    for (auto& sq : squares) sq = Image(64, 64);

    if (boardImg.empty()) return squares;
    const int bh = boardImg.h, bw = boardImg.w;
    const double sqH = static_cast<double>(bh) / 8.0;
    const double sqW = static_cast<double>(bw) / 8.0;

    for (int rankIdx = 0; rankIdx < 8; ++rankIdx) {  // 0 is top row on screen
        for (int fileIdx = 0; fileIdx < 8; ++fileIdx) {  // 0 is left col on screen
            const int y1 = static_cast<int>(rankIdx * sqH);
            const int y2 = std::max(y1 + 1, static_cast<int>((rankIdx + 1) * sqH));
            const int x1 = static_cast<int>(fileIdx * sqW);
            const int x2 = std::max(x1 + 1, static_cast<int>((fileIdx + 1) * sqW));

            Image crop = boardImg.crop(x1, y1, x2 - x1, y2 - y1);
            if (crop.w != 64 || crop.h != 64) crop = resizeArea(crop, 64, 64);

            const int chessRank = orientationWhite ? (7 - rankIdx) : rankIdx;
            const int chessFile = orientationWhite ? fileIdx : (7 - fileIdx);
            const int sqIndex = chessRank * 8 + chessFile;  // a1 = 0
            squares[static_cast<std::size_t>(sqIndex)] = std::move(crop);
        }
    }
    return squares;
}

std::pair<int, int> BoardDetector::squareScreenCenter(int sq, RectI roi, bool orientationWhite) const {
    const int fileIdx = sq % 8;
    const int rankIdx = sq / 8;

    const double sqW = static_cast<double>(roi.w) / 8.0;
    const double sqH = static_cast<double>(roi.h) / 8.0;

    const int col = orientationWhite ? fileIdx : (7 - fileIdx);
    const int row = orientationWhite ? (7 - rankIdx) : rankIdx;

    const int centerX = static_cast<int>(roi.x + (col + 0.5) * sqW);
    const int centerY = static_cast<int>(roi.y + (row + 0.5) * sqH);
    return {centerX, centerY};
}

std::optional<double> BoardDetector::pieceForegroundBrightness(const Plane& square) {
    if (square.empty()) return std::nullopt;

    const int h = square.h, w = square.w;
    const int corner = std::max(2, static_cast<int>(0.10 * std::min(h, w)));
    std::vector<std::uint8_t> corners;
    corners.reserve(static_cast<std::size_t>(corner) * corner * 4);
    for (int y = 0; y < corner; ++y) {
        for (int x = 0; x < corner; ++x) {
            corners.push_back(square.at(x, y));
            corners.push_back(square.at(w - 1 - x, y));
            corners.push_back(square.at(x, h - 1 - y));
            corners.push_back(square.at(w - 1 - x, h - 1 - y));
        }
    }
    const double background = cv::medianU8(std::move(corners));

    const int marginH = std::max(2, static_cast<int>(0.15 * h));
    const int marginW = std::max(2, static_cast<int>(0.15 * w));
    const int y1 = marginH, y2 = h - marginH;
    const int x1 = marginW, x2 = w - marginW;
    if (y2 <= y1 || x2 <= x1) return std::nullopt;

    double sum = 0.0;
    std::size_t count = 0, total = 0;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const double v = square.at(x, y);
            ++total;
            if (std::abs(v - background) > 18.0) {
                sum += v;
                ++count;
            }
        }
    }
    if (total == 0) return std::nullopt;
    if (static_cast<double>(count) / total < 0.04) return std::nullopt;
    return sum / static_cast<double>(count);
}

bool BoardDetector::detectOrientation(const Image& boardImg) const {
    try {
        if (boardImg.empty() || boardImg.h == 0) return true;
        const Plane gray = cv::bgr2gray(boardImg);
        const int bh = gray.h, bw = gray.w;

        std::vector<double> topBrightness, bottomBrightness;
        for (const int row : {0, 1, 6, 7}) {
            auto& target = row < 2 ? topBrightness : bottomBrightness;
            for (int col = 0; col < 8; ++col) {
                const int y1 = static_cast<int>(row * bh / 8.0);
                const int y2 = static_cast<int>((row + 1) * bh / 8.0);
                const int x1 = static_cast<int>(col * bw / 8.0);
                const int x2 = static_cast<int>((col + 1) * bw / 8.0);
                const Plane square = gray.crop(x1, y1, x2 - x1, y2 - y1);
                const auto brightness = pieceForegroundBrightness(square);
                if (brightness) target.push_back(*brightness);
            }
        }

        if (!topBrightness.empty() && !bottomBrightness.empty()) {
            const double topScore = cv::medianD(std::move(topBrightness));
            const double bottomScore = cv::medianD(std::move(bottomBrightness));
            return bottomScore > topScore;
        }

        // Conservative fallback for sparse/endgame positions.
        const double bottomMean = planeMean(gray, 0, static_cast<int>(6 * bh / 8.0), bw, bh);
        const double topMean = planeMean(gray, 0, 0, bw, static_cast<int>(2 * bh / 8.0));
        return bottomMean >= topMean;
    } catch (...) {
        return true;
    }
}

}  // namespace cm
