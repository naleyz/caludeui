#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>

// Packed 0xAABBGGRR (same layout as ImU32). 0 means "the terminal's default
// colour" — the renderer resolves it, so inverse video and theme changes work.
using TermColor = std::uint32_t;

enum TermFlag : std::uint16_t
{
    kTermBold      = 1 << 0,
    kTermDim       = 1 << 1,
    kTermItalic    = 1 << 2,
    kTermUnderline = 1 << 3,
    kTermInverse   = 1 << 4,
    kTermStrike    = 1 << 5,
    kTermWide      = 1 << 6, // first half of a double-width glyph
    kTermWideTail  = 1 << 7, // second half — never drawn on its own
};

struct TermAttr
{
    TermColor     fg    = 0;
    TermColor     bg    = 0;
    std::uint16_t flags = 0;
};

struct TermCell
{
    std::uint32_t cp    = ' ';
    TermColor     fg    = 0;
    TermColor     bg    = 0;
    std::uint16_t flags = 0;
};

struct TermRow
{
    std::vector<TermCell> cells;
    bool wrapped = false; // row continues onto the next one (auto-wrap)

    TermRow() = default;
    TermRow(int w, TermColor bg) : cells(static_cast<std::size_t>(w), TermCell{' ', 0, bg, 0}) {}
};

// A PTY + xterm-compatible VT parser: forks a child behind a pseudo-terminal,
// feeds its output through a CSI/OSC/DCS state machine and exposes the grid
// via snapshot(). Covers what Claude Code's TUI (both the classic and the
// fullscreen alt-screen renderer) actually emits: scroll regions, alternate
// screen, SGR mouse, bracketed paste, focus events, synchronized output,
// OSC 0/2 titles, OSC 52 clipboard and OSC 9 notifications/progress.
//
// Threading: a reader thread parses output under m_mu; everything else is
// called from the UI thread.
class Terminal
{
public:
    struct SpawnOptions
    {
        std::vector<std::string> argv; // argv[0] is resolved through PATH
        std::filesystem::path    cwd;
        std::vector<std::string> unsetEnv;
        std::vector<std::pair<std::string, std::string>> setEnv;
        int cols = 80;
        int rows = 24;
    };

    enum class MouseAction { Press, Release, Motion, WheelUp, WheelDown };
    struct MouseEvent
    {
        MouseAction action = MouseAction::Press;
        int  button = 0; // 0 left, 1 middle, 2 right, -1 none (plain motion)
        int  col = 0, row = 0;
        bool shift = false, alt = false, ctrl = false;
    };

    struct Snapshot
    {
        std::vector<TermRow> rows; // visible rows, top to bottom
        std::int64_t firstAbsLine = 0; // absolute line index of rows[0]
        int  cols = 80, screenRows = 24;
        int  scroll = 0, maxScroll = 0; // lines scrolled back from the live screen
        int  cursorX = 0, cursorY = -1; // relative to rows; -1 = not visible
        bool cursorVisible = true;
        bool altScreen     = false;
        bool mouseTracking = false;
        std::uint64_t generation = 0;   // bumps on every chunk of output
    };

    struct Progress
    {
        int state = 0; // OSC 9;4 — 0 none, 1 value, 2 error, 3 indeterminate, 4 paused
        int value = 0;
    };

    Terminal() = default;
    ~Terminal();
    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Returns false (and fills *error) if the PTY couldn't be opened, the cwd
    // doesn't exist or argv[0] couldn't be exec'd.
    bool start(const SpawnOptions& opts, std::string* error = nullptr);

    // SIGTERM, wait up to `grace`, then SIGKILL; always reaps the child.
    void stop(std::chrono::milliseconds grace = std::chrono::milliseconds(800));
    // Sends SIGTERM without waiting (lets several children wind down in parallel).
    void requestTerminate();

    // Reaps the child if it exited. Call every frame from the UI thread.
    void poll();

    void resize(int cols, int rows);
    void sendInput(std::string_view bytes);
    void paste(std::string_view text);
    // Encodes the event if the app enabled mouse reporting; false otherwise.
    bool sendMouse(const MouseEvent& ev);
    void setFocused(bool focused);

    Snapshot snapshot(int scrollBack, int maxRows);
    // Plain text between two absolute positions (inclusive start, exclusive end col).
    std::string textRange(std::int64_t line0, int col0, std::int64_t line1, int col1);

    bool running() const { return m_childPid > 0; }
    pid_t pid() const { return m_childPid; }
    std::optional<int> exitCode() const { return m_exitCode; }

    bool appCursorKeys();
    bool mouseTracking();
    std::string title();
    Progress progress();
    std::optional<std::string> takeClipboard(); // OSC 52 payload, decoded
    int takeAttention();                        // BEL + desktop notifications since last call

    // Looks `name` up in $PATH plus a few user-local fallbacks.
    static std::optional<std::filesystem::path> FindExecutable(const std::string& name);

private:
    struct CsiParams
    {
        char             prefix = 0; // '?', '>', '<', '=' or 0
        std::string      inter;      // intermediate bytes, e.g. "$" or "!"
        std::vector<int> args;       // -1 = omitted
        int arg(std::size_t i, int def) const { return i < args.size() && args[i] > 0 ? args[i] : def; }
        int raw(std::size_t i) const { return i < args.size() && args[i] >= 0 ? args[i] : 0; }
    };

    void readerLoop();
    void closeReader();
    void process(const char* buf, std::size_t n);
    void handleByte(unsigned char c);
    void handleControl(unsigned char c);
    void handleEsc(unsigned char c);
    void handleCsi(const std::string& seq, char finalByte);
    void handleOsc(const std::string& seq);
    void handleMode(const CsiParams& p, bool set);
    void applySgr(const CsiParams& p);
    void reply(std::string_view bytes);

    void putChar(std::uint32_t cp);
    void lineFeed();
    void reverseIndex();
    void scrollRegionUp(int n, bool toScrollback);
    void scrollRegionDown(int n);
    void eraseDisplay(int mode);
    void eraseLine(int mode);
    void enterAltScreen();
    void leaveAltScreen();
    void resetState();
    void clampCursor();
    void clearWideAt(TermRow& row, int x);
    TermRow blankRow() const { return TermRow(m_cols, m_attr.bg); }
    TermCell blankCell() const { return TermCell{' ', 0, m_attr.bg, 0}; }
    const std::vector<TermRow>& visibleScreen() const;

    // ── process ──────────────────────────────────────────────────────
    int   m_masterFd = -1;
    pid_t m_childPid = -1;
    std::optional<int> m_exitCode;
    std::chrono::steady_clock::time_point m_reapedAt{};
    std::atomic<bool> m_readerStop{false};
    std::atomic<bool> m_readerDone{true};
    std::thread       m_reader;
    std::mutex        m_mu;

    // ── grid ─────────────────────────────────────────────────────────
    int m_cols = 80, m_rows = 24;
    int m_cx = 0, m_cy = 0;
    int m_top = 0, m_bottom = 23; // scroll region, inclusive
    struct SavedCursor { int x = 0, y = 0; TermAttr attr; };
    SavedCursor m_saved;
    TermAttr    m_attr;

    static constexpr std::size_t kMaxScrollback = 5000;
    std::deque<TermRow>  m_scrollback;
    std::int64_t         m_droppedLines = 0; // lines trimmed off the scrollback front
    std::vector<TermRow> m_screen;
    std::vector<TermRow> m_primary;          // the main screen while the alt screen is up
    bool                 m_alt = false;
    std::uint32_t        m_lastCp = ' ';

    // ── modes ────────────────────────────────────────────────────────
    bool m_appCursor = false;
    bool m_autowrap = true;
    bool m_cursorVisible = true;
    int  m_mouseMode = 0; // 0, 1000, 1002, 1003
    bool m_mouseSgr = false;
    bool m_focusEvents = false;
    bool m_bracketedPaste = false;
    bool m_sync = false;
    std::chrono::steady_clock::time_point m_syncSince{};
    std::vector<TermRow> m_syncFrozen;
    int  m_syncCx = 0, m_syncCy = 0;

    // ── side channels ────────────────────────────────────────────────
    std::string   m_title;
    Progress      m_progress;
    std::optional<std::string> m_clipboard;
    int           m_attention = 0;
    std::uint64_t m_generation = 0;
    int           m_mouseButtonsDown = 0;

    // ── parser ───────────────────────────────────────────────────────
    enum class PS { Ground, Esc, EscCharset, Csi, Osc, OscEsc, Str, StrEsc };
    PS          m_ps = PS::Ground;
    std::string m_seq;
    std::uint32_t m_utfCp = 0;
    int           m_utfRemaining = 0;
};
