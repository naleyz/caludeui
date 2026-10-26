#include "theme/theme.hpp"

namespace theme
{
namespace
{
ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

ImVec4 WithAlpha(ImVec4 c, float a) { c.w = a; return c; }
} // namespace

void Apply(Mode mode)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors    = style.Colors;

    const bool dark  = mode == Mode::Dark;
    const ImVec4 bg     = dark ? palette::Dark  : palette::Light;
    const ImVec4 text   = dark ? palette::Light : palette::Dark;
    const ImVec4 border = dark ? Mix(palette::Dark, palette::MidGray, 0.35f) : palette::LightGray;
    const ImVec4 panel  = dark ? Mix(palette::Dark, palette::Light, 0.06f)   : palette::LightGray;
    const ImVec4 panelHi= dark ? Mix(palette::Dark, palette::Light, 0.10f)   : Mix(palette::LightGray, palette::Light, 0.5f);

    colors[ImGuiCol_Text]                  = text;
    colors[ImGuiCol_TextDisabled]           = palette::MidGray;
    colors[ImGuiCol_WindowBg]               = bg;
    colors[ImGuiCol_ChildBg]                = bg;
    // Popups sit a step below frames so checkboxes/inputs inside them stay visible.
    colors[ImGuiCol_PopupBg]                = dark ? Mix(palette::Dark, palette::Light, 0.03f) : palette::Light;
    colors[ImGuiCol_Border]                 = border;
    colors[ImGuiCol_BorderShadow]           = WithAlpha(border, 0.0f);

    colors[ImGuiCol_FrameBg]                = panel;
    colors[ImGuiCol_FrameBgHovered]         = panelHi;
    colors[ImGuiCol_FrameBgActive]          = panelHi;

    colors[ImGuiCol_TitleBg]                = panel;
    colors[ImGuiCol_TitleBgActive]          = panel;
    colors[ImGuiCol_TitleBgCollapsed]       = panel;
    colors[ImGuiCol_MenuBarBg]              = panel;

    colors[ImGuiCol_ScrollbarBg]            = bg;
    colors[ImGuiCol_ScrollbarGrab]          = border;
    colors[ImGuiCol_ScrollbarGrabHovered]   = palette::MidGray;
    colors[ImGuiCol_ScrollbarGrabActive]    = palette::Orange;

    colors[ImGuiCol_CheckMark]              = palette::Orange;
    colors[ImGuiCol_SliderGrab]             = palette::Orange;
    colors[ImGuiCol_SliderGrabActive]       = Mix(palette::Orange, text, 0.2f);

    colors[ImGuiCol_Button]                 = panelHi;
    colors[ImGuiCol_ButtonHovered]          = Mix(palette::Orange, panelHi, 0.35f);
    colors[ImGuiCol_ButtonActive]           = palette::Orange;

    colors[ImGuiCol_Header]                 = Mix(palette::Orange, panel, 0.75f);
    colors[ImGuiCol_HeaderHovered]          = Mix(palette::Orange, panel, 0.45f);
    colors[ImGuiCol_HeaderActive]           = palette::Orange;

    colors[ImGuiCol_Separator]              = border;
    colors[ImGuiCol_SeparatorHovered]       = palette::Blue;
    colors[ImGuiCol_SeparatorActive]        = palette::Blue;

    colors[ImGuiCol_ResizeGrip]             = WithAlpha(palette::Orange, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]      = WithAlpha(palette::Orange, 0.6f);
    colors[ImGuiCol_ResizeGripActive]       = palette::Orange;

    colors[ImGuiCol_Tab]                    = panel;
    colors[ImGuiCol_TabHovered]             = Mix(palette::Orange, panel, 0.45f);
    colors[ImGuiCol_TabSelected]            = Mix(palette::Orange, panel, 0.7f);
    colors[ImGuiCol_TabSelectedOverline]    = palette::Orange;
    colors[ImGuiCol_TabDimmed]              = panel;
    colors[ImGuiCol_TabDimmedSelected]      = panelHi;

    colors[ImGuiCol_DockingPreview]         = WithAlpha(palette::Orange, 0.5f);
    colors[ImGuiCol_DockingEmptyBg]         = bg;

    colors[ImGuiCol_PlotLines]              = palette::Blue;
    colors[ImGuiCol_PlotHistogram]          = palette::Green;

    colors[ImGuiCol_TextSelectedBg]         = WithAlpha(palette::Orange, 0.4f);
    colors[ImGuiCol_NavCursor]              = palette::Orange;
    colors[ImGuiCol_ModalWindowDimBg]       = WithAlpha(palette::Dark, 0.65f);

    colors[ImGuiCol_TableHeaderBg]          = panel;
    colors[ImGuiCol_TableRowBgAlt]          = WithAlpha(palette::Light, dark ? 0.025f : 0.3f);

    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
}
} // namespace theme
