#include "engine/EngineDownload.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "engine/EngineUci.h"
#include "json.hpp"
#include "miniz.h"  // must precede miniz_zip.h (provides the mz_* base types)
#include "miniz_zip.h"

namespace cm {

namespace fs = std::filesystem;

namespace {

const char* kReleasesApi = "https://api.github.com/repos/official-stockfish/Stockfish/releases/latest";
const char* kFallbackUrl =
    "https://github.com/official-stockfish/Stockfish/releases/download/sf_19/"
    "stockfish-windows-x86-64-universal.zip";
const wchar_t* kUserAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ChessMonger/1.0";

void report(const EngineDownload::ProgressCallback& progress, const std::string& message) {
    if (progress) progress(message);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

HttpHandle wrapHandle(HINTERNET h) { return HttpHandle(h, &WinHttpCloseHandle); }

// One blocking WinHTTP GET. Returns false with *error set on any failure
// (DNS/connect/status/read); only HTTP 200 counts as success.
bool httpGet(const std::string& url, int receiveTimeoutMs, std::vector<std::uint8_t>& out,
             std::string& error) {
    out.clear();
    const std::wstring wideUrl = widen(url);

    // CrackUrl supplies host/port/scheme; the request path (+query) is taken
    // verbatim from the original URL (everything from the first '/' after
    // the authority) so signed redirect-style URLs survive untouched.
    wchar_t host[256] = {0};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    // lpszUrlPath stays null: the path is taken from the raw URL below.
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        error = "Invalid URL: " + url;
        return false;
    }

    std::wstring fullPath = L"/";
    {
        const std::size_t schemeEnd = wideUrl.find(L"://");
        if (schemeEnd != std::wstring::npos) {
            const std::size_t slash = wideUrl.find(L'/', schemeEnd + 3);
            if (slash != std::wstring::npos) fullPath = wideUrl.substr(slash);
        }
    }

    HttpHandle session = wrapHandle(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHttpOpen failed.";
        return false;
    }
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, receiveTimeoutMs);

    HttpHandle connect = wrapHandle(WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connect) {
        error = "WinHttpConnect failed.";
        return false;
    }

    const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request = wrapHandle(WinHttpOpenRequest(connect.get(), L"GET", fullPath.c_str(), nullptr,
                                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       flags));
    if (!request) {
        error = "WinHttpOpenRequest failed.";
        return false;
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "HTTP request failed for: " + url;
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status) + " for: " + url;
        return false;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) {
            error = "HTTP read failed for: " + url;
            return false;
        }
        if (avail == 0) break;
        std::vector<std::uint8_t> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), avail, &read)) {
            error = "HTTP read failed for: " + url;
            return false;
        }
        chunk.resize(read);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return true;
}

// Extract a zip held in memory under destDir. Two passes, mirroring the
// Python _extract_archive_safely: first validate EVERY member path stays
// inside the destination, only then write anything.
bool extractZipSafely(const std::vector<std::uint8_t>& bytes, const fs::path& destDir, std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        error = "Downloaded file is not a readable zip archive.";
        return false;
    }
    struct ZipGuard {
        mz_zip_archive* z;
        ~ZipGuard() { mz_zip_reader_end(z); }
    } guard{&zip};

    std::error_code ec;
    const std::string destCanon = fs::weakly_canonical(destDir, ec).string();
    if (ec) {
        error = "Cannot resolve the engine folder path.";
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> targets(count);  // "" = directory entry / skipped

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            error = "Corrupt zip archive (bad file entry).";
            return false;
        }
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || mz_zip_reader_is_file_a_directory(&zip, i) || name.back() == '/')
            continue;

        // Reject absolute paths and any ".." component outright.
        const bool absolute = name.front() == '/' || (name.size() >= 2 && name[1] == ':');
        bool hasDotDot = false;
        for (std::size_t pos = 0; pos <= name.size();) {
            const std::size_t slash = name.find('/', pos);
            const std::string part =
                name.substr(pos, slash == std::string::npos ? slash : slash - pos);
            if (part == "..") hasDotDot = true;
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        const fs::path target = fs::weakly_canonical(destDir / name, ec);
        const std::string targetStr = target.string();
        const bool inside = !ec && targetStr.size() > destCanon.size() &&
                            targetStr.compare(0, destCanon.size(), destCanon) == 0 &&
                            (targetStr[destCanon.size()] == '\\' || targetStr[destCanon.size()] == '/');
        if (absolute || hasDotDot || !inside) {
            error = "Unsafe archive member: " + std::string(stat.m_filename);
            return false;
        }
        targets[i] = targetStr;
    }

    for (mz_uint i = 0; i < count; ++i) {
        if (targets[i].empty()) continue;
        const fs::path target(targets[i]);
        fs::create_directories(target.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, targets[i].c_str(), 0)) {
            error = "Failed to extract: " + targets[i];
            return false;
        }
    }
    return true;
}

// Download one archive URL, extract it, and return findEngine's verdict.
std::string fetchAndInstall(const std::string& url, const fs::path& dir,
                            const EngineDownload::ProgressCallback& progress) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!httpGet(url, 120000, bytes, error)) {
        report(progress, "Error fetching Stockfish: " + error);
        return "";
    }
    if (!extractZipSafely(bytes, dir, error)) {
        report(progress, "Error installing Stockfish: " + error);
        return "";
    }
    return EngineUci::findEngine("");
}

}  // namespace

std::string EngineDownload::downloadStockfish(const ProgressCallback& progress) {
    std::error_code ec;
    const fs::path dir(engineDir());
    fs::create_directories(dir, ec);

    // ── Latest GitHub release (python: GITHUB_RELEASES_API) ────────────────
    std::string downloadUrl;
    {
        report(progress, "Fetching latest Stockfish release info...");
        std::vector<std::uint8_t> body;
        std::string error;
        if (httpGet(kReleasesApi, 30000, body, error)) {
            try {
                const auto data = nlohmann::json::parse(body.begin(), body.end());
                for (const auto& asset : data.value("assets", nlohmann::json::array())) {
                    const std::string name = toLower(asset.value("name", ""));
                    if (name.find("windows") != std::string::npos &&
                        name.find("x86-64") != std::string::npos && endsWith(name, ".zip")) {
                        downloadUrl = asset.value("browser_download_url", "");
                        break;
                    }
                }
            } catch (const std::exception& e) {
                report(progress, std::string("Error fetching Stockfish from GitHub API: ") + e.what());
            }
        } else {
            report(progress, "Error fetching Stockfish from GitHub API: " + error);
        }
    }

    if (!downloadUrl.empty()) {
        report(progress, "Downloading Stockfish binary...");
        const std::string found = fetchAndInstall(downloadUrl, dir, progress);
        if (!found.empty()) {
            report(progress, "Stockfish engine installed successfully.");
            return found;
        }
    }

    // ── Pinned fallback (python: sf_19 universal build) ────────────────────
    {
        const std::string found = fetchAndInstall(kFallbackUrl, dir, progress);
        if (!found.empty()) {
            report(progress, "Stockfish engine installed successfully.");
            return found;
        }
    }

    return "";
}

}  // namespace cm
