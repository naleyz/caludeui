#include "ui/new_session_dialog.hpp"
#include "config/paths.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/model_picker.hpp"
#include "ui/util.hpp"
#include "ui/widgets.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

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
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& s : sessions) // already newest first
    {
        if (s.cwd.empty()) continue;
        if (auto it = index.find(s.cwd); it != index.end())
        {
            ++recent_[it->second].sessions;
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(s.cwd, ec) || recent_.size() >= 30) continue;
        index.emplace(s.cwd, recent_.size());
        recent_.push_back({s.cwd, 1, s.lastTimestamp});
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
    const char* kTitle = "Новая сессия###new_session";
    if (openRequested_)
    {
        openRequested_ = false;
        RefreshRecent(sessions);
        if (cwd_.empty() && !recent_.empty()) cwd_ = recent_.front().path;
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
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(fs * 50.0f, display.x - 40), 0}, ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f, 20.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(widgets::kCardBg));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(widgets::kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
    const bool open = ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    if (!open) return launched;

    const auto& style = ImGui::GetStyle();
    const std::string dir = ExpandTilde(cwd_);
    std::error_code ec;
    const bool dirOk = !dir.empty() && std::filesystem::is_directory(dir, ec);
    const float w = ImGui::GetContentRegionAvail().x;
    bool cancel = false;

    // ── header ──
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(nullptr, fs * 1.35f);
        ImGui::TextUnformatted("Новая сессия");
        ImGui::PopFont();
        const char* keys = "Ctrl+Enter — запустить · Esc — отмена";
        const char* closeGlyph = icons::Glyph(icons::Icon::Close);
        const float closeW = ImGui::CalcTextSize(closeGlyph).x + style.FramePadding.x * 2;
        const float keysW = ImGui::CalcTextSize(keys).x * 0.88f;
        ImGui::SetCursorScreenPos({p.x + w - closeW - keysW - 14.0f, p.y + (fs * 1.35f - fs * 0.88f) * 0.5f + 2.0f});
        widgets::SmallMuted(keys);
        ImGui::SetCursorScreenPos({p.x + w - closeW, p.y + (fs * 1.35f - ImGui::GetFrameHeight()) * 0.5f});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::MidGray);
        ImGui::PushID("close");
        if (ImGui::Button(closeGlyph)) cancel = true;
        ImGui::PopID();
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos({p.x, p.y + fs * 1.35f + 16.0f});
    }

    // ── folder ──
    auto caption = [&](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(widgets::kMuted));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    };
    caption("Папка проекта");
    {
        const char* browse = picker_->running.load() ? icons::Label(icons::Icon::Folder, "Выбор…###browse")
                                                     : icons::Label(icons::Icon::Folder, "Обзор…###browse");
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {style.FramePadding.x + 6.0f, style.FramePadding.y + 4.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        const float browseW = ImGui::CalcTextSize(browse, nullptr, true).x + (style.FramePadding.x + 6.0f) * 2;
        ImGui::SetNextItemWidth(w - browseW - style.ItemSpacing.x);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##cwd", "/путь/к/проекту", &cwd_);
        ImGui::SameLine();
        ImGui::BeginDisabled(picker_->running.load());
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        if (ImGui::Button(browse)) StartPicker();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        ImGui::PopStyleVar(2);

        // What's at that path, in one line under the field.
        std::string status;
        ImU32 tone = widgets::kMuted;
        if (dir.empty()) status = "Выберите папку из недавних или укажите путь.";
        else if (!dirOk)
        {
            status = "Такой папки нет";
            tone   = IM_COL32(0xec, 0x7e, 0x7e, 255);
        }
        else
        {
            status = "\xe2\x9c\x93 " + TildePath(dir);
            tone   = IM_COL32(0xa9, 0xbb, 0x8d, 255);
            for (const auto& r : recent_)
                if (r.path == dir)
                {
                    status += " · " + std::to_string(r.sessions) + " сесс. здесь, последняя " + HumanTime(r.lastActive);
                    break;
                }
        }
        {
            std::lock_guard lock(picker_->mu);
            if (!picker_->error.empty())
            {
                status = picker_->error;
                tone   = widgets::kMuted;
            }
        }
        ImGui::PushFont(nullptr, fs * 0.88f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tone));
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // ── recent projects ──
    bool launchNow = false;
    if (!recent_.empty())
    {
        ImGui::Dummy({0, 6});
        caption("Недавние проекты — двойной клик запускает сразу");
        const float rowH = fs * 2.0f + 8.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(IM_COL32(0x14, 0x14, 0x13, 255)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.0f, 6.0f});
        ImGui::BeginChild("##recent", {0, rowH * std::min<float>(4.5f, static_cast<float>(recent_.size())) + 12.0f},
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        auto* dl = ImGui::GetWindowDrawList();
        for (const auto& r : recent_)
        {
            ImGui::PushID(r.path.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rw = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##row", r.path == dir, ImGuiSelectableFlags_AllowDoubleClick, {rw, rowH - 4.0f}))
            {
                cwd_ = r.path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launchNow = true;
            }
            const std::string name = std::filesystem::path(r.path).filename().string();
            const std::string meta = std::to_string(r.sessions) + " сесс. · " + HumanTime(r.lastActive);
            const float metaW = ImGui::CalcTextSize(meta.c_str()).x * 0.88f;
            ImGui::PushClipRect(p, {p.x + rw - metaW - 16.0f, p.y + rowH}, true);
            dl->AddText({p.x + 10.0f, p.y + 4.0f}, ImGui::ColorConvertFloat4ToU32(theme::palette::Light), name.c_str());
            dl->AddText(ImGui::GetFont(), fs * 0.88f, {p.x + 10.0f, p.y + fs + 6.0f}, widgets::kMuted, TildePath(r.path).c_str());
            ImGui::PopClipRect();
            dl->AddText(ImGui::GetFont(), fs * 0.88f, {p.x + rw - metaW - 10.0f, p.y + 5.0f}, widgets::kMuted, meta.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ── options ──
    ImGui::Dummy({0, 8});
    {
        const float labelW = fs * 11.0f;
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelW);
        };
        label("Режим разрешений");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##perm", kPermissionChoices[static_cast<std::size_t>(permissionIndex_)].label))
        {
            for (int i = 0; i < static_cast<int>(kPermissionChoices.size()); ++i)
                if (ImGui::Selectable(kPermissionChoices[static_cast<std::size_t>(i)].label, i == permissionIndex_))
                    permissionIndex_ = i;
            ImGui::EndCombo();
        }
        label("Модель");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ModelCombo("new_session_model", model_, models, access, "как в настройках", "", showLegacyModels);

        label("Продолжить");
        widgets::Toggle("##continue", &continueLast_);
        ImGui::SameLine(0, 10.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(continueLast_ ? theme::palette::Light : theme::palette::MidGray,
                           "последнюю сессию в этой папке (--continue)");
    }

    // ── first message ──
    ImGui::Dummy({0, 6});
    ImGui::BeginDisabled(continueLast_);
    caption(continueLast_ ? "Первое сообщение — не нужно: сессия продолжится с того же места"
                          : "Первое сообщение — необязательно");
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::InputTextMultiline("##prompt", &prompt_, {-FLT_MIN, ImGui::GetTextLineHeight() * 4.5f});
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    LaunchSpec spec;
    spec.cwd = ExpandTilde(cwd_); // cwd_ may have just changed via the recent list
    spec.continueLast = continueLast_;
    spec.permissionMode = kPermissionChoices[static_cast<std::size_t>(permissionIndex_)].value;
    spec.model = model_;
    if (!continueLast_) spec.initialPrompt = prompt_;

    // ── the command it will run ──
    ImGui::Dummy({0, 6});
    {
        const std::string cmd = spec.CommandLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const char* copy = icons::Glyph(icons::Icon::Copy);
        const float copyW = ImGui::CalcTextSize(copy).x + style.FramePadding.x * 2;
        ImGui::PushFont(fonts::Mono, fs * 0.9f);
        const ImVec2 ts = ImGui::CalcTextSize(cmd.c_str(), nullptr, false, w - copyW - 28.0f);
        ImGui::PopFont();
        const float h = std::max(ts.y, ImGui::GetFrameHeight()) + 16.0f;
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, IM_COL32(0x0f, 0x0f, 0x0e, 255), 8.0f);
        ImGui::SetCursorScreenPos({p.x + 12.0f, p.y + (h - ts.y) * 0.5f});
        ImGui::PushFont(fonts::Mono, fs * 0.9f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - copyW - 28.0f);
        ImGui::TextColored(theme::palette::MidGray, "%s", cmd.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({p.x + w - copyW - 6.0f, p.y + (h - ImGui::GetFrameHeight()) * 0.5f});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::MidGray);
        ImGui::PushID("copy_cmd");
        if (ImGui::Button(copy)) ImGui::SetClipboardText(cmd.c_str());
        ImGui::PopID();
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Скопировать команду");
        ImGui::SetCursorScreenPos({p.x, p.y + h + 14.0f});
    }

    // ── actions ──
    {
        const char* go = icons::Label(icons::Icon::NewChat, "Запустить");
        const float goW = ImGui::CalcTextSize(go).x + style.FramePadding.x * 2 + 24.0f;
        const float cancelW = ImGui::CalcTextSize("Отмена").x + style.FramePadding.x * 2 + 16.0f;
        const float btnH = ImGui::GetFrameHeight() * 1.3f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - goW - cancelW - style.ItemSpacing.x);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        if (ImGui::Button("Отмена", {cancelW, btnH})) cancel = true;
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirOk);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::palette::Orange);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.89f, 0.55f, 0.43f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.76f, 0.40f, 0.29f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Dark);
        const bool launch = ImGui::Button(go, {goW, btnH}) ||
                            (dirOk && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::GetIO().KeyCtrl);
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if (!dirOk) ImGui::SetItemTooltip("Сначала выберите существующую папку");

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
        if ((launch && dirOk) || (launchNow && std::filesystem::is_directory(spec.cwd, ec)))
        {
            launched = spec;
            prompt_.clear();
            ImGui::CloseCurrentPopup();
        }
        else if (cancel) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return launched;
}
} // namespace ui
