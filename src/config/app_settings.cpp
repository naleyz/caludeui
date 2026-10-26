#include "config/app_settings.hpp"
#include "config/json_file.hpp"
#include "config/paths.hpp"

#include <algorithm>
#include <cstdlib>

namespace config
{
namespace
{
constexpr const char* kShowLegacyModels = "showLegacyModels";
constexpr const char* kUsageAutoRefresh = "usageAutoRefresh";
constexpr const char* kTerminalFont     = "terminalFont";
constexpr const char* kSidebarPanels    = "sidebarPanels";

// Drops ids the file has but this version doesn't know, and appends any
// known id the file is missing (visible) — see PanelConfig's doc comment.
std::vector<PanelConfig> ReconcilePanels(std::vector<PanelConfig> loaded)
{
    const std::vector<PanelConfig> known = AppSettings{}.sidebarPanels;
    auto isKnown = [&](const PanelConfig& p) {
        return std::any_of(known.begin(), known.end(), [&](const PanelConfig& k) { return k.id == p.id; });
    };
    std::erase_if(loaded, [&](const PanelConfig& p) { return !isKnown(p); });
    for (const auto& k : known)
        if (std::none_of(loaded.begin(), loaded.end(), [&](const PanelConfig& p) { return p.id == k.id; }))
            loaded.push_back({k.id, true});
    return loaded;
}

std::filesystem::path AppSettingsPath()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "claudeui" / "settings.json";
    return HomeDir() / ".config" / "claudeui" / "settings.json";
}
} // namespace

AppSettingsStore::AppSettingsStore() : path_(AppSettingsPath())
{
    if (auto j = ReadJsonObject(path_, &loadError_))
    {
        if (auto it = j->find(kShowLegacyModels); it != j->end() && it->is_boolean())
            settings_.showLegacyModels = it->get<bool>();
        if (auto it = j->find(kUsageAutoRefresh); it != j->end() && it->is_boolean())
            settings_.usageAutoRefresh = it->get<bool>();
        if (auto it = j->find(kTerminalFont); it != j->end() && it->is_object())
        {
            TerminalFont& f = settings_.terminalFont;
            auto str = [&](const char* key, std::string& out) {
                if (auto v = it->find(key); v != it->end() && v->is_string()) out = v->get<std::string>();
            };
            str("family", f.family);
            str("regular", f.regularPath);
            str("bold", f.boldPath);
            if (auto v = it->find("size"); v != it->end() && v->is_number())
                f.size = std::clamp(v->get<float>(), TerminalFont::kMinSize, TerminalFont::kMaxSize);
        }
        if (auto it = j->find(kSidebarPanels); it != j->end() && it->is_array())
        {
            std::vector<PanelConfig> loaded;
            for (const auto& entry : *it)
            {
                if (!entry.is_object()) continue;
                auto id = entry.find("id");
                if (id == entry.end() || !id->is_string()) continue;
                loaded.push_back({id->get<std::string>(), entry.value("visible", true)});
            }
            settings_.sidebarPanels = ReconcilePanels(std::move(loaded));
        }
    }
}

bool AppSettingsStore::SetBool(const char* key, bool AppSettings::*member, bool value, std::string* error)
{
    error->clear();
    const bool ok = UpdateJsonFile(
        path_,
        [&](Json& root) {
            root[key] = value;
            return true;
        },
        error, /*backup=*/false);
    if (ok)
    {
        settings_.*member = value;
        loadError_.clear();
        ++revision_;
    }
    return ok;
}

bool AppSettingsStore::SetShowLegacyModels(bool value, std::string* error)
{
    return SetBool(kShowLegacyModels, &AppSettings::showLegacyModels, value, error);
}

bool AppSettingsStore::SetUsageAutoRefresh(bool value, std::string* error)
{
    return SetBool(kUsageAutoRefresh, &AppSettings::usageAutoRefresh, value, error);
}

bool AppSettingsStore::SetTerminalFont(const TerminalFont& value, std::string* error)
{
    error->clear();
    TerminalFont v = value;
    v.size         = std::clamp(v.size, TerminalFont::kMinSize, TerminalFont::kMaxSize);
    const bool ok = UpdateJsonFile(
        path_,
        [&](Json& root) {
            if (v == TerminalFont{}) root.erase(kTerminalFont); // all defaults: leave no trace in the file
            else
                root[kTerminalFont] = Json{{"family", v.family}, {"regular", v.regularPath}, {"bold", v.boldPath}, {"size", v.size}};
            return true;
        },
        error, /*backup=*/false);
    if (ok)
    {
        settings_.terminalFont = v;
        loadError_.clear();
        ++revision_;
    }
    return ok;
}

bool AppSettingsStore::SetSidebarPanels(const std::vector<PanelConfig>& value, std::string* error)
{
    error->clear();
    std::vector<PanelConfig> v = ReconcilePanels(value);
    const bool ok = UpdateJsonFile(
        path_,
        [&](Json& root) {
            if (v == AppSettings{}.sidebarPanels) root.erase(kSidebarPanels); // default order/visibility: leave no trace
            else
            {
                Json arr = Json::array();
                for (const auto& p : v) arr.push_back(Json{{"id", p.id}, {"visible", p.visible}});
                root[kSidebarPanels] = std::move(arr);
            }
            return true;
        },
        error, /*backup=*/false);
    if (ok)
    {
        settings_.sidebarPanels = std::move(v);
        loadError_.clear();
        ++revision_;
    }
    return ok;
}
} // namespace config
