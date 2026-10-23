#pragma once
#include <functional>
#include <string>

namespace cm {

// Downloads the official Stockfish Windows build into <appDir>\engine
// (the EngineController.download_stockfish equivalent): latest GitHub
// release first, the pinned Stockfish 19 archive as fallback. Zip
// extraction is path-traversal safe, exactly as in the Python version.
class EngineDownload {
   public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Blocking download + extract. Returns the found stockfish.exe path,
    // or "" when every source failed (the caller then asks the user to
    // place stockfish.exe in the 'engine' folder manually).
    static std::string downloadStockfish(const ProgressCallback& progress = ProgressCallback());
};

}  // namespace cm
