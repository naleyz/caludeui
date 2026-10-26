#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace config
{
enum class Store
{
    UserSettings, // ~/.claude/settings.json
    GlobalConfig, // ~/.claude.json
};

enum class Kind { Bool, Enum, Text, Model };

enum class WriteMode
{
    Explicit,    // always store the chosen value
    OmitDefault, // choosing the default removes the key (what Claude Code itself does)
};

struct Option
{
    std::string value;
    std::string label; // empty → show value
};

using KeyPath = std::vector<std::string>;

// One entry of Claude Code's /config menu, mapped to where it's persisted.
// The mapping was taken from the Claude Code 2.1.x config panel source, not
// guessed: each item's onChange handler names the file and key it writes.
struct SettingDef
{
    std::string id;
    std::string label;       // exactly as /config shows it
    std::string description; // what it does, in Russian
    std::string group;
    Store       store = Store::UserSettings;
    KeyPath     path;
    Kind        kind = Kind::Bool;
    nlohmann::json defaultValue; // null → depends on account/plan, not known offline
    std::string defaultNote;     // shown when defaultValue is null
    std::vector<Option> options; // Enum choices / Text suggestions
    WriteMode   writeMode = WriteMode::Explicit;
    // Older Claude Code versions kept these in ~/.claude.json; the settings
    // file overrides, the global value is only a fallback when reading.
    bool        legacyGlobalFallback = false;
    // Keys removed whenever this setting is written or reset.
    std::vector<KeyPath> clearAlso;
    // If this key is `true`, the effective value is `false` regardless of path.
    KeyPath     forcedOffBy;
    // Where Claude Code's own /config writes it, if it differs from `store`.
    std::string scopeNote;
};

const std::vector<SettingDef>& AllSettings();
const std::vector<std::string>& SettingGroups(); // display order
} // namespace config
