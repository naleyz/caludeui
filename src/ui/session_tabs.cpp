#include "ui/session_tabs.hpp"
#include "sessions/live_sessions.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/util.hpp"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
using Clock = std::chrono::steady_clock;

ImU32 Col(const ImVec4& c, float a = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a});
}

// Claude Code's own CLI startup mark, pixel for pixel: captured live via
// term_probe (our VT parser reading the real `claude` banner, so the source
// whitespace/glyphs are exact) and decoded from the block-quadrant
// characters (▐▛█▜▝▀ etc, each cell = a 2x2 sub-pixel block) into a plain
// bitmap. Do not hand-draw a different shape here — this *is* the logo.
constexpr int   kMascotCols = 18;
constexpr int   kMascotRows = 6;
// A terminal cell is much taller than it is wide (JetBrains Mono: 600/1320
// advance/line-height ≈ 0.4545), and each source character is a 2x2 block of
// these bitmap cells — so the cells themselves share that same ratio. Drawing
// them square flattened the logo into a pancake; height must be ~2.2x width.
constexpr float kMascotPxW = 8.0f;
constexpr float kMascotPxH = kMascotPxW * 1320.0f / 600.0f;
constexpr const char* kMascotBody[kMascotRows] = {
    "...#############..",
    "...##.#######.##..",
    ".#################",
    "...#############..",
    ".....#.#...#.#....",
    "..................",
};
// The two "eye" gaps in kMascotBody, as (row, col) — used to draw a squint.
constexpr int kEyeRow = 1;
constexpr int kEyeCols[2] = {5, 13};

// Walks in from the left over kWalkDuration, then rocks side to side like a
// wave every couple of seconds while resting. Right-clicking it squints its
// eyes for a moment. `elapsed` is seconds since the empty state first
// appeared; `sincePet` is seconds since it was last right-clicked (or a huge
// number if never). Returns true if it was right-clicked this frame.
bool DrawMascot(ImVec2 restTopLeft, float elapsed, float sincePet)
{
    constexpr float kWalkDuration = 1.1f;
    constexpr float kWalkDistance = 220.0f;

    float t = std::min(1.0f, elapsed / kWalkDuration);
    float ease = 1.0f - (1.0f - t) * (1.0f - t); // ease-out quad
    float bob = t < 1.0f ? std::sin(elapsed * 16.0f) * 2.0f * (1.0f - t) : 0.0f;
    ImVec2 origin = {restTopLeft.x - kWalkDistance * (1.0f - ease), restTopLeft.y + bob};

    // Wave: a few quick rocks, fading in and out, every 2.2s once arrived.
    float angle = 0.0f;
    if (t >= 1.0f)
    {
        float waveT   = elapsed - kWalkDuration;
        float phase   = std::fmod(waveT, 2.2f);
        float localT  = std::clamp(phase / 1.0f, 0.0f, 1.0f);
        float envelope = phase < 1.0f ? std::sin(3.14159265f * localT) : 0.0f;
        angle = 0.22f * envelope * std::sin(phase * 22.0f);
    }

    // Pet: eyes squint for a beat, triggered by a right-click on the sprite.
    constexpr float kPetDuration = 1.2f;
    float squint = sincePet < kPetDuration ? std::sin(3.14159265f * sincePet / kPetDuration) * 0.65f : 0.0f;

    ImVec2 spriteSize = {kMascotCols * kMascotPxW, kMascotRows * kMascotPxH};
    ImVec2 center     = {origin.x + spriteSize.x * 0.5f, origin.y + spriteSize.y * 0.5f};
    float  s = std::sin(angle), c = std::cos(angle);
    auto rotate = [&](ImVec2 p) {
        float dx = p.x - center.x, dy = p.y - center.y;
        return ImVec2{center.x + dx * c - dy * s, center.y + dx * s + dy * c};
    };

    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 body = Col(theme::palette::Orange);
    auto fillCell = [&](ImVec2 p0, ImVec2 p1) {
        if (angle == 0.0f) { dl->AddRectFilled(p0, p1, body); return; }
        ImVec2 c0 = p0, c1 = {p1.x, p0.y}, c2 = p1, c3 = {p0.x, p1.y};
        dl->AddQuadFilled(rotate(c0), rotate(c1), rotate(c2), rotate(c3), body);
    };

    // Adjacent rotated quads otherwise leave faint AA seams between them.
    const ImDrawListFlags savedFlags = dl->Flags;
    if (angle != 0.0f) dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int r = 0; r < kMascotRows; ++r)
        for (int col = 0; col < kMascotCols; ++col)
        {
            if (kMascotBody[r][col] == '.') continue;
            ImVec2 p0 = {origin.x + col * kMascotPxW, origin.y + r * kMascotPxH};
            fillCell(p0, {p0.x + kMascotPxW, p0.y + kMascotPxH});
        }
    if (squint > 0.0f)
        for (int eyeCol : kEyeCols)
        {
            ImVec2 p0 = {origin.x + eyeCol * kMascotPxW, origin.y + kEyeRow * kMascotPxH};
            fillCell(p0, {p0.x + kMascotPxW, p0.y + kMascotPxH * squint}); // eyelid closing from the top
        }
    dl->Flags = savedFlags;

    ImVec2 boundsMax = {origin.x + spriteSize.x, origin.y + spriteSize.y};
    return ImGui::IsMouseHoveringRect(origin, boundsMax, false) &&
           ImGui::IsMouseClicked(ImGuiMouseButton_Right);
}
} // namespace

TerminalPanel* SessionTabs::Open(LaunchSpec spec, std::string title)
{
    if (!spec.resumeId.empty())
    {
        if (auto* existing = FindBySession(spec.resumeId))
        {
            if (!existing->Running()) existing->Restart();
            Focus(existing);
            return existing;
        }
    }
    auto panel = std::make_unique<TerminalPanel>(std::move(spec), std::move(title));
    panel->Launch();
    auto* raw = panel.get();
    tabs_.push_back(std::move(panel));
    Focus(raw);
    mascotActive_ = false; // replay the walk-in next time the empty state shows
    return raw;
}

TerminalPanel* SessionTabs::FindBySession(const std::string& sessionId)
{
    if (sessionId.empty()) return nullptr;
    for (auto& t : tabs_)
        if (t->SessionId() == sessionId) return t.get();
    return nullptr;
}

void SessionTabs::Focus(const TerminalPanel* panel)
{
    for (auto& t : tabs_)
        if (t.get() == panel)
        {
            selectUid_ = t->Uid();
            t->FocusTerminal();
        }
}

int SessionTabs::RunningCount() const
{
    return static_cast<int>(std::count_if(tabs_.begin(), tabs_.end(), [](const auto& t) { return t->Running(); }));
}

bool SessionTabs::AnyTerminalFocused() const
{
    return std::any_of(tabs_.begin(), tabs_.end(), [](const auto& t) { return t->HasFocus(); });
}

void SessionTabs::ExitAll()
{
    for (auto& t : tabs_)
        if (t->Running() && !t->ExitRequested()) t->RequestExit();
}

void SessionTabs::TerminateAll()
{
    for (auto& t : tabs_) t->RequestTerminate();
    for (auto& t : tabs_) t->Stop();
}

void SessionTabs::Tick(const sessions::LiveSessionRegistry& live)
{
    bool anyExited = false;
    for (auto& t : tabs_)
    {
        const bool before = std::any_of(wasRunning_.begin(), wasRunning_.end(),
                                        [&](const auto& p) { return p.first == t->Uid() && p.second; });
        t->Tick(live);
        if (before && !t->Running()) anyExited = true;
    }
    wasRunning_.clear();
    for (auto& t : tabs_) wasRunning_.emplace_back(t->Uid(), t->Running());

    // Tabs closed via "/exit and close" disappear once the process is gone —
    // unless the /exit was called off, then the tab simply stays.
    for (std::size_t i = 0; i < tabs_.size();)
    {
        auto it = std::find(closeWhenExited_.begin(), closeWhenExited_.end(), tabs_[i]->Uid());
        if (it != closeWhenExited_.end() && !tabs_[i]->Running())
        {
            closeWhenExited_.erase(it);
            CloseTab(i);
            continue;
        }
        if (it != closeWhenExited_.end() && !tabs_[i]->ExitRequested()) closeWhenExited_.erase(it);
        ++i;
    }
    if (anyExited && onSessionExited) onSessionExited();
}

void SessionTabs::CloseTab(std::size_t index)
{
    if (index >= tabs_.size()) return;
    const bool wasActive = tabs_[index]->Uid() == activeUid_;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index)); // destructor stops a still-running process
    if (wasActive && !tabs_.empty()) Focus(tabs_[std::min(index, tabs_.size() - 1)].get());
}

void SessionTabs::HandleShortcuts()
{
    auto& io = ImGui::GetIO();
    if (io.WantTextInput) return; // typing into a search box or dialog

    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T, global) && onNewSession) onNewSession();

    auto activeIndex = [&]() -> int {
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            if (tabs_[i]->Uid() == activeUid_) return static_cast<int>(i);
        return -1;
    };
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_W, global))
    {
        int i = activeIndex();
        if (i >= 0)
        {
            auto& t = tabs_[static_cast<std::size_t>(i)];
            if (t->Running()) confirmCloseUid_ = t->Uid();
            else CloseTab(static_cast<std::size_t>(i));
        }
    }
    if (!tabs_.empty())
    {
        int i = activeIndex();
        int n = static_cast<int>(tabs_.size());
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, global | ImGuiInputFlags_Repeat))
            Focus(tabs_[static_cast<std::size_t>((i + 1 + n) % n)].get());
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, global | ImGuiInputFlags_Repeat))
            Focus(tabs_[static_cast<std::size_t>((i - 1 + n) % n)].get());
    }
}

void SessionTabs::Draw()
{
    for (std::size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i]->TakeCloseRequest())
        {
            if (tabs_[i]->Running()) confirmCloseUid_ = tabs_[i]->Uid();
            else
            {
                CloseTab(i);
                break;
            }
        }

    if (tabs_.empty())
    {
        DrawEmptyState();
        return;
    }

    if (ImGui::BeginTabBar("##sessions", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
                                             ImGuiTabBarFlags_AutoSelectNewTabs))
    {
        if (ImGui::TabItemButton(icons::Glyph(icons::Icon::NewChat), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip) && onNewSession)
            onNewSession();
        ImGui::SetItemTooltip("Новая сессия (Ctrl+Shift+T)");

        std::size_t closeIndex = tabs_.size();
        for (std::size_t i = 0; i < tabs_.size(); ++i)
        {
            auto& t = tabs_[i];
            ImGuiTabItemFlags flags = 0;
            if (t->Uid() == selectUid_) flags |= ImGuiTabItemFlags_SetSelected;
            if (t->NeedsAttention()) flags |= ImGuiTabItemFlags_UnsavedDocument;

            std::string title = t->Title();
            if (title.size() > 40)
            {
                std::size_t cut = 40;
                while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
                title = title.substr(0, cut) + "…";
            }
            std::string label = title + "###tab" + std::to_string(t->Uid());

            bool styled = true;
            switch (t->GetStatus())
            {
            case TerminalPanel::Status::Busy:    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Orange); break;
            case TerminalPanel::Status::Waiting: ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::Blue); break;
            case TerminalPanel::Status::Exited:
            case TerminalPanel::Status::Blocked:
            case TerminalPanel::Status::Failed:  ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::MidGray); break;
            default: styled = false; break;
            }

            bool open = true;
            const bool selected = ImGui::BeginTabItem(label.c_str(), &open, flags);
            if (styled) ImGui::PopStyleColor();

            // Progress (OSC 9;4) as a line along the tab's bottom edge.
            auto prog = t->Progress();
            if (prog.state != 0)
            {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float frac = prog.state == 3 ? 1.0f : prog.value / 100.0f;
                ImGui::GetWindowDrawList()->AddRectFilled({a.x, b.y - 2}, {a.x + (b.x - a.x) * frac, b.y},
                                                          Col(theme::palette::Orange, prog.state == 3 ? 0.5f : 1.0f));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ImGui::IsItemActive())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(t->Title().c_str());
                ImGui::TextColored(theme::palette::MidGray, "%s", t->StatusText().c_str());
                ImGui::TextColored(theme::palette::MidGray, "%s", TildePath(t->Spec().cwd.string()).c_str());
                if (!t->SessionId().empty()) ImGui::TextColored(theme::palette::MidGray, "%s", t->SessionId().c_str());
                ImGui::EndTooltip();
            }

            if (selected)
            {
                activeUid_ = t->Uid();
                if (selectUid_ == t->Uid()) selectUid_ = 0;
                t->Draw();
                ImGui::EndTabItem();
            }
            if (!open) closeIndex = i;
        }
        ImGui::EndTabBar();

        if (closeIndex < tabs_.size())
        {
            if (tabs_[closeIndex]->Running()) confirmCloseUid_ = tabs_[closeIndex]->Uid();
            else CloseTab(closeIndex);
        }
    }
    DrawCloseConfirm();
}

void SessionTabs::DrawCloseConfirm()
{
    if (confirmCloseUid_ == 0) return;
    auto it = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto& t) { return t->Uid() == confirmCloseUid_; });
    if (it == tabs_.end())
    {
        confirmCloseUid_ = 0;
        return;
    }
    if (!ImGui::IsPopupOpen("Закрыть сессию?")) ImGui::OpenPopup("Закрыть сессию?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool stopAndClose = false;
    if (ImGui::BeginPopupModal("Закрыть сессию?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        auto& t = *it;
        ImGui::Text("«%s» ещё работает (%s).", t->Title().c_str(), t->StatusText().c_str());
        ImGui::TextColored(theme::palette::MidGray, "/exit даст Claude Code сохранить сессию и завершиться штатно.");
        ImGui::Spacing();
        if (ImGui::Button(icons::Label(icons::Icon::Exit, "/exit и закрыть вкладку")))
        {
            t->RequestExit();
            closeWhenExited_.push_back(t->Uid());
            confirmCloseUid_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Остановить и закрыть"))
        {
            stopAndClose = true;
            confirmCloseUid_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            confirmCloseUid_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (stopAndClose) CloseTab(static_cast<std::size_t>(it - tabs_.begin())); // invalidates `it`
}

void SessionTabs::DrawEmptyState()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const char* wordmark = "ClaudeUI";
    const char* line1 = "Нет открытых сессий";
    const char* line2 = "Выберите сессию слева, чтобы продолжить её, или начните новую.";
    const char* button = icons::Label(icons::Icon::NewChat, "Новая сессия  (Ctrl+Shift+T)");
    const auto& style = ImGui::GetStyle();
    const float wordmarkSize = fonts::Serif->LegacySize * 2.4f;
    float h = wordmarkSize + ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetFrameHeight() + style.ItemSpacing.y * 4;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - h) * 0.45f));

    auto centered = [&](const char* text, const ImVec4& color) {
        ImGui::SetCursorPosX(std::max(0.0f, (avail.x - ImGui::CalcTextSize(text).x) * 0.5f));
        ImGui::TextColored(color, "%s", text);
    };
    ImGui::PushFont(fonts::Serif, wordmarkSize);
    centered(wordmark, theme::palette::Orange);
    ImGui::PopFont();
    ImGui::Spacing();
    centered(line1, theme::palette::Light);
    centered(line2, theme::palette::MidGray);
    ImGui::Spacing();
    ImGui::SetCursorPosX(std::max(0.0f, (avail.x - ImGui::CalcTextSize(button).x - style.FramePadding.x * 2) * 0.5f));
    if (ImGui::Button(button) && onNewSession) onNewSession();

    if (!mascotActive_)
    {
        mascotShownAt_ = Clock::now();
        mascotActive_  = true;
    }
    ImGui::Spacing();
    ImGui::Spacing();
    const float spriteW = kMascotCols * kMascotPxW;
    ImGui::SetCursorPosX(std::max(0.0f, (avail.x - spriteW) * 0.5f));
    ImVec2 restTopLeft = ImGui::GetCursorScreenPos();
    float elapsed  = std::chrono::duration<float>(Clock::now() - mascotShownAt_).count();
    float sincePet = mascotEverPetted_ ? std::chrono::duration<float>(Clock::now() - mascotPettedAt_).count() : 1e9f;
    if (DrawMascot(restTopLeft, elapsed, sincePet))
    {
        mascotPettedAt_    = Clock::now();
        mascotEverPetted_  = true;
    }
    ImGui::Dummy({spriteW, kMascotRows * kMascotPxH});
}
} // namespace ui
