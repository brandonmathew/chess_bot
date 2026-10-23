#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "chess.hpp"
#include "core/Config.h"
#include "engine/EngineUci.h"
#include "exec/MoveExecutor.h"
#include "overlay/OverlayWindow.h"
#include "vision/BoardDetector.h"
#include "vision/GameTracker.h"

namespace cm {

// Everything the ImGui frame needs, copied under a mutex once per frame.
struct UiSnapshot {
    bool running = false;
    std::string turnText = "Turn: White to move";
    std::string lastMoveText = "Last Move: None";
    std::string evalText = "Eval: +0.00";
    std::string bestMoveText = "Best Move: --";
    std::string engineStatus = "Engine: idle";
    bool desync = false;
    std::string desyncMessage;
    bool gameOver = false;
};

// Single tracking worker:
//   * exactly one analysis/execution pipeline (kills the old stop->start
//     double-loop race and the concurrent auto-move executions),
//   * generation token: a new start invalidates any previous thread,
//   * all GameTracker access is mutex-guarded, engine/mouse stay on this
//     thread, the UI reads an immutable snapshot.
class Worker {
   public:
    Worker() = default;
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Lifecycle -------------------------------------------------------------
    void start(AppConfig settings, OverlayWindow* overlay);
    void stop();
    bool running() const { return running_.load(); }

    UiSnapshot snapshot() const;
    // Current board orientation (which colour sits at the bottom).
    bool orientation() const;

    // GUI-driven mutations (thread-safe; all bump the state epoch so the
    // worker re-analyses the position).
    void updateSettings(const AppConfig& settings);
    void resetBoard();
    void clearDesync();
    void setOrientation(bool orientationWhite);
    // FEN (contains '/') or UCI/SAN move; returns false + error on failure.
    bool syncText(const std::string& text, std::string& error, bool& appliedFen);

   private:
    void threadMain(AppConfig settings, OverlayWindow* overlay, int generation);
    bool sleepInterruptible(int milliseconds, int generation) const;
    void setEngineStatus(const std::string& status);

    mutable std::mutex settingsMutex_;
    AppConfig settings_;

    mutable std::mutex stateMutex_;
    GameTracker tracker_;
    UiSnapshot ui_;

    OverlayWindow* overlay_ = nullptr;
    EngineUci engine_;  // touched only by the worker thread (and after join)

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> generation_{0};
    std::atomic<int> settingsEpoch_{0};
    std::atomic<int> stateEpoch_{0};
};

}  // namespace cm
