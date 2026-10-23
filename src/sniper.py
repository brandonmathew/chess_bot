import tkinter as tk
from typing import Optional, Tuple, Callable

class RegionSelector:
    """Fullscreen click-and-drag screen region selector tool."""
    def __init__(self, on_selected_callback: Callable[[Tuple[int, int, int, int]], None]):
        self.callback = on_selected_callback
        self.root: Optional[tk.Toplevel] = None
        self.canvas: Optional[tk.Canvas] = None
        self.start_x = 0
        self.start_y = 0
        self.rect_id = None

    def start(self, parent_tk: tk.Tk):
        self.root = tk.Toplevel(parent_tk)
        self.root.attributes("-fullscreen", True)
        self.root.attributes("-alpha", 0.3)
        self.root.attributes("-topmost", True)
        self.root.config(cursor="cross")
        
        self.canvas = tk.Canvas(self.root, cursor="cross", bg="gray20", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Instructions banner
        screen_w = self.root.winfo_screenwidth()
        self.canvas.create_text(
            screen_w // 2, 40,
            text="🎯 Click and drag a box covering the 8x8 chess board (Press ESC to cancel)",
            fill="#00FFCC", font=("Helvetica", 14, "bold")
        )

        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.root.bind("<Escape>", lambda e: self.close())

    def _on_press(self, event):
        self.start_x = event.x
        self.start_y = event.y
        if self.rect_id:
            self.canvas.delete(self.rect_id)
        self.rect_id = self.canvas.create_rectangle(
            self.start_x, self.start_y, self.start_x, self.start_y,
            outline="#00FF66", width=3, fill="#00FF66", stipple="gray25"
        )

    def _on_drag(self, event):
        cur_x, cur_y = event.x, event.y
        self.canvas.coords(self.rect_id, self.start_x, self.start_y, cur_x, cur_y)

    def _on_release(self, event):
        end_x, end_y = event.x, event.y
        
        left = min(self.start_x, end_x)
        top = min(self.start_y, end_y)
        width = abs(self.start_x - end_x)
        height = abs(self.start_y - end_y)
        
        self.close()
        
        if width > 80 and height > 80:
            # Make square if close to square
            dim = (width + height) // 2
            self.callback((left, top, dim, dim))

    def close(self):
        if self.root and tk.Toplevel.winfo_exists(self.root):
            self.root.destroy()
            self.root = None
