#include "ui/settings_panel.hpp"
#include "config/paths.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/model_picker.hpp"
#include "ui/util.hpp"
#include "ui/widgets.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

namespace ui
{
namespace
{
using Source = config::SettingsStore::Source;

std::string JoinPath(const config::KeyPath& p)
{
    std::string s;
    for (const auto& k : p) s += (s.empty() ? "" : ".") + k;
    return s;
}

const char* FileLabel(config::Store s)
{
    return s == config::Store::UserSettings ? "~/.claude/settings.json" : "~/.claude.json";
}

std::string OptionLabel(const config::SettingDef& def, const std::string& value)
{
    for (const auto& o : def.options)
        if (o.value == value) return o.label.empty() ? o.value : o.label;
    return value;
}
} // namespace

std::string SettingsPanel::ValuesSignature() const
{
    std::string signature;
    for (const auto& def : config::AllSettings()) signature += store_.Get(def).value.dump() + '\x1f';
    return signature;
}

void SettingsPanel::Apply(const config::SettingDef& def, const nlohmann::json& value)
{
    std::string err;
    if (store_.Set(def, value, &err))
    {
        error_.clear();
        shownSignature_ = ValuesSignature(); // our own write is not an external change
        seenRevision_ = store_.Revision();
        notice_ = def.label + " → " + (value.is_string() ? value.get<std::string>() : value.dump()) + " (сохранено в " +
                  FileLabel(def.store) + ")";
        noticeUntil_ = ImGui::GetTime() + 4.0;
    }
    else
    {
        error_ = def.label + ": " + err;
    }
}

void SettingsPanel::Reset(const config::SettingDef& def)
{
    std::string err;
    if (store_.Reset(def, &err))
    {
        error_.clear();
        shownSignature_ = ValuesSignature();
        seenRevision_ = store_.Revision();
        textEdits_.erase(def.id);
        notice_ = def.label + " сброшено к значению по умолчанию";
        noticeUntil_ = ImGui::GetTime() + 4.0;
    }
    else
    {
        error_ = def.label + ": " + err;
    }
}

namespace
{
constexpr ImU32 kPillSetBg     = IM_COL32(0x3a, 0x2a, 0x22, 255);
constexpr ImU32 kPillSetFg     = IM_COL32(0xe8, 0x95, 0x7a, 255);
constexpr ImU32 kPillInfoBg    = IM_COL32(0x1e, 0x2a, 0x36, 255);
constexpr ImU32 kPillInfoFg    = IM_COL32(0x9c, 0xc0, 0xe4, 255);
constexpr ImU32 kPillErrBg     = IM_COL32(0x3c, 0x1e, 0x1c, 255);
constexpr ImU32 kPillErrFg     = IM_COL32(0xec, 0x7e, 0x7e, 255);
constexpr float kRowPad        = 12.0f;

// Where a row's control goes, once its text is laid out.
struct RowFrame
{
    ImVec2 top;
    float  width    = 0.0f;
    float  controlW = 0.0f;
    float  textH    = 0.0f;
};

// Lays out the text half of a card row — the label (+ tags drawn by `tags`)
// and a muted description under it — and returns where the control column is.
template <class Tags>
RowFrame RowText(const char* label, bool emphasize, const std::string& description, float controlW, Tags&& tags)
{
    const float fs = ImGui::GetFontSize();
    RowFrame f;
    f.top      = {ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + kRowPad};
    f.width    = ImGui::GetContentRegionAvail().x;
    f.controlW = controlW;
    // Room on the right for the control and the "сбросить" button beside it.
    const float resetW = ImGui::CalcTextSize("сбросить").x + ImGui::GetStyle().FramePadding.x * 2 + fs * 0.6f;
    const float textW  = std::max(fs * 8.0f, f.width - controlW - resetW - fs * 1.2f);

    ImGui::SetCursorScreenPos(f.top);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
    ImGui::PushStyleColor(ImGuiCol_Text, emphasize ? theme::palette::Light : theme::palette::LightGray);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    tags();
    if (!description.empty())
    {
        ImGui::PushFont(nullptr, fs * 0.88f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(widgets::kMuted));
        ImGui::TextUnformatted(description.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    f.textH = ImGui::GetCursorScreenPos().y - f.top.y - ImGui::GetStyle().ItemSpacing.y;
    return f;
}

// Places the cursor in the control column, vertically centred on the text.
void RowControlAt(const RowFrame& f, float x, float h)
{
    const float rowH = std::max(f.textH, h);
    ImGui::SetCursorScreenPos({x, f.top.y + (rowH - h) * 0.5f});
}

// Moves the cursor below the row.
void RowEnd(const RowFrame& f)
{
    const float rowH = std::max(f.textH, ImGui::GetFrameHeight());
    ImGui::SetCursorScreenPos({f.top.x, f.top.y + rowH + kRowPad});
    ImGui::Dummy({f.width, 0.0f});
}

// The control column's left edge for a control `w` wide, right-aligned.
float RightAligned(const RowFrame& f, float w)
{
    return f.top.x + f.width - w;
}

// A quiet "сбросить" button left of the control; returns true on click.
bool ResetButton(const RowFrame& f, const char* tooltip)
{
    const char* label = "сбросить##reset";
    const float bw = ImGui::CalcTextSize("сбросить").x + ImGui::GetStyle().FramePadding.x * 2;
    RowControlAt(f, f.top.x + f.width - f.controlW - bw - ImGui::GetFontSize() * 0.6f, ImGui::GetFrameHeight());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(widgets::kMuted));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("%s", tooltip);
    return clicked;
}
} // namespace

bool SettingsPanel::DrawControl(const config::SettingDef& def, const config::SettingsStore::Value& v)
{
    const float controlW = ImGui::CalcItemWidth();
    ImGui::SetNextItemWidth(controlW);
    const bool unknownDefault = v.value.is_null();
    const std::string defaultPreview = "по умолчанию";
    auto noteTooltip = [&] {
        if (unknownDefault && !def.defaultNote.empty()) ImGui::SetItemTooltip("По умолчанию: %s", def.defaultNote.c_str());
    };

    switch (def.kind)
    {
    case config::Kind::Bool:
    {
        if (unknownDefault)
        {
            // Unset and the default depends on the account: a three-way choice.
            if (ImGui::BeginCombo("##v", defaultPreview.c_str()))
            {
                if (ImGui::Selectable("вкл (true)")) Apply(def, true);
                if (ImGui::Selectable("выкл (false)")) Apply(def, false);
                ImGui::EndCombo();
            }
            noteTooltip();
            return false;
        }
        bool b = v.value.get<bool>();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + controlW - widgets::ToggleWidth());
        if (widgets::Toggle("##v", &b))
        {
            Apply(def, b);
            return true;
        }
        ImGui::SetItemTooltip("%s", b ? "Включено" : "Выключено");
        return false;
    }

    case config::Kind::Enum:
    {
        std::string current = v.value.is_string() ? v.value.get<std::string>() : std::string{};
        // A few short choices read best side by side.
        if (!unknownDefault && def.options.size() >= 2 && def.options.size() <= 4)
        {
            std::vector<std::string> names;
            int index = -1;
            for (std::size_t i = 0; i < def.options.size(); ++i)
            {
                const auto& o = def.options[i];
                names.push_back(o.label.empty() ? o.value : o.label);
                if (o.value == current) index = static_cast<int>(i);
            }
            std::vector<const char*> labels;
            for (const auto& n : names) labels.push_back(n.c_str());
            const float segW = widgets::SegmentedWidth(labels.data(), static_cast<int>(labels.size()));
            if (index >= 0 && segW <= controlW)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + controlW - segW);
                if (widgets::Segmented("##v", labels.data(), static_cast<int>(labels.size()), &index))
                    Apply(def, def.options[static_cast<std::size_t>(index)].value);
                return false;
            }
        }
        std::string preview = unknownDefault ? defaultPreview : OptionLabel(def, current);
        if (ImGui::BeginCombo("##v", preview.c_str()))
        {
            for (const auto& o : def.options)
            {
                const bool isCurrent = !unknownDefault && o.value == current;
                std::string label = o.label.empty() ? o.value : o.label;
                if (o.label.size() && o.label != o.value) label += "  [" + o.value + "]";
                if (ImGui::Selectable(label.c_str(), isCurrent) && !isCurrent) Apply(def, o.value);
                if (isCurrent) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        noteTooltip();
        return false;
    }

    case config::Kind::Model:
    {
        std::string model = v.value.is_string() ? v.value.get<std::string>() : std::string("default");
        if (ModelCombo(def.id.c_str(), model, models_, access_, "default (рекомендуемая)", "default",
                       app_.Get().showLegacyModels))
            Apply(def, model);
        return false;
    }

    case config::Kind::Text:
    {
        const std::string current = v.value.is_string() ? v.value.get<std::string>() : std::string{};
        // The edit buffer only lives while the field is being typed into, so
        // the field follows external changes the rest of the time.
        auto it = textEdits_.try_emplace(def.id, current).first;

        const float arrowW = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(controlW - arrowW - ImGui::GetStyle().ItemInnerSpacing.x);
        bool commit = ImGui::InputTextWithHint("##v", "по умолчанию", &it->second, ImGuiInputTextFlags_EnterReturnsTrue);
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        const bool active = ImGui::IsItemActive();
        const std::string typed = it->second;
        if (!active) textEdits_.erase(it);

        if (commit && typed != current)
        {
            if (typed.empty()) Reset(def);
            else Apply(def, typed);
        }

        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::BeginCombo("##suggest", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft))
        {
            for (const auto& o : def.options)
            {
                std::string label = o.label.empty() ? o.value : o.label;
                if (ImGui::Selectable(label.c_str(), o.value == current) && o.value != current)
                {
                    textEdits_.erase(def.id);
                    Apply(def, o.value);
                }
            }
            ImGui::EndCombo();
        }
        return false;
    }
    }
    return false;
}

void SettingsPanel::DrawRow(const config::SettingDef& def)
{
    const auto v = store_.Get(def);
    const bool explicitlySet = v.source != Source::Default;
    const float controlW = ImGui::GetFontSize() * 14.0f;

    ImGui::PushID(def.id.c_str());
    const RowFrame f = RowText(def.label.c_str(), explicitlySet, def.description, controlW, [&] {
        // Key and file on hover over the label.
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted(def.description.c_str());
            ImGui::Spacing();
            ImGui::TextColored(theme::palette::MidGray, "Ключ: %s → %s", FileLabel(def.store), JoinPath(def.path).c_str());
            if (def.writeMode == config::WriteMode::OmitDefault)
                ImGui::TextColored(theme::palette::MidGray, "Значение по умолчанию хранится как отсутствие ключа.");
            if (!def.scopeNote.empty()) ImGui::TextColored(theme::palette::Blue, "%s", def.scopeNote.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        auto tag = [](const char* text, ImU32 bg, ImU32 fg, const char* tip) {
            ImGui::SameLine(0, 8.0f);
            widgets::Pill(text, bg, fg);
            if (tip) ImGui::SetItemTooltip("%s", tip);
        };
        switch (v.source)
        {
        case Source::UserSettings:
        case Source::GlobalConfig: tag("задано", kPillSetBg, kPillSetFg, FileLabel(def.store)); break;
        case Source::LegacyGlobal:
            tag("задано*", kPillInfoBg, kPillInfoFg,
                "Взято из ~/.claude.json (старое место хранения). Новое значение запишется в ~/.claude/settings.json и будет главнее.");
            break;
        case Source::ForcedOff: tag("выкл.", kPillInfoBg, kPillInfoFg, ("Выключено ключом " + JoinPath(def.forcedOffBy) + ": true").c_str()); break;
        case Source::Default: break;
        }
        if (v.invalid)
            tag("неверный тип", kPillErrBg, kPillErrFg, "В файле у ключа неверный тип — Claude Code его проигнорирует. Выберите значение или сбросьте.");
    });

    const bool resettable = v.source == Source::UserSettings || v.source == Source::GlobalConfig ||
                            v.source == Source::ForcedOff || v.invalid;
    if (resettable && ResetButton(f, "Сбросить: удалить ключ из файла — вернётся значение по умолчанию")) Reset(def);

    RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
    ImGui::PushItemWidth(controlW);
    DrawControl(def, v);
    ImGui::PopItemWidth();
    RowEnd(f);
    ImGui::PopID();
}

void SettingsPanel::SaveTerminalFont(const config::TerminalFont& value, const char* notice)
{
    std::string err;
    if (app_.SetTerminalFont(value, &err))
    {
        error_.clear();
        notice_      = notice;
        noticeUntil_ = ImGui::GetTime() + 4.0;
    }
    else
    {
        error_ = "ClaudeUI: " + err;
    }
}

// Terminal face and size, as rows of the ClaudeUI card. The face is swapped by
// main() between frames once the setting is saved; the size follows the slider
// live and is saved on release.
void SettingsPanel::DrawTerminalFont()
{
    if (!fontsScanned_)
    {
        monoFonts_    = config::ScanMonospaceFonts();
        fontsScanned_ = true;
    }
    const config::TerminalFont current = app_.Get().terminalFont;
    const float fs = ImGui::GetFontSize();
    const float controlW = fs * 18.0f;

    // ── face ──
    widgets::CardDivider();
    {
        const RowFrame f = RowText("Шрифт терминала", !current.IsDefaultFace(),
                                   "Только моноширинные; кириллицу и символы, которых в нём нет, доберёт JetBrains Mono.",
                                   controlW, [] {});
        if (!current.IsDefaultFace() && ResetButton(f, "Вернуть JetBrains Mono"))
        {
            config::TerminalFont next = current;
            next.family.clear();
            next.regularPath.clear();
            next.boldPath.clear();
            SaveTerminalFont(next, "Шрифт терминала: JetBrains Mono (встроенный)");
        }
        std::string shown = "JetBrains Mono (встроенный)";
        if (!current.IsDefaultFace())
        {
            shown = current.family.empty() ? std::filesystem::path(current.regularPath).filename().string() : current.family;
            const bool listed = current.regularPath == fonts::BundledFontPath("anthropic-mono.ttf") ||
                                std::any_of(monoFonts_.begin(), monoFonts_.end(),
                                            [&](const auto& m) { return m.regular == current.regularPath; });
            if (!listed) shown = "Свой файл: " + shown;
        }
        RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::BeginCombo("##terminal_font", shown.c_str()))
        {
            if (ImGui::Selectable("JetBrains Mono (встроенный)", current.IsDefaultFace()))
            {
                config::TerminalFont next = current;
                next.family.clear();
                next.regularPath.clear();
                next.boldPath.clear();
                SaveTerminalFont(next, "Шрифт терминала: JetBrains Mono (встроенный)");
            }
            // The project's own face: Latin from Anthropic Mono, Cyrillic and symbols come from JetBrains Mono.
            if (const std::string anthropic = fonts::BundledFontPath("anthropic-mono.ttf"); !anthropic.empty())
            {
                const bool selected = current.regularPath == anthropic;
                if (ImGui::Selectable("Anthropic Mono (встроенный, латиница)", selected))
                {
                    config::TerminalFont next = current;
                    next.family      = "Anthropic Mono";
                    next.regularPath = anthropic;
                    next.boldPath.clear(); // the bundled file is one weight
                    SaveTerminalFont(next, "Шрифт терминала: Anthropic Mono");
                }
            }
            ImGui::Separator();
            for (const auto& fam : monoFonts_)
            {
                const bool selected = fam.regular == current.regularPath;
                if (ImGui::Selectable(fam.family.c_str(), selected))
                {
                    config::TerminalFont next = current;
                    next.family      = fam.family;
                    next.regularPath = fam.regular;
                    next.boldPath    = fam.bold;
                    SaveTerminalFont(next, ("Шрифт терминала: " + fam.family).c_str());
                }
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::SetItemTooltip("%s", fam.regular.c_str());
            }
            ImGui::EndCombo();
        }
        RowEnd(f);
    }

    // ── size ──
    widgets::CardDivider();
    {
        const bool sizeSet = current.size != config::TerminalFont{}.size;
        const RowFrame f = RowText("Размер", sizeSet, "Меняется сразу, сохраняется, когда отпустите ползунок.", controlW, [] {});
        if (sizeSet && ResetButton(f, "Вернуть размер по умолчанию"))
        {
            config::TerminalFont next = current;
            next.size = config::TerminalFont{}.size;
            SaveTerminalFont(next, "Размер шрифта терминала сброшен");
        }
        RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
        int size = fontSizeDrag_.value_or(static_cast<int>(current.size + 0.5f));
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::SliderInt("##terminal_font_size", &size, static_cast<int>(config::TerminalFont::kMinSize),
                             static_cast<int>(config::TerminalFont::kMaxSize), "%d px"))
        {
            fontSizeDrag_ = size;
            fonts::PreviewTerminalSize(static_cast<float>(size));
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && fontSizeDrag_)
        {
            config::TerminalFont next = current;
            next.size = static_cast<float>(*fontSizeDrag_);
            SaveTerminalFont(next, "Размер шрифта терминала сохранён");
        }
        if (!ImGui::IsItemActive()) fontSizeDrag_.reset();
        RowEnd(f);
    }

    // ── a face that isn't in the list: any .ttf/.otf by path ──
    widgets::CardDivider();
    {
        const RowFrame f = RowText("Свой файл", false, "Путь к .ttf или .otf, если шрифта нет в списке.", controlW, [] {});
        RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
        const char* apply = "Применить##terminal_font_path";
        const float applyW = ImGui::CalcTextSize("Применить").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(controlW - applyW - ImGui::GetStyle().ItemSpacing.x);
        const bool entered = ImGui::InputTextWithHint("##terminal_font_path", "/путь/к/шрифту.ttf", &fontPathEdit_,
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(fontPathEdit_.empty());
        const bool clicked = ImGui::Button(apply);
        ImGui::EndDisabled();
        if ((entered || clicked) && !fontPathEdit_.empty())
        {
            std::string path = fontPathEdit_;
            if (path.starts_with("~/")) path = (config::HomeDir() / path.substr(2)).string();
            std::string why;
            if (!config::LooksLikeFontFile(path, &why))
            {
                error_ = "Шрифт терминала: " + why;
            }
            else
            {
                config::TerminalFont next = current;
                next.regularPath = path;
                next.boldPath    = config::GuessBoldSibling(path);
                next.family      = std::filesystem::path(path).stem().string();
                SaveTerminalFont(next, ("Шрифт терминала: " + next.family).c_str());
                fontPathEdit_.clear();
            }
        }
        RowEnd(f);
    }

    if (!fonts::TerminalFontError().empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kPillErrFg));
        ImGui::TextWrapped("%s", fonts::TerminalFontError().c_str());
        ImGui::PopStyleColor();
    }

    // Sample in the face actually in use: Latin, Cyrillic, and the symbols
    // Claude Code's interface prints — so a font that lacks them shows up here.
    // (Box drawing and block elements are drawn by the terminal itself.)
    ImGui::PushFont(fonts::Mono, fonts::TerminalSize);
    const float sampleLine = ImGui::GetTextLineHeightWithSpacing(); // grows with the size slider
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0x0f / 255.0f, 0x0f / 255.0f, 0x0e / 255.0f, 1.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f, 10.0f});
    ImGui::BeginChild("##terminal_font_sample", {0, sampleLine * 3.0f + 20.0f}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(fonts::Mono, fonts::TerminalSize);
    ImGui::TextColored(theme::palette::LightGray, "The quick brown fox 0O1lI |  Съешь ещё этих мягких булок");
    ImGui::TextColored(theme::palette::MidGray, "\xe2\x9d\xaf \xe2\x8f\xba \xe2\x9c\xbb \xe2\x8e\xbf \xe2\x9c\x93 \xe2\x86\x91\xe2\x86\x93 \xe2\x80\xa6 \xe2\x80\x94 \xc2\xab\xc2\xbb");
    ImGui::PushFont(fonts::MonoBold, fonts::TerminalSize);
    ImGui::TextColored(theme::palette::Orange, "Bold: Hello, мир");
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 6.0f});
}

void SettingsPanel::SaveSessionProxy(const config::ProxySetting& value, const char* notice)
{
    std::string err;
    if (app_.SetSessionProxy(value, &err))
    {
        error_.clear();
        notice_      = notice;
        noticeUntil_ = ImGui::GetTime() + 4.0;
        proxyCheckResult_.reset(); // the last check was for the old route
    }
    else
    {
        error_ = "ClaudeUI: " + err;
    }
}

// Proxy for the sessions ClaudeUI starts: ClaudeUI's environment, a proxy of
// its own, or none. Applied by main() (ApplySessionProxy) to every launch, the
// VPN exit check and the usage probe.
void SettingsPanel::DrawSessionProxy(bool divider)
{
    using Mode = config::ProxySetting::Mode;
    const config::ProxySetting current = app_.Get().sessionProxy;
    const float fs = ImGui::GetFontSize();
    const float controlW = fs * 18.0f;

    // ── mode ──
    if (divider) widgets::CardDivider();
    {
        static constexpr const char* kModes[] = {"Как в системе", "Свой", "Без прокси"};
        std::string desc;
        switch (current.mode)
        {
        case Mode::System: desc = "Берётся из окружения ClaudeUI (https_proxy, all_proxy…). Запущенный из меню, ClaudeUI может не видеть то, что задано в .zshrc."; break;
        case Mode::Custom: desc = "Свой адрес для сессий — переменные прокси из окружения игнорируются."; break;
        case Mode::None:   desc = "Сессии идут напрямую, даже если в окружении задан прокси."; break;
        }
        desc += " Действует для новых сессий.";
        const RowFrame f = RowText("Прокси для сессий", current.mode != Mode::System, desc, controlW, [] {});
        const float segW = widgets::SegmentedWidth(kModes, 3);
        RowControlAt(f, RightAligned(f, segW), ImGui::GetFrameHeight());
        int mode = static_cast<int>(current.mode);
        if (widgets::Segmented("##proxy_mode", kModes, 3, &mode))
        {
            config::ProxySetting next = current;
            next.mode = static_cast<Mode>(mode);
            SaveSessionProxy(next, mode == 0 ? "Прокси: как в системе" : mode == 1 ? "Прокси: свой адрес" : "Прокси: без прокси");
        }
        RowEnd(f);
    }

    if (current.mode == Mode::Custom)
    {
        // ── address ──
        widgets::CardDivider();
        {
            const std::string problem = net::CheckProxyUrl(current.url);
            const std::string desc = problem.empty() ? std::string("HTTP(S)-прокси: http://хост:порт, при необходимости с логином: http://user:pass@хост:порт.")
                                                     : problem + " — пока используется прокси из окружения.";
            const RowFrame f = RowText("Адрес прокси", true, desc, controlW, [&] {
                if (!problem.empty())
                {
                    ImGui::SameLine(0, 8.0f);
                    widgets::Pill("не задан", kPillErrBg, kPillErrFg);
                }
            });
            RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
            std::string buf = proxyUrlEdit_.value_or(current.url);
            ImGui::SetNextItemWidth(controlW);
            const bool entered = ImGui::InputTextWithHint("##proxy_url", "http://127.0.0.1:10809", &buf, ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemActive()) proxyUrlEdit_ = buf;
            if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && buf != current.url)
            {
                config::ProxySetting next = current;
                next.url = buf;
                SaveSessionProxy(next, net::CheckProxyUrl(buf).empty() ? "Адрес прокси сохранён" : "Адрес прокси сохранён, но он не подходит");
            }
            if (!ImGui::IsItemActive()) proxyUrlEdit_.reset();
            RowEnd(f);
        }
        // ── no_proxy ──
        widgets::CardDivider();
        {
            const RowFrame f = RowText("Без прокси для", current.noProxy != config::ProxySetting::kDefaultNoProxy,
                                       "NO_PROXY: хосты через запятую, которые идут напрямую.", controlW, [] {});
            RowControlAt(f, RightAligned(f, controlW), ImGui::GetFrameHeight());
            std::string buf = noProxyEdit_.value_or(current.noProxy);
            ImGui::SetNextItemWidth(controlW);
            const bool entered = ImGui::InputTextWithHint("##no_proxy", config::ProxySetting::kDefaultNoProxy, &buf,
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemActive()) noProxyEdit_ = buf;
            if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && buf != current.noProxy)
            {
                config::ProxySetting next = current;
                next.noProxy = buf;
                SaveSessionProxy(next, "NO_PROXY сохранён");
            }
            if (!ImGui::IsItemActive()) noProxyEdit_.reset();
            RowEnd(f);
        }
    }

    // ── check where sessions would come out ──
    widgets::CardDivider();
    {
        if (proxyCheck_.valid() && proxyCheck_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            proxyCheckResult_ = proxyCheck_.get();
            proxyCheck_ = {};
        }
        const bool checking = proxyCheck_.valid();
        std::string desc = "Тот же запрос, что ClaudeUI делает перед запуском сессии: куда пойдёт трафик и в какой стране выход.";
        ImU32 tone = 0;
        if (proxyCheckResult_)
        {
            using Outcome = net::EgressCheck::Outcome;
            const auto& r = *proxyCheckResult_;
            // ProxyDown's summary already names the proxy.
            desc = r.outcome == Outcome::ProxyDown ? r.Summary() : r.route.Describe() + " · " + r.Summary();
            tone = r.outcome == Outcome::Ok ? IM_COL32(0xa9, 0xbb, 0x8d, 255)
                 : r.outcome == Outcome::NoVpn ? widgets::kMuted
                                               : kPillErrFg;
        }
        const RowFrame f = RowText("Проверить выход", false, tone ? std::string() : desc, controlW, [&] {
            if (!tone) return;
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.88f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tone));
            ImGui::TextUnformatted(desc.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        });
        const char* label = checking ? "Проверяю…##proxy_check" : "Проверить##proxy_check";
        const float bw = ImGui::CalcTextSize(checking ? "Проверяю…" : "Проверить").x + ImGui::GetStyle().FramePadding.x * 2;
        RowControlAt(f, RightAligned(f, bw), ImGui::GetFrameHeight());
        ImGui::BeginDisabled(checking);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        if (ImGui::Button(label)) proxyCheck_ = net::Guard().Check(/*forceFresh=*/true);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        RowEnd(f);
    }
}

void SettingsPanel::DrawAppSection(float width)
{
    static constexpr const char* kLegacyLabel = "Показывать устаревшие модели (Claude 3.x)";
    static constexpr const char* kUsageLabel  = "Обновлять лимиты в фоне";
    const bool showLegacy   = app_.Get().showLegacyModels;
    const bool autoUsage    = app_.Get().usageAutoRefresh;
    const bool fontModified = !(app_.Get().terminalFont == config::TerminalFont{});
    auto matches = [&](const char* label, const char* keywords) {
        return !filter_.IsActive() || filter_.PassFilter(label) || filter_.PassFilter(keywords);
    };
    const bool legacyVisible = (!onlyChanged_ || showLegacy) && matches(kLegacyLabel, "ClaudeUI legacy модели");
    const bool fontVisible = (!onlyChanged_ || fontModified) &&
                             matches("Шрифт терминала", "ClaudeUI terminal font шрифт терминал размер моноширинный");
    const bool usageVisible = (!onlyChanged_ || !autoUsage) && matches(kUsageLabel, "ClaudeUI usage лимиты расход extra");
    const bool proxyVisible = (!onlyChanged_ || !(app_.Get().sessionProxy == config::ProxySetting{})) &&
                              matches("Прокси для сессий", "ClaudeUI proxy прокси all_proxy https_proxy no_proxy vpn впн");
    if (!legacyVisible && !fontVisible && !usageVisible && !proxyVisible) return;

    SectionAnchor("ClaudeUI");
    widgets::CardBegin("##section_app", "ClaudeUI", width, "настройки программы — в файлы Claude Code не пишутся");
    const float toggleCol = ImGui::GetFontSize() * 14.0f;
    bool first = true;

    auto saveFlag = [&](bool ok, const std::string& err, const char* notice) {
        if (ok)
        {
            error_.clear();
            notice_ = notice;
            noticeUntil_ = ImGui::GetTime() + 4.0;
        }
        else error_ = "ClaudeUI: " + err;
    };

    if (legacyVisible)
    {
        first = false;
        const RowFrame f = RowText(kLegacyLabel, showLegacy,
                                   "Блок «Устаревшие» в выборе модели. API может их уже не обслуживать.", toggleCol, [&] {
                                       ImGui::SetItemTooltip("Хранится в %s", app_.Path().c_str());
                                   });
        RowControlAt(f, RightAligned(f, widgets::ToggleWidth()), ImGui::GetFrameHeight());
        bool legacy = showLegacy;
        if (widgets::Toggle("##legacy", &legacy))
        {
            std::string err;
            const bool ok = app_.SetShowLegacyModels(legacy, &err);
            saveFlag(ok, err, legacy ? "Устаревшие модели добавлены в выбор модели" : "Устаревшие модели скрыты");
        }
        RowEnd(f);
    }
    if (usageVisible)
    {
        if (!first) widgets::CardDivider();
        first = false;
        const RowFrame f = RowText(kUsageLabel, !autoUsage,
                                   "Раз в 5 минут, пока окно в фокусе: `claude -p /usage` — локально, токены не тратятся.",
                                   toggleCol, [] {});
        RowControlAt(f, RightAligned(f, widgets::ToggleWidth()), ImGui::GetFrameHeight());
        bool on = autoUsage;
        if (widgets::Toggle("##usage_auto", &on))
        {
            std::string err;
            const bool ok = app_.SetUsageAutoRefresh(on, &err);
            saveFlag(ok, err, on ? "Лимиты будут обновляться в фоне" : "Фоновое обновление лимитов выключено");
        }
        RowEnd(f);
    }
    if (proxyVisible)
    {
        DrawSessionProxy(!first);
        first = false;
    }
    if (fontVisible) DrawTerminalFont();
    if (!app_.LoadError().empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kPillErrFg));
        ImGui::TextWrapped("%s", app_.LoadError().c_str());
        ImGui::PopStyleColor();
    }
    widgets::CardEnd();
    ImGui::Dummy({0, 14.0f});
}

std::vector<std::string> SettingsPanel::Sections()
{
    std::vector<std::string> out{"ClaudeUI"};
    for (const auto& g : config::SettingGroups()) out.push_back(g);
    return out;
}

int SettingsPanel::ChangedCount(const std::string& section) const
{
    if (section == "ClaudeUI")
    {
        const auto& a = app_.Get();
        return (a.showLegacyModels ? 1 : 0) + (!a.usageAutoRefresh ? 1 : 0) + (!(a.terminalFont == config::TerminalFont{}) ? 1 : 0) +
               (!(a.sessionProxy == config::ProxySetting{}) ? 1 : 0);
    }
    int n = 0;
    for (const auto& def : config::AllSettings())
        if (def.group == section && store_.Get(def).source != Source::Default) ++n;
    return n;
}

void SettingsPanel::ScrollTo(std::string section)
{
    scrollTarget_ = section;
    pinned_       = std::move(section);
    pinGrace_     = 3; // the jump itself takes a frame or two to land
}

const std::string& SettingsPanel::VisibleSection() const
{
    return pinned_.empty() ? visibleSection_ : pinned_;
}

void SettingsPanel::SectionAnchor(const std::string& section)
{
    if (scrollTarget_ == section)
    {
        ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
        scrollTarget_.clear();
    }
    if (ImGui::GetCursorScreenPos().y <= rowsTop_ + ImGui::GetFrameHeight() || visibleSection_.empty())
        visibleSection_ = section;
    lastSection_ = section;
}

void SettingsPanel::Draw()
{
    store_.PollExternalChanges();
    if (store_.Revision() != seenRevision_)
    {
        // Running Claude Code sessions rewrite ~/.claude.json constantly
        // (counters, caches); only speak up when a value shown here moved.
        std::string signature = ValuesSignature();
        if (!shownSignature_.empty() && signature != shownSignature_)
        {
            textEdits_.clear();
            notice_ = "Настройки изменились извне — значения перечитаны";
            noticeUntil_ = ImGui::GetTime() + 4.0;
        }
        shownSignature_ = std::move(signature);
        seenRevision_ = store_.Revision();
    }

    const auto& style = ImGui::GetStyle();
    const float fs    = ImGui::GetFontSize();
    const float maxW  = std::min(ImGui::GetContentRegionAvail().x, fs * 64.0f);

    // ── toolbar: search · only changed · the two files · help ──
    {
        const char* userFile   = "settings.json \xe2\x86\x97";
        const char* globalFile = ".claude.json \xe2\x86\x97";
        const char* onlyLabel  = "Только заданные";
        const float rightW = widgets::ToggleWidth() + 6.0f + ImGui::CalcTextSize(onlyLabel).x + 16.0f +
                             ImGui::CalcTextSize(userFile).x + ImGui::CalcTextSize(globalFile).x + style.FramePadding.x * 4 +
                             style.ItemSpacing.x * 2 + ImGui::CalcTextSize("(?)").x + style.ItemSpacing.x;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {style.FramePadding.x + 4.0f, style.FramePadding.y + 3.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::SetNextItemWidth(std::max(fs * 10.0f, maxW - rightW - 16.0f));
        filter_.Draw("##settings_search", 0.0f);
        if (!filter_.IsActive() && !ImGui::IsItemActive())
        {
            ImVec2 r = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText({r.x + style.FramePadding.x + 4.0f, r.y + style.FramePadding.y + 3.0f},
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                icons::Label(icons::Icon::Search, "Найти параметр: название, описание или ключ"));
        }
        ImGui::PopStyleVar(2);
        const float barH = ImGui::GetItemRectSize().y;
        const float rowY = ImGui::GetItemRectMin().y;
        ImGui::SameLine(0, 16.0f);
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, rowY + (barH - ImGui::GetFrameHeight()) * 0.5f});
        widgets::Toggle("##only_changed", &onlyChanged_);
        ImGui::SameLine(0, 6.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(onlyChanged_ ? theme::palette::Light : theme::palette::MidGray, "%s", onlyLabel);
        ImGui::SameLine(0, 16.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(widgets::kOutline));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::MidGray);
        if (ImGui::Button(userFile)) OpenWithDesktop(store_.PathOf(config::Store::UserSettings));
        ImGui::SetItemTooltip("Открыть %s в редакторе", store_.PathOf(config::Store::UserSettings).c_str());
        ImGui::SameLine();
        if (ImGui::Button(globalFile)) OpenWithDesktop(store_.PathOf(config::Store::GlobalConfig));
        ImGui::SetItemTooltip("Открыть %s в редакторе", store_.PathOf(config::Store::GlobalConfig).c_str());
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::SameLine();
        HelpMarker("Значения пишутся туда же, куда их пишет сам Claude Code: ~/.claude/settings.json или ~/.claude.json. "
                   "Запись идёт под тем же lock-файлом, что использует Claude Code, атомарно (временный файл + rename), "
                   "все прочие ключи сохраняются. Перед первым изменением за запуск делается копия в "
                   "~/.claude/claudeui-backups (хранятся последние 10).\n\n"
                   "Новые сессии всегда стартуют с новыми значениями; уже запущенные могут подхватить не все сразу.");
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, std::max(ImGui::GetCursorScreenPos().y, rowY + barH + 4.0f)});
    }
    ImGui::Dummy({0, 6.0f});

    // Errors stay in the layout until the next successful write; notices float (below).
    ImGui::PushItemWidth(maxW);
    for (auto store : {config::Store::UserSettings, config::Store::GlobalConfig})
        if (const auto& e = store_.LoadError(store); !e.empty())
        {
            const std::string msg = std::string("Не удалось прочитать ") + FileLabel(store) + ": " + e;
            widgets::Banner(msg.c_str(), kPillErrBg, kPillErrFg);
            ImGui::Dummy({0, 6.0f});
        }
    if (!error_.empty())
    {
        widgets::Banner(error_.c_str(), kPillErrBg, kPillErrFg);
        ImGui::Dummy({0, 6.0f});
    }
    ImGui::PopItemWidth();

    // ── cards ──
    ImGui::BeginChild("##settings_rows");
    rowsTop_ = ImGui::GetWindowPos().y;
    visibleSection_.clear();
    const float cardW = std::min(ImGui::GetContentRegionAvail().x, maxW);
    DrawAppSection(cardW);
    const auto& all = config::AllSettings();
    for (const auto& group : config::SettingGroups())
    {
        std::vector<const config::SettingDef*> rows;
        for (const auto& def : all)
        {
            if (def.group != group) continue;
            if (filter_.IsActive() && !filter_.PassFilter(def.label.c_str()) &&
                !filter_.PassFilter(def.description.c_str()) && !filter_.PassFilter(JoinPath(def.path).c_str()))
                continue;
            if (onlyChanged_ && store_.Get(def).source == Source::Default) continue;
            rows.push_back(&def);
        }
        if (rows.empty()) continue;

        SectionAnchor(group);
        widgets::CardBegin(("##section_" + group).c_str(), group.c_str(), cardW);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (i > 0) widgets::CardDivider();
            DrawRow(*rows[i]);
        }
        widgets::CardEnd();
        ImGui::Dummy({0, 14.0f});
    }
    if (filter_.IsActive() && visibleSection_.empty())
        ImGui::TextColored(theme::palette::MidGray, "Ничего не нашлось.");
    // Room under the last card, so any section can be scrolled to the top.
    ImGui::Dummy({0, std::max(0.0f, ImGui::GetWindowHeight() - fs * 14.0f)});

    // Scrolled to the very end: the last sections can't reach the top, so
    // the last one counts as the visible one.
    if (ImGui::GetScrollMaxY() > 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) visibleSection_ = lastSection_;
    // A section picked in the navigation stays highlighted until the list is scrolled by hand.
    if (!pinned_.empty())
    {
        if (pinGrace_ > 0)
        {
            --pinGrace_;
            pinnedScrollY_ = ImGui::GetScrollY();
        }
        else if (std::fabs(ImGui::GetScrollY() - pinnedScrollY_) > 0.5f) pinned_.clear();
    }
    ImGui::EndChild();

    // Notice: a short-lived strip floating at the bottom of the list, so it
    // doesn't push the cards around while you click.
    if (ImGui::GetTime() < noticeUntil_ && error_.empty() && !notice_.empty())
    {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const std::string text = "\xe2\x9c\x93  " + notice_;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const float  pad = 14.0f;
        const ImVec2 a{lo.x, hi.y - ts.y - pad * 2 - 12.0f};
        const ImVec2 b{std::min(hi.x, a.x + ts.x + pad * 2), hi.y - 12.0f};
        auto* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(a, b, IM_COL32(0x1f, 0x24, 0x19, 245), 10.0f);
        dl->AddRect(a, b, IM_COL32(0x3a, 0x45, 0x2c, 255), 10.0f);
        dl->AddText({a.x + pad, a.y + pad}, IM_COL32(0xa9, 0xbb, 0x8d, 255), text.c_str());
    }
}
} // namespace ui
