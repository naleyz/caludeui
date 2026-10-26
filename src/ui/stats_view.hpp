#pragma once

#include "config/model_catalog.hpp"
#include "ui/model_usage_chart.hpp"
#include "ui/usage_panel.hpp"
#include "usage/model_usage.hpp"
#include "usage/usage.hpp"

#include <vector>

namespace ui
{
// The statistics screen (rail → «Статистика»), a dashboard across the whole
// main area: cards for the 5-hour and weekly limits and extra usage, tokens
// per model since the window began, what the open sessions cost, and the
// week's split by product.
class StatsView
{
public:
    void Draw(const usage::AccountUsage& account, const std::vector<UsagePanel::SessionRow>& sessions, usage::Monitor& monitor,
              const usage::TranscriptUsage& transcripts, const config::ModelCatalog& catalog);

private:
    ModelUsageChart chart_;
};
} // namespace ui
