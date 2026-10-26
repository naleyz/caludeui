#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h>

namespace sessions
{
// A running Claude Code process, as announced in ~/.claude/sessions/<pid>.json.
struct LiveSession
{
    pid_t       pid = -1;
    std::string sessionId;
    std::string cwd;
    std::string name;    // derived or /rename'd session name
    std::string status;     // "busy", "waiting" or "idle", as written by Claude Code
    std::string waitingFor; // with "waiting": "permission prompt", "input needed", "dialog open"
    std::string kind;    // "interactive", ...
    std::string version;
    std::int64_t startedAtMs = 0;
    std::int64_t updatedAtMs = 0;
};

// Reads Claude Code's own process registry. Entries whose pid is gone (or
// was reused by another process — checked against /proc/<pid>/stat start
// time) are ignored, so crashed sessions don't show up as live.
class LiveSessionRegistry
{
public:
    // Returns true if the set of live sessions (or any status) changed.
    bool Refresh();

    const std::vector<LiveSession>& All() const { return live_; }
    const LiveSession* FindBySession(const std::string& sessionId) const;
    const LiveSession* FindByPid(pid_t pid) const;

private:
    std::vector<LiveSession> live_;
};
} // namespace sessions
