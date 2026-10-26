#include "ui/new_session_dialog.hpp"
#include "config/paths.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/model_picker.hpp"
#include "ui/util.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <sys/wait.h>

namespace ui
{
namespace
{
struct PermissionChoice
{
    const char* value;
    const char* label;
};
constexpr std::array kPermissionChoices = {
    PermissionChoice{"", "как в настройках"},
    PermissionChoice{"default", "Manual — спрашивать"},
    PermissionChoice{"acceptEdits", "Accept edits — правки без вопросов"},
    PermissionChoice{"plan", "Plan — только планирование"},
    PermissionChoice{"auto", "Auto"},
    PermissionChoice{"dontAsk", "Don't ask"},
};

std::string ExpandTilde(std::string p)
{
    if (p == "~" || p.starts_with("~/")) p = config::HomeDir().string() + p.substr(1);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
} // namespace

struct NewSessionDialog::PickerState
{
    std::atomic<bool> running{false};
    std::mutex mu;
    std::optional<std::string> result;
    std::string error;
};

NewSessionDialog::NewSessionDialog() : picker_(std::make_shared<PickerState>()) {}
NewSessionDialog::~NewSessionDialog() = default; // a running picker thread keeps its own shared_ptr

void NewSessionDialog::Open(const std::string& cwd)
{
    openRequested_ = true;
    if (!cwd.empty()) cwd_ = cwd;
    prompt_.clear();
    continueLast_ = false;
}

void NewSessionDialog::RefreshRecent(const std::vector<sessions::SessionInfo>& sessions)
{
    recent_.clear();
    std::unordered_set<std::string> seen;
    for (const auto& s : sessions) // already newest first
    {
        if (s.cwd.empty() || !seen.insert(s.cwd).second) continue;
        std::error_code ec;
        if (std::filesystem::is_directory(s.cwd, ec)) recent_.push_back(s.cwd);
        if (recent_.size() >= 30) break;
    }
}

void NewSessionDialog::StartPicker()
{
    if (picker_->running.exchange(true)) return;
    std::string start = ExpandTilde(cwd_.empty() ? config::HomeDir().string() : cwd_);

    std::thread([state = picker_, start] {
        // kdialog on KDE, zenity elsewhere; whichever exists.
        std::string cmd;
        if (std::system("command -v kdialog >/dev/null 2>&1") == 0)
            cmd = "kdialog --getexistingdirectory " + ShellQuote(start) + " --title 'Папка для новой сессии' 2>/dev/null";
        else if (std::system("command -v zenity >/dev/null 2>&1") == 0)
            cmd = "zenity --file-selection --directory --title='Папка для новой сессии' --filename=" +
                  ShellQuote(start + "/") + " 2>/dev/null";

        std::optional<std::string> picked;
        std::string error;
        if (cmd.empty())
        {
            error = "Не найден kdialog или zenity — введите путь вручную.";
        }
        else if (FILE* f = ::popen(cmd.c_str(), "r"))
        {
            std::string out;
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) out += buf;
            int status = ::pclose(f);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !out.empty()) picked = out;
        }
        std::lock_guard lock(state->mu);
        state->result = picked;
        state->error = error;
        state->running.store(false);
    }).detach();
}

std::optional<LaunchSpec> NewSessionDialog::Draw(const std::vector<sessions::SessionInfo>& sessions,
                                                 const config::ModelCatalog& models, const config::ModelAccess& access,
                                                 bool showLegacyModels)
{
    const char* kTitle = "Новая сессия";
    if (openRequested_)
    {
        openRequested_ = false;
        RefreshRecent(sessions);
        if (cwd_.empty() && !recent_.empty()) cwd_ = recent_.front();
        ImGui::OpenPopup(kTitle);
    }

    {
        std::lock_guard lock(picker_->mu);
        if (picker_->result)
        {
            cwd_ = *picker_->result;
            picker_->result.reset();
        }
    }

    std::optional<LaunchSpec> launched;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(760.0f, display.x - 40), 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) return launched;

    const auto& style = ImGui::GetStyle();
    const std::string dir = ExpandTilde(cwd_);
    std::error_code ec;
    const bool dirOk = !dir.empty() && std::filesystem::is_directory(dir, ec);

    // ── folder ──
    ImGui::TextUnformatted("Папка");
    const char* browse = picker_->running.load() ? icons::Label(icons::Icon::Folder, "Выбор…###browse") : icons::Label(icons::Icon::Folder, "Обзор…###browse");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(browse, nullptr, true).x - style.FramePadding.x * 2 - style.ItemSpacing.x);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##cwd", "/путь/к/проекту", &cwd_);
    ImGui::SameLine();
    ImGui::BeginDisabled(picker_->running.load());
    if (ImGui::Button(browse)) StartPicker();
    ImGui::EndDisabled();
    {
        std::lock_guard lock(picker_->mu);
        if (!picker_->error.empty()) ImGui::TextColored(theme::palette::MidGray, "%s", picker_->error.c_str());
    }
    if (!dir.empty() && !dirOk) ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "Такой папки нет");

    bool launchNow = false;
    if (!recent_.empty())
    {
        ImGui::TextColored(theme::palette::MidGray, "Недавние проекты — двойной клик запускает сразу");
        ImGui::BeginChild("##recent", {0, ImGui::GetTextLineHeightWithSpacing() * std::min<float>(7, recent_.size()) +
                                                style.WindowPadding.y * 2},
                          ImGuiChildFlags_Borders);
        for (const auto& r : recent_)
        {
            if (ImGui::Selectable(TildePath(r).c_str(), r == dir, ImGuiSelectableFlags_AllowDoubleClick))
            {
                cwd_ = r;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launchNow = true;
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    // ── options ──
    if (ImGui::BeginTable("##opts", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Режим разрешений").x);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Режим разрешений");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##perm", kPermissionChoices[static_cast<std::size_t>(permissionIndex_)].label))
        {
            for (int i = 0; i < static_cast<int>(kPermissionChoices.size()); ++i)
                if (ImGui::Selectable(kPermissionChoices[static_cast<std::size_t>(i)].label, i == permissionIndex_))
                    permissionIndex_ = i;
            ImGui::EndCombo();
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Модель");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ModelCombo("new_session_model", model_, models, access, "как в настройках", "", showLegacyModels);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::Checkbox("Продолжить последнюю сессию в этой папке (--continue)", &continueLast_);
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(continueLast_);
    ImGui::TextUnformatted("Первое сообщение");
    ImGui::SameLine();
    ImGui::TextColored(theme::palette::MidGray, "необязательно");
    ImGui::InputTextMultiline("##prompt", &prompt_, {-FLT_MIN, ImGui::GetTextLineHeight() * 4});
    ImGui::EndDisabled();

    LaunchSpec spec;
    spec.cwd = ExpandTilde(cwd_); // cwd_ may have just changed via the recent list
    spec.continueLast = continueLast_;
    spec.permissionMode = kPermissionChoices[static_cast<std::size_t>(permissionIndex_)].value;
    spec.model = model_;
    if (!continueLast_) spec.initialPrompt = prompt_;

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(fonts::Mono, 0.0f);
    ImGui::TextColored(theme::palette::MidGray, "%s", spec.CommandLine().c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::BeginDisabled(!dirOk);
    const bool launch = ImGui::Button(icons::Label(icons::Icon::NewChat, "Запустить")) ||
                        (dirOk && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::GetIO().KeyCtrl);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Enter");
    ImGui::SameLine();
    if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();

    if ((launch && dirOk) || (launchNow && std::filesystem::is_directory(spec.cwd, ec)))
    {
        launched = spec;
        prompt_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return launched;
}
} // namespace ui
