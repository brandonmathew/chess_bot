// DirectX 11 + Win32 host for ChessMonger.
//
// Floating-menu design (GUI subsystem - no console window): the app is
// a single borderless, always-on-top panel that floats on the desktop
// over other applications, plus the suggestion-arrow overlay. There is
// no framed "settings window" and no other window: the OS window is a
// WS_POPUP clipped to a rounded rectangle, and the ImGui menu fills it
// edge to edge. Drag the panel by its header strip; quit with the X in
// the header or File > Quit. Tracking auto-starts from the saved
// config on launch. The Insert key toggles the menu panel show/hide
// from anywhere (the arrow overlay is unaffected).
#include <windows.h>
#include <d3d11.h>

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

constexpr int kPanelWidth = 680;
constexpr int kPanelHeight = 880;

bool g_panelVisible = true;
constexpr int kInsertHotkeyId = 78;  // Insert toggles the menu panel

void togglePanel(HWND window) {
    g_panelVisible = !g_panelVisible;
    if (g_panelVisible) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        g_occluded = false;
    } else {
        ShowWindow(window, SW_HIDE);
    }
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
        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kInsertHotkeyId) togglePanel(window);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
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

    // Borderless topmost popup: the floating menu panel itself.
    HWND window = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "GambitChess Bot", WS_POPUP, 80, 80,
                                  kPanelWidth, kPanelHeight, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 1;

    // Clip the square window to a rounded rectangle: the desktop shows
    // through the corners, so what floats is a panel, not a window.
    if (HRGN region = CreateRoundRectRgn(0, 0, kPanelWidth + 1, kPanelHeight + 1, 30, 30)) {
        SetWindowRgn(window, region, TRUE);
    }

    if (!createDeviceD3D(window)) {
        cleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        cm::log::print("Failed to initialise Direct3D 11.");
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    cm::App app;
    app.setHostWindow(window);
    app.init();  // auto-starts tracking when a board region is configured

    if (!RegisterHotKey(window, kInsertHotkeyId, MOD_NOREPEAT, VK_INSERT)) {
        cm::log::print("[App] Could not register the Insert show/hide hotkey (%lu).", GetLastError());
    }

    bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Panel hidden via Insert: keep pumping messages (the hotkey and
        // the bot itself live there) but skip all rendering.
        if (!g_panelVisible) {
            Sleep(15);
            continue;
        }

        // Minimise CPU usage while the panel is fully occluded.
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

        if (app.quitRequested()) DestroyWindow(window);
    }

    UnregisterHotKey(window, kInsertHotkeyId);

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
