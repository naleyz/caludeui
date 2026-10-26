#pragma once

#include "config/app_settings.hpp"
#include "config/font_catalog.hpp"
#include "config/model_access.hpp"
#include "config/model_catalog.hpp"
#include "config/settings_store.hpp"
#include "net/egress.hpp"

#include <imgui.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui
{
// Editor for Claude Code's /config options, backed by SettingsStore.
class SettingsPanel
{
public:
    SettingsPanel(config::SettingsStore& store, config::AppSettingsStore& app, const config::ModelCatalog& models,
                  const config::ModelAccess& access)
        : store_(store), app_(app), models_(models), access_(access)
    {
    }
    void Draw();

    // Section names in display order ("ClaudeUI" first, then the /config groups),
    // for a navigation list next to the panel.
    static std::vector<std::string> Sections();
    void ScrollTo(std::string section);
    // What the navigation should highlight: the section just picked there, or
    // else the one whose header is at (or last scrolled past) the top of the list.
    const std::string& VisibleSection() const;
    // How many settings in a section differ from the default (for the navigation).
    int ChangedCount(const std::string& section) const;

private:
    // Before a section header: scroll it to the top if asked, track which one is visible.
    void SectionAnchor(const std::string& section);
    std::string scrollTarget_;
    std::string visibleSection_;
    std::string lastSection_;
    std::string pinned_;
    int         pinGrace_      = 0;
    float       pinnedScrollY_ = 0.0f;
    float       rowsTop_       = 0.0f;

    void DrawRow(const config::SettingDef& def);
    bool DrawControl(const config::SettingDef& def, const config::SettingsStore::Value& v);
    void Apply(const config::SettingDef& def, const nlohmann::json& value);
    void Reset(const config::SettingDef& def);

    void DrawAppSection(float width);
    void DrawTerminalFont();
    void DrawSessionProxy(bool divider);
    void SaveSessionProxy(const config::ProxySetting& value, const char* notice);
    std::optional<std::string> proxyUrlEdit_;   // being typed; the saved value otherwise
    std::optional<std::string> noProxyEdit_;
    net::EgressGuard::Future   proxyCheck_;     // "Проверить выход" in flight
    std::optional<net::EgressCheck> proxyCheckResult_;
    void SaveTerminalFont(const config::TerminalFont& value, const char* notice);

    config::SettingsStore& store_;
    config::AppSettingsStore& app_;
    const config::ModelCatalog& models_;
    const config::ModelAccess& access_;
    std::vector<config::MonoFontFamily> monoFonts_; // installed monospace families, scanned on first use
    bool fontsScanned_ = false;
    std::string fontPathEdit_; // path being typed for a font that isn't in the list
    // Size the slider is being dragged to. The slider hands its value out only on
    // the frames the mouse moves, not on the frame it is released, so the value to
    // save has to be remembered from the drag.
    std::optional<int> fontSizeDrag_;
    ImGuiTextFilter filter_;
    bool onlyChanged_ = false;
    std::string error_;
    std::string notice_;
    double noticeUntil_ = 0.0;
    std::string shownSignature_; // all displayed values, to tell real external edits from noise
    std::uint64_t seenRevision_ = 0;
    std::string ValuesSignature() const;
    std::unordered_map<std::string, std::string> textEdits_; // Text settings being typed into
};
} // namespace ui
