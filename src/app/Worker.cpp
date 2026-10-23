#include "app/Worker.h"

#include <algorithm>
#include <random>
#include <sstream>

#include "capture/ScreenCapture.h"
#include "core/Log.h"
#include "engine/EngineDownload.h"

namespace cm {

namespace {

std::mt19937& workerRng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

double workerUniform(double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(workerRng());
}

std::string uciString(const chess::Move& move) { return chess::uci::moveToUci(move); }

std::string joinPv(const std::vector<std::string>& pv) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < pv.size() && i < 6; ++i) {
        if (i) ss << ' ';
        ss << pv[i];
    }
    return ss.str();
}

// python-chess semantics: white/black fix the side, "auto" treats the side at
// the bottom of the board as the player.
bool isOurTurn(bool whiteToMove, const std::string& playerColor, bool orientationWhite) {
    if (playerColor == "white") return whiteToMove;
    if (playerColor == "black") return !whiteToMove;
    return whiteToMove == orientationWhite;
}

}  // namespace

Worker::~Worker() {
    stop();
    engine_.stop();
}

void Worker::start(AppConfig settings, OverlayWindow* overlay) {
    if (running_.load()) stop();

    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        settings_ = std::move(settings);
    }
    settingsEpoch_.fetch_add(1);
    overlay_ = overlay;

    running_.store(true);
    const int generation = generation_.fetch_add(1) + 1;
    thread_ = std::thread([this, generation] {
        AppConfig cfg;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            cfg = settings_;
        }
        threadMain(cfg, overlay_, generation);
    });
}

void Worker::stop() {
    running_.store(false);
    generation_.fetch_add(1);  // invalidate any in-flight analysis/delay
    if (thread_.joinable()) thread_.join();

    std::lock_guard<std::mutex> lock(stateMutex_);
    ui_.running = false;
}

UiSnapshot Worker::snapshot() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    UiSnapshot copy = ui_;
    copy.running = running_.load();
    return copy;
}

bool Worker::orientation() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return tracker_.orientationWhite();
}

void Worker::updateSettings(const AppConfig& settings) {
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        settings_ = settings;
    }
    settingsEpoch_.fetch_add(1);
}

void Worker::clearDesync() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ui_.desync = false;
    ui_.desyncMessage.clear();
}

void Worker::setOrientation(bool orientationWhite) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    tracker_.setOrientation(orientationWhite);
    stateEpoch_.fetch_add(1);
}

void Worker::resetBoard() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const bool orientation = tracker_.orientationWhite();
    tracker_.resetGame(chess::constants::STARTPOS, orientation);
    ui_.turnText = "Turn: White to move";
    ui_.lastMoveText = "Last Move: None";
    ui_.evalText = "Eval: +0.00";
    ui_.bestMoveText = "Best Move: --";
    ui_.desync = false;
    ui_.desyncMessage.clear();
    ui_.gameOver = false;
    stateEpoch_.fetch_add(1);
    if (overlay_) overlay_->clearArrow();
}

bool Worker::syncText(const std::string& text, std::string& error, bool& appliedFen) {
    appliedFen = false;
    std::string trimmed = text;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r'))
        trimmed.pop_back();
    if (trimmed.empty()) {
        error.clear();
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);

    // Full FEN strings contain '/' and are longer than a move token.
    if (trimmed.find('/') != std::string::npos && trimmed.size() > 15) {
        tracker_.applyFen(trimmed);
        appliedFen = true;
        ui_.turnText = std::string("Turn: ") +
                       (tracker_.board().sideToMove() == chess::Color::WHITE ? "White" : "Black") + " to move";
        ui_.lastMoveText = "Position Synced from FEN";
        ui_.desync = false;
        ui_.desyncMessage.clear();
        ui_.evalText = "Eval: +0.00";
        ui_.bestMoveText = "Best Move: --";
        ui_.gameOver = false;
        stateEpoch_.fetch_add(1);
        if (overlay_) overlay_->clearArrow();
        return true;
    }

    // UCI (e2e4 / e7e8q) first, SAN fallback (Nf6, O-O, e6).
    chess::Move move;
    bool haveMove = false;
    try {
        chess::Board tmp;
        tmp.setFen(tracker_.boardFen());
        std::string uci = trimmed;
        std::transform(uci.begin(), uci.end(), uci.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        chess::Move parsed = chess::uci::uciToMove(tmp, uci);
        if (parsed != chess::Move{}) {
            move = parsed;
            haveMove = true;
        }
    } catch (...) {
        haveMove = false;
    }

    if (!haveMove) {
        try {
            chess::Move parsed = chess::uci::parseSan(tracker_.board(), trimmed);
            if (parsed != chess::Move{}) {
                move = parsed;
                haveMove = true;
            }
        } catch (...) {
            haveMove = false;
        }
    }

    if (!haveMove) {
        error = "Could not parse move or FEN '" + trimmed + "'";
        return false;
    }

    if (!tracker_.applyManualMove(move)) {
        error = "'" + trimmed + "' is not a legal move in the current position.";
        return false;
    }

    ui_.lastMoveText = "Last Move: " + uciString(move);
    ui_.turnText = std::string("Turn: ") +
                   (tracker_.board().sideToMove() == chess::Color::WHITE ? "White" : "Black") + " to move";
    ui_.desync = false;
    ui_.desyncMessage.clear();
    ui_.gameOver = false;
    stateEpoch_.fetch_add(1);
    if (overlay_) overlay_->clearArrow();
    return true;
}

bool Worker::sleepInterruptible(int milliseconds, int generation) const {
    constexpr int kStep = 40;
    int elapsed = 0;
    while (elapsed < milliseconds) {
        if (!running_.load() || generation_.load() != generation) return false;
        const int chunk = std::min(kStep, milliseconds - elapsed);
        Sleep(static_cast<DWORD>(chunk));
        elapsed += chunk;
    }
    return running_.load() && generation_.load() == generation;
}

void Worker::setEngineStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ui_.engineStatus = status;
}

// ─────────────────────────────────────────────────────────────────────────────

void Worker::threadMain(AppConfig settings, OverlayWindow* overlay, int generation) {
    ScreenCapture capture;
    BoardDetector detector;
    MoveExecutor executor(detector);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ui_.running = true;
    }

    // ── Engine bootstrap (python: EngineController.start inside the loop) ────
    std::string exePath = EngineUci::findEngine(settings.stockfish_path);
    if (exePath.empty() && running_.load() && generation_.load() == generation) {
        exePath = EngineDownload::downloadStockfish(
            [](const std::string& msg) { log::print("[Engine] %s", msg.c_str()); });
    }
    if (exePath.empty()) {
        log::print("[Engine] Engine start error: Failed to download Stockfish automatically. Place "
                   "stockfish.exe in the 'engine' folder.");
        setEngineStatus("Engine: unavailable");
    } else {
        std::string error;
        if (engine_.start(exePath, settings.engine_skill_level, &error)) {
            setEngineStatus("Engine: Stockfish ready (skill " + std::to_string(settings.engine_skill_level) +
                            "/20)");
        } else {
            log::print("[Engine] Engine start error: %s", error.c_str());
            setEngineStatus("Engine: failed to start");
        }
    }
    int appliedSkill = settings.engine_skill_level;

    // ── Step 1: capture a clean baseline BEFORE any overlay arrow exists ─────
    if (settings.hasRoi()) {
        const Image clean = capture.grabRegion(settings.roiRect());
        if (!clean.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto squares = detector.sliceBoard(clean, tracker_.orientationWhite());
            tracker_.initializeBaseline(squares);
        }
    }

    std::string analyzedFen;  // position we have already drawn/auto-played
    DWORD lastEngineAttempt = GetTickCount() - 60000;  // allow a first try at once
    int seenStateEpoch = stateEpoch_.load();
    int seenSettingsEpoch = settingsEpoch_.load();

    while (running_.load() && generation_.load() == generation) {
        AppConfig cfg;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            cfg = settings_;
        }

        // Settings/state changed behind our back -> re-analyse.
        if (settingsEpoch_.load() != seenSettingsEpoch || stateEpoch_.load() != seenStateEpoch) {
            seenSettingsEpoch = settingsEpoch_.load();
            seenStateEpoch = stateEpoch_.load();
            analyzedFen.clear();
        }

        // Skill slider takes effect on the live engine (worker thread only).
        if (engine_.running() && cfg.engine_skill_level != appliedSkill) {
            engine_.setSkillLevel(cfg.engine_skill_level);
            appliedSkill = cfg.engine_skill_level;
            setEngineStatus("Engine: Stockfish ready (skill " + std::to_string(appliedSkill) + "/20)");
        }

        if (!cfg.hasRoi()) {
            sleepInterruptible(250, generation);
            continue;
        }

        const RectI roi = cfg.roiRect();
        const Image boardImg = capture.grabRegion(roi);
        if (boardImg.empty()) {
            sleepInterruptible(120, generation);
            continue;
        }

        // ── Track: slice + process under the state lock ──────────────────────
        std::optional<chess::Move> detected;
        bool whiteToMove = true, gameOver = false, ourTurn = false, orientationWhite = true;
        std::string fen;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            orientationWhite = tracker_.orientationWhite();
            const auto squares = detector.sliceBoard(boardImg, orientationWhite);
            detected = tracker_.processFrame(boardImg, squares);

            if (const auto desyncMsg = tracker_.takeDesync()) {
                ui_.desync = true;
                ui_.desyncMessage = *desyncMsg;
            }

            whiteToMove = tracker_.board().sideToMove() == chess::Color::WHITE;
            gameOver = tracker_.board().isGameOver().first != chess::GameResultReason::NONE;
            fen = tracker_.boardFen();
            ourTurn = isOurTurn(whiteToMove, cfg.player_color, orientationWhite);

            if (detected) {
                log::print("[Vision] Move detected: %s", uciString(*detected).c_str());
                ui_.lastMoveText = "Last Move: " + uciString(*detected);
                ui_.turnText =
                    std::string("Turn: ") + (whiteToMove ? "White" : "Black") + " to move";
            }
        }

        if (detected) analyzedFen.clear();

        // ── Game over ────────────────────────────────────────────────────────
        if (gameOver) {
            if (analyzedFen != fen) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ui_.evalText = "Game Over!";
                ui_.gameOver = true;
                analyzedFen = fen;
                if (overlay) overlay->clearArrow();
            }
            sleepInterruptible(200, generation);
            continue;
        }

        // ── Opponent's turn: wait (python `_set_opponent_waiting_ui`) ────────
        if (!ourTurn) {
            if (analyzedFen != fen) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ui_.turnText = std::string("Turn: ") + (whiteToMove ? "White" : "Black") +
                               " (Opponent) to move";
                ui_.bestMoveText = "Best Move: Waiting for opponent...";
                ui_.gameOver = false;
                analyzedFen = fen;
                if (overlay) overlay->clearArrow();
            }
            sleepInterruptible(150, generation);
            continue;
        }

        // ── Our turn: analyse once per position ──────────────────────────────
        if (analyzedFen == fen) {
            sleepInterruptible(150, generation);
            continue;
        }
        if (!engine_.running()) {
            // python retried discovery/download on every analysis attempt;
            // throttle ours to once a minute to avoid hammering the network.
            const DWORD now = GetTickCount();
            if (now - lastEngineAttempt > 60000) {
                lastEngineAttempt = now;
                std::string exe = EngineUci::findEngine(cfg.stockfish_path);
                if (exe.empty() && running_.load() && generation_.load() == generation) {
                    exe = EngineDownload::downloadStockfish(
                        [](const std::string& msg) { log::print("[Engine] %s", msg.c_str()); });
                }
                if (!exe.empty()) {
                    std::string err;
                    if (engine_.start(exe, cfg.engine_skill_level, &err)) {
                        appliedSkill = cfg.engine_skill_level;
                        setEngineStatus("Engine: Stockfish ready (skill " +
                                        std::to_string(appliedSkill) + "/20)");
                    } else {
                        log::print("[Engine] Engine start error: %s", err.c_str());
                    }
                }
            }
            sleepInterruptible(500, generation);
            continue;
        }

        EngineUci::AnalyzeResult result;
        std::string error;
        const bool analysed = engine_.analyze(fen, cfg.engine_move_time, cfg.engine_depth, result, &error,
                                              &running_);
        if (!analysed) {
            if (running_.load()) {
                log::print("[Engine] Engine calculation error: %s", error.c_str());
                sleepInterruptible(1000, generation);
            }
            continue;
        }

        // The position may have changed while we were thinking (external sync,
        // opponent move, stop/start): never commit against a stale position.
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            valid = (tracker_.boardFen() == fen) && running_.load() && generation_.load() == generation;
            if (valid) {
                ui_.evalText = "Eval: " + result.scoreStr;
                ui_.bestMoveText = "Best Move: " + (result.bestUci.empty() ? std::string("--") : result.bestUci);
                ui_.turnText = std::string("Turn: ") + (whiteToMove ? "White" : "Black") + " to move";
                ui_.gameOver = false;
                if (!result.pv.empty()) {
                    log::print("[Engine] depth=%d score=%s nodes=%llu pv: %s", result.depth,
                               result.scoreStr.c_str(), static_cast<unsigned long long>(result.nodes),
                               joinPv(result.pv).c_str());
                }
            }
        }
        if (!valid) continue;

        // Resolve the move (with optional human-slip simulation).
        chess::Board position;
        position.setFen(fen);
        chess::Move bestMove;
        try {
            bestMove = chess::uci::uciToMove(position, result.bestUci);
        } catch (...) {
            bestMove = chess::Move{};
        }
        if (bestMove == chess::Move{}) {
            log::print("[Engine] Could not resolve engine move '%s' - skipping.", result.bestUci.c_str());
            sleepInterruptible(500, generation);
            continue;
        }
        if (cfg.blunder_chance > 0.0) {
            std::uniform_real_distribution<double> coin(0.0, 1.0);
            if (coin(workerRng()) < cfg.blunder_chance) {
                chess::Movelist moves;
                chess::movegen::legalmoves(moves, position);
                if (moves.size() > 0) {
                    std::uniform_int_distribution<int> pick(0, moves.size() - 1);
                    bestMove = *(moves.begin() + pick(workerRng()));
                    log::print("[Engine] Simulating human slip (blunder_chance=%.2f): %s",
                               cfg.blunder_chance, uciString(bestMove).c_str());
                }
            }
        }

        analyzedFen = fen;

        // ── Overlay suggestion (visual mode) ─────────────────────────────────
        if (cfg.mode == "visual" && cfg.show_overlay && overlay) {
            int rgb = 0x0066FF00;  // r | g<<8 | b<<16 for #00FF66
            int r = 0, g = 0, b = 0;
            if (parseHexColor(cfg.overlay_arrow_color, r, g, b)) rgb = r | (g << 8) | (b << 16);
            const int arrowWidth = std::clamp(cfg.overlay_arrow_width, 2, 24);

            int fromX = 0, fromY = 0, toX = 0, toY = 0;
            bool orient = true;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                orient = tracker_.orientationWhite();
            }
            const auto from = detector.squareScreenCenter(bestMove.from(), roi, orient);
            const auto to = detector.squareScreenCenter(bestMove.to(), roi, orient);
            fromX = from.first;
            fromY = from.second;
            toX = to.first;
            toY = to.second;
            overlay->setArrow(fromX, fromY, toX, toY, rgb, arrowWidth, result.scoreStr);
        } else if (overlay) {
            overlay->clearArrow();
        }

        // ── Autonomous mode: human delay then click ──────────────────────────
        if (cfg.mode == "auto") {
            const double delay =
                workerUniform(std::min(cfg.auto_move_delay_min, cfg.auto_move_delay_max),
                              std::max(cfg.auto_move_delay_min, cfg.auto_move_delay_max));
            if (!sleepInterruptible(static_cast<int>(delay * 1000.0), generation)) break;

            bool stillOurTurn = false;
            bool orient = true;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                const bool samePosition = (tracker_.boardFen() == fen);
                orient = tracker_.orientationWhite();
                const bool over = tracker_.board().isGameOver().first != chess::GameResultReason::NONE;
                stillOurTurn = samePosition && !over && isOurTurn(tracker_.board().sideToMove() == chess::Color::WHITE,
                                                                  cfg.player_color, orient);
            }
            if (!stillOurTurn || !running_.load()) continue;

            MoveExecutor::Options options;
            options.humanize = cfg.humanize_mouse;
            options.promotionTowardCenter = cfg.promotion_picker_toward_center;

            if (executor.executeMove(bestMove, roi, orient, options)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (tracker_.applyManualMove(bestMove)) {
                    log::print("[Tracker] Auto move executed: %s", uciString(bestMove).c_str());
                    ui_.lastMoveText = "Last Move: " + uciString(bestMove);
                    ui_.turnText = std::string("Turn: ") +
                                   (tracker_.board().sideToMove() == chess::Color::WHITE ? "White" : "Black") +
                                   " to move";
                    stateEpoch_.fetch_add(1);  // force fresh analysis next round
                    analyzedFen.clear();
                }
            } else {
                // Fail-safe or aborted: try again shortly (never in a tight loop).
                analyzedFen.clear();
                sleepInterruptible(500, generation);
            }
        }

        sleepInterruptible(80, generation);
    }

    // Thread teardown: leave the engine warm (python stopped it only on close).
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ui_.running = false;
    }
    log::print("[Tracker] Tracking loop stopped.");
}

}  // namespace cm
