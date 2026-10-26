#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace config
{
using Json = nlohmann::ordered_json; // keeps the file's key order on rewrite

// Reads a JSON object. A missing file yields an empty object; anything that
// isn't a parseable object yields nullopt with *error filled in.
std::optional<Json> ReadJsonObject(const std::filesystem::path& path, std::string* error);

// Safe read-modify-write of a JSON file that Claude Code also writes:
//   1. takes `<path>.lock` — the same mkdir-based lock Claude Code's
//      proper-lockfile uses for ~/.claude.json, so the two never interleave;
//   2. re-reads the file from disk under the lock (never writes a stale copy);
//   3. lets `mutate` edit it (returning false aborts without writing);
//   4. backs the original up once per run into ~/.claude/claudeui-backups
//      (skip with backup=false for ClaudeUI's own files);
//   5. writes a temp file with the original permissions, fsyncs, renames.
// Formatting matches JSON.stringify(v, null, 2), trailing newline preserved.
bool UpdateJsonFile(const std::filesystem::path& path, const std::function<bool(Json&)>& mutate,
                    std::string* error, bool backup = true);

// Serialises like JSON.stringify(v, null, 2).
std::string Dump(const Json& j);
} // namespace config
