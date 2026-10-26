#include "ui/rail.hpp"
#include "theme/theme.hpp"
#include "ui/usage_panel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace ui
{
namespace
{
ImU32 Col(const ImVec4& c, float a = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a});
}

constexpr ImU32 kActiveBg = IM_COL32(0x2b, 0x2a, 0x27, 255);
constexpr ImU32 kHoverBg  = IM_COL32(0x20, 0x20, 0x1e, 255);
constexpr ImU32 kIdleIcon = IM_COL32(0x8f, 0x8d, 0x85, 255);
constexpr ImU32 kTrack    = IM_COL32(0x2b, 0x2a, 0x27, 255);
constexpr ImU32 kDivider  = IM_COL32(0x2b, 0x2a, 0x27, 255);


// Icons are authored on a 24-unit grid (like the SVG mockups) and drawn into a
// square of `size` px whose top-left is `o`.
struct Pen
{
    ImDrawList* dl;
    ImVec2      o;
    float       k; // px per grid unit
    ImU32       col;
    float       w;

    ImVec2 P(float x, float y) const { return {o.x + x * k, o.y + y * k}; }
    void Line(std::initializer_list<ImVec2> pts) const
    {
        ImVec2 buf[8];
        int    n = 0;
        for (auto p : pts) buf[n++] = P(p.x, p.y);
        dl->AddPolyline(buf, n, col, ImDrawFlags_None, w);
    }
    void Rect(float x0, float y0, float x1, float y1, float r) const
    {
        dl->AddRect(P(x0, y0), P(x1, y1), col, r * k, ImDrawFlags_None, w);
    }
};

void IconSessions(const Pen& p)
{
    p.Rect(3, 4, 21, 20, 2);
    p.Line({{7, 9}, {10, 12}, {7, 15}});
    p.Line({{12, 15}, {17, 15}});
}

void IconStats(const Pen& p)
{
    p.Line({{3, 3}, {3, 21}, {21, 21}});
    p.Line({{7, 15}, {11, 11}, {14, 14}, {20, 8}});
    p.Line({{16, 8}, {20, 8}, {20, 12}});
}

void IconPanels(const Pen& p)
{
    p.Rect(3, 3, 10, 21, 1.5f);
    p.Rect(14, 3, 21, 11, 1.5f);
    p.Rect(14, 15, 21, 21, 1.5f);
}

void IconGear(const Pen& p)
{
    const ImVec2 c = p.P(12, 12);
    p.dl->AddCircle(c, 3.0f * p.k, p.col, 0, p.w);
    p.dl->AddCircle(c, 6.6f * p.k, p.col, 0, p.w);
    for (int i = 0; i < 8; ++i)
    {
        const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 4.0f;
        const ImVec2 d{std::cos(a), std::sin(a)};
        p.dl->AddLine({c.x + d.x * 6.6f * p.k, c.y + d.y * 6.6f * p.k}, {c.x + d.x * 9.4f * p.k, c.y + d.y * 9.4f * p.k},
                      p.col, p.w * 1.7f);
    }
}

// Tooltip to the right of an item instead of under the mouse.
void SideTooltip(const char* text, const char* hint = nullptr)
{
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({max.x + 10.0f, (min.y + max.y) * 0.5f}, ImGuiCond_Always, {0.0f, 0.5f});
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    if (hint)
    {
        ImGui::SameLine();
        ImGui::TextColored(theme::palette::MidGray, "%s", hint);
    }
    ImGui::EndTooltip();
}

// One square rail button. Returns true when clicked.
bool RailButton(const char* id, bool active, float size, float scale, void (*icon)(const Pen&))
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, {size, size});
    const bool hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    if (active || hovered)
        dl->AddRectFilled(pos, {pos.x + size, pos.y + size}, active ? kActiveBg : kHoverBg, 10.0f * scale);
    const float iconPx = 20.0f * scale;
    const ImU32 col = active ? Col(theme::palette::Light) : hovered ? Col(theme::palette::MidGray) : kIdleIcon;
    icon(Pen{dl, {pos.x + (size - iconPx) * 0.5f, pos.y + (size - iconPx) * 0.5f}, iconPx / 24.0f, col, 1.6f * scale});
    return clicked;
}
} // namespace

void Rail::Draw(Section& current, const Info& info, float scale)
{
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos  = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const float  btn     = 44.0f * scale;
    const float  x       = winPos.x + (winSize.x - btn) * 0.5f;

    // ── Logo ───────────────────────────────────────────────────────────
    {
        const float s = 32.0f * scale;
        const ImVec2 o{winPos.x + (winSize.x - s) * 0.5f, winPos.y + 14.0f * scale};
        dl->AddRectFilled(o, {o.x + s, o.y + s}, Col(theme::palette::Orange), 9.0f * scale);
        const Pen p{dl, {o.x + 8.0f * scale, o.y + 8.0f * scale}, 16.0f * scale / 24.0f, Col(theme::palette::Dark), 2.4f * scale};
        p.Line({{4, 17}, {10, 12}, {4, 7}});
        p.Line({{12, 19}, {20, 19}});
    }

    // ── Sections ───────────────────────────────────────────────────────
    float y = winPos.y + 60.0f * scale;
    auto place = [&] {
        ImGui::SetCursorScreenPos({x, y});
        y += btn + 6.0f * scale;
    };

    place();
    if (RailButton("##rail_sessions", current == Section::Sessions, btn, scale, IconSessions)) current = Section::Sessions;
    const ImVec2 sessMin = ImGui::GetItemRectMin(), sessMax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) SideTooltip("Сессии");
    if (info.runningTabs > 0 && current != Section::Sessions)
    {
        const std::string n = std::to_string(info.runningTabs);
        const float r = 8.0f * scale;
        const ImVec2 c{sessMax.x - r + 2.0f * scale, sessMin.y + r - 2.0f * scale};
        dl->AddCircleFilled(c, r, Col(theme::palette::Blue));
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        const float  fs = ImGui::GetFontSize() * 0.75f;
        const float  k  = fs / ImGui::GetFontSize();
        dl->AddText(ImGui::GetFont(), fs, {c.x - ts.x * k * 0.5f, c.y - ts.y * k * 0.5f}, Col(theme::palette::Dark), n.c_str());
    }

    place();
    if (RailButton("##rail_stats", current == Section::Stats, btn, scale, IconStats)) current = Section::Stats;
    if (ImGui::IsItemHovered()) SideTooltip("Статистика");

    place();
    if (RailButton("##rail_panels", current == Section::Panels, btn, scale, IconPanels)) current = Section::Panels;
    if (ImGui::IsItemHovered()) SideTooltip("Панели");

    // ── Bottom: limits, divider, settings ──────────────────────────────
    float bottom = winPos.y + winSize.y - 14.0f * scale;

    const float gearY = bottom - btn;
    ImGui::SetCursorScreenPos({x, gearY});
    if (RailButton("##rail_settings", current == Section::Settings, btn, scale, IconGear)) current = Section::Settings;
    if (ImGui::IsItemHovered()) SideTooltip("Настройки", "Ctrl+,");

    const float divY = gearY - 8.0f * scale;
    dl->AddLine({winPos.x + (winSize.x - 32.0f * scale) * 0.5f, divY}, {winPos.x + (winSize.x + 32.0f * scale) * 0.5f, divY},
                kDivider, 1.0f);

    if (info.fiveHour || info.week)
    {
        const float barW = 6.0f * scale, barH = 44.0f * scale, gap = 5.0f * scale;
        const float barsTop = divY - 8.0f * scale - barH;
        const float left    = winPos.x + (winSize.x - (barW * 2 + gap)) * 0.5f;
        ImGui::SetCursorScreenPos({left - 6.0f * scale, barsTop - 4.0f * scale});
        if (ImGui::InvisibleButton("##rail_limits", {barW * 2 + gap + 12.0f * scale, barH + 8.0f * scale}))
            current = Section::Stats;
        const bool hovered = ImGui::IsItemHovered();
        auto bar = [&](float bx, const std::optional<Limit>& l) {
            dl->AddRectFilled({bx, barsTop}, {bx + barW, barsTop + barH}, kTrack, barW * 0.5f);
            if (!l) return;
            const float h = barH * static_cast<float>(std::clamp(l->usedPct, 0.0, 100.0) / 100.0);
            if (h > 0.5f) dl->AddRectFilled({bx, barsTop + barH - h}, {bx + barW, barsTop + barH}, LimitTierColor(l->tier), barW * 0.5f);
        };
        bar(left, info.fiveHour);
        bar(left + barW + gap, info.week);
        if (hovered)
        {
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImGui::SetNextWindowPos({max.x + 10.0f, (min.y + max.y) * 0.5f}, ImGuiCond_Always, {0.0f, 0.5f});
            ImGui::BeginTooltip();
            if (info.fiveHour) ImGui::TextUnformatted(info.fiveHour->tooltip.c_str());
            if (info.week) ImGui::TextUnformatted(info.week->tooltip.c_str());
            ImGui::TextColored(theme::palette::MidGray, "Клик — подробная статистика");
            ImGui::EndTooltip();
        }
    }
}
} // namespace ui
