#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sessions
{
// One ~/.claude/projects/<project>/<uuid>.jsonl file.
struct SessionInfo
{
    std::string id;             // uuid, from the file's stem
    std::filesystem::path projectDir; // the encoded ~/.claude/projects/<...> dir
    std::filesystem::path filePath;   // full path to the .jsonl

    std::string cwd;            // real project working directory, from the transcript
    std::string title;          // custom-title (set via /rename) or ai-title, last one wins
    std::string firstUserPreview; // first user message, truncated/flattened
    std::string lastPrompt;     // most recent prompt, truncated/flattened

    std::string firstTimestamp; // ISO 8601, from the earliest parsed line
    std::string lastTimestamp;  // ISO 8601, from the latest parsed line

    std::uintmax_t fileSizeBytes = 0;
    std::filesystem::file_time_type mtime{};

    // Best human-readable label: title, else first prompt, else empty.
    const std::string& DisplayName() const { return !title.empty() ? title : firstUserPreview; }
};
} // namespace sessions
