#include "engine/EngineUci.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "core/Config.h"

namespace cm {

namespace fs = std::filesystem;

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWithExe(const std::string& path) {
    return path.size() > 4 && toLower(path.substr(path.size() - 4)) == ".exe";
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(path), ec);
}

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// Parse the numeric / score / pv fields out of one "info ..." line.
// Returns false for lines that carry no search state ("info string ...").
bool parseInfoLine(const std::string& line, EngineUci::AnalyzeResult& out) {
    std::istringstream ss(line);
    std::vector<std::string> tok;
    for (std::string t; ss >> t;) tok.push_back(t);
    if (tok.size() < 2 || tok[0] != "info" || tok[1] == "string") return false;

    bool sawDepth = false;
    for (std::size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] == "depth" && i + 1 < tok.size()) {
            out.depth = std::atoi(tok[++i].c_str());
            sawDepth = true;
        } else if (tok[i] == "nodes" && i + 1 < tok.size()) {
            out.nodes = static_cast<std::uint64_t>(std::strtoull(tok[++i].c_str(), nullptr, 10));
        } else if (tok[i] == "score" && i + 2 < tok.size()) {
            const std::string kind = tok[i + 1];
            const int value = std::atoi(tok[i + 2].c_str());
            if (kind == "mate") {
                // python-chess: f"M{relative.mate()}" (signed).
                out.scoreStr = "M" + std::to_string(value);
            } else {
                // python-chess: f"{cp / 100.0:+.2f}".
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%+.2f", value / 100.0);
                out.scoreStr = buf;
            }
            i += 2;
        } else if (tok[i] == "pv") {
            out.pv.assign(tok.begin() + static_cast<std::ptrdiff_t>(i + 1), tok.end());
            break;  // pv is always the last field
        }
    }
    return sawDepth;
}

}  // namespace

std::string engineDir() { return appDir() + "\\engine"; }

std::string EngineUci::findEngine(const std::string& configuredPath) {
    // 1. Explicitly configured path.
    if (!configuredPath.empty() && endsWithExe(configuredPath) && fileExists(configuredPath))
        return configuredPath;

    // 2. Anything named *stockfish*.exe under <appDir>\engine (python walks
    //    ENGINE_DIR the same way). Sorted so the pick is deterministic.
    {
        std::error_code ec;
        const fs::path dir(engineDir());
        std::vector<std::string> candidates;
        if (fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const std::string name = toLower(entry.path().filename().string());
                if (endsWithExe(name) && name.find("stockfish") != std::string::npos)
                    candidates.push_back(entry.path().string());
            }
        }
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end());
            return candidates.front();
        }
    }

    // 3. stockfish.exe on PATH (python: shutil.which).
    {
        char buf[MAX_PATH] = {0};
        const DWORD n = SearchPathA(nullptr, "stockfish.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) return std::string(buf, n);
    }

    return "";
}

EngineUci::~EngineUci() { stop(); }

bool EngineUci::running() const {
    if (!process_) return false;
    DWORD code = 0;
    return GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE;
}

bool EngineUci::start(const std::string& exePath, int skillLevel, std::string* error) {
    stop();

    if (exePath.empty() || !fileExists(exePath)) {
        setError(error, "Stockfish executable not found: " + exePath);
        return false;
    }
    if (!endsWithExe(exePath)) {
        setError(error, "Stockfish path is not an .exe: " + exePath);
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
        setError(error, "Failed to create engine pipes.");
        if (outRead) CloseHandle(outRead);
        if (outWrite) CloseHandle(outWrite);
        if (inRead) CloseHandle(inRead);
        if (inWrite) CloseHandle(inWrite);
        return false;
    }
    // Our ends of the pipes must not be inherited by the child.
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;

    // Run the engine from its own folder so a bundled NNUE network file
    // sitting next to the .exe is found.
    const std::string workDir = fs::path(exePath).parent_path().string();
    std::string cmdLine = "\"" + exePath + "\"";

    PROCESS_INFORMATION pi{};
    const BOOL created =
        CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!created) {
        setError(error, "CreateProcess failed for: " + exePath);
        CloseHandle(outRead);
        CloseHandle(inWrite);
        return false;
    }
    CloseHandle(pi.hThread);

    process_ = pi.hProcess;
    stdinWrite_ = inWrite;
    stdoutRead_ = outRead;
    readBuffer_.clear();

    // UCI handshake (python-chess popen_uci + configure).
    if (!sendLine("uci") || !waitForLine("uciok", 15000)) {
        setError(error, "Stockfish did not answer the UCI handshake (uciok).");
        stop();
        return false;
    }
    setSkillLevel(skillLevel);
    if (!sendLine("isready") || !waitForLine("readyok", 10000)) {
        setError(error, "Stockfish did not become ready (readyok).");
        stop();
        return false;
    }
    return true;
}

void EngineUci::stop() {
    if (process_) {
        if (running()) {
            sendLine("quit");
            if (WaitForSingleObject(process_, 1000) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 0);
                WaitForSingleObject(process_, 1000);
            }
        }
    }
    closeHandles();
}

void EngineUci::closeHandles() {
    if (stdinWrite_) {
        CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }
    if (stdoutRead_) {
        CloseHandle(stdoutRead_);
        stdoutRead_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    readBuffer_.clear();
}

void EngineUci::setSkillLevel(int skillLevel) {
    skillLevel_ = std::clamp(skillLevel, 0, 20);
    if (running())
        sendLine("setoption name Skill Level value " + std::to_string(skillLevel_));
}

bool EngineUci::sendLine(const std::string& line) {
    if (!stdinWrite_) return false;
    const std::string data = line + "\n";
    DWORD writtenTotal = 0;
    while (writtenTotal < data.size()) {
        DWORD written = 0;
        if (!WriteFile(stdinWrite_, data.data() + writtenTotal,
                       static_cast<DWORD>(data.size() - writtenTotal), &written, nullptr))
            return false;
        writtenTotal += written;
    }
    return true;  // a completed WriteFile on a pipe is already delivered
}

bool EngineUci::readLine(std::string& out, int timeoutMs, const std::atomic<bool>* cancelFlag) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeoutMs));
    for (;;) {
        const std::size_t nl = readBuffer_.find('\n');
        if (nl != std::string::npos) {
            out = readBuffer_.substr(0, nl);
            readBuffer_.erase(0, nl + 1);
            while (!out.empty() && (out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
                out.pop_back();
            return true;
        }

        if (cancelFlag && !cancelFlag->load()) return false;
        if (!process_ || !stdoutRead_) return false;

        DWORD avail = 0;
        if (!PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail > 0) {
            std::vector<char> chunk(std::min<DWORD>(avail, 8192));
            DWORD read = 0;
            if (!ReadFile(stdoutRead_, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr))
                return false;
            if (read == 0) return false;
            readBuffer_.append(chunk.data(), read);
            continue;
        }

        if (!running()) return false;  // engine exited and the pipe is drained
        if (GetTickCount64() >= deadline) return false;
        Sleep(5);
    }
}

bool EngineUci::waitForLine(const std::string& exact, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeoutMs));
    std::string line;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        if (!readLine(line, static_cast<int>(deadline - now))) return false;
        if (line == exact) return true;  // other lines (id/option/...) are skipped
    }
}

void EngineUci::drainInput() {
    readBuffer_.clear();
    if (!stdoutRead_) return;
    DWORD avail = 0;
    while (PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        std::vector<char> chunk(std::min<DWORD>(avail, 8192));
        DWORD read = 0;
        if (!ReadFile(stdoutRead_, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) ||
            read == 0)
            break;
    }
}

bool EngineUci::analyze(const std::string& fen, double moveTimeSec, int depth, AnalyzeResult& result,
                        std::string* error, const std::atomic<bool>* cancelFlag) {
    result = AnalyzeResult{};
    if (!running()) {
        setError(error, "Engine is not running.");
        return false;
    }

    // Drop anything left over from a previously stopped search so a stale
    // "bestmove" can never be mistaken for this position's answer.
    drainInput();

    if (!sendLine("position fen " + fen)) {
        setError(error, "Failed to send the position to the engine.");
        return false;
    }

    std::string go = "go";
    if (depth > 0) go += " depth " + std::to_string(depth);
    const int moveMs = static_cast<int>(moveTimeSec * 1000.0);
    if (moveMs > 0) go += " movetime " + std::to_string(moveMs);
    if (!sendLine(go)) {
        setError(error, "Failed to send the search command to the engine.");
        return false;
    }

    // Stockfish honours movetime as its clock; the extra grace covers a
    // pure-depth search and slow machines. Cancellation shortens the wait.
    ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, moveMs)) + 30000;
    bool stopSent = false;
    bool cancelled = false;

    std::string line;
    for (;;) {
        if (cancelFlag && !cancelFlag->load() && !stopSent) {
            sendLine("stop");
            stopSent = true;
            cancelled = true;
            deadline = GetTickCount64() + 2000;  // grace for the trailing bestmove
        }
        if (GetTickCount64() >= deadline) {
            if (!stopSent) {
                // Ask politely once before declaring a timeout, so the
                // engine returns to a clean prompt for the next search.
                sendLine("stop");
                stopSent = true;
                deadline = GetTickCount64() + 2000;
                continue;
            }
            setError(error, cancelled ? "Analysis cancelled." : "Engine timed out.");
            return false;
        }

        if (!readLine(line, 200, cancelFlag)) {
            if (cancelFlag && !cancelFlag->load()) continue;  // handled at loop top
            if (!running()) {
                setError(error, "Engine exited during analysis.");
                return false;
            }
            continue;  // plain 200 ms slice expiry: re-check deadline/cancel
        }

        if (line.rfind("info ", 0) == 0) {
            parseInfoLine(line, result);
        } else if (line.rfind("bestmove", 0) == 0) {
            std::istringstream ss(line);
            std::string tag, best;
            ss >> tag >> best;
            if (cancelled) {
                setError(error, "Analysis cancelled.");
                return false;
            }
            if (best.empty() || best == "(none)") {
                setError(error, "Engine reported no move for this position.");
                return false;
            }
            result.bestUci = best;
            if (result.pv.empty()) result.pv = {best};
            return true;
        }
        // Anything else (id/option/banner lines) is ignored.
    }
}

}  // namespace cm
