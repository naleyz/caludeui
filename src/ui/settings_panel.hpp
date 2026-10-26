#pragma once

#include "config/app_settings.hpp"
#include "config/font_catalog.hpp"
#include "config/model_access.hpp"
#include "config/model_catalog.hpp"
#include "config/settings_store.hpp"

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

private:
    void DrawRow(const config::SettingDef& def);
    bool DrawControl(const config::SettingDef& def, const config::SettingsStore::Value& v);
    void Apply(const config::SettingDef& def, const nlohmann::json& value);
    void Reset(const config::SettingDef& def);

    void DrawAppSection();
    void DrawTerminalFont();
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
