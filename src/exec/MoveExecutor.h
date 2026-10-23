#pragma once
#include "chess.hpp"
#include "core/Image.h"
#include "vision/BoardDetector.h"

namespace cm {

// Human-like mouse execution (SendInput): smooth Bézier trajectories, random
// square jitter, natural delays - the pyautogui equivalent.
class MoveExecutor {
   public:
    struct Options {
        bool humanize = true;
        bool dragDrop = false;
        bool promotionTowardCenter = true;
    };

    explicit MoveExecutor(const BoardDetector& detector) : detector_(detector) {}

    // Returns false when aborted by the corner fail-safe.
    bool executeMove(const chess::Move& move, RectI roi, bool orientationWhite, const Options& options);

    // True when the cursor sits in the top-left corner (pyautogui FAILSAFE).
    static bool failsafeTripped();

   private:
    void humanMoveMouse(int targetX, int targetY, double duration, bool& aborted) const;
    void moveMouse(int x, int y, double duration, bool humanize, bool& aborted) const;

    const BoardDetector& detector_;
};

}  // namespace cm
