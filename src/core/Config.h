#pragma once
#include <string>
#include <vector>

#include "core/Image.h"

namespace cm {

// Directory containing the executable (all relative paths resolve against it).
std::string appDir();

struct AppConfig {
    // Screen/board ROI: {left, top, width, height}; empty = not configured.
    std::vector<int> board_roi;
    std::string player_color = "white";  // "white" | "black" | "auto"
    std::string stockfish_path;

    // Engine settings
    int engine_depth = 15;
    int engine_skill_level = 20;   // 0-20
    double engine_move_time = 0.5;  // seconds

    // Bot behaviour
    std::string mode = "visual";  // "visual" (overlay hints) | "auto" (clicks)
    double auto_move_delay_min = 0.4;
    double auto_move_delay_max = 1.2;
    bool humanize_mouse = true;
    double blunder_chance = 0.0;  // 0.0 - 1.0

    // Overlay
    bool show_overlay = true;
    std::string overlay_arrow_color = "#00FF66";
    int overlay_arrow_width = 6;

    // Promotion picker: click the queen slot toward the board centre instead
    // of blindly re-clicking the destination square.
    bool promotion_picker_toward_center = true;

    bool hasRoi() const { return board_roi.size() == 4; }
    RectI roiRect() const;

    bool save() const;
    static AppConfig load();
};

// "#RRGGBB" <-> components in 0..255 (overlay colours are stored as hex).
bool parseHexColor(const std::string& hex, int& r, int& g, int& b);
std::string formatHexColor(int r, int g, int b);

}  // namespace cm
