// Headless check of the usage readers.
//   usage_probe --parse     — offline self-test of the cache / status-line parsers
//   usage_probe [FILE]      — what the panel would show from FILE (default ~/.claude.json)
//   usage_probe --refresh   — runs `claude -p /usage` (no tokens) and shows the result
#include "config/json_file.hpp"
#include "config/paths.hpp"
#include "usage/usage.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
int failures = 0;
void Expect(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

void Print(const usage::AccountUsage& a)
{
    const auto now = usage::NowMs();
    std::printf("valid=%d fetched %lld s ago\n", a.valid, a.fetchedAtMs ? static_cast<long long>((now - a.fetchedAtMs) / 1000) : -1LL);
    auto win = [&](const char* name, const std::optional<usage::Window>& w) {
        if (!w) return std::printf("%-10s —\n", name), void();
        std::printf("%-10s used %.0f%%  left %.0f%%  resets in %lld min%s\n", name, w->usedPct, w->LeftPct(),
                    static_cast<long long>((w->resetsAtMs - now) / 60000), w->Expired(now) ? "  (already rolled over)" : "");
    };
    win("5 hours", a.fiveHour);
    win("week", a.sevenDay);
    for (const auto& w : a.modelWeekly) std::printf("week %-6s used %.0f%%\n", w.label.c_str(), w.usedPct);
    std::printf("extra usage: enabled=%d used=%s limit=%s balance=%s\n", a.extra.enabled,
                a.extra.used ? a.extra.used->Format().c_str() : "—", a.extra.limit ? a.extra.limit->Format().c_str() : "—",
                a.extra.balance ? a.extra.balance->Format().c_str() : "—");
}

int SelfTest()
{
    using namespace usage;
    std::puts("timestamps");
    Expect(ParseIso8601Ms("1970-01-01T00:00:01Z") == 1000, "Z suffix");
    Expect(ParseIso8601Ms("2026-09-25T15:20:00.668946+00:00") == 1790349600669, "fraction and +00:00");
    Expect(ParseIso8601Ms("2026-09-25T18:20:00+03:00") == 1790349600000, "+03:00 offset is subtracted");
    Expect(ParseIso8601Ms("") == 0 && ParseIso8601Ms("nonsense") == 0, "garbage → 0");

    std::puts("money");
    Money usd{5375, "USD", 2};
    Expect(usd.Format() == "$53.75", "$53.75");
    Expect((Money{1250, "EUR", 2}).Format() == "12.50 EUR", "12.50 EUR");
    Expect((Money{500, "JPY", 0}).Format() == "500 JPY", "no decimals");

    std::puts("cache: newer `spend` shape");
    {
        auto cfg = config::Json::parse(R"({"cachedUsageUtilization":{"fetchedAtMs":1000,"accountUuid":"x","utilization":{
            "five_hour":{"utilization":57,"resets_at":"2026-09-25T15:20:00.668946+00:00"},
            "seven_day":{"utilization":7.5,"resets_at":"2026-10-02T04:00:00+00:00"},
            "seven_day_opus":null,
            "extra_usage":{"is_enabled":true,"monthly_limit":null,"used_credits":4223,"currency":"USD","decimal_places":2},
            "spend":{"used":{"amount_minor":5375,"currency":"USD","exponent":2},"limit":null,"balance":null,"cap":null,"enabled":true},
            "seven_day_breakdown":{"rows":[{"key":"claude_code","display_name":"Claude Code","percent":89},{"key":"chat","display_name":"Chats","percent":11},{"key":"junk"}]},
            "limits":[{"kind":"session","percent":57,"severity":"normal"},{"kind":"weekly_all","percent":7.5,"severity":"warning"},
                      {"kind":"weekly_scoped","percent":31,"severity":"critical","resets_at":"2026-10-02T04:00:00+00:00","scope":{"model":{"display_name":"Fable"}}}]}}})");
        auto a = ParseUsageCache(cfg);
        Expect(a.valid && a.fetchedAtMs == 1000, "valid, fetchedAtMs");
        Expect(a.fiveHour && a.fiveHour->usedPct == 57 && a.fiveHour->resetsAtMs == 1790349600669, "5-hour window");
        Expect(a.sevenDay && a.sevenDay->usedPct == 7.5 && a.sevenDay->LeftPct() == 92.5, "weekly window, fractional");
        Expect(a.modelWeekly.size() == 1 && a.modelWeekly[0].label == "Fable" && a.modelWeekly[0].usedPct == 31, "per-model weekly from limits[]");
        Expect(a.extra.enabled && a.extra.used && a.extra.used->minor == 5375, "spend.used wins over extra_usage");
        Expect(!a.extra.limit && !a.extra.balance, "no cap, no balance");
        Expect(a.fiveHour->severity == 0 && a.sevenDay->severity == 1 && a.modelWeekly[0].severity == 2, "server severity per window");
        Expect(a.products.size() == 2 && a.products[0].key == "claude_code" && a.products[0].percent == 89 && a.products[1].name == "Chats",
               "product shares from seven_day_breakdown (rows without a percent skipped)");
    }
    std::puts("tiers");
    {
        Window w;
        w.usedPct = 40;
        Expect(w.Tier() == 0, "40% → normal");
        w.usedPct = 76;
        Expect(w.Tier() == 1, "76% → warning");
        w.usedPct = 94;
        Expect(w.Tier() == 2, "94% → critical");
        w.severity = 0;
        Expect(w.Tier() == 0, "the server's verdict wins over the thresholds");
    }
    std::puts("cache: older shape, missing pieces");
    {
        auto cfg = config::Json::parse(R"({"cachedUsageUtilization":{"fetchedAtMs":1,"utilization":{
            "five_hour":{"utilization":10,"resets_at":null},
            "seven_day_opus":{"utilization":40,"resets_at":"2026-10-02T04:00:00+00:00"},
            "extra_usage":{"is_enabled":true,"monthly_limit":10000,"used_credits":2500,"currency":"EUR","decimal_places":2}}}})");
        auto a = ParseUsageCache(cfg);
        Expect(a.fiveHour && a.fiveHour->resetsAtMs == 0, "null resets_at → unknown");
        Expect(!a.sevenDay, "absent window stays empty");
        Expect(a.modelWeekly.size() == 1 && a.modelWeekly[0].label == "Opus", "fixed seven_day_opus key");
        Expect(a.extra.used && a.extra.used->Format() == "25.00 EUR" && a.extra.limit && a.extra.limit->Format() == "100.00 EUR",
               "extra_usage numbers are minor units of its currency");
        Expect(!ParseUsageCache(config::Json::object()).valid, "no cache → invalid");
        Expect(!ParseUsageCache(config::Json::parse(R"({"cachedUsageUtilization":"junk"})")).valid, "junk cache → invalid");
    }

    std::puts("status line");
    {
        auto s = ParseStatusLine(R"({"session_id":"abc","cost":{"total_cost_usd":1.2345,"total_duration_ms":5},
            "rate_limits":{"five_hour":{"used_percentage":61.5,"resets_at":1790349600},"seven_day":{"used_percentage":9,"resets_at":1790900000}}})");
        Expect(s && s->sessionId == "abc" && s->costUsd == 1.2345, "session id and cost");
        Expect(s && s->fiveHour && s->fiveHour->usedPct == 61.5 && s->fiveHour->resetsAtMs == 1790349600000, "unix-second resets_at → ms");
        Expect(s && s->sevenDay && s->sevenDay->usedPct == 9, "weekly");
        auto bare = ParseStatusLine(R"({"session_id":"x","cost":{"total_cost_usd":0}})");
        Expect(bare && !bare->fiveHour && !bare->sevenDay && bare->costUsd == 0, "no rate_limits yet (before the first reply)");
        Expect(!ParseStatusLine("{\"session_id\":\"cut off mid-wri"), "a half-written file is rejected");
        Expect(!ParseStatusLine("[]"), "not an object");
    }
    std::printf("\n%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--parse")) return SelfTest();

    if (argc > 1 && !std::strcmp(argv[1], "--refresh"))
    {
        auto& m = usage::Global();
        m.RequestRefresh();
        while (m.State() == usage::Monitor::RefreshState::Running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::printf("refresh: %s %s\n", m.State() == usage::Monitor::RefreshState::Idle ? "ok" : "FAILED", m.Error().c_str());
        argc = 1;
    }

    std::filesystem::path file = argc > 1 ? argv[1] : config::GlobalConfigFile();
    std::string err;
    auto cfg = config::ReadJsonObject(file, &err);
    if (!cfg)
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Print(usage::ParseUsageCache(*cfg));
    return 0;
}
