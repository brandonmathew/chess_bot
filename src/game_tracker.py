"""
game_tracker.py — ChessMonger move detection engine.

Detection strategy (in priority order):
  1. HIGHLIGHT DETECTION – Read the yellow/green squares that Lichess/Chess.com
     paint on the board after every move.  These colours are vivid, definitive,
     and immune to piece-drag animations and overlay artefacts.
  2. OCCUPANCY CHANGE – Compare which squares changed from "has-piece" to
     "empty" (departure) and "empty" to "has-piece" (arrival).  Used when
     no highlight squares are visible (e.g. board theme without highlights).
  3. FIRST-FRAME CATCH-UP – On the very first captured frame, check for
     existing highlights (the last move's Lichess indicator may still be visible)
     and commit it immediately so tracking starts from the correct position.
  4. DESYNC DETECTION – Compare piece counts on screen vs. the tracker's
     known board.  If they differ significantly, fire an out_of_sync callback
     so the GUI can warn the user to paste the FEN.
"""

import cv2
import numpy as np
import chess
from typing import List, Optional, Callable

# ── Square occupancy labels ──────────────────────────────────────────────────
EMPTY       = 0
WHITE_PIECE = 1
BLACK_PIECE = 2

# ── Lichess / Chess.com highlight HSV ranges ─────────────────────────────────
# Yellow/gold last-move indicator (Lichess dark and light themes)
_YELLOW_LO = np.array([ 15, 55,  70], dtype=np.uint8)
_YELLOW_HI = np.array([ 48, 255, 255], dtype=np.uint8)

# Green "best move" / premove highlight (Lichess & Chess.com)
_GREEN_LO  = np.array([ 55, 45,  55], dtype=np.uint8)
_GREEN_HI  = np.array([100, 255, 255], dtype=np.uint8)

# Blue highlight (some Chess.com themes)
_BLUE_LO   = np.array([100, 45,  55], dtype=np.uint8)
_BLUE_HI   = np.array([135, 255, 255], dtype=np.uint8)

# Minimum fraction of a square's pixels that must match a highlight colour
_MIN_HIGHLIGHT_FRAC = 0.15

# How far off the piece count can be before we call it a desync
_DESYNC_PIECE_DELTA = 3


class GameTracker:
    def __init__(self, initial_fen: str = chess.STARTING_FEN,
                 on_desync: Optional[Callable[[str], None]] = None):
        """
        Parameters
        ----------
        initial_fen : starting FEN string
        on_desync   : optional callback(reason_str) called when the tracker
                      believes screen position no longer matches its board.
        """
        self.board = chess.Board(initial_fen)
        self.orientation_white: bool = True
        self.move_history: List[chess.Move] = []
        self.on_desync = on_desync

        # Occupancy baseline (list of EMPTY/WHITE_PIECE/BLACK_PIECE per square)
        self._baseline_occ: Optional[List[int]] = None
        self._color_threshold: float = 100.0
        self._color_calibrated: bool = False

        # Debounce state
        self._pending_move:  Optional[chess.Move] = None
        self._pending_count: int = 0
        self._required_conf: int = 2          # frames move must be stable

        # Desync tracking
        self._desync_warned: bool = False
        self._desync_count:  int  = 0
        self._frame_count:   int  = 0

    # ── Public API ────────────────────────────────────────────────────────────

    def reset_game(self, fen: str = chess.STARTING_FEN,
                   orientation_white: bool = True):
        self.board = chess.Board(fen)
        self.orientation_white = orientation_white
        self.move_history.clear()
        self._baseline_occ = None
        self._color_threshold = 100.0
        self._color_calibrated = False
        self._pending_move  = None
        self._pending_count = 0
        self._desync_warned = False
        self._desync_count  = 0
        self._frame_count   = 0

    def set_orientation(self, orientation_white: bool):
        self.orientation_white = orientation_white
        self._baseline_occ = None

    def initialize_baseline(self, squares: List[Optional[np.ndarray]]):
        """Snapshot current occupancy as the reference for future frames."""
        if not self._color_calibrated:
            self._calibrate_piece_colors(squares)
        self._baseline_occ = [self._classify(sq) for sq in squares]
        self._pending_move  = None
        self._pending_count = 0

    def apply_manual_move(self, move: chess.Move):
        """Push a move that was entered by the user or executed by the bot."""
        if move in self.board.legal_moves:
            self.board.push(move)
            self.move_history.append(move)
            # Install the expected post-move occupancy immediately. Fast
            # opponents (especially the Lichess analysis computer) may reply
            # before the next capture. Re-snapshotting that reply as a fresh
            # baseline would silently lose the opponent's move and stall.
            self._baseline_occ = self._board_occupancy()
            self._pending_move  = None
            self._pending_count = 0
            self._desync_warned = False
            self._desync_count = 0

    def apply_fen(self, fen: str):
        """Sync the tracker to an externally-provided FEN (user paste)."""
        try:
            self.board = chess.Board(fen)
            self._baseline_occ = None
            self._pending_move  = None
            self._pending_count = 0
            self._desync_warned = False
            self._desync_count = 0
            self.move_history.clear()
            print(f"[Tracker] FEN synced: {fen[:60]}")
        except Exception as e:
            print(f"[Tracker] Bad FEN: {e}")

    def process_frame(self,
                      board_img: np.ndarray,
                      squares:   List[Optional[np.ndarray]]) -> Optional[chess.Move]:
        """
        Analyse one frame.  Returns a confirmed chess.Move or None.

        Parameters
        ----------
        board_img : full board crop (used for HSV highlight detection)
        squares   : list of 64 square crops mapped by chess square index
        """
        if len(squares) != 64 or board_img is None:
            return None

        self._frame_count += 1

        # ── Auto-detect new game ──────────────────────────────────────────
        if len(self.move_history) > 0 and self._is_starting_board(squares):
            print("[Tracker] New game detected – resetting board state.")
            self.reset_game(orientation_white=self.orientation_white)
            self.initialize_baseline(squares)
            return None

        # ── First-frame: try highlight catch-up BEFORE storing baseline ───
        # If the user started tracking mid-game, Lichess still shows the
        # last-move highlights.  We try to commit that move immediately so
        # the tracker catches up by at least one ply.
        if self._baseline_occ is None:
            first_move = self._detect_from_highlights(board_img)
            if first_move is not None:
                print(f"[Tracker] First-frame highlight catch-up: {first_move.uci()}")
                self._commit_move(first_move, squares)
                return first_move          # immediately return; baseline set inside _commit_move

            # No highlights on first frame – just store baseline
            self.initialize_baseline(squares)

            # ── Desync check on first frame ───────────────────────────────
            self._check_desync(squares)
            return None

        # ── 1. Highlight-based detection ──────────────────────────────────
        candidate = self._detect_from_highlights(board_img)

        # ── 2. Occupancy-change fallback ──────────────────────────────────
        if candidate is None:
            candidate = self._detect_from_occupancy(squares)

        # ── 3. Debounce ───────────────────────────────────────────────────
        if candidate is None:
            self._pending_move  = None
            self._pending_count = 0
            # Periodic desync check every 30 frames (~2.5 s at 12 fps)
            if self._frame_count % 30 == 0:
                self._check_desync(squares)
            return None

        if candidate == self._pending_move:
            self._pending_count += 1
        else:
            self._pending_move  = candidate
            self._pending_count = 1

        if self._pending_count >= self._required_conf:
            self._commit_move(candidate, squares)
            return candidate

        return None

    # ── Detection helpers ─────────────────────────────────────────────────────

    def _detect_from_highlights(self,
                                 board_img: np.ndarray) -> Optional[chess.Move]:
        """
        Find highlighted squares (yellow / green / blue tint applied by the
        chess site after a move) and match them to a legal move.
        Returns the matching legal move, or None.
        """
        try:
            hsv = cv2.cvtColor(board_img, cv2.COLOR_BGR2HSV)
        except Exception:
            return None

        bh, bw = board_img.shape[:2]
        sq_h = bh / 8.0
        sq_w = bw / 8.0

        highlighted: List[int] = []

        for rank_idx in range(8):
            for file_idx in range(8):
                y1 = int(rank_idx * sq_h)
                y2 = int((rank_idx + 1) * sq_h)
                x1 = int(file_idx * sq_w)
                x2 = int((file_idx + 1) * sq_w)
                patch = hsv[y1:y2, x1:x2]

                if patch.size == 0:
                    continue

                mask_y = cv2.inRange(patch, _YELLOW_LO, _YELLOW_HI)
                mask_g = cv2.inRange(patch, _GREEN_LO,  _GREEN_HI)
                mask_b = cv2.inRange(patch, _BLUE_LO,   _BLUE_HI)
                combined = cv2.bitwise_or(mask_y, cv2.bitwise_or(mask_g, mask_b))

                # mss captures the desktop overlay. Remove its neon green
                # arrow/circles before interpreting colors as site highlights;
                # otherwise the engine suggestion can be committed as a real
                # chess move (for example b1c3).
                overlay_green = cv2.inRange(
                    patch,
                    np.array([40, 180, 180], dtype=np.uint8),
                    np.array([85, 255, 255], dtype=np.uint8),
                )
                combined[overlay_green > 0] = 0

                frac = float(np.count_nonzero(combined)) / combined.size
                if frac >= _MIN_HIGHLIGHT_FRAC:
                    if self.orientation_white:
                        chess_rank = 7 - rank_idx
                        chess_file = file_idx
                    else:
                        chess_rank = rank_idx
                        chess_file = 7 - file_idx
                    sq = chess.square(chess_file, chess_rank)
                    highlighted.append(sq)

        if len(highlighted) < 2:
            return None

        return self._match_highlights_to_move(highlighted)

    def _match_highlights_to_move(self,
                                   highlighted: List[int]) -> Optional[chess.Move]:
        """
        Given a list of ≥2 highlighted squares, find the legal move whose
        from/to squares best match.
        """
        legal  = list(self.board.legal_moves)
        hl_set = set(highlighted)

        best_move  = None
        best_score = -1

        for move in legal:
            involved = {move.from_square, move.to_square}
            if self.board.is_castling(move):
                if move.to_square == chess.G1:
                    involved |= {chess.H1, chess.F1}
                elif move.to_square == chess.C1:
                    involved |= {chess.A1, chess.D1}
                elif move.to_square == chess.G8:
                    involved |= {chess.H8, chess.F8}
                elif move.to_square == chess.C8:
                    involved |= {chess.A8, chess.D8}

            hits  = len(hl_set & involved)
            extra = len(hl_set - involved)
            score = hits * 2 - extra

            if score > best_score:
                best_score = score
                best_move  = move

        # Require both from_sq and to_sq to be highlighted
        if best_move is not None:
            if (best_move.from_square in hl_set and
                    best_move.to_square in hl_set):
                return best_move

        return None

    def _detect_from_occupancy(self,
                                squares: List[Optional[np.ndarray]]) -> Optional[chess.Move]:
        """
        Compare current occupancy against the stored baseline.
        Squares that changed occupied→empty are departure candidates.
        Squares that changed empty→occupied are arrival candidates.
        """
        if self._baseline_occ is None:
            return None

        curr_occ = [self._classify(sq) for sq in squares]

        departed: List[int] = []
        arrived:  List[int] = []

        for i in range(64):
            prev = self._baseline_occ[i]
            curr = curr_occ[i]
            if prev != EMPTY and curr == EMPTY:
                departed.append(i)
            elif curr != EMPTY and curr != prev:
                # Empty -> occupied is a normal arrival. Occupied -> the
                # opposite colour is a capture on the destination square.
                arrived.append(i)

        if not departed or not arrived:
            return None

        legal   = list(self.board.legal_moves)
        dep_set = set(departed)
        arr_set = set(arrived)

        best_move  = None
        best_score = -1

        for move in legal:
            score = 0

            if move.from_square in dep_set:
                score += 2
            if move.to_square in arr_set:
                score += 2

            # Castling rook bonus
            if self.board.is_castling(move):
                rk = {chess.G1: (chess.H1, chess.F1),
                      chess.C1: (chess.A1, chess.D1),
                      chess.G8: (chess.H8, chess.F8),
                      chess.C8: (chess.A8, chess.D8)}.get(move.to_square)
                if rk:
                    if rk[0] in dep_set: score += 1
                    if rk[1] in arr_set: score += 1

            # Penalise unexplained changes
            involved_dep = {move.from_square}
            involved_arr = {move.to_square}
            if self.board.is_castling(move):
                rk = {chess.G1: (chess.H1, chess.F1),
                      chess.C1: (chess.A1, chess.D1),
                      chess.G8: (chess.H8, chess.F8),
                      chess.C8: (chess.A8, chess.D8)}.get(move.to_square)
                if rk:
                    involved_dep.add(rk[0])
                    involved_arr.add(rk[1])

            extra = len(dep_set - involved_dep) + len(arr_set - involved_arr)
            score -= extra

            if score > best_score:
                best_score = score
                best_move  = move

        # Accept only if both from and to squares confirm
        if (best_move is not None and
                best_move.from_square in dep_set and
                best_move.to_square   in arr_set and
                best_score >= 3):
            return best_move

        return None

    # ── Desync detection ──────────────────────────────────────────────────────

    def _check_desync(self, squares: List[Optional[np.ndarray]]):
        """
        Compare piece counts visible on screen vs. tracker's internal board.
        Fires on_desync callback if they differ by more than _DESYNC_PIECE_DELTA.
        """
        if self._desync_warned or self.on_desync is None:
            return

        curr_occ  = [self._classify(sq) for sq in squares]
        screen_w  = sum(1 for o in curr_occ if o == WHITE_PIECE)
        screen_b  = sum(1 for o in curr_occ if o == BLACK_PIECE)
        screen_total = screen_w + screen_b

        board_total  = bin(self.board.occupied).count('1')

        expected_occ = {sq for sq in chess.SquareSet(self.board.occupied)}
        screen_occ = {i for i, value in enumerate(curr_occ) if value != EMPTY}
        position_mismatch = len(expected_occ ^ screen_occ)

        delta = abs(screen_total - board_total)
        mismatch = delta > _DESYNC_PIECE_DELTA or position_mismatch > 6
        if not mismatch:
            self._desync_count = 0
            return

        # Screenshot tools and move animations can obscure the board briefly.
        # Require repeated mismatches before showing a persistent warning.
        self._desync_count += 1
        if self._desync_count >= 3:
            msg = (f"Screen shows ~{screen_total} pieces "
                   f"but tracker expects {board_total} "
                   f"({position_mismatch} squares differ). "
                   f"Paste FEN from Lichess to sync position.")
            print(f"[Tracker] DESYNC: {msg}")
            self._desync_warned = True
            self.on_desync(msg)

    # ── Occupancy classification ──────────────────────────────────────────────

    def _classify(self, sq_crop: Optional[np.ndarray]) -> int:
        """Classify a square as EMPTY / WHITE_PIECE / BLACK_PIECE."""
        brightness = self._piece_brightness(sq_crop)
        if brightness is None:
            return EMPTY
        return (WHITE_PIECE if brightness >= self._color_threshold
                else BLACK_PIECE)

    def _piece_brightness(self,
                          sq_crop: Optional[np.ndarray]) -> Optional[float]:
        """Measure visible piece brightness, or return None for an empty square."""
        if sq_crop is None or sq_crop.size == 0:
            return None

        h, w = sq_crop.shape[:2]
        if h < 8 or w < 8:
            return None

        if len(sq_crop.shape) == 3:
            gray = cv2.cvtColor(sq_crop, cv2.COLOR_BGR2GRAY)
            hsv = cv2.cvtColor(sq_crop, cv2.COLOR_BGR2HSV)
            overlay_mask = cv2.inRange(
                hsv,
                np.array([35, 100, 100], dtype=np.uint8),
                np.array([95, 255, 255], dtype=np.uint8),
            ) > 0
            # Include anti-aliased pixels around arrow and circle edges.
            overlay_mask = cv2.dilate(
                overlay_mask.astype(np.uint8),
                np.ones((3, 3), dtype=np.uint8),
                iterations=1,
            ) > 0
        else:
            gray = sq_crop
            overlay_mask = np.zeros(gray.shape, dtype=bool)

        mean_bright = float(np.mean(gray))

        # Synthetic test fixtures and some flat piece sets can be completely
        # uniform. Preserve the unambiguous extreme cases.
        if float(np.std(gray)) < 1.0:
            if mean_bright > 252.0:
                return mean_bright
            if mean_bright < 35.0:
                return mean_bright
            return None

        # Estimate this square's own background from its corners. This adapts
        # to light/dark squares, last-move highlights, and different themes.
        corner_size = max(2, int(0.10 * min(h, w)))
        corners = np.concatenate((
            gray[:corner_size, :corner_size].ravel(),
            gray[:corner_size, -corner_size:].ravel(),
            gray[-corner_size:, :corner_size].ravel(),
            gray[-corner_size:, -corner_size:].ravel(),
        ))
        background_mean = float(np.median(corners))

        # Ignore labels near square edges and remove our neon overlay from the
        # foreground mask instead of discarding the entire covered square.
        margin_h = max(2, int(0.15 * h))
        margin_w = max(2, int(0.15 * w))
        inner = gray[margin_h:h - margin_h, margin_w:w - margin_w]
        inner_overlay = overlay_mask[margin_h:h - margin_h,
                                     margin_w:w - margin_w]
        foreground = (np.abs(inner.astype(np.float32) - background_mean) > 18.0)
        foreground &= ~inner_overlay

        foreground_fraction = float(np.count_nonzero(foreground)) / foreground.size
        if foreground_fraction < 0.04:
            return None

        return float(np.mean(inner[foreground]))

    def _calibrate_piece_colors(self,
                                squares: List[Optional[np.ndarray]]):
        """Learn the dark/light piece split for the current visual theme."""
        values = [self._piece_brightness(square) for square in squares]

        # Prefer supervised calibration from the authoritative chess position.
        # This handles themes where the same piece color appears brighter or
        # darker depending on the underlying square color.
        expected_dark = []
        expected_light = []
        for square_index, value in enumerate(values):
            if value is None:
                continue
            piece = self.board.piece_at(square_index)
            if piece is None:
                continue
            target = expected_light if piece.color == chess.WHITE else expected_dark
            target.append(value)

        if len(expected_dark) >= 4 and len(expected_light) >= 4:
            dark_center = float(np.median(expected_dark))
            light_center = float(np.median(expected_light))
            if light_center - dark_center >= 20.0:
                self._color_threshold = (dark_center + light_center) / 2.0
                self._color_calibrated = True
                print(f"[Vision] Piece colors calibrated: dark={dark_center:.1f}, "
                      f"light={light_center:.1f}, "
                      f"split={self._color_threshold:.1f}")
                return

        # Fallback for an unknown position: separate measured foregrounds into
        # two one-dimensional brightness groups.
        samples = np.array([value for value in values if value is not None],
                           dtype=np.float32)
        if samples.size < 8:
            return

        low = float(np.percentile(samples, 20))
        high = float(np.percentile(samples, 80))
        for _ in range(12):
            low_group = samples[np.abs(samples - low) <= np.abs(samples - high)]
            high_group = samples[np.abs(samples - low) > np.abs(samples - high)]
            if low_group.size < 4 or high_group.size < 4:
                return
            new_low = float(np.mean(low_group))
            new_high = float(np.mean(high_group))
            if abs(new_low - low) < 0.05 and abs(new_high - high) < 0.05:
                low, high = new_low, new_high
                break
            low, high = new_low, new_high

        if high - low < 20.0:
            return

        self._color_threshold = (low + high) / 2.0
        self._color_calibrated = True
        print(f"[Vision] Piece colors calibrated: dark={low:.1f}, "
              f"light={high:.1f}, split={self._color_threshold:.1f}")

    def _is_starting_board(self, squares: List[Optional[np.ndarray]]) -> bool:
        if len(squares) != 64:
            return False
        occ = [self._classify(sq) for sq in squares]
        white_home = sum(1 for i in range(16) if occ[i] == WHITE_PIECE)
        black_home = sum(1 for i in range(48, 64) if occ[i] == BLACK_PIECE)
        middle_occupied = sum(1 for i in range(16, 48) if occ[i] != EMPTY)

        # A genuine reset has no developed piece in ranks 3-6. The previous
        # threshold allowed up to six occupied middle squares, so ordinary
        # opening positions such as 1.e4 were incorrectly treated as a new
        # game and the tracker silently reset after the first move.
        # Require the exact 32-piece starting occupancy. During an automated
        # drag the moving piece briefly disappears, producing 15 home pieces;
        # accepting that transient frame caused destructive mid-game resets.
        return white_home == 16 and black_home == 16 and middle_occupied == 0

    def _board_occupancy(self) -> List[int]:
        """Return color occupancy derived from the authoritative chess board."""
        occupancy = [EMPTY] * 64
        for square_index, piece in self.board.piece_map().items():
            occupancy[square_index] = WHITE_PIECE if piece.color == chess.WHITE else BLACK_PIECE
        return occupancy

    # ── Commit ────────────────────────────────────────────────────────────────

    def _commit_move(self, move: chess.Move,
                     squares: List[Optional[np.ndarray]]):
        """Push the confirmed move and snapshot new baseline."""
        self.board.push(move)
        self.move_history.append(move)
        self._desync_warned = False       # reset desync flag after good detection
        self._desync_count = 0
        self.initialize_baseline(squares)
        side = 'White' if self.board.turn == chess.WHITE else 'Black'
        print(f"[Tracker] Committed: {move.uci()}  ({side} to move next)")
