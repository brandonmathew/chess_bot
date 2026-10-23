import os
import json
from dataclasses import dataclass, asdict

CONFIG_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config.json")
ENGINE_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "engine")

@dataclass
class AppConfig:
    # Screen / Board ROI: [left, top, width, height]
    board_roi: list = None
    player_color: str = "white"  # "white", "black", or "auto"
    
    # Engine Settings
    stockfish_path: str = ""
    engine_depth: int = 15
    engine_skill_level: int = 20  # 0-20
    engine_move_time: float = 0.5  # seconds
    
    # Bot Behavior Settings
    mode: str = "visual"  # "visual" (overlay hints) or "auto" (mouse clicks)
    auto_move_delay_min: float = 0.4
    auto_move_delay_max: float = 1.2
    humanize_mouse: bool = True
    blunder_chance: float = 0.0  # 0.0 to 1.0 (for human simulation)
    
    # Overlay Settings
    show_overlay: bool = True
    overlay_arrow_color: str = "#00FF66"
    overlay_arrow_width: int = 6

    def save(self):
        try:
            with open(CONFIG_FILE, "w", encoding="utf-8") as f:
                json.dump(asdict(self), f, indent=4)
        except Exception as e:
            print(f"Failed to save config: {e}")

    @classmethod
    def load(cls) -> "AppConfig":
        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    return cls(**data)
            except Exception as e:
                print(f"Failed to load config, using defaults: {e}")
        return cls()
