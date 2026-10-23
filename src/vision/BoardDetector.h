#pragma once
#include <array>
#include <optional>
#include <utility>

#include "core/Image.h"

namespace cm {

// Screen-space chess board detection: locate the 8x8 grid, slice it into 64
// squares indexed by chess square (a1 = 0 ... h8 = 63), map squares back to
// screen coordinates and guess which colour sits at the bottom.
class BoardDetector {
   public:
    // Scan a full screen capture for an 8x8 board; returns (left, top, w, h)
    // in the capture's coordinate space, or nullopt.
    std::optional<RectI> autoDetectBoard(const Image& screen) const;

    // Snap a loose box onto the internal 8x8 grid boundaries.
    RectI refineBoardBox(const Plane& gray, RectI box) const;

    // Cut the board into 64 crops; result[sq] with a1 = 0 ... h8 = 63.
    std::array<Image, 64> sliceBoard(const Image& boardImg, bool orientationWhite) const;

    // Screen-space centre of a chess square inside the given ROI.
    std::pair<int, int> squareScreenCenter(int sq, RectI roi, bool orientationWhite) const;

    // True when white pieces sit at the bottom of the image.
    bool detectOrientation(const Image& boardImg) const;

    // Heuristic: how much does this crop look like a checkerboard?
    static double evaluateBoardCandidate(const Plane& grayCrop);

    // Shared brightness helper used by orientation detection.
    static std::optional<double> pieceForegroundBrightness(const Plane& square);

   private:
    std::optional<RectI> detectByDominantColors(const Image& image) const;
    static double gridBoundaryScore(const Plane& crop);
};

}  // namespace cm
