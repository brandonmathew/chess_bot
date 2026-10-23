// Ported from tests/test_chess_vision.py + orientation cases from
// tests/test_advanced_tracking.py.
#include <utility>

#include "chess.hpp"
#include "doctest.h"
#include "fixtures.h"
#include "vision/BoardDetector.h"
#include "vision/GameTracker.h"

using cm::BoardDetector;
using cm::Image;
using cm::RectI;

namespace {
const fixtures::Bgr kWhitePiece{245, 245, 245};
const fixtures::Bgr kBlackPiece{15, 15, 15};
}  // namespace

TEST_CASE("test_board_slicing") {
    // Synthetic 800x800 test chessboard.
    const Image img = fixtures::checkerboard(800, 240, 60);

    BoardDetector detector;
    const auto squares = detector.sliceBoard(img, true);

    REQUIRE(squares.size() == 64);
    for (const Image& sq : squares) {
        CHECK(sq.w == 64);
        CHECK(sq.h == 64);
        CHECK(sq.data.size() == 64U * 64U * 3U);
    }
}

TEST_CASE("test_coordinate_mapping") {
    BoardDetector detector;
    const RectI roi{100, 100, 800, 800};

    // In white orientation e4 is file 4, screen row 4 -> 100 + 4.5*100 on both axes.
    const auto e4 = detector.squareScreenCenter(fixtures::sq('e', 4), roi, true);
    CHECK(e4.first == 550);
    CHECK(e4.second == 550);

    const auto a1 = detector.squareScreenCenter(fixtures::sq('a', 1), roi, true);
    CHECK(a1.first == 150);
    CHECK(a1.second == 850);
}

TEST_CASE("test_black_orientation") {
    BoardDetector detector;
    cm::GameTracker tracker;
    tracker.resetGame(chess::constants::STARTPOS, false);
    CHECK_FALSE(tracker.orientationWhite());

    // In black orientation e7 maps to file column 3, screen row 6.
    const RectI roi{0, 0, 800, 800};
    const auto e7 = detector.squareScreenCenter(fixtures::sq('e', 7), roi, false);
    CHECK(e7.first == 350);
    CHECK(e7.second == 650);
}

TEST_CASE("test_visual_orientation_detection_for_both_colors") {
    BoardDetector detector;
    const Image whiteBottom = fixtures::renderPosition(chess::Board(), kWhitePiece, kBlackPiece);
    const Image blackBottom = fixtures::rotate180(whiteBottom);

    CHECK(detector.detectOrientation(whiteBottom));
    CHECK_FALSE(detector.detectOrientation(blackBottom));
}
