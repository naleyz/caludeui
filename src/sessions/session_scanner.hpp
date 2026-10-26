#pragma once

#include "sessions/session_info.hpp"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace sessions
{
// Discovers Claude Code sessions under ~/.claude/projects/**/*.jsonl.
//
// Only bounded head/tail chunks of each file are read (not the whole
// file) so that scanning stays fast even when some session transcripts
// are tens of megabytes. Rescan() re-parses only files whose size or
// mtime changed since the previous pass, so it is cheap to call often.
class SessionScanner
{
public:
    static std::vector<SessionInfo> ScanAll();
    // True if <id>.jsonl exists in any project dir — i.e. `claude --resume <id>`
    // has something to resume (a session closed before its first message doesn't).
    static bool TranscriptExists(const std::string& sessionId);

    // Returns true if anything was added, changed or removed.
    bool Rescan();
    const std::vector<SessionInfo>& Sessions() const { return sessions_; }

private:
    std::unordered_map<std::string, SessionInfo> cache_; // keyed by file path
    std::vector<SessionInfo> sessions_;                  // newest activity first
};

// Appends a `custom-title` record to the transcript — the same line Claude
// Code itself writes when you run `/rename`, so the new name is picked up by
// both this scanner and Claude Code's own history. False (with `error` set)
// if the file couldn't be opened/written.
bool RenameSessionFile(const std::filesystem::path& jsonlPath, const std::string& sessionId,
                       const std::string& newTitle, std::string* error);

// Deletes the session's transcript file. False (with `error` set) if it
// couldn't be removed.
bool DeleteSessionFile(const std::filesystem::path& jsonlPath, std::string* error);
} // namespace sessions
