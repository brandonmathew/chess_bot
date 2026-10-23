import tkinter as tk
import math
from typing import Optional, Tuple
import chess
from src.board_detector import BoardDetector

class ChessOverlay:
    def __init__(self, board_detector: BoardDetector):
        self.detector = board_detector
        self.root: Optional[tk.Toplevel] = None
        self.canvas: Optional[tk.Canvas] = None
        self.roi: Optional[Tuple[int, int, int, int]] = None
        self.orientation_white: bool = True
        self.trans_color = "#abcdef"  # Transparent background key

    def show(self, parent_tk: tk.Tk, roi: Tuple[int, int, int, int], orientation_white: bool = True):
        self.roi = roi
        self.orientation_white = orientation_white
        
        left, top, width, height = roi
        
        if self.root is None or not tk.Toplevel.winfo_exists(self.root):
            self.root = tk.Toplevel(parent_tk)
            self.root.overrideredirect(True)
            self.root.attributes("-topmost", True)
            self.root.attributes("-transparentcolor", self.trans_color)
            self.root.config(bg=self.trans_color)
            
            # Make window click-through on Windows (WS_EX_TRANSPARENT)
            try:
                import ctypes
                hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
                # GWL_EXSTYLE = -20, WS_EX_TRANSPARENT = 0x20, WS_EX_LAYERED = 0x80000
                style = ctypes.windll.user32.GetWindowLongW(hwnd, -20)
                ctypes.windll.user32.SetWindowLongW(hwnd, -20, style | 0x80000 | 0x20)
            except Exception:
                pass
            
            self.canvas = tk.Canvas(self.root, bg=self.trans_color, highlightthickness=0)
            self.canvas.pack(fill=tk.BOTH, expand=True)

        self.root.geometry(f"{width}x{height}+{left}+{top}")
        self.clear()

    def hide(self):
        if self.root and tk.Toplevel.winfo_exists(self.root):
            self.root.withdraw()

    def clear(self):
        if self.canvas:
            self.canvas.delete("all")

    def update_roi(self, roi: Tuple[int, int, int, int], orientation_white: bool = True):
        self.roi = roi
        self.orientation_white = orientation_white
        if self.root and tk.Toplevel.winfo_exists(self.root):
            left, top, width, height = roi
            self.root.geometry(f"{width}x{height}+{left}+{top}")

    def draw_move_suggestion(self, move: chess.Move, eval_str: str = "", arrow_color: str = "#00FF66"):
        """Draws the arrow and evaluation text relative to the overlay window coordinates."""
        if not self.canvas or not self.roi or not self.root or not tk.Toplevel.winfo_exists(self.root):
            return
            
        self.root.deiconify()
        self.clear()
        
        left, top, width, height = self.roi
        
        # Absolute screen centers
        from_abs_x, from_abs_y = self.detector.get_square_screen_center(move.from_square, self.roi, self.orientation_white)
        to_abs_x, to_abs_y = self.detector.get_square_screen_center(move.to_square, self.roi, self.orientation_white)
        
        # Convert to relative overlay canvas coordinates
        x1, y1 = from_abs_x - left, from_abs_y - top
        x2, y2 = to_abs_x - left, to_abs_y - top
        
        sq_w = width / 8.0
        radius = int(sq_w * 0.35)
        
        # Source square highlight circle
        self.canvas.create_oval(
            x1 - radius, y1 - radius, x1 + radius, y1 + radius,
            outline=arrow_color, width=3
        )
        
        # Destination square highlight circle
        self.canvas.create_oval(
            x2 - radius, y2 - radius, x2 + radius, y2 + radius,
            fill=arrow_color, outline=arrow_color, stipple="gray25", width=2
        )
        
        # Draw connecting arrow line
        self.canvas.create_line(
            x1, y1, x2, y2,
            fill=arrow_color,
            width=6,
            arrow=tk.LAST,
            arrowshape=(16, 20, 8),
            capstyle=tk.ROUND,
            joinstyle=tk.ROUND
        )
        
        # Draw Eval label badge if provided
        if eval_str:
            badge_x = max(40, min(width - 40, x2))
            badge_y = max(20, min(height - 20, y2 - int(sq_w * 0.45)))
            
            self.canvas.create_rectangle(
                badge_x - 30, badge_y - 12, badge_x + 30, badge_y + 12,
                fill="#1E1E1E", outline=arrow_color, width=2
            )
            self.canvas.create_text(
                badge_x, badge_y,
                text=eval_str,
                fill="#FFFFFF",
                font=("Helvetica", 10, "bold")
            )
