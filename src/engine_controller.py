import os
import sys
import zipfile
import io
from pathlib import Path
import requests
import chess
import chess.engine
from typing import Optional, Dict, Any, Tuple
from src.config import ENGINE_DIR

GITHUB_RELEASES_API = "https://api.github.com/repos/official-stockfish/Stockfish/releases/latest"

class EngineController:
    def __init__(self, engine_path: Optional[str] = None):
        self.engine_path = engine_path if (engine_path and os.path.exists(engine_path) and engine_path.lower().endswith(".exe")) else self._find_or_download_engine()
        self.engine: Optional[chess.engine.SimpleEngine] = None
        self.skill_level: int = 20

    def _find_or_download_engine(self) -> str:
        os.makedirs(ENGINE_DIR, exist_ok=True)
        # Check if any .exe exists in ENGINE_DIR
        for root, dirs, files in os.walk(ENGINE_DIR):
            for file in files:
                if file.lower().endswith(".exe") and "stockfish" in file.lower():
                    return os.path.join(root, file)
        
        # Check if stockfish.exe is in PATH
        import shutil
        path_in_env = shutil.which("stockfish.exe")
        if path_in_env and path_in_env.lower().endswith(".exe"):
            return path_in_env
            
        return ""

    def download_stockfish(self, progress_callback=None) -> str:
        """Downloads official Stockfish Windows binary to ENGINE_DIR."""
        os.makedirs(ENGINE_DIR, exist_ok=True)
        headers = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ChessMonger/1.0"}
        
        try:
            if progress_callback:
                progress_callback("Fetching latest Stockfish release info...")
            resp = requests.get(GITHUB_RELEASES_API, headers=headers, timeout=15)
            if resp.status_code == 200:
                data = resp.json()
                download_url = None
                for asset in data.get("assets", []):
                    name = asset.get("name", "").lower()
                    if "windows" in name and "x86-64" in name and name.endswith(".zip"):
                        download_url = asset.get("browser_download_url")
                        break
                
                if download_url:
                    if progress_callback:
                        progress_callback("Downloading Stockfish binary...")
                    r = requests.get(download_url, headers=headers, timeout=60)
                    if r.status_code == 200:
                        self._extract_archive_safely(r.content)
                        
                        found = self._find_or_download_engine()
                        if found:
                            self.engine_path = found
                            if progress_callback:
                                progress_callback("Stockfish engine installed successfully.")
                            return self.engine_path
        except Exception as e:
            print(f"Error fetching Stockfish from GitHub API: {e}")

        # Fallback direct link
        fallback_url = "https://github.com/official-stockfish/Stockfish/releases/download/sf_19/stockfish-windows-x86-64-universal.zip"
        try:
            r = requests.get(fallback_url, headers=headers, timeout=60)
            if r.status_code == 200:
                self._extract_archive_safely(r.content)
                found = self._find_or_download_engine()
                if found:
                    self.engine_path = found
                    return self.engine_path
        except Exception as e:
            print(f"Fallback download failed: {e}")
                
        raise RuntimeError("Failed to download Stockfish engine automatically. Please place stockfish.exe in the 'engine' folder.")

    def _extract_archive_safely(self, content: bytes):
        """Extract a downloaded archive without allowing path traversal."""
        destination = Path(ENGINE_DIR).resolve()
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            for member in archive.infolist():
                target = (destination / member.filename).resolve()
                if target != destination and destination not in target.parents:
                    raise RuntimeError(f"Unsafe archive member: {member.filename}")
            archive.extractall(destination)

    def start(self, skill_level: int = 20):
        if not self.engine_path or not os.path.exists(self.engine_path) or not self.engine_path.lower().endswith(".exe"):
            self.engine_path = self.download_stockfish()

        if self.engine is None:
            self.skill_level = skill_level
            self.engine = chess.engine.SimpleEngine.popen_uci(self.engine_path)
            try:
                self.engine.configure({"Skill Level": self.skill_level})
            except Exception:
                pass

    def stop(self):
        if self.engine:
            try:
                self.engine.quit()
            except Exception:
                pass
            self.engine = None

    def set_skill_level(self, skill_level: int):
        self.skill_level = max(0, min(20, skill_level))
        if self.engine:
            try:
                self.engine.configure({"Skill Level": self.skill_level})
            except Exception:
                pass

    def get_best_move(self, board: chess.Board, time_limit: float = 0.5, depth: Optional[int] = None) -> Tuple[Optional[chess.Move], Dict[str, Any]]:
        """
        Calculates the best move and position evaluation for the given board.
        Returns (best_move, info_dict)
        """
        if not self.engine:
            self.start(self.skill_level)

        limit = chess.engine.Limit(time=time_limit, depth=depth)
        info = self.engine.analyse(board, limit)
        
        best_move = None
        if "pv" in info and len(info["pv"]) > 0:
            best_move = info["pv"][0]
        else:
            result = self.engine.play(board, limit)
            best_move = result.move

        eval_str = "0.0"
        score = info.get("score")
        if score:
            relative = score.relative
            if relative.is_mate():
                eval_str = f"M{relative.mate()}"
            else:
                cp = relative.score()
                eval_str = f"{cp / 100.0:+.2f}" if cp is not None else "0.0"

        meta = {
            "score_str": eval_str,
            "depth": info.get("depth", 0),
            "pv": info.get("pv", []),
            "nodes": info.get("nodes", 0)
        }
        return best_move, meta
