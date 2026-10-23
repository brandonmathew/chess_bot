#pragma once
#include <string>
#include <vector>

#include "app/Worker.h"
#include "capture/RegionSniper.h"
#include "capture/ScreenCapture.h"
#include "core/Config.h"
#include "overlay/OverlayWindow.h"
#include "vision/BoardDetector.h"

namespace cm {

// The whole application UI (the customtkinter GUI ported to Dear ImGui).
// Everything in here runs on the main thread; long-running work happens in
// Worker.
class App {
   public:
    bool init();
    void shutdown();

    std::string configSummary() const;

    // Builds one ImGui frame (called from the render loop).
    void frame();

   private:
    // Section renderers
    void drawMenuBar();
    void drawHeader();
    void drawCalibration();
    void drawLiveStatus();
    void drawEngineSettings();
    void drawActions();
    void drawLog();
    void drawModals();

    // Callbacks (python GUI handlers)
    void autoDetectBoard();
    void manualSelectBoard();
    void onBoardRegionSelected(RectI roi, bool allowExpand);
    bool effectiveOrientation(const Image* boardImg = nullptr);
    void onSideChanged(const std::string& side);
    void onModeChanged(const std::string& mode);
    void toggleTracking();
    void startTracking();
    void stopTracking();
    void manualPushMove();
    void resetGame();
    void applyOverlayVisibility();

    void persistSettings();  // debounced save-to-disk

   public:
    bool quitRequested() const { return quitRequested_; }

   private:
    AppConfig cfg_;
    ScreenCapture capture_;
    BoardDetector detector_;
    OverlayWindow overlay_;
    RegionSniper sniper_;
    Worker worker_;

    std::string boardStatus_ = "Board Region: Not configured";
    char syncBuffer_[512] = {0};
    std::string infoPopup_;
    std::string errorPopup_;
    bool fairPlayOpen_ = false;
    bool aboutOpen_ = false;
    bool logAutoScroll_ = true;

    bool wasRunning_ = false;
    bool quitRequested_ = false;
};

}  // namespace cm
