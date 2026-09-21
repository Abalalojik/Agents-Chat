// Agents Chat — Win32 + DirectX 11 host for the Dear ImGui interface.
// Platform setup follows Dear ImGui's example_win32_directx11.

#include "App.h"
#include "CloudSync.h"
#include "Platform.h"
#include "Settings.h"
#include "Store.h"
#include "Theme.h"
#include "Updater.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <nlohmann/json.hpp>

#include <d3d11.h>
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "../resources/resource.h"

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

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool scheduledSync = argc > 1 && std::wstring(argv[1]) == L"--sync-cloud";
    const bool captureAntigravity = argc > 1 && std::wstring(argv[1]) == L"--capture-antigravity-status";
    const bool applyUpdate = argc > 3 && std::wstring(argv[1]) == L"--apply-update";
    std::filesystem::path updateDestination;
    unsigned long updateParentPid = 0;
    if (applyUpdate)
    {
        updateDestination = argv[2];
        try { updateParentPid = std::stoul(argv[3]); } catch (...) { updateParentPid = 0; }
    }
    if (argv) LocalFree(argv);
    if (applyUpdate)
        return updateParentPid && Updater::ApplyPendingUpdate(updateDestination, updateParentPid) ? 0 : 3;
    if (captureAntigravity)
    {
        std::string payload((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        if (!payload.empty())
        {
            try
            {
                nlohmann::json state = nlohmann::json::parse(payload);
                state["agents_chat_captured_at"] = Platform::NowIsoUtc();
                std::string error;
                Platform::WriteFileAtomic(Platform::DataRoot() / "antigravity-status.json", state.dump(2), error);
            }
            catch (...) {}
        }
        std::cout << "Agents Chat" << std::endl;
        return 0;
    }
    if (scheduledSync)
    {
        CloudSync cloud(Platform::DataRoot());
        return cloud.SyncBlocking() ? 0 : 2;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // for the folder picker

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, instance,
                      LoadIconW(instance, MAKEINTRESOURCEW(IDI_AGENTCHATS)), nullptr, nullptr, nullptr,
                      L"AgentChats", LoadIconW(instance, MAKEINTRESOURCEW(IDI_AGENTCHATS))};
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

    ApplyAgentsChatTheme(scale);

    // Segoe UI covers French accents and typographic quotes; fall back to the built-in font.
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(segoe))
        io.Fonts->AddFontFromFileTTF(segoe, 18.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Store store(dataRoot);
    store.Load();
    // The self-improvement sous-serveur is attached to the app's own clone by App, once it exists:
    // never to the checkout this executable may have been built from.
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
