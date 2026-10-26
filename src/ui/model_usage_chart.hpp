#pragma once

#include "config/model_catalog.hpp"
#include "usage/model_usage.hpp"
#include "usage/usage.hpp"

namespace ui
{
// "Модели": which models used how many tokens since the current limit window
// began (the weekly or the 5-hour one), as stacked bars over time plus a
// legend with each model's share. Numbers come from usage::TranscriptUsage.
class ModelUsageChart
{
public:
    void Draw(const usage::TranscriptUsage& data, const usage::AccountUsage& account, const config::ModelCatalog& catalog);

private:
    int window_ = 0; // 0: week, 1: 5 hours
    int metric_ = 0; // 0: without cache reads, 1: output only, 2: everything
};
} // namespace ui
