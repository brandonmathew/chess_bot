import os
import time
import random
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import customtkinter as ctk
import chess
import cv2
import numpy as np
from typing import Optional, Tuple

from src.config import AppConfig
from src.screen_capture import ScreenCapture
from src.board_detector import BoardDetector
from src.game_tracker import GameTracker
from src.engine_controller import EngineController
from src.move_executor import MoveExecutor
from src.overlay import ChessOverlay
from src.sniper import RegionSelector

# Appearance settings
ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

class ChessMongerGUI:
    def __init__(self, root: ctk.CTk):
        self.root = root
        self.root.title("♟️ GambitChess Bot")
        self.root.geometry("640x780")
        self.root.minsize(580, 700)

        # Core Components
        self.config = AppConfig.load()
        self.capture = ScreenCapture()
        self.detector = BoardDetector()
        self.tracker = GameTracker(on_desync=self._on_tracker_desync)
        self.engine = EngineController(self.config.stockfish_path)
        self.executor = MoveExecutor(self.detector)
        self.overlay = ChessOverlay(self.detector)
        self.sniper = RegionSelector(self.on_board_region_selected)

        # Worker Thread control
        self.is_running = False
        self.worker_thread: Optional[threading.Thread] = None
        self.last_suggested_move: Optional[chess.Move] = None

        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _build_ui(self):
        # Header Banner
        header = ctk.CTkFrame(self.root, corner_radius=10, fg_color="#1a1d24")
        header.pack(fill="x", padx=16, pady=(16, 8))

        title_lbl = ctk.CTkLabel(
            header, text="♟️ GambitChess Bot",
            font=ctk.CTkFont(size=20, weight="bold"),
            text_color="#00FFCC"
        )
        title_lbl.pack(pady=(10, 2))
        subtitle_lbl = ctk.CTkLabel(
            header, text="Screen Vision & Stockfish Engine Translation",
            font=ctk.CTkFont(size=12), text_color="#8892b0"
        )
        subtitle_lbl.pack(pady=(0, 10))

        # Main Scrollable / Stacked Container
        main_frame = ctk.CTkScrollableFrame(self.root, corner_radius=10)
        main_frame.pack(fill="both", expand=True, padx=16, pady=8)

        # 1. Vision & Board Calibration Section
        calib_card = ctk.CTkFrame(main_frame, corner_radius=8)
        calib_card.pack(fill="x", pady=6, padx=6)

        ctk.CTkLabel(calib_card, text="1. Board Calibration", font=ctk.CTkFont(size=14, weight="bold")).pack(anchor="w", padx=12, pady=(8, 4))
        
        btn_row = ctk.CTkFrame(calib_card, fg_color="transparent")
        btn_row.pack(fill="x", padx=12, pady=6)

        self.btn_auto_detect = ctk.CTkButton(
            btn_row, text="🔍 Auto-Detect Board",
            command=self.auto_detect_board,
            fg_color="#2b5278", hover_color="#386899"
        )
        self.btn_auto_detect.pack(side="left", fill="x", expand=True, padx=(0, 6))

        self.btn_manual_select = ctk.CTkButton(
            btn_row, text="🎯 Manual Select (Box)",
            command=self.manual_select_board,
            fg_color="#3d4450", hover_color="#4f5867"
        )
        self.btn_manual_select.pack(side="right", fill="x", expand=True, padx=(6, 0))

        self.lbl_board_status = ctk.CTkLabel(
            calib_card,
            text=f"Board Region: {self.config.board_roi or 'Not configured'}",
            font=ctk.CTkFont(size=11), text_color="#a0aec0"
        )
        self.lbl_board_status.pack(anchor="w", padx=12, pady=(0, 8))

        # 2. Live Game Information Card
        game_card = ctk.CTkFrame(main_frame, corner_radius=8, fg_color="#1e222b")
        game_card.pack(fill="x", pady=6, padx=6)

        ctk.CTkLabel(game_card, text="2. Live Match Status", font=ctk.CTkFont(size=14, weight="bold"), text_color="#64ffda").pack(anchor="w", padx=12, pady=(8, 4))

        # Desync warning banner (hidden by default)
        self.desync_frame = ctk.CTkFrame(game_card, corner_radius=6, fg_color="#7b2d00")
        self.lbl_desync = ctk.CTkLabel(
            self.desync_frame,
            text="⚠️ Position mismatch detected — paste FEN below to sync",
            font=ctk.CTkFont(size=11), text_color="#FFD580", wraplength=480
        )
        self.lbl_desync.pack(padx=10, pady=6)
        # Don't pack desync_frame yet — only show when needed

        info_grid = ctk.CTkFrame(game_card, fg_color="transparent")
        info_grid.pack(fill="x", padx=12, pady=4)

        # Row 1: Side & Turn
        self.lbl_turn = ctk.CTkLabel(info_grid, text="Turn: White to move", font=ctk.CTkFont(size=13))
        self.lbl_turn.grid(row=0, column=0, sticky="w", pady=2)

        self.lbl_last_move = ctk.CTkLabel(info_grid, text="Last Move: None", font=ctk.CTkFont(size=13))
        self.lbl_last_move.grid(row=0, column=1, sticky="e", pady=2)
        info_grid.grid_columnconfigure(0, weight=1)
        info_grid.grid_columnconfigure(1, weight=1)

        # Row 2: Evaluation & Best Move
        eval_frame = ctk.CTkFrame(game_card, corner_radius=6, fg_color="#12151c")
        eval_frame.pack(fill="x", padx=12, pady=8)

        self.lbl_eval = ctk.CTkLabel(
            eval_frame, text="Eval: +0.00",
            font=ctk.CTkFont(size=16, weight="bold"), text_color="#00FF66"
        )
        self.lbl_eval.pack(side="left", padx=16, pady=8)

        self.lbl_best_move = ctk.CTkLabel(
            eval_frame, text="Best Move: --",
            font=ctk.CTkFont(size=16, weight="bold"), text_color="#FFD700"
        )
        self.lbl_best_move.pack(side="right", padx=16, pady=8)

        # Orientation & Side selection
        side_row = ctk.CTkFrame(game_card, fg_color="transparent")
        side_row.pack(fill="x", padx=12, pady=(0, 6))

        ctk.CTkLabel(side_row, text="Playing As:").pack(side="left", padx=(0, 8))
        self.side_var = ctk.StringVar(value=self.config.player_color)
        self.side_menu = ctk.CTkSegmentedButton(
            side_row, values=["white", "black", "auto"],
            variable=self.side_var, command=self.on_side_changed
        )
        self.side_menu.pack(side="left", fill="x", expand=True)

        # FEN / Move sync section
        fen_label_row = ctk.CTkFrame(game_card, fg_color="transparent")
        fen_label_row.pack(fill="x", padx=12, pady=(4, 0))
        ctk.CTkLabel(
            fen_label_row,
            text="📋 Sync: paste FEN (from Lichess Share→FEN) or type a UCI move",
            font=ctk.CTkFont(size=10), text_color="#718096"
        ).pack(anchor="w")

        sync_row = ctk.CTkFrame(game_card, fg_color="transparent")
        sync_row.pack(fill="x", padx=12, pady=(2, 10))

        self.entry_sync_move = ctk.CTkEntry(
            sync_row,
            placeholder_text="Paste FEN  —or—  UCI move (e.g. e2e4)",
            height=30
        )
        self.entry_sync_move.pack(side="left", fill="x", expand=True, padx=(0, 6))
        self.entry_sync_move.bind("<Return>", lambda e: self.manual_push_move())

        btn_push_sync = ctk.CTkButton(
            sync_row, text="Sync", width=70, height=30,
            fg_color="#3182ce", hover_color="#2b6cb0",
            command=self.manual_push_move
        )
        btn_push_sync.pack(side="right")


        # 3. Settings & Engine Controls
        settings_card = ctk.CTkFrame(main_frame, corner_radius=8)
        settings_card.pack(fill="x", pady=6, padx=6)

        ctk.CTkLabel(settings_card, text="3. Engine & Bot Settings", font=ctk.CTkFont(size=14, weight="bold")).pack(anchor="w", padx=12, pady=(8, 4))

        # Mode Selection: Visual Overlay vs Autonomous Auto-Play
        mode_row = ctk.CTkFrame(settings_card, fg_color="transparent")
        mode_row.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(mode_row, text="Operating Mode:").pack(side="left", padx=(0, 8))
        self.mode_var = ctk.StringVar(value=self.config.mode)
        self.mode_menu = ctk.CTkSegmentedButton(
            mode_row, values=["visual", "auto"],
            variable=self.mode_var, command=self.on_mode_changed
        )
        self.mode_menu.pack(side="left", fill="x", expand=True)

        # Skill Level Slider
        skill_row = ctk.CTkFrame(settings_card, fg_color="transparent")
        skill_row.pack(fill="x", padx=12, pady=4)
        self.lbl_skill = ctk.CTkLabel(skill_row, text=f"Stockfish Skill: {self.config.engine_skill_level} / 20")
        self.lbl_skill.pack(side="left")
        self.slider_skill = ctk.CTkSlider(
            skill_row, from_=0, to=20, number_of_steps=20,
            command=self.on_skill_changed
        )
        self.slider_skill.set(self.config.engine_skill_level)
        self.slider_skill.pack(side="right", fill="x", expand=True, padx=(12, 0))

        # Engine Depth Slider
        depth_row = ctk.CTkFrame(settings_card, fg_color="transparent")
        depth_row.pack(fill="x", padx=12, pady=4)
        self.lbl_depth = ctk.CTkLabel(depth_row, text=f"Search Depth: {self.config.engine_depth}")
        self.lbl_depth.pack(side="left")
        self.slider_depth = ctk.CTkSlider(
            depth_row, from_=5, to=25, number_of_steps=20,
            command=self.on_depth_changed
        )
        self.slider_depth.set(self.config.engine_depth)
        self.slider_depth.pack(side="right", fill="x", expand=True, padx=(12, 0))

        # Humanized mouse checkbox
        self.chk_humanize = ctk.CTkCheckBox(
            settings_card, text="Humanized Mouse Curves & Realistic Delays",
            command=self.on_humanize_toggled
        )
        if self.config.humanize_mouse:
            self.chk_humanize.select()
        self.chk_humanize.pack(anchor="w", padx=12, pady=(6, 10))

        # 4. Master Action Button
        action_frame = ctk.CTkFrame(self.root, fg_color="transparent")
        action_frame.pack(fill="x", padx=16, pady=(4, 16))

        self.btn_toggle_tracker = ctk.CTkButton(
            action_frame, text="▶ START LIVE TRACKING",
            font=ctk.CTkFont(size=15, weight="bold"),
            height=44,
            fg_color="#00C853", hover_color="#00E676",
            command=self.toggle_tracking
        )
        self.btn_toggle_tracker.pack(side="left", fill="x", expand=True, padx=(0, 6))

        self.btn_reset_game = ctk.CTkButton(
            action_frame, text="🔄 Reset Board",
            font=ctk.CTkFont(size=13),
            height=44, width=120,
            fg_color="#c53030", hover_color="#e53e3e",
            command=self.reset_game
        )
        self.btn_reset_game.pack(side="right", padx=(6, 0))

    # --- Callbacks & Handlers ---

    def auto_detect_board(self):
        self.lbl_board_status.configure(text="Scanning screen for chess board...")
        self.root.update()
        time.sleep(0.2)

        # Search every physical monitor and translate monitor-local detector
        # coordinates back into the Windows virtual-desktop coordinate space.
        candidates = []
        for monitor_index in self.capture.monitor_indices():
            try:
                screen_img, (origin_x, origin_y) = \
                    self.capture.grab_screen_with_origin(monitor_index)
                local_roi = self.detector.auto_detect_board(screen_img)
                if local_roi:
                    x, y, width, height = local_roi
                    candidates.append((
                        width * height,
                        (x + origin_x, y + origin_y, width, height),
                    ))
            except Exception as e:
                print(f"Monitor {monitor_index} board scan failed: {e}")

        roi = max(candidates, key=lambda item: item[0])[1] if candidates else None
        if roi:
            self.on_board_region_selected(roi)
            messagebox.showinfo("Success", f"Chess board detected at screen coordinates: {roi}")
        else:
            messagebox.showwarning("Not Found", "Could not auto-detect chess board. Please make sure the chess board is visible on screen or use 'Manual Select (Box)'.")
            self.lbl_board_status.configure(text=f"Board Region: {self.config.board_roi or 'Not configured'}")

    def manual_select_board(self):
        self.sniper.start(self.root)

    def get_effective_orientation(self, board_img: Optional[np.ndarray] = None) -> bool:
        """Returns True if White is at bottom, False if Black is at bottom."""
        if self.config.player_color == "white":
            return True
        elif self.config.player_color == "black":
            return False
        else:  # "auto"
            if board_img is None and self.config.board_roi:
                try:
                    board_img = self.capture.grab_region(tuple(self.config.board_roi))
                except Exception:
                    pass
            if board_img is not None:
                return self.detector.detect_orientation(board_img)
            return self.tracker.orientation_white

    def on_board_region_selected(self, roi: Tuple[int, int, int, int],
                                 allow_expand: bool = False):
        # Contour detection can include a thin border around the board. Snap
        # the selection to the internal 8x8 grid so every square crop stays
        # aligned and neighboring pieces cannot bleed into empty squares.
        try:
            capture_roi = roi
            if allow_expand:
                # Recover older saved regions that were cropped a few pixels
                # inside the board. Refinement can then expand as well as trim.
                pad = max(6, int(min(roi[2], roi[3]) * 0.015))
                capture_roi = (
                    roi[0] - pad,
                    roi[1] - pad,
                    roi[2] + 2 * pad,
                    roi[3] + 2 * pad,
                )

            initial_img = self.capture.grab_region(capture_roi)
            local_box = self.detector._refine_board_box(
                cv2.cvtColor(initial_img, cv2.COLOR_BGR2GRAY),
                (0, 0, initial_img.shape[1], initial_img.shape[0]),
            )
            local_x, local_y, refined_w, refined_h = local_box
            roi = (capture_roi[0] + local_x, capture_roi[1] + local_y,
                   refined_w, refined_h)
        except Exception as e:
            print(f"Board region refinement failed: {e}")

        self.config.board_roi = list(roi)
        self.config.save()
        self.lbl_board_status.configure(text=f"Board Region: {roi}")
        
        board_img = self.capture.grab_region(roi)
        is_white = self.get_effective_orientation(board_img)

        self.tracker.set_orientation(is_white)
        if self.config.player_color == "auto":
            detected_color = "White" if is_white else "Black"
            print(f"[Vision] Auto color detected: {detected_color} at bottom")
        self.overlay.update_roi(roi, is_white)

    def on_side_changed(self, value):
        self.config.player_color = value
        self.config.save()
        is_white = self.get_effective_orientation()
        self.tracker.set_orientation(is_white)
        if self.config.board_roi:
            self.overlay.update_roi(tuple(self.config.board_roi), is_white)
        if self.is_running:
            threading.Thread(target=self._calculate_and_display_engine_move, daemon=True).start()

    def on_mode_changed(self, value):
        self.config.mode = value
        self.config.save()

        if value == "auto":
            # Auto mode must keep the captured board clean. Mouse execution
            # already targets square centers and does not need an arrow.
            self.overlay.clear()
            self.overlay.hide()
        elif self.is_running and self.config.board_roi and self.config.show_overlay:
            roi = tuple(self.config.board_roi)
            self.overlay.show(self.root, roi, self.get_effective_orientation())
            threading.Thread(
                target=self._calculate_and_display_engine_move,
                daemon=True,
            ).start()

    def on_skill_changed(self, value):
        val = int(value)
        self.lbl_skill.configure(text=f"Stockfish Skill: {val} / 20")
        self.config.engine_skill_level = val
        self.config.save()
        self.engine.set_skill_level(val)

    def on_depth_changed(self, value):
        val = int(value)
        self.lbl_depth.configure(text=f"Search Depth: {val}")
        self.config.engine_depth = val
        self.config.save()

    def on_humanize_toggled(self):
        self.config.humanize_mouse = bool(self.chk_humanize.get())
        self.config.save()

    def _on_tracker_desync(self, reason: str):
        """Called from background thread when tracker detects position mismatch."""
        def _show():
            self.lbl_desync.configure(text=f"⚠️ {reason}")
            # Pack the banner if not already visible
            try:
                if not self.desync_frame.winfo_ismapped():
                    self.desync_frame.pack(fill="x", padx=12, pady=(0, 4))
            except Exception:
                pass
        self.root.after(0, _show)

    def _hide_desync_banner(self):
        try:
            self.desync_frame.pack_forget()
        except Exception:
            pass

    def manual_push_move(self):
        text = self.entry_sync_move.get().strip()
        if not text:
            return
        try:
            # If user pasted a full FEN string
            if "/" in text and len(text) > 15:
                self.tracker.apply_fen(text)
                self.entry_sync_move.delete(0, "end")
                self._hide_desync_banner()
                turn_str = "White to move" if self.tracker.board.turn == chess.WHITE else "Black to move"
                self.lbl_turn.configure(text=f"Turn: {turn_str}")
                self.lbl_last_move.configure(text="✅ Position Synced from FEN")
                threading.Thread(target=self._calculate_and_display_engine_move, daemon=True).start()
                return

            # Treat as UCI move (e.g. e7e6) or standard algebraic notation (e.g. e6, Nf6)
            move = None
            try:
                move = chess.Move.from_uci(text.lower())
            except Exception:
                try:
                    move = self.tracker.board.parse_san(text)
                except Exception:
                    pass

            if move and move in self.tracker.board.legal_moves:
                self.tracker.apply_manual_move(move)
                self._hide_desync_banner()
                self._on_move_registered(move)
                self.entry_sync_move.delete(0, "end")
                threading.Thread(target=self._calculate_and_display_engine_move, daemon=True).start()
            else:
                messagebox.showerror("Illegal Move", f"'{text}' is not a legal move in the current position.")
        except Exception as e:
            messagebox.showerror("Invalid Input", f"Could not parse move or FEN '{text}': {e}")

    def reset_game(self):
        is_white = self.get_effective_orientation()
        self.tracker.reset_game(orientation_white=is_white)
        self.last_suggested_move = None
        self._hide_desync_banner()
        self.overlay.clear()
        self.lbl_turn.configure(text="Turn: White to move")
        self.lbl_last_move.configure(text="Last Move: None")
        self.lbl_eval.configure(text="Eval: +0.00")
        self.lbl_best_move.configure(text="Best Move: --")
        if self.is_running:
            threading.Thread(target=self._calculate_and_display_engine_move, daemon=True).start()

    def toggle_tracking(self):
        if not self.is_running:
            if not self.config.board_roi:
                messagebox.showerror("Error", "Please detect or calibrate the chess board region first.")
                return
            self.start_tracking()
        else:
            self.stop_tracking()

    def start_tracking(self):
        # Revalidate saved/manual calibration before each run. This also fixes
        # older config values that included a thin border around the board.
        if self.config.board_roi:
            self.on_board_region_selected(
                tuple(self.config.board_roi),
                allow_expand=True,
            )

        self.is_running = True
        self.btn_toggle_tracker.configure(
            text="⏹ STOP TRACKING",
            fg_color="#c53030", hover_color="#e53e3e"
        )
        
        roi = tuple(self.config.board_roi)
        is_white = self.get_effective_orientation()
        if self.config.mode == "visual" and self.config.show_overlay:
            self.overlay.show(self.root, roi, is_white)
        else:
            self.overlay.clear()
            self.overlay.hide()
        
        self.worker_thread = threading.Thread(target=self._tracking_loop, daemon=True)
        self.worker_thread.start()

    def stop_tracking(self):
        self.is_running = False
        self.btn_toggle_tracker.configure(
            text="▶ START LIVE TRACKING",
            fg_color="#00C853", hover_color="#00E676"
        )
        self.overlay.hide()

    def _tracking_loop(self):
        """Continuous background loop capturing screen and updating chess state."""
        try:
            self.engine.start(self.config.engine_skill_level)
        except Exception as e:
            print(f"Engine start error: {e}")

        roi = tuple(self.config.board_roi)
        is_white = self.get_effective_orientation()

        # Step 1: Capture clean baseline BEFORE rendering any overlay arrow
        try:
            clean_board_img = self.capture.grab_region(roi)
            initial_squares = self.detector.slice_board(clean_board_img, is_white)
            self.tracker.initialize_baseline(initial_squares)
        except Exception as e:
            print(f"Error capturing initial baseline: {e}")

        # Step 2: Now calculate and display engine recommendation for the player
        self._calculate_and_display_engine_move()

        while self.is_running:
            try:
                # Capture current board crop
                board_img = self.capture.grab_region(roi)
                squares = self.detector.slice_board(board_img, is_white)

                # Check for move — new API requires board_img for highlight detection
                detected_move = self.tracker.process_frame(board_img, squares)
                if detected_move:
                    print(f"[Vision] Move detected: {detected_move.uci()}")
                    self.root.after(0, self._on_move_registered, detected_move)

                    # Compute best move for new position
                    self._calculate_and_display_engine_move()

                time.sleep(0.08)  # ~12 FPS screen vision check
            except Exception as e:
                print(f"Tracking error: {e}")
                time.sleep(0.2)

    def _calculate_and_display_engine_move(self):
        if self.tracker.board.is_game_over():
            self.root.after(0, lambda: self.lbl_eval.configure(text="Game Over!"))
            return

        is_white_turn = (self.tracker.board.turn == chess.WHITE)
        color = self.config.player_color
        if color == "white":
            is_our_turn = is_white_turn
        elif color == "black":
            is_our_turn = not is_white_turn
        else:
            # In auto-color mode, the side at the bottom is the player. Never
            # calculate or execute the opponent's moves.
            is_our_turn = (is_white_turn == self.tracker.orientation_white)

        # If it's opponent's turn, clear overlay and wait
        if not is_our_turn:
            self.last_suggested_move = None
            self.root.after(0, self._set_opponent_waiting_ui)
            return

        try:
            best_move, meta = self.engine.get_best_move(
                self.tracker.board,
                time_limit=self.config.engine_move_time,
                depth=self.config.engine_depth
            )
            self.last_suggested_move = best_move
            
            # Update GUI & Overlay
            self.root.after(0, lambda: self._update_engine_ui(best_move, meta))
            
            # If auto-play mode is active and it's player's turn
            if self.config.mode == "auto" and is_our_turn and best_move:
                delay = random.uniform(self.config.auto_move_delay_min, self.config.auto_move_delay_max)
                time.sleep(delay)
                
                if self.is_running:
                    roi = tuple(self.config.board_roi)
                    is_white = self.tracker.orientation_white
                    self.executor.execute_move(best_move, roi, is_white, humanize=self.config.humanize_mouse)
                    self.tracker.apply_manual_move(best_move)
                    self.root.after(0, self._on_move_registered, best_move)
        except Exception as e:
            print(f"Engine calculation error: {e}")

    def _set_opponent_waiting_ui(self):
        turn_str = "Black (Opponent)" if self.tracker.board.turn == chess.BLACK else "White (Opponent)"
        self.lbl_turn.configure(text=f"Turn: {turn_str} to move")
        self.lbl_best_move.configure(text="Best Move: Waiting for opponent...")
        self.overlay.clear()

    def _update_engine_ui(self, best_move: Optional[chess.Move], meta: dict):
        eval_score = meta.get("score_str", "+0.00")
        self.lbl_eval.configure(text=f"Eval: {eval_score}")
        turn_str = "White to move" if self.tracker.board.turn == chess.WHITE else "Black to move"
        self.lbl_turn.configure(text=f"Turn: {turn_str}")
        
        if best_move:
            self.lbl_best_move.configure(text=f"Best Move: {best_move.uci()}")
            if self.config.mode == "visual" and self.config.show_overlay:
                self.overlay.draw_move_suggestion(best_move, eval_score)
            else:
                self.overlay.clear()
                self.overlay.hide()
        else:
            self.lbl_best_move.configure(text="Best Move: --")
            self.overlay.clear()

    def _on_move_registered(self, move: chess.Move):
        self.lbl_last_move.configure(text=f"Last Move: {move.uci()}")

    def on_close(self):
        self.is_running = False
        self.overlay.hide()
        self.engine.stop()
        self.capture.close()
        self.root.destroy()
