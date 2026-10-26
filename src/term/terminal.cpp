#include "term/terminal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace
{
using Clock = std::chrono::steady_clock;

// How long a synchronized-output block (?2026h) may hold back rendering
// before we give up and show whatever is on the grid.
constexpr auto kSyncTimeout = std::chrono::milliseconds(250);

constexpr TermColor Rgb(int r, int g, int b)
{
    return 0xFF000000u | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(g) << 8) |
           static_cast<std::uint32_t>(r);
}

constexpr TermColor kAnsi16[16] = {
    Rgb(0x2a, 0x29, 0x27), Rgb(0xd9, 0x5f, 0x57), Rgb(0x78, 0x8c, 0x5d), Rgb(0xc8, 0xa6, 0x5a),
    Rgb(0x6a, 0x9b, 0xcc), Rgb(0xb0, 0x7a, 0xb8), Rgb(0x5f, 0xa8, 0xa8), Rgb(0xb0, 0xae, 0xa5),
    Rgb(0x6b, 0x69, 0x63), Rgb(0xef, 0x86, 0x6e), Rgb(0x9c, 0xb6, 0x7a), Rgb(0xe3, 0xc2, 0x78),
    Rgb(0x8f, 0xb8, 0xe0), Rgb(0xcf, 0x9c, 0xd6), Rgb(0x86, 0xc8, 0xc8), Rgb(0xfa, 0xf9, 0xf5),
};

TermColor Ansi256(int n)
{
    if (n < 0) return 0;
    if (n < 16) return kAnsi16[n];
    if (n < 232)
    {
        n -= 16;
        auto v = [](int x) { return x ? 55 + x * 40 : 0; };
        return Rgb(v(n / 36), v((n / 6) % 6), v(n % 6));
    }
    if (n < 256)
    {
        int g = 8 + (n - 232) * 10;
        return Rgb(g, g, g);
    }
    return 0;
}

// Display width of a codepoint: 0 for combining/zero-width, 2 for East Asian
// wide and emoji-presentation ranges, 1 otherwise. Mirrors what Ink's
// string-width computes closely enough for Claude Code's layout to line up.
int CharWidth(std::uint32_t cp)
{
    struct Range { std::uint32_t lo, hi; };
    static constexpr Range kZero[] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
        {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
        {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF},
    };
    static constexpr Range kWide[] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
        {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
        {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
        {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
        {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
        {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
        {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
        {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
        {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
        {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
        {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
    };
    auto in = [cp](const Range* begin, const Range* end) {
        auto it = std::upper_bound(begin, end, cp, [](std::uint32_t v, const Range& r) { return v < r.lo; });
        return it != begin && cp <= (it - 1)->hi;
    };
    if (cp < 0x300) return 1;
    if (in(std::begin(kZero), std::end(kZero))) return 0;
    if (in(std::begin(kWide), std::end(kWide))) return 2;
    return 1;
}

std::string DecodeBase64(std::string_view in)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in)
    {
        int v = val(c);
        if (v < 0) continue; // padding / whitespace
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

void AppendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void WriteAll(int fd, std::string_view bytes)
{
    std::size_t done = 0;
    int spins = 0;
    while (done < bytes.size())
    {
        ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n > 0)
        {
            done += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN && ++spins < 200)
        {
            // The child isn't draining its input (e.g. a huge paste) — give it a moment.
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            timeval tv{0, 10000};
            ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
            continue;
        }
        break;
    }
}
} // namespace

// ── lifecycle ───────────────────────────────────────────────────────────
Terminal::~Terminal() { stop(std::chrono::milliseconds(300)); }

std::optional<std::filesystem::path> Terminal::FindExecutable(const std::string& name)
{
    if (name.empty()) return std::nullopt;
    if (name.find('/') != std::string::npos)
    {
        if (::access(name.c_str(), X_OK) == 0) return std::filesystem::path(name);
        return std::nullopt;
    }

    std::vector<std::string> dirs;
    if (const char* path = std::getenv("PATH"))
    {
        std::string_view sv(path);
        while (!sv.empty())
        {
            auto pos = sv.find(':');
            auto dir = sv.substr(0, pos);
            if (!dir.empty()) dirs.emplace_back(dir);
            if (pos == std::string_view::npos) break;
            sv.remove_prefix(pos + 1);
        }
    }
    // GUI launchers often don't carry the login shell's PATH.
    if (const char* home = std::getenv("HOME"))
    {
        dirs.push_back(std::string(home) + "/.local/bin");
        dirs.push_back(std::string(home) + "/.claude/local");
        dirs.push_back(std::string(home) + "/.npm-global/bin");
    }
    dirs.emplace_back("/usr/local/bin");
    dirs.emplace_back("/usr/bin");

    for (const auto& d : dirs)
    {
        std::filesystem::path candidate = std::filesystem::path(d) / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return std::nullopt;
}

bool Terminal::start(const SpawnOptions& opts, std::string* error)
{
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };

    stop();
    if (opts.argv.empty()) return fail("пустая команда");

    auto exe = FindExecutable(opts.argv[0]);
    if (!exe) return fail("не найден исполняемый файл «" + opts.argv[0] + "» (PATH, ~/.local/bin)");

    std::error_code ec;
    if (!opts.cwd.empty() && !std::filesystem::is_directory(opts.cwd, ec))
        return fail("папка не существует: " + opts.cwd.string());

    {
        std::lock_guard lock(m_mu);
        m_cols = std::max(2, opts.cols);
        m_rows = std::max(2, opts.rows);
        resetState();
        m_scrollback.clear();
        m_droppedLines = 0;
        m_title.clear();
        m_progress = {};
        m_clipboard.reset();
        m_attention = 0;
    }
    m_exitCode.reset();

    // Everything the child needs is built before fork(): the parent is
    // multi-threaded, so the child may only make async-signal-safe calls.
    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e)
    {
        std::string_view kv(*e);
        std::string_view key = kv.substr(0, kv.find('='));
        bool drop = key == "TERM" || key == "COLORTERM" || key == "TERM_PROGRAM";
        for (const auto& u : opts.unsetEnv)
            drop = drop || key == u;
        for (const auto& [k, v] : opts.setEnv)
            drop = drop || key == k;
        if (!drop) envStrings.emplace_back(kv);
    }
    envStrings.emplace_back("TERM=xterm-256color");
    envStrings.emplace_back("COLORTERM=truecolor");
    envStrings.emplace_back("TERM_PROGRAM=ClaudeUI");
    for (const auto& [k, v] : opts.setEnv)
        envStrings.push_back(k + "=" + v);

    std::vector<char*> envp;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> argvStrings = opts.argv;
    std::vector<char*> argv;
    for (auto& s : argvStrings) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::string exePath = exe->string();
    std::string cwd     = opts.cwd.string();

    int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return fail(std::string("posix_openpt: ") + std::strerror(errno));
    if (::grantpt(master) < 0 || ::unlockpt(master) < 0)
    {
        ::close(master);
        return fail(std::string("grantpt/unlockpt: ") + std::strerror(errno));
    }
    char slaveName[128];
    if (::ptsname_r(master, slaveName, sizeof(slaveName)) != 0)
    {
        ::close(master);
        return fail("ptsname_r failed");
    }
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(m_cols);
    ws.ws_row = static_cast<unsigned short>(m_rows);
    ::ioctl(master, TIOCSWINSZ, &ws);

    int slave = ::open(slaveName, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0)
    {
        ::close(master);
        return fail(std::string("open pty slave: ") + std::strerror(errno));
    }

    // Reports a failed chdir/exec back to the parent; closes on successful exec.
    int errPipe[2];
    if (::pipe2(errPipe, O_CLOEXEC) != 0)
    {
        ::close(master);
        ::close(slave);
        return fail(std::string("pipe2: ") + std::strerror(errno));
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(master);
        ::close(slave);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        return fail(std::string("fork: ") + std::strerror(errno));
    }

    if (pid == 0)
    {
        ::close(errPipe[0]);
        ::setsid();
        ::ioctl(slave, TIOCSCTTY, 0);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) ::close(slave);

        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);

        char tag = 'c';
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
        {
            int e = errno;
            (void)!::write(errPipe[1], &tag, 1);
            (void)!::write(errPipe[1], &e, sizeof(e));
            ::_exit(127);
        }
        ::execve(exePath.c_str(), argv.data(), envp.data());
        int e = errno;
        tag = 'e';
        (void)!::write(errPipe[1], &tag, 1);
        (void)!::write(errPipe[1], &e, sizeof(e));
        ::_exit(127);
    }

    ::close(slave);
    ::close(errPipe[1]);

    char tag = 0;
    int  childErrno = 0;
    ssize_t got;
    do got = ::read(errPipe[0], &tag, 1);
    while (got < 0 && errno == EINTR);
    if (got == 1)
    {
        ssize_t r;
        do r = ::read(errPipe[0], &childErrno, sizeof(childErrno));
        while (r < 0 && errno == EINTR);
    }
    ::close(errPipe[0]);

    if (got == 1)
    {
        ::waitpid(pid, nullptr, 0);
        ::close(master);
        return fail(std::string(tag == 'c' ? "chdir: " : "exec " + exePath + ": ") + std::strerror(childErrno));
    }

    int flags = ::fcntl(master, F_GETFL, 0);
    ::fcntl(master, F_SETFL, flags | O_NONBLOCK);

    m_masterFd = master;
    m_childPid = pid;
    m_readerStop.store(false);
    m_readerDone.store(false);
    m_reader = std::thread(&Terminal::readerLoop, this);
    return true;
}

void Terminal::requestTerminate()
{
    if (m_childPid > 0) ::kill(m_childPid, SIGTERM);
}

void Terminal::closeReader()
{
    m_readerStop.store(true);
    if (m_reader.joinable()) m_reader.join();
    if (m_masterFd >= 0)
    {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
}

void Terminal::stop(std::chrono::milliseconds grace)
{
    pid_t pid = m_childPid;
    if (pid > 0)
    {
        ::kill(pid, SIGTERM);
        auto deadline = Clock::now() + grace;
        int  status   = 0;
        pid_t r       = 0;
        while ((r = ::waitpid(pid, &status, WNOHANG)) == 0 && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (r == 0)
        {
            ::kill(pid, SIGKILL);
            r = ::waitpid(pid, &status, 0);
        }
        if (r == pid)
            m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        m_childPid = -1;
    }
    closeReader();
}

void Terminal::poll()
{
    if (m_childPid > 0)
    {
        int status = 0;
        pid_t r    = ::waitpid(m_childPid, &status, WNOHANG);
        if (r == m_childPid)
        {
            m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            m_childPid = -1;
            m_reapedAt = Clock::now();
        }
        else if (r < 0 && errno == ECHILD)
        {
            m_childPid = -1;
            m_reapedAt = Clock::now();
        }
    }
    // Let the reader drain the last output, but don't wait forever for
    // grandchildren that inherited the PTY.
    if (m_childPid <= 0 && m_masterFd >= 0 &&
        (m_readerDone.load() || Clock::now() - m_reapedAt > std::chrono::milliseconds(300)))
        closeReader();
}

void Terminal::readerLoop()
{
    char buf[16384];
    while (!m_readerStop.load(std::memory_order_relaxed))
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_masterFd, &rfds);
        timeval tv{0, 16000};
        int ret = ::select(m_masterFd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break; // EIO: every slave fd closed — the child is gone

        std::lock_guard lock(m_mu);
        process(buf, static_cast<std::size_t>(n));
    }
    m_readerDone.store(true);
}

// ── input side ──────────────────────────────────────────────────────────
void Terminal::sendInput(std::string_view bytes)
{
    if (m_masterFd < 0 || m_childPid <= 0 || bytes.empty()) return;
    WriteAll(m_masterFd, bytes);
}

void Terminal::reply(std::string_view bytes)
{
    // Called from the reader thread with m_mu held.
    if (m_masterFd >= 0) WriteAll(m_masterFd, bytes);
}

void Terminal::paste(std::string_view text)
{
    bool bracketed;
    {
        std::lock_guard lock(m_mu);
        bracketed = m_bracketedPaste;
    }
    std::string data;
    data.reserve(text.size() + 12);
    if (bracketed) data += "\x1b[200~";
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        if (c == '\n') c = '\r';
        // A pasted end-marker would let the clipboard escape bracketed mode.
        if (bracketed && c == '\x1b') continue;
        data.push_back(c);
    }
    if (bracketed) data += "\x1b[201~";
    sendInput(data);
}

bool Terminal::sendMouse(const MouseEvent& ev)
{
    int  mode;
    bool sgr;
    {
        std::lock_guard lock(m_mu);
        mode = m_mouseMode;
        sgr  = m_mouseSgr;
    }
    if (mode == 0) return false;

    int  cb      = 0;
    bool release = false;
    switch (ev.action)
    {
    case MouseAction::Press:
        cb = std::clamp(ev.button, 0, 2);
        m_mouseButtonsDown |= 1 << cb;
        break;
    case MouseAction::Release:
        cb      = sgr ? std::clamp(ev.button, 0, 2) : 3;
        release = true;
        m_mouseButtonsDown &= ~(1 << std::clamp(ev.button, 0, 2));
        break;
    case MouseAction::Motion:
        if (mode == 1000) return true; // swallowed: app only wants clicks
        if (mode == 1002 && m_mouseButtonsDown == 0) return true;
        cb = (ev.button >= 0 ? std::clamp(ev.button, 0, 2) : 3) + 32;
        break;
    case MouseAction::WheelUp:   cb = 64; break;
    case MouseAction::WheelDown: cb = 65; break;
    }
    if (ev.shift) cb += 4;
    if (ev.alt) cb += 8;
    if (ev.ctrl) cb += 16;

    int x = ev.col + 1, y = ev.row + 1;
    char buf[48];
    int  len;
    if (sgr)
        len = std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, x, y, release ? 'm' : 'M');
    else
    {
        if (x > 223 || y > 223) return true;
        len = std::snprintf(buf, sizeof(buf), "\x1b[M%c%c%c", 32 + cb, 32 + x, 32 + y);
    }
    sendInput(std::string_view(buf, static_cast<std::size_t>(len)));
    return true;
}

void Terminal::setFocused(bool focused)
{
    bool want;
    {
        std::lock_guard lock(m_mu);
        want = m_focusEvents;
    }
    if (want) sendInput(focused ? "\x1b[I" : "\x1b[O");
}

bool Terminal::appCursorKeys()
{
    std::lock_guard lock(m_mu);
    return m_appCursor;
}

bool Terminal::mouseTracking()
{
    std::lock_guard lock(m_mu);
    return m_mouseMode != 0;
}

std::string Terminal::title()
{
    std::lock_guard lock(m_mu);
    return m_title;
}

Terminal::Progress Terminal::progress()
{
    std::lock_guard lock(m_mu);
    return m_progress;
}

std::optional<std::string> Terminal::takeClipboard()
{
    std::lock_guard lock(m_mu);
    auto c = std::move(m_clipboard);
    m_clipboard.reset();
    return c;
}

int Terminal::takeAttention()
{
    std::lock_guard lock(m_mu);
    return std::exchange(m_attention, 0);
}

// ── snapshot ────────────────────────────────────────────────────────────
const std::vector<TermRow>& Terminal::visibleScreen() const
{
    if (m_sync && Clock::now() - m_syncSince < kSyncTimeout && !m_syncFrozen.empty()) return m_syncFrozen;
    return m_screen;
}

Terminal::Snapshot Terminal::snapshot(int scrollBack, int maxRows)
{
    std::lock_guard lock(m_mu);
    Snapshot s;
    s.cols          = m_cols;
    s.screenRows    = m_rows;
    s.altScreen     = m_alt;
    s.mouseTracking = m_mouseMode != 0;
    s.generation    = m_generation;

    const auto& screen = visibleScreen();
    const bool  frozen = &screen == &m_syncFrozen;
    int cx = frozen ? m_syncCx : m_cx;
    int cy = frozen ? m_syncCy : m_cy;
    s.cursorVisible = m_cursorVisible;

    const int sb     = m_alt ? 0 : static_cast<int>(m_scrollback.size());
    const int total  = sb + static_cast<int>(screen.size());
    const int want   = std::max(1, std::min(maxRows, static_cast<int>(screen.size())));
    s.maxScroll      = std::max(0, total - want);
    s.scroll         = std::clamp(scrollBack, 0, s.maxScroll);
    const int first  = total - want - s.scroll;
    s.firstAbsLine   = m_droppedLines + first;

    s.rows.reserve(static_cast<std::size_t>(want));
    for (int i = first; i < first + want; ++i)
    {
        if (i < sb) s.rows.push_back(m_scrollback[static_cast<std::size_t>(i)]);
        else s.rows.push_back(screen[static_cast<std::size_t>(i - sb)]);
    }

    // The screen's last `want` rows are shown when not scrolled; map the cursor.
    int cursorLine = sb + cy;
    s.cursorX = std::min(cx, m_cols - 1);
    s.cursorY = cursorLine - first;
    if (s.cursorY < 0 || s.cursorY >= want) s.cursorY = -1;
    return s;
}

std::string Terminal::textRange(std::int64_t line0, int col0, std::int64_t line1, int col1)
{
    std::lock_guard lock(m_mu);
    if (line1 < line0 || (line1 == line0 && col1 < col0))
    {
        std::swap(line0, line1);
        std::swap(col0, col1);
    }
    const std::int64_t sb = m_alt ? 0 : static_cast<std::int64_t>(m_scrollback.size());
    auto rowAt = [&](std::int64_t abs) -> const TermRow* {
        std::int64_t i = abs - m_droppedLines;
        if (i < 0) return nullptr;
        if (i < sb) return &m_scrollback[static_cast<std::size_t>(i)];
        i -= sb;
        if (i < static_cast<std::int64_t>(m_screen.size())) return &m_screen[static_cast<std::size_t>(i)];
        return nullptr;
    };

    std::string out;
    for (std::int64_t l = line0; l <= line1; ++l)
    {
        const TermRow* row = rowAt(l);
        if (!row) continue;
        int from = l == line0 ? col0 : 0;
        int to   = l == line1 ? col1 : static_cast<int>(row->cells.size());
        to       = std::min(to, static_cast<int>(row->cells.size()));
        std::string line;
        for (int x = std::max(0, from); x < to; ++x)
        {
            const auto& c = row->cells[static_cast<std::size_t>(x)];
            if (c.flags & kTermWideTail) continue;
            AppendUtf8(line, c.cp ? c.cp : ' ');
        }
        if (l != line1 || to >= static_cast<int>(row->cells.size()))
            while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (l != line1 && !row->wrapped) out.push_back('\n');
    }
    return out;
}

// ── resize ──────────────────────────────────────────────────────────────
void Terminal::resize(int cols, int rows)
{
    cols = std::max(2, cols);
    rows = std::max(2, rows);
    {
        std::lock_guard lock(m_mu);
        if (cols == m_cols && rows == m_rows) return;

        auto fit = [&](std::vector<TermRow>& buf, bool isLive, bool toScrollback) {
            if (static_cast<int>(buf.size()) > rows)
            {
                int excess = static_cast<int>(buf.size()) - rows;
                // Drop blank rows below the cursor first, then scroll the top away.
                int below = isLive ? std::max(0, static_cast<int>(buf.size()) - 1 - m_cy) : excess;
                int fromBottom = std::min(excess, below);
                buf.resize(buf.size() - static_cast<std::size_t>(fromBottom));
                int fromTop = excess - fromBottom;
                for (int i = 0; i < fromTop; ++i)
                {
                    if (toScrollback)
                    {
                        m_scrollback.push_back(std::move(buf.front()));
                        if (m_scrollback.size() > kMaxScrollback)
                        {
                            m_scrollback.pop_front();
                            ++m_droppedLines;
                        }
                    }
                    buf.erase(buf.begin());
                }
                if (isLive) m_cy -= fromTop;
            }
            while (static_cast<int>(buf.size()) < rows) buf.emplace_back(cols, 0);
            for (auto& r : buf) r.cells.resize(static_cast<std::size_t>(cols), TermCell{});
        };

        m_cols = cols;
        if (m_alt)
        {
            fit(m_screen, true, false);
            int savedCy = m_cy;
            m_cy = m_saved.y;
            fit(m_primary, true, true);
            m_saved.y = m_cy;
            m_cy = savedCy;
        }
        else
        {
            fit(m_screen, true, true);
        }
        m_syncFrozen.clear();
        m_rows   = rows;
        m_top    = 0;
        m_bottom = rows - 1;
        clampCursor();
        m_saved.x = std::clamp(m_saved.x, 0, cols - 1);
        m_saved.y = std::clamp(m_saved.y, 0, rows - 1);
        ++m_generation;
    }

    if (m_masterFd >= 0)
    {
        winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ::ioctl(m_masterFd, TIOCSWINSZ, &ws); // the kernel delivers SIGWINCH
    }
}

// ── parser ──────────────────────────────────────────────────────────────
void Terminal::resetState()
{
    m_screen.assign(static_cast<std::size_t>(m_rows), TermRow(m_cols, 0));
    m_primary.clear();
    m_alt = false;
    m_cx = m_cy = 0;
    m_top = 0;
    m_bottom = m_rows - 1;
    m_saved = {};
    m_attr = {};
    m_appCursor = false;
    m_autowrap = true;
    m_cursorVisible = true;
    m_mouseMode = 0;
    m_mouseSgr = false;
    m_focusEvents = false;
    m_bracketedPaste = false;
    m_sync = false;
    m_syncFrozen.clear();
    m_ps = PS::Ground;
    m_seq.clear();
    m_utfCp = 0;
    m_utfRemaining = 0;
    m_lastCp = ' ';
}

void Terminal::process(const char* buf, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        handleByte(static_cast<unsigned char>(buf[i]));
    ++m_generation;
}

void Terminal::handleByte(unsigned char c)
{
    switch (m_ps)
    {
    case PS::Ground:
        if (c == 0x1b)
        {
            m_utfRemaining = 0;
            m_ps = PS::Esc;
        }
        else if (c < 0x20 || c == 0x7f) handleControl(c);
        else if (c < 0x80)
        {
            m_utfRemaining = 0;
            putChar(c);
        }
        else if ((c & 0xC0) == 0x80)
        {
            if (m_utfRemaining <= 0) return; // stray continuation byte
            m_utfCp = (m_utfCp << 6) | (c & 0x3F);
            if (--m_utfRemaining == 0) putChar(m_utfCp);
        }
        else if ((c & 0xE0) == 0xC0) { m_utfCp = c & 0x1F; m_utfRemaining = 1; }
        else if ((c & 0xF0) == 0xE0) { m_utfCp = c & 0x0F; m_utfRemaining = 2; }
        else if ((c & 0xF8) == 0xF0) { m_utfCp = c & 0x07; m_utfRemaining = 3; }
        break;

    case PS::Esc:
        handleEsc(c);
        break;

    case PS::EscCharset:
        m_ps = PS::Ground; // ESC ( B and friends: the designator byte is consumed
        break;

    case PS::Csi:
        if (c >= 0x40 && c <= 0x7e)
        {
            handleCsi(m_seq, static_cast<char>(c));
            m_ps = PS::Ground;
        }
        else if (c == 0x1b) { m_seq.clear(); m_ps = PS::Esc; }
        else if (c < 0x20) handleControl(c); // C0 controls execute mid-sequence
        else if (m_seq.size() < 256) m_seq.push_back(static_cast<char>(c));
        break;

    case PS::Osc:
        if (c == 0x07)
        {
            handleOsc(m_seq);
            m_ps = PS::Ground;
        }
        else if (c == 0x1b) m_ps = PS::OscEsc;
        else if (m_seq.size() < (1u << 20)) m_seq.push_back(static_cast<char>(c));
        break;

    case PS::OscEsc:
        if (c == '\\') handleOsc(m_seq);
        m_ps = PS::Ground;
        if (c != '\\') handleEsc(c);
        break;

    case PS::Str: // DCS / APC / PM / SOS payloads are ignored
        if (c == 0x1b) m_ps = PS::StrEsc;
        else if (c == 0x07) m_ps = PS::Ground;
        break;

    case PS::StrEsc:
        m_ps = c == '\\' ? PS::Ground : PS::Str;
        break;
    }
}

void Terminal::handleControl(unsigned char c)
{
    switch (c)
    {
    case '\r': m_cx = 0; break;
    case '\n':
    case 0x0b:
    case 0x0c: lineFeed(); break;
    case '\b':
        if (m_cx >= m_cols) m_cx = m_cols - 1;
        if (m_cx > 0) --m_cx;
        break;
    case '\t':
        m_cx = std::min(((std::min(m_cx, m_cols - 1) / 8) + 1) * 8, m_cols - 1);
        break;
    case 0x07: ++m_attention; break;
    default: break; // SO/SI/NUL/DEL
    }
}

void Terminal::handleEsc(unsigned char c)
{
    m_ps = PS::Ground;
    switch (c)
    {
    case '[': m_seq.clear(); m_ps = PS::Csi; break;
    case ']': m_seq.clear(); m_ps = PS::Osc; break;
    case 'P':
    case '_':
    case '^':
    case 'X': m_ps = PS::Str; break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
    case '#':
    case '%':
    case ' ': m_ps = PS::EscCharset; break;
    case '7': m_saved = {std::min(m_cx, m_cols - 1), m_cy, m_attr}; break;
    case '8':
        m_cx = m_saved.x;
        m_cy = m_saved.y;
        m_attr = m_saved.attr;
        clampCursor();
        break;
    case 'D': lineFeed(); break;
    case 'E': m_cx = 0; lineFeed(); break;
    case 'M': reverseIndex(); break;
    case 'c':
    {
        auto title = m_title;
        resetState();
        m_title = title;
        break;
    }
    case 0x1b: m_ps = PS::Esc; break;
    default: break; // ESC = / ESC > (keypad modes) and unknowns
    }
}

void Terminal::handleCsi(const std::string& seq, char f)
{
    CsiParams p;
    std::size_t i = 0;
    if (!seq.empty() && (seq[0] == '?' || seq[0] == '>' || seq[0] == '<' || seq[0] == '='))
        p.prefix = seq[i++];
    int  cur = -1;
    bool any = false;
    for (; i < seq.size(); ++i)
    {
        char c = seq[i];
        if (c >= '0' && c <= '9')
        {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 65535) cur = 65535;
            any = true;
        }
        else if (c == ';' || c == ':')
        {
            p.args.push_back(cur);
            cur = -1;
            any = true;
        }
        else if (c >= 0x20 && c <= 0x2f) p.inter.push_back(c);
    }
    if (any) p.args.push_back(cur);

    // ── prefixed / intermediate forms ────────────────────────────────
    if (p.prefix == '?')
    {
        if (p.inter == "$" && f == 'p') // DECRQM: report whether we know the mode
        {
            int mode = p.raw(0);
            int state = 0;
            switch (mode)
            {
            case 1:    state = m_appCursor ? 1 : 2; break;
            case 7:    state = m_autowrap ? 1 : 2; break;
            case 25:   state = m_cursorVisible ? 1 : 2; break;
            case 1000:
            case 1002:
            case 1003: state = m_mouseMode == mode ? 1 : 2; break;
            case 1004: state = m_focusEvents ? 1 : 2; break;
            case 1006: state = m_mouseSgr ? 1 : 2; break;
            case 1049: state = m_alt ? 1 : 2; break;
            case 2004: state = m_bracketedPaste ? 1 : 2; break;
            case 2026: state = m_sync ? 1 : 2; break;
            default:   state = 0; break;
            }
            char buf[40];
            int  len = std::snprintf(buf, sizeof(buf), "\x1b[?%d;%d$y", mode, state);
            reply(std::string_view(buf, static_cast<std::size_t>(len)));
            return;
        }
        if (f == 'h' || f == 'l')
        {
            handleMode(p, f == 'h');
            return;
        }
        if (f == 'J') { eraseDisplay(p.raw(0)); return; }
        if (f == 'K') { eraseLine(p.raw(0)); return; }
        return; // ?u (kitty keyboard query) etc.: unsupported, stay silent
    }
    if (p.prefix == '>')
    {
        if (f == 'c') reply("\x1b[>0;10;1c"); // DA2
        return; // >4m modifyOtherKeys, >0q XTVERSION, >u kitty push: ignored
    }
    if (p.prefix != 0) return;
    if (p.inter == "!" && f == 'p') // DECSTR soft reset
    {
        m_attr = {};
        m_appCursor = false;
        m_autowrap = true;
        m_cursorVisible = true;
        m_top = 0;
        m_bottom = m_rows - 1;
        m_saved = {};
        return;
    }
    if (!p.inter.empty()) return; // DECSCUSR (" q") and the like

    const bool pending = m_cx >= m_cols;
    switch (f)
    {
    case 'A': m_cy = std::max(m_cy >= m_top ? m_top : 0, m_cy - p.arg(0, 1)); m_cx = std::min(m_cx, m_cols - 1); break;
    case 'B': m_cy = std::min(m_cy <= m_bottom ? m_bottom : m_rows - 1, m_cy + p.arg(0, 1)); m_cx = std::min(m_cx, m_cols - 1); break;
    case 'C':
    case 'a': m_cx = std::min(m_cols - 1, std::min(m_cx, m_cols - 1) + p.arg(0, 1)); break;
    case 'D': m_cx = std::max(0, std::min(m_cx, m_cols - 1) - p.arg(0, 1)); break;
    case 'E': m_cy = std::min(m_rows - 1, m_cy + p.arg(0, 1)); m_cx = 0; break;
    case 'F': m_cy = std::max(0, m_cy - p.arg(0, 1)); m_cx = 0; break;
    case 'G':
    case '`': m_cx = std::clamp(p.arg(0, 1) - 1, 0, m_cols - 1); break;
    case 'd': m_cy = std::clamp(p.arg(0, 1) - 1, 0, m_rows - 1); m_cx = std::min(m_cx, m_cols - 1); break;
    case 'e': m_cy = std::min(m_rows - 1, m_cy + p.arg(0, 1)); m_cx = std::min(m_cx, m_cols - 1); break;
    case 'H':
    case 'f':
        m_cy = std::clamp(p.arg(0, 1) - 1, 0, m_rows - 1);
        m_cx = std::clamp(p.arg(1, 1) - 1, 0, m_cols - 1);
        break;
    case 'I':
        for (int k = 0; k < p.arg(0, 1); ++k) handleControl('\t');
        break;
    case 'Z':
        for (int k = 0; k < p.arg(0, 1); ++k)
            m_cx = std::max(0, ((std::min(m_cx, m_cols - 1) - 1) / 8) * 8);
        break;
    case 'J': eraseDisplay(p.raw(0)); break;
    case 'K': eraseLine(p.raw(0)); break;
    case 'X':
    {
        if (pending) break;
        auto& row = m_screen[static_cast<std::size_t>(m_cy)];
        int n = std::min(p.arg(0, 1), m_cols - m_cx);
        clearWideAt(row, m_cx);
        clearWideAt(row, m_cx + n - 1);
        for (int x = m_cx; x < m_cx + n; ++x) row.cells[static_cast<std::size_t>(x)] = blankCell();
        break;
    }
    case 'L':
        if (m_cy < m_top || m_cy > m_bottom) break;
        for (int k = 0; k < std::min(p.arg(0, 1), m_bottom - m_cy + 1); ++k)
        {
            m_screen.erase(m_screen.begin() + m_bottom);
            m_screen.insert(m_screen.begin() + m_cy, blankRow());
        }
        m_cx = 0;
        break;
    case 'M':
        if (m_cy < m_top || m_cy > m_bottom) break;
        for (int k = 0; k < std::min(p.arg(0, 1), m_bottom - m_cy + 1); ++k)
        {
            m_screen.erase(m_screen.begin() + m_cy);
            m_screen.insert(m_screen.begin() + m_bottom, blankRow());
        }
        m_cx = 0;
        break;
    case 'P':
    {
        if (pending) break;
        auto& cells = m_screen[static_cast<std::size_t>(m_cy)].cells;
        int n = std::min(p.arg(0, 1), m_cols - m_cx);
        cells.erase(cells.begin() + m_cx, cells.begin() + m_cx + n);
        cells.insert(cells.end(), static_cast<std::size_t>(n), blankCell());
        break;
    }
    case '@':
    {
        if (pending) break;
        auto& cells = m_screen[static_cast<std::size_t>(m_cy)].cells;
        int n = std::min(p.arg(0, 1), m_cols - m_cx);
        cells.insert(cells.begin() + m_cx, static_cast<std::size_t>(n), blankCell());
        cells.resize(static_cast<std::size_t>(m_cols));
        break;
    }
    case 'b':
        for (int k = 0; k < std::min(p.arg(0, 1), 4096); ++k) putChar(m_lastCp);
        break;
    case 'S': scrollRegionUp(p.arg(0, 1), false); break;
    case 'T': scrollRegionDown(p.arg(0, 1)); break;
    case 'r':
    {
        int top = p.arg(0, 1) - 1;
        int bot = p.arg(1, m_rows) - 1;
        top = std::clamp(top, 0, m_rows - 1);
        bot = std::clamp(bot, 0, m_rows - 1);
        if (top < bot)
        {
            m_top = top;
            m_bottom = bot;
            m_cx = m_cy = 0;
        }
        break;
    }
    case 's': m_saved = {std::min(m_cx, m_cols - 1), m_cy, m_attr}; break;
    case 'u':
        m_cx = m_saved.x;
        m_cy = m_saved.y;
        clampCursor();
        break;
    case 'm': applySgr(p); break;
    case 'c':
        if (p.raw(0) == 0) reply("\x1b[?62;22c"); // DA1: VT220 with ANSI colour
        break;
    case 'n':
        if (p.raw(0) == 5) reply("\x1b[0n");
        else if (p.raw(0) == 6)
        {
            char buf[32];
            int len = std::snprintf(buf, sizeof(buf), "\x1b[%d;%dR", m_cy + 1, std::min(m_cx, m_cols - 1) + 1);
            reply(std::string_view(buf, static_cast<std::size_t>(len)));
        }
        break;
    case 't':
        if (p.raw(0) == 18)
        {
            char buf[32];
            int len = std::snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt", m_rows, m_cols);
            reply(std::string_view(buf, static_cast<std::size_t>(len)));
        }
        break;
    default: break; // h/l (ANSI modes), g (tab clear), q (LEDs)...
    }
}

void Terminal::handleMode(const CsiParams& p, bool set)
{
    for (int mode : p.args)
    {
        switch (mode)
        {
        case 1:    m_appCursor = set; break;
        case 7:    m_autowrap = set; break;
        case 25:   m_cursorVisible = set; break;
        case 47:
        case 1047:
            if (set) enterAltScreen();
            else leaveAltScreen();
            break;
        case 1048:
            if (set) m_saved = {std::min(m_cx, m_cols - 1), m_cy, m_attr};
            else { m_cx = m_saved.x; m_cy = m_saved.y; m_attr = m_saved.attr; clampCursor(); }
            break;
        case 1049:
            if (set)
            {
                m_saved = {std::min(m_cx, m_cols - 1), m_cy, m_attr};
                enterAltScreen();
            }
            else
            {
                leaveAltScreen();
                m_cx = m_saved.x;
                m_cy = m_saved.y;
                m_attr = m_saved.attr;
                clampCursor();
            }
            break;
        case 1000:
        case 1002:
        case 1003:
            if (set) m_mouseMode = mode;
            else if (m_mouseMode == mode) m_mouseMode = 0;
            break;
        case 1004: m_focusEvents = set; break;
        case 1006: m_mouseSgr = set; break;
        case 2004: m_bracketedPaste = set; break;
        case 2026:
            if (set && !m_sync)
            {
                m_syncFrozen = m_screen;
                m_syncCx = m_cx;
                m_syncCy = m_cy;
                m_syncSince = Clock::now();
            }
            m_sync = set;
            if (!set) m_syncFrozen.clear();
            break;
        default: break;
        }
    }
}

void Terminal::applySgr(const CsiParams& p)
{
    if (p.args.empty())
    {
        m_attr = {};
        return;
    }
    const auto& a = p.args;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        int n = a[i] < 0 ? 0 : a[i];
        auto extended = [&](TermColor& target) {
            if (i + 1 >= a.size()) return;
            int kind = a[i + 1];
            if (kind == 5 && i + 2 < a.size())
            {
                target = Ansi256(a[i + 2]);
                i += 2;
            }
            else if (kind == 2 && i + 4 < a.size())
            {
                // Tolerate the colon form with an empty colour-space id (38:2::r:g:b).
                std::size_t base = i + 2;
                if (i + 5 < a.size() && a[i + 2] < 0) base = i + 3;
                target = Rgb(std::clamp(a[base], 0, 255), std::clamp(a[base + 1], 0, 255),
                             std::clamp(a[base + 2], 0, 255));
                i = base + 2;
            }
        };
        switch (n)
        {
        case 0:  m_attr = {}; break;
        case 1:  m_attr.flags |= kTermBold; break;
        case 2:  m_attr.flags |= kTermDim; break;
        case 3:  m_attr.flags |= kTermItalic; break;
        case 4:  m_attr.flags |= kTermUnderline; break;
        case 7:  m_attr.flags |= kTermInverse; break;
        case 9:  m_attr.flags |= kTermStrike; break;
        case 21: m_attr.flags |= kTermUnderline; break;
        case 22: m_attr.flags &= ~(kTermBold | kTermDim); break;
        case 23: m_attr.flags &= ~kTermItalic; break;
        case 24: m_attr.flags &= ~kTermUnderline; break;
        case 27: m_attr.flags &= ~kTermInverse; break;
        case 29: m_attr.flags &= ~kTermStrike; break;
        case 38: extended(m_attr.fg); break;
        case 39: m_attr.fg = 0; break;
        case 48: extended(m_attr.bg); break;
        case 49: m_attr.bg = 0; break;
        case 58: { TermColor ignored = 0; extended(ignored); break; } // underline colour
        default:
            if (n >= 30 && n <= 37) m_attr.fg = kAnsi16[n - 30];
            else if (n >= 40 && n <= 47) m_attr.bg = kAnsi16[n - 40];
            else if (n >= 90 && n <= 97) m_attr.fg = kAnsi16[n - 90 + 8];
            else if (n >= 100 && n <= 107) m_attr.bg = kAnsi16[n - 100 + 8];
            break;
        }
    }
}

void Terminal::handleOsc(const std::string& seq)
{
    auto semi = seq.find(';');
    std::string_view code = std::string_view(seq).substr(0, semi);
    std::string_view rest = semi == std::string::npos ? std::string_view{} : std::string_view(seq).substr(semi + 1);

    if (code == "0" || code == "2")
    {
        m_title.assign(rest.substr(0, 512));
    }
    else if (code == "52")
    {
        auto s2 = rest.find(';');
        std::string_view data = s2 == std::string_view::npos ? rest : rest.substr(s2 + 1);
        if (data != "?") m_clipboard = DecodeBase64(data);
    }
    else if (code == "9")
    {
        if (rest.starts_with("4;"))
        {
            // ConEmu progress: 9;4;state;value
            int state = 0, value = 0;
            std::sscanf(std::string(rest.substr(2)).c_str(), "%d;%d", &state, &value);
            m_progress = {std::clamp(state, 0, 4), std::clamp(value, 0, 100)};
        }
        else
        {
            ++m_attention; // iTerm2-style desktop notification
        }
    }
    else if (code == "777" || code == "99")
    {
        ++m_attention;
    }
    else if (code == "10" || code == "11")
    {
        if (rest == "?")
        {
            // Lets "auto" themes detect a dark background.
            reply(code == "10" ? "\x1b]10;rgb:fafa/f9f9/f5f5\x1b\\" : "\x1b]11;rgb:1414/1414/1313\x1b\\");
        }
    }
}

// ── grid operations ─────────────────────────────────────────────────────
void Terminal::clampCursor()
{
    m_cx = std::clamp(m_cx, 0, m_cols - 1);
    m_cy = std::clamp(m_cy, 0, m_rows - 1);
}

void Terminal::clearWideAt(TermRow& row, int x)
{
    if (x < 0 || x >= static_cast<int>(row.cells.size())) return;
    auto& c = row.cells[static_cast<std::size_t>(x)];
    if ((c.flags & kTermWide) && x + 1 < static_cast<int>(row.cells.size()))
        row.cells[static_cast<std::size_t>(x + 1)] = TermCell{' ', c.fg, c.bg, 0};
    if ((c.flags & kTermWideTail) && x > 0)
        row.cells[static_cast<std::size_t>(x - 1)] = TermCell{' ', c.fg, c.bg, 0};
    c.flags &= ~(kTermWide | kTermWideTail);
}

void Terminal::putChar(std::uint32_t cp)
{
    int w = CharWidth(cp);
    if (w == 0) return;

    if (m_cx >= m_cols || (w == 2 && m_cx == m_cols - 1))
    {
        if (m_autowrap)
        {
            m_screen[static_cast<std::size_t>(m_cy)].wrapped = true;
            m_cx = 0;
            lineFeed();
        }
        else
        {
            m_cx = m_cols - w;
        }
    }

    auto& row = m_screen[static_cast<std::size_t>(m_cy)];
    clearWideAt(row, m_cx);
    if (w == 2) clearWideAt(row, m_cx + 1);
    row.cells[static_cast<std::size_t>(m_cx)] =
        TermCell{cp, m_attr.fg, m_attr.bg, static_cast<std::uint16_t>(m_attr.flags | (w == 2 ? kTermWide : 0))};
    if (w == 2)
        row.cells[static_cast<std::size_t>(m_cx + 1)] =
            TermCell{0, m_attr.fg, m_attr.bg, static_cast<std::uint16_t>(m_attr.flags | kTermWideTail)};
    m_cx += w;
    m_lastCp = cp;
}

void Terminal::lineFeed()
{
    if (m_cy == m_bottom) scrollRegionUp(1, true);
    else if (m_cy < m_rows - 1) ++m_cy;
}

void Terminal::reverseIndex()
{
    if (m_cy == m_top) scrollRegionDown(1);
    else if (m_cy > 0) --m_cy;
}

void Terminal::scrollRegionUp(int n, bool toScrollback)
{
    n = std::clamp(n, 1, m_bottom - m_top + 1);
    for (int k = 0; k < n; ++k)
    {
        TermRow gone = std::move(m_screen[static_cast<std::size_t>(m_top)]);
        m_screen.erase(m_screen.begin() + m_top);
        m_screen.insert(m_screen.begin() + m_bottom, blankRow());
        if (toScrollback && !m_alt && m_top == 0)
        {
            m_scrollback.push_back(std::move(gone));
            if (m_scrollback.size() > kMaxScrollback)
            {
                m_scrollback.pop_front();
                ++m_droppedLines;
            }
        }
    }
}

void Terminal::scrollRegionDown(int n)
{
    n = std::clamp(n, 1, m_bottom - m_top + 1);
    for (int k = 0; k < n; ++k)
    {
        m_screen.erase(m_screen.begin() + m_bottom);
        m_screen.insert(m_screen.begin() + m_top, blankRow());
    }
}

void Terminal::eraseDisplay(int mode)
{
    const int cx = std::min(m_cx, m_cols - 1);
    if (mode == 0)
    {
        eraseLine(0);
        for (int y = m_cy + 1; y < m_rows; ++y) m_screen[static_cast<std::size_t>(y)] = blankRow();
    }
    else if (mode == 1)
    {
        for (int y = 0; y < m_cy; ++y) m_screen[static_cast<std::size_t>(y)] = blankRow();
        auto& row = m_screen[static_cast<std::size_t>(m_cy)];
        clearWideAt(row, cx);
        for (int x = 0; x <= cx; ++x) row.cells[static_cast<std::size_t>(x)] = blankCell();
    }
    else if (mode == 2)
    {
        for (auto& r : m_screen) r = blankRow();
    }
    else if (mode == 3)
    {
        m_scrollback.clear();
        m_droppedLines = 0;
    }
}

void Terminal::eraseLine(int mode)
{
    auto& row = m_screen[static_cast<std::size_t>(m_cy)];
    const int cx = std::min(m_cx, m_cols - 1);
    if (mode == 0)
    {
        clearWideAt(row, cx);
        for (int x = cx; x < m_cols; ++x) row.cells[static_cast<std::size_t>(x)] = blankCell();
        row.wrapped = false;
    }
    else if (mode == 1)
    {
        clearWideAt(row, cx);
        for (int x = 0; x <= cx; ++x) row.cells[static_cast<std::size_t>(x)] = blankCell();
    }
    else if (mode == 2)
    {
        row = blankRow();
    }
}

void Terminal::enterAltScreen()
{
    if (m_alt) return;
    m_primary = std::move(m_screen);
    m_screen.assign(static_cast<std::size_t>(m_rows), blankRow());
    m_alt = true;
    m_top = 0;
    m_bottom = m_rows - 1;
}

void Terminal::leaveAltScreen()
{
    if (!m_alt) return;
    m_screen = std::move(m_primary);
    m_primary.clear();
    m_screen.resize(static_cast<std::size_t>(m_rows), TermRow(m_cols, 0));
    for (auto& r : m_screen) r.cells.resize(static_cast<std::size_t>(m_cols), TermCell{});
    m_alt = false;
    m_top = 0;
    m_bottom = m_rows - 1;
}
