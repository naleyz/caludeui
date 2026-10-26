#include "config/app_settings.hpp"
#include "config/model_access.hpp"
#include "config/model_catalog.hpp"
#include "config/paths.hpp"
#include "config/settings_store.hpp"
#include "net/route.hpp"
#include "renderer/vulkan_context.hpp"
#include "sessions/live_sessions.hpp"
#include "sessions/session_scanner.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/rail.hpp"
#include "ui/layout_panel.hpp"
#include "ui/new_session_dialog.hpp"
#include "ui/session_list_panel.hpp"
#include "ui/session_tabs.hpp"
#include "ui/settings_panel.hpp"
#include "ui/stats_view.hpp"
#include "ui/usage_panel.hpp"
#include "ui/util.hpp"
#include "usage/model_usage.hpp"
#include "usage/usage.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
void FramebufferResizeCallback(GLFWwindow* window, int, int)
{
    auto* vk = static_cast<VulkanContext*>(glfwGetWindowUserPointer(window));
    vk->onResize();
}

std::string BaseName(const std::filesystem::path& p)
{
    auto name = p.filename().string();
    return name.empty() ? p.string() : name;
}

enum class QuitState { None, Asking, WaitingForExit, Done };

#ifdef CLAUDEUI_DEBUG
// Debug builds only: CLAUDEUI_TEST_INPUT="wait:6;text:/help;key:Escape;..." feeds
// synthetic events through ImGui's input queue — the same path real keys take
// — so focus and key routing into the terminal can be checked headlessly.
class InputScript
{
public:
    explicit InputScript(const char* spec)
    {
        std::string s = spec ? spec : "";
        std::size_t pos = 0;
        while (pos < s.size())
        {
            auto end = s.find(';', pos);
            std::string step = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!step.empty()) steps_.push_back(step);
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }

    void Tick(ImGuiIO& io, double now)
    {
        if (releaseMouse_)
        {
            io.AddMouseButtonEvent(releaseMouseButton_, false);
            releaseMouse_ = false;
            return;
        }
        if (releaseKey_ != ImGuiKey_None)
        {
            io.AddKeyEvent(releaseKey_, false);
            if (releaseShift_) io.AddKeyEvent(ImGuiMod_Shift, false);
            if (releaseCtrl_) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            releaseKey_ = ImGuiKey_None;
            return;
        }
        if (next_ >= steps_.size() || now < resumeAt_) return;
        const std::string step = steps_[next_++];
        std::fprintf(stderr, "[input-script] %.2f %s\n", now, step.c_str());
        if (step.starts_with("wait:")) resumeAt_ = now + std::atof(step.c_str() + 5);
        else if (step.starts_with("text:")) io.AddInputCharactersUTF8(step.c_str() + 5);
        else if (step.starts_with("key:"))
        {
            std::string name = step.substr(4);
            releaseShift_ = name.starts_with("Shift+");
            if (releaseShift_) name = name.substr(6);
            releaseCtrl_ = name.starts_with("Ctrl+");
            if (releaseCtrl_) name = name.substr(5);
            ImGuiKey key = name == "Enter" ? ImGuiKey_Enter : name == "Escape" ? ImGuiKey_Escape
                         : name == "Tab" ? ImGuiKey_Tab : name == "Backspace" ? ImGuiKey_Backspace
                         : name == "Up" ? ImGuiKey_UpArrow : name == "Down" ? ImGuiKey_DownArrow
                         : name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z'
                             ? static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'))
                             : ImGuiKey_None;
            if (key == ImGuiKey_None) return;
            if (releaseShift_) io.AddKeyEvent(ImGuiMod_Shift, true);
            if (releaseCtrl_) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(key, true);
            releaseKey_ = key;
        }
        else if (step.starts_with("cmd:") && onCommand)
        {
            onCommand(step.substr(4));
        }
        else if (step.starts_with("wheel:")) io.AddMouseWheelEvent(0.0f, static_cast<float>(std::atof(step.c_str() + 6)));
        else if (step.starts_with("mouse:"))
        {
            float x = 0, y = 0;
            std::sscanf(step.c_str() + 6, "%f,%f", &x, &y);
            io.AddMousePosEvent(x, y);
        }
        else if (step == "click" || step == "rclick")
        {
            releaseMouseButton_ = step == "rclick" ? 1 : 0;
            io.AddMouseButtonEvent(releaseMouseButton_, true);
            releaseMouse_ = true;
        }
        // Press and release apart, for dragging a slider between them.
        else if (step == "down") io.AddMouseButtonEvent(0, true);
        else if (step == "up") io.AddMouseButtonEvent(0, false);
    }

private:
    std::vector<std::string> steps_;
    std::size_t next_ = 0;
    double resumeAt_ = 0.0;
    ImGuiKey releaseKey_ = ImGuiKey_None;
    bool releaseShift_ = false;
    bool releaseCtrl_ = false;
    bool releaseMouse_ = false;
    int  releaseMouseButton_ = 0;

public:
    std::function<void(const std::string&)> onCommand; // "cmd:NAME" steps
};
#endif

struct CommandLine
{
    std::vector<std::string> newDirs;   // --new DIR (repeatable)
    std::vector<std::string> resumeIds; // --resume ID (repeatable)
    bool showSettings = false;          // --settings
};

// Returns -1 to continue, otherwise the process exit code.
// Hands Settings → ClaudeUI → proxy to the network layer, which every launch,
// VPN check and usage probe reads from.
void ApplySessionProxy(const config::ProxySetting& p)
{
    net::SessionProxy sp;
    sp.mode    = p.mode == config::ProxySetting::Mode::Custom ? net::ProxyMode::Custom
                 : p.mode == config::ProxySetting::Mode::None ? net::ProxyMode::None
                                                              : net::ProxyMode::System;
    sp.url     = p.url;
    sp.noProxy = p.noProxy;
    // A custom proxy that isn't usable yet (being typed) falls back to the environment.
    if (sp.mode == net::ProxyMode::Custom && !net::CheckProxyUrl(sp.url).empty()) sp.mode = net::ProxyMode::System;
    net::SetSessionProxy(std::move(sp));
}

int ParseArgs(int argc, char** argv, CommandLine& out)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if ((a == "--new" || a == "--resume") && i + 1 < argc)
        {
            std::string v = argv[++i];
            if (a == "--new")
            {
                std::error_code ec;
                auto abs = std::filesystem::absolute(v, ec);
                out.newDirs.push_back(ec ? v : abs.lexically_normal().string());
            }
            else out.resumeIds.push_back(v);
        }
        else if (a == "--settings") out.showSettings = true;
        else
        {
            std::printf("usage: claudeui [--new DIR]... [--resume SESSION_ID]... [--settings]\n"
                        "  --new DIR        open a new Claude Code session in DIR\n"
                        "  --resume ID      resume a session by id\n"
                        "  --settings       start on the settings tab\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    return -1;
}
} // namespace

int main(int argc, char** argv)
{
    CommandLine cli;
    if (int code = ParseArgs(argc, argv, cli); code >= 0) return code;

    // Under KWin, libdecor ignores requests to hide window decorations —
    // plain xdg-shell honours them. We keep OS decorations (no custom
    // titlebar here), but disabling libdecor sidesteps that compositor's
    // client-side-decoration quirks regardless.
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);

    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "ClaudeUI");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "ClaudeUI");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "ClaudeUI");

    GLFWwindow* window = glfwCreateWindow(1440, 900, "ClaudeUI", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 900, 560, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // Declared before `vk` so it is destroyed after it: the swapchain and surface
    // must go while the window and the display connection still exist.
    struct GlfwGuard
    {
        GLFWwindow* w;
        ~GlfwGuard()
        {
            glfwDestroyWindow(w);
            glfwTerminate();
        }
    } glfwGuard{window};

    VulkanContext vk(window);
    glfwSetWindowUserPointer(window, &vk);
    glfwSetFramebufferSizeCallback(window, FramebufferResizeCallback);

    // ── ImGui bring-up ───────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "claudeui_layout.ini";

    float xscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, nullptr);
    // Read before the fonts are built: the terminal face is one of them.
    config::AppSettingsStore appSettings;
    ui::fonts::Load(xscale, appSettings.Get().terminalFont);
    ApplySessionProxy(appSettings.Get().sessionProxy); // before any tab (e.g. --new) launches

    theme::Apply(theme::Mode::Dark);

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion                   = VK_API_VERSION_1_3;
    initInfo.Instance                     = vk.instance();
    initInfo.PhysicalDevice               = vk.physDevice();
    initInfo.Device                       = vk.device();
    initInfo.QueueFamily                  = vk.graphicsFamily();
    initInfo.Queue                        = vk.graphicsQueue();
    initInfo.DescriptorPool               = vk.imguiPool();
    initInfo.MinImageCount                = vk.minImageCount();
    initInfo.ImageCount                   = vk.imageCount();
    initInfo.PipelineInfoMain.RenderPass  = vk.renderPass();
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&initInfo);

    // ── App state ────────────────────────────────────────────────────────
    config::SettingsStore          settings;
    sessions::SessionScanner       scanner;
    sessions::LiveSessionRegistry  live;
    scanner.Rescan();
    live.Refresh();

    // The installed claude's model registry takes ~0.2 s to scan; show the
    // built-in list until it's in.
    config::ModelCatalog modelCatalog = config::BuiltinModelCatalog();
    std::future<config::ModelCatalog> modelCatalogScan = std::async(std::launch::async, config::LoadModelCatalog);

    ui::SessionTabs      tabs;
    ui::SessionListPanel sessionList;
    ui::UsagePanel       usagePanel;
    // Refreshed every frame from the already-loaded config, so it follows
    // Claude Code rewriting its model cache in ~/.claude.json.
    config::ModelAccess modelAccess;
    ui::SettingsPanel    settingsPanel(settings, appSettings, modelCatalog, modelAccess);
    ui::LayoutPanel       layoutPanel(appSettings);
    ui::NewSessionDialog newSession;

    bool rescanRequested = false;
    auto findSession = [&](const std::string& id) -> const sessions::SessionInfo* {
        for (const auto& s : scanner.Sessions())
            if (s.id == id) return &s;
        return nullptr;
    };

    tabs.onNewSession    = [&] { newSession.Open(); };
    tabs.onSessionExited = [&] { rescanRequested = true; };

    sessionList.SetCallbacks({
        .open =
            [&](const sessions::SessionInfo& s) {
                ui::LaunchSpec spec;
                spec.cwd      = s.cwd.empty() ? config::HomeDir() : std::filesystem::path(s.cwd);
                spec.resumeId = s.id;
                std::string title = s.DisplayName().empty() ? s.id.substr(0, 8) : s.DisplayName();
                tabs.Open(std::move(spec), std::move(title));
            },
        .newInDir = [&](const std::string& cwd) { newSession.Open(cwd); },
        .continueIn =
            [&](const std::string& cwd) {
                ui::LaunchSpec spec;
                spec.cwd          = cwd;
                spec.continueLast = true;
                tabs.Open(std::move(spec), BaseName(cwd) + " (continue)");
            },
        .rescan     = [&] { rescanRequested = true; },
        .newSession = [&] { newSession.Open(); },
    });

    for (const auto& id : cli.resumeIds)
    {
        if (const auto* s = findSession(id))
        {
            ui::LaunchSpec spec;
            spec.cwd      = s->cwd.empty() ? config::HomeDir() : std::filesystem::path(s->cwd);
            spec.resumeId = s->id;
            tabs.Open(std::move(spec), s->DisplayName().empty() ? s->id.substr(0, 8) : s->DisplayName());
        }
        else std::fprintf(stderr, "claudeui: session %s not found under ~/.claude/projects\n", id.c_str());
    }
    for (const auto& dir : cli.newDirs)
    {
        ui::LaunchSpec spec;
        spec.cwd = dir;
        tabs.Open(std::move(spec), BaseName(dir));
    }
    bool selectSettingsTab = cli.showSettings;
    ui::Section section    = ui::Section::Sessions;
    ui::Rail    rail;
    int         lastTabCount = 0;
    usage::TranscriptUsage transcripts; // per-model tokens for the statistics view
    transcripts.Refresh();              // background read, so the chart is ready when statistics open
    ui::StatsView          statsView;

    // ── Main loop ────────────────────────────────────────────────────────
    QuitState quit            = QuitState::None;
    double    quitDeadline    = 0.0;
    double    lastFrame       = 0.0;
    double    nextLiveRefresh = 0.0;
    double    nextRescan      = 0.0;
    double    nextUsagePoll   = 0.0;
    std::uint64_t usageConfigRevision = ~0ull; // forces the first read of ~/.claude.json's usage cache
    std::uint64_t appliedSettingsRevision = appSettings.Revision();
    bool      terminalHadFocus = false;
    std::string windowTitle;
    std::string vpnAlertText;   // set once when a running session's VPN exit turns Russia
    double      vpnAlertUntil = 0.0;
    float       lastUsageBoxH = 0.0f; // last measured height of the "usage" panel's wrapper (see below)
#ifdef CLAUDEUI_DEBUG
    InputScript inputScript(std::getenv("CLAUDEUI_TEST_INPUT"));
    inputScript.onCommand = [&](const std::string& cmd) {
        if (cmd == "exit-all") tabs.ExitAll();
        else if (cmd == "close-window") glfwSetWindowShouldClose(window, GLFW_TRUE);
    };
#endif

    while (quit != QuitState::Done)
    {
        // Frame pacing: the swapchain prefers MAILBOX (no vsync cap), so wait
        // for events up to a frame budget instead of spinning a core. Terminal
        // output still shows up within one budget.
        const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED);
        const bool focusedWin = glfwGetWindowAttrib(window, GLFW_FOCUSED);
        const double budget = iconified ? 0.25 : focusedWin ? 1.0 / 60.0 : 1.0 / 20.0;
        const double wait   = lastFrame + budget - glfwGetTime();
        if (wait > 0) glfwWaitEventsTimeout(wait);
        else glfwPollEvents();
        lastFrame = glfwGetTime();

        if (glfwWindowShouldClose(window))
        {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            if (tabs.RunningCount() == 0) break;
            if (quit == QuitState::None) quit = QuitState::Asking;
        }

        const double now = glfwGetTime();
        if (modelCatalogScan.valid() &&
            modelCatalogScan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            modelCatalog = modelCatalogScan.get();
        settings.PollExternalChanges();
        modelAccess = config::ReadModelAccess(settings.Data(config::Store::GlobalConfig),
                                              settings.Data(config::Store::UserSettings));
        auto& usageMonitor = usage::Global();
        if (settings.Revision() != usageConfigRevision)
        {
            usageConfigRevision = settings.Revision();
            usageMonitor.SetGlobalConfig(settings.Data(config::Store::GlobalConfig));
        }
        if (now >= nextUsagePoll)
        {
            std::vector<std::uint64_t> uids;
            for (const auto& t : tabs.Tabs()) uids.push_back(t->Uid());
            usageMonitor.PollSessions(uids);
            usageMonitor.AutoRefresh(appSettings.Get().usageAutoRefresh, focusedWin);
            nextUsagePoll = now + 1.0;
        }
        if (now >= nextLiveRefresh)
        {
            live.Refresh();
            nextLiveRefresh = now + 1.0;
        }
        if (rescanRequested || now >= nextRescan)
        {
            scanner.Rescan();
            rescanRequested = false;
            nextRescan      = now + 5.0;
        }

        // While a terminal owns the keyboard, Tab/arrows/Escape must reach
        // Claude Code instead of driving ImGui's keyboard navigation.
        if (terminalHadFocus) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef CLAUDEUI_DEBUG
        inputScript.Tick(io, now);
#endif

        // A changed terminal font is applied here, between frames: the atlas
        // can't be modified once NewFrame has locked it.
        if (appSettings.Revision() != appliedSettingsRevision)
        {
            appliedSettingsRevision = appSettings.Revision();
            ui::fonts::SetTerminalFont(appSettings.Get().terminalFont);
            ApplySessionProxy(appSettings.Get().sessionProxy);
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        tabs.Tick(live);
        std::unordered_set<std::string> openInTabs;
        for (const auto& t : tabs.Tabs())
        {
            if (t->TakeVpnRussiaAlert())
            {
                vpnAlertText  = t->Title() + ": VPN вышел в РФ";
                vpnAlertUntil = now + 8.0;
            }
            if (t->SessionId().empty()) continue;
            if (t->Running()) openInTabs.insert(t->SessionId());
            if (const auto* s = findSession(t->SessionId()); s && !s->title.empty() && s->title != t->Title())
                t->SetTitle(s->title); // picks up ai-title / /rename as they appear
        }

        // ── Section: rail clicks, Ctrl+, and --settings ──────────────────
        // A tab opened while statistics or settings fill the main area (from
        // the session list, the new-session dialog) brings the terminal forward.
        if (tabs.Count() > lastTabCount && section != ui::Section::Sessions) section = ui::Section::Sessions;
        lastTabCount = tabs.Count();
        if (std::exchange(selectSettingsTab, false)) section = ui::Section::Settings;
        if (!io.WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, ImGuiInputFlags_RouteGlobal))
            section = section == ui::Section::Settings ? ui::Section::Sessions : ui::Section::Settings;

        // Usage rows for the limits card and the statistics view.
        std::vector<ui::UsagePanel::SessionRow> usageRows;
        for (const auto& t : tabs.Tabs())
        {
            ui::UsagePanel::SessionRow row;
            row.title   = t->Title();
            row.running = t->Running();
            if (const auto* u = usageMonitor.Session(t->Uid())) row.costUsd = u->costUsd;
            usageRows.push_back(std::move(row));
        }

        constexpr ImVec4 kRailBg    {0x0f / 255.0f, 0x0f / 255.0f, 0x0e / 255.0f, 1.0f};
        constexpr ImVec4 kSidebarBg {0x1a / 255.0f, 0x1a / 255.0f, 0x18 / 255.0f, 1.0f};
        constexpr ImU32  kLine = IM_COL32(0x2b, 0x2a, 0x27, 255);

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(3);
        ImDrawList* rootDl = ImGui::GetWindowDrawList();

        // ── Rail ─────────────────────────────────────────────────────────
        {
            ui::Rail::Info info;
            info.runningTabs = tabs.RunningCount();
            const auto& acct = usageMonitor.Account();
            auto limit = [](const char* name, const usage::Window& w) {
                ui::Rail::Limit l;
                const bool expired = w.Expired(usage::NowMs());
                l.usedPct = expired ? 0.0 : w.usedPct;
                l.tier    = expired ? 0 : w.Tier();
                l.tooltip = ui::LimitSummary(name, w);
                return l;
            };
            if (acct.fiveHour) info.fiveHour = limit("5 часов", *acct.fiveHour);
            if (acct.sevenDay) info.week = limit("Неделя", *acct.sevenDay);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, kRailBg);
            ImGui::BeginChild("##rail", {ui::kRailWidth * xscale, 0}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            rail.Draw(section, info, xscale);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            const ImVec2 r = ImGui::GetItemRectMax();
            rootDl->AddLine({r.x - 0.5f, 0}, {r.x - 0.5f, r.y}, kLine);
        }
        ImGui::SameLine(0, 0);

        // ── Sidebar: what the rail selected ──────────────────────────────
        auto sidebarTitle = [&](const char* text) {
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.3f);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
            ImGui::Dummy({0, 4});
        };
        // Statistics take the whole main area; every other section has the sidebar.
        if (section != ui::Section::Stats)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebarBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f * xscale, 14.0f * xscale});
            ImGui::BeginChild("##sidebar_v2", {300.0f * xscale, 0}, ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding,
                              section == ui::Section::Sessions ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            switch (section)
            {
            case ui::Section::Sessions:
            case ui::Section::Stats: // not reached: statistics have no sidebar
            {
                // Panel stack: order/visibility from the "Панели" section
                // (appSettings.sidebarPanels). "usage" is the auto-sized limits
                // card; "sessions" is the sole Stretch panel and gets whatever's
                // left — computed up front, because BeginChild's "0 height = fill
                // the rest" only works for the last child drawn, and panels can be
                // reordered.
                const auto& panels = appSettings.Get().sidebarPanels;
                bool anyVisible = false;
                float autoTotal = 0.0f;
                for (const auto& p : panels)
                {
                    if (!p.visible) continue;
                    anyVisible = true;
                    if (p.id == "usage") autoTotal += (lastUsageBoxH > 0 ? lastUsageBoxH : 150.0f * xscale) + 10.0f * xscale + ImGui::GetStyle().ItemSpacing.y;
                }
                if (!anyVisible)
                    ImGui::TextColored(theme::palette::MidGray, "Все панели скрыты — включите их в разделе «Панели».");
                const float stretchH = std::max(60.0f * xscale, ImGui::GetContentRegionAvail().y - autoTotal);
                for (const auto& p : panels)
                {
                    if (!p.visible) continue;
                    if (p.id == "usage")
                    {
                        ImGui::Dummy({0, 10.0f * xscale - ImGui::GetStyle().ItemSpacing.y});
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::Dark);
                        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * xscale);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f * xscale, 12.0f * xscale});
                        ImGui::BeginChild("##panel_usage", {0, 0},
                                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                          ImGuiWindowFlags_NoScrollbar);
                        if (usagePanel.DrawCompact(usageMonitor.Account(), usageRows)) section = ui::Section::Stats;
                        ImGui::EndChild();
                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor();
                        lastUsageBoxH = ImGui::GetItemRectSize().y;
                    }
                    else if (p.id == "sessions")
                    {
                        ImGui::BeginChild("##panel_sessions", {0, stretchH});
                        sessionList.Draw(scanner.Sessions(), live, openInTabs);
                        ImGui::EndChild();
                    }
                }
                break;
            }
            case ui::Section::Panels:
                sidebarTitle("Панели");
                ImGui::PushTextWrapPos(0.0f);
                layoutPanel.Draw();
                ImGui::PopTextWrapPos();
                break;
            case ui::Section::Settings:
            {
                sidebarTitle("Настройки");
                const auto sections = ui::SettingsPanel::Sections();
                const std::string& visible = settingsPanel.VisibleSection();
                auto caption = [](const char* text) {
                    ImGui::Dummy({0, 6});
                    ImGui::TextColored(ImVec4{0.56f, 0.55f, 0.52f, 1.0f}, "%s", text);
                };
                for (std::size_t i = 0; i < sections.size(); ++i)
                {
                    if (i == 0) caption("Приложение");
                    if (i == 1) caption("Claude Code · /config");
                    if (ImGui::Selectable(sections[i].c_str(), sections[i] == visible, 0, {0, ImGui::GetFrameHeight()}))
                        settingsPanel.ScrollTo(sections[i]);
                    // How many settings here differ from the default.
                    if (const int changed = settingsPanel.ChangedCount(sections[i]); changed > 0)
                    {
                        const std::string n = "\xe2\x97\x8f " + std::to_string(changed);
                        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
                        const float fs = ImGui::GetFontSize() * 0.82f, k = fs / ImGui::GetFontSize();
                        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fs,
                                                            {b.x - ts.x * k - 8.0f, (a.y + b.y - ts.y * k) * 0.5f},
                                                            IM_COL32(0xe8, 0x95, 0x7a, 255), n.c_str());
                        ImGui::SetItemTooltip("Не по умолчанию: %d", changed);
                    }
                }
                break;
            }
            }
            ImGui::EndChild();
            {
                const ImVec2 r = ImGui::GetItemRectMax();
                rootDl->AddLine({r.x - 0.5f, 0}, {r.x - 0.5f, r.y}, kLine);
            }
            ImGui::SameLine(0, 0);
        }

        // ── Main area: session tabs, or the settings ─────────────────────
        if (section == ui::Section::Stats)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f * xscale, 20.0f * xscale});
            ImGui::BeginChild("##stats_pane", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            transcripts.Refresh();
            statsView.Draw(usageMonitor.Account(), usageRows, usageMonitor, transcripts, modelCatalog);
            ImGui::EndChild();
        }
        else if (section == ui::Section::Settings)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f * xscale, 20.0f * xscale});
            ImGui::BeginChild("##settings_pane", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            settingsPanel.Draw();
            ImGui::EndChild();
        }
        else
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f * xscale, 8.0f * xscale});
            ImGui::BeginChild("##sessions_pane", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            tabs.Draw();
            ImGui::EndChild();
        }

        ImGui::End();

        tabs.HandleShortcuts();

        if (auto spec = newSession.Draw(scanner.Sessions(), modelCatalog, modelAccess,
                                        appSettings.Get().showLegacyModels))
        {
            std::string title = BaseName(spec->cwd);
            if (spec->continueLast) title += " (continue)";
            tabs.Open(std::move(*spec), std::move(title));
        }

        // ── Quit confirmation ────────────────────────────────────────────
        if (quit == QuitState::Asking && !ImGui::IsPopupOpen("Выход из ClaudeUI")) ImGui::OpenPopup("Выход из ClaudeUI");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Выход из ClaudeUI", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const int running = tabs.RunningCount();
            if (quit == QuitState::Asking)
            {
                ImGui::Text("Сессий ещё работает: %d", running);
                ImGui::TextColored(theme::palette::MidGray,
                                   "/exit даст каждой сохранить состояние — потом их можно продолжить из списка.");
                ImGui::Spacing();
                if (ImGui::Button(ui::icons::Label(ui::icons::Icon::Exit, "/exit всем и выйти")))
                {
                    tabs.ExitAll();
                    quit         = QuitState::WaitingForExit;
                    quitDeadline = now + 20.0;
                }
                ImGui::SameLine();
                if (ImGui::Button("Остановить и выйти"))
                {
                    tabs.TerminateAll();
                    quit = QuitState::Done;
                }
                ImGui::SameLine();
                if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                {
                    quit = QuitState::None;
                    ImGui::CloseCurrentPopup();
                }
            }
            else if (quit == QuitState::WaitingForExit)
            {
                if (running == 0) quit = QuitState::Done;
                ImGui::Text("Ждём, пока сессии завершатся: осталось %d", running);
                for (const auto& t : tabs.Tabs())
                {
                    if (!t->Running()) continue;
                    ImGui::TextColored(theme::palette::MidGray, "  %s — %s", t->Title().c_str(),
                                       t->StatusText().c_str());
                    if (!t->ExitError().empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "(/exit отменён: %s)", t->ExitError().c_str());
                    }
                }
                if (now > quitDeadline)
                    ImGui::TextColored(theme::palette::Orange, "Не все завершились за 20 секунд (занятая сессия "
                                                               "выполнит /exit только после текущего ответа).");
                ImGui::Spacing();
                if (ImGui::Button("Остановить оставшиеся и выйти"))
                {
                    tabs.TerminateAll();
                    quit = QuitState::Done;
                }
                ImGui::SameLine();
                if (ImGui::Button("Отмена"))
                {
                    quit = QuitState::None;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        if (now < vpnAlertUntil) ui::DrawToast(vpnAlertText.c_str(), IM_COL32(0xd9, 0x5f, 0x57, 0xff));

        terminalHadFocus = section != ui::Section::Settings && tabs.AnyTerminalFocused() &&
                           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

        std::string title = "ClaudeUI";
#ifdef CLAUDEUI_DEBUG
        if (std::getenv("CLAUDEUI_TEST_INPUT")) title += " [test]"; // tells scripted test windows apart from real ones
#endif
        if (int n = tabs.RunningCount(); n > 0) title += " — " + std::to_string(n) + " активн.";
        if (title != windowTitle)
        {
            glfwSetWindowTitle(window, title.c_str());
            windowTitle = title;
        }

        ImGui::Render();
        VkCommandBuffer cmd = vk.beginFrame();
        if (cmd == VK_NULL_HANDLE)
        {
            continue; // swapchain was just recreated — skip this frame
        }
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vk.endFrame();
    }

    tabs.TerminateAll();
    usage::Global().Shutdown();
    vk.waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
