#include "ui/terminal_panel.hpp"
#include "sessions/live_sessions.hpp"
#include "sessions/session_scanner.hpp"
#include "term/session_env.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/util.hpp"
#include "usage/usage.hpp"

#include <imgui_internal.h> // MenuItemEx: menu items with an aligned icon column

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// How often a running session's VPN exit is re-verified. net::EgressGuard
// shares/caches lookups by route signature, so several tabs ticking this on
// their own schedule don't each cause a separate network round trip.
constexpr auto kVpnRecheckInterval = 120s;

std::uint64_t NextUid()
{
    static std::uint64_t next = 1;
    return next++;
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * alpha});
}

// Blends two packed 0xAABBGGRR colours.
ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    auto ch = [&](int shift) {
        float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t) << shift;
    };
    return ch(0) | ch(8) | ch(16) | (0xFFu << 24);
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

bool Before(std::int64_t l0, int c0, std::int64_t l1, int c1)
{
    return l0 < l1 || (l0 == l1 && c0 < c1);
}

// Block elements (U+2580–U+259F) and light/heavy box drawing are drawn as
// geometry: font glyphs rarely fill the whole line height, which leaves
// gaps in Claude Code's logo and in box borders. Returns false for anything else.
bool DrawCellGraphic(ImDrawList* dl, std::uint32_t cp, float x, float y, float w, float h, ImU32 col)
{
    if (cp >= 0x2580 && cp <= 0x259F)
    {
        auto rect = [&](float x0, float y0, float x1, float y1) {
            dl->AddRectFilled({x + w * x0, y + h * y0}, {x + w * x1, y + h * y1}, col);
        };
        auto quads = [&](int bits) { // 1 UL, 2 UR, 4 LL, 8 LR
            if (bits & 1) rect(0, 0, 0.5f, 0.5f);
            if (bits & 2) rect(0.5f, 0, 1, 0.5f);
            if (bits & 4) rect(0, 0.5f, 0.5f, 1);
            if (bits & 8) rect(0.5f, 0.5f, 1, 1);
        };
        if (cp == 0x2580) rect(0, 0, 1, 0.5f);
        else if (cp <= 0x2588) rect(0, 1.0f - (cp - 0x2580) / 8.0f, 1, 1); // ▁…█
        else if (cp <= 0x258F) rect(0, 0, (0x2590 - cp) / 8.0f, 1);         // ▉…▏
        else if (cp == 0x2590) rect(0.5f, 0, 1, 1);
        else if (cp <= 0x2593) // ░▒▓
        {
            ImU32 a = static_cast<ImU32>(((col >> 24) & 0xFF) * (cp - 0x2590) / 4);
            dl->AddRectFilled({x, y}, {x + w, y + h}, (col & 0x00FFFFFF) | (a << 24));
        }
        else if (cp == 0x2594) rect(0, 0, 1, 0.125f);
        else if (cp == 0x2595) rect(0.875f, 0, 1, 1);
        else
        {
            static constexpr int kQuads[] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};
            quads(kQuads[cp - 0x2596]);
        }
        return true;
    }

    // Box drawing: arms from the cell centre. 1 left, 2 right, 4 up, 8 down.
    int arms = 0;
    bool heavy = false;
    switch (cp)
    {
    case 0x2500: arms = 1 | 2; break;
    case 0x2501: arms = 1 | 2; heavy = true; break;
    case 0x2502: arms = 4 | 8; break;
    case 0x2503: arms = 4 | 8; heavy = true; break;
    case 0x250C: case 0x256D: arms = 2 | 8; break;
    case 0x2510: case 0x256E: arms = 1 | 8; break;
    case 0x2514: case 0x2570: arms = 2 | 4; break;
    case 0x2518: case 0x256F: arms = 1 | 4; break;
    case 0x251C: arms = 2 | 4 | 8; break;
    case 0x2524: arms = 1 | 4 | 8; break;
    case 0x252C: arms = 1 | 2 | 8; break;
    case 0x2534: arms = 1 | 2 | 4; break;
    case 0x253C: arms = 1 | 2 | 4 | 8; break;
    case 0x2574: arms = 1; break;
    case 0x2576: arms = 2; break;
    default: return false;
    }
    const float t  = std::max(1.0f, std::floor(w * (heavy ? 0.22f : 0.11f)));
    const float cx = std::floor(x + w * 0.5f - t * 0.5f), cy = std::floor(y + h * 0.5f - t * 0.5f);
    if (arms & 1) dl->AddRectFilled({x, cy}, {cx + t, cy + t}, col);
    if (arms & 2) dl->AddRectFilled({cx, cy}, {x + w, cy + t}, col);
    if (arms & 4) dl->AddRectFilled({cx, y}, {cx + t, cy + t}, col);
    if (arms & 8) dl->AddRectFilled({cx, cy}, {cx + t, y + h}, col);
    return true;
}
} // namespace

// ── LaunchSpec ──────────────────────────────────────────────────────────
std::vector<std::string> LaunchSpec::Args() const
{
    std::vector<std::string> a = {"claude"};
    if (!resumeId.empty()) a.insert(a.end(), {"--resume", resumeId});
    else if (continueLast) a.push_back("--continue");
    if (!permissionMode.empty()) a.insert(a.end(), {"--permission-mode", permissionMode});
    if (!model.empty()) a.insert(a.end(), {"--model", model});
    if (!initialPrompt.empty())
    {
        if (initialPrompt.front() == '-') a.push_back("--");
        a.push_back(initialPrompt);
    }
    return a;
}

std::string LaunchSpec::CommandLine() const
{
    std::string s = "cd " + ShellQuote(cwd.string()) + " &&";
    for (const auto& arg : Args()) s += " " + ShellQuote(arg);
    return s;
}

// ── lifecycle ───────────────────────────────────────────────────────────
TerminalPanel::TerminalPanel(LaunchSpec spec, std::string title)
    : spec_(std::move(spec)), title_(std::move(title)), uid_(NextUid()), sessionId_(spec_.resumeId)
{
}

bool TerminalPanel::Launch(bool freshCheck)
{
    error_.clear();
    exitRequestedAt_.reset();
    pendingInput_.clear();
    exitStep_ = ExitStep::None;
    exitError_.clear();
    scrollBack_ = 0;
    selActive_ = selecting_ = false;
    forwardedButtons_ = 0;
    knownToRegistry_ = false;
    waitingFor_.clear();
    wantFocus_ = true;
    vpnRecheck_ = {};
    nextVpnCheckAt_ = {};
    vpnRussiaAlertPending_ = false;

    // Without a VPN this resolves at once and claude starts within this call.
    egress_ = net::Guard().Check(freshCheck);
    FinishLaunchIfReady();
    return status_ != Status::Failed && status_ != Status::Blocked;
}

bool TerminalPanel::FinishLaunchIfReady()
{
    if (!egress_.valid()) return true;
    if (egress_.wait_for(0s) != std::future_status::ready)
    {
        status_ = Status::Checking;
        return false;
    }
    egressResult_ = egress_.get();
    egress_       = {};
    if (egressResult_.Blocks())
    {
        status_ = Status::Blocked;
        return true;
    }
    Spawn();
    return true;
}

bool TerminalPanel::Spawn()
{
    Terminal::SpawnOptions opts;
    opts.argv     = spec_.Args();
    opts.cwd      = spec_.cwd;
    opts.cols     = cols_ > 0 ? cols_ : 120;
    opts.rows     = rows_ > 0 ? rows_ : 40;
    opts.unsetEnv = kInheritedSessionEnv;

    // Have the session report its cost and live limits into a file the usage panel reads.
    const auto extras = usage::Global().ForLaunch(uid_, spec_.cwd);
    opts.argv.insert(opts.argv.begin() + 1, extras.args.begin(), extras.args.end());
    opts.setEnv = extras.env;
    // The proxy chosen in Settings → ClaudeUI, over whatever ClaudeUI inherited.
    const net::ProxyEnv proxy = net::SessionProxyEnv();
    opts.unsetEnv.insert(opts.unsetEnv.end(), proxy.unset.begin(), proxy.unset.end());
    opts.setEnv.insert(opts.setEnv.end(), proxy.set.begin(), proxy.set.end());

    if (!term_.start(opts, &error_))
    {
        status_ = Status::Failed;
        return false;
    }
    cols_ = opts.cols;
    rows_ = opts.rows;
    status_ = Status::Starting;
    launchedAt_ = Clock::now();
    return true;
}

// Typing "/exit⏎" blindly is not safe: on a permission prompt Enter would
// *approve* the pending action, and a half-typed draft would be submitted as
// "draft/exit". So exiting is a small verified protocol, driven from Tick():
//   1. waiting on a prompt/dialog → Esc (confirm:no), give it time to close;
//   2. read the input line off the screen; while it holds text, send
//      Ctrl+K / Ctrl+U (kill to end / to start of line) and re-check;
//   3. type "/exit", re-read, and press Enter only if the line is exactly "/exit".
// Anything unexpected cancels the exit with an explanation instead of guessing.
void TerminalPanel::RequestExit()
{
    if (!Running() || exitStep_ != ExitStep::None) return;
    exitError_.clear();
    exitAttempts_ = 0;
    exitRequestedAt_ = Clock::now();
    if (status_ == Status::Waiting)
    {
        term_.sendInput("\x1b");
        exitStep_ = ExitStep::AfterDeny;
        nextExitAt_ = Clock::now() + 500ms; // a lone Esc followed by '/' would read as Alt+/
    }
    else
    {
        exitStep_ = ExitStep::Clear;
        nextExitAt_ = Clock::now();
    }
}

std::optional<TerminalPanel::PromptLine> TerminalPanel::ReadPrompt()
{
    auto snap = term_.snapshot(0, std::max(rows_, 5));
    auto firstInk = [](const TermRow& row) -> int {
        for (int x = 0; x < static_cast<int>(row.cells.size()); ++x)
        {
            auto cp = row.cells[static_cast<std::size_t>(x)].cp;
            if (cp != ' ' && cp != 0 && cp != 0xA0) return x;
        }
        return -1;
    };
    auto isRule = [&](const TermRow& row) {
        int x = firstInk(row);
        return x >= 0 && row.cells[static_cast<std::size_t>(x)].cp == 0x2500;
    };
    auto rowText = [](const TermRow& row, int from) {
        std::string s;
        for (int x = from; x < static_cast<int>(row.cells.size()); ++x)
        {
            const auto& c = row.cells[static_cast<std::size_t>(x)];
            if (c.flags & kTermWideTail) continue;
            // Dim cells are the placeholder / suggested prompt, not typed text.
            if (c.flags & kTermDim) s.push_back(' ');
            else AppendUtf8(s, c.cp == 0 || c.cp == 0xA0 ? ' ' : c.cp);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        std::size_t lead = s.find_first_not_of(' ');
        return lead == std::string::npos ? std::string{} : s.substr(lead);
    };

    // The input box is: a rule line, the prompt row ("❯ text"), continuation
    // rows, another rule line; the mode/status line sits below the box, so a
    // candidate needs rules on both sides. Search from the bottom.
    const int n = static_cast<int>(snap.rows.size());
    for (int r = n - 1; r >= 1; --r)
    {
        const auto& row = snap.rows[static_cast<std::size_t>(r)];
        int x = firstInk(row);
        if (x < 0 || isRule(row) || !isRule(snap.rows[static_cast<std::size_t>(r - 1)])) continue;

        int bottom = r + 1;
        while (bottom < n && !isRule(snap.rows[static_cast<std::size_t>(bottom)])) ++bottom;
        if (bottom >= n) continue; // not enclosed: e.g. the status line under the box

        PromptLine p;
        p.glyph = row.cells[static_cast<std::size_t>(x)].cp;
        p.text = rowText(row, x + 1);
        for (int k = r + 1; k < bottom; ++k)
        {
            std::string more = rowText(snap.rows[static_cast<std::size_t>(k)], 0);
            if (!more.empty()) p.text += "\n" + more;
        }
        return p;
    }
    return std::nullopt;
}

void TerminalPanel::AdvanceExit()
{
    if (exitStep_ == ExitStep::None || Clock::now() < nextExitAt_ || !pendingInput_.empty()) return;
    const auto now = Clock::now();
    auto fail = [&](std::string why) {
        exitError_ = std::move(why);
        exitStep_ = ExitStep::None;
        exitRequestedAt_.reset();
    };
    if (!Running())
    {
        exitStep_ = ExitStep::None;
        return;
    }

    constexpr std::uint32_t kPromptGlyph = 0x276F; // ❯
    auto prompt = ReadPrompt();
#ifdef CLAUDEUI_DEBUG
    std::fprintf(stderr, "[exit] step=%d attempt=%d prompt=%s glyph=U+%04X text=\"%s\"\n", static_cast<int>(exitStep_),
                 exitAttempts_, prompt ? "found" : "none", prompt ? prompt->glyph : 0,
                 prompt ? prompt->text.c_str() : "");
#endif
    switch (exitStep_)
    {
    case ExitStep::None: break;
    case ExitStep::AfterDeny:
        exitStep_ = ExitStep::Clear;
        break;

    case ExitStep::Clear:
        if (!prompt)
        {
            if (++exitAttempts_ > 12) fail("не видно поля ввода — закройте открытый диалог и повторите");
            else nextExitAt_ = now + 250ms;
        }
        else if (prompt->glyph != kPromptGlyph)
        {
            fail("поле ввода в особом режиме — вернитесь в обычный ввод и повторите");
        }
        else if (!prompt->text.empty())
        {
            if (++exitAttempts_ > 4)
            {
                fail("не удалось очистить поле ввода — очистите его вручную");
                break;
            }
            // Each key as its own write so they can't be parsed as one paste.
            const int lines = 2 + static_cast<int>(std::count(prompt->text.begin(), prompt->text.end(), '\n'));
            auto at = now;
            for (int i = 0; i < lines * 2; ++i)
            {
                pendingInput_.push_back({at, "\x0b"}); // Ctrl+K
                at += 12ms;
                pendingInput_.push_back({at, "\x15"}); // Ctrl+U
                at += 12ms;
            }
            nextExitAt_ = at + 250ms;
        }
        else
        {
            term_.sendInput("/exit");
            exitStep_ = ExitStep::Confirm;
            exitAttempts_ = 0;
            nextExitAt_ = now + 250ms;
        }
        break;

    case ExitStep::Confirm:
        if (prompt && prompt->glyph == kPromptGlyph && prompt->text == "/exit")
        {
#ifdef CLAUDEUI_DEBUG
            if (std::getenv("CLAUDEUI_EXIT_DRYRUN"))
            {
                std::fprintf(stderr, "[exit] DRYRUN: would press Enter now\n");
                exitStep_ = ExitStep::None;
                break;
            }
#endif
            term_.sendInput("\r");
            exitStep_ = ExitStep::None;
        }
        else if (++exitAttempts_ > 8)
        {
            fail("в поле ввода не «/exit» — Enter не нажат, проверьте поле ввода");
        }
        else
        {
            nextExitAt_ = now + 150ms;
        }
        break;
    }
}

void TerminalPanel::Stop()
{
    term_.stop();
}

void TerminalPanel::RequestTerminate()
{
    term_.requestTerminate();
}

void TerminalPanel::Restart()
{
    if (Running()) term_.stop();
    if (sessions::SessionScanner::TranscriptExists(sessionId_))
    {
        // Come back to the same conversation instead of starting a new one.
        spec_.resumeId = sessionId_;
        spec_.continueLast = false;
        spec_.initialPrompt.clear();
    }
    else
    {
        // Closed before the first message: there is nothing to resume.
        spec_.resumeId.clear();
        sessionId_.clear();
    }
    Launch(/*freshCheck=*/true);
}

// Re-verifies the VPN exit every kVpnRecheckInterval while the session is
// running, independent of the one-shot check `Launch` gated on. A launch
// that hasn't settled yet (`egress_` still valid) is left alone — the first
// periodic check is scheduled from here once it has.
void TerminalPanel::PollVpnRecheck()
{
    if (vpnRecheck_.valid())
    {
        if (vpnRecheck_.wait_for(0s) != std::future_status::ready) return;
        const auto previous = egressResult_.outcome;
        egressResult_ = vpnRecheck_.get();
        vpnRecheck_   = {};
        if (egressResult_.outcome == net::EgressCheck::Outcome::Russia && previous != net::EgressCheck::Outcome::Russia)
            vpnRussiaAlertPending_ = true;
    }
    if (!Running() || egress_.valid()) return;
    const auto now = Clock::now();
    if (nextVpnCheckAt_ == Clock::time_point{}) nextVpnCheckAt_ = now + kVpnRecheckInterval;
    if (now < nextVpnCheckAt_) return;
    nextVpnCheckAt_ = now + kVpnRecheckInterval;
    vpnRecheck_     = net::Guard().Check();
}

void TerminalPanel::Tick(const sessions::LiveSessionRegistry& live)
{
    term_.poll();

    // A launch waiting on the VPN exit check: nothing else to do until it settles.
    if (egress_.valid() && !FinishLaunchIfReady()) return;
    PollVpnRecheck();

    while (!pendingInput_.empty() && Clock::now() >= pendingInput_.front().first)
    {
        if (Running()) term_.sendInput(pendingInput_.front().second);
        pendingInput_.erase(pendingInput_.begin());
    }
    AdvanceExit();

    if (auto clip = term_.takeClipboard()) ImGui::SetClipboardText(clip->c_str());
    if (term_.takeAttention() > 0 && !focused_) attention_ = true;
    progress_ = term_.progress();

    if (!Running())
    {
        if (status_ != Status::Failed && status_ != Status::NotStarted && status_ != Status::Exited &&
            status_ != Status::Blocked && status_ != Status::Checking)
        {
            status_ = Status::Exited;
            resumable_ = sessions::SessionScanner::TranscriptExists(sessionId_);
        }
        progress_ = {};
        return;
    }

    const Status before = status_;
    if (const auto* ls = live.FindByPid(term_.pid()))
    {
        knownToRegistry_ = true;
        if (!ls->sessionId.empty()) sessionId_ = ls->sessionId;
        if (ls->status == "busy") status_ = Status::Busy;
        else if (ls->status == "waiting") status_ = Status::Waiting;
        else status_ = Status::Idle;
        waitingFor_ = ls->waitingFor;
    }
    else if (!knownToRegistry_)
    {
        // Registry entries appear a moment after startup; older Claude Code
        // versions don't write them at all — then the terminal title decides.
        std::string t = term_.title();
        if (Clock::now() - launchedAt_ < 3s && t.empty()) status_ = Status::Starting;
        else if (!t.empty() && !t.starts_with("\xe2\x9c\xb3")) status_ = Status::Busy; // not "✳" → spinner
        else status_ = Status::Idle;
    }

    if (status_ == Status::Waiting && before != Status::Waiting && !focused_) attention_ = true;
}

std::string TerminalPanel::StatusText() const
{
    switch (status_)
    {
    case Status::NotStarted: return "не запущена";
    case Status::Checking:   return "проверка VPN…";
    case Status::Blocked:    return "заблокирована: выход VPN из РФ";
    case Status::Starting:   return "запуск…";
    case Status::Busy:       return ExitRequested() ? "работает, /exit в очереди" : "работает";
    case Status::Waiting:
        if (waitingFor_ == "permission prompt") return "ждёт разрешения";
        if (waitingFor_ == "input needed") return "ждёт ответа";
        if (waitingFor_ == "dialog open") return "открыт диалог";
        return "ждёт вас";
    case Status::Idle:       return ExitRequested() ? "завершается…" : "ожидает ввода";
    case Status::Exited:
    {
        auto code = term_.exitCode();
        if (!code) return "завершена";
        return *code == 0 ? "завершена" : "завершена с кодом " + std::to_string(*code);
    }
    case Status::Failed:     return "не запустилась";
    }
    return {};
}

ImU32 TerminalPanel::StatusColor() const
{
    switch (status_)
    {
    case Status::Busy:    return Col(theme::palette::Orange);
    case Status::Waiting: return Col(theme::palette::Blue);
    case Status::Idle:    return Col(theme::palette::Green);
    case Status::Failed:
    case Status::Blocked: return IM_COL32(0xd9, 0x5f, 0x57, 0xff);
    default:              return Col(theme::palette::MidGray, 0.6f);
    }
}

// ── drawing ─────────────────────────────────────────────────────────────
namespace
{
struct ChipStyle
{
    ImU32 bg, fg;
};
constexpr ChipStyle kChipNeutral{IM_COL32(0x23, 0x23, 0x21, 255), IM_COL32(0xb0, 0xae, 0xa5, 255)};
constexpr ChipStyle kChipBlue{IM_COL32(0x1e, 0x2a, 0x36, 255), IM_COL32(0x9c, 0xc0, 0xe4, 255)};
constexpr ChipStyle kChipOrange{IM_COL32(0x3a, 0x2a, 0x22, 255), IM_COL32(0xe8, 0x95, 0x7a, 255)};
constexpr ChipStyle kChipGreen{IM_COL32(0x23, 0x25, 0x1c, 255), IM_COL32(0xa9, 0xbb, 0x8d, 255)};
constexpr ChipStyle kChipRed{IM_COL32(0x3c, 0x1e, 0x1c, 255), IM_COL32(0xec, 0x7e, 0x7e, 255)};

float ChipWidth(const char* text, bool dot)
{
    const float h = ImGui::GetFrameHeight() * 0.85f;
    return ImGui::CalcTextSize(text).x + h * 0.9f + (dot ? h * 0.45f : 0.0f);
}

// A pill-shaped label at the cursor; an item, so tooltips can hang off it.
// `dot`: 0 none, else the dot's colour; `spin` draws a spinner in its place.
void Chip(const char* id, const char* text, ChipStyle st, ImU32 dot = 0, bool spin = false)
{
    const float h = ImGui::GetFrameHeight() * 0.85f;
    const float w = ChipWidth(text, dot != 0 || spin);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, st.bg, h * 0.5f);
    float tx = p.x + h * 0.45f;
    if (spin) SpinnerAt(dl, {tx + h * 0.15f, p.y + h * 0.5f}, dot ? dot : st.fg, h * 0.2f, 1.5f);
    else if (dot) dl->AddCircleFilled({tx + h * 0.12f, p.y + h * 0.5f}, h * 0.13f, dot);
    if (dot || spin) tx += h * 0.45f;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText({tx, p.y + (h - ts.y) * 0.5f}, st.fg, text);
}
} // namespace

// VPN chip next to the status once the exit check has run over a VPN; what
// the check saw is in the tooltip. Not shown without a VPN, where nothing is checked.
// While running, this reflects the periodic re-check (PollVpnRecheck), not
// just the launch-time result, so a VPN that drops to RU mid-session turns
// the chip red instead of silently staying green.
void TerminalPanel::DrawVpnChip()
{
    using Outcome = net::EgressCheck::Outcome;
    if (status_ == Status::Checking || status_ == Status::NotStarted) return;
    if (!egressResult_.route.ViaVpn())
    {
        Chip("##vpn", "без VPN", kChipNeutral);
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("Прямое подключение — страна выхода не проверяется.");
            ImGui::TextColored(theme::palette::MidGray, "Прокси для сессий: Настройки → ClaudeUI");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        return;
    }

    const bool known = egressResult_.outcome == Outcome::Ok || egressResult_.outcome == Outcome::Russia;
    const bool russiaWhileRunning = egressResult_.outcome == Outcome::Russia && Running();
    const std::string text = known ? "VPN · " + egressResult_.countryCode : std::string("VPN · не проверен");
    const ChipStyle st = russiaWhileRunning                        ? kChipRed
                         : known && egressResult_.outcome == Outcome::Ok ? kChipGreen
                                                                         : kChipOrange;
    Chip("##vpn", text.c_str(), st);
    if (ImGui::BeginItemTooltip())
    {
        ImGui::TextUnformatted(egressResult_.route.Describe().c_str());
        ImGui::TextColored(theme::palette::MidGray, "%s", egressResult_.Summary().c_str());
        if (russiaWhileRunning)
            ImGui::TextColored(theme::palette::Orange, "Сессия уже была подключена — рекомендуем закрыть её (/exit)");
        ImGui::TextColored(theme::palette::MidGray, Running() ? "Проверяется каждые 2 мин, пока сессия активна"
                                                               : "Проверено при запуске");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
}

void TerminalPanel::DrawToolbar()
{
    const auto& style = ImGui::GetStyle();
    const float fs    = ImGui::GetFontSize();
    const bool running = Running();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const float  headH  = fs * 1.15f + fs * 0.9f + 6.0f; // title + path lines
    auto* dl = ImGui::GetWindowDrawList();

    // ── Right: chips and actions, measured first so the title can clip before them ──
    ChipStyle statusStyle = kChipNeutral;
    switch (status_)
    {
    case Status::Busy:    statusStyle = kChipOrange; break;
    case Status::Waiting: statusStyle = kChipBlue; break;
    case Status::Idle:    statusStyle = kChipGreen; break;
    case Status::Failed:
    case Status::Blocked: statusStyle = kChipRed; break;
    default: break;
    }
    const std::string status = StatusText();
    const bool spin = status_ == Status::Starting || status_ == Status::Checking;

    std::string cost, model, effort;
    if (const auto* u = usage::Global().Session(uid_))
    {
        if (u->costUsd > 0.0)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "$%.2f", u->costUsd);
            cost = buf;
        }
        model  = u->model;
        effort = u->effort;
    }
    // Until the session reports its model (status line), show the one it was launched with.
    if (model.empty() && !spec_.model.empty()) model = spec_.model;
    std::string modelChip = model;
    if (!modelChip.empty() && !effort.empty()) modelChip += " · " + effort;
    const bool vpnShown = status_ != Status::Checking && status_ != Status::NotStarted;
    const std::string vpnText = !egressResult_.route.ViaVpn() ? "без VPN"
                                : egressResult_.countryCode.empty() ? "VPN · не проверен"
                                                                    : "VPN · " + egressResult_.countryCode;
    const bool exitFailed = !exitError_.empty() && running;

    const char* primary = running                     ? icons::Label(icons::Icon::Exit, "/exit")
                          : status_ == Status::Blocked ? icons::Label(icons::Icon::Retry, "Проверить снова")
                                                       : icons::Label(icons::Icon::Retry, "Перезапустить");
    const char* more = icons::Glyph(icons::Icon::More);
    const float sp = style.ItemSpacing.x;
    float right = ChipWidth(status.c_str(), true) + sp;
    if (!modelChip.empty()) right += ChipWidth(modelChip.c_str(), false) + sp;
    if (vpnShown) right += ChipWidth(vpnText.c_str(), false) + sp;
    if (exitFailed) right += ChipWidth("/exit отменён", false) + sp;
    if (!cost.empty()) right += ChipWidth(cost.c_str(), false) + sp;
    right += sp + ImGui::CalcTextSize(primary).x + ImGui::CalcTextSize("Стоп").x + ImGui::CalcTextSize(more).x +
             style.FramePadding.x * 6 + sp * 2;

    // ── Left: title over the folder ─────────────────────────────────────
    const float leftMax = std::max(origin.x + fs * 6, origin.x + width - right - sp * 2);
    ImGui::PushClipRect(origin, {leftMax, origin.y + headH}, true);
    dl->AddText(ImGui::GetFont(), fs * 1.15f, {origin.x + 2, origin.y}, Col(theme::palette::Light), title_.c_str());
    const std::string path = TildePath(spec_.cwd.string());
    dl->AddText(ImGui::GetFont(), fs * 0.9f, {origin.x + 2, origin.y + fs * 1.15f + 4.0f}, IM_COL32(0x8f, 0x8d, 0x85, 255),
                path.c_str());
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##title", {std::max(1.0f, leftMax - origin.x), headH});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", spec_.CommandLine().c_str());

    // ── Right group, vertically centred on the two lines ───────────────
    const float chipH = ImGui::GetFrameHeight() * 0.85f;
    float x = std::max(leftMax + sp, origin.x + width - right);
    auto at = [&](float h) { ImGui::SetCursorScreenPos({x, origin.y + (headH - h) * 0.5f}); };

    at(chipH);
    Chip("##status", status.c_str(), statusStyle, statusStyle.fg, spin);
    if (!waitingFor_.empty() && status_ == Status::Waiting) ImGui::SetItemTooltip("%s", waitingFor_.c_str());
    x = ImGui::GetItemRectMax().x + sp;
    if (!modelChip.empty())
    {
        at(chipH);
        Chip("##model", modelChip.c_str(), kChipNeutral);
        ImGui::SetItemTooltip("Модель сессии%s", effort.empty() ? "" : " и уровень усилий (/effort)");
        x = ImGui::GetItemRectMax().x + sp;
    }
    if (vpnShown)
    {
        at(chipH);
        DrawVpnChip();
        x = ImGui::GetItemRectMax().x + sp;
    }
    if (exitFailed)
    {
        at(chipH);
        Chip("##exit_err", "/exit отменён", kChipRed);
        ImGui::SetItemTooltip("%s", exitError_.c_str());
        x = ImGui::GetItemRectMax().x + sp;
    }
    if (!cost.empty())
    {
        at(chipH);
        Chip("##cost", cost.c_str(), kChipNeutral);
        ImGui::SetItemTooltip("Стоимость сессии по прейскуранту API (столько насчитал Claude Code)");
        x = ImGui::GetItemRectMax().x + sp;
    }
    x += sp;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0x3a / 255.0f, 0x39 / 255.0f, 0x35 / 255.0f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
    at(ImGui::GetFrameHeight());
    if (running)
    {
        if (ImGui::Button(primary)) RequestExit();
        ImGui::SetItemTooltip("Отправить /exit — Claude Code сохранит сессию и завершится сам (Ctrl+Shift+W)");
    }
    else
    {
        ImGui::BeginDisabled(status_ == Status::Checking);
        if (ImGui::Button(primary)) Restart();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(status_ == Status::Blocked
                                  ? "Заново определить страну выхода VPN и запустить, если это уже не РФ"
                              : resumable_ ? "Продолжить эту же сессию (--resume)"
                                           : "Запустить заново");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Стоп")) Stop();
    ImGui::SetItemTooltip("Принудительно остановить процесс (SIGTERM)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushID("session_more"); // the glyph alone isn't a unique ID
    const bool openMenu = ImGui::Button(more);
    ImGui::PopID();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (openMenu) ImGui::OpenPopup("##session_menu");
    ImGui::SetItemTooltip("Ещё действия");
    if (ImGui::BeginPopup("##session_menu"))
    {
        DrawMenuItems();
        ImGui::EndPopup();
    }

    // Below the header: a hairline, with OSC 9;4 progress drawn over it.
    ImGui::SetCursorScreenPos({origin.x, origin.y + headH + 8.0f});
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine({p.x - style.WindowPadding.x, p.y}, {p.x + width + style.WindowPadding.x, p.y}, IM_COL32(0x2b, 0x2a, 0x27, 255));
    if (progress_.state != 0 && running)
    {
        ImU32 c = progress_.state == 2 ? IM_COL32(0xd9, 0x5f, 0x57, 0xff) : Col(theme::palette::Orange);
        if (progress_.state == 3) // indeterminate: a sliding segment
        {
            float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1.0));
            dl->AddRectFilled({p.x + width * t, p.y - 1}, {p.x + width * std::min(1.0f, t + 0.25f), p.y + 1}, c);
        }
        else
        {
            dl->AddRectFilled({p.x, p.y - 1}, {p.x + width * (progress_.value / 100.0f), p.y + 1}, c);
        }
    }
    ImGui::Dummy({width, 4});
}

void TerminalPanel::DrawMenuItems()
{
    if (ImGui::MenuItemEx("Копировать выделение", icons::MenuGlyph(icons::Icon::Copy), "Ctrl+Shift+C", false, HasSelection())) CopySelection();
    if (ImGui::MenuItemEx("Вставить", nullptr, "Ctrl+Shift+V", false, Running())) PasteClipboard();
    ImGui::Separator();
    if (ImGui::MenuItemEx("Копировать ID сессии", icons::MenuGlyph(icons::Icon::Copy), nullptr, false, !sessionId_.empty()))
        ImGui::SetClipboardText(sessionId_.c_str());
    if (ImGui::MenuItemEx("Копировать команду запуска", icons::MenuGlyph(icons::Icon::Copy)))
    {
        LaunchSpec s = spec_;
        if (sessions::SessionScanner::TranscriptExists(sessionId_))
        {
            s.resumeId = sessionId_;
            s.continueLast = false;
            s.initialPrompt.clear();
        }
        ImGui::SetClipboardText(s.CommandLine().c_str());
    }
    if (ImGui::MenuItemEx("Открыть папку проекта", icons::MenuGlyph(icons::Icon::Folder))) OpenWithDesktop(spec_.cwd);
    ImGui::Separator();
    if (ImGui::MenuItemEx("Прокрутить в конец", nullptr, nullptr, false, scrollBack_ > 0)) scrollBack_ = 0;
    if (ImGui::MenuItemEx("Перезапустить", icons::MenuGlyph(icons::Icon::Retry))) Restart();
    if (ImGui::MenuItemEx("Закрыть вкладку", icons::MenuGlyph(icons::Icon::Close), "Ctrl+Shift+W")) closeRequested_ = true;
}

void TerminalPanel::Draw()
{
    DrawToolbar();

    // The grid is measured and drawn in the monospace face; the toolbar above
    // and popups stay in the interface font.
    ImGui::PushFont(fonts::Mono, fonts::TerminalSize);
    const float charW = ImGui::CalcTextSize("M").x;
    const float charH = ImGui::GetTextLineHeight();
    constexpr float padX = 6.0f, padY = 4.0f;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int cols = std::max(20, static_cast<int>((avail.x - padX * 2) / charW));
    int rows = std::max(5, static_cast<int>((avail.y - padY * 2) / charH));

    // Resize once the layout settles: dragging the splitter would otherwise
    // SIGWINCH Claude Code on every frame and make it redraw constantly.
    if (cols != cols_ || rows != rows_)
    {
        if (cols != pendingCols_ || rows != pendingRows_)
        {
            pendingCols_ = cols;
            pendingRows_ = rows;
            pendingSince_ = Clock::now();
        }
        if (cols_ == 0 || Clock::now() - pendingSince_ > 80ms)
        {
            cols_ = cols;
            rows_ = rows;
            term_.resize(cols_, rows_);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::Dark);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padX, padY});
    ImGui::BeginChild("##term", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size   = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", {std::max(1.0f, size.x), std::max(1.0f, size.y)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    if (wantFocus_)
    {
        ImGui::SetWindowFocus();
        wantFocus_ = false;
    }
    const bool nowFocused = ImGui::IsWindowFocused();
    if (nowFocused != focused_)
    {
        focused_ = nowFocused;
        term_.setFocused(focused_);
    }
    if (focused_) attention_ = false;

    if (focused_ && Running()) HandleKeyboard();

    auto snap = term_.snapshot(scrollBack_, rows_);
    // Keep the view anchored while scrolled back and new lines arrive.
    if (scrollBack_ > 0 && snap.maxScroll > lastMaxScroll_)
    {
        scrollBack_ = std::min(scrollBack_ + snap.maxScroll - lastMaxScroll_, snap.maxScroll);
        snap = term_.snapshot(scrollBack_, rows_);
    }
    lastMaxScroll_ = snap.maxScroll;
    scrollBack_ = snap.scroll;

    HandleMouse(snap, charW, charH, origin, hovered);
    RenderGrid(snap, charW, charH, origin);
    ImGui::PopFont();

    if (ImGui::BeginPopup("##term_ctx"))
    {
        DrawMenuItems();
        ImGui::EndPopup();
    }

    // Overlays
    auto* dl = ImGui::GetWindowDrawList();
    if (scrollBack_ > 0)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " ↑ %d строк — Shift+PgDn / колесо вниз ", scrollBack_);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        ImVec2 p  = {origin.x + size.x - ts.x - 4, origin.y + 2};
        dl->AddRectFilled(p, {p.x + ts.x, p.y + ts.y}, Col(theme::palette::Blue, 0.85f), 3.0f);
        dl->AddText(p, Col(theme::palette::Dark), buf);
    }
    if (status_ == Status::Starting || status_ == Status::Checking)
    {
        const char* msg = status_ == Status::Checking ? "Проверка выходного IP (VPN)…" : "Запуск claude…";
        ImVec2 c  = {origin.x + size.x * 0.5f, origin.y + size.y * 0.5f - 14.0f};
        ImVec2 ms = ImGui::CalcTextSize(msg);
        SpinnerAt(dl, c, Col(theme::palette::Orange), 14.0f, 3.0f);
        dl->AddText({c.x - ms.x * 0.5f, c.y + 24.0f}, Col(theme::palette::MidGray), msg);
    }
    if (status_ == Status::Blocked)
    {
        const std::string lines[] = {
            "Запуск claude заблокирован",
            "Выход VPN — Российская Федерация: " + egressResult_.Summary(),
            "Маршрут: " + egressResult_.route.Describe(),
            "Смените сервер VPN и нажмите «Проверить снова».",
        };
        float textW = 0.0f, lineH = ImGui::GetTextLineHeightWithSpacing();
        for (const auto& l : lines) textW = std::max(textW, ImGui::CalcTextSize(l.c_str()).x);
        const float boxW = textW + 32, boxH = lineH * 4 + 24;
        ImVec2 p = {origin.x + (size.x - boxW) * 0.5f, origin.y + (size.y - boxH) * 0.5f};
        dl->AddRectFilled(p, {p.x + boxW, p.y + boxH}, Col(theme::palette::Dark, 0.94f), 6.0f);
        dl->AddRect(p, {p.x + boxW, p.y + boxH}, IM_COL32(0xd9, 0x5f, 0x57, 0xff), 6.0f);
        const ImU32 colors[] = {IM_COL32(0xd9, 0x5f, 0x57, 0xff), Col(theme::palette::Light),
                                Col(theme::palette::MidGray), Col(theme::palette::MidGray)};
        for (int i = 0; i < 4; ++i) dl->AddText({p.x + 16, p.y + 12 + lineH * static_cast<float>(i)}, colors[i], lines[i].c_str());
    }
    if (status_ == Status::Failed || status_ == Status::Exited)
    {
        std::string msg = status_ == Status::Failed ? "Не удалось запустить claude: " + error_
                                                    : "Сессия " + StatusText() + ".";
        std::string hint = resumable_ ? "«Перезапустить» — продолжить эту же сессию"
                                      : "«Перезапустить» — начать заново";
        ImVec2 ms = ImGui::CalcTextSize(msg.c_str());
        ImVec2 hs = ImGui::CalcTextSize(hint.c_str());
        float boxW = std::max(ms.x, hs.x) + 24, boxH = ms.y + hs.y + 20;
        ImVec2 p = {origin.x + (size.x - boxW) * 0.5f, origin.y + size.y - boxH - 12};
        dl->AddRectFilled(p, {p.x + boxW, p.y + boxH}, Col(theme::palette::Dark, 0.92f), 6.0f);
        dl->AddRect(p, {p.x + boxW, p.y + boxH}, Col(theme::palette::MidGray, 0.5f), 6.0f);
        dl->AddText({p.x + 12, p.y + 8}, Col(theme::palette::Light), msg.c_str());
        dl->AddText({p.x + 12, p.y + 12 + ms.y}, Col(theme::palette::MidGray), hint.c_str());
    }

    ImGui::EndChild();
}

void TerminalPanel::RenderGrid(const Terminal::Snapshot& snap, float charW, float charH, ImVec2 origin)
{
    auto* dl = ImGui::GetWindowDrawList();
    ImFont* regular = fonts::Mono ? fonts::Mono : ImGui::GetFont();
    ImFont* bold    = fonts::MonoBold ? fonts::MonoBold : regular;
    const float fontSize = ImGui::GetFontSize();

    const ImU32 defFg  = Col(theme::palette::LightGray);
    const ImU32 defBg  = Col(theme::palette::Dark);
    const ImU32 selCol = Col(theme::palette::Orange, 0.35f);

    GridPos s0 = selA_, s1 = selB_;
    if (Before(s1.line, s1.col, s0.line, s0.col)) std::swap(s0, s1);
    const bool sel = HasSelection();

    auto resolve = [&](const TermCell& c, ImU32& fg, ImU32& bg) {
        fg = c.fg ? c.fg : defFg;
        bg = c.bg;
        if (c.flags & kTermInverse)
        {
            ImU32 f = fg;
            fg = bg ? bg : defBg;
            bg = f;
        }
        if (c.flags & kTermDim) fg = Mix(fg, bg ? bg : defBg, 0.45f);
    };

    std::string run;
    for (int r = 0; r < static_cast<int>(snap.rows.size()); ++r)
    {
        const auto& cells = snap.rows[static_cast<std::size_t>(r)].cells;
        const int n = std::min(static_cast<int>(cells.size()), snap.cols);
        const float y = origin.y + r * charH;
        const std::int64_t absLine = snap.firstAbsLine + r;

        // Backgrounds, merged into runs.
        for (int x = 0; x < n;)
        {
            ImU32 fg, bg;
            resolve(cells[static_cast<std::size_t>(x)], fg, bg);
            int end = x + 1;
            while (end < n)
            {
                ImU32 f2, b2;
                resolve(cells[static_cast<std::size_t>(end)], f2, b2);
                if (b2 != bg) break;
                ++end;
            }
            if (bg) dl->AddRectFilled({origin.x + x * charW, y}, {origin.x + end * charW, y + charH}, bg);
            x = end;
        }

        if (sel && absLine >= s0.line && absLine <= s1.line)
        {
            int from = absLine == s0.line ? s0.col : 0;
            int to   = absLine == s1.line ? s1.col : snap.cols;
            if (to > from)
                dl->AddRectFilled({origin.x + from * charW, y}, {origin.x + to * charW, y + charH}, selCol);
        }

        // Glyphs: ASCII in runs of equal style; everything else one cell at a
        // time so a glyph whose advance differs from the grid can't shift the row.
        for (int x = 0; x < n;)
        {
            const TermCell& c = cells[static_cast<std::size_t>(x)];
            if (c.flags & kTermWideTail)
            {
                ++x;
                continue;
            }
            ImU32 fg, bg;
            resolve(c, fg, bg);
            const std::uint16_t style = c.flags & (kTermBold | kTermUnderline | kTermStrike);
            int end = x + 1;
            run.clear();

            if (c.cp < 0x80)
            {
                run.push_back(c.cp >= 0x20 ? static_cast<char>(c.cp) : ' ');
                while (end < n)
                {
                    const TermCell& d = cells[static_cast<std::size_t>(end)];
                    ImU32 f2, b2;
                    resolve(d, f2, b2);
                    if (d.cp >= 0x80 || f2 != fg || (d.flags & (kTermBold | kTermUnderline | kTermStrike)) != style ||
                        (d.flags & kTermWideTail))
                        break;
                    run.push_back(d.cp >= 0x20 ? static_cast<char>(d.cp) : ' ');
                    ++end;
                }
                if (run.find_first_not_of(' ') != std::string::npos)
                    dl->AddText(style & kTermBold ? bold : regular, fontSize, {origin.x + x * charW, y}, fg,
                                run.data(), run.data() + run.size());
            }
            else if (DrawCellGraphic(dl, c.cp, std::floor(origin.x + x * charW), std::floor(y), std::ceil(charW),
                                     std::ceil(charH), fg))
            {
                end = x + 1;
            }
            else
            {
                AppendUtf8(run, c.cp);
                const bool wide = (c.flags & kTermWide) != 0;
                end = x + (wide ? 2 : 1);
                ImFont* f = style & kTermBold ? bold : regular;
                float gx = origin.x + x * charW;
                if (wide)
                {
                    float adv = f->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, run.c_str()).x;
                    gx += std::max(0.0f, (2 * charW - adv) * 0.5f);
                }
                dl->AddText(f, fontSize, {gx, y}, fg, run.data(), run.data() + run.size());
            }

            if (style & kTermUnderline)
                dl->AddLine({origin.x + x * charW, y + charH - 1.5f}, {origin.x + end * charW, y + charH - 1.5f}, fg);
            if (style & kTermStrike)
                dl->AddLine({origin.x + x * charW, y + charH * 0.5f}, {origin.x + end * charW, y + charH * 0.5f}, fg);
            x = end;
        }
    }

    // Cursor (Claude Code's fullscreen renderer hides it and draws its own).
    if (Running() && snap.cursorVisible && snap.cursorY >= 0 && scrollBack_ == 0)
    {
        ImVec2 a = {origin.x + snap.cursorX * charW, origin.y + snap.cursorY * charH};
        ImVec2 b = {a.x + charW, a.y + charH};
        if (focused_)
        {
            const bool on = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
            dl->AddRectFilled(a, b, Col(theme::palette::Orange, on ? 0.85f : 0.35f));
        }
        else
        {
            dl->AddRect(a, b, Col(theme::palette::MidGray, 0.7f));
        }
    }
}

// ── input ───────────────────────────────────────────────────────────────
void TerminalPanel::HandleKeyboard()
{
    auto& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    const int  mod  = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0); // xterm modifier parameter
    std::string out;

    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    // App-level chords stay out of the PTY.
    if (ctrl && shift)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelection();
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard();
        return; // Ctrl+Shift+T/W/… are handled by the tab bar
    }
    if (shift && !ctrl && ImGui::IsKeyPressed(ImGuiKey_Insert, false))
    {
        PasteClipboard();
        return;
    }
    if (ctrl && (ImGui::IsKeyDown(ImGuiKey_PageUp) || ImGui::IsKeyDown(ImGuiKey_PageDown) ||
                 ImGui::IsKeyDown(ImGuiKey_Tab)))
        return; // tab switching

    // Local scrollback in the classic (non-alt-screen) renderer.
    if (shift && !ctrl && !alt)
    {
        int page = std::max(1, rows_ - 2);
        if (pressed(ImGuiKey_PageUp)) { scrollBack_ += page; return; }
        if (pressed(ImGuiKey_PageDown)) { scrollBack_ = std::max(0, scrollBack_ - page); return; }
    }

    auto csiMod = [&](const char* plain, char finalByte) {
        if (mod == 1) out += plain;
        else
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mod, finalByte);
            out += buf;
        }
    };
    auto tilde = [&](int code) {
        char buf[16];
        if (mod == 1) std::snprintf(buf, sizeof(buf), "\x1b[%d~", code);
        else std::snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, mod);
        out += buf;
    };
    const bool appCursor = term_.appCursorKeys();

    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) out += (shift || alt) ? "\x1b\r" : "\r";
    if (pressed(ImGuiKey_Backspace)) out += ctrl ? "\x17" : alt ? "\x1b\x7f" : "\x7f";
    if (pressed(ImGuiKey_Tab)) out += shift ? "\x1b[Z" : "\t";
    if (pressed(ImGuiKey_Escape)) out += "\x1b";
    if (pressed(ImGuiKey_UpArrow)) csiMod(appCursor ? "\x1bOA" : "\x1b[A", 'A');
    if (pressed(ImGuiKey_DownArrow)) csiMod(appCursor ? "\x1bOB" : "\x1b[B", 'B');
    if (pressed(ImGuiKey_RightArrow)) csiMod(appCursor ? "\x1bOC" : "\x1b[C", 'C');
    if (pressed(ImGuiKey_LeftArrow)) csiMod(appCursor ? "\x1bOD" : "\x1b[D", 'D');
    if (pressed(ImGuiKey_Home)) csiMod(appCursor ? "\x1bOH" : "\x1b[H", 'H');
    if (pressed(ImGuiKey_End)) csiMod(appCursor ? "\x1bOF" : "\x1b[F", 'F');
    if (pressed(ImGuiKey_Insert)) tilde(2);
    if (pressed(ImGuiKey_Delete)) tilde(3);
    if (pressed(ImGuiKey_PageUp)) tilde(5);
    if (pressed(ImGuiKey_PageDown)) tilde(6);
    if (pressed(ImGuiKey_F1)) csiMod("\x1bOP", 'P');
    if (pressed(ImGuiKey_F2)) csiMod("\x1bOQ", 'Q');
    if (pressed(ImGuiKey_F3)) csiMod("\x1bOR", 'R');
    if (pressed(ImGuiKey_F4)) csiMod("\x1bOS", 'S');
    static constexpr std::pair<ImGuiKey, int> kFn[] = {
        {ImGuiKey_F5, 15}, {ImGuiKey_F6, 17}, {ImGuiKey_F7, 18},  {ImGuiKey_F8, 19},
        {ImGuiKey_F9, 20}, {ImGuiKey_F10, 21}, {ImGuiKey_F11, 23}, {ImGuiKey_F12, 24},
    };
    for (auto [key, code] : kFn)
        if (pressed(key)) tilde(code);

    if (ctrl && !alt)
    {
        for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
            if (pressed(static_cast<ImGuiKey>(k))) out.push_back(static_cast<char>(1 + k - ImGuiKey_A));
        if (pressed(ImGuiKey_Space)) out.push_back('\0');
        if (pressed(ImGuiKey_LeftBracket)) out.push_back('\x1b');
        if (pressed(ImGuiKey_Backslash)) out.push_back('\x1c');
        if (pressed(ImGuiKey_RightBracket)) out.push_back('\x1d');
        if (pressed(ImGuiKey_Slash) || pressed(ImGuiKey_Minus)) out.push_back('\x1f');
    }
    else
    {
        for (ImWchar wc : io.InputQueueCharacters)
        {
            if (wc < 0x20 || wc == 0x7f) continue;
            if (alt) out.push_back('\x1b');
            AppendUtf8(out, wc);
        }
    }
    io.InputQueueCharacters.resize(0);

    if (!out.empty())
    {
        term_.sendInput(out);
        scrollBack_ = 0;
        selActive_ = false;
    }
}

void TerminalPanel::HandleMouse(const Terminal::Snapshot& snap, float charW, float charH, ImVec2 origin, bool hovered)
{
    auto& io = ImGui::GetIO();
    int col = std::clamp(static_cast<int>(std::floor((io.MousePos.x - origin.x) / charW)), 0, std::max(0, snap.cols - 1));
    int row = std::clamp(static_cast<int>(std::floor((io.MousePos.y - origin.y) / charH)), 0,
                         std::max(0, static_cast<int>(snap.rows.size()) - 1));

    // Shift bypasses the app's mouse reporting so text can still be selected locally.
    const bool appMouse = snap.mouseTracking && Running() && !io.KeyShift;
    Terminal::MouseEvent ev;
    ev.col = col;
    ev.row = row;
    ev.alt = io.KeyAlt;
    ev.ctrl = io.KeyCtrl;

    if (hovered && io.MouseWheel != 0.0f)
    {
        wheelAccum_ += io.MouseWheel;
        while (std::fabs(wheelAccum_) >= 1.0f)
        {
            const bool up = wheelAccum_ > 0;
            wheelAccum_ += up ? -1.0f : 1.0f;
            if (appMouse)
            {
                ev.action = up ? Terminal::MouseAction::WheelUp : Terminal::MouseAction::WheelDown;
                term_.sendMouse(ev);
            }
            else if (!snap.altScreen)
            {
                scrollBack_ = std::clamp(scrollBack_ + (up ? 3 : -3), 0, snap.maxScroll);
            }
            else if (Running())
            {
                term_.sendInput(up ? "\x1b[A" : "\x1b[B"); // alternate-scroll: arrows
            }
        }
    }
    else if (!hovered)
    {
        wheelAccum_ = 0.0f;
    }

    if (hovered && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)))
        wantFocus_ = true;

    if (appMouse || forwardedButtons_)
    {
        selecting_ = false;
        for (int b = 0; b < 3; ++b)
        {
            ev.button = b;
            if (hovered && appMouse && ImGui::IsMouseClicked(b))
            {
                ev.action = Terminal::MouseAction::Press;
                if (term_.sendMouse(ev)) forwardedButtons_ |= 1 << b;
            }
            if ((forwardedButtons_ & (1 << b)) && ImGui::IsMouseReleased(b))
            {
                ev.action = Terminal::MouseAction::Release;
                term_.sendMouse(ev);
                forwardedButtons_ &= ~(1 << b);
            }
        }
        if ((col != lastMouseCol_ || row != lastMouseRow_) && (hovered || forwardedButtons_))
        {
            ev.action = Terminal::MouseAction::Motion;
            ev.button = (forwardedButtons_ & 1) ? 0 : (forwardedButtons_ & 2) ? 1 : (forwardedButtons_ & 4) ? 2 : -1;
            term_.sendMouse(ev);
        }
    }
    else
    {
        GridPos pos{snap.firstAbsLine + row, col};
        if (hovered && ImGui::IsMouseClicked(0))
        {
            selecting_ = true;
            selActive_ = true;
            selA_ = selB_ = pos;
        }
        if (selecting_ && ImGui::IsMouseDown(0))
        {
            // Include the cell under the pointer when dragging forward.
            selB_ = Before(selA_.line, selA_.col, pos.line, pos.col) ? GridPos{pos.line, pos.col + 1} : pos;
        }
        if (selecting_ && ImGui::IsMouseReleased(0))
        {
            selecting_ = false;
            if (!HasSelection()) selActive_ = false;
        }
        if (hovered && ImGui::IsMouseClicked(1)) ImGui::OpenPopup("##term_ctx");
    }
    lastMouseCol_ = col;
    lastMouseRow_ = row;
}

void TerminalPanel::CopySelection()
{
    if (!HasSelection()) return;
    GridPos a = selA_, b = selB_;
    if (Before(b.line, b.col, a.line, a.col)) std::swap(a, b);
    std::string text = term_.textRange(a.line, a.col, b.line, b.col);
    if (!text.empty()) ImGui::SetClipboardText(text.c_str());
}

void TerminalPanel::PasteClipboard()
{
    if (!Running()) return;
    if (const char* text = ImGui::GetClipboardText(); text && *text)
    {
        term_.paste(text);
        scrollBack_ = 0;
    }
}
} // namespace ui
