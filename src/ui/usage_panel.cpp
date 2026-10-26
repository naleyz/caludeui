#include "ui/usage_panel.hpp"
#include "theme/theme.hpp"
#include "ui/icons.hpp"
#include "ui/util.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>

// Laid out like claude.ai's Settings → Usage page: a headline sentence, then one
// row per limit — name and reset time on the left, a thin bar, the figure on
// the right. Bars are blue while there is room, yellow when a limit is getting
// close, red when it is nearly gone.
namespace ui
{
namespace
{
// claude.ai's dark-theme tokens for the three states of a meter.
struct Tier
{
    ImU32 fill;
    ImU32 track;
    ImU32 text; // the figure on the right; 0 = the plain secondary colour
};
constexpr ImU32 kTrackEmpty = IM_COL32(255, 255, 255, 13); // white at 5%: nothing on this bar yet
constexpr Tier kTiers[] = {
    {IM_COL32(0x2a, 0x78, 0xd6, 255), IM_COL32(0x03, 0x20, 0x42, 255), 0},                              // room left
    {IM_COL32(0xfa, 0xb2, 0x19, 255), IM_COL32(0x31, 0x1a, 0x00, 255), IM_COL32(0xdb, 0x93, 0x00, 255)}, // getting close
    {IM_COL32(0xd0, 0x3b, 0x3b, 255), IM_COL32(0x3c, 0x0e, 0x0e, 255), IM_COL32(0xec, 0x7e, 0x7e, 255)}, // nearly gone
};

ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

struct Sizes
{
    float body, small, heading, headline;
};
// The page's 14 / 13 / 15 / 22 px, scaled to the interface font.
Sizes TextSizes()
{
    const float b = ImGui::GetFontSize();
    return {b, b * 0.93f, b * 1.07f, b * 1.55f};
}

ImVec2 TextSize(float size, const char* text)
{
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text)
{
    dl->AddText(ImGui::GetFont(), size, pos, color, text);
}

// "1 ч 35 мин", "2 д 5 ч", "12 мин"
std::string Duration(std::int64_t ms)
{
    const std::int64_t mins = std::max<std::int64_t>(0, ms) / 60000;
    char buf[48];
    if (mins < 1) return "меньше минуты";
    if (mins < 60) std::snprintf(buf, sizeof(buf), "%lld мин", static_cast<long long>(mins));
    else if (mins < 24 * 60)
        std::snprintf(buf, sizeof(buf), "%lld ч %02lld мин", static_cast<long long>(mins / 60), static_cast<long long>(mins % 60));
    else
        std::snprintf(buf, sizeof(buf), "%lld д %lld ч", static_cast<long long>(mins / (24 * 60)),
                      static_cast<long long>((mins / 60) % 24));
    return buf;
}

// Local wall-clock time, phrased to follow "Сброс": "в 18:20" today, "пт, 07:00" within the week, "2 окт, 07:00" further out.
std::string ClockTime(std::int64_t ms, std::int64_t nowMs)
{
    static constexpr const char* kMonths[]   = {"янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек"};
    static constexpr const char* kWeekdays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    auto local = [](std::int64_t t) {
        const std::time_t secs = static_cast<std::time_t>(t / 1000);
        std::tm tm{};
        ::localtime_r(&secs, &tm);
        return tm;
    };
    const std::tm at = local(ms), now = local(nowMs);
    char buf[48];
    if (at.tm_year == now.tm_year && at.tm_yday == now.tm_yday) std::snprintf(buf, sizeof(buf), "в %02d:%02d", at.tm_hour, at.tm_min);
    else if (ms - nowMs < 6LL * 24 * 3600 * 1000) std::snprintf(buf, sizeof(buf), "%s, %02d:%02d", kWeekdays[at.tm_wday], at.tm_hour, at.tm_min);
    else std::snprintf(buf, sizeof(buf), "%d %s, %02d:%02d", at.tm_mday, kMonths[at.tm_mon], at.tm_hour, at.tm_min);
    return buf;
}

std::string Ago(std::int64_t ms, std::int64_t nowMs)
{
    const std::int64_t secs = std::max<std::int64_t>(0, nowMs - ms) / 1000;
    if (secs < 20) return "только что";
    return Duration(secs * 1000) + " назад";
}

std::string Percent(double v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", v);
    return buf;
}



void Footnote(const std::string& text, ImU32 color)
{
    const Sizes sz = TextSizes();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Text(ImGui::GetWindowDrawList(), sz.small, p, color, text.c_str());
    ImGui::Dummy({0.0f, sz.small + 2.0f});
}



const char* ProductName(const usage::ProductShare& p)
{
    if (p.key == "chat") return "Чаты";
    if (p.key == "other") return "Прочее";
    return p.name.c_str();
}
} // namespace

ImU32 LimitTierTrack(int tier)
{
    return kTiers[std::clamp(tier, 0, 2)].track;
}

ImU32 LimitTierText(int tier)
{
    return kTiers[std::clamp(tier, 0, 2)].text;
}

std::string LimitClock(std::int64_t ms)
{
    return ClockTime(ms, usage::NowMs());
}

std::string LimitDuration(std::int64_t ms)
{
    return Duration(ms);
}

std::string LimitAgo(std::int64_t ms)
{
    return Ago(ms, usage::NowMs());
}

std::string ProductLabel(const usage::ProductShare& p)
{
    return ProductName(p);
}

ImU32 LimitTierColor(int tier)
{
    return kTiers[std::clamp(tier, 0, 2)].fill;
}

std::string LimitSummary(const char* name, const usage::Window& w)
{
    const std::int64_t now = usage::NowMs();
    if (w.Expired(now)) return std::string(name) + ": окно закончилось, счётчик обнулился";
    std::string s = std::string(name) + ": " + Percent(w.usedPct);
    if (w.resetsAtMs > 0) s += " · сброс " + ClockTime(w.resetsAtMs, now);
    return s;
}

bool UsagePanel::DrawCompact(const usage::AccountUsage& account, const std::vector<SessionRow>& sessions)
{
    const std::int64_t now = usage::NowMs();
    const Sizes sz = TextSizes();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 primary = Col(theme::palette::Light), secondary = Col(theme::palette::MidGray);
    const float w = ImGui::GetContentRegionAvail().x;

    // Title + freshness.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Text(dl, sz.body, p, primary, "Лимиты");
        const std::int64_t at = std::max(account.fetchedAtMs, account.liveWindowsAtMs);
        if (at > 0)
        {
            // "обновлено 4 мин назад", shortened when the sidebar is narrow.
            const float titleW = TextSize(sz.body, "Лимиты").x + 12.0f;
            std::string upd = "обновлено " + Ago(at, now);
            if (TextSize(sz.small, upd.c_str()).x + titleW > w) upd = Ago(at, now);
            const ImVec2 us = TextSize(sz.small, upd.c_str());
            if (us.x + titleW <= w) Text(dl, sz.small, {p.x + w - us.x, p.y + (sz.body - sz.small) * 0.5f}, secondary, upd.c_str());
        }
        ImGui::Dummy({w, sz.body + 8.0f});
    }

    if (!account.valid) Footnote("Данных ещё нет — откройте статистику.", secondary);
    auto row = [&](const char* name, const usage::Window& win) {
        const bool expired = win.Expired(now);
        const double used = expired ? 0.0 : win.usedPct;
        const int tier = expired ? 0 : win.Tier();
        std::string right = Percent(used);
        if (!expired && win.resetsAtMs > 0) right += " · сброс " + ClockTime(win.resetsAtMs, now);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Text(dl, sz.small, p, primary, name);
        const ImVec2 rs = TextSize(sz.small, right.c_str());
        Text(dl, sz.small, {p.x + w - rs.x, p.y}, tier > 0 && kTiers[tier].text ? kTiers[tier].text : secondary, right.c_str());
        const float barY = p.y + sz.small + 6.0f, barH = 6.0f;
        dl->AddRectFilled({p.x, barY}, {p.x + w, barY + barH}, kTrackEmpty, barH * 0.5f);
        if (used > 0.0)
            dl->AddRectFilled({p.x, barY}, {p.x + std::max(barH, w * static_cast<float>(std::clamp(used, 0.0, 100.0) / 100.0)), barY + barH},
                              kTiers[tier].fill, barH * 0.5f);
        ImGui::Dummy({w, sz.small + 6.0f + barH + 10.0f});
    };
    if (account.fiveHour) row("5 часов", *account.fiveHour);
    if (account.sevenDay) row("Неделя", *account.sevenDay);

    // What the open tabs have cost so far.
    double total = 0.0;
    bool any = false;
    for (const auto& s : sessions)
        if (s.costUsd)
        {
            total += *s.costUsd;
            any = true;
        }
    if (any)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine({p.x, p.y}, {p.x + w, p.y}, IM_COL32(0x2b, 0x2a, 0x27, 255));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "$%.2f", total);
        Text(dl, sz.small, {p.x, p.y + 8.0f}, secondary, "Открытые сессии");
        const ImVec2 bs = TextSize(sz.small, buf);
        Text(dl, sz.small, {p.x + w - bs.x, p.y + 8.0f}, primary, buf);
        ImGui::Dummy({w, sz.small + 10.0f});
    }

    // The whole card opens the full statistics.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

} // namespace ui
