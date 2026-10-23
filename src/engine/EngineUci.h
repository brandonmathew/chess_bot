#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cm {

// Directory that holds the Stockfish binary: <appDir>\engine (the Python
// implementation's ENGINE_DIR).
std::string engineDir();

// Synchronous UCI client for Stockfish (the python-chess SimpleEngine
// equivalent). Owns the engine child process: anonymous pipes for
// stdin/stdout, CREATE_NO_WINDOW, and blocking line reads with deadlines -
// so callers must use it from a single worker thread, never the UI thread.
class EngineUci {
   public:
    struct AnalyzeResult {
        std::string bestUci;             // e.g. "e2e4" / "e7e8q"
        std::string scoreStr = "0.0";    // python format: "+1.23" / "-0.45" / "M3" / "M-3"
        std::vector<std::string> pv;     // principal variation (UCI moves)
        int depth = 0;
        std::uint64_t nodes = 0;
    };

    EngineUci() = default;
    ~EngineUci();

    EngineUci(const EngineUci&) = delete;
    EngineUci& operator=(const EngineUci&) = delete;

    // Locate a Stockfish executable (python: _find_or_download_engine):
    //   1. `configuredPath` when it exists and ends in .exe,
    //   2. any *stockfish*.exe under <appDir>\engine (recursive),
    //   3. stockfish.exe on PATH.
    // Returns "" when nothing is found.
    static std::string findEngine(const std::string& configuredPath);

    // Spawn the engine and complete the UCI handshake (uci/uciok, skill
    // level, isready/readyok). Returns false with *error set on failure.
    bool start(const std::string& exePath, int skillLevel, std::string* error = nullptr);

    // Send "quit", wait briefly, then terminate. Safe to call when stopped.
    void stop();

    bool running() const;

    // Clamps to 0..20 and applies live when the engine is running.
    void setSkillLevel(int skillLevel);
    int skillLevel() const { return skillLevel_; }

    // Analyse one position: sends "position fen ..." then
    // "go depth <depth> movetime <ms>" (whichever limit is set), reads info
    // lines until "bestmove". Blocks the calling thread; when `cancelFlag`
    // is provided and flips to false, the search is stopped ("stop") and
    // the call returns false instead of a move for a stale position.
    bool analyze(const std::string& fen, double moveTimeSec, int depth, AnalyzeResult& result,
                 std::string* error = nullptr, const std::atomic<bool>* cancelFlag = nullptr);

   private:
    bool sendLine(const std::string& line);
    // Read one '\n'-terminated line (trailing '\r' stripped). Returns false
    // on timeout, on the cancel flag flipping, or when the engine exits.
    bool readLine(std::string& out, int timeoutMs, const std::atomic<bool>* cancelFlag = nullptr);
    bool waitForLine(const std::string& exact, int timeoutMs);
    void drainInput();
    void closeHandles();

    HANDLE process_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    std::string readBuffer_;  // bytes read from the pipe but not yet consumed
    int skillLevel_ = 20;
};

}  // namespace cm
