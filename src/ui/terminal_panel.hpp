#pragma once

#include "net/egress.hpp"
#include "term/terminal.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sessions
{
class LiveSessionRegistry;
}

namespace ui
{
// Everything needed to (re)launch one `claude` process.
struct LaunchSpec
{
    std::filesystem::path cwd;
    std::string resumeId;             // --resume <id>
    bool        continueLast = false; // --continue
    std::string permissionMode;       // --permission-mode <mode>, empty = from settings
    std::string model;                // --model <name>, empty = from settings
    std::string initialPrompt;        // first message, passed as the positional argument

    std::vector<std::string> Args() const; // argv including "claude"
    std::string CommandLine() const;       // `cd … && claude …`, shell-quoted, for copying
};

// One embedded Claude Code session: the PTY, its grid renderer, keyboard and
// mouse translation, and a toolbar with /exit, stop and restart.
class TerminalPanel
{
public:
    // Checking: a VPN is in use and the exit country is being looked up before
    // spawning. Blocked: that exit is in Russia, so claude was not started.
    enum class Status { NotStarted, Checking, Blocked, Starting, Busy, Waiting, Idle, Exited, Failed };

    TerminalPanel(LaunchSpec spec, std::string title);

    // Starts claude — after the VPN exit check when a VPN is in use (see
    // net::EgressGuard). Returns false if it failed or was blocked at once;
    // otherwise the outcome may still be pending (Status::Checking).
    // `freshCheck` ignores a recent passing check ("check again").
    bool Launch(bool freshCheck = false);
    void RequestExit();      // types /exit so Claude Code shuts down cleanly
    void Stop();             // SIGTERM, SIGKILL after a grace period
    void RequestTerminate(); // SIGTERM only, returns immediately
    void Restart();          // relaunch, resuming this session if its id is known

    // Per-frame bookkeeping; call for every tab, including hidden ones.
    void Tick(const sessions::LiveSessionRegistry& live);
    // Toolbar + terminal, filling the current content region.
    void Draw();

    bool        Running() const { return term_.running(); }
    Status      GetStatus() const { return status_; }
    std::string StatusText() const;
    ImU32       StatusColor() const;
    const std::string& Title() const { return title_; }
    void        SetTitle(std::string t) { title_ = std::move(t); }
    const std::string& SessionId() const { return sessionId_; }
    const LaunchSpec&  Spec() const { return spec_; }
    Terminal::Progress Progress() const { return progress_; }
    std::uint64_t      Uid() const { return uid_; }

    bool NeedsAttention() const { return attention_; }
    bool HasFocus() const { return focused_; }
    void FocusTerminal() { wantFocus_ = true; }
    bool ExitRequested() const { return exitRequestedAt_.has_value(); }
    const std::string& ExitError() const { return exitError_; } // why the last /exit was called off
    bool TakeCloseRequest() { return std::exchange(closeRequested_, false); }
    // True once, the first time a periodic re-check (see PollVpnRecheck) finds
    // the VPN exit has become Russia while the session was already running.
    bool TakeVpnRussiaAlert() { return std::exchange(vpnRussiaAlertPending_, false); }

private:
    bool Spawn();
    // Acts on the exit check once it has finished: spawns, or blocks. True when settled.
    bool FinishLaunchIfReady();
    // While running, periodically re-runs the VPN exit check (independent of
    // the launch-gating `egress_`) so the chip stays current for the whole
    // life of the session, not just at launch.
    void PollVpnRecheck();
    void HandleKeyboard();
    void HandleMouse(const Terminal::Snapshot& snap, float charW, float charH, ImVec2 origin, bool hovered);
    void RenderGrid(const Terminal::Snapshot& snap, float charW, float charH, ImVec2 origin);
    void DrawToolbar();
    void DrawVpnChip();
    void DrawMenuItems();
    void CopySelection();
    void PasteClipboard();
    bool HasSelection() const { return selActive_ && (selA_.line != selB_.line || selA_.col != selB_.col); }

    LaunchSpec    spec_;
    std::string   title_;
    std::uint64_t uid_;
    std::string   sessionId_; // from --resume, or discovered through the live registry
    std::string   error_;
    Terminal      term_;
    Status        status_ = Status::NotStarted;
    Terminal::Progress progress_;
    std::string   waitingFor_; // "permission prompt", "input needed", ...
    bool          knownToRegistry_ = false;
    bool          resumable_ = false; // after exit: a transcript exists, Restart resumes it

    net::EgressGuard::Future egress_; // valid while a launch waits on the exit check
    net::EgressCheck         egressResult_; // latest finished check; NoVpn until one runs

    // Periodic re-check while running (separate future from `egress_`, which
    // only ever gates a launch and must not be disturbed by it).
    net::EgressGuard::Future vpnRecheck_;
    std::chrono::steady_clock::time_point nextVpnCheckAt_{};
    bool vpnRussiaAlertPending_ = false;

    std::chrono::steady_clock::time_point launchedAt_{};
    std::optional<std::chrono::steady_clock::time_point> exitRequestedAt_;
    // Keystrokes that must reach the PTY with gaps between them (see RequestExit).
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> pendingInput_;

    // Verified /exit protocol (see RequestExit).
    enum class ExitStep { None, AfterDeny, Clear, Confirm };
    struct PromptLine
    {
        std::uint32_t glyph = 0; // ❯ in normal input, ! in bash mode, …
        std::string   text;      // typed text, placeholder excluded
    };
    std::optional<PromptLine> ReadPrompt();
    void AdvanceExit();
    ExitStep    exitStep_ = ExitStep::None;
    int         exitAttempts_ = 0;
    std::string exitError_;
    std::chrono::steady_clock::time_point nextExitAt_{};

    // View
    int   scrollBack_ = 0;
    int   lastMaxScroll_ = 0;
    float wheelAccum_ = 0.0f;
    int   cols_ = 0, rows_ = 0;
    int   pendingCols_ = 0, pendingRows_ = 0;
    std::chrono::steady_clock::time_point pendingSince_{};
    bool  focused_ = false;
    bool  wantFocus_ = true;
    bool  attention_ = false;
    bool  closeRequested_ = false;
    int   lastMouseCol_ = -1, lastMouseRow_ = -1;
    int   forwardedButtons_ = 0; // presses sent to the app; their releases must follow

    struct GridPos
    {
        std::int64_t line = 0;
        int col = 0;
    };
    bool    selecting_ = false;
    bool    selActive_ = false;
    GridPos selA_, selB_;
};
} // namespace ui
