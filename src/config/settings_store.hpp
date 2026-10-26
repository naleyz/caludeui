#pragma once

#include "config/json_file.hpp"
#include "config/settings_schema.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace config
{
// Reads and writes Claude Code's user-level settings files, as described by
// AllSettings(). Keeps an in-memory copy for drawing and picks up changes
// made by Claude Code itself (or a text editor) via mtime polling.
class SettingsStore
{
public:
    enum class Source
    {
        Default,       // key absent everywhere
        UserSettings,  // ~/.claude/settings.json
        GlobalConfig,  // ~/.claude.json
        LegacyGlobal,  // ~/.claude.json, pre-migration location (settings.json wins if set)
        ForcedOff,     // e.g. disableWorkflows: true
    };

    struct Value
    {
        nlohmann::json value; // null if unset and the default isn't known offline
        Source source = Source::Default;
        bool   invalid = false; // key present but of the wrong type
    };

    SettingsStore();

    const std::filesystem::path& PathOf(Store s) const
    {
        return s == Store::UserSettings ? userSettingsPath_ : globalConfigPath_;
    }

    // Last successfully loaded contents of a file.
    const Json& Data(Store s) const { return s == Store::UserSettings ? user_ : global_; }
    // Bumps on every (re)load, whoever triggered it.
    std::uint64_t Revision() const { return revision_; }

    void Reload();
    // Reloads files whose mtime/size changed; returns true if anything was reloaded.
    bool PollExternalChanges();

    Value Get(const SettingDef& def) const;
    bool  Set(const SettingDef& def, const nlohmann::json& value, std::string* error);
    bool  Reset(const SettingDef& def, std::string* error);

    // Non-empty if a file couldn't be read/parsed (editing it is then refused).
    const std::string& LoadError(Store s) const
    {
        return s == Store::UserSettings ? userLoadError_ : globalLoadError_;
    }

private:
    struct FileStamp
    {
        std::filesystem::file_time_type mtime{};
        std::uintmax_t size = 0;
        bool exists = false;
        bool operator==(const FileStamp&) const = default;
    };

    static FileStamp StampOf(const std::filesystem::path& p);
    void LoadOne(Store s);

    std::filesystem::path userSettingsPath_;
    std::filesystem::path globalConfigPath_;
    Json user_   = Json::object();
    Json global_ = Json::object();
    std::string userLoadError_, globalLoadError_;
    FileStamp userStamp_, globalStamp_;
    std::chrono::steady_clock::time_point lastPoll_{};
    std::uint64_t revision_ = 0;
};
} // namespace config
