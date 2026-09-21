#pragma once
#include <imgui.h>

// The look shared by the Windows and Linux hosts.
inline void ApplyAgentsChatTheme(float scale)
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
