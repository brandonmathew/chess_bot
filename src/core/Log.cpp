#include "core/Log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace cm {
namespace log {

namespace {
std::mutex g_mutex;
std::deque<std::string> g_lines;
constexpr std::size_t kMaxLines = 500;
}  // namespace

void print(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    (void)n;

    std::string line(buf);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::fputs(buf, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        g_lines.push_back(line);
        while (g_lines.size() > kMaxLines) g_lines.pop_front();
    }
}

std::vector<std::string> recent(std::size_t maxLines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> out;
    const std::size_t total = g_lines.size();
    const std::size_t take = std::min(maxLines, total);
    out.reserve(take);
    for (std::size_t i = total - take; i < total; ++i) out.push_back(g_lines[i]);
    return out;
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.clear();
}

}  // namespace log
}  // namespace cm
