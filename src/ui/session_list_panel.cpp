#include "ui/session_list_panel.hpp"
#include "sessions/live_sessions.hpp"
#include "sessions/session_scanner.hpp"
#include "theme/theme.hpp"
#include "ui/icons.hpp"
#include "ui/terminal_panel.hpp"
#include "ui/util.hpp"

#include <imgui_internal.h> // MenuItemEx
#include <misc/cpp/imgui_stdlib.h> // InputTextWithHint(..., std::string*)

#include <unordered_map>

namespace ui
{
namespace
{
struct Group
{
    std::string cwd;
    std::vector<const sessions::SessionInfo*> items;
};

// Groups by cwd (falling back to the encoded project dir name), preserving
// the newest-first order so the most recently active project leads.
std::vector<Group> GroupByProject(const std::vector<sessions::SessionInfo>& sessions)
{
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& s : sessions)
    {
        std::string key = s.cwd.empty() ? s.projectDir.filename().string() : s.cwd;
        auto [it, inserted] = index.try_emplace(key, groups.size());
        if (inserted) groups.push_back({key, {}});
        groups[it->second].items.push_back(&s);
    }
    return groups;
}

ImU32 Col(const ImVec4& c, float a = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a});
}

const char* LiveStatusText(const std::string& status)
{
    if (status == "busy") return "работает";
    if (status == "waiting") return "ждёт ввода";
    if (status == "idle") return "простаивает";
    return status.c_str();
}
} // namespace

void SessionListPanel::RequestOpen(const sessions::SessionInfo& s, const sessions::LiveSessionRegistry& live,
                                   const std::unordered_set<std::string>& openInTabs)
{
    const auto* running = live.FindBySession(s.id);
    if (running && !openInTabs.contains(s.id))
    {
        confirmSession_ = s;
        confirmDetail_  = "pid " + std::to_string(running->pid) + ", " + LiveStatusText(running->status);
        confirmOpen_    = true;
        return;
    }
    if (cb_.open) cb_.open(s);
}

void SessionListPanel::DrawOpenConfirm()
{
    if (!confirmOpen_) return;
    if (!ImGui::IsPopupOpen("Сессия уже запущена")) ImGui::OpenPopup("Сессия уже запущена");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Сессия уже запущена", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("«%s» уже открыта в другом терминале (%s).", confirmSession_.DisplayName().c_str(),
                    confirmDetail_.c_str());
        ImGui::TextColored(theme::palette::MidGray,
                           "Два процесса, продолжающие одну сессию, пишут в один транскрипт вперемешку.");
        ImGui::TextColored(theme::palette::MidGray, "Лучше закройте её там (/exit) и откройте здесь.");
        ImGui::Spacing();
        if (ImGui::Button("Всё равно открыть"))
        {
            if (cb_.open) cb_.open(confirmSession_);
            confirmOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            confirmOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SessionListPanel::Draw(const std::vector<sessions::SessionInfo>& sessions,
                            const sessions::LiveSessionRegistry& live,
                            const std::unordered_set<std::string>& openInTabs)
{
    // ── Header ─────────────────────────────────────────────────────────
    int liveElsewhere = 0;
    for (const auto& l : live.All())
        if (!openInTabs.contains(l.sessionId)) ++liveElsewhere;

    ImGui::TextColored(theme::palette::MidGray, "%zu сессий", sessions.size());
    if (!openInTabs.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(theme::palette::Green, "· %zu здесь", openInTabs.size());
    }
    if (liveElsewhere > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(theme::palette::Blue, "· %d в других терминалах", liveElsewhere);
        ImGui::SetItemTooltip("Процессы Claude Code, запущенные не из ClaudeUI (по ~/.claude/sessions)");
    }
    const char* refresh = icons::Label(icons::Icon::Retry, "Обновить");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(refresh).x -
                                                        ImGui::GetStyle().FramePadding.x * 2));
    if (ImGui::SmallButton(refresh) && cb_.rescan) cb_.rescan();

    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Только активные").x - ImGui::GetFrameHeight() -
                            ImGui::GetStyle().ItemSpacing.x * 2);
    filter_.Draw("##search", 0.0f);
    if (!filter_.IsActive() && !ImGui::IsItemActive())
    {
        ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText({r.x + ImGui::GetStyle().FramePadding.x, r.y + ImGui::GetStyle().FramePadding.y},
                                            Col(theme::palette::MidGray, 0.7f),
                                            icons::Label(icons::Icon::Search, "Поиск: название, текст, папка, id"));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Только активные", &onlyLive_);
    ImGui::Separator();

    // ── List ───────────────────────────────────────────────────────────
    auto passes = [&](const Group& g, const sessions::SessionInfo& s) {
        if (onlyLive_ && !openInTabs.contains(s.id) && !live.FindBySession(s.id)) return false;
        if (!filter_.IsActive()) return true;
        return filter_.PassFilter(g.cwd.c_str()) || filter_.PassFilter(s.title.c_str()) ||
               filter_.PassFilter(s.firstUserPreview.c_str()) || filter_.PassFilter(s.lastPrompt.c_str()) ||
               filter_.PassFilter(s.id.c_str());
    };

    ImGui::BeginChild("##session_list", {0, 0});
    if (sessions.empty())
    {
        ImGui::TextColored(theme::palette::MidGray, "В ~/.claude/projects пока нет сессий.");
    }

    for (const auto& group : GroupByProject(sessions))
    {
        int visible = 0;
        for (const auto* s : group.items) visible += passes(group, *s) ? 1 : 0;
        if (visible == 0) continue;

        ImGui::PushID(group.cwd.c_str());
        std::string header = TildePath(group.cwd) + "  (" + std::to_string(visible) + ")###group";
        const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                                      ImGuiTreeNodeFlags_AllowOverlap);
        if (ImGui::BeginPopupContextItem("##group_ctx"))
        {
            if (ImGui::MenuItemEx("Новая сессия в этой папке…", icons::MenuGlyph(icons::Icon::NewChat)) && cb_.newInDir) cb_.newInDir(group.cwd);
            if (ImGui::MenuItemEx("Продолжить последнюю (--continue)", icons::MenuGlyph(icons::Icon::Retry)) && cb_.continueIn)
                cb_.continueIn(group.cwd);
            if (ImGui::MenuItemEx("Открыть папку", icons::MenuGlyph(icons::Icon::Folder))) OpenWithDesktop(group.cwd);
            if (ImGui::MenuItemEx("Копировать путь", icons::MenuGlyph(icons::Icon::Copy))) ImGui::SetClipboardText(group.cwd.c_str());
            ImGui::EndPopup();
        }
        // "+" on the header's right edge.
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(icons::Glyph(icons::Icon::NewChat)).x -
                        ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::SmallButton(icons::Glyph(icons::Icon::NewChat)) && cb_.newInDir) cb_.newInDir(group.cwd);
        ImGui::SetItemTooltip("Новая сессия в %s", TildePath(group.cwd).c_str());

        if (open)
        {
            const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("##sessions", 3, tflags))
            {
                ImGui::TableSetupColumn("##when", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00 мая 00:00").x);
                ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##size", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000.0 МБ").x);

                for (const auto* s : group.items)
                {
                    if (!passes(group, *s)) continue;
                    ImGui::PushID(s->id.c_str());
                    ImGui::TableNextRow();

                    const bool here = openInTabs.contains(s->id);
                    const auto* running = live.FindBySession(s->id);

                    ImGui::TableSetColumnIndex(0);
                    const bool selected = selectedId_ == s->id;
                    if (ImGui::Selectable("##row", selected,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                              ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selectedId_ = s->id;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) RequestOpen(*s, live, openInTabs);
                    }
                    if (selected && ImGui::IsItemFocused() &&
                        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                        RequestOpen(*s, live, openInTabs);

                    if (ImGui::BeginPopupContextItem("##row_ctx"))
                    {
                        selectedId_ = s->id;
                        if (ImGui::MenuItemEx(here ? "Перейти к вкладке" : "Открыть", icons::MenuGlyph(icons::Icon::Sessions)))
                            RequestOpen(*s, live, openInTabs);
                        if (ImGui::MenuItemEx("Копировать ID", icons::MenuGlyph(icons::Icon::Copy))) ImGui::SetClipboardText(s->id.c_str());
                        if (ImGui::MenuItemEx("Копировать команду resume", icons::MenuGlyph(icons::Icon::Copy)))
                        {
                            LaunchSpec spec;
                            spec.cwd = s->cwd.empty() ? s->projectDir : std::filesystem::path(s->cwd);
                            spec.resumeId = s->id;
                            ImGui::SetClipboardText(spec.CommandLine().c_str());
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItemEx("Новая сессия в этой папке…", icons::MenuGlyph(icons::Icon::NewChat)) && cb_.newInDir)
                            cb_.newInDir(group.cwd);
                        if (ImGui::MenuItemEx("Открыть папку проекта", icons::MenuGlyph(icons::Icon::Folder))) OpenWithDesktop(group.cwd);
                        if (ImGui::MenuItemEx("Показать транскрипт (.jsonl)", icons::MenuGlyph(icons::Icon::Folder)))
                            OpenWithDesktop(s->filePath.parent_path());
                        ImGui::Separator();
                        if (ImGui::MenuItemEx("Переименовать…", nullptr))
                        {
                            renameTarget_ = *s;
                            renameBuffer_ = s->DisplayName();
                            renameError_.clear();
                            renameOpen_ = true;
                        }
                        const bool active = here || running;
                        ImGui::BeginDisabled(active);
                        if (ImGui::MenuItemEx("Удалить сессию…", nullptr))
                        {
                            deleteTarget_ = *s;
                            deleteError_.clear();
                            deleteOpen_ = true;
                        }
                        ImGui::EndDisabled();
                        if (active) ImGui::SetItemTooltip("Нельзя удалить активную сессию — сначала закройте её (/exit)");
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
                        if (!s->title.empty()) ImGui::TextUnformatted(s->title.c_str());
                        if (!s->firstUserPreview.empty())
                            ImGui::TextColored(theme::palette::MidGray, "Первый запрос: %s", s->firstUserPreview.c_str());
                        if (!s->lastPrompt.empty() && s->lastPrompt != s->firstUserPreview)
                            ImGui::TextColored(theme::palette::MidGray, "Последний: %s", s->lastPrompt.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Separator();
                        ImGui::TextColored(theme::palette::MidGray, "%s", s->id.c_str());
                        ImGui::TextColored(theme::palette::MidGray, "Начата %s, активность %s",
                                           HumanTime(s->firstTimestamp).c_str(), HumanTime(s->lastTimestamp).c_str());
                        if (running)
                            ImGui::TextColored(theme::palette::Blue, "Запущена: pid %d, %s", running->pid,
                                               LiveStatusText(running->status));
                        ImGui::TextColored(theme::palette::MidGray, "Двойной клик или Enter — открыть");
                        ImGui::EndTooltip();
                    }

                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(theme::palette::MidGray, "%s", HumanTime(s->lastTimestamp).c_str());

                    ImGui::TableSetColumnIndex(1);
                    if (here) StatusDot(Col(theme::palette::Green));
                    else if (running) StatusDot(Col(theme::palette::Blue));
                    const std::string& name = s->DisplayName();
                    if (name.empty()) ImGui::TextColored(theme::palette::MidGray, "(без сообщений)");
                    else ImGui::TextUnformatted(name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(theme::palette::MidGray, "%s", HumanSize(s->fileSizeBytes).c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    DrawOpenConfirm();
    DrawRenameDialog();
    DrawDeleteConfirm();
}

void SessionListPanel::DrawRenameDialog()
{
    if (!renameOpen_) return;
    if (!ImGui::IsPopupOpen("Переименовать сессию")) ImGui::OpenPopup("Переименовать сессию");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Переименовать сессию", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
        const bool entered = ImGui::InputTextWithHint("##rename", "название сессии", &renameBuffer_,
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        if (!renameError_.empty()) ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "%s", renameError_.c_str());
        ImGui::Spacing();
        const bool save = ImGui::Button("Сохранить") || entered;
        if (save && !renameBuffer_.empty())
        {
            if (sessions::RenameSessionFile(renameTarget_.filePath, renameTarget_.id, renameBuffer_, &renameError_))
            {
                renameOpen_ = false;
                if (cb_.rescan) cb_.rescan();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            renameOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SessionListPanel::DrawDeleteConfirm()
{
    if (!deleteOpen_) return;
    if (!ImGui::IsPopupOpen("Удалить сессию")) ImGui::OpenPopup("Удалить сессию");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Удалить сессию", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Удалить «%s»?", deleteTarget_.DisplayName().c_str());
        ImGui::TextColored(theme::palette::MidGray, "Транскрипт (.jsonl) будет удалён без возможности восстановления.");
        if (!deleteError_.empty()) ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "%s", deleteError_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Удалить"))
        {
            if (sessions::DeleteSessionFile(deleteTarget_.filePath, &deleteError_))
            {
                deleteOpen_ = false;
                if (selectedId_ == deleteTarget_.id) selectedId_.clear();
                if (cb_.rescan) cb_.rescan();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            deleteOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} // namespace ui
