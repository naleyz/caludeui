#pragma once

#include <string>
#include <vector>

// Set by a parent Claude Code session (e.g. when ClaudeUI is started from a
// terminal inside one). Inherited, they make the child think it's a nested
// sub-session: transcript saving is disabled and messaging goes to the parent.
// Drop them from the environment of every `claude` ClaudeUI starts.
inline const std::vector<std::string> kInheritedSessionEnv = {
    "CLAUDECODE",
    "CLAUDE_CODE_ENTRYPOINT",
    "CLAUDE_CODE_SESSION_ID",
    "CLAUDE_CODE_CHILD_SESSION",
    "CLAUDE_CODE_SESSION_ATTENDED",
    "CLAUDE_CODE_MESSAGING_SOCKET",
    "CLAUDE_CODE_MESSAGING_TOKEN",
    "CLAUDE_CODE_BRIDGE_SESSION_ID",
    "CLAUDE_CODE_EXECPATH",
    "CLAUDE_PID",
    "CLAUDE_EFFORT",
};
