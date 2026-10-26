#pragma once

#include <imgui.h>

#include <optional>
#include <string>

namespace ui
{
// What the sidebar (and, for settings, the main area) shows.
enum class Section
{
    Sessions,
    Stats,
    Panels,
    Settings,
};

// The narrow icon column on the far left: sections at the top, the two plan
// limits as mini bars and the settings gear at the bottom. Icons are drawn
// as vector strokes, so they look the same whichever fonts are installed.
class Rail
{
public:
    struct Limit
    {
        double      usedPct = 0.0; // 0..100
        int         tier    = 0;   // 0 room left, 1 getting close, 2 nearly gone
        std::string tooltip;       // "5 часов: 42% · сброс в 18:20"
    };
    struct Info
    {
        int                  runningTabs = 0; // badge on "Сессии" while another section is open
        std::optional<Limit> fiveHour;
        std::optional<Limit> week;
    };

    // Fills the current window (a fixed-width child). Changes `current` on a click.
    void Draw(Section& current, const Info& info, float scale);
};

// Width of the rail in unscaled pixels.
inline constexpr float kRailWidth = 60.0f;
} // namespace ui
