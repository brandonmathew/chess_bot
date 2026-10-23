cmake_minimum_required(VERSION 3.20)
project(ChessMonger LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CM_WARNINGS -Wall -Wextra -Wno-unused-parameter)

add_compile_definitions(
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  IMGUI_DEFINE_MATH_OPERATORS
)

# ---------------------------------------------------------------------------
# Dear ImGui (Win32 + DirectX 11)
# ---------------------------------------------------------------------------
add_library(imgui STATIC
  vendor/imgui/imgui.cpp
  vendor/imgui/imgui_draw.cpp
  vendor/imgui/imgui_tables.cpp
  vendor/imgui/imgui_widgets.cpp
  vendor/imgui/backends/imgui_impl_win32.cpp
  vendor/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi dwmapi imm32 gdi32 user32)

# ---------------------------------------------------------------------------
# miniz (zip extraction for the Stockfish downloader)
# ---------------------------------------------------------------------------
add_library(miniz STATIC
  vendor/miniz.c
  vendor/miniz_tdef.c
  vendor/miniz_tinfl.c
  vendor/miniz_zip.c
)
target_include_directories(miniz PUBLIC vendor)

# ---------------------------------------------------------------------------
# Core library: CV + chess + capture + engine + input (no UI toolkit)
# ---------------------------------------------------------------------------
add_library(cm_core STATIC
  src/core/Image.cpp
  src/core/CvOps.cpp
  src/core/Config.cpp
  src/core/Log.cpp
  src/vision/EdgeContour.cpp
  src/vision/BoardDetector.cpp
  src/vision/GameTracker.cpp
  src/capture/ScreenCapture.cpp
  src/capture/RegionSniper.cpp
  src/overlay/OverlayWindow.cpp
  src/exec/MoveExecutor.cpp
  src/engine/EngineUci.cpp
  src/engine/EngineDownload.cpp
)
target_include_directories(cm_core PUBLIC src vendor)
target_link_libraries(cm_core PUBLIC miniz winhttp gdi32 user32 shell32 ole32)
target_compile_options(cm_core PRIVATE ${CM_WARNINGS})

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(chessmonger
  src/main.cpp
  src/app/App.cpp
  src/app/Worker.cpp
)
target_link_libraries(chessmonger PRIVATE cm_core imgui)
target_compile_options(chessmonger PRIVATE ${CM_WARNINGS})

# ---------------------------------------------------------------------------
# Tests (ported from the original Python unittest suite)
# ---------------------------------------------------------------------------
enable_testing()
add_executable(chessmonger_tests
  tests/test_main.cpp
  tests/vision_tests.cpp
  tests/test_tracking.cpp
  tests/test_engine.cpp
)
target_link_libraries(chessmonger_tests PRIVATE cm_core)
target_compile_options(chessmonger_tests PRIVATE ${CM_WARNINGS})
add_test(NAME unit COMMAND chessmonger_tests)
