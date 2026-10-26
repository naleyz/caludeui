#pragma once

#include "config/app_settings.hpp"

#include <string>

namespace ui
{
// The "Панели" tab: lets the user show/hide and reorder the sidebar's
// stacked panels (session list, limits/usage). Reordering is drag-and-drop;
// resizing the proportion between panels happens directly in the sidebar
// (ImGuiChildFlags_ResizeY on each panel's wrapper, see main.cpp), which
// this tab just reminds the user about.
class LayoutPanel
{
public:
    explicit LayoutPanel(config::AppSettingsStore& app) : app_(app) {}
    void Draw();

private:
    config::AppSettingsStore& app_;
    std::string                error_;
};
} // namespace ui
