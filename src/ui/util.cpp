#include "ui/util.hpp"
#include "config/paths.hpp"
#include "theme/theme.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <numbers>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ui
{
std::string ShellQuote(const std::string& s)
{
    if (!s.empty() && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./=:@%+,") ==
                          std::string::npos)
        return s;
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

void OpenWithDesktop(const std::filesystem::path& path)
{
    // posix_spawn rather than fork(): we're multi-threaded, so the child must
    // not run arbitrary code before exec.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    std::string arg = path.string();
    char* argv[] = {const_cast<char*>("xdg-open"), arg.data(), nullptr};
    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, "xdg-open", &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc == 0) std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach(); // reap without blocking the UI
}

std::string TildePath(const std::string& path)
{
    std::string home = config::HomeDir().string();
    if (!home.empty() && path.starts_with(home) && (path.size() == home.size() || path[home.size()] == '/'))
        return "~" + path.substr(home.size());
    return path;
}

std::string HumanTime(const std::string& iso)
{
    std::tm tm{};
    if (iso.size() < 19 || std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                       &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return iso;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    std::time_t t = ::timegm(&tm);
    std::tm local{};
    ::localtime_r(&t, &local);

    std::time_t now = std::time(nullptr);
    std::tm today{};
    ::localtime_r(&now, &today);

    static constexpr const char* kMonths[] = {"янв", "фев", "мар", "апр", "мая", "июн",
                                             "июл", "авг", "сен", "окт", "ноя", "дек"};
    char buf[64];
    auto dayIndex = [](const std::tm& x) { return x.tm_year * 400 + x.tm_yday; };
    int diff = dayIndex(today) - dayIndex(local);
    if (diff == 0) std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    else if (diff == 1) std::snprintf(buf, sizeof(buf), "вчера %02d:%02d", local.tm_hour, local.tm_min);
    else if (local.tm_year == today.tm_year)
        std::snprintf(buf, sizeof(buf), "%d %s %02d:%02d", local.tm_mday, kMonths[local.tm_mon], local.tm_hour,
                      local.tm_min);
    else std::snprintf(buf, sizeof(buf), "%d %s %d", local.tm_mday, kMonths[local.tm_mon], local.tm_year + 1900);
    return buf;
}

std::string HumanSize(std::uintmax_t bytes)
{
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu Б", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.0f КБ", bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f МБ", bytes / (1024.0 * 1024.0));
    return buf;
}

void StatusDot(ImU32 color, float radius)
{
    ImVec2 p   = ImGui::GetCursorScreenPos();
    float  h   = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + radius + 1.0f, p.y + h * 0.5f}, radius, color, 12);
    ImGui::Dummy({radius * 2.0f + 2.0f, h});
    ImGui::SameLine();
}

void SpinnerAt(ImDrawList* dl, ImVec2 centre, ImU32 color, float radius, float thickness)
{
    constexpr int   kSegments = 20;
    constexpr float kSweep    = std::numbers::pi_v<float> * 1.5f; // 3/4 turn, so it always reads as spinning, not a static ring
    const float      t        = static_cast<float>(ImGui::GetTime());

    dl->PathClear();
    for (int i = 0; i <= kSegments; ++i)
    {
        float a = t * 6.0f + (static_cast<float>(i) / kSegments) * kSweep;
        dl->PathLineTo({centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius});
    }
    dl->PathStroke(color, ImDrawFlags_None, thickness);
}

void Spinner(ImU32 color, float radius, float thickness)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = ImGui::GetTextLineHeight();
    SpinnerAt(ImGui::GetWindowDrawList(), {p.x + radius + 1.0f, p.y + h * 0.5f}, color, radius, thickness);
    ImGui::Dummy({radius * 2.0f + 2.0f, h});
    ImGui::SameLine();
}

void HelpMarker(const char* text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawToast(const char* text, ImU32 accent)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f, 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::palette::Dark);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(accent));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Light);
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - 16.0f, vp->WorkPos.y + 16.0f}, ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted(text);
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}
} // namespace ui
