#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace config
{
struct ModelInfo
{
    std::string id;          // "claude-opus-4-8" — accepted by `claude --model` and settings.model
    std::string family;      // "opus", "sonnet", "haiku", "fable"
    std::string displayName; // "Opus 4.8"
    int  major = 0, minor = 0;
    bool native1m = false; // 1M context window out of the box
    bool suffix1m = false; // 1M available via the "[1m]" suffix
    bool legacy = false;   // pre-Claude-4 generation; hidden unless the user opts in
};

// Models Claude Code itself knows about. Newest first within each family.
struct ModelCatalog
{
    std::vector<ModelInfo> models;
    std::string source; // where the list came from, for display

    const ModelInfo* Find(std::string_view id) const;
    std::vector<const ModelInfo*> Family(std::string_view family, bool includeLegacy) const;
};

// Families shown in pickers, in display order.
const std::vector<std::string>& ModelFamilies();
// Whether `claude --model <family>` is a valid "latest version" alias.
bool FamilyHasAlias(std::string_view family);

// Snapshot taken from Claude Code 2.1.272; used until (or if) the scan fails.
ModelCatalog BuiltinModelCatalog();

// Reads the model registry embedded in the installed `claude` (the native
// binary or cli.js both carry it as `{id:"claude-…",family:"…",display_name:"…",…}`
// literals). Takes a fraction of a second on a ~230 MB binary — call it off
// the UI thread. Falls back to BuiltinModelCatalog().
ModelCatalog LoadModelCatalog();
} // namespace config
