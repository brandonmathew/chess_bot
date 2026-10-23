// Ported from tests/test_chess_vision.py (tracker case) and
// tests/test_advanced_tracking.py (all tracking cases).
#include <string>
#include <vector>

#include "chess.hpp"
#include "core/Image.h"
#include "doctest.h"
#include "fixtures.h"
#include "vision/BoardDetector.h"
#include "vision/GameTracker.h"

using cm::BoardDetector;
using cm::GameTracker;
using cm::Image;
using fixtures::sq;

namespace {
const fixtures::Bgr kWhitePiece{245, 245, 245};
const fixtures::Bgr kBlackPiece{15, 15, 15};

std::string uciOf(const chess::Move& move) { return chess::uci::moveToUci(move); }
}  // namespace

TEST_CASE("test_game_tracker_move_detection") {
    // Initial synthetic board (100px squares, light/dark checkerboard).
    Image boardImg1 = fixtures::checkerboard(800, 240, 50);

    // Add a piece on e2 (screen row 6, screen col 4).
    fixtures::setRect(boardImg1, 400, 600, 100, 100, 255, 255, 255);

    BoardDetector detector;
    const auto squares1 = detector.sliceBoard(boardImg1, true);

    GameTracker tracker;
    tracker.resetGame(chess::constants::STARTPOS, true);
    tracker.initializeBaseline(squares1);

    // Move e2-e4: piece removed from e2, placed on e4.
    Image boardImg2 = boardImg1.clone();
    fixtures::setRect(boardImg2, 400, 600, 100, 100, 240, 240, 240);  // e2 empty
    fixtures::setRect(boardImg2, 400, 400, 100, 100, 255, 255, 255);  // e4 piece

    const auto squares2 = detector.sliceBoard(boardImg2, true);

    // No highlight colours on this board, so the occupancy-change path runs.
    tracker.processFrame(boardImg2, squares2);
    const auto move = tracker.processFrame(boardImg2, squares2);

    REQUIRE(move.has_value());
    CHECK(uciOf(*move) == "e2e4");
    CHECK(tracker.board().sideToMove() == chess::Color::BLACK);
}

TEST_CASE("test_castling_detection") {
    GameTracker tracker;
    for (const std::string& m : {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"})
        fixtures::pushUci(tracker.board(), m);

    // Baseline: white king on e1, rook on h1 (everything else empty).
    auto base = fixtures::solidSquares(100);
    fixtures::paint(base, sq('e', 1), 255);
    fixtures::paint(base, sq('h', 1), 255);
    tracker.initializeBaseline(base);

    // After O-O: king on g1, rook on f1, e1/h1 empty.
    auto after = base;
    fixtures::paint(after, sq('e', 1), 100);
    fixtures::paint(after, sq('h', 1), 100);
    fixtures::paint(after, sq('g', 1), 255);
    fixtures::paint(after, sq('f', 1), 255);

    const Image plain = fixtures::checkerboard(512, 190, 55);
    tracker.processFrame(plain, after);
    const auto detected = tracker.processFrame(plain, after);

    REQUIRE(detected.has_value());
    CHECK(uciOf(*detected) == "e1g1");
    CHECK(tracker.board().sideToMove() == chess::Color::BLACK);
}

TEST_CASE("test_en_passant_detection") {
    GameTracker tracker;
    tracker.resetGame("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR b KQkq - 0 2", true);

    auto base = fixtures::solidSquares(100);
    fixtures::paint(base, sq('f', 5), 255);  // white pawn
    fixtures::paint(base, sq('g', 7), 255);  // black pawn
    tracker.initializeBaseline(base);

    const Image plain = fixtures::checkerboard(512, 190, 55);

    // Black plays g7-g5.
    auto g5Board = base;
    fixtures::paint(g5Board, sq('g', 7), 100);
    fixtures::paint(g5Board, sq('g', 5), 255);
    tracker.processFrame(plain, g5Board);
    tracker.processFrame(plain, g5Board);

    REQUIRE(tracker.board().enpassantSq() == chess::Square(sq('g', 6)));

    // White captures en passant: f5-g6 (f5 vacated, g5 pawn removed, g6 occupied).
    auto epBoard = g5Board;
    fixtures::paint(epBoard, sq('f', 5), 100);
    fixtures::paint(epBoard, sq('g', 5), 100);
    fixtures::paint(epBoard, sq('g', 6), 255);

    tracker.processFrame(plain, epBoard);
    const auto detected = tracker.processFrame(plain, epBoard);

    REQUIRE(detected.has_value());
    CHECK(uciOf(*detected) == "f5g6");
}

TEST_CASE("test_capture_onto_occupied_square") {
    // Reproduces the previously stalled sequence: 1. e4 c5 2. d4 cxd4.
    GameTracker tracker;
    tracker.resetGame();
    fixtures::pushUci(tracker.board(), "e2e4");
    fixtures::pushUci(tracker.board(), "c7c5");
    fixtures::pushUci(tracker.board(), "d2d4");

    auto base = fixtures::solidSquares(100);
    fixtures::paint(base, sq('d', 4), 255);  // white pawn before capture
    fixtures::paint(base, sq('c', 5), 20);   // black pawn before capture
    tracker.initializeBaseline(base);

    auto after = base;
    fixtures::paint(after, sq('c', 5), 100);  // c5 now empty
    fixtures::paint(after, sq('d', 4), 20);   // black pawn replaces white pawn

    const Image plain = fixtures::checkerboard(512, 190, 55);
    tracker.processFrame(plain, after);
    const auto detected = tracker.processFrame(plain, after);

    REQUIRE(detected.has_value());
    CHECK(uciOf(*detected) == "c5d4");
    CHECK(tracker.board().sideToMove() == chess::Color::WHITE);
}

TEST_CASE("test_tracks_white_then_black_with_overlay_visible") {
    BoardDetector detector;
    GameTracker tracker;

    chess::Board position;
    const Image initial = fixtures::renderPosition(position, kWhitePiece, kBlackPiece);
    tracker.resetGame();
    tracker.initializeBaseline(detector.sliceBoard(initial, true));

    position.makeMove(chess::uci::uciToMove(position, "e2e4"), false);
    Image afterWhite = fixtures::renderPosition(position, kWhitePiece, kBlackPiece);
    fixtures::drawOverlay(afterWhite, sq('e', 2), sq('e', 4));
    const auto whiteSquares = detector.sliceBoard(afterWhite, true);
    tracker.processFrame(afterWhite, whiteSquares);
    const auto whiteMove = tracker.processFrame(afterWhite, whiteSquares);

    REQUIRE(whiteMove.has_value());
    CHECK(uciOf(*whiteMove) == "e2e4");
    CHECK(tracker.board().sideToMove() == chess::Color::BLACK);

    position.makeMove(chess::uci::uciToMove(position, "d7d5"), false);
    Image afterBlack = fixtures::renderPosition(position, kWhitePiece, kBlackPiece);
    fixtures::drawOverlay(afterBlack, sq('d', 7), sq('d', 5));
    const auto blackSquares = detector.sliceBoard(afterBlack, true);
    tracker.processFrame(afterBlack, blackSquares);
    const auto blackMove = tracker.processFrame(afterBlack, blackSquares);

    REQUIRE(blackMove.has_value());
    CHECK(uciOf(*blackMove) == "d7d5");
    CHECK(tracker.board().sideToMove() == chess::Color::WHITE);
}

TEST_CASE("test_auto_move_baseline_catches_instant_reply") {
    // Auto mode advances the internal board before the opponent's visual
    // reply; the reply must be matched against the expected post-auto-move
    // occupancy instead of being swallowed as a new baseline.
    BoardDetector detector;
    GameTracker tracker;
    tracker.resetGame();

    const chess::Move autoMove = chess::uci::uciToMove(tracker.board(), "e2e4");
    REQUIRE(tracker.applyManualMove(autoMove));

    chess::Board replied;
    fixtures::pushUci(replied, "e2e4");
    fixtures::pushUci(replied, "e7e5");
    const Image replyImage = fixtures::renderPosition(replied, kWhitePiece, kBlackPiece);
    const auto replySquares = detector.sliceBoard(replyImage, true);

    tracker.processFrame(replyImage, replySquares);
    const auto detected = tracker.processFrame(replyImage, replySquares);

    REQUIRE(detected.has_value());
    CHECK(uciOf(*detected) == "e7e5");
    CHECK(tracker.board().sideToMove() == chess::Color::WHITE);
}

TEST_CASE("test_transient_drag_is_not_a_new_game") {
    BoardDetector detector;
    GameTracker tracker;

    chess::Board position;
    fixtures::pushUci(position, "e2e4");
    fixtures::pushUci(position, "e7e5");
    tracker.resetGame(position.getFen());
    tracker.moveHistory().push_back(chess::uci::uciToMove(position, "e7e5"));

    Image transient = fixtures::renderPosition(position, kWhitePiece, kBlackPiece);
    // Simulate a dragged knight temporarily absent from g1.
    fixtures::setRect(transient, 6 * 64, 7 * 64, 64, 64, 181, 136, 88);
    const auto squares = detector.sliceBoard(transient, true);

    CHECK_FALSE(tracker.isStartingBoard(squares));
}

TEST_CASE("test_adapts_to_gray_piece_theme") {
    BoardDetector detector;
    GameTracker tracker;

    const Image themed =
        fixtures::renderPosition(chess::Board(), fixtures::Bgr{240, 240, 240}, fixtures::Bgr{85, 85, 85});
    const auto squares = detector.sliceBoard(themed, true);
    tracker.resetGame();
    tracker.initializeBaseline(squares);

    const auto countOf = [&](int value) {
        int count = 0;
        for (int label : tracker.baselineOcc()) count += (label == value);
        return count;
    };
    CHECK(countOf(cm::WHITE_PIECE) == 16);
    CHECK(countOf(cm::BLACK_PIECE) == 16);
    CHECK(tracker.colorCalibrated());
}

TEST_CASE("test_bright_empty_squares_are_not_pieces") {
    // Light themes can have empty squares well above the old brightness
    // cutoff; they must stay empty for desync counting.
    GameTracker tracker;

    Image emptyLight(64, 64);
    fixtures::fill(emptyLight, 230);
    Image solidWhite(64, 64);
    fixtures::fill(solidWhite, 255);

    CHECK(tracker.classify(emptyLight) == 0);
    CHECK(tracker.classify(solidWhite) == 1);
}

TEST_CASE("test_green_overlay_is_not_a_piece") {
    // Screen capture includes the transparent overlay window; a neon green
    // arrow over an empty square must not count as a piece.
    GameTracker tracker;

    Image boardSquare(64, 64);
    fixtures::setRect(boardSquare, 0, 0, 64, 64, 181, 136, 88);
    fixtures::setRect(boardSquare, 8, 28, 48, 8, 102, 255, 0);  // green overlay bar

    CHECK(tracker.classify(boardSquare) == 0);
}

TEST_CASE("test_green_overlay_is_not_a_move_highlight") {
    // The engine arrow must not be mistaken for the site's last-move
    // highlight and committed as a legal move.
    GameTracker tracker;
    Image board = fixtures::checkerboard(512, 190, 55);
    fixtures::setRect(board, 60, 300, 392, 40, 102, 255, 0);  // green arrow bar

    tracker.resetGame();
    const auto detected = tracker.detectFromHighlights(board);

    CHECK_FALSE(detected.has_value());
}
