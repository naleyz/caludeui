#pragma once

#include "ui/terminal_panel.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sessions
{
class LiveSessionRegistry;
}

namespace ui
{
// Tab bar of embedded Claude Code sessions.
class SessionTabs
{
public:
    // Opens (and launches) a new tab. When resuming a session that already
    // has a tab, that tab is focused instead and returned.
    TerminalPanel* Open(LaunchSpec spec, std::string title);
    TerminalPanel* FindBySession(const std::string& sessionId);
    void Focus(const TerminalPanel* panel);

    void Tick(const sessions::LiveSessionRegistry& live);
    void Draw();
    void HandleShortcuts();

    int  Count() const { return static_cast<int>(tabs_.size()); }
    int  RunningCount() const;
    bool AnyTerminalFocused() const;
    const std::vector<std::unique_ptr<TerminalPanel>>& Tabs() const { return tabs_; }

    void ExitAll();      // /exit to every running session
    void TerminateAll(); // SIGTERM everything, then reap

    std::function<void()> onNewSession;
    std::function<void()> onSessionExited; // lets the session list rescan

private:
    void CloseTab(std::size_t index);
    void DrawCloseConfirm();
    void DrawEmptyState();

    std::vector<std::unique_ptr<TerminalPanel>> tabs_;
    std::uint64_t selectUid_ = 0;       // select this tab on the next frame
    std::uint64_t activeUid_ = 0;
    std::uint64_t confirmCloseUid_ = 0; // running tab the user tried to close
    std::vector<std::uint64_t> closeWhenExited_;
    std::vector<std::pair<std::uint64_t, bool>> wasRunning_;

    // Empty-state mascot: walks in once per visit, then waves periodically.
    std::chrono::steady_clock::time_point mascotShownAt_{};
    bool mascotActive_ = false;
    std::chrono::steady_clock::time_point mascotPettedAt_{};
    bool mascotEverPetted_ = false;
};
} // namespace ui
