#pragma once

#include "usage/usage.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ui
{
// "Лимиты и расход": how much of the 5-hour and weekly plan limits is left,
// this month's extra-usage spend, and what each open session has cost.
// Drawn under the session list; see usage::Monitor for where the numbers come from.
class UsagePanel
{
public:
    struct SessionRow
    {
        std::string          title;
        bool                 running = false;
        std::optional<double> costUsd; // nullopt: nothing reported yet
    };

    // Height the panel took last frame, so the list above can leave room for it.
    float Height() const { return height_; }

    void Draw(const usage::AccountUsage& account, const std::vector<SessionRow>& sessions, usage::Monitor& monitor);

private:
    float height_ = 0.0f;
};
} // namespace ui
