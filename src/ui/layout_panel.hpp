#pragma once

#include "config/app_settings.hpp"

#include <string>

namespace ui
{
// The "Панели" section: lets the user show/hide and reorder the session
// sidebar's stacked panels (session list, limits card). Reordering is
// drag-and-drop; the limits card sizes itself, the list takes the rest.
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
