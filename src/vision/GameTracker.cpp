#include "vision/GameTracker.h"

#include <algorithm>
#include <cmath>

#include "core/CvOps.h"
#include "core/Log.h"

namespace cm {

// ── Lichess / Chess.com highlight HSV ranges (ported constants) ─────────────
static const int kYellowLo[3] = {15, 55, 70};
static const int kYellowHi[3] = {48, 255, 255};
static const int kGreenLo[3] = {55, 45, 55};
static const int kGreenHi[3] = {100, 255, 255};
static const int kBlueLo[3] = {100, 45, 55};
static const int kBlueHi[3] = {135, 255, 255};

// Our own neon overlay colour, stripped before colours are read as highlights.
static const int kOverlayGreenLo[3] = {40, 180, 180};
static const int kOverlayGreenHi[3] = {85, 255, 255};
static const int kOverlayMaskLo[3] = {35, 100, 100};
static const int kOverlayMaskHi[3] = {95, 255, 255};

// Minimum fraction of a square's pixels that must match a highlight colour.
static constexpr double kMinHighlightFrac = 0.15;
// How far off the piece count can be before we call it a desync.
static constexpr int kDesyncPieceDelta = 3;

static Hsv cropHsv(const Hsv& src, int x, int y, int w, int h) {
    Hsv out;
    out.h = src.h.crop(x, y, w, h);
    out.s = src.s.crop(x, y, w, h);
    out.v = src.v.crop(x, y, w, h);
    return out;
}

static chess::Movelist legalMovesOf(const chess::Board& board) {
    chess::Movelist moves;
    chess::movegen::legalmoves(moves, board);
    return moves;
}

static bool isLegal(const chess::Board& board, const chess::Move& move) {
    const chess::Movelist moves = legalMovesOf(board);
    for (const chess::Move& m : moves) {
        if (m == move) return true;
    }
    return false;
}

static std::string uciOf(const chess::Move& move) { return chess::uci::moveToUci(move); }

// Squares the rook travels during castling (standard chess only, matching the
// hard-coded map in the original implementation).
static void castlingRookSquares(const chess::Move& move, int& rookFrom, int& rookTo) {
    rookFrom = rookTo = -1;
    if (move.typeOf() != chess::Move::CASTLING) return;
    switch (move.to()) {
        case 6: rookFrom = 7; rookTo = 5; break;    // g1: h1 -> f1
        case 2: rookFrom = 0; rookTo = 3; break;    // c1: a1 -> d1
        case 62: rookFrom = 63; rookTo = 61; break;  // g8: h8 -> f8
        case 58: rookFrom = 56; rookTo = 59; break;  // c8: a8 -> d8
        default: break;
    }
}

GameTracker::GameTracker(std::string initialFen) : board_(initialFen) {}

void GameTracker::resetGame(const std::string& fen, bool orientationWhite) {
    chess::Board fresh;
    if (fresh.setFen(fen)) board_ = std::move(fresh);
    orientationWhite_ = orientationWhite;
    moveHistory_.clear();
    baselineOcc_.clear();
    colorThreshold_ = 100.0;
    colorCalibrated_ = false;
    pendingMove_.reset();
    pendingCount_ = 0;
    desyncWarned_ = false;
    desyncCount_ = 0;
    frameCount_ = 0;
}

void GameTracker::setOrientation(bool orientationWhite) {
    orientationWhite_ = orientationWhite;
    baselineOcc_.clear();
}

void GameTracker::initializeBaseline(const std::array<Image, 64>& squares) {
    if (!colorCalibrated_) calibratePieceColors(squares);
    baselineOcc_.resize(64);
    for (int i = 0; i < 64; ++i) baselineOcc_[static_cast<std::size_t>(i)] = classify(squares[static_cast<std::size_t>(i)]);
    pendingMove_.reset();
    pendingCount_ = 0;
}

bool GameTracker::applyManualMove(const chess::Move& move) {
    if (!isLegal(board_, move)) return false;
    board_.makeMove(move);
    moveHistory_.push_back(move);
    // Install the expected post-move occupancy immediately: fast opponents may
    // reply before the next capture, and re-snapshotting their reply as a fresh
    // baseline would silently lose that move and stall.
    baselineOcc_ = boardOccupancy();
    pendingMove_.reset();
    pendingCount_ = 0;
    desyncWarned_ = false;
    desyncCount_ = 0;
    return true;
}

void GameTracker::applyFen(const std::string& fen) {
    // Parse into a scratch board first so a bad FEN leaves the position intact
    // (the Python version raised before assigning, keeping the old board).
    chess::Board fresh;
    if (!fen.empty() && fresh.setFen(fen)) {
        board_ = std::move(fresh);
        baselineOcc_.clear();
        pendingMove_.reset();
        pendingCount_ = 0;
        desyncWarned_ = false;
        desyncCount_ = 0;
        moveHistory_.clear();
        log::print("[Tracker] FEN synced: %.60s", fen.c_str());
    } else {
        log::print("[Tracker] Bad FEN: %.80s", fen.c_str());
    }
}

std::optional<chess::Move> GameTracker::processFrame(const Image& boardImg,
                                                     const std::array<Image, 64>& squares) {
    if (squares.size() != 64 || boardImg.empty()) return std::nullopt;

    ++frameCount_;

    // ── Auto-detect new game ────────────────────────────────────────────────
    if (!moveHistory_.empty() && isStartingBoard(squares)) {
        log::print("[Tracker] New game detected - resetting board state.");
        resetGame(chess::constants::STARTPOS, orientationWhite_);
        initializeBaseline(squares);
        return std::nullopt;
    }

    // ── First frame: try highlight catch-up BEFORE storing the baseline ─────
    if (baselineOcc_.empty()) {
        const auto firstMove = detectFromHighlights(boardImg);
        if (firstMove) {
            log::print("[Tracker] First-frame highlight catch-up: %s", uciOf(*firstMove).c_str());
            commitMove(*firstMove, squares);
            return firstMove;
        }
        initializeBaseline(squares);
        checkDesync(squares);
        return std::nullopt;
    }

    // ── 1. Highlight-based detection ────────────────────────────────────────
    std::optional<chess::Move> candidate = detectFromHighlights(boardImg);

    // ── 2. Occupancy-change fallback ────────────────────────────────────────
    if (!candidate) candidate = detectFromOccupancy(squares);

    // ── 3. Debounce ─────────────────────────────────────────────────────────
    if (!candidate) {
        pendingMove_.reset();
        pendingCount_ = 0;
        if (frameCount_ % 30 == 0) checkDesync(squares);  // ~2.5 s at 12 fps
        return std::nullopt;
    }

    if (pendingMove_ && *pendingMove_ == *candidate) {
        ++pendingCount_;
    } else {
        pendingMove_ = *candidate;
        pendingCount_ = 1;
    }

    if (pendingCount_ >= requiredConf_) {
        commitMove(*candidate, squares);
        return candidate;
    }
    return std::nullopt;
}

// ── Detection helpers ───────────────────────────────────────────────────────

std::optional<chess::Move> GameTracker::detectFromHighlights(const Image& boardImg) const {
    const Hsv hsv = cv::bgr2hsv(boardImg);

    const double sqH = boardImg.h / 8.0;
    const double sqW = boardImg.w / 8.0;

    std::vector<int> highlighted;

    for (int rankIdx = 0; rankIdx < 8; ++rankIdx) {
        for (int fileIdx = 0; fileIdx < 8; ++fileIdx) {
            const int y1 = static_cast<int>(rankIdx * sqH);
            const int y2 = static_cast<int>((rankIdx + 1) * sqH);
            const int x1 = static_cast<int>(fileIdx * sqW);
            const int x2 = static_cast<int>((fileIdx + 1) * sqW);
            if (y2 <= y1 || x2 <= x1) continue;

            const Hsv patch = cropHsv(hsv, x1, y1, x2 - x1, y2 - y1);
            if (patch.v.empty()) continue;

            Plane maskY = cv::inRange(patch, kYellowLo, kYellowHi);
            Plane maskG = cv::inRange(patch, kGreenLo, kGreenHi);
            Plane maskB = cv::inRange(patch, kBlueLo, kBlueHi);
            cv::bitwiseOrInPlace(maskY, maskG);
            cv::bitwiseOrInPlace(maskY, maskB);
            Plane combined = maskY;

            // mss/GDI captures include our own overlay. Remove its neon green
            // before interpreting colours as site highlights, otherwise the
            // engine suggestion can be committed as a real chess move.
            const Plane overlayGreen = cv::inRange(patch, kOverlayGreenLo, kOverlayGreenHi);
            cv::clearWhere(combined, overlayGreen);

            const double frac = cv::fracNonzero(combined);
            if (frac >= kMinHighlightFrac) {
                const int chessRank = orientationWhite_ ? (7 - rankIdx) : rankIdx;
                const int chessFile = orientationWhite_ ? fileIdx : (7 - fileIdx);
                highlighted.push_back(chessRank * 8 + chessFile);
            }
        }
    }

    if (highlighted.size() < 2) return std::nullopt;
    return matchHighlightsToMove(highlighted);
}

std::optional<chess::Move> GameTracker::matchHighlightsToMove(const std::vector<int>& highlighted) const {
    const chess::Movelist legal = legalMovesOf(board_);

    std::array<bool, 64> hl{};
    for (const int sq : highlighted) hl[static_cast<std::size_t>(sq)] = true;
    const int hlCount = static_cast<int>(highlighted.size());

    chess::Move bestMove;
    bool haveBest = false;
    int bestScore = -1;

    for (const chess::Move& move : legal) {
        std::array<bool, 64> involved{};
        involved[static_cast<std::size_t>(move.from())] = true;
        involved[static_cast<std::size_t>(move.to())] = true;
        int rookFrom = -1, rookTo = -1;
        castlingRookSquares(move, rookFrom, rookTo);
        if (rookFrom >= 0) {
            involved[static_cast<std::size_t>(rookFrom)] = true;
            involved[static_cast<std::size_t>(rookTo)] = true;
        }

        int hits = 0;
        for (int sq = 0; sq < 64; ++sq) {
            if (hl[static_cast<std::size_t>(sq)] && involved[static_cast<std::size_t>(sq)]) ++hits;
        }
        const int extra = hlCount - hits;  // highlighted squares not explained
        const int score = hits * 2 - extra;

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;
            haveBest = true;
        }
    }

    // Require both from_sq and to_sq to be highlighted.
    if (haveBest && hl[static_cast<std::size_t>(bestMove.from())] && hl[static_cast<std::size_t>(bestMove.to())]) {
        return bestMove;
    }
    return std::nullopt;
}

std::optional<chess::Move> GameTracker::detectFromOccupancy(
    const std::array<Image, 64>& squares) const {
    if (baselineOcc_.size() != 64) return std::nullopt;

    std::array<int, 64> curr{};
    for (int i = 0; i < 64; ++i) curr[static_cast<std::size_t>(i)] = classify(squares[static_cast<std::size_t>(i)]);

    std::array<bool, 64> dep{}, arr{};
    int depCount = 0, arrCount = 0;
    for (int i = 0; i < 64; ++i) {
        const int prev = baselineOcc_[static_cast<std::size_t>(i)];
        const int cur = curr[static_cast<std::size_t>(i)];
        if (prev != EMPTY && cur == EMPTY) {
            dep[static_cast<std::size_t>(i)] = true;
            ++depCount;
        } else if (cur != EMPTY && cur != prev) {
            // Empty -> occupied is a normal arrival; occupied -> opposite
            // colour is a capture on the destination square.
            arr[static_cast<std::size_t>(i)] = true;
            ++arrCount;
        }
    }
    if (depCount == 0 || arrCount == 0) return std::nullopt;

    const chess::Movelist legal = legalMovesOf(board_);

    chess::Move bestMove;
    bool haveBest = false;
    int bestScore = -1;

    for (const chess::Move& move : legal) {
        int score = 0;
        if (dep[static_cast<std::size_t>(move.from())]) score += 2;
        if (arr[static_cast<std::size_t>(move.to())]) score += 2;

        int rookFrom = -1, rookTo = -1;
        castlingRookSquares(move, rookFrom, rookTo);
        if (rookFrom >= 0) {
            if (dep[static_cast<std::size_t>(rookFrom)]) score += 1;
            if (arr[static_cast<std::size_t>(rookTo)]) score += 1;
        }

        // Penalise unexplained changes.
        std::array<bool, 64> involvedDep{}, involvedArr{};
        involvedDep[static_cast<std::size_t>(move.from())] = true;
        involvedArr[static_cast<std::size_t>(move.to())] = true;
        if (rookFrom >= 0) {
            involvedDep[static_cast<std::size_t>(rookFrom)] = true;
            involvedArr[static_cast<std::size_t>(rookTo)] = true;
        }
        int extra = 0;
        for (int sq = 0; sq < 64; ++sq) {
            if (dep[static_cast<std::size_t>(sq)] && !involvedDep[static_cast<std::size_t>(sq)]) ++extra;
            if (arr[static_cast<std::size_t>(sq)] && !involvedArr[static_cast<std::size_t>(sq)]) ++extra;
        }
        score -= extra;

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;
            haveBest = true;
        }
    }

    if (haveBest && dep[static_cast<std::size_t>(bestMove.from())] &&
        arr[static_cast<std::size_t>(bestMove.to())] && bestScore >= 3) {
        return bestMove;
    }
    return std::nullopt;
}

// ── Desync detection ────────────────────────────────────────────────────────

void GameTracker::checkDesync(const std::array<Image, 64>& squares) {
    if (desyncWarned_) return;

    int screenWhite = 0, screenBlack = 0;
    std::array<bool, 64> screenOcc{};
    for (int i = 0; i < 64; ++i) {
        const int occ = classify(squares[static_cast<std::size_t>(i)]);
        if (occ == WHITE_PIECE) ++screenWhite;
        if (occ == BLACK_PIECE) ++screenBlack;
        screenOcc[static_cast<std::size_t>(i)] = (occ != EMPTY);
    }
    const int screenTotal = screenWhite + screenBlack;

    int boardTotal = 0;
    int positionMismatch = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const bool onBoard = board_.at(chess::Square(sq)) != chess::Piece::NONE;
        boardTotal += onBoard ? 1 : 0;
        if (onBoard != screenOcc[static_cast<std::size_t>(sq)]) ++positionMismatch;
    }

    const int delta = std::abs(screenTotal - boardTotal);
    const bool mismatch = delta > kDesyncPieceDelta || positionMismatch > 6;
    if (!mismatch) {
        desyncCount_ = 0;
        return;
    }

    // Screenshot tools and move animations can obscure the board briefly:
    // require repeated mismatches before showing a persistent warning.
    if (++desyncCount_ >= 3) {
        std::string msg = "Screen shows ~" + std::to_string(screenTotal) + " pieces but tracker expects " +
                          std::to_string(boardTotal) + " (" + std::to_string(positionMismatch) +
                          " squares differ). Paste FEN from Lichess to sync position.";
        log::print("[Tracker] DESYNC: %s", msg.c_str());
        desyncWarned_ = true;
        desyncMessage_ = msg;
    }
}

std::optional<std::string> GameTracker::takeDesync() {
    std::optional<std::string> msg = std::move(desyncMessage_);
    desyncMessage_.reset();
    return msg;
}

// ── Occupancy classification ────────────────────────────────────────────────

int GameTracker::classify(const Image& squareCrop) const {
    const auto brightness = pieceBrightness(squareCrop);
    if (!brightness) return EMPTY;
    return (*brightness >= colorThreshold_) ? WHITE_PIECE : BLACK_PIECE;
}

std::optional<double> GameTracker::pieceBrightness(const Image& sqCrop) const {
    if (sqCrop.empty()) return std::nullopt;
    const int h = sqCrop.h, w = sqCrop.w;
    if (h < 8 || w < 8) return std::nullopt;

    const Plane gray = cv::bgr2gray(sqCrop);
    const Hsv hsv = cv::bgr2hsv(sqCrop);

    // Anti-aliased pixels around arrow/circle edges are included, so the
    // overlay itself is masked out instead of discarding the whole square.
    Plane overlayMask = cv::inRange(hsv, kOverlayMaskLo, kOverlayMaskHi);
    overlayMask = cv::dilateRect(overlayMask, 3, 3);

    const double meanBright = cv::meanPlane(gray);

    // Synthetic fixtures and flat piece sets can be completely uniform:
    // preserve the unambiguous extreme cases.
    if (cv::stddevU8(gray.d.data(), gray.d.size()) < 1.0) {
        if (meanBright > 252.0) return meanBright;
        if (meanBright < 35.0) return meanBright;
        return std::nullopt;
    }

    // Estimate this square's own background from its corners: adapts to
    // light/dark squares, last-move highlights and different themes.
    const int cornerSize = std::max(2, static_cast<int>(0.10 * std::min(h, w)));
    std::vector<std::uint8_t> corners;
    corners.reserve(static_cast<std::size_t>(cornerSize) * cornerSize * 4);
    for (int y = 0; y < cornerSize; ++y) {
        for (int x = 0; x < cornerSize; ++x) {
            corners.push_back(gray.at(x, y));
            corners.push_back(gray.at(w - 1 - x, y));
            corners.push_back(gray.at(x, h - 1 - y));
            corners.push_back(gray.at(w - 1 - x, h - 1 - y));
        }
    }
    const double backgroundMean = cv::medianU8(std::move(corners));

    // Ignore labels near square edges and drop overlay pixels from the
    // foreground mask instead of discarding the whole covered square.
    const int marginH = std::max(2, static_cast<int>(0.15 * h));
    const int marginW = std::max(2, static_cast<int>(0.15 * w));
    const int y1 = marginH, y2 = h - marginH;
    const int x1 = marginW, x2 = w - marginW;
    if (y2 <= y1 || x2 <= x1) return std::nullopt;

    double sum = 0.0;
    std::size_t count = 0, total = 0;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            ++total;
            if (overlayMask.at(x, y)) continue;
            if (std::abs(static_cast<double>(gray.at(x, y)) - backgroundMean) > 18.0) {
                sum += gray.at(x, y);
                ++count;
            }
        }
    }
    if (total == 0) return std::nullopt;
    if (static_cast<double>(count) / static_cast<double>(total) < 0.04) return std::nullopt;
    return sum / static_cast<double>(count);
}

void GameTracker::calibratePieceColors(const std::array<Image, 64>& squares) {
    std::vector<std::optional<double>> values(64);
    for (int i = 0; i < 64; ++i) {
        values[static_cast<std::size_t>(i)] = pieceBrightness(squares[static_cast<std::size_t>(i)]);
    }

    // Prefer supervised calibration from the authoritative chess position:
    // handles themes where the same colour looks brighter/darker on the two
    // square colours.
    std::vector<double> expectedDark, expectedLight;
    for (int sq = 0; sq < 64; ++sq) {
        const auto& value = values[static_cast<std::size_t>(sq)];
        if (!value) continue;
        const chess::Piece piece = board_.at(chess::Square(sq));
        if (piece == chess::Piece::NONE) continue;
        (piece.color() == chess::Color::WHITE ? expectedLight : expectedDark).push_back(*value);
    }

    if (expectedDark.size() >= 4 && expectedLight.size() >= 4) {
        const double darkCenter = cv::medianD(expectedDark);
        const double lightCenter = cv::medianD(expectedLight);
        if (lightCenter - darkCenter >= 20.0) {
            colorThreshold_ = (darkCenter + lightCenter) / 2.0;
            colorCalibrated_ = true;
            log::print("[Vision] Piece colors calibrated: dark=%.1f, light=%.1f, split=%.1f", darkCenter,
                       lightCenter, colorThreshold_);
            return;
        }
    }

    // Fallback for an unknown position: separate foregrounds into two groups.
    std::vector<float> samples;
    samples.reserve(64);
    for (const auto& value : values) {
        if (value) samples.push_back(static_cast<float>(*value));
    }
    if (samples.size() < 8) return;

    double low = cv::percentileF(samples, 20.0);
    double high = cv::percentileF(samples, 80.0);
    for (int iter = 0; iter < 12; ++iter) {
        std::vector<float> lowGroup, highGroup;
        for (const float v : samples) {
            (std::fabs(v - static_cast<float>(low)) <= std::fabs(v - static_cast<float>(high)) ? lowGroup : highGroup)
                .push_back(v);
        }
        if (lowGroup.size() < 4 || highGroup.size() < 4) return;
        double newLow = 0.0, newHigh = 0.0;
        for (const float v : lowGroup) newLow += v;
        newLow /= static_cast<double>(lowGroup.size());
        for (const float v : highGroup) newHigh += v;
        newHigh /= static_cast<double>(highGroup.size());
        const bool converged = std::fabs(newLow - low) < 0.05 && std::fabs(newHigh - high) < 0.05;
        low = newLow;
        high = newHigh;
        if (converged) break;
    }

    if (high - low < 20.0) return;

    colorThreshold_ = (low + high) / 2.0;
    colorCalibrated_ = true;
    log::print("[Vision] Piece colors calibrated: dark=%.1f, light=%.1f, split=%.1f", low, high, colorThreshold_);
}

bool GameTracker::isStartingBoard(const std::array<Image, 64>& squares) const {
    if (squares.size() != 64) return false;
    std::array<int, 64> occ{};
    for (int i = 0; i < 64; ++i) occ[static_cast<std::size_t>(i)] = classify(squares[static_cast<std::size_t>(i)]);

    int whiteHome = 0, blackHome = 0, middleOccupied = 0;
    for (int i = 0; i < 16; ++i) whiteHome += (occ[static_cast<std::size_t>(i)] == WHITE_PIECE);
    for (int i = 48; i < 64; ++i) blackHome += (occ[static_cast<std::size_t>(i)] == BLACK_PIECE);
    for (int i = 16; i < 48; ++i) middleOccupied += (occ[static_cast<std::size_t>(i)] != EMPTY);

    // A genuine reset has no developed piece on ranks 3-6 and exactly the
    // 32-piece starting occupancy: accepting transient drag frames (15 home
    // pieces) used to cause destructive mid-game resets.
    return whiteHome == 16 && blackHome == 16 && middleOccupied == 0;
}

std::vector<int> GameTracker::boardOccupancy() const {
    std::vector<int> occupancy(64, EMPTY);
    for (int sq = 0; sq < 64; ++sq) {
        const chess::Piece piece = board_.at(chess::Square(sq));
        if (piece == chess::Piece::NONE) continue;
        occupancy[static_cast<std::size_t>(sq)] = (piece.color() == chess::Color::WHITE) ? WHITE_PIECE : BLACK_PIECE;
    }
    return occupancy;
}

// ── Commit ──────────────────────────────────────────────────────────────────

void GameTracker::commitMove(const chess::Move& move, const std::array<Image, 64>& squares) {
    board_.makeMove(move);
    moveHistory_.push_back(move);
    desyncWarned_ = false;
    desyncCount_ = 0;
    initializeBaseline(squares);
    const char* side = (board_.sideToMove() == chess::Color::WHITE) ? "White" : "Black";
    log::print("[Tracker] Committed: %s  (%s to move next)", uciOf(move).c_str(), side);
}

}  // namespace cm
