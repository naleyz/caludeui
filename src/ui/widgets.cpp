#include "ui/widgets.hpp"
#include "theme/theme.hpp"

#include <imgui_internal.h> // item flags (disabled state)

#include <algorithm>
#include <cfloat>

namespace ui::widgets
{
namespace
{
ImU32 Col(const ImVec4& c)
{
    return ImGui::ColorConvertFloat4ToU32(c);
}
} // namespace

bool Toggle(const char* id, bool* v)
{
    const float h = ImGui::GetFrameHeight() * 0.95f;
    const float w = ToggleWidth();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  y = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    const bool clicked = ImGui::InvisibleButton(id, {w, ImGui::GetFrameHeight()});
    if (clicked) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    ImU32 track = *v ? Col(theme::palette::Orange) : IM_COL32(0x3a, 0x39, 0x35, 255);
    if (hovered) track = *v ? IM_COL32(0xe3, 0x8c, 0x6e, 255) : IM_COL32(0x46, 0x45, 0x40, 255);
    if (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) track = IM_COL32(0x2b, 0x2a, 0x27, 255);
    dl->AddRectFilled({p.x, y}, {p.x + w, y + h}, track, h * 0.5f);
    const float r = h * 0.5f - 3.0f;
    const float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled({cx, y + h * 0.5f}, r, Col(theme::palette::Light));
    return clicked;
}

float SegmentedWidth(const char* const* labels, int count)
{
    const auto& st = ImGui::GetStyle();
    float w = 6.0f;
    for (int i = 0; i < count; ++i) w += ImGui::CalcTextSize(labels[i]).x + st.FramePadding.x * 2 + 2.0f;
    return w;
}

bool Segmented(const char* id, const char* const* labels, int count, int* value)
{
    const auto& st = ImGui::GetStyle();
    const float h  = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + SegmentedWidth(labels, count), p.y + h}, IM_COL32(0x14, 0x14, 0x13, 255), 8.0f);

    bool changed = false;
    float x = p.x + 3.0f;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i)
    {
        const float w = ImGui::CalcTextSize(labels[i]).x + st.FramePadding.x * 2;
        ImGui::SetCursorScreenPos({x, p.y + 3.0f});
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##seg", {w, h - 6.0f});
        ImGui::PopID();
        const bool on = *value == i;
        if (on) dl->AddRectFilled({x, p.y + 3.0f}, {x + w, p.y + h - 3.0f}, IM_COL32(0x2b, 0x2a, 0x27, 255), 6.0f);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled({x, p.y + 3.0f}, {x + w, p.y + h - 3.0f}, IM_COL32(0x20, 0x20, 0x1e, 255), 6.0f);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText({x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f}, on ? Col(theme::palette::Light) : Col(theme::palette::MidGray),
                    labels[i]);
        if (clicked && !on)
        {
            *value  = i;
            changed = true;
        }
        x += w + 2.0f;
    }
    ImGui::PopID();
    // Leave the cursor after the control, as a single item would.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy({SegmentedWidth(labels, count), h});
    return changed;
}

void Pill(const char* text, ImU32 bg, ImU32 fg)
{
    const float fs = ImGui::GetFontSize() * 0.82f;
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    const float h = fs + 6.0f, w = ts.x + 14.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
    ImGui::Dummy({w, ImGui::GetTextLineHeight()});
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({p.x, y}, {p.x + w, y + h}, bg, h * 0.5f);
    dl->AddText(ImGui::GetFont(), fs, {p.x + 7.0f, y + 3.0f}, fg, text);
}

void Banner(const char* text, ImU32 bg, ImU32 fg)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 12.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - pad * 2);
    const float h = ts.y + pad * 1.4f;
    ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + w, p.y + h}, bg, 10.0f);
    ImGui::SetCursorScreenPos({p.x + pad, p.y + pad * 0.7f});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fg));
    ImGui::PushTextWrapPos(p.x + w - pad - ImGui::GetWindowPos().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({w, 0});
}

void CardBegin(const char* id, const char* title, float width, const char* subtitle)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0f, 0.0f});
    ImGui::BeginChild(id, {width, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::Dummy({0, 10.0f});
    if (!title) return;
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.1f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle)
    {
        ImGui::SameLine();
        const float sw = ImGui::CalcTextSize(subtitle).x * 0.88f;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowContentRegionMax().x - sw));
        ImGui::AlignTextToFramePadding();
        SmallMuted(subtitle);
    }
    ImGui::Dummy({0, 4.0f});
    CardDivider();
}

void CardEnd()
{
    ImGui::Dummy({0, 6.0f});
    ImGui::EndChild();
}

void CardDivider()
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowSize().x;
    ImGui::GetWindowDrawList()->AddLine({x0 + 1.0f, p.y}, {x1 - 1.0f, p.y}, kLine);
    ImGui::Dummy({0, 1.0f});
}

void SmallMuted(const char* text)
{
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.88f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
} // namespace ui::widgets
