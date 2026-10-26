#include "ui/session_list_panel.hpp"
#include "sessions/live_sessions.hpp"
#include "sessions/session_scanner.hpp"
#include "theme/theme.hpp"
#include "ui/icons.hpp"
#include "ui/terminal_panel.hpp"
#include "ui/util.hpp"

#include <imgui_internal.h> // MenuItemEx
#include <misc/cpp/imgui_stdlib.h> // InputTextWithHint(..., std::string*)

#include <cfloat>
#include <filesystem>
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
    const auto& style = ImGui::GetStyle();
    const float fs    = ImGui::GetFontSize();

    // ── Title ──────────────────────────────────────────────────────────
    {
        int liveElsewhere = 0;
        for (const auto& l : live.All())
            if (!openInTabs.contains(l.sessionId)) ++liveElsewhere;

        ImGui::PushFont(nullptr, fs * 1.3f);
        ImGui::TextUnformatted("Сессии");
        ImGui::PopFont();
        std::string meta = std::to_string(openInTabs.size()) + " активны";
        if (liveElsewhere > 0) meta += " · " + std::to_string(liveElsewhere) + " снаружи";
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(meta.c_str()).x));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::palette::MidGray, "%s", meta.c_str());
        if (liveElsewhere > 0)
            ImGui::SetItemTooltip("%zu открыто в ClaudeUI, %d — в других терминалах (по ~/.claude/sessions)",
                                  openInTabs.size(), liveElsewhere);
    }

    // ── New session ────────────────────────────────────────────────────
    ImGui::Dummy({0, 2});
    ImGui::PushStyleColor(ImGuiCol_Button, theme::palette::Orange);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.89f, 0.55f, 0.43f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.76f, 0.40f, 0.29f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Dark);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (ImGui::Button(icons::Label(icons::Icon::NewChat, "Новая сессия"), {-FLT_MIN, ImGui::GetFrameHeight() * 1.55f}) &&
        cb_.newSession)
        cb_.newSession();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::SetItemTooltip("Ctrl+Shift+T");

    // ── Search · only-live · rescan ────────────────────────────────────
    ImGui::Dummy({0, 2});
    const char* liveGlyph  = "\xe2\x97\x8f"; // ●
    const char* rescanGlyph = icons::Glyph(icons::Icon::Retry);
    const float btnW = [&](const char* t) { return ImGui::CalcTextSize(t).x + style.FramePadding.x * 2; }(liveGlyph) +
                       ImGui::CalcTextSize(rescanGlyph).x + style.FramePadding.x * 2 + style.ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(-btnW);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    filter_.Draw("##search", 0.0f);
    if (!filter_.IsActive() && !ImGui::IsItemActive())
    {
        ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText({r.x + style.FramePadding.x, r.y + style.FramePadding.y},
                                            Col(theme::palette::MidGray, 0.7f),
                                            icons::Label(icons::Icon::Search, "Поиск по сессиям"));
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, onlyLive_ ? theme::palette::Green : theme::palette::MidGray);
    if (ImGui::Button("\xe2\x97\x8f##only_live")) onlyLive_ = !onlyLive_;
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(onlyLive_ ? "Показаны только запущенные — показать все" : "Показать только запущенные");
    ImGui::SameLine();
    ImGui::PushID("rescan");
    if (ImGui::Button(rescanGlyph) && cb_.rescan) cb_.rescan();
    ImGui::PopID();
    ImGui::SetItemTooltip("Перечитать ~/.claude/projects (%zu сессий)", sessions.size());
    ImGui::PopStyleVar();
    ImGui::Dummy({0, 2});

    // ── List ───────────────────────────────────────────────────────────
    auto passes = [&](const Group& g, const sessions::SessionInfo& s) {
        if (onlyLive_ && !openInTabs.contains(s.id) && !live.FindBySession(s.id)) return false;
        if (!filter_.IsActive()) return true;
        return filter_.PassFilter(g.cwd.c_str()) || filter_.PassFilter(s.title.c_str()) ||
               filter_.PassFilter(s.firstUserPreview.c_str()) || filter_.PassFilter(s.lastPrompt.c_str()) ||
               filter_.PassFilter(s.id.c_str());
    };

    ImGui::BeginChild("##session_list", {0, 0});
    if (sessions.empty()) ImGui::TextColored(theme::palette::MidGray, "В ~/.claude/projects пока нет сессий.");

    ImDrawList* dl       = ImGui::GetWindowDrawList();
    const float small    = fs * 0.9f;
    const float rowH     = fs + small + style.FramePadding.y * 2 + 6.0f;
    const ImU32 primary  = Col(theme::palette::Light);
    const ImU32 muted    = Col(theme::palette::MidGray);
    const ImU32 faint    = IM_COL32(0x8f, 0x8d, 0x85, 255);

    for (const auto& group : GroupByProject(sessions))
    {
        int visible = 0;
        for (const auto* s : group.items) visible += passes(group, *s) ? 1 : 0;
        if (visible == 0) continue;

        ImGui::PushID(group.cwd.c_str());

        // Group header: project folder name, muted; the full path in the tooltip.
        std::string name = std::filesystem::path(group.cwd).filename().string();
        if (name.empty()) name = TildePath(group.cwd);
        ImGui::Dummy({0, 4});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.56f, 0.55f, 0.52f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{0, 0, 0, 0});
        const std::string header = name + "  " + std::to_string(visible) + "###group";
        const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("%s", TildePath(group.cwd).c_str());
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
        const char* plus = icons::Glyph(icons::Icon::NewChat);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(plus).x - style.FramePadding.x * 2);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::MidGray);
        if (ImGui::SmallButton(plus) && cb_.newInDir) cb_.newInDir(group.cwd);
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Новая сессия в %s", TildePath(group.cwd).c_str());

        if (open)
        {
            for (const auto* s : group.items)
            {
                if (!passes(group, *s)) continue;
                ImGui::PushID(s->id.c_str());

                const bool here     = openInTabs.contains(s->id);
                const auto* running = live.FindBySession(s->id);
                const bool selected = selectedId_ == s->id;

                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  w = ImGui::GetContentRegionAvail().x;
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0, 0});
                if (ImGui::Selectable("##row", selected,
                                      ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, {w, rowH}))
                {
                    selectedId_ = s->id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) RequestOpen(*s, live, openInTabs);
                }
                ImGui::PopStyleVar();
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
                    ImGui::TextColored(theme::palette::MidGray, "Начата %s, активность %s · %s",
                                       HumanTime(s->firstTimestamp).c_str(), HumanTime(s->lastTimestamp).c_str(),
                                       HumanSize(s->fileSizeBytes).c_str());
                    if (running)
                        ImGui::TextColored(theme::palette::Blue, "Запущена: pid %d, %s", running->pid,
                                           LiveStatusText(running->status));
                    ImGui::TextColored(theme::palette::MidGray, "Двойной клик или Enter — открыть");
                    ImGui::EndTooltip();
                }

                // Status dot: colour = what the session is doing; hollow = not running.
                const float padX = style.FramePadding.x + 2.0f, padY = style.FramePadding.y + 2.0f;
                const float r    = fs * 0.25f;
                const ImVec2 dc{p.x + padX + r, p.y + padY + fs * 0.5f};
                std::string meta;
                if (running)
                {
                    ImU32 c = Col(theme::palette::Green);
                    if (here)
                    {
                        if (running->status == "busy") c = Col(theme::palette::Orange);
                        else if (running->status == "waiting") c = Col(theme::palette::Blue);
                        else c = Col(theme::palette::LightGray);
                        meta = std::string(LiveStatusText(running->status)) + " · открыта здесь";
                    }
                    else meta = "в другом терминале · pid " + std::to_string(running->pid);
                    dl->AddCircleFilled(dc, r, c);
                }
                else
                {
                    if (here) meta = "открыта здесь";
                    else meta = HumanSize(s->fileSizeBytes);
                    dl->AddCircle(dc, r - 0.5f, IM_COL32(0x6b, 0x6a, 0x64, 255), 0, 1.5f);
                }

                const std::string when = HumanTime(s->lastTimestamp);
                const ImVec2 ws  = ImGui::CalcTextSize(when.c_str());
                const float  tx  = dc.x + r + 10.0f;
                const float  wx  = p.x + w - padX - ws.x * 0.9f;
                ImGui::PushClipRect({tx, p.y}, {wx - 8.0f, p.y + rowH}, true);
                const std::string& title = s->DisplayName();
                dl->AddText(ImGui::GetFont(), fs, {tx, p.y + padY}, title.empty() ? muted : primary,
                            title.empty() ? "(без сообщений)" : title.c_str());
                dl->AddText(ImGui::GetFont(), small, {tx, p.y + padY + fs + 2.0f}, running || here ? muted : faint, meta.c_str());
                ImGui::PopClipRect();
                dl->AddText(ImGui::GetFont(), small, {wx, p.y + padY + (fs - small) * 0.5f}, faint, when.c_str());

                ImGui::PopID();
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
