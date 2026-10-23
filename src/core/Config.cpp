#include "core/Config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "core/Log.h"
#include "json.hpp"

namespace cm {

using json = nlohmann::json;

std::string appDir() {
    char buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n > 0 ? n : 0);
    const auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

static std::string configPath() { return appDir() + "\\config.json"; }

RectI AppConfig::roiRect() const {
    if (!hasRoi()) return RectI{};
    return RectI{board_roi[0], board_roi[1], board_roi[2], board_roi[3]};
}

template <typename T>
static void readValue(const json& j, const char* key, T& out) {
    const auto it = j.find(key);
    if (it == j.end()) return;
    try {
        it->get_to(out);
    } catch (...) {
        // Type mismatch: keep the default instead of discarding the file.
    }
}

AppConfig AppConfig::load() {
    AppConfig cfg;
    std::FILE* f = std::fopen(configPath().c_str(), "rb");
    if (!f) return cfg;
    std::fclose(f);

    try {
        std::ifstream in(configPath(), std::ios::binary);
        json j;
        in >> j;
        if (!j.is_object()) return cfg;

        readValue(j, "board_roi", cfg.board_roi);
        readValue(j, "player_color", cfg.player_color);
        readValue(j, "stockfish_path", cfg.stockfish_path);
        readValue(j, "engine_depth", cfg.engine_depth);
        readValue(j, "engine_skill_level", cfg.engine_skill_level);
        readValue(j, "engine_move_time", cfg.engine_move_time);
        readValue(j, "mode", cfg.mode);
        readValue(j, "auto_move_delay_min", cfg.auto_move_delay_min);
        readValue(j, "auto_move_delay_max", cfg.auto_move_delay_max);
        readValue(j, "humanize_mouse", cfg.humanize_mouse);
        readValue(j, "blunder_chance", cfg.blunder_chance);
        readValue(j, "show_overlay", cfg.show_overlay);
        readValue(j, "overlay_arrow_color", cfg.overlay_arrow_color);
        readValue(j, "overlay_arrow_width", cfg.overlay_arrow_width);
        readValue(j, "promotion_picker_toward_center", cfg.promotion_picker_toward_center);
    } catch (const std::exception& e) {
        log::print("Failed to load config, using defaults: %s", e.what());
        return AppConfig{};
    }

    if (cfg.board_roi.size() != 4) cfg.board_roi.clear();
    return cfg;
}

bool AppConfig::save() const {
    json j;
    j["board_roi"] = board_roi;
    j["player_color"] = player_color;
    j["stockfish_path"] = stockfish_path;
    j["engine_depth"] = engine_depth;
    j["engine_skill_level"] = engine_skill_level;
    j["engine_move_time"] = engine_move_time;
    j["mode"] = mode;
    j["auto_move_delay_min"] = auto_move_delay_min;
    j["auto_move_delay_max"] = auto_move_delay_max;
    j["humanize_mouse"] = humanize_mouse;
    j["blunder_chance"] = blunder_chance;
    j["show_overlay"] = show_overlay;
    j["overlay_arrow_color"] = overlay_arrow_color;
    j["overlay_arrow_width"] = overlay_arrow_width;
    j["promotion_picker_toward_center"] = promotion_picker_toward_center;

    try {
        std::ofstream out(configPath(), std::ios::binary);
        if (!out) {
            log::print("Failed to save config: cannot open %s", configPath().c_str());
            return false;
        }
        out << j.dump(4) << "\n";
        return true;
    } catch (const std::exception& e) {
        log::print("Failed to save config: %s", e.what());
        return false;
    }
}

bool parseHexColor(const std::string& hex, int& r, int& g, int& b) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6) return false;
    int vals[3];
    for (int i = 0; i < 3; ++i) {
        const std::string part = s.substr(static_cast<std::size_t>(i) * 2, 2);
        char* end = nullptr;
        const long v = std::strtol(part.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || v < 0 || v > 255) return false;
        vals[i] = static_cast<int>(v);
    }
    r = vals[0];
    g = vals[1];
    b = vals[2];
    return true;
}

std::string formatHexColor(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r & 255, g & 255, b & 255);
    return buf;
}

}  // namespace cm
