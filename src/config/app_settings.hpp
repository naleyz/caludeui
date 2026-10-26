#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace config
{
// One entry in the sidebar's panel stack ("sessions", "usage", …), in
// display order. Unknown ids (from an older/newer version's file) are
// dropped on load; ids missing from a loaded file are appended as visible,
// so a panel introduced later isn't silently hidden. See ui::LayoutPanel
// for the id → human label mapping.
struct PanelConfig
{
    std::string id;
    bool        visible = true;
    bool operator==(const PanelConfig&) const = default;
};
// The face and size of the embedded terminal's text.
struct TerminalFont
{
    static constexpr float kMinSize     = 8.0f;
    static constexpr float kMaxSize     = 32.0f;
    static constexpr float kDefaultSize = 16.0f; // what the terminal has always been drawn at

    std::string family;       // as shown in the picker; "" with no path = the built-in JetBrains Mono
    std::string regularPath;  // .ttf/.otf; "" = built-in default
    std::string boldPath;     // "" = no separate bold face: bold text is drawn in the regular one
    float       size = kDefaultSize; // logical px, DPI scale applied on top

    bool IsDefaultFace() const { return regularPath.empty(); }
    bool operator==(const TerminalFont&) const = default;
};

// ClaudeUI's own preferences — never written into Claude Code's files.
struct AppSettings
{
    bool         showLegacyModels = false; // offer the Claude 3.x generation in model pickers
    bool         usageAutoRefresh = true;  // keep the limits panel current by running `claude -p /usage`
    TerminalFont terminalFont;
    // Display order + visibility of the sidebar's stacked panels.
    std::vector<PanelConfig> sidebarPanels = {{"sessions", true}, {"usage", true}};
};

// Persists AppSettings in $XDG_CONFIG_HOME/claudeui/settings.json
// (~/.config/claudeui/settings.json). Unknown keys in the file are preserved.
class AppSettingsStore
{
public:
    AppSettingsStore();

    const AppSettings& Get() const { return settings_; }
    const std::filesystem::path& Path() const { return path_; }
    const std::string& LoadError() const { return loadError_; }

    bool SetShowLegacyModels(bool value, std::string* error);
    bool SetUsageAutoRefresh(bool value, std::string* error);
    bool SetTerminalFont(const TerminalFont& value, std::string* error);
    bool SetSidebarPanels(const std::vector<PanelConfig>& value, std::string* error);

    // Bumped on every successful change, so the UI can tell when to re-apply.
    std::uint64_t Revision() const { return revision_; }

private:
    bool SetBool(const char* key, bool AppSettings::*member, bool value, std::string* error);

    std::filesystem::path path_;
    AppSettings settings_;
    std::string loadError_;
    std::uint64_t revision_ = 0;
};
} // namespace config
