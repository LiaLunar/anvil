// The colours of the overlay and the Manager: ImGui's dark style with the
// darker window background it had until 1.92, which is what the two were
// designed and photographed on. Frames and tables go with it, or they would
// fade into the window. TextDisabled stays ImGui's, brighter than it was.
#pragma once

#include <imgui.h>

namespace anvil::ui {

inline void use_dark_colors() {
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.29f, 0.48f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(0.225f, 0.485f, 0.805f, 0.449f);  // ImGui's mix of the two above
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
}

}  // namespace anvil::ui
