#pragma once

#include <cstdlib>
#include <filesystem>

namespace config
{
inline std::filesystem::path HomeDir()
{
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "");
}

// ~/.claude, or $CLAUDE_CONFIG_DIR when Claude Code is pointed elsewhere.
inline std::filesystem::path ClaudeDir()
{
    if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir && *dir) return std::filesystem::path(dir);
    return HomeDir() / ".claude";
}

// ~/.claude.json lives next to ~/.claude unless CLAUDE_CONFIG_DIR is set.
inline std::filesystem::path GlobalConfigFile()
{
    if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir && *dir)
        return std::filesystem::path(dir) / ".claude.json";
    return HomeDir() / ".claude.json";
}
} // namespace config
