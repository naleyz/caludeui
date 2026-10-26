#pragma once

#include "usage/usage.hpp"

#include <imgui.h>

#include <optional>
#include <string>
#include <vector>

namespace ui
{
// Bar colours for a limit's state (usage::Window::Tier): blue, amber, red —
// claude.ai's dark-theme tokens. Text: 0 means "use the plain secondary colour".
ImU32 LimitTierColor(int tier);
ImU32 LimitTierTrack(int tier);
ImU32 LimitTierText(int tier);
// "в 18:20" today, "пт, 07:00" this week, "2 окт, 07:00" further out.
std::string LimitClock(std::int64_t ms);
// "1 ч 35 мин", "2 д 5 ч", "12 мин"
std::string LimitDuration(std::int64_t ms);
// "только что", "4 мин назад"
std::string LimitAgo(std::int64_t ms);
// The server's product name, in Russian where we have one.
std::string ProductLabel(const usage::ProductShare& p);
// "5 часов: 42% · сброс в 18:20" — one line about a window, for tooltips.
std::string LimitSummary(const char* name, const usage::Window& w);

// The limits card under the session list: the 5-hour and weekly bars and what
// the open sessions cost. The full picture is ui::StatsView; see usage::Monitor
// for where the numbers come from.
class UsagePanel
{
public:
    struct SessionRow
    {
        std::string          title;
        bool                 running = false;
        std::optional<double> costUsd; // nullopt: nothing reported yet
    };

    // A small card for the session sidebar: the 5-hour and weekly bars and what
    // the open sessions cost. Returns true when it was clicked (→ statistics).
    bool DrawCompact(const usage::AccountUsage& account, const std::vector<SessionRow>& sessions);
};
} // namespace ui
