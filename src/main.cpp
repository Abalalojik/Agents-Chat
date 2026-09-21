// Agents Chat — Win32 + DirectX 11 host for the Dear ImGui interface.
// Platform setup follows Dear ImGui's example_win32_directx11.

#include "App.h"
#include "CloudSync.h"
#include "Platform.h"
#include "Settings.h"
#include "Store.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTarget = nullptr;
    bool g_swapChainOccluded = false;
    UINT g_resizeWidth = 0, g_resizeHeight = 0;

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }

    void CleanupRenderTarget()
    {
        if (g_renderTarget)
        {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
        if (hr == DXGI_ERROR_UNSUPPORTED) // no hardware device: fall back to the software rasterizer
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
        if (FAILED(hr))
            return false;
        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();
        if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
    }

    LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;

        switch (msg)
        {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                return 0;
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) // no ALT application menu
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void ApplyTheme(float scale)
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.FrameRounding = 4.0f;
        style.PopupRounding = 6.0f;
        style.WindowRounding = 6.0f;
        style.ScrollbarRounding = 4.0f;
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.19f, 0.20f, 0.22f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.17f, 0.18f, 0.19f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.23f, 0.25f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.25f, 0.26f, 0.29f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.40f, 0.95f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.35f, 0.85f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.25f, 0.26f, 0.29f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.29f, 0.32f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.31f, 0.35f, 1.0f);
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool scheduledSync = argc > 1 && std::wstring(argv[1]) == L"--sync-cloud";
    if (argv) LocalFree(argv);
    if (scheduledSync)
    {
        CloudSync cloud(Platform::DataRoot());
        return cloud.SyncBlocking() ? 0 : 2;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // for the folder picker

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, instance, nullptr, nullptr, nullptr, nullptr, L"AgentChats", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Agents Chat", WS_OVERLAPPEDWINDOW, 100, 100,
                              static_cast<int>(1400 * scale), static_cast<int>(860 * scale),
                              nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Impossible d'initialiser DirectX 11.", L"Agents Chat", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const std::filesystem::path dataRoot = Platform::DataRoot();
    static const std::string iniPath = Platform::Narrow((dataRoot / L"imgui.ini").wstring());
    io.IniFilename = iniPath.c_str();

    ApplyTheme(scale);

    // Segoe UI covers French accents and typographic quotes; fall back to the built-in font.
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(segoe))
        io.Fonts->AddFontFromFileTTF(segoe, 18.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Store store(dataRoot);
    store.Load();
    const std::filesystem::path projectRoot = Platform::ProjectRoot();
    store.EnsureSelfImprovementSubserver(Platform::Narrow(projectRoot.wstring()));
    Settings settings(dataRoot);
    settings.Load();
    App app(store, settings, hwnd);

    const float clearColor[4] = {0.19f, 0.20f, 0.22f, 1.0f};
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Sleep while the window is hidden or minimized.
        if (g_swapChainOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            Sleep(10);
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeWidth != 0 && g_resizeHeight != 0)
        {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Frame();

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_swapChain->Present(1, 0); // vsync
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
