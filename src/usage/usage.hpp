#pragma once

#include "config/json_file.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Plan limits and spend, taken from Claude Code itself — ClaudeUI never reads
// the OAuth credentials or talks to the usage endpoint on its own. Two sources:
//
//  * ~/.claude.json `cachedUsageUtilization` — the last /api/oauth/usage answer
//    any Claude Code process fetched: 5-hour and weekly windows, extra-usage
//    spend. Refreshed on demand by running `claude -p /usage` (a local command,
//    no model call, no tokens).
//  * a status-line sidecar: sessions started by ClaudeUI are launched with a
//    `statusLine` that copies the JSON Claude Code feeds it (session cost, live
//    rate-limit windows) into a file per tab, which we read.
namespace usage
{
using Json = config::Json;

// One rate-limit window (5 hours, a week, a week of one model).
struct Window
{
    std::string  label;          // "" for the main windows; "Opus" for a per-model weekly one
    double       usedPct = 0.0;  // 0..100
    std::int64_t resetsAtMs = 0; // unix ms; 0 = unknown
    int          severity = -1;  // the server's own verdict: 0 normal, 1 warning, 2 critical; -1 = not given

    double LeftPct() const { return usedPct >= 100.0 ? 0.0 : 100.0 - usedPct; }
    // The window has rolled over since this was measured: the real figure is 0.
    bool Expired(std::int64_t nowMs) const { return resetsAtMs > 0 && resetsAtMs <= nowMs; }
    // 0 normal, 1 warning, 2 critical — the server's word for it, else by how full it is.
    int Tier() const { return severity >= 0 ? severity : usedPct >= 90.0 ? 2 : usedPct >= 75.0 ? 1 : 0; }
};

// One product's share of this week's usage (Claude Code, chats, …).
struct ProductShare
{
    std::string key;  // "claude_code", "chat", "cowork", "other"
    std::string name; // as the server labels it
    double      percent = 0.0;
};

struct Money
{
    std::int64_t minor = 0; // 5375 with exponent 2 = 53.75
    std::string  currency = "USD";
    int          exponent = 2;

    double      Amount() const;
    std::string Format() const; // "$53.75", "53.75 EUR"
};

// "Extra usage": what is billed once the plan's own limits run out.
struct ExtraUsage
{
    bool                enabled = false;
    std::optional<Money> used;    // this billing month
    std::optional<Money> limit;   // monthly spend cap, when one is set
    std::optional<Money> balance; // prepaid credit balance, when there is one
};

struct AccountUsage
{
    bool         valid = false;
    std::int64_t fetchedAtMs = 0; // when Claude Code last asked the server (0 for live-only data)
    std::optional<Window> fiveHour;
    std::optional<Window> sevenDay;
    std::vector<Window>   modelWeekly;
    std::vector<ProductShare> products; // what this week's usage came from
    ExtraUsage            extra;
    // The 5-hour/weekly figures above come from a running session that reported
    // after the cached server answer; this is when.
    std::int64_t liveWindowsAtMs = 0;
};

// Reads `cachedUsageUtilization` out of the parsed ~/.claude.json.
AccountUsage ParseUsageCache(const Json& globalConfig);

// What Claude Code writes on the status line's stdin, reduced to what we show.
struct SessionUsage
{
    std::string           sessionId;
    double                costUsd = 0.0; // API-list-price cost Claude Code counted for this session
    std::string           model;         // "Opus 5.5" — the model the session runs on right now
    std::string           modelId;       // "claude-opus-5-5"
    std::string           effort;        // "medium", "xhigh", … ("" if not reported)
    std::optional<Window> fiveHour;
    std::optional<Window> sevenDay;
    std::int64_t          updatedAtMs = 0; // when the file was written
};
std::optional<SessionUsage> ParseStatusLine(std::string_view json);

// "2026-09-25T15:20:00.668946+00:00" → unix ms; 0 if it doesn't parse.
std::int64_t ParseIso8601Ms(std::string_view iso);

class Monitor
{
public:
    enum class RefreshState { Idle, Running, Failed };

    // Extra `claude` arguments and environment that make a session report its
    // usage into a sidecar file.
    struct LaunchExtras
    {
        std::vector<std::string>                         args;
        std::vector<std::pair<std::string, std::string>> env;
    };

    Monitor();
    ~Monitor();
    Monitor(const Monitor&)            = delete;
    Monitor& operator=(const Monitor&) = delete;

    // ── sessions ────────────────────────────────────────────────────────
    // Nothing (no args, no env) when the user or the project already defines a
    // status line of their own: ours would replace it.
    LaunchExtras ForLaunch(std::uint64_t tabUid, const std::filesystem::path& cwd);
    // Re-reads the sidecars of these tabs whose files changed.
    void PollSessions(const std::vector<std::uint64_t>& tabUids);
    const SessionUsage* Session(std::uint64_t tabUid) const;

    // ── account ─────────────────────────────────────────────────────────
    void SetGlobalConfig(const Json& globalConfig);
    // The cached server answer, with the 5-hour/weekly windows replaced by what
    // a running session reported since, if that is newer.
    AccountUsage Account() const;

    // Runs `claude -p /usage` in the background; the answer lands in
    // ~/.claude.json, which SetGlobalConfig then picks up.
    void RequestRefresh();
    // Refreshes when the data is old. `allowed`: the user has auto-refresh on.
    void AutoRefresh(bool allowed, bool windowFocused);

    RefreshState State() const;
    std::string  Error() const;
    std::int64_t LastOkMs() const;

    // Stops a refresh in flight and removes this process's sidecar files.
    // Call before the process exits; the destructor does it too.
    void Shutdown();

private:
    struct Sidecar
    {
        std::int64_t mtimeNs = 0; // of the file, to tell when it was rewritten
        SessionUsage data;
        bool         has = false;
    };

    std::filesystem::path SidecarPath(std::uint64_t tabUid) const;
    void RunRefresh();
    void Finish(bool ok, std::string error);

    std::filesystem::path dir_;
    std::filesystem::path probeDir_;
    long                  pid_ = 0;

    AccountUsage                              account_;
    std::unordered_map<std::uint64_t, Sidecar> sidecars_;

    mutable std::mutex mu_;
    std::thread        worker_;
    RefreshState       state_ = RefreshState::Idle;
    std::string        error_;
    std::int64_t       lastAttemptMs_ = 0;
    std::int64_t       lastOkMs_      = 0;
    std::atomic<bool>  stop_{false};
};

// The one instance the terminals and the panel share.
Monitor& Global();

// Milliseconds since the Unix epoch.
std::int64_t NowMs();
} // namespace usage
