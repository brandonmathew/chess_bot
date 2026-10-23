import unittest
import cv2
import numpy as np
import chess
from src.board_detector import BoardDetector
from src.game_tracker import GameTracker, WHITE_PIECE, BLACK_PIECE

# Helper: plain dark-board image (no highlights) for occupancy-change testing
def _plain_board(h: int = 512, w: int = 512) -> np.ndarray:
    img = np.zeros((h, w, 3), dtype=np.uint8)
    sq_h, sq_w = h // 8, w // 8
    for r in range(8):
        for c in range(8):
            v = 190 if (r + c) % 2 == 0 else 55
            img[r*sq_h:(r+1)*sq_h, c*sq_w:(c+1)*sq_w] = v
    return img

class TestAdvancedTracking(unittest.TestCase):
    def setUp(self):
        self.detector = BoardDetector()
        self.tracker = GameTracker()
        self._board_img = _plain_board()

    def _create_empty_board_images(self):
        images = []
        for i in range(64):
            img = np.full((100, 100, 3), 100, dtype=np.uint8)
            images.append(img)
        return images

    def _render_position(self, board: chess.Board,
                         white_fill=(245, 245, 245),
                         black_fill=(15, 15, 15)) -> np.ndarray:
        """Render a themed board with simple, color-distinguishable pieces."""
        size = 512
        square = size // 8
        image = np.zeros((size, size, 3), dtype=np.uint8)
        for screen_row in range(8):
            for file_index in range(8):
                color = (181, 136, 88) if (screen_row + file_index) % 2 else (229, 218, 185)
                y1, x1 = screen_row * square, file_index * square
                image[y1:y1 + square, x1:x1 + square] = color

                rank_index = 7 - screen_row
                piece = board.piece_at(chess.square(file_index, rank_index))
                if piece:
                    center = (x1 + square // 2, y1 + square // 2)
                    fill = white_fill if piece.color == chess.WHITE else black_fill
                    cv2.circle(image, center, 19, fill, -1)
                    if piece.color == chess.WHITE:
                        cv2.circle(image, center, 19, (20, 20, 20), 2)
        return image

    def _draw_overlay(self, image: np.ndarray, from_sq: int, to_sq: int):
        def center(square_index):
            file_index = chess.square_file(square_index)
            rank_index = chess.square_rank(square_index)
            return (file_index * 64 + 32, (7 - rank_index) * 64 + 32)

        cv2.line(image, center(from_sq), center(to_sq), (102, 255, 0), 6)
        cv2.circle(image, center(from_sq), 18, (102, 255, 0), 3)
        cv2.circle(image, center(to_sq), 18, (102, 255, 0), 3)

    def test_castling_detection(self):
        # White O-O from starting position after e4, e5, Nf3, Nc6, Bc4, Bc5
        moves = ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"]
        for m in moves:
            self.tracker.board.push_san(m)

        # Now it's white's turn to castle O-O (e1->g1, h1->f1)
        base_squares = self._create_empty_board_images()
        # White king on e1, Rook on h1
        base_squares[chess.E1][:] = 255
        base_squares[chess.H1][:] = 255
        self.tracker.initialize_baseline(base_squares)

        # After castling: King on g1, Rook on f1, e1 and h1 are empty
        after_squares = [np.copy(sq) for sq in base_squares]
        after_squares[chess.E1][:] = 100
        after_squares[chess.H1][:] = 100
        after_squares[chess.G1][:] = 255
        after_squares[chess.F1][:] = 255

        # Apply move across 2 frames
        self.tracker.process_frame(self._board_img, after_squares)
        detected = self.tracker.process_frame(self._board_img, after_squares)

        self.assertEqual(detected, chess.Move.from_uci("e1g1"))
        self.assertEqual(self.tracker.board.turn, chess.BLACK)

    def test_en_passant_detection(self):
        # Setup position with possible en passant
        fen = "rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR b KQkq - 0 2"
        self.tracker.reset_game(fen=fen, orientation_white=True)

        base_squares = self._create_empty_board_images()
        base_squares[chess.F5][:] = 255  # White pawn on f5
        base_squares[chess.G7][:] = 255  # Black pawn on g7
        self.tracker.initialize_baseline(base_squares)

        # Black plays g7g5
        g5_squares = [np.copy(sq) for sq in base_squares]
        g5_squares[chess.G7][:] = 100
        g5_squares[chess.G5][:] = 255
        self.tracker.process_frame(self._board_img, g5_squares)
        self.tracker.process_frame(self._board_img, g5_squares)

        self.assertEqual(self.tracker.board.ep_square, chess.G6)

        # Now White captures en passant: f5g6 (affecting f5, g6, and captured pawn on g5)
        ep_squares = [np.copy(sq) for sq in g5_squares]
        ep_squares[chess.F5][:] = 100
        ep_squares[chess.G5][:] = 100  # captured pawn gone
        ep_squares[chess.G6][:] = 255  # white pawn on g6

        self.tracker.process_frame(self._board_img, ep_squares)
        detected = self.tracker.process_frame(self._board_img, ep_squares)

        self.assertEqual(detected, chess.Move.from_uci("f5g6"))

    def test_capture_onto_occupied_square(self):
        # Reproduce the stalled sequence: 1. e4 c5 2. d4 cxd4.
        self.tracker.reset_game()
        self.tracker.board.push_uci("e2e4")
        self.tracker.board.push_uci("c7c5")
        self.tracker.board.push_uci("d2d4")

        base_squares = self._create_empty_board_images()
        base_squares[chess.D4][:] = 255  # White pawn before capture
        base_squares[chess.C5][:] = 20   # Black pawn before capture
        self.tracker.initialize_baseline(base_squares)

        after_squares = [np.copy(sq) for sq in base_squares]
        after_squares[chess.C5][:] = 100  # c5 is now empty
        after_squares[chess.D4][:] = 20   # black pawn replaces white pawn

        self.tracker.process_frame(self._board_img, after_squares)
        detected = self.tracker.process_frame(self._board_img, after_squares)

        self.assertEqual(detected, chess.Move.from_uci("c5d4"))
        self.assertEqual(self.tracker.board.turn, chess.WHITE)

    def test_tracks_white_then_black_with_overlay_visible(self):
        detector = BoardDetector()
        position = chess.Board()
        initial = self._render_position(position)
        self.tracker.reset_game()
        self.tracker.initialize_baseline(detector.slice_board(initial, True))

        position.push_uci("e2e4")
        after_white = self._render_position(position)
        self._draw_overlay(after_white, chess.E2, chess.E4)
        white_squares = detector.slice_board(after_white, True)
        self.tracker.process_frame(after_white, white_squares)
        white_move = self.tracker.process_frame(after_white, white_squares)

        self.assertEqual(white_move, chess.Move.from_uci("e2e4"))
        self.assertEqual(self.tracker.board.turn, chess.BLACK)

        position.push_uci("d7d5")
        after_black = self._render_position(position)
        self._draw_overlay(after_black, chess.D7, chess.D5)
        black_squares = detector.slice_board(after_black, True)
        self.tracker.process_frame(after_black, black_squares)
        black_move = self.tracker.process_frame(after_black, black_squares)

        self.assertEqual(black_move, chess.Move.from_uci("d7d5"))
        self.assertEqual(self.tracker.board.turn, chess.WHITE)

    def test_auto_move_baseline_catches_instant_reply(self):
        # Auto mode advances the internal board before the opponent's visual
        # reply. The reply must be compared with the expected post-auto-move
        # occupancy instead of being swallowed as a new baseline.
        detector = BoardDetector()
        self.tracker.reset_game()
        self.tracker.apply_manual_move(chess.Move.from_uci("e2e4"))

        replied = chess.Board()
        replied.push_uci("e2e4")
        replied.push_uci("e7e5")
        reply_image = self._render_position(replied)
        reply_squares = detector.slice_board(reply_image, True)

        self.tracker.process_frame(reply_image, reply_squares)
        detected = self.tracker.process_frame(reply_image, reply_squares)

        self.assertEqual(detected, chess.Move.from_uci("e7e5"))
        self.assertEqual(self.tracker.board.turn, chess.WHITE)

    def test_transient_drag_is_not_a_new_game(self):
        position = chess.Board()
        position.push_uci("e2e4")
        position.push_uci("e7e5")
        self.tracker.reset_game(position.fen())
        self.tracker.move_history.append(chess.Move.from_uci("e7e5"))

        transient = self._render_position(position)
        # Simulate a dragged knight temporarily absent from g1.
        transient[7 * 64:(7 + 1) * 64, 6 * 64:(6 + 1) * 64] = (181, 136, 88)
        squares = BoardDetector().slice_board(transient, True)

        self.assertFalse(self.tracker._is_starting_board(squares))

    def test_black_orientation(self):
        self.tracker.reset_game(orientation_white=False)
        self.assertFalse(self.tracker.orientation_white)

        # In black view, e7 is rank 1 on screen (chess rank 6)
        roi = (0, 0, 800, 800)
        e7_center = self.detector.get_square_screen_center(chess.E7, roi, orientation_white=False)
        # col = 7 - 4 = 3 -> (3.5 * 100) = 350, row = rank 6 -> (6.5 * 100) = 650
        self.assertEqual(e7_center, (350, 650))

    def test_visual_orientation_detection_for_both_colors(self):
        white_bottom = self._render_position(chess.Board())
        black_bottom = np.rot90(white_bottom, 2).copy()

        self.assertTrue(self.detector.detect_orientation(white_bottom))
        self.assertFalse(self.detector.detect_orientation(black_bottom))

    def test_adapts_to_gray_piece_theme(self):
        themed = self._render_position(
            chess.Board(),
            white_fill=(240, 240, 240),
            black_fill=(85, 85, 85),
        )
        squares = self.detector.slice_board(themed, True)
        self.tracker.reset_game()
        self.tracker.initialize_baseline(squares)

        self.assertEqual(self.tracker._baseline_occ.count(WHITE_PIECE), 16)
        self.assertEqual(self.tracker._baseline_occ.count(BLACK_PIECE), 16)
        self.assertTrue(self.tracker._color_calibrated)

    def test_bright_empty_squares_are_not_pieces(self):
        # Light board themes can have empty squares well above the old 210
        # brightness cutoff. They must remain empty for desync counting.
        empty_light = np.full((64, 64, 3), 230, dtype=np.uint8)
        solid_white_piece = np.full((64, 64, 3), 255, dtype=np.uint8)

        self.assertEqual(self.tracker._classify(empty_light), 0)
        self.assertEqual(self.tracker._classify(solid_white_piece), 1)

    def test_green_overlay_is_not_a_piece(self):
        # mss captures the transparent overlay window too. A neon green arrow
        # over an empty square must not increase the visible piece count.
        board_square = np.full((64, 64, 3), (181, 136, 88), dtype=np.uint8)
        board_square[28:36, 8:56] = (102, 255, 0)  # BGR neon green overlay

        self.assertEqual(self.tracker._classify(board_square), 0)

    def test_green_overlay_is_not_a_move_highlight(self):
        # The engine arrow must not be mistaken for the site's last-move
        # highlight and committed as a legal move.
        board = _plain_board()
        board[300:340, 60:452] = (102, 255, 0)  # BGR neon green arrow
        self.tracker.reset_game()

        self.assertIsNone(self.tracker._detect_from_highlights(board))

if __name__ == "__main__":
    unittest.main()
