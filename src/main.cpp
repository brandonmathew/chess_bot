// DirectX 11 + Win32 host for ChessMonger.
//
// Overlay-first design: this is a GUI-subsystem program (no console
// window). It launches invisibly - no main window at all - starts
// tracking from the saved config, and lives in the system tray. The
// only thing on the desktop is the suggestion overlay itself. The
// settings window appears only when the board was never calibrated,
// or on demand: tray icon (double-click / right-click menu) or the
// global Ctrl+Alt+G hotkey. Closing the settings window hides it
// again; quitting happens from the tray menu or File > Quit.
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <cstdio>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"

#include "app/App.h"
#include "core/Log.h"

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
bool g_occluded = false;

// Settings-window / tray state.
bool g_settingsVisible = false;
bool g_trayAdded = false;

constexpr UINT kTrayCallback = WM_APP + 7;
constexpr UINT_PTR kTrayUid = 1;
constexpr int kTrayOpenId = 1001;
constexpr int kTrayQuitId = 1002;
constexpr int kHotkeyId = 77;  // Ctrl+Alt+G toggles the settings window

void showSettings(HWND window) {
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    g_settingsVisible = true;
    g_occluded = false;
}

void hideSettings(HWND window) {
    ShowWindow(window, SW_HIDE);
    g_settingsVisible = false;
}

void toggleSettings(HWND window) {
    if (g_settingsVisible) {
        hideSettings(window);
    } else {
        showSettings(window);
    }
}

void addTrayIcon(HWND window) {
    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window;
    nid.uID = kTrayUid;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    std::snprintf(nid.szTip, sizeof(nid.szTip), "GambitChess Bot - overlay running");
    g_trayAdded = Shell_NotifyIconA(NIM_ADD, &nid) != FALSE;
}

void removeTrayIcon(HWND window) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window;
    nid.uID = kTrayUid;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void showTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, kTrayOpenId, "Open Settings");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, kTrayQuitId, "Quit GambitChess Bot");
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(window);  // required for the menu to dismiss correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
    PostMessageA(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool createDeviceD3D(HWND window) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result =
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                      D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &featureLevel,
                                      &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
                                               &featureLevel, &g_context);
    }
    if (result != S_OK) return false;
    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

void enableDpiAwareness() {
    // Per-monitor v2 when available (crisp UI + correct sniper/overlay
    // coordinates on mixed-DPI multi-monitor setups).
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        if (auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4 /* PER_MONITOR_AWARE_V2 */)))) return;
        }
    }
    SetProcessDPIAware();
}

}  // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam,
                                                            LPARAM lParam);

LRESULT WINAPI wndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return true;

    switch (message) {
        case WM_SIZE:
            if (g_device != nullptr && wParam != SIZE_MINIMIZED && LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)),
                                           static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_MINIMIZE) return 0;  // avoid ImGui minimize jank
            break;
        case WM_CLOSE:
            // The X button returns to overlay-only mode; it does not quit.
            hideSettings(window);
            return 0;
        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kHotkeyId) toggleSettings(window);
            return 0;
        case kTrayCallback:
            if (lParam == WM_LBUTTONDBLCLK) {
                toggleSettings(window);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayMenu(window);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kTrayOpenId) {
                showSettings(window);
                return 0;
            }
            if (LOWORD(wParam) == kTrayQuitId) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DESTROY:
            removeTrayIcon(window);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

int runApp(HINSTANCE instance) {
    enableDpiAwareness();

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "ChessMongerMainWindow";
    RegisterClassExA(&wc);

    // Created hidden on purpose: see the file header.
    HWND window = CreateWindowExA(0, wc.lpszClassName, "GambitChess Bot", WS_OVERLAPPEDWINDOW, 100, 100,
                                  700, 920, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 1;
    if (!createDeviceD3D(window)) {
        cleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        cm::log::print("Failed to initialise Direct3D 11.");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    cm::App app;
    app.init();  // auto-starts tracking when a board region is configured

    addTrayIcon(window);
    RegisterHotKey(window, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 0x47 /* G */);

    // First run (no calibrated board yet) is the one case where the user
    // must see the settings window immediately.
    if (app.needsSettings()) showSettings(window);

    bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (app.quitRequested()) {
            DestroyWindow(window);
            continue;
        }

        // Overlay-only mode: nothing to render, just keep pumping messages
        // (tray, hotkey, overlay and worker callbacks all live there).
        if (!g_settingsVisible) {
            Sleep(15);
            continue;
        }

        // Minimise CPU usage while the window is fully occluded.
        if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.frame();

        ImGui::Render();
        const float clearColor[4] = {0.02f, 0.02f, 0.03f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present = g_swapChain->Present(1, 0);  // vsync
        g_occluded = (present == DXGI_STATUS_OCCLUDED);
    }

    UnregisterHotKey(window, kHotkeyId);
    removeTrayIcon(window);

    app.shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    if (IsWindow(window)) DestroyWindow(window);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}

// GUI-subsystem entry point (CMake WIN32 / --subsystem,windows builds).
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) { return runApp(instance); }

// Console-subsystem entry point (kept so the same file also links and
// runs in a plain console build).
int main() { return runApp(GetModuleHandleA(nullptr)); }
