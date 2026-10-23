import time
import random
import math
import pyautogui
import chess
from typing import Tuple, List
from src.board_detector import BoardDetector

# Set pyautogui fail-safe (moving mouse to corner will abort)
pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.05

class MoveExecutor:
    def __init__(self, board_detector: BoardDetector):
        self.detector = board_detector

    def _bezier_curve(self, p0: Tuple[int, int], p1: Tuple[int, int], p2: Tuple[int, int], t: float) -> Tuple[int, int]:
        """Quadratic Bezier curve point calculation."""
        x = (1 - t)**2 * p0[0] + 2 * (1 - t) * t * p1[0] + t**2 * p2[0]
        y = (1 - t)**2 * p0[1] + 2 * (1 - t) * t * p1[1] + t**2 * p2[1]
        return int(x), int(y)

    def _human_move_mouse(self, target_x: int, target_y: int, duration: float = 0.25):
        """Moves the mouse to (target_x, target_y) using a smooth curved trajectory."""
        start_x, start_y = pyautogui.position()
        
        # Control point with slight random offset to create a natural curve
        mid_x = (start_x + target_x) / 2 + random.randint(-40, 40)
        mid_y = (start_y + target_y) / 2 + random.randint(-40, 40)
        
        steps = max(10, int(duration * 60))
        for step in range(steps + 1):
            t = step / steps
            # Smooth ease in / ease out (cubic hermite)
            ease_t = 3 * t**2 - 2 * t**3
            x, y = self._bezier_curve((start_x, start_y), (mid_x, mid_y), (target_x, target_y), ease_t)
            pyautogui.moveTo(x, y)
            time.sleep(duration / steps)

    def execute_move(self, move: chess.Move, roi: Tuple[int, int, int, int], orientation_white: bool = True, humanize: bool = True, drag_drop: bool = False):
        """
        Translates chess.Move to screen coordinates and executes mouse click/drag.
        """
        sq_w = roi[2] / 8.0
        sq_h = roi[3] / 8.0
        jitter_x = int(sq_w * 0.15)
        jitter_y = int(sq_h * 0.15)
        
        from_x, from_y = self.detector.get_square_screen_center(move.from_square, roi, orientation_white)
        to_x, to_y = self.detector.get_square_screen_center(move.to_square, roi, orientation_white)
        
        # Add random jitter within square center
        from_x += random.randint(-jitter_x, jitter_x)
        from_y += random.randint(-jitter_y, jitter_y)
        to_x += random.randint(-jitter_x, jitter_x)
        to_y += random.randint(-jitter_y, jitter_y)
        
        move_time = random.uniform(0.15, 0.35) if humanize else 0.05
        
        if drag_drop:
            # Drag and drop execution
            if humanize:
                self._human_move_mouse(from_x, from_y, move_time)
            else:
                pyautogui.moveTo(from_x, from_y)
            pyautogui.mouseDown()
            time.sleep(random.uniform(0.04, 0.08))
            if humanize:
                self._human_move_mouse(to_x, to_y, move_time)
            else:
                pyautogui.moveTo(to_x, to_y)
            pyautogui.mouseUp()
        else:
            # Click source, click target execution
            if humanize:
                self._human_move_mouse(from_x, from_y, move_time)
            else:
                pyautogui.moveTo(from_x, from_y)
            pyautogui.click()
            
            time.sleep(random.uniform(0.08, 0.18))
            
            if humanize:
                self._human_move_mouse(to_x, to_y, move_time)
            else:
                pyautogui.moveTo(to_x, to_y)
            pyautogui.click()
            
        # Handle promotion auto-click if needed (chess.com / lichess prompt)
        if move.promotion:
            time.sleep(0.15)
            # Clicking the promotion square again selects Queen by default on most interfaces
            pyautogui.click()
