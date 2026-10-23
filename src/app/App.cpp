#include "app/App.h"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "core/CvOps.h"
#include "core/Log.h"

namespace cm {

namespace {

// customtkinter palette carried over from the original GUI.
const ImVec4 kAccent(0.0f, 1.0f, 0.8f, 1.0f);      // #00FFCC
const ImVec4 kSection(0.39f, 1.0f, 0.855f, 1.0f);  // #64FFDA
const ImVec4 kEval(0.0f, 1.0f, 0.4f, 1.0f);        // #00FF66
const ImVec4 kBest(1.0f, 0.843f, 0.0f, 1.0f);      // #FFD700
const ImVec4 kMuted(0.627f, 0.682f, 0.753f, 1.0f); // #A0AEC0
const ImVec4 kWarn(1.0f, 0.835f, 0.502f, 1.0f);    // #FFD580
const ImVec4 kButton(0.169f, 0.322f, 0.471f, 1.0f);   // #2B5278
const ImVec4 kButtonHover(0.22f, 0.408f, 0.6f, 1.0f);  // #386899
const ImVec4 kStart(0.0f, 0.784f, 0.325f, 1.0f);    // #00C853
const ImVec4 kDanger(0.773f, 0.188f, 0.188f, 1.0f); // #C53030

std::string formatRoi(const std::vector<int>& roi) {
    if (roi.size() != 4) return "Not configured";
    char buf[80];
    std::snprintf(buf, sizeof(buf), "[%d, %d, %d, %d]", roi[0], roi[1], roi[2], roi[3]);
    return buf;
}

// Segmented control (the CTkSegmentedButton equivalent).
// Returns the index of the clicked segment, or -1.
int segmented(const char* id, const std::vector<const char*>& items, int current, float width) {
    int clicked = -1;
    const float gap = 4.0f;
    const float itemW = (width - gap * static_cast<float>(items.size() - 1)) /
                        static_cast<float>(items.size());
    ImGui::PushID(id);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) ImGui::SameLine(0.0f, gap);
        const bool active = (static_cast<int>(i) == current);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, kButton);
        if (ImGui::Button(items[i], ImVec2(itemW, 0))) clicked = static_cast<int>(i);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return clicked;
}

int sideIndex(const std::string& value) {
    if (value == "white") return 0;
    if (value == "black") return 1;
    return 2;
}

int modeIndex(const std::string& value) { return value == "auto" ? 1 : 0; }

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

bool App::init() {
    cfg_ = AppConfig::load();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.078f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.078f, 0.086f, 0.11f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.14f, 0.16f, 0.19f, 1.0f);
    colors[ImGuiCol_Text] = ImVec4(0.88f, 0.9f, 0.93f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.5f, 0.57f, 1.0f);
    colors[ImGuiCol_Button] = kButton;
    colors[ImGuiCol_ButtonHovered] = kButtonHover;
    colors[ImGuiCol_ButtonActive] = ImVec4(0.1f, 0.24f, 0.4f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.169f, 0.322f, 0.471f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = kButtonHover;
    colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.1f, 0.13f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.15f, 0.19f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
    colors[ImGuiCol_CheckMark] = kEval;
    colors[ImGuiCol_SliderGrab] = ImVec4(0.2f, 0.6f, 0.9f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.1f, 0.98f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);

    if (!overlay_.create()) log::print("[Overlay] overlay window unavailable - hints disabled");
    if (cfg_.hasRoi()) boardStatus_ = "Board Region: " + formatRoi(cfg_.board_roi);

    log::print("ChessMonger ready. Config: %s", configSummary().c_str());

    // Overlay-only launch: with a saved board region there is nothing to
    // ask the user - start tracking immediately and let the desktop
    // overlay be the only thing on screen.
    if (cfg_.hasRoi()) {
        startTracking();
        log::print("[App] Overlay-only mode: settings window hidden (tray icon or Ctrl+Alt+G opens it).");
    }
    return true;
}

std::string App::configSummary() const {
    return "mode=" + cfg_.mode + ", side=" + cfg_.player_color +
           ", region=" + formatRoi(cfg_.board_roi);
}

void App::shutdown() {
    stopTracking();
    overlay_.destroy();
    sniper_.close();
    if (!cfg_.save()) log::print("Config was not saved on exit.");
    log::print("Goodbye.");
}

// ─────────────────────────────────────────────────────────────────────────────

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Config")) persistSettings();
        if (ImGui::MenuItem("Reset Board")) resetGame();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quitRequested_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Fair-Play Notice")) fairPlayOpen_ = true;
        if (ImGui::MenuItem("About")) aboutOpen_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::drawHeader() {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("GambitChess Bot");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("Screen Vision & Stockfish Engine Translation");
    ImGui::Separator();
}

void App::drawCalibration() {
    ImGui::SeparatorText("1. Board Calibration");

    const float half = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
    if (ImGui::Button("Auto-Detect Board", ImVec2(half, 0))) autoDetectBoard();
    ImGui::SameLine();
    if (ImGui::Button("Manual Select (Box)", ImVec2(half, 0))) manualSelectBoard();

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(boardStatus_.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void App::drawLiveStatus() {
    ImGui::SeparatorText("2. Live Match Status");
    const UiSnapshot ui = worker_.snapshot();

    if (ui.desync) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.482f, 0.176f, 0.0f, 1.0f));
        ImGui::BeginChild("desync_banner", ImVec2(0, 0), true);
        ImGui::TextWrapped("Position mismatch detected - paste FEN below to sync");
        ImGui::TextWrapped("%s", ui.desyncMessage.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Row 1: turn / last move
    ImGui::Columns(2, "status_row1", false);
    ImGui::TextUnformatted(ui.turnText.c_str());
    ImGui::NextColumn();
    ImGui::TextUnformatted(ui.lastMoveText.c_str());
    ImGui::NextColumn();
    ImGui::Columns(1);

    // Row 2: eval / best move
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.071f, 0.082f, 0.11f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("eval_row", ImVec2(0, 44), false);
    {
        const float mid = ImGui::GetWindowWidth() * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        ImGui::TextColored(kEval, "%s", ui.evalText.c_str());
        const float bestW = ImGui::CalcTextSize(ui.bestMoveText.c_str()).x;
        ImGui::SameLine(std::max(mid, ImGui::GetWindowWidth() - bestW - 24.0f));
        ImGui::TextColored(kBest, "%s", ui.bestMoveText.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Playing as (segmented)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Playing As:");
    ImGui::SameLine();
    const int side = sideIndex(cfg_.player_color);
    const std::vector<const char*> sides = {"white", "black", "auto"};
    const int sideClicked =
        segmented("side", sides, side, ImGui::GetContentRegionAvail().x);
    if (sideClicked >= 0 && sideClicked != side) onSideChanged(sides[static_cast<std::size_t>(sideClicked)]);

    // FEN / UCI sync
    ImGui::TextDisabled("Sync: paste FEN (from Lichess Share->FEN) or type a UCI move");
    ImGui::SetNextItemWidth(-78.0f);
    // EnterReturnsTrue makes InputText report exactly the Enter keypress
    // (the vendored ImGui has no IsItemDeactivatedAfterEnter).
    const bool enterPressed = ImGui::InputText("##sync", syncBuffer_, sizeof(syncBuffer_),
                                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Sync", ImVec2(70, 0)) || enterPressed) manualPushMove();
    ImGui::Spacing();
}

void App::drawEngineSettings() {
    ImGui::SeparatorText("3. Engine & Bot Settings");
    const UiSnapshot ui = worker_.snapshot();
    ImGui::TextDisabled("%s", ui.engineStatus.c_str());

    // Operating mode
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Operating Mode:");
    ImGui::SameLine();
    const int mode = modeIndex(cfg_.mode);
    const std::vector<const char*> modes = {"visual", "auto"};
    const int modeClicked = segmented("mode", modes, mode, ImGui::GetContentRegionAvail().x);
    if (modeClicked >= 0 && modeClicked != mode)
        onModeChanged(modeClicked == 1 ? "auto" : "visual");

    // Skill
    ImGui::Text("Stockfish Skill: %d / 20", cfg_.engine_skill_level);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##skill", &cfg_.engine_skill_level, 0, 20)) worker_.updateSettings(cfg_);
    if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

    // Depth
    ImGui::Text("Search Depth: %d", cfg_.engine_depth);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##depth", &cfg_.engine_depth, 5, 25)) worker_.updateSettings(cfg_);
    if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

    if (ImGui::Checkbox("Humanized Mouse Curves & Realistic Delays", &cfg_.humanize_mouse)) {
        worker_.updateSettings(cfg_);
        persistSettings();
    }
    if (ImGui::Checkbox("Show Overlay Hint", &cfg_.show_overlay)) {
        worker_.updateSettings(cfg_);
        persistSettings();
        applyOverlayVisibility();
    }

    if (ImGui::CollapsingHeader("Advanced (timing, overlay, fairness)")) {
        ImGui::Text("Engine Move Time: %.2f s", cfg_.engine_move_time);
        ImGui::SetNextItemWidth(-1.0f);
        float moveTime = static_cast<float>(cfg_.engine_move_time);
        if (ImGui::SliderFloat("##movetime", &moveTime, 0.1f, 3.0f, "%.2f")) {
            cfg_.engine_move_time = moveTime;
            worker_.updateSettings(cfg_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

        ImGui::Text("Auto-Play Delay: min %.1f s", cfg_.auto_move_delay_min);
        ImGui::SetNextItemWidth(-1.0f);
        float delayMin = static_cast<float>(cfg_.auto_move_delay_min);
        if (ImGui::SliderFloat("##delaymin", &delayMin, 0.0f, 3.0f, "%.1f")) {
            cfg_.auto_move_delay_min = delayMin;
            worker_.updateSettings(cfg_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

        ImGui::Text("Auto-Play Delay: max %.1f s", cfg_.auto_move_delay_max);
        ImGui::SetNextItemWidth(-1.0f);
        float delayMax = static_cast<float>(cfg_.auto_move_delay_max);
        if (ImGui::SliderFloat("##delaymax", &delayMax, 0.0f, 5.0f, "%.1f")) {
            cfg_.auto_move_delay_max = delayMax;
            worker_.updateSettings(cfg_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

        // Overlay arrow appearance (previously dead config keys).
        int r = 0, g = 0, b = 0;
        if (!parseHexColor(cfg_.overlay_arrow_color, r, g, b)) {
            r = 0;
            g = 255;
            b = 102;
        }
        float rgb[3] = {r / 255.0f, g / 255.0f, b / 255.0f};
        ImGui::ColorEdit3("Overlay Arrow Color", rgb);
        const std::string hex = formatHexColor(static_cast<int>(rgb[0] * 255.0f + 0.5f),
                                               static_cast<int>(rgb[1] * 255.0f + 0.5f),
                                               static_cast<int>(rgb[2] * 255.0f + 0.5f));
        if (hex != cfg_.overlay_arrow_color) {
            cfg_.overlay_arrow_color = hex;
            worker_.updateSettings(cfg_);
            persistSettings();
        }

        ImGui::Text("Overlay Arrow Width: %d", cfg_.overlay_arrow_width);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##arrowwidth", &cfg_.overlay_arrow_width, 2, 24))
            worker_.updateSettings(cfg_);
        if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

        ImGui::Text("Blunder Chance: %.0f %%", cfg_.blunder_chance * 100.0);
        ImGui::SetNextItemWidth(-1.0f);
        float blunder = static_cast<float>(cfg_.blunder_chance);
        if (ImGui::SliderFloat("##blunder", &blunder, 0.0f, 1.0f, "%.2f")) {
            cfg_.blunder_chance = blunder;
            worker_.updateSettings(cfg_);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) persistSettings();

        if (ImGui::Checkbox("Promotion: click queen slot toward board centre",
                            &cfg_.promotion_picker_toward_center)) {
            worker_.updateSettings(cfg_);
            persistSettings();
        }
    }
    ImGui::Spacing();
}

void App::drawLog() {
    if (!ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) log::clear();

    if (ImGui::BeginChild("log_view", ImVec2(0, 170), true)) {
        const std::vector<std::string> lines = log::recent(300);
        for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
        if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void App::drawActions() {
    const UiSnapshot ui = worker_.snapshot();
    const bool running = ui.running;

    const float total = ImGui::GetContentRegionAvail().x;
    const float resetW = 130.0f;
    const float startW = total - resetW - 8.0f;

    ImGui::PushStyleColor(ImGuiCol_Button, running ? kDanger : kStart);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          running ? ImVec4(0.9f, 0.25f, 0.25f, 1.0f)
                                  : ImVec4(0.0f, 0.9f, 0.45f, 1.0f));
    if (ImGui::Button(running ? "STOP TRACKING" : "START LIVE TRACKING", ImVec2(startW, 44)))
        toggleTracking();
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.773f, 0.188f, 0.188f, 1.0f));
    if (ImGui::Button("Reset Board", ImVec2(resetW, 44))) resetGame();
    ImGui::PopStyleColor();
}

void App::drawModals() {
    if (!infoPopup_.empty()) ImGui::OpenPopup("Info");
    if (ImGui::BeginPopupModal("Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", infoPopup_.c_str());
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            infoPopup_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!errorPopup_.empty()) ImGui::OpenPopup("Error");
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("%s", errorPopup_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            errorPopup_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (fairPlayOpen_) ImGui::OpenPopup("Fair-Play Notice");
    bool fairPlay = fairPlayOpen_;
    if (ImGui::BeginPopupModal("Fair-Play Notice", &fairPlay, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Auto mode moves your mouse for you. Using any automation while playing rated or "
            "official games can violate the website's Terms of Service and may get your account "
            "restricted or banned.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Recommended use: analysis of your own games, puzzles/practice positions and "
            "single-player modes where assisting play is allowed. You are responsible for how you "
            "use this tool.");
        ImGui::Spacing();
        if (ImGui::Button("I understand", ImVec2(140, 0))) {
            fairPlayOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!fairPlay) fairPlayOpen_ = false;

    if (aboutOpen_) ImGui::OpenPopup("About");
    bool about = aboutOpen_;
    if (ImGui::BeginPopupModal("About", &about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kAccent, "GambitChess Bot (ChessMonger C++)");
        ImGui::TextWrapped(
            "Screen-vision chess helper: hand-rolled computer vision, Dear ImGui interface, UCI "
            "engine support with automatic Stockfish download.");
        ImGui::Spacing();
        ImGui::TextWrapped("Built with Dear ImGui, Disservin chess-library, nlohmann/json and miniz.");
        ImGui::TextWrapped("Stockfish is licensed under GPLv3 - a copy ships in the engine folder.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            aboutOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!about) aboutOpen_ = false;
}

void App::frame() {
    drawMenuBar();

    // The panel IS the OS window (a borderless topmost popup clipped to a
    // rounded rectangle): pin the ImGui window to the whole work area so
    // the menu floats directly on the desktop - no window frame anywhere.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("GambitChess Bot", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

    drawPanelHeader();

    drawHeader();

    // Scrollable body with the action row pinned underneath.
    const float footer = 128.0f;
    ImGui::BeginChild("sections", ImVec2(0, -footer), true);
    drawCalibration();
    drawLiveStatus();
    drawEngineSettings();
    drawLog();
    ImGui::EndChild();

    drawActions();

    ImGui::End();
    ImGui::PopStyleVar(2);

    drawModals();
}

void App::drawPanelHeader() {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 stripStart = ImGui::GetCursorPos();

    // Drag strip: pressing and dragging moves the whole floating panel
    // (forwarded to the OS as a title-bar drag). The X at the right quits.
    ImGui::InvisibleButton("##panel_drag", ImVec2(std::max(1.0f, width - 36.0f), 24.0f));
    if (ImGui::IsItemActivated() && hostWindow_) {
        ReleaseCapture();
        SendMessageA(static_cast<HWND>(hostWindow_), WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("X")) quitRequested_ = true;

    const ImVec2 winPos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(winPos.x + stripStart.x + 2.0f, winPos.y + stripStart.y + 4.0f),
        ImGui::GetColorU32(kAccent), "GambitChess Bot - drag to move");

    ImGui::Separator();
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void App::autoDetectBoard() {
    boardStatus_ = "Scanning screen for chess board...";

    bool found = false;
    long long bestArea = -1;
    RectI best{};
    for (const MonitorInfo& monitor : capture_.monitors()) {
        const Image screen = capture_.grabRegion(monitor.rect);
        if (screen.empty()) continue;
        const auto local = detector_.autoDetectBoard(screen);
        if (!local) continue;
        const long long area = static_cast<long long>(local->w) * local->h;
        if (area > bestArea) {
            bestArea = area;
            best = RectI{local->x + monitor.rect.x, local->y + monitor.rect.y, local->w, local->h};
            found = true;
        }
    }

    if (found) {
        onBoardRegionSelected(best, false);
        infoPopup_ = "Chess board detected at screen coordinates: " +
                     formatRoi({best.x, best.y, best.w, best.h});
    } else {
        errorPopup_ =
            "Could not auto-detect chess board. Please make sure the chess board is visible on "
            "screen or use 'Manual Select (Box)'.";
        boardStatus_ = "Board Region: " + formatRoi(cfg_.board_roi);
    }
}

void App::manualSelectBoard() {
    sniper_.open([this](RectI roi) { onBoardRegionSelected(roi, false); });
}

void App::onBoardRegionSelected(RectI roi, bool allowExpand) {
    // Contour detection can include a thin border around the board. Snap the
    // selection to the internal 8x8 grid so every square crop stays aligned.
    try {
        RectI captureRoi = roi;
        if (allowExpand) {
            // Also recovers older saved regions cropped a few pixels inside the
            // board: refinement can then expand as well as trim.
            const int pad = std::max(6, static_cast<int>(std::min(roi.w, roi.h) * 0.015));
            captureRoi = RectI{roi.x - pad, roi.y - pad, roi.w + 2 * pad, roi.h + 2 * pad};
        }

        const Image initial = capture_.grabRegion(captureRoi);
        if (!initial.empty()) {
            const Plane gray = cv::bgr2gray(initial);
            const RectI local = detector_.refineBoardBox(gray, RectI{0, 0, initial.w, initial.h});
            roi = RectI{captureRoi.x + local.x, captureRoi.y + local.y, local.w, local.h};
        }
    } catch (const std::exception& e) {
        log::print("Board region refinement failed: %s", e.what());
    }

    cfg_.board_roi = {roi.x, roi.y, roi.w, roi.h};
    persistSettings();
    boardStatus_ = "Board Region: " + formatRoi(cfg_.board_roi);

    const Image boardImg = capture_.grabRegion(roi);
    const bool isWhite = effectiveOrientation(boardImg.empty() ? nullptr : &boardImg);

    worker_.setOrientation(isWhite);
    if (cfg_.player_color == "auto") {
        log::print("[Vision] Auto color detected: %s at bottom", isWhite ? "White" : "Black");
    }
    if (overlay_.visible()) overlay_.show(roi);  // reposition while visible
    worker_.updateSettings(cfg_);
}

bool App::effectiveOrientation(const Image* boardImg) {
    if (cfg_.player_color == "white") return true;
    if (cfg_.player_color == "black") return false;

    // "auto": detect which colour sits at the bottom of the captured board.
    if (boardImg) return detector_.detectOrientation(*boardImg);
    if (cfg_.hasRoi()) {
        const Image img = capture_.grabRegion(cfg_.roiRect());
        if (!img.empty()) return detector_.detectOrientation(img);
    }
    return worker_.orientation();
}

void App::onSideChanged(const std::string& side) {
    cfg_.player_color = side;
    persistSettings();
    const bool isWhite = effectiveOrientation();
    worker_.setOrientation(isWhite);
    if (cfg_.hasRoi() && overlay_.visible()) overlay_.show(cfg_.roiRect());
    worker_.updateSettings(cfg_);
}

void App::onModeChanged(const std::string& mode) {
    cfg_.mode = mode;
    persistSettings();
    worker_.updateSettings(cfg_);
    applyOverlayVisibility();
}

void App::applyOverlayVisibility() {
    // The overlay is the product's face in both modes now: in auto mode the
    // arrow shows the move that is about to be played.
    const bool shouldShow = worker_.running() && cfg_.show_overlay && cfg_.hasRoi();
    if (shouldShow) {
        overlay_.clearArrow();
        overlay_.show(cfg_.roiRect());
    } else {
        overlay_.clearArrow();
        overlay_.hide();
    }
}

void App::toggleTracking() {
    if (worker_.running()) {
        stopTracking();
    } else {
        startTracking();
    }
}

void App::startTracking() {
    if (!cfg_.hasRoi()) {
        errorPopup_ = "Please detect or calibrate the chess board region first.";
        return;
    }

    // Revalidate saved/manual calibration before each run (also fixes older
    // config values that included a thin border around the board).
    onBoardRegionSelected(cfg_.roiRect(), true);

    worker_.start(cfg_, &overlay_);
    applyOverlayVisibility();
    log::print("[Tracker] Live tracking started (%s mode).", cfg_.mode.c_str());
}

void App::stopTracking() {
    if (!worker_.running()) return;
    worker_.stop();
    overlay_.hide();
    log::print("[Tracker] Live tracking stopped.");
}

void App::manualPushMove() {
    const std::string text(syncBuffer_);
    if (text.empty()) return;

    std::string error;
    bool appliedFen = false;
    if (worker_.syncText(text, error, appliedFen)) {
        syncBuffer_[0] = '\0';
    } else if (!error.empty()) {
        errorPopup_ = error;
    }
}

void App::resetGame() {
    const bool isWhite = effectiveOrientation();
    worker_.setOrientation(isWhite);
    worker_.resetBoard();
}

void App::persistSettings() {
    cfg_.save();
}

}  // namespace cm
