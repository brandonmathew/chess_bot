#pragma once
// Synthetic board/position renderers ported from the numpy/cv2 fixtures used
// by the original Python unittest suite (tests/test_advanced_tracking.py).
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "chess.hpp"
#include "core/Image.h"

namespace fixtures {

using cm::Image;

struct Bgr {
    std::uint8_t b, g, r;
};

// Square index helper: sq('e', 4) == chess E4.
constexpr int sq(char file, int rank) { return (rank - 1) * 8 + (file - 'a'); }

inline void fill(Image& img, std::uint8_t value) { img.data.assign(img.data.size(), value); }

inline void setRect(Image& img, int x, int y, int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= img.h) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= img.w) continue;
            img.at(yy, xx, 0) = b;
            img.at(yy, xx, 1) = g;
            img.at(yy, xx, 2) = r;
        }
    }
}

// Plain N x N checkerboard (grayscale squares).
inline Image checkerboard(int size, std::uint8_t lightValue, std::uint8_t darkValue) {
    Image img(size, size);
    const int cell = size / 8;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            const std::uint8_t v = ((r + c) % 2 == 0) ? lightValue : darkValue;
            setRect(img, c * cell, r * cell, cell, cell, v, v, v);
        }
    }
    return img;
}

inline void filledCircle(Image& img, int cx, int cy, int radius, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
        if (y < 0 || y >= img.h) continue;
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || x >= img.w) continue;
            const int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                img.at(y, x, 0) = b;
                img.at(y, x, 1) = g;
                img.at(y, x, 2) = r;
            }
        }
    }
}

inline void circleOutline(Image& img, int cx, int cy, int radius, int thickness, std::uint8_t b, std::uint8_t g,
                          std::uint8_t r) {
    const int inner = radius - thickness;
    const int inner2 = inner * inner;
    const int outer2 = radius * radius;
    for (int y = cy - radius; y <= cy + radius; ++y) {
        if (y < 0 || y >= img.h) continue;
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || x >= img.w) continue;
            const int dx = x - cx, dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= outer2 && (inner < 0 || d2 > inner2)) {
                img.at(y, x, 0) = b;
                img.at(y, x, 1) = g;
                img.at(y, x, 2) = r;
            }
        }
    }
}

inline void thickLine(Image& img, int x1, int y1, int x2, int y2, int thickness, std::uint8_t b, std::uint8_t g,
                      std::uint8_t r) {
    const int steps = std::max({1, std::abs(x2 - x1), std::abs(y2 - y1)});
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const int x = static_cast<int>(x1 + (x2 - x1) * t);
        const int y = static_cast<int>(y1 + (y2 - y1) * t);
        filledCircle(img, x, y, thickness / 2, b, g, r);
    }
}

// Themed 512x512 board with simple color-distinguishable piece discs
// (python: _render_position).
inline Image renderPosition(const chess::Board& board, Bgr whiteFill, Bgr blackFill, bool whiteOutline = true) {
    const int size = 512;
    const int cell = size / 8;
    Image img(size, size);
    for (int screenRow = 0; screenRow < 8; ++screenRow) {
        for (int fileIdx = 0; fileIdx < 8; ++fileIdx) {
            const Bgr color =
                ((screenRow + fileIdx) % 2) ? Bgr{181, 136, 88} : Bgr{229, 218, 185};
            setRect(img, fileIdx * cell, screenRow * cell, cell, cell, color.b, color.g, color.r);

            const int rankIdx = 7 - screenRow;
            const chess::Piece piece = board.at(chess::Square(rankIdx * 8 + fileIdx));
            if (piece == chess::Piece::NONE) continue;

            const int cx = fileIdx * cell + cell / 2;
            const int cy = screenRow * cell + cell / 2;
            const bool isWhite = piece.color() == chess::Color::WHITE;
            const Bgr fill = isWhite ? whiteFill : blackFill;
            filledCircle(img, cx, cy, 19, fill.b, fill.g, fill.r);
            if (isWhite && whiteOutline) circleOutline(img, cx, cy, 19, 2, 20, 20, 20);
        }
    }
    return img;
}

// Engine arrow overlay in the tracker's known "not a highlight" green
// (python: _draw_overlay).
inline void drawOverlay(Image& img, int fromSq, int toSq) {
    const auto center = [](int s) { return std::pair<int, int>{(s % 8) * 64 + 32, (7 - s / 8) * 64 + 32}; };
    const auto [x1, y1] = center(fromSq);
    const auto [x2, y2] = center(toSq);
    thickLine(img, x1, y1, x2, y2, 6, 102, 255, 0);
    circleOutline(img, x1, y1, 18, 3, 102, 255, 0);
    circleOutline(img, x2, y2, 18, 3, 102, 255, 0);
}

// 64 uniform square crops (python: _create_empty_board_images).
inline std::array<Image, 64> solidSquares(std::uint8_t value, int size = 100) {
    std::array<Image, 64> out{};
    for (Image& img : out) {
        img = Image(size, size);
        fill(img, value);
    }
    return out;
}

inline void paint(std::array<Image, 64>& squares, int square, std::uint8_t value) {
    fill(squares[static_cast<std::size_t>(square)], value);
}

inline void pushUci(chess::Board& board, const std::string& uci) {
    const chess::Move move = chess::uci::uciToMove(board, uci);
    board.makeMove(move);
}

inline Image rotate180(const Image& src) {
    Image out(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            std::memcpy(out.px(x, y), src.px(src.w - 1 - x, src.h - 1 - y), 3);
        }
    }
    return out;
}

}  // namespace fixtures
