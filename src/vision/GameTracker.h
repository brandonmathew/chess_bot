#pragma once
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "chess.hpp"
#include "core/Image.h"

namespace cm {

// Per-square occupancy labels
enum Occupancy : int {
    EMPTY = 0,
    WHITE_PIECE = 1,
    BLACK_PIECE = 2,
};

// Move detection pipeline (identical strategy to the original Python tracker):
//   1. HIGHLIGHT DETECTION - the yellow/green/blue squares the site paints on
//      the board after every move.
//   2. OCCUPANCY CHANGE - departure/arrival squares vs. a stored baseline.
//   3. FIRST-FRAME CATCH-UP - commit a still-visible last-move highlight so
//      tracking can start mid-game.
//   4. DESYNC DETECTION - compare on-screen piece counts with the internal
//      board and fire a callback when they diverge.
class GameTracker {
   public:
    explicit GameTracker(std::string initialFen = chess::constants::STARTPOS);

    // ── Public API ──────────────────────────────────────────────────────────
    void resetGame(const std::string& fen = chess::constants::STARTPOS, bool orientationWhite = true);
    void setOrientation(bool orientationWhite);
    void initializeBaseline(const std::array<Image, 64>& squares);

    // Push a move entered by the user or executed by the bot.
    // Returns false when the move is not legal in the current position.
    bool applyManualMove(const chess::Move& move);

    // Sync to an externally provided FEN (user paste).
    void applyFen(const std::string& fen);

    // Analyse one frame; returns a confirmed move (debounced) or nullopt.
    std::optional<chess::Move> processFrame(const Image& boardImg,
                                            const std::array<Image, 64>& squares);

    // ── State ───────────────────────────────────────────────────────────────
    chess::Board& board() { return board_; }
    const chess::Board& board() const { return board_; }
    std::string boardFen() const { return board_.getFen(); }
    bool orientationWhite() const { return orientationWhite_; }
    const std::vector<chess::Move>& moveHistory() const { return moveHistory_; }
    std::vector<chess::Move>& moveHistory() { return moveHistory_; }

    // ── Exposed for tests / diagnostics (Python "_-" internals) ─────────────
    int classify(const Image& squareCrop) const;
    bool isStartingBoard(const std::array<Image, 64>& squares) const;
    std::optional<chess::Move> detectFromHighlights(const Image& boardImg) const;
    std::optional<chess::Move> detectFromOccupancy(const std::array<Image, 64>& squares) const;
    const std::vector<int>& baselineOcc() const { return baselineOcc_; }
    double colorThreshold() const { return colorThreshold_; }
    bool colorCalibrated() const { return colorCalibrated_; }

    // Delivers the desync warning exactly once (the python on_desync callback
    // equivalent, minus the cross-thread callback hazards).
    std::optional<std::string> takeDesync();

   private:
    std::optional<chess::Move> matchHighlightsToMove(const std::vector<int>& highlighted) const;
    void checkDesync(const std::array<Image, 64>& squares);
    std::optional<double> pieceBrightness(const Image& sqCrop) const;
    void calibratePieceColors(const std::array<Image, 64>& squares);
    std::vector<int> boardOccupancy() const;
    void commitMove(const chess::Move& move, const std::array<Image, 64>& squares);

    chess::Board board_;
    bool orientationWhite_ = true;
    std::vector<chess::Move> moveHistory_;

    std::vector<int> baselineOcc_;
    double colorThreshold_ = 100.0;
    bool colorCalibrated_ = false;

    std::optional<chess::Move> pendingMove_;
    int pendingCount_ = 0;
    int requiredConf_ = 2;  // frames a move must stay stable

    bool desyncWarned_ = false;
    int desyncCount_ = 0;
    int frameCount_ = 0;
    std::optional<std::string> desyncMessage_;
};

}  // namespace cm
