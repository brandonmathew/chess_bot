#include "exec/MoveExecutor.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "core/Log.h"

namespace cm {

namespace {

std::mt19937& rng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

double uniform(double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(rng());
}

int uniformInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }

void setCursorAbsolute(int x, int y) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(std::lround((x - vx) * 65535.0 / vw));
    input.mi.dy = static_cast<LONG>(std::lround((y - vy) * 65535.0 / vh));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(INPUT));
}

void mouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(20);  // stands in for pyautogui's PAUSE
}

void clickAt(int x, int y) {
    setCursorAbsolute(x, y);
    Sleep(15);
    mouseButton(true);
    mouseButton(false);
}

// Quadratic Bézier point with smooth ease-in/ease-out (cubic Hermite).
std::pair<int, int> bezierPoint(std::pair<int, int> p0, std::pair<int, int> p1, std::pair<int, int> p2,
                                double t) {
    const double mt = 1.0 - t;
    const double x = mt * mt * p0.first + 2 * mt * t * p1.first + t * t * p2.first;
    const double y = mt * mt * p0.second + 2 * mt * t * p1.second + t * t * p2.second;
    return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
}

}  // namespace

bool MoveExecutor::failsafeTripped() {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    return p.x <= 5 && p.y <= 5;
}

void MoveExecutor::humanMoveMouse(int targetX, int targetY, double duration, bool& aborted) const {
    POINT start{};
    GetCursorPos(&start);

    // Control point with a slight random offset for a natural curve.
    const int midX = static_cast<int>((start.x + targetX) / 2.0) + uniformInt(-40, 40);
    const int midY = static_cast<int>((start.y + targetY) / 2.0) + uniformInt(-40, 40);

    const int steps = std::max(10, static_cast<int>(duration * 60));
    for (int step = 0; step <= steps; ++step) {
        if (failsafeTripped()) {
            aborted = true;
            log::print("[Mouse] Fail-safe triggered (cursor in corner) - move aborted.");
            return;
        }
        const double t = static_cast<double>(step) / steps;
        const double easeT = 3.0 * t * t - 2.0 * t * t * t;
        const auto [x, y] = bezierPoint({start.x, start.y}, {midX, midY}, {targetX, targetY}, easeT);
        setCursorAbsolute(x, y);
        Sleep(static_cast<DWORD>(std::max(1.0, duration * 1000.0 / steps)));
    }
}

void MoveExecutor::moveMouse(int x, int y, double duration, bool humanize, bool& aborted) const {
    if (humanize) {
        humanMoveMouse(x, y, duration, aborted);
    } else {
        setCursorAbsolute(x, y);
        Sleep(15);
    }
}

bool MoveExecutor::executeMove(const chess::Move& move, RectI roi, bool orientationWhite,
                               const Options& options) {
    if (failsafeTripped()) {
        log::print("[Mouse] Fail-safe triggered before move execution.");
        return false;
    }

    const double sqW = roi.w / 8.0;
    const double sqH = roi.h / 8.0;
    const int jitterX = static_cast<int>(sqW * 0.15);
    const int jitterY = static_cast<int>(sqH * 0.15);

    auto [fromX, fromY] = detector_.squareScreenCenter(move.from(), roi, orientationWhite);
    auto [toX, toY] = detector_.squareScreenCenter(move.to(), roi, orientationWhite);

    // Random jitter inside the target square.
    if (jitterX > 0) {
        fromX += uniformInt(-jitterX, jitterX);
        toX += uniformInt(-jitterX, jitterX);
    }
    if (jitterY > 0) {
        fromY += uniformInt(-jitterY, jitterY);
        toY += uniformInt(-jitterY, jitterY);
    }

    const double moveTime = options.humanize ? uniform(0.15, 0.35) : 0.05;
    bool aborted = false;

    if (options.dragDrop) {
        moveMouse(fromX, fromY, moveTime, options.humanize, aborted);
        if (aborted) return false;
        mouseButton(true);
        Sleep(static_cast<DWORD>(uniform(40, 80)));
        moveMouse(toX, toY, moveTime, options.humanize, aborted);
        if (aborted) {
            mouseButton(false);
            return false;
        }
        mouseButton(false);
    } else {
        moveMouse(fromX, fromY, moveTime, options.humanize, aborted);
        if (aborted) return false;
        mouseButton(true);
        mouseButton(false);

        Sleep(static_cast<DWORD>(uniform(80, 180)));

        moveMouse(toX, toY, moveTime, options.humanize, aborted);
        if (aborted) return false;
        mouseButton(true);
        mouseButton(false);
    }

    // Promotion: most interfaces show a picker after the destination click.
    if (move.typeOf() == chess::Move::PROMOTION) {
        Sleep(150);
        if (options.promotionTowardCenter) {
            // The queen occupies the picker cell adjacent to the destination,
            // on the side facing the board centre.
            const int col = orientationWhite ? (move.to() % 8) : (7 - move.to() % 8);
            const int row = orientationWhite ? (7 - move.to() / 8) : (move.to() / 8);
            const int offset = (row < 4) ? static_cast<int>(sqH) : -static_cast<int>(sqH);
            (void)col;
            clickAt(toX, toY + offset);
        } else {
            clickAt(toX, toY);
        }
    }

    return true;
}

}  // namespace cm
