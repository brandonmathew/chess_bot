#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace cm {
namespace log {

// printf-style logging: goes to stdout *and* into a ring buffer that the GUI
// log panel renders.
void print(const char* fmt, ...);

// Last N buffered lines, oldest first.
std::vector<std::string> recent(std::size_t maxLines = 300);

void clear();

}  // namespace log
}  // namespace cm
