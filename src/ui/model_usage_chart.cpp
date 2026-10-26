#include "ui/model_usage_chart.hpp"
#include "theme/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace ui
{
namespace
{
constexpr std::int64_t kHourMs = 3600LL * 1000;
constexpr std::int64_t kDayMs  = 24 * kHourMs;

ImU32 Col(const ImVec4& c, float a = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a});
}

ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

// "3.4M", "628K", "1008"
std::string Tokens(std::int64_t n)
{
    char buf[32];
    const double v = static_cast<double>(n);
    if (n < 1000) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
    else if (n < 1'000'000) std::snprintf(buf, sizeof(buf), v < 100'000 ? "%.1fK" : "%.0fK", v / 1e3);
    else if (n < 1'000'000'000) std::snprintf(buf, sizeof(buf), v < 100'000'000 ? "%.1fM" : "%.0fM", v / 1e6);
    else std::snprintf(buf, sizeof(buf), "%.2fB", v / 1e9);
    return buf;
}

std::tm Local(std::int64_t ms)
{
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    ::localtime_r(&secs, &tm);
    return tm;
}

std::string Clock(std::int64_t ms)
{
    const std::tm t = Local(ms);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return buf;
}

std::string DayLabel(std::int64_t ms)
{
    static constexpr const char* kWeekdays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    const std::tm t = Local(ms);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s %d", kWeekdays[t.tm_wday], t.tm_mday);
    return buf;
}

std::int64_t Metric(const usage::TokenCounts& t, int metric)
{
    switch (metric)
    {
    case 1:  return t.output;
    case 2:  return t.input + t.cacheWrite + t.cacheRead + t.output;
    default: return t.input + t.cacheWrite + t.output;
    }
}

// The limit window `w` is in now: [reset − period, reset). A reset time in the
// past means the window rolled over since Claude Code last asked the server.
struct Range
{
    std::int64_t start, end;
    bool         known;
};
Range WindowRange(const std::optional<usage::Window>& w, std::int64_t period, std::int64_t now)
{
    if (w && w->resetsAtMs > 0)
    {
        std::int64_t end = w->resetsAtMs;
        if (end <= now) end += ((now - end) / period + 1) * period;
        return {end - period, end, true};
    }
    return {now - period, now, false};
}

struct ModelRow
{
    std::string  id;
    std::string  name;
    ImU32        color = 0;
    std::int64_t total = 0;
    usage::TokenCounts counts;
};

} // namespace

void ModelUsageChart::Draw(const usage::TranscriptUsage& data, const usage::AccountUsage& account,
                           const config::ModelCatalog& catalog)
{
    const float fs = ImGui::GetFontSize();
    const std::int64_t now = usage::NowMs();
    const std::int64_t period = window_ == 0 ? 7 * kDayMs : 5 * kHourMs;
    const std::int64_t bucket = window_ == 0 ? kDayMs : kHourMs / 2;
    const Range range = WindowRange(window_ == 0 ? account.sevenDay : account.fiveHour, period, now);
    const int nBuckets = static_cast<int>(period / bucket);

    widgets::CardBegin("##models_card", nullptr);

    // ── Heading: title + window on the left, switches on the right ─────
    static constexpr const char* kWindows[] = {"Неделя", "5 часов"};
    static constexpr const char* kMetrics[] = {"Без чтения кэша", "Вывод", "Всё"};
    const float switchesW = widgets::SegmentedWidth(kWindows, 2) + 10.0f + widgets::SegmentedWidth(kMetrics, 3) + 10.0f +
                            ImGui::CalcTextSize("(?)").x;
    const ImVec2 head = ImGui::GetCursorScreenPos();
    const float  w    = ImGui::GetContentRegionAvail().x;
    const bool   oneLine = w > switchesW + fs * 18.0f;

    ImGui::PushFont(nullptr, fs * 1.1f);
    ImGui::TextUnformatted("Токены по моделям");
    ImGui::PopFont();
    {
        std::string sub = range.known ? "с " + (window_ == 0 ? DayLabel(range.start) + ", " : std::string()) + Clock(range.start) +
                                            " до сброса " + (window_ == 0 ? DayLabel(range.end) + ", " : std::string()) + Clock(range.end)
                                      : std::string(window_ == 0 ? "последние 7 дней" : "последние 5 часов") +
                                            " (время сброса пока неизвестно)";
        sub += " · только этот компьютер";
        widgets::SmallMuted(sub.c_str());
    }
    const float afterTitle = ImGui::GetCursorScreenPos().y;
    if (oneLine) ImGui::SetCursorScreenPos({head.x + w - switchesW, head.y + 2.0f});
    else ImGui::Dummy({0, 4});
    widgets::Segmented("##window", kWindows, 2, &window_);
    ImGui::SameLine(0, 10.0f);
    widgets::Segmented("##metric", kMetrics, 3, &metric_);
    ImGui::SameLine(0, 10.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(fs * 30.0f);
        ImGui::TextUnformatted("Без чтения кэша — ввод, запись в кэш и вывод. Чтение кэша обычно в десятки раз больше "
                               "остального и дешевле, поэтому по умолчанию не считается.");
        ImGui::TextColored(theme::palette::MidGray, "Токены из транскриптов Claude Code на этом компьютере. Лимиты сервер "
                                                    "считает по-своему (модели весят по-разному), так что это не доля лимита.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (oneLine) ImGui::SetCursorScreenPos({head.x, std::max(afterTitle, ImGui::GetCursorScreenPos().y)});
    ImGui::Dummy({0, 8});

    if (data.ScannedAtMs() == 0)
    {
        ImGui::TextColored(theme::palette::MidGray, "Читаю транскрипты…");
        widgets::CardEnd();
        return;
    }

    // ── Aggregate ──────────────────────────────────────────────────────
    std::map<std::string, ModelRow> byId;
    std::vector<std::map<std::string, std::int64_t>> buckets(static_cast<std::size_t>(nBuckets));
    for (const auto& e : data.Since(range.start))
    {
        if (e.atMs >= range.end) break;
        ModelRow& r = byId[e.model];
        r.counts += e.tokens;
        const std::int64_t v = Metric(e.tokens, metric_);
        r.total += v;
        const int b = std::clamp(static_cast<int>((e.atMs - range.start) / bucket), 0, nBuckets - 1);
        buckets[static_cast<std::size_t>(b)][e.model] += v;
    }

    std::vector<ModelRow> rows;
    for (auto& [id, r] : byId)
    {
        if (r.total <= 0) continue;
        r.id = id;
        const config::ModelInfo* info = catalog.Find(id);
        r.name = info ? info->displayName : (id.starts_with("claude-") ? id.substr(7) : id);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const ModelRow& a, const ModelRow& b) { return a.total > b.total; });

    // Colour by family; later models of the same family get lighter shades.
    std::map<std::string, int> familySeen;
    for (auto& r : rows)
    {
        const config::ModelInfo* info = catalog.Find(r.id);
        std::string family = info ? info->family : "";
        if (family.empty())
            for (const char* f : {"opus", "sonnet", "haiku", "fable"})
                if (r.id.find(f) != std::string::npos) family = f;
        ImVec4 base = theme::palette::MidGray;
        if (family == "opus") base = theme::palette::Orange;
        else if (family == "sonnet") base = theme::palette::Blue;
        else if (family == "haiku") base = theme::palette::Green;
        else if (family == "fable") base = ImVec4{0xa4 / 255.0f, 0x8b / 255.0f, 0xd0 / 255.0f, 1.0f};
        const int n = familySeen[family]++;
        r.color = Col(Mix(base, theme::palette::Light, std::min(0.75f, 0.5f * static_cast<float>(n))));
    }

    if (rows.empty())
    {
        ImGui::TextColored(theme::palette::MidGray, "В этом окне ответов Claude ещё не было.");
        widgets::CardEnd();
        return;
    }
    std::int64_t all = 0;
    for (const auto& r : rows) all += r.total;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 muted = widgets::kMuted;

    // ── Share bar: every model's slice of the window, in one line ──────
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  h = 10.0f, gap = 2.0f;
        float x = p.x;
        const float usable = w - gap * static_cast<float>(rows.size() - 1);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const float sw = std::max(2.0f, usable * static_cast<float>(static_cast<double>(rows[i].total) / static_cast<double>(all)));
            ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
            if (i == 0) corners |= ImDrawFlags_RoundCornersLeft;
            if (i + 1 == rows.size()) corners |= ImDrawFlags_RoundCornersRight;
            const float x1 = i + 1 == rows.size() ? p.x + w : std::min(p.x + w, x + sw);
            dl->AddRectFilled({x, p.y}, {x1, p.y + h}, rows[i].color, h * 0.5f, corners);
            x = x1 + gap;
        }
        ImGui::InvisibleButton("##share", {w, h});
        if (ImGui::IsItemHovered())
        {
            const float mx = ImGui::GetMousePos().x;
            float acc = p.x;
            for (const auto& r : rows)
            {
                const float sw = usable * static_cast<float>(static_cast<double>(r.total) / static_cast<double>(all)) + gap;
                if (mx < acc + sw || &r == &rows.back())
                {
                    ImGui::SetTooltip("%s — %.1f%%", r.name.c_str(), 100.0 * static_cast<double>(r.total) / static_cast<double>(all));
                    break;
                }
                acc += sw;
            }
        }
        ImGui::Dummy({0, 10});
    }

    // ── Bars, legend beside (or under) them ────────────────────────────
    const float legendW = fs * 15.0f;
    const bool  legendBeside = w > legendW + fs * 30.0f;
    const float chartW = legendBeside ? w - legendW - fs * 2.0f : w;

    std::int64_t peak = 1;
    for (const auto& b : buckets)
    {
        std::int64_t sum = 0;
        for (const auto& [m, v] : b) sum += v;
        peak = std::max(peak, sum);
    }
    const double mag = std::pow(10.0, std::floor(std::log10(static_cast<double>(peak))));
    double top = mag;
    for (double k : {1.0, 2.0, 5.0, 10.0})
        if (k * mag >= static_cast<double>(peak))
        {
            top = k * mag;
            break;
        }

    const float small  = fs * 0.85f;
    const float axisW  = ImGui::CalcTextSize("000.0M").x * 0.85f + 8.0f;
    const ImVec2 p0    = ImGui::GetCursorScreenPos();
    const float  plotH = fs * 12.0f;
    const float  labelH = small + 6.0f;
    const ImVec2 plotMin{p0.x + axisW, p0.y + small * 0.5f};
    const ImVec2 plotMax{p0.x + chartW, plotMin.y + plotH};

    for (int g = 0; g <= 2; ++g)
    {
        const float y = plotMax.y - plotH * static_cast<float>(g) / 2.0f;
        dl->AddLine({plotMin.x, y}, {plotMax.x, y}, widgets::kLine);
        const std::string lbl = Tokens(static_cast<std::int64_t>(top * g / 2.0));
        const ImVec2 ls = ImGui::CalcTextSize(lbl.c_str());
        dl->AddText(ImGui::GetFont(), small, {plotMin.x - 8.0f - ls.x * 0.85f, y - small * 0.5f}, muted, lbl.c_str());
    }

    const float slot = (plotMax.x - plotMin.x) / static_cast<float>(nBuckets);
    const float barW = std::max(3.0f, slot * 0.6f);
    ImGui::SetCursorScreenPos(plotMin);
    ImGui::InvisibleButton("##chart", {plotMax.x - plotMin.x, plotH + labelH});
    const bool hovered = ImGui::IsItemHovered();
    const int  hoverB  = hovered ? std::clamp(static_cast<int>((ImGui::GetMousePos().x - plotMin.x) / slot), 0, nBuckets - 1) : -1;

    for (int b = 0; b < nBuckets; ++b)
    {
        const std::int64_t bStart = range.start + b * bucket;
        const bool future  = bStart > now;
        const bool current = !future && now < bStart + bucket;
        const float x0 = plotMin.x + slot * static_cast<float>(b) + (slot - barW) * 0.5f;
        if (b == hoverB)
            dl->AddRectFilled({plotMin.x + slot * b, plotMin.y}, {plotMin.x + slot * (b + 1), plotMax.y}, IM_COL32(255, 255, 255, 8), 6.0f);

        // Stack bottom-up in legend order (biggest model at the bottom); round the top segment.
        const auto& bk = buckets[static_cast<std::size_t>(b)];
        std::vector<std::pair<float, ImU32>> segs;
        for (const auto& r : rows)
            if (auto it = bk.find(r.id); it != bk.end() && it->second > 0)
                segs.push_back({plotH * static_cast<float>(static_cast<double>(it->second) / top), r.color});
        float y = plotMax.y;
        for (std::size_t i = 0; i < segs.size(); ++i)
        {
            const bool last = i + 1 == segs.size();
            dl->AddRectFilled({x0, y - segs[i].first}, {x0 + barW, y}, segs[i].second, last ? std::min(4.0f, segs[i].first) : 0.0f,
                              last ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersNone);
            y -= segs[i].first;
        }

        if (window_ == 0 || b % 2 == 0)
        {
            const std::string lbl = window_ == 0 ? DayLabel(bStart) : Clock(bStart);
            const ImVec2 ls = ImGui::CalcTextSize(lbl.c_str());
            const ImU32 c = current ? Col(theme::palette::Light) : future ? IM_COL32(0x5a, 0x59, 0x55, 255) : muted;
            dl->AddText(ImGui::GetFont(), small, {plotMin.x + slot * (b + 0.5f) - ls.x * 0.85f * 0.5f, plotMax.y + 5.0f}, c, lbl.c_str());
        }
    }

    if (hoverB >= 0)
    {
        const std::int64_t bStart = range.start + hoverB * bucket;
        const auto& bk = buckets[static_cast<std::size_t>(hoverB)];
        ImGui::BeginTooltip();
        if (window_ == 0) ImGui::Text("%s", DayLabel(bStart).c_str());
        else ImGui::Text("%s–%s", Clock(bStart).c_str(), Clock(bStart + bucket).c_str());
        std::int64_t sum = 0;
        for (const auto& r : rows)
        {
            auto it = bk.find(r.id);
            if (it == bk.end() || it->second <= 0) continue;
            sum += it->second;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(r.color), "\xe2\x96\xa0");
            ImGui::SameLine();
            ImGui::Text("%s  %s", r.name.c_str(), Tokens(it->second).c_str());
        }
        if (sum == 0) ImGui::TextColored(theme::palette::MidGray, bStart > now ? "ещё впереди" : "ответов не было");
        else if (rows.size() > 1) ImGui::TextColored(theme::palette::MidGray, "всего %s", Tokens(sum).c_str());
        ImGui::EndTooltip();
    }
    const float chartBottom = plotMax.y + labelH + 4.0f;

    // ── Legend ─────────────────────────────────────────────────────────
    const ImVec2 lg = legendBeside ? ImVec2{p0.x + chartW + fs * 2.0f, p0.y} : ImVec2{p0.x, chartBottom + 8.0f};
    const float  lw = legendBeside ? legendW : w;
    ImGui::SetCursorScreenPos(lg);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(ImGui::GetFont(), small, p, muted, "МОДЕЛЬ");
        const char* right = "токены · доля";
        dl->AddText(ImGui::GetFont(), small, {p.x + lw - ImGui::CalcTextSize(right).x * 0.85f, p.y}, muted, right);
        ImGui::Dummy({lw, small + 8.0f});
    }
    const float valW = ImGui::CalcTextSize("000.0M").x, shareW = ImGui::CalcTextSize("100%").x;
    for (const auto& r : rows)
    {
        ImGui::SetCursorScreenPos({lg.x, ImGui::GetCursorScreenPos().y});
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sw = fs * 0.6f;
        dl->AddRectFilled({p.x, p.y + (fs - sw) * 0.5f}, {p.x + sw, p.y + (fs + sw) * 0.5f}, r.color, 2.0f);
        ImGui::PushClipRect({p.x, p.y}, {p.x + lw - valW - shareW - 16.0f, p.y + fs + 2}, true);
        dl->AddText({p.x + sw + 8.0f, p.y}, Col(theme::palette::Light), r.name.c_str());
        ImGui::PopClipRect();
        char share[16];
        const double pct = 100.0 * static_cast<double>(r.total) / static_cast<double>(all);
        if (pct < 1.0) std::snprintf(share, sizeof(share), "<1%%");
        else std::snprintf(share, sizeof(share), "%.0f%%", pct);
        const std::string val = Tokens(r.total);
        dl->AddText({p.x + lw - shareW - 10.0f - ImGui::CalcTextSize(val.c_str()).x, p.y}, Col(theme::palette::Light), val.c_str());
        dl->AddText({p.x + lw - ImGui::CalcTextSize(share).x, p.y}, muted, share);
        ImGui::Dummy({lw, fs + 6.0f});
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(r.id.c_str());
            ImGui::TextColored(theme::palette::MidGray, "ввод %s · запись в кэш %s · чтение кэша %s · вывод %s",
                               Tokens(r.counts.input).c_str(), Tokens(r.counts.cacheWrite).c_str(),
                               Tokens(r.counts.cacheRead).c_str(), Tokens(r.counts.output).c_str());
            ImGui::EndTooltip();
        }
    }
    {
        ImGui::SetCursorScreenPos({lg.x, ImGui::GetCursorScreenPos().y + 2.0f});
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine({p.x, p.y}, {p.x + lw, p.y}, widgets::kLine);
        const std::string total = Tokens(all);
        dl->AddText({p.x, p.y + 8.0f}, Col(theme::palette::MidGray), "Всего");
        dl->AddText({p.x + lw - ImGui::CalcTextSize(total.c_str()).x, p.y + 8.0f}, Col(theme::palette::Light), total.c_str());
        ImGui::Dummy({lw, fs + 12.0f});
    }

    ImGui::SetCursorScreenPos({p0.x, std::max(ImGui::GetCursorScreenPos().y, chartBottom)});
    ImGui::Dummy({w, 4});
    widgets::CardEnd();
}
} // namespace ui
