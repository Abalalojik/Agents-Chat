// Agents Chat — SDL2 + OpenGL 3 host for the Dear ImGui interface (Linux).
// Platform setup follows Dear ImGui's example_sdl2_opengl3; behaviour mirrors main.cpp.

#include "App.h"
#include "CloudSync.h"
#include "Platform.h"
#include "Settings.h"
#include "Store.h"
#include "Theme.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <nlohmann/json.hpp>

#include <SDL.h>
#include <SDL_opengl.h>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "--capture-antigravity-status")
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
    if (command == "--sync-cloud")
    {
        CloudSync cloud(Platform::DataRoot());
        return cloud.SyncBlocking() ? 0 : 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        std::cerr << "SDL : " << SDL_GetError() << std::endl;
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

    float dpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0 || dpi <= 0.0f)
        dpi = 96.0f;
    const float scale = dpi / 96.0f;
    SDL_Window* window = SDL_CreateWindow("Agents Chat", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          static_cast<int>(1400 * scale), static_cast<int>(860 * scale),
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl)
    {
        std::cerr << "OpenGL 3 : " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const std::filesystem::path dataRoot = Platform::DataRoot();
    static const std::string iniPath = (dataRoot / "imgui.ini").string();
    io.IniFilename = iniPath.c_str();

    ApplyAgentsChatTheme(scale);

    // A font with French accents and typographic quotes; fall back to the built-in font.
    for (const char* font : {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        if (std::filesystem::exists(font))
        {
            io.Fonts->AddFontFromFileTTF(font, 18.0f);
            break;
        }

    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    Store store(dataRoot);
    store.Load();
    Settings settings(dataRoot);
    settings.Load();
    App app(store, settings, window);

    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                 event.window.windowID == SDL_GetWindowID(window)))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        app.Frame();

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
                   static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
        glClearColor(0.19f, 0.20f, 0.22f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
