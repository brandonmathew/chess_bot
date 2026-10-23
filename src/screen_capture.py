import mss
import numpy as np
from PIL import Image
from typing import Optional, Tuple

class ScreenCapture:
    def __init__(self):
        self._sct = mss.mss()

    def grab_screen(self, monitor_index: int = 1) -> np.ndarray:
        """Capture entire monitor. Returns BGR numpy array."""
        mon = self._sct.monitors[monitor_index]
        img = np.array(self._sct.grab(mon))
        # mss returns BGRA, convert to BGR
        return img[:, :, :3]

    def grab_screen_with_origin(self, monitor_index: int = 1):
        """Capture one monitor and return (BGR image, global left/top origin)."""
        mon = self._sct.monitors[monitor_index]
        img = np.array(self._sct.grab(mon))[:, :, :3]
        return img, (int(mon["left"]), int(mon["top"]))

    def monitor_indices(self):
        """Return physical monitor indices (index 0 is MSS's virtual desktop)."""
        return range(1, len(self._sct.monitors))

    def grab_region(self, region: Tuple[int, int, int, int]) -> np.ndarray:
        """
        Capture a specific bounding box.
        region format: (left, top, width, height) or [left, top, width, height]
        Returns BGR numpy array.
        """
        left, top, width, height = int(region[0]), int(region[1]), int(region[2]), int(region[3])
        bbox = {"left": left, "top": top, "width": width, "height": height}
        img = np.array(self._sct.grab(bbox))
        return img[:, :, :3]

    def close(self):
        if self._sct:
            self._sct.close()
