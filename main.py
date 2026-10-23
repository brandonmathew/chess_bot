import sys
import os
import ctypes
import customtkinter as ctk

# Ensure root folder is in sys.path
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)

from src.gui import ChessMongerGUI

def enable_high_dpi():
    """Enables DPI awareness on Windows for crisp UI and accurate screen pixel coordinates."""
    try:
        # Per-monitor DPI aware
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

def main():
    if sys.platform.startswith("win"):
        enable_high_dpi()

    app = ctk.CTk()
    gui = ChessMongerGUI(app)
    app.mainloop()

if __name__ == "__main__":
    main()
