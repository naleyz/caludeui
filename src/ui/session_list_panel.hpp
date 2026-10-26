#pragma once

#include "sessions/session_info.hpp"

#include <imgui.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sessions
{
class LiveSessionRegistry;
}

namespace ui
{
// Every Claude Code session found under ~/.claude/projects, grouped by project
// and searchable, with live status: open in a ClaudeUI tab, or running in some
// other terminal (from Claude Code's own process registry).
class SessionListPanel
{
public:
    struct Callbacks
    {
        std::function<void(const sessions::SessionInfo&)> open;       // resume (or focus its tab)
        std::function<void(const std::string& cwd)>       newInDir;   // new session dialog, prefilled
        std::function<void(const std::string& cwd)>       continueIn; // claude --continue in that dir
        std::function<void()>                             rescan;
    };

    void SetCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    void Draw(const std::vector<sessions::SessionInfo>& sessions, const sessions::LiveSessionRegistry& live,
              const std::unordered_set<std::string>& openInTabs);

private:
    void RequestOpen(const sessions::SessionInfo& s, const sessions::LiveSessionRegistry& live,
                     const std::unordered_set<std::string>& openInTabs);
    void DrawOpenConfirm();
    void DrawRenameDialog();
    void DrawDeleteConfirm();

    Callbacks       cb_;
    ImGuiTextFilter filter_;
    bool            onlyLive_ = false;
    std::string     selectedId_;

    // Resume confirmation for a session that is already running elsewhere.
    bool                  confirmOpen_ = false;
    sessions::SessionInfo confirmSession_;
    std::string           confirmDetail_;

    // Rename (writes a real custom-title record, see sessions::RenameSessionFile).
    bool                  renameOpen_ = false;
    sessions::SessionInfo renameTarget_;
    std::string           renameBuffer_;
    std::string           renameError_;

    // Delete confirmation (sessions::DeleteSessionFile).
    bool                  deleteOpen_ = false;
    sessions::SessionInfo deleteTarget_;
    std::string           deleteError_;
};
} // namespace ui
