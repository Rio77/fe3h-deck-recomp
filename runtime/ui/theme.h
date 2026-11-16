// The menus' colours (in-game overlay and start screen): slate blue panels with gold accents and a thin gold trim,
// after Fire Emblem: Three Houses' framed menus. One place, so every page matches.
#pragma once
#include "imgui.h"

// gold: headings, selection borders, the focus bar; bright: a changed value; dim: slider grabs
#define THEME_GOLD ImVec4(0.89f, 0.76f, 0.47f, 1)
#define THEME_GOLD_U32 IM_COL32(227, 194, 120, 255)
#define THEME_GOLD_SOFT_U32 IM_COL32(227, 194, 120, 110)
#define THEME_TRIM_U32 IM_COL32(227, 194, 120, 90)  // the windows' inner border
#define THEME_GOLD_BRIGHT ImVec4(0.98f, 0.85f, 0.56f, 1)
#define THEME_GOLD_DIM ImVec4(0.60f, 0.51f, 0.32f, 1)
// text: warm off-white; quiet: section titles and hints
#define THEME_TEXT ImVec4(0.96f, 0.94f, 0.88f, 1)
#define THEME_TEXT_U32 IM_COL32(245, 240, 224, 255)
#define THEME_TEXT_DIM_U32 IM_COL32(220, 216, 205, 255)
#define THEME_QUIET ImVec4(0.62f, 0.64f, 0.72f, 1)
// slate panels, from the window back to the controls on it
#define THEME_WINDOW ImVec4(0.10f, 0.12f, 0.18f, 0.94f)
#define THEME_CHILD ImVec4(0.13f, 0.16f, 0.23f, 0.85f)
#define THEME_POPUP ImVec4(0.10f, 0.12f, 0.18f, 0.98f)
#define THEME_FRAME ImVec4(0.17f, 0.20f, 0.29f, 1)
#define THEME_HOVER ImVec4(0.24f, 0.29f, 0.42f, 1)
#define THEME_ACTIVE ImVec4(0.30f, 0.36f, 0.52f, 1)
#define THEME_TAB ImVec4(0.15f, 0.18f, 0.26f, 1)
#define THEME_TAB_SELECTED ImVec4(0.28f, 0.34f, 0.50f, 1)
#define THEME_SEPARATOR ImVec4(0.42f, 0.37f, 0.26f, 1)

// The thin gold trim just inside a window's edge (call while drawing into that window)
static inline void theme_trim(float scale) {
    ImVec2 a = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float in = 6 * scale;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(a.x + s.x - in, a.y + s.y - in), THEME_TRIM_U32,
                                        ImGui::GetStyle().WindowRounding > 4 * scale ? ImGui::GetStyle().WindowRounding - in : 0,
                                        0, 1.5f * scale);
}
