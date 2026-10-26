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
constexpr std::int64_t kFreshMs = 60 * 1000;      // Claude Code won't rewrite its cache more often than this
constexpr std::int64_t kStaleMs = 60 * 60 * 1000; // …and stops trusting it after this
constexpr std::size_t  kMaxSessionRows = 6;

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

// One row of the page: title (+ a second line under it) on the left, a bar, a figure on the right.
struct Meter
{
    std::string title;
    std::string sub;
    std::string value;
    double      fraction = 0.0; // 0..1, how much of the bar is filled
    int         tier = 0;
    bool        empty = false;  // nothing to show yet: the bar stays a bare track
};

void MeterRow(const Meter& m, float labelW, float valueW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Sizes sz = TextSizes();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  gap = ImGui::GetStyle().ItemSpacing.x * 1.5f;

    const ImU32 primary   = Col(theme::palette::Light);
    const ImU32 secondary = Col(theme::palette::MidGray);
    const Tier& tier      = kTiers[std::clamp(m.tier, 0, 2)];

    Text(dl, sz.body, p, primary, m.title.c_str());
    if (!m.sub.empty()) Text(dl, sz.small, {p.x, p.y + sz.body + 2.0f}, secondary, m.sub.c_str());

    const float barH  = std::max(4.0f, std::round(sz.body * 0.28f));
    const float barX0 = p.x + labelW + gap, barX1 = p.x + w - valueW - gap;
    const float cy    = p.y + sz.body * 0.55f;
    if (barX1 > barX0)
    {
        dl->AddRectFilled({barX0, cy - barH * 0.5f}, {barX1, cy + barH * 0.5f}, m.empty ? kTrackEmpty : tier.track, barH * 0.5f);
        if (!m.empty && m.fraction > 0.0)
        {
            const float end = std::max(barX0 + barH, barX0 + static_cast<float>(std::clamp(m.fraction, 0.0, 1.0)) * (barX1 - barX0));
            dl->AddRectFilled({barX0, cy - barH * 0.5f}, {end, cy + barH * 0.5f}, tier.fill, barH * 0.5f);
        }
    }
    const ImVec2 vs = TextSize(sz.small, m.value.c_str());
    Text(dl, sz.small, {p.x + w - vs.x, p.y + (sz.body - sz.small) * 0.5f}, tier.text ? tier.text : secondary, m.value.c_str());

    ImGui::Dummy({w, sz.body + (m.sub.empty() ? 0.0f : sz.small + 2.0f) + ImGui::GetStyle().ItemSpacing.y * 1.5f});
}

// Section title in the page's 15 px style, then a little air. `help` adds a "(?)" beside it.
void SectionTitle(const char* text, const char* help = nullptr)
{
    const Sizes sz = TextSizes();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Text(ImGui::GetWindowDrawList(), sz.heading, p, Col(theme::palette::Light), text);
    ImGui::Dummy({TextSize(sz.heading, text).x, sz.heading + 2.0f});
    if (!help) return;
    ImGui::SameLine();
    HelpMarker(help);
}

void Footnote(const std::string& text, ImU32 color)
{
    const Sizes sz = TextSizes();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Text(ImGui::GetWindowDrawList(), sz.small, p, color, text.c_str());
    ImGui::Dummy({0.0f, sz.small + 2.0f});
}

// The row for a plan window; nothing at all when the window isn't known.
Meter WindowMeter(const std::string& title, const usage::Window& w, std::int64_t now)
{
    Meter m;
    m.title = title;
    if (w.Expired(now))
    {
        m.sub   = "Окно закончилось " + ClockTime(w.resetsAtMs, now);
        m.value = "0% занято";
        m.empty = true;
        return m;
    }
    m.fraction = w.usedPct / 100.0;
    m.tier     = w.Tier();
    m.value    = Percent(w.usedPct) + " занято";
    m.sub      = w.resetsAtMs > 0 ? "Сброс " + ClockTime(w.resetsAtMs, now) + "  ·  осталось " + Percent(w.LeftPct())
                                  : "Осталось " + Percent(w.LeftPct());
    if (w.usedPct <= 0.0) m.empty = true;
    return m;
}

void MeterTooltip(const usage::Window& w, std::int64_t now)
{
    if (!ImGui::BeginItemTooltip()) return;
    if (w.Expired(now)) ImGui::TextUnformatted("Окно уже закончилось — счётчик обнулился, цифры ещё не обновлены.");
    else
    {
        ImGui::Text("Использовано %.0f%%, осталось %.0f%%", w.usedPct, w.LeftPct());
        if (w.resetsAtMs > 0)
            ImGui::TextColored(theme::palette::MidGray, "Сброс через %s (%s)", Duration(w.resetsAtMs - now).c_str(),
                               ClockTime(w.resetsAtMs, now).c_str());
    }
    ImGui::EndTooltip();
}

const char* ProductName(const usage::ProductShare& p)
{
    if (p.key == "chat") return "Чаты";
    if (p.key == "other") return "Прочее";
    return p.name.c_str();
}
} // namespace

void UsagePanel::Draw(const usage::AccountUsage& account, const std::vector<SessionRow>& sessions, usage::Monitor& monitor)
{
    const std::int64_t now = usage::NowMs();
    const auto state       = monitor.State();
    const bool running     = state == usage::Monitor::RefreshState::Running;
    const Sizes sz         = TextSizes();

    // Callers wrap this in their own bordered/resizable region (main.cpp's
    // sidebar panel stack), which already gives it a top edge — no Rule()
    // needed here on top of that.
    ImGui::BeginChild("##usage_panel", {0, 0}, ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ── Title line: collapse arrow + name, refresh state on the right ────
    const std::int64_t updatedAt = std::max(account.fetchedAtMs, account.liveWindowsAtMs);
    const std::int64_t age       = updatedAt > 0 ? now - updatedAt : -1;
    const bool fresh             = age >= 0 && age < kFreshMs;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Light);
    const bool open = ImGui::TreeNodeEx("Лимиты и расход###usage", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                      ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor();

    const float iconW = ImGui::CalcTextSize(icons::Glyph(icons::Icon::Retry)).x + ImGui::GetStyle().FramePadding.x * 2;
    std::string updated;
    if (running) updated = "обновляю…";
    else if (updatedAt > 0) updated = "обновлено " + Ago(updatedAt, now);
    ImGui::PushFont(nullptr, sz.small);
    const float labelW = ImGui::CalcTextSize(updated.c_str()).x;
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - iconW - ImGui::GetStyle().ItemSpacing.x - labelW);
    {
        const bool stale = !running && account.liveWindowsAtMs == 0 && age > kStaleMs;
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(nullptr, sz.small);
        ImGui::PushStyleColor(ImGuiCol_Text, stale ? theme::palette::Orange : theme::palette::MidGray);
        ImGui::TextUnformatted(updated.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            if (account.liveWindowsAtMs > 0)
                ImGui::TextUnformatted(("5 часов и неделя — из открытой сессии, " + Ago(account.liveWindowsAtMs, now) + ".").c_str());
            if (account.fetchedAtMs > 0)
                ImGui::TextUnformatted(((account.liveWindowsAtMs > 0 ? "Остальное" : "Все цифры") + std::string(" — ответ сервера, который Claude Code получил ") +
                                        Ago(account.fetchedAtMs, now) + ".").c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::BeginDisabled(running || fresh);
    ImGui::PushID("usage_refresh");
    if (ImGui::SmallButton(icons::Glyph(icons::Icon::Retry))) monitor.RequestRefresh();
    ImGui::PopID();
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", running ? "Спрашиваю Claude Code…"
                                : fresh ? "Данные обновлены меньше минуты назад"
                                        : "Запустить `claude -p /usage` — локальная команда, токены не тратятся");

    if (open)
    {
        if (!account.valid)
        {
            ImGui::TextColored(theme::palette::MidGray, running ? "Запрашиваю у Claude Code…" : "Данных ещё нет — нажмите ⟳.");
        }
        else
        {
            // Headline: how far into the week you are (or, without a weekly figure, into the session).
            const usage::Window* head = account.sevenDay ? &*account.sevenDay : account.fiveHour ? &*account.fiveHour : nullptr;
            if (head)
            {
                const double used = head->Expired(now) ? 0.0 : head->usedPct;
                const std::string line = Percent(used) + (account.sevenDay ? " недельного лимита использовано." : " лимита сессии использовано.");
                const ImVec2 p = ImGui::GetCursorScreenPos();
                Text(ImGui::GetWindowDrawList(), sz.headline, {p.x, p.y + 2.0f}, Col(theme::palette::Light), line.c_str());
                ImGui::Dummy({0.0f, sz.headline + ImGui::GetStyle().ItemSpacing.y});
            }

            // Label and figure columns are as wide as the longest entry, the bars take what is left.
            std::vector<std::pair<Meter, const usage::Window*>> rows;
            if (account.fiveHour) rows.push_back({WindowMeter("Текущая сессия", *account.fiveHour, now), &*account.fiveHour});
            if (account.sevenDay) rows.push_back({WindowMeter("Эта неделя", *account.sevenDay, now), &*account.sevenDay});
            for (const auto& w : account.modelWeekly) rows.push_back({WindowMeter("Неделя · " + w.label, w, now), &w});
            float labelW = 0.0f;
            for (const auto& [m, w] : rows) labelW = std::max(labelW, TextSize(sz.body, m.title.c_str()).x);
            const float valueW = TextSize(sz.small, "100% занято").x;
            ImGui::Dummy({0.0f, 2.0f});
            for (const auto& [m, w] : rows)
            {
                MeterRow(m, labelW, valueW);
                MeterTooltip(*w, now);
            }

            // ── Extra usage ─────────────────────────────────────────────
            const usage::ExtraUsage& e = account.extra;
            ImGui::Dummy({0.0f, 4.0f});
            SectionTitle("Расход сверх подписки");
            const ImU32 secondary = Col(theme::palette::MidGray);
            if (!e.enabled) Footnote("Extra usage выключен", secondary);
            else if (e.used && e.limit && e.limit->minor > 0)
            {
                const double frac = std::clamp(static_cast<double>(e.used->minor) / static_cast<double>(e.limit->minor), 0.0, 1.0);
                const usage::Money left{std::max<std::int64_t>(0, e.limit->minor - e.used->minor), e.limit->currency, e.limit->exponent};
                Meter m;
                m.title    = e.used->Format();
                m.sub      = "из " + e.limit->Format() + " лимита на месяц  ·  осталось " + left.Format();
                m.value    = Percent(frac * 100.0) + " занято";
                m.fraction = frac;
                m.tier     = frac >= 0.9 ? 2 : frac >= 0.75 ? 1 : 0;
                m.empty    = e.used->minor <= 0;
                MeterRow(m, TextSize(sz.body, m.title.c_str()).x, valueW);
            }
            else if (e.used)
            {
                Footnote(e.used->Format() + " в этом месяце  ·  " + (e.limit ? "лимит " + e.limit->Format() : std::string("без лимита трат")), secondary);
                if (e.balance) Footnote("Кредиты на балансе: " + e.balance->Format(), secondary);
            }
            else Footnote("нет данных", secondary);
            Footnote("Кредиты списываются автоматически, когда кончился лимит подписки.", Col(theme::palette::MidGray));

            // ── Where this week's usage went ────────────────────────────
            if (!account.products.empty())
            {
                ImGui::Dummy({0.0f, 2.0f});
                ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Light);
                const bool showProducts = ImGui::TreeNodeEx("Использование за неделю по продуктам###products", ImGuiTreeNodeFlags_NoTreePushOnOpen);
                ImGui::PopStyleColor();
                if (showProducts)
                {
                    float nameW = 0.0f;
                    for (const auto& pr : account.products) nameW = std::max(nameW, TextSize(sz.body, ProductName(pr)).x);
                    const float pctW = TextSize(sz.small, "100%").x;
                    ImGui::Dummy({0.0f, 2.0f});
                    for (const auto& pr : account.products)
                    {
                        Meter m;
                        m.title    = ProductName(pr);
                        m.value    = Percent(pr.percent);
                        m.fraction = pr.percent / 100.0;
                        m.empty    = pr.percent <= 0.0;
                        MeterRow(m, nameW, pctW);
                    }
                }
            }
        }

        // ── Sessions ────────────────────────────────────────────────────
        if (!sessions.empty())
        {
            ImGui::Dummy({0.0f, 4.0f});
            SectionTitle("Открытые сессии",
                         "Сколько Claude Code насчитал за сессию по прейскуранту API. Часть покрывает подписка — "
                         "в Extra usage уходит только то, что сверх лимитов, а по сессиям сервер это не разбивает. "
                         "Цифра появляется после первого ответа в сессии.");

            double total = 0.0;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            std::size_t shown = 0;
            for (const auto& s : sessions)
            {
                if (s.costUsd) total += *s.costUsd;
                if (shown >= kMaxSessionRows) continue;
                ++shown;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  w = ImGui::GetContentRegionAvail().x;
                char cost[32];
                if (s.costUsd) std::snprintf(cost, sizeof(cost), "$%.2f", *s.costUsd);
                else std::snprintf(cost, sizeof(cost), "—");
                const ImVec2 cs = TextSize(sz.small, cost);

                const float dot = sz.body * 0.28f;
                dl->AddCircleFilled({p.x + dot + 1.0f, p.y + sz.body * 0.55f}, dot, s.running ? Col(theme::palette::Green) : Col(theme::palette::MidGray));
                const float titleX = p.x + dot * 2.0f + 8.0f;
                ImGui::PushClipRect({titleX, p.y}, {p.x + w - cs.x - 10.0f, p.y + sz.body + 4.0f}, true);
                Text(dl, sz.body, {titleX, p.y}, Col(theme::palette::Light), s.title.c_str());
                ImGui::PopClipRect();
                Text(dl, sz.small, {p.x + w - cs.x, p.y + (sz.body - sz.small) * 0.5f}, Col(theme::palette::MidGray), cost);
                ImGui::Dummy({w, sz.body + ImGui::GetStyle().ItemSpacing.y * 0.5f});
            }
            if (sessions.size() > shown)
                Footnote("и ещё " + std::to_string(sessions.size() - shown) + "…", Col(theme::palette::MidGray));
            if (sessions.size() > 1)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Итого по открытым: $%.2f", total);
                Footnote(buf, Col(theme::palette::MidGray));
            }
        }

        if (state == usage::Monitor::RefreshState::Failed)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTiers[2].text));
            ImGui::TextWrapped("Не удалось обновить: %s", monitor.Error().c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    height_ = ImGui::GetItemRectSize().y;
}
} // namespace ui
