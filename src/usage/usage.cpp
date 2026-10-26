#include "usage/usage.hpp"
#include "config/paths.hpp"
#include "net/egress.hpp"
#include "term/session_env.hpp"
#include "term/terminal.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace usage
{
namespace fs = std::filesystem;

namespace
{
constexpr std::int64_t kAutoRefreshAgeMs   = 5 * 60 * 1000; // Claude Code itself distrusts its cache after 1 h
constexpr std::int64_t kRetryAfterMs       = 2 * 60 * 1000; // between attempts, so a failing one doesn't spin
constexpr std::int64_t kNoDataRetryMs      = 30 * 60 * 1000; // …and after one that worked but left no plan data (API-key accounts)
constexpr int          kRefreshTimeoutSecs = 60;
constexpr std::int64_t kStaleFileAgeSecs   = 24 * 3600;

const Json* Obj(const Json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
}

std::optional<double> Num(const Json& j, const char* key)
{
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<double>();
    return std::nullopt;
}

std::string Str(const Json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// Claude Code's money is a struct {amount_minor, currency, exponent} in the
// newer fields, and a bare number of minor units in the older ones.
std::optional<Money> ParseMoney(const Json& j, const Money& fallback)
{
    Money m = fallback;
    if (j.is_number())
    {
        m.minor = static_cast<std::int64_t>(std::llround(j.get<double>()));
        return m;
    }
    if (!j.is_object()) return std::nullopt;
    auto minor = Num(j, "amount_minor");
    if (!minor) return std::nullopt;
    m.minor = static_cast<std::int64_t>(std::llround(*minor));
    if (auto cur = Str(j, "currency"); !cur.empty()) m.currency = cur;
    if (auto exp = Num(j, "exponent")) m.exponent = static_cast<int>(*exp);
    return m;
}

int ParseSeverity(const std::string& s)
{
    if (s == "normal") return 0;
    if (s == "warning") return 1;
    if (s == "critical" || s == "danger" || s == "error") return 2;
    return -1;
}

std::optional<Window> ParseWindow(const Json& j, std::string label)
{
    if (!j.is_object()) return std::nullopt;
    auto pct = Num(j, "utilization");
    if (!pct) return std::nullopt;
    Window w;
    w.label      = std::move(label);
    w.usedPct    = std::clamp(*pct, 0.0, 100.0);
    w.resetsAtMs = ParseIso8601Ms(Str(j, "resets_at"));
    return w;
}

// The status line reports resets_at as unix seconds.
std::optional<Window> ParseStatusWindow(const Json& root, const char* key)
{
    const Json* w = Obj(root, key);
    if (!w) return std::nullopt;
    auto pct = Num(*w, "used_percentage");
    if (!pct) return std::nullopt;
    Window out;
    out.usedPct = std::clamp(*pct, 0.0, 100.0);
    if (auto at = Num(*w, "resets_at")) out.resetsAtMs = static_cast<std::int64_t>(*at * (*at > 1e12 ? 1.0 : 1000.0));
    return out;
}

fs::path StateDir()
{
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) return fs::path(xdg) / "claudeui";
    return config::HomeDir() / ".local" / "state" / "claudeui";
}

std::optional<std::string> ReadWhole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Does any settings file that would apply to a session in `cwd` define a status line?
bool HasStatusLine(const fs::path& cwd)
{
    std::vector<fs::path> files = {config::ClaudeDir() / "settings.json"};
    if (!cwd.empty())
    {
        files.push_back(cwd / ".claude" / "settings.json");
        files.push_back(cwd / ".claude" / "settings.local.json");
    }
    for (const auto& f : files)
    {
        auto text = ReadWhole(f);
        if (!text) continue;
        auto j = Json::parse(*text, nullptr, false);
        if (j.is_object() && j.contains("statusLine")) return true;
    }
    return false;
}

std::int64_t StatMtimeNs(const fs::path& p)
{
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

// Copies the JSON on stdin into $CLAUDEUI_USAGE_FILE (atomically — a rename, so
// a reader never sees half a file) and prints nothing, so the status line
// stays empty. `$$` keeps two overlapping runs from sharing a temp file.
constexpr const char* kStatusLineCommand =
    "f=$CLAUDEUI_USAGE_FILE; [ -n \"$f\" ] && cat >\"$f.$$\" && mv -f \"$f.$$\" \"$f\"; true";
} // namespace

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ── data types ──────────────────────────────────────────────────────────
double Money::Amount() const
{
    return static_cast<double>(minor) / std::pow(10.0, exponent);
}

std::string Money::Format() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", std::max(0, exponent), Amount());
    if (currency == "USD") return std::string("$") + buf;
    return std::string(buf) + " " + currency;
}

std::int64_t ParseIso8601Ms(std::string_view iso)
{
    if (iso.empty()) return 0;
    std::string s(iso);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, n = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6) return 0;
    const char* rest = s.c_str() + n;
    double frac = 0.0;
    if (*rest == '.')
    {
        char* end = nullptr;
        frac = std::strtod(rest, &end); // ".668946"
        rest = end;
    }
    long offsetSecs = 0;
    if (*rest == '+' || *rest == '-')
    {
        int oh = 0, om = 0;
        if (std::sscanf(rest + 1, "%d:%d", &oh, &om) >= 1) offsetSecs = (oh * 3600L + om * 60L) * (*rest == '-' ? -1 : 1);
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = sec;
    const std::time_t t = ::timegm(&tm);
    if (t == static_cast<std::time_t>(-1)) return 0;
    return (static_cast<std::int64_t>(t) - offsetSecs) * 1000 + static_cast<std::int64_t>(std::llround(frac * 1000.0));
}

AccountUsage ParseUsageCache(const Json& cfg)
{
    AccountUsage out;
    const Json* cache = Obj(cfg, "cachedUsageUtilization");
    if (!cache) return out;
    const Json* u = Obj(*cache, "utilization");
    if (!u) return out;

    if (auto at = Num(*cache, "fetchedAtMs")) out.fetchedAtMs = static_cast<std::int64_t>(*at);

    if (auto it = u->find("five_hour"); it != u->end()) out.fiveHour = ParseWindow(*it, "");
    if (auto it = u->find("seven_day"); it != u->end()) out.sevenDay = ParseWindow(*it, "");

    // Per-model weekly windows: the server's limits[] list, or the older fixed keys.
    if (auto it = u->find("limits"); it != u->end() && it->is_array())
    {
        for (const auto& l : *it)
        {
            if (!l.is_object() || Str(l, "kind") != "weekly_scoped") continue;
            auto pct = Num(l, "percent");
            if (!pct) continue;
            Window w;
            w.usedPct    = std::clamp(*pct, 0.0, 100.0);
            w.resetsAtMs = ParseIso8601Ms(Str(l, "resets_at"));
            w.severity   = ParseSeverity(Str(l, "severity"));
            if (const Json* scope = Obj(l, "scope"))
                if (const Json* model = Obj(*scope, "model")) w.label = Str(*model, "display_name");
            if (w.label.empty()) w.label = "по модели";
            out.modelWeekly.push_back(std::move(w));
        }
    }
    else
    {
        for (auto [key, label] : {std::pair{"seven_day_opus", "Opus"}, std::pair{"seven_day_sonnet", "Sonnet"}})
            if (auto it = u->find(key); it != u->end())
                if (auto w = ParseWindow(*it, label)) out.modelWeekly.push_back(*w);
    }

    // The server's verdict on the two main windows lives in limits[] only.
    if (auto it = u->find("limits"); it != u->end() && it->is_array())
        for (const auto& l : *it)
        {
            if (!l.is_object()) continue;
            const std::string kind = Str(l, "kind");
            if (kind == "session" && out.fiveHour) out.fiveHour->severity = ParseSeverity(Str(l, "severity"));
            else if (kind == "weekly_all" && out.sevenDay) out.sevenDay->severity = ParseSeverity(Str(l, "severity"));
        }

    if (const Json* bd = Obj(*u, "seven_day_breakdown"))
        if (auto it = bd->find("rows"); it != bd->end() && it->is_array())
            for (const auto& r : *it)
            {
                if (!r.is_object()) continue;
                auto pct = Num(r, "percent");
                if (!pct) continue;
                out.products.push_back({Str(r, "key"), Str(r, "display_name"), std::clamp(*pct, 0.0, 100.0)});
            }

    // Extra usage. `spend` is the newer, fuller shape; `extra_usage` the older one.
    if (const Json* spend = Obj(*u, "spend"))
    {
        Money base;
        if (auto it = spend->find("used"); it != spend->end())
            if (auto m = ParseMoney(*it, base))
            {
                out.extra.used = m;
                base           = *m;
            }
        for (auto [key, slot] : {std::pair{"limit", &out.extra.limit}, std::pair{"balance", &out.extra.balance}})
            if (auto it = spend->find(key); it != spend->end()) *slot = ParseMoney(*it, base);
        if (!out.extra.limit)
            if (auto it = spend->find("cap"); it != spend->end()) out.extra.limit = ParseMoney(*it, base);
        if (auto it = spend->find("enabled"); it != spend->end() && it->is_boolean()) out.extra.enabled = it->get<bool>();
    }
    else if (const Json* extra = Obj(*u, "extra_usage"))
    {
        Money base;
        if (auto cur = Str(*extra, "currency"); !cur.empty()) base.currency = cur;
        if (auto dp = Num(*extra, "decimal_places")) base.exponent = static_cast<int>(*dp);
        if (auto it = extra->find("used_credits"); it != extra->end()) out.extra.used = ParseMoney(*it, base);
        if (auto it = extra->find("monthly_limit"); it != extra->end()) out.extra.limit = ParseMoney(*it, base);
        if (auto it = extra->find("is_enabled"); it != extra->end() && it->is_boolean()) out.extra.enabled = it->get<bool>();
    }

    out.valid = out.fiveHour || out.sevenDay || out.extra.used;
    return out;
}

std::optional<SessionUsage> ParseStatusLine(std::string_view json)
{
    Json root = Json::parse(json, nullptr, false);
    if (!root.is_object()) return std::nullopt;

    SessionUsage out;
    out.sessionId = Str(root, "session_id");
    if (const Json* cost = Obj(root, "cost"))
        if (auto usd = Num(*cost, "total_cost_usd")) out.costUsd = *usd;
    if (const Json* model = Obj(root, "model"))
    {
        out.model   = Str(*model, "display_name");
        out.modelId = Str(*model, "id");
    }
    if (const Json* effort = Obj(root, "effort")) out.effort = Str(*effort, "level");
    if (const Json* limits = Obj(root, "rate_limits"))
    {
        out.fiveHour = ParseStatusWindow(*limits, "five_hour");
        out.sevenDay = ParseStatusWindow(*limits, "seven_day");
    }
    return out;
}

// ── Monitor ─────────────────────────────────────────────────────────────
Monitor::Monitor()
{
    dir_      = StateDir() / "usage";
    probeDir_ = StateDir() / "probe";
    pid_      = static_cast<long>(::getpid());

    // Sidecars of ClaudeUI processes that died without cleaning up.
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const auto cutoff = std::chrono::system_clock::now() - std::chrono::seconds(kStaleFileAgeSecs);
    for (const auto& e : fs::directory_iterator(dir_, ec))
    {
        std::error_code tec;
        const auto t = e.last_write_time(tec);
        if (tec) continue;
        if (std::chrono::clock_cast<std::chrono::system_clock>(t) < cutoff) fs::remove(e.path(), tec);
    }
}

Monitor::~Monitor()
{
    Shutdown();
}

void Monitor::Shutdown()
{
    stop_ = true;
    std::thread worker;
    {
        std::lock_guard lock(mu_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    // This process's sidecars are meaningless once it is gone.
    std::error_code ec;
    const std::string prefix = std::to_string(pid_) + "-";
    for (const auto& e : fs::directory_iterator(dir_, ec))
        if (e.path().filename().string().starts_with(prefix)) fs::remove(e.path(), ec);
}

fs::path Monitor::SidecarPath(std::uint64_t tabUid) const
{
    return dir_ / (std::to_string(pid_) + "-" + std::to_string(tabUid) + ".json");
}

Monitor::LaunchExtras Monitor::ForLaunch(std::uint64_t tabUid, const fs::path& cwd)
{
    LaunchExtras out;
    std::error_code ec;
    fs::remove(SidecarPath(tabUid), ec); // a relaunched tab must not show its previous run's numbers
    sidecars_.erase(tabUid);
    if (HasStatusLine(cwd)) return out;

    Json settings = {{"statusLine", {{"type", "command"}, {"command", kStatusLineCommand}}}};
    out.args      = {"--settings", settings.dump()};
    out.env       = {{"CLAUDEUI_USAGE_FILE", SidecarPath(tabUid).string()}};
    return out;
}

void Monitor::PollSessions(const std::vector<std::uint64_t>& tabUids)
{
    for (auto it = sidecars_.begin(); it != sidecars_.end();)
        it = std::find(tabUids.begin(), tabUids.end(), it->first) == tabUids.end() ? sidecars_.erase(it) : std::next(it);

    for (std::uint64_t uid : tabUids)
    {
        const fs::path path = SidecarPath(uid);
        const std::int64_t mtime = StatMtimeNs(path);
        if (mtime < 0) continue;
        Sidecar& sc = sidecars_[uid];
        if (sc.has && sc.mtimeNs == mtime) continue;
        auto text = ReadWhole(path);
        if (!text) continue;
        auto parsed = ParseStatusLine(*text);
        if (!parsed) continue; // caught mid-write or not ours: keep the last good reading
        parsed->updatedAtMs = mtime / 1'000'000;
        sc.data             = std::move(*parsed);
        sc.mtimeNs          = mtime;
        sc.has              = true;
    }
}

const SessionUsage* Monitor::Session(std::uint64_t tabUid) const
{
    auto it = sidecars_.find(tabUid);
    return it != sidecars_.end() && it->second.has ? &it->second.data : nullptr;
}

void Monitor::SetGlobalConfig(const Json& globalConfig)
{
    account_ = ParseUsageCache(globalConfig);
}

AccountUsage Monitor::Account() const
{
    AccountUsage a = account_;
    const SessionUsage* freshest = nullptr;
    for (const auto& [uid, sc] : sidecars_)
    {
        if (!sc.has || !(sc.data.fiveHour || sc.data.sevenDay)) continue;
        if (sc.data.updatedAtMs <= a.fetchedAtMs) continue;
        if (!freshest || sc.data.updatedAtMs > freshest->updatedAtMs) freshest = &sc.data;
    }
    if (freshest)
    {
        if (freshest->fiveHour) a.fiveHour = freshest->fiveHour;
        if (freshest->sevenDay) a.sevenDay = freshest->sevenDay;
        a.liveWindowsAtMs = freshest->updatedAtMs;
        a.valid           = true;
    }
    return a;
}

Monitor::RefreshState Monitor::State() const
{
    std::lock_guard lock(mu_);
    return state_;
}

std::string Monitor::Error() const
{
    std::lock_guard lock(mu_);
    return error_;
}

std::int64_t Monitor::LastOkMs() const
{
    std::lock_guard lock(mu_);
    return lastOkMs_;
}

void Monitor::Finish(bool ok, std::string error)
{
    std::lock_guard lock(mu_);
    state_ = ok ? RefreshState::Idle : RefreshState::Failed;
    error_ = std::move(error);
    if (ok) lastOkMs_ = NowMs();
}

void Monitor::RequestRefresh()
{
    std::lock_guard lock(mu_);
    if (state_ == RefreshState::Running) return;
    if (worker_.joinable()) worker_.join(); // the previous run has finished
    state_         = RefreshState::Running;
    error_.clear();
    lastAttemptMs_ = NowMs();
    worker_        = std::thread([this] { RunRefresh(); });
}

void Monitor::AutoRefresh(bool allowed, bool windowFocused)
{
    if (!allowed || !windowFocused) return;
    const std::int64_t now = NowMs();
    {
        std::lock_guard lock(mu_);
        if (state_ == RefreshState::Running || now - lastAttemptMs_ < kRetryAfterMs) return;
        if (!account_.valid && lastOkMs_ > 0 && now - lastOkMs_ < kNoDataRetryMs) return;
    }
    if (account_.valid && now - account_.fetchedAtMs < kAutoRefreshAgeMs) return;
    RequestRefresh();
}

// Runs `claude -p /usage` — Claude Code's own local command: it asks the server
// with Claude Code's credentials, prints the result and stores the raw answer
// in ~/.claude.json. No model call is made, so it costs no tokens.
void Monitor::RunRefresh()
{
    // Same rule as launching a session: never talk to the API from a Russian exit.
    auto check = net::Guard().Check(false);
    while (check.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready)
        if (stop_) return Finish(false, {});
    if (check.get().Blocks()) return Finish(false, "выход VPN в России — запрос к Claude не отправлялся");

    auto exe = Terminal::FindExecutable("claude");
    if (!exe) return Finish(false, "не найден исполняемый файл claude");

    std::error_code ec;
    fs::create_directories(probeDir_, ec);

    // Same environment rules as a session: no parent-session variables, and
    // the proxy chosen in Settings → ClaudeUI.
    const net::ProxyEnv proxy = net::SessionProxyEnv();
    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e)
    {
        std::string_view kv(*e);
        const std::string_view key = kv.substr(0, kv.find('='));
        if (std::find(kInheritedSessionEnv.begin(), kInheritedSessionEnv.end(), key) != kInheritedSessionEnv.end()) continue;
        if (std::find(proxy.unset.begin(), proxy.unset.end(), key) != proxy.unset.end()) continue;
        envStrings.emplace_back(kv);
    }
    for (const auto& [k, v] : proxy.set) envStrings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> argvStrings = {exe->string(), "-p", "/usage", "--output-format", "json",
                                            "--no-session-persistence"};
    std::vector<char*> argv;
    for (auto& s : argvStrings) argv.push_back(s.data());
    argv.push_back(nullptr);

    int out[2];
    if (::pipe2(out, O_CLOEXEC) != 0) return Finish(false, std::string("pipe: ") + std::strerror(errno));

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, probeDir_.c_str());
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID); // no controlling terminal: it must not touch ours

    pid_t child = -1;
    const int rc = ::posix_spawn(&child, exe->c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(out[1]);
    if (rc != 0)
    {
        ::close(out[0]);
        return Finish(false, std::string("не удалось запустить claude: ") + std::strerror(rc));
    }

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRefreshTimeoutSecs);
    bool timedOut       = false;
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (stop_) break;
        if (left.count() <= 0)
        {
            timedOut = true;
            break;
        }
        pollfd pfd{out[0], POLLIN, 0};
        const int n = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(left.count(), 1000)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) continue;
        char buf[4096];
        const ssize_t got = ::read(out[0], buf, sizeof(buf));
        if (got > 0) output.append(buf, static_cast<std::size_t>(got));
        else if (got == 0 || errno != EINTR) break;
        if (output.size() > (1u << 20)) break; // it prints a page of text, not a megabyte
    }
    ::close(out[0]);
    if (timedOut || stop_ || output.size() > (1u << 20)) ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (stop_) return Finish(false, {});

    if (timedOut) return Finish(false, "claude -p /usage не ответил за " + std::to_string(kRefreshTimeoutSecs) + " с");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return Finish(false, "claude -p /usage завершился с ошибкой (код " +
                                 std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");

    Json j = Json::parse(output, nullptr, false);
    if (!j.is_object()) return Finish(false, "claude -p /usage вернул не JSON");
    if (auto it = j.find("is_error"); it != j.end() && it->is_boolean() && it->get<bool>())
        return Finish(false, "claude: " + Str(j, "result"));
    Finish(true, {});
}

Monitor& Global()
{
    static Monitor monitor;
    return monitor;
}
} // namespace usage
