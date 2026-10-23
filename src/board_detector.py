import cv2
import numpy as np
from typing import Optional, Tuple, List, Dict
import chess

class BoardDetector:
    def __init__(self):
        pass

    def auto_detect_board(self, screen_img: np.ndarray) -> Optional[Tuple[int, int, int, int]]:
        """
        Scan full screen image and attempt to detect the 8x8 chess board bounding box.
        Returns (left, top, width, height) or None if not found.
        """
        gray = cv2.cvtColor(screen_img, cv2.COLOR_BGR2GRAY)
        h, w = gray.shape

        # Min size for a chess board: at least 200x200 px, max size 95% screen
        min_dim = int(min(h, w) * 0.2)
        max_dim = int(min(h, w) * 0.95)

        # 1. Edge & Contour Detection
        edges = cv2.Canny(gray, 40, 150)
        contours, _ = cv2.findContours(edges, cv2.RETR_TREE, cv2.CHAIN_APPROX_SIMPLE)

        best_box = None
        best_score = -1

        for cnt in contours:
            peri = cv2.arcLength(cnt, True)
            approx = cv2.approxPolyDP(cnt, 0.02 * peri, True)

            # Look for 4-point quadrilaterals
            if len(approx) == 4:
                x, y, bw, bh = cv2.boundingRect(approx)
                aspect_ratio = float(bw) / bh if bh > 0 else 0
                
                # Board must be roughly square (aspect ratio 0.95 to 1.05) and reasonable size
                if 0.92 <= aspect_ratio <= 1.08 and min_dim <= bw <= max_dim and min_dim <= bh <= max_dim:
                    # Check internal grid structure (alternating squares)
                    crop = gray[y:y+bh, x:x+bw]
                    score = self._evaluate_board_candidate(crop)
                    if score > best_score:
                        best_score = score
                        best_box = (x, y, bw, bh)

        # Return best box if confidence is sufficient
        if best_box and best_score > 0.3:
            return self._refine_board_box(gray, best_box)

        # Some browser themes do not expose a continuous outer-board contour.
        # Fall back to finding the two dominant alternating square colors.
        color_box = self._detect_by_dominant_colors(screen_img)
        if color_box:
            return self._refine_board_box(gray, color_box)

        return None

    def _detect_by_dominant_colors(self, image: np.ndarray) -> Optional[Tuple[int, int, int, int]]:
        """Find a board from two dominant colored regions when contours fail."""
        h, w = image.shape[:2]
        quantized = (image // 16).astype(np.uint16)
        codes = (quantized[:, :, 0] +
                 quantized[:, :, 1] * 16 +
                 quantized[:, :, 2] * 256)
        counts = np.bincount(codes.ravel(), minlength=4096)
        order = np.argsort(counts)[::-1]

        colors = []
        min_count = int(h * w * 0.003)
        for code in order:
            if counts[code] < min_count:
                break
            color = np.array([
                (code % 16) * 16,
                ((code // 16) % 16) * 16,
                ((code // 256) % 16) * 16,
            ], dtype=np.uint8)
            hsv = cv2.cvtColor(color.reshape(1, 1, 3), cv2.COLOR_BGR2HSV)[0, 0]
            if hsv[1] > 25 and 40 < hsv[2] < 250:
                colors.append(color)
            if len(colors) >= 10:
                break

        if len(colors) < 2:
            return None

        image_i16 = image.astype(np.int16)
        masks = []
        for color in colors:
            distance = np.max(np.abs(image_i16 - color.astype(np.int16)), axis=2)
            masks.append(distance <= 24)

        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        kernel = np.ones((9, 9), dtype=np.uint8)
        best = None
        best_score = float("-inf")

        for first in range(len(colors)):
            for second in range(first + 1, len(colors)):
                color_distance = np.linalg.norm(
                    colors[first].astype(np.float32) -
                    colors[second].astype(np.float32)
                )
                if color_distance < 35.0:
                    continue

                mask = ((masks[first] | masks[second]) * 255).astype(np.uint8)
                mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
                contours, _ = cv2.findContours(
                    mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
                )
                for contour in contours:
                    x, y, bw, bh = cv2.boundingRect(contour)
                    if min(bw, bh) < int(min(h, w) * 0.20):
                        continue
                    if not 0.90 <= (bw / bh if bh else 0) <= 1.10:
                        continue

                    coverage = float(np.mean(mask[y:y + bh, x:x + bw] > 0))
                    if coverage < 0.50:
                        continue

                    side = min(max(bw, bh), w, h)
                    cx, cy = x + bw // 2, y + bh // 2
                    sx = max(0, min(w - side, cx - side // 2))
                    sy = max(0, min(h - side, cy - side // 2))
                    candidate = gray[sy:sy + side, sx:sx + side]
                    checker_score = self._evaluate_board_candidate(candidate)
                    if checker_score < 0.15:
                        continue

                    score = coverage * side * side * checker_score
                    if score > best_score:
                        best_score = score
                        best = (sx, sy, side, side)

        return best

    def _refine_board_box(self, gray: np.ndarray,
                          box: Tuple[int, int, int, int]) -> Tuple[int, int, int, int]:
        """Snap a loose contour box to the actual 8x8 grid boundaries."""
        x, y, bw, bh = box
        crop = gray[y:y + bh, x:x + bw]
        max_side = min(bw, bh)
        # Contour detection is normally only a few pixels outside the board.
        # A 4% trim window is enough while keeping calibration responsive.
        min_side = max(64, int(max_side * 0.96))
        best = (float("-inf"), 0, 0, max_side)

        # Coarse search first, then refine around the winner. This keeps the
        # more reliable checkerboard score responsive on large boards.
        coarse_sides = list(range(min_side, max_side + 1, 2))
        if max_side not in coarse_sides:
            coarse_sides.append(max_side)
        for side in coarse_sides:
            max_x_offset = bw - side
            max_y_offset = bh - side
            y_offsets = list(range(0, max_y_offset + 1, 2))
            x_offsets = list(range(0, max_x_offset + 1, 2))
            if max_y_offset not in y_offsets:
                y_offsets.append(max_y_offset)
            if max_x_offset not in x_offsets:
                x_offsets.append(max_x_offset)
            for oy in y_offsets:
                for ox in x_offsets:
                    candidate = crop[oy:oy + side, ox:ox + side]
                    score = self._grid_boundary_score(candidate)
                    if score > best[0]:
                        best = (score, ox, oy, side)

        _, coarse_x, coarse_y, coarse_side = best
        for side in range(max(min_side, coarse_side - 2),
                          min(max_side, coarse_side + 2) + 1):
            max_x_offset = bw - side
            max_y_offset = bh - side
            for oy in range(max(0, coarse_y - 2),
                            min(max_y_offset, coarse_y + 2) + 1):
                for ox in range(max(0, coarse_x - 2),
                                min(max_x_offset, coarse_x + 2) + 1):
                    candidate = crop[oy:oy + side, ox:ox + side]
                    score = self._grid_boundary_score(candidate)
                    if score > best[0]:
                        best = (score, ox, oy, side)

        _, ox, oy, side = best
        return (x + ox, y + oy, side, side)

    @staticmethod
    def _grid_boundary_score(crop: np.ndarray) -> float:
        """Score checkerboard alignment using piece-free square corners."""
        h, w = crop.shape
        if h < 64 or w < 64:
            return float("-inf")

        small = cv2.resize(crop, (64, 64), interpolation=cv2.INTER_AREA)
        squares = small.reshape(8, 8, 8, 8).transpose(0, 2, 1, 3)
        corners = np.concatenate((
            squares[:, :, :2, :2].reshape(8, 8, -1),
            squares[:, :, :2, -2:].reshape(8, 8, -1),
            squares[:, :, -2:, :2].reshape(8, 8, -1),
            squares[:, :, -2:, -2:].reshape(8, 8, -1),
        ), axis=2).astype(np.float32)
        medians = np.median(corners, axis=2)
        corner_noise = float(np.mean(np.abs(corners - medians[:, :, None])))
        checker = np.indices((8, 8)).sum(axis=0) % 2
        group_a = medians[checker == 0]
        group_b = medians[checker == 1]
        contrast = abs(float(np.mean(group_a)) - float(np.mean(group_b)))
        inconsistency = float(np.std(group_a) + np.std(group_b))
        return contrast - 2.0 * corner_noise - inconsistency

    def _evaluate_board_candidate(self, crop: np.ndarray) -> float:
        """Heuristic score to verify if a crop looks like an 8x8 chess board."""
        try:
            ch, cw = crop.shape
            sq_h = ch / 8.0
            sq_w = cw / 8.0
            
            # Sample mean intensities of the 64 squares
            means = []
            for r in range(8):
                for c in range(8):
                    # Sample center 50% of square to avoid piece edges
                    y1 = int((r + 0.25) * sq_h)
                    y2 = int((r + 0.75) * sq_h)
                    x1 = int((c + 0.25) * sq_w)
                    x2 = int((c + 0.75) * sq_w)
                    sq_crop = crop[y1:y2, x1:x2]
                    means.append(np.mean(sq_crop))
            
            means = np.array(means).reshape((8, 8))
            
            # Check checkerboard pattern correlation
            checker = np.indices((8, 8)).sum(axis=0) % 2
            # Light vs dark square contrast
            light_mean = np.mean(means[checker == 1])
            dark_mean = np.mean(means[checker == 0])
            contrast = abs(light_mean - dark_mean)
            
            # Standard deviation among light squares and dark squares should be relatively moderate
            return float(contrast) / 255.0
        except Exception:
            return 0.0

    def slice_board(self, board_img: np.ndarray, orientation_white: bool = True) -> List[np.ndarray]:
        """
        Slices the board image into 64 square crops.
        Index 0..63 maps to chess.A1 (0) .. chess.H8 (63) if orientation_white is True.
        """
        bh, bw = board_img.shape[:2]
        sq_h = bh / 8.0
        sq_w = bw / 8.0
        
        squares = [None] * 64
        for rank_idx in range(8):  # 0 is top row on screen, 7 is bottom row
            for file_idx in range(8):  # 0 is left col on screen, 7 is right col
                y1 = int(rank_idx * sq_h)
                y2 = int((rank_idx + 1) * sq_h)
                x1 = int(file_idx * sq_w)
                x2 = int((file_idx + 1) * sq_w)
                
                sq_crop = board_img[y1:y2, x1:x2]
                if sq_crop.shape[0] != 64 or sq_crop.shape[1] != 64:
                    sq_crop = cv2.resize(sq_crop, (64, 64), interpolation=cv2.INTER_AREA)
                
                if orientation_white:
                    chess_rank = 7 - rank_idx
                    chess_file = file_idx
                else:
                    chess_rank = rank_idx
                    chess_file = 7 - file_idx
                
                sq_index = chess.square(chess_file, chess_rank)
                squares[sq_index] = sq_crop
                
        return squares

    def get_square_screen_center(self, sq_index: int, roi: Tuple[int, int, int, int], orientation_white: bool = True) -> Tuple[int, int]:
        """
        Returns (screen_x, screen_y) center pixel coordinate for a given square (0..63).
        """
        left, top, width, height = roi
        file_idx = chess.square_file(sq_index)
        rank_idx = chess.square_rank(sq_index)
        
        sq_w = width / 8.0
        sq_h = height / 8.0
        
        if orientation_white:
            col = file_idx
            row = 7 - rank_idx
        else:
            col = 7 - file_idx
            row = rank_idx
            
        center_x = int(left + (col + 0.5) * sq_w)
        center_y = int(top + (row + 0.5) * sq_h)
        return (center_x, center_y)

    def detect_orientation(self, board_img: np.ndarray) -> bool:
        """
        Attempts to detect whether white or black is at the bottom.
        Returns True for White at bottom, False for Black at bottom.
        """
        try:
            if len(board_img.shape) != 3:
                return True

            bh, bw = board_img.shape[:2]
            top_piece_brightness = []
            bottom_piece_brightness = []

            for screen_row in (0, 1, 6, 7):
                target = top_piece_brightness if screen_row < 2 else bottom_piece_brightness
                for screen_col in range(8):
                    y1 = int(screen_row * bh / 8.0)
                    y2 = int((screen_row + 1) * bh / 8.0)
                    x1 = int(screen_col * bw / 8.0)
                    x2 = int((screen_col + 1) * bw / 8.0)
                    square = board_img[y1:y2, x1:x2]
                    brightness = self._piece_foreground_brightness(square)
                    if brightness is not None:
                        target.append(brightness)

            if top_piece_brightness and bottom_piece_brightness:
                top_score = float(np.median(top_piece_brightness))
                bottom_score = float(np.median(bottom_piece_brightness))
                return bottom_score > top_score

            # Conservative fallback for sparse/endgame positions.
            gray = cv2.cvtColor(board_img, cv2.COLOR_BGR2GRAY)
            return bool(np.mean(gray[int(6 * bh / 8):, :]) >=
                        np.mean(gray[:int(2 * bh / 8), :]))
        except Exception:
            return True

    @staticmethod
    def _piece_foreground_brightness(square: np.ndarray) -> Optional[float]:
        """Return piece brightness independent of the square's board color."""
        if square is None or square.size == 0:
            return None

        gray = cv2.cvtColor(square, cv2.COLOR_BGR2GRAY)
        h, w = gray.shape
        corner = max(2, int(0.10 * min(h, w)))
        corners = np.concatenate((
            gray[:corner, :corner].ravel(),
            gray[:corner, -corner:].ravel(),
            gray[-corner:, :corner].ravel(),
            gray[-corner:, -corner:].ravel(),
        ))
        background = float(np.median(corners))
        margin_h = max(2, int(0.15 * h))
        margin_w = max(2, int(0.15 * w))
        inner = gray[margin_h:h - margin_h, margin_w:w - margin_w]
        foreground = np.abs(inner.astype(np.float32) - background) > 18.0
        if float(np.count_nonzero(foreground)) / foreground.size < 0.04:
            return None
        return float(np.mean(inner[foreground]))
