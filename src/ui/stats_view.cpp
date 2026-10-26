#include "ui/stats_view.hpp"
#include "theme/theme.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace ui
{
namespace
{
constexpr std::int64_t kFreshMs = 60 * 1000; // Claude Code won't rewrite its cache more often than this

ImU32 Col(const ImVec4& c)
{
    return ImGui::ColorConvertFloat4ToU32(c);
}

// Label on the left, a muted note on the right, on one line.
void CardTopLine(const char* left, const std::string& right)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, Col(theme::palette::MidGray), left);
    if (!right.empty())
    {
        const float fs = ImGui::GetFontSize() * 0.88f;
        const ImVec2 rs = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, right.c_str());
        dl->AddText(ImGui::GetFont(), fs, {p.x + w - rs.x, p.y + (ImGui::GetFontSize() - fs) * 0.5f}, widgets::kMuted, right.c_str());
    }
    ImGui::Dummy({w, ImGui::GetFontSize() + 6.0f});
}

void BigText(const char* text, float scale)
{
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void Bar(double fraction, ImU32 fill, ImU32 track)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x, h = 6.0f;
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, track, h * 0.5f);
    if (fraction > 0.0)
        dl->AddRectFilled(p, {p.x + std::max(h, w * static_cast<float>(std::clamp(fraction, 0.0, 1.0))), p.y + h}, fill, h * 0.5f);
    ImGui::Dummy({w, h + 8.0f});
}

std::string Percent(double v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", v);
    return buf;
}

// One limit window as a card: name and reset time, the figure, a bar, what's left.
void LimitCard(const char* id, const char* name, const std::optional<usage::Window>& w, float width,
               const std::vector<usage::Window>* extraWindows = nullptr)
{
    const std::int64_t now = usage::NowMs();
    widgets::CardBegin(id, nullptr, width);
    if (!w)
    {
        CardTopLine(name, "");
        BigText("—", 2.2f);
        Bar(0.0, 0, IM_COL32(255, 255, 255, 13));
        widgets::SmallMuted("нет данных");
        widgets::CardEnd();
        return;
    }
    const bool expired = w->Expired(now);
    const double used = expired ? 0.0 : w->usedPct;
    const int tier = expired ? 0 : w->Tier();
    CardTopLine(name, w->resetsAtMs > 0 ? "сброс " + LimitClock(w->resetsAtMs) : std::string());
    const std::string big = Percent(used);
    ImGui::PushStyleColor(ImGuiCol_Text, tier > 0 && LimitTierText(tier) ? ImGui::ColorConvertU32ToFloat4(LimitTierText(tier))
                                                                          : theme::palette::Light);
    BigText(big.c_str(), 2.2f);
    ImGui::PopStyleColor();
    Bar(used / 100.0, LimitTierColor(tier), used > 0.0 ? LimitTierTrack(tier) : IM_COL32(255, 255, 255, 13));
    std::string foot;
    if (expired) foot = "окно закончилось — ждём свежих цифр";
    else
    {
        foot = "осталось " + Percent(w->LeftPct());
        if (w->resetsAtMs > now) foot += " · через " + LimitDuration(w->resetsAtMs - now);
    }
    widgets::SmallMuted(foot.c_str());
    widgets::CardEnd();
    if (extraWindows && !extraWindows->empty() && ImGui::BeginItemTooltip())
    {
        for (const auto& m : *extraWindows) ImGui::TextUnformatted(LimitSummary(("Неделя · " + m.label).c_str(), m).c_str());
        ImGui::EndTooltip();
    }
}
} // namespace

void StatsView::Draw(const usage::AccountUsage& account, const std::vector<UsagePanel::SessionRow>& sessions,
                     usage::Monitor& monitor, const usage::TranscriptUsage& transcripts, const config::ModelCatalog& catalog)
{
    const float fs = ImGui::GetFontSize();
    const float gap = fs * 1.1f;
    const std::int64_t now = usage::NowMs();
    const bool running = monitor.State() == usage::Monitor::RefreshState::Running;

    // ── Header ─────────────────────────────────────────────────────────
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        BigText("Статистика", 1.5f);
        const std::int64_t at = std::max(account.fetchedAtMs, account.liveWindowsAtMs);
        const bool fresh = at > 0 && now - at < kFreshMs;
        const char* btn = icons::Label(icons::Icon::Retry, "Обновить");
        const float btnW = ImGui::CalcTextSize(btn).x + ImGui::GetStyle().FramePadding.x * 2;
        const std::string upd = running ? "обновляю…" : at > 0 ? "обновлено " + LimitAgo(at) : "";
        const float updW = ImGui::CalcTextSize(upd.c_str()).x * 0.88f;
        ImGui::SetCursorScreenPos({p.x + w - btnW - updW - 12.0f, p.y + (fs * 1.5f - fs * 0.88f) * 0.5f + 2.0f});
        widgets::SmallMuted(upd.c_str());
        ImGui::SetCursorScreenPos({p.x + w - btnW, p.y + (fs * 1.5f - ImGui::GetFrameHeight()) * 0.5f + 2.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::BeginDisabled(running || fresh);
        if (ImGui::Button(btn)) monitor.RequestRefresh();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", running ? "Спрашиваю Claude Code…"
                                    : fresh ? "Данные обновлены меньше минуты назад"
                                            : "Запустить `claude -p /usage` — локальная команда, токены не тратятся");
        ImGui::SetCursorScreenPos({p.x, p.y + fs * 1.5f + 14.0f});
    }

    if (monitor.State() == usage::Monitor::RefreshState::Failed)
    {
        widgets::Banner(("Не удалось обновить: " + monitor.Error()).c_str(), IM_COL32(0x3c, 0x1e, 0x1c, 255), IM_COL32(0xec, 0x7e, 0x7e, 255));
        ImGui::Dummy({0, 6});
    }
    if (!account.valid)
    {
        widgets::Banner(running ? "Запрашиваю лимиты у Claude Code…" : "Данных о лимитах ещё нет — нажмите «Обновить».",
                        IM_COL32(0x1b, 0x21, 0x28, 255), IM_COL32(0x9c, 0xc0, 0xe4, 255));
        ImGui::Dummy({0, 6});
    }

    // ── Cards: 5 hours · week · extra usage ────────────────────────────
    const float w = ImGui::GetContentRegionAvail().x;
    const bool inRow = w > fs * 40.0f;
    const float cardW = inRow ? (w - gap * 2) / 3.0f : 0.0f;
    LimitCard("##card_5h", "5 часов", account.fiveHour, cardW);
    if (inRow) ImGui::SameLine(0, gap);
    else ImGui::Dummy({0, gap * 0.5f});
    LimitCard("##card_week", "Неделя", account.sevenDay, cardW, &account.modelWeekly);
    if (inRow) ImGui::SameLine(0, gap);
    else ImGui::Dummy({0, gap * 0.5f});
    {
        double total = 0.0;
        bool any = false;
        for (const auto& s : sessions)
            if (s.costUsd)
            {
                total += *s.costUsd;
                any = true;
            }
        const usage::ExtraUsage& e = account.extra;
        widgets::CardBegin("##card_extra", nullptr, cardW);
        CardTopLine("Сверх подписки", "этот месяц");
        // Same height as the limit cards beside it, whose figure is set larger.
        const float bigGap = ImGui::GetFontSize() * (2.2f - 1.45f);
        ImGui::Dummy({0, bigGap * 0.5f - ImGui::GetStyle().ItemSpacing.y * 0.5f});
        if (!e.enabled)
        {
            BigText("Выключено", 1.45f);
            Bar(0.0, 0, IM_COL32(255, 255, 255, 13));
        }
        else if (e.used && e.limit && e.limit->minor > 0)
        {
            const double frac = static_cast<double>(e.used->minor) / static_cast<double>(e.limit->minor);
            BigText((e.used->Format() + " из " + e.limit->Format()).c_str(), 1.45f);
            const int tier = frac >= 0.9 ? 2 : frac >= 0.75 ? 1 : 0;
            Bar(frac, LimitTierColor(tier), LimitTierTrack(tier));
        }
        else
        {
            BigText(e.used ? e.used->Format().c_str() : "—", 1.45f);
            Bar(0.0, 0, IM_COL32(255, 255, 255, 13));
        }
        char foot[96];
        if (e.balance) std::snprintf(foot, sizeof(foot), "на балансе %s", e.balance->Format().c_str());
        else if (any) std::snprintf(foot, sizeof(foot), "открытые сессии по ценам API: $%.2f", total);
        else std::snprintf(foot, sizeof(foot), "кредиты — только сверх лимитов подписки");
        ImGui::Dummy({0, bigGap * 0.5f - ImGui::GetStyle().ItemSpacing.y * 0.5f});
        widgets::SmallMuted(foot);
        widgets::CardEnd();
    }
    ImGui::Dummy({0, gap * 0.5f});

    // ── Tokens per model ───────────────────────────────────────────────
    chart_.Draw(transcripts, account, catalog);
    ImGui::Dummy({0, gap * 0.5f});

    // ── Open sessions · products ───────────────────────────────────────
    const float halfW = inRow ? (w - gap) / 2.0f : 0.0f;
    widgets::CardBegin("##card_sessions", "Открытые сессии", halfW);
    {
        ImGui::Dummy({0, 4});
        double total = 0.0;
        if (sessions.empty()) widgets::SmallMuted("Вкладок нет — откройте сессию в разделе «Сессии».");
        auto* dl = ImGui::GetWindowDrawList();
        for (const auto& s : sessions)
        {
            if (s.costUsd) total += *s.costUsd;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  rw = ImGui::GetContentRegionAvail().x;
            char cost[32];
            if (s.costUsd) std::snprintf(cost, sizeof(cost), "$%.2f", *s.costUsd);
            else std::snprintf(cost, sizeof(cost), "—");
            const float cw = ImGui::CalcTextSize(cost).x;
            dl->AddCircleFilled({p.x + 4.0f, p.y + fs * 0.5f}, fs * 0.22f,
                                s.running ? Col(theme::palette::Green) : IM_COL32(0x6b, 0x6a, 0x64, 255));
            ImGui::PushClipRect(p, {p.x + rw - cw - 12.0f, p.y + fs + 4.0f}, true);
            dl->AddText({p.x + 16.0f, p.y}, Col(theme::palette::Light), s.title.c_str());
            ImGui::PopClipRect();
            dl->AddText({p.x + rw - cw, p.y}, Col(theme::palette::Light), cost);
            ImGui::Dummy({rw, fs + 8.0f});
        }
        if (sessions.size() > 1)
        {
            widgets::CardDivider();
            ImGui::Dummy({0, 4});
            char buf[32];
            std::snprintf(buf, sizeof(buf), "$%.2f", total);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rw = ImGui::GetContentRegionAvail().x;
            dl->AddText(p, Col(theme::palette::MidGray), "Итого");
            dl->AddText({p.x + rw - ImGui::CalcTextSize(buf).x, p.y}, Col(theme::palette::Light), buf);
            ImGui::Dummy({rw, fs + 4.0f});
        }
        if (!sessions.empty())
        {
            ImGui::Dummy({0, 2});
            widgets::SmallMuted("По прейскуранту API — столько насчитал Claude Code; часть покрывает подписка.");
        }
    }
    widgets::CardEnd();
    if (inRow) ImGui::SameLine(0, gap);
    else ImGui::Dummy({0, gap * 0.5f});
    widgets::CardBegin("##card_products", "Неделя по продуктам", halfW);
    {
        ImGui::Dummy({0, 4});
        if (account.products.empty()) widgets::SmallMuted("Сервер пока не прислал разбивку.");
        for (const auto& pr : account.products)
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  rw = ImGui::GetContentRegionAvail().x;
            const std::string name = ProductLabel(pr), pct = Percent(pr.percent);
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddText(p, Col(theme::palette::Light), name.c_str());
            dl->AddText({p.x + rw - ImGui::CalcTextSize(pct.c_str()).x, p.y}, Col(theme::palette::MidGray), pct.c_str());
            ImGui::Dummy({rw, fs + 4.0f});
            Bar(pr.percent / 100.0, LimitTierColor(0), IM_COL32(255, 255, 255, 13));
        }
        if (!account.products.empty()) widgets::SmallMuted("Доли — из ответа сервера, который получил Claude Code.");
    }
    widgets::CardEnd();
}
} // namespace ui
