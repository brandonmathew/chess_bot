# ♟️ ChessMonger Desktop

A standalone, screen-based chess vision bot and engine assistant designed to work with **any desktop chess application** (Chess.com Desktop, Lichess, Arena, Fritz, ChessBase, etc.) or any chess board on your screen.

Unlike browser extensions that inspect DOM/canvas elements, **ChessMonger Desktop** uses real-time **Computer Vision** to detect the chess board directly from screen pixels, track moves with differential visual analysis, calculate optimal moves using **Stockfish**, and provide visual overlay suggestions or autonomous humanized auto-play.

---

## 🚀 Features

- **Screen-Based Vision (DOM-Independent):** Reads the board directly from screen frames using OpenCV and `mss`. Works with native desktop apps, electron apps, emulators, and browsers.
- **Differential Move Recognition:** Analyzes square delta against `python-chess` legal move trees. 100% immune to custom piece themes, 3D pieces, custom boards, and anti-cheat DOM obfuscations. Handles standard moves, captures, castling ($O-O$ and $O-O-O$), en passant, and promotions.
- **Auto-Detection & Interactive Sniper Calibration:** Auto-detects 8x8 chess boards on screen or lets you click-and-drag a box to lock onto any board region.
- **Stockfish Engine Integration:** Integrated official Stockfish 19 UCI engine with configurable search depth (5–25), skill level (0–20), and evaluation display.
- **Transparent Click-Through Visual Overlay:** Renders neon arrows and evaluation badges directly on top of the screen board without interfering with mouse clicks.
- **Humanized Auto-Play:** Executes moves using smooth Bezier mouse curves, randomized square offset jitter, and natural human thinking delays.
- **Multi-Orientation Support:** Seamlessly switches between White perspective, Black perspective, or Auto-detection.

---

## 🧱 Architecture

```text
       🖥️ Computer Screen (Desktop Chess App)
                       │
             [Fast Screen Capture] (mss)
                       │
            [Board Detector / ROI] (OpenCV)
                       │
       [64 Square Differential Analysis]
                       │
     [Game Tracker & Legal Move Engine] (python-chess)
                       │
           [Stockfish UCI Engine] (v19)
                   ┌───┴───┐
                   │       │
                   ▼       ▼
    [Transparent Overlay]  [Humanized Mouse Executor]
       (Best Move Arrow)        (Autonomous Play)
```

---

## 📦 Installation & Setup

### 1. Prerequisites
Ensure you have **Python 3.10+** installed on Windows.

### 2. Install Dependencies
```bash
pip install -r requirements.txt
```

### 3. Run ChessMonger Desktop
```bash
python main.py
```

Stockfish engine binary is automatically managed and placed in `engine/`.

---

## 🎮 How to Use

1. **Launch your Desktop Chess App** (e.g. Chess.com, Lichess, or any chess GUI) and start a match.
2. **Launch ChessMonger Desktop**:
   ```bash
   python main.py
   ```
3. **Calibrate the Board:**
   - Click **`🔍 Auto-Detect Board`** to automatically locate the chessboard on your screen, **OR**
   - Click **`🎯 Manual Select (Box)`** and drag a rectangle over the 8x8 chessboard.
4. **Choose your Mode & Side:**
   - **Mode:** Select `visual` for subtle on-screen arrows, or `auto` for autonomous mouse clicking.
   - **Side:** Choose `white`, `black`, or `auto`.
5. **Start Tracking:**
   - Click **`▶ START LIVE TRACKING`**.
   - Play your game! Moves made on the screen will be instantly recognized, evaluated, and suggested.

---

## ⚙️ Configuration & Customization

All settings can be customized in the GUI or saved in `config.json`:
- `engine_skill_level`: Stockfish skill level (0 to 20).
- `engine_depth`: Search depth (default: 15).
- `auto_move_delay_min` / `max`: Configurable thinking delay for human simulation.
- `humanize_mouse`: Enable/disable natural Bezier curve movements.

---

## ⚠️ Fair-Play Notice

This software is developed strictly for **educational and research purposes** in computer vision and artificial intelligence. Using chess engines or automated tools in rated online games violates platform terms of service.
