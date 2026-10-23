// Ported from tests/test_chess_vision.py::test_stockfish_engine
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "chess.hpp"
#include "doctest.h"
#include "engine/EngineUci.h"

namespace fs = std::filesystem;

namespace {

// The engine normally lives next to the application (engine/stockfish/*.exe).
// Tests run from the build directory, so walk up a few levels as a fallback.
std::string locateEngine() {
    std::string found = cm::EngineUci::findEngine("");
    if (!found.empty()) return found;

    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 4; ++depth) {
        const fs::path engineDir = dir / "engine";
        if (fs::exists(engineDir) && fs::is_directory(engineDir)) {
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(engineDir, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0 &&
                    name.find("stockfish") != std::string::npos) {
                    return entry.path().string();
                }
            }
        }
        const fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return "";
}

}  // namespace

TEST_CASE("test_stockfish_engine") {
    const std::string exe = locateEngine();
    if (exe.empty()) {
        WARN("Stockfish binary not found - skipping engine test.");
        return;
    }

    cm::EngineUci engine;
    std::string error;
    const bool started = engine.start(exe, 10, &error);
    REQUIRE(started);
    REQUIRE(engine.running());

    cm::EngineUci::AnalyzeResult result;
    const bool ok = engine.analyze(chess::constants::STARTPOS, 0.3, 10, result, &error);
    REQUIRE(ok);
    CHECK_FALSE(result.bestUci.empty());
    CHECK_FALSE(result.scoreStr.empty());

    // The returned move must be legal in the starting position.
    chess::Board board;
    const chess::Move best = chess::uci::uciToMove(board, result.bestUci);
    CHECK(best != chess::Move{});
    chess::Movelist legal;
    chess::movegen::legalmoves(legal, board);
    CHECK(legal.contains(best));

    engine.stop();
}
