#include "sessions/live_sessions.hpp"
#include "config/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <signal.h>

namespace sessions
{
namespace
{
std::filesystem::path RegistryDir()
{
    return config::ClaudeDir() / "sessions";
}

std::string ReadSmallFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > (1u << 20) ? std::string{} : s;
}

// Field 22 of /proc/<pid>/stat (process start time in clock ticks). The comm
// field may contain spaces or parens, so parse from the last ')'.
std::string ProcStartTime(pid_t pid)
{
    std::string stat = ReadSmallFile("/proc/" + std::to_string(pid) + "/stat");
    auto close = stat.rfind(')');
    if (close == std::string::npos) return {};
    std::istringstream rest(stat.substr(close + 1));
    std::string field;
    // After ')' comes field 3 (state); starttime is field 22 → the 20th token.
    for (int i = 3; i <= 22 && (rest >> field); ++i)
        if (i == 22) return field;
    return {};
}

bool IsAlive(pid_t pid, const std::string& procStart)
{
    if (pid <= 0) return false;
    if (::kill(pid, 0) != 0 && errno != EPERM) return false;
    if (procStart.empty()) return true;
    return ProcStartTime(pid) == procStart;
}

std::string Str(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::int64_t Int(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<std::int64_t>() : 0;
}
} // namespace

bool LiveSessionRegistry::Refresh()
{
    std::vector<LiveSession> next;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(RegistryDir(), ec))
    {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || entry.path().extension() != ".json") continue;

        auto j = nlohmann::json::parse(ReadSmallFile(entry.path()), nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;

        LiveSession s;
        s.pid = static_cast<pid_t>(Int(j, "pid"));
        if (!IsAlive(s.pid, Str(j, "procStart"))) continue;
        s.sessionId   = Str(j, "sessionId");
        s.cwd         = Str(j, "cwd");
        s.name        = Str(j, "name");
        s.status      = Str(j, "status");
        s.waitingFor  = Str(j, "waitingFor");
        s.kind        = Str(j, "kind");
        s.version     = Str(j, "version");
        s.startedAtMs = Int(j, "startedAt");
        s.updatedAtMs = Int(j, "updatedAt");
        next.push_back(std::move(s));
    }
    std::sort(next.begin(), next.end(), [](const LiveSession& a, const LiveSession& b) { return a.pid < b.pid; });

    auto same = [](const LiveSession& a, const LiveSession& b) {
        return a.pid == b.pid && a.sessionId == b.sessionId && a.status == b.status &&
               a.waitingFor == b.waitingFor && a.name == b.name;
    };
    bool changed = next.size() != live_.size() || !std::equal(next.begin(), next.end(), live_.begin(), same);
    live_ = std::move(next);
    return changed;
}

const LiveSession* LiveSessionRegistry::FindBySession(const std::string& sessionId) const
{
    if (sessionId.empty()) return nullptr;
    for (const auto& s : live_)
        if (s.sessionId == sessionId) return &s;
    return nullptr;
}

const LiveSession* LiveSessionRegistry::FindByPid(pid_t pid) const
{
    for (const auto& s : live_)
        if (s.pid == pid) return &s;
    return nullptr;
}
} // namespace sessions
