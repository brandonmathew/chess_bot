import unittest
import numpy as np
import chess
from src.board_detector import BoardDetector
from src.game_tracker import GameTracker
from src.engine_controller import EngineController

class TestChessVision(unittest.TestCase):
    def setUp(self):
        self.detector = BoardDetector()
        self.tracker = GameTracker()

    def test_board_slicing(self):
        # Create a synthetic 800x800 test chessboard image
        img = np.zeros((800, 800, 3), dtype=np.uint8)
        for r in range(8):
            for c in range(8):
                if (r + c) % 2 == 0:
                    img[r*100:(r+1)*100, c*100:(c+1)*100] = 240
                else:
                    img[r*100:(r+1)*100, c*100:(c+1)*100] = 60

        squares = self.detector.slice_board(img, orientation_white=True)
        self.assertEqual(len(squares), 64)
        for sq in squares:
            self.assertEqual(sq.shape, (64, 64, 3))

    def test_coordinate_mapping(self):
        roi = (100, 100, 800, 800)
        # In white orientation, e4 is file 4 (e), rank 3 (4) -> x = 100 + 4.5*100 = 550, y = 100 + (7-3 + 0.5)*100 = 550
        e4_center = self.detector.get_square_screen_center(chess.E4, roi, orientation_white=True)
        self.assertEqual(e4_center, (550, 550))

        # A1 center
        a1_center = self.detector.get_square_screen_center(chess.A1, roi, orientation_white=True)
        self.assertEqual(a1_center, (150, 850))

    def test_game_tracker_move_detection(self):
        # Create initial synthetic board
        board_img_1 = np.zeros((800, 800, 3), dtype=np.uint8)
        for r in range(8):
            for c in range(8):
                color = 200 if (r + c) % 2 == 0 else 50
                board_img_1[r*100:(r+1)*100, c*100:(c+1)*100] = color

        # Add piece on e2 (rank 6 on screen, col 4 on screen)
        board_img_1[600:700, 400:500] = 255
        squares_1 = self.detector.slice_board(board_img_1, orientation_white=True)

        self.tracker.reset_game(orientation_white=True)
        self.tracker.initialize_baseline(squares_1)

        # Move e2 to e4: piece removed from e2, placed on e4 (rank 4 on screen, col 4 on screen)
        board_img_2 = np.copy(board_img_1)
        board_img_2[600:700, 400:500] = 200  # Reset e2 square
        board_img_2[400:500, 400:500] = 255  # Piece on e4

        squares_2 = self.detector.slice_board(board_img_2, orientation_white=True)

        # board_img_2 has no highlight colours, so occupancy-change path is used
        self.tracker.process_frame(board_img_2, squares_2)
        m = self.tracker.process_frame(board_img_2, squares_2)

        self.assertEqual(m, chess.Move.from_uci("e2e4"))
        self.assertEqual(self.tracker.board.turn, chess.BLACK)

    def test_stockfish_engine(self):
        engine = EngineController()
        engine.start(skill_level=10)
        
        board = chess.Board()
        best_move, meta = engine.get_best_move(board, time_limit=0.3, depth=10)
        
        self.assertIsNotNone(best_move)
        self.assertTrue(best_move in board.legal_moves)
        self.assertIn("score_str", meta)
        
        engine.stop()

if __name__ == "__main__":
    unittest.main()
