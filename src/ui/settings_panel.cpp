#include "ui/settings_panel.hpp"
#include "config/paths.hpp"
#include "theme/theme.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/model_picker.hpp"
#include "ui/util.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>

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

bool SettingsPanel::DrawControl(const config::SettingDef& def, const config::SettingsStore::Value& v)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
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
        if (ImGui::Checkbox(b ? "вкл##v" : "выкл##v", &b))
        {
            Apply(def, b);
            return true;
        }
        return false;
    }

    case config::Kind::Enum:
    {
        std::string current = v.value.is_string() ? v.value.get<std::string>() : std::string{};
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
        ImGui::SetNextItemWidth(-arrowW - ImGui::GetStyle().ItemInnerSpacing.x);
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

    ImGui::PushID(def.id.c_str());
    ImGui::TableNextRow();

    // ── label ──
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (explicitlySet) ImGui::TextColored(theme::palette::Light, "%s", def.label.c_str());
    else ImGui::TextColored(theme::palette::LightGray, "%s", def.label.c_str());
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

    // ── control ──
    ImGui::TableSetColumnIndex(1);
    DrawControl(def, v);

    // ── source + reset ──
    ImGui::TableSetColumnIndex(2);
    ImGui::AlignTextToFramePadding();
    switch (v.source)
    {
    case Source::UserSettings:
    case Source::GlobalConfig:
        ImGui::TextColored(theme::palette::Orange, "задано");
        ImGui::SetItemTooltip("Явно указано в %s", FileLabel(def.store));
        break;
    case Source::LegacyGlobal:
        ImGui::TextColored(theme::palette::Blue, "задано*");
        ImGui::SetItemTooltip("Взято из ~/.claude.json (старое место хранения). Новое значение запишется в "
                              "~/.claude/settings.json и будет главнее.");
        break;
    case Source::ForcedOff:
        ImGui::TextColored(theme::palette::Blue, "выкл.");
        ImGui::SetItemTooltip("Выключено ключом %s: true. «Сброс» уберёт его.", JoinPath(def.forcedOffBy).c_str());
        break;
    case Source::Default:
        ImGui::TextDisabled("умолч.");
        ImGui::SetItemTooltip("Ключа нет в файле — действует значение по умолчанию");
        break;
    }
    if (v.invalid)
    {
        ImGui::SameLine();
        ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "!");
        ImGui::SetItemTooltip("В файле у ключа неверный тип — Claude Code его проигнорирует. Выберите значение или сбросьте.");
    }
    ImGui::SameLine();
    const bool resettable = (v.source == Source::UserSettings || v.source == Source::GlobalConfig ||
                             v.source == Source::ForcedOff) || v.invalid;
    ImGui::BeginDisabled(!resettable);
    if (ImGui::SmallButton("Сброс")) Reset(def);
    ImGui::EndDisabled();
    if (resettable) ImGui::SetItemTooltip("Удалить ключ из файла — вернётся значение по умолчанию");

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

// Terminal face and size. The face is swapped by main() between frames once the
// setting is saved; the size follows the slider live and is saved on release.
void SettingsPanel::DrawTerminalFont()
{
    if (!fontsScanned_)
    {
        monoFonts_    = config::ScanMonospaceFonts();
        fontsScanned_ = true;
    }
    const config::TerminalFont current = app_.Get().terminalFont;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Шрифт терминала");
    ImGui::SameLine();

    std::string shown = "JetBrains Mono (встроенный)";
    if (!current.IsDefaultFace())
    {
        shown = current.family.empty() ? std::filesystem::path(current.regularPath).filename().string() : current.family;
        const bool listed = current.regularPath == fonts::BundledFontPath("anthropic-mono.ttf") ||
                            std::any_of(monoFonts_.begin(), monoFonts_.end(),
                                        [&](const auto& f) { return f.regular == current.regularPath; });
        if (!listed) shown = "Свой файл: " + shown;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
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
    ImGui::SameLine();
    ImGui::BeginDisabled(current == config::TerminalFont{});
    if (ImGui::Button("Сбросить##terminal_font")) SaveTerminalFont({}, "Шрифт терминала сброшен");
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Размер");
    ImGui::SameLine();
    int size = fontSizeDrag_.value_or(static_cast<int>(current.size + 0.5f));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
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

    // A face that isn't in the list: any .ttf/.otf by path.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Свой файл");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool entered = ImGui::InputTextWithHint("##terminal_font_path", "/путь/к/шрифту.ttf", &fontPathEdit_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(fontPathEdit_.empty());
    const bool clicked = ImGui::Button("Применить##terminal_font_path");
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

    if (!fonts::TerminalFontError().empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.85f, 0.37f, 0.34f, 1.0f});
        ImGui::TextWrapped("%s", fonts::TerminalFontError().c_str());
        ImGui::PopStyleColor();
    }

    // Sample in the face actually in use: Latin, Cyrillic, and the symbols
    // Claude Code's interface prints — so a font that lacks them shows up here.
    // (Box drawing and block elements are drawn by the terminal itself.)
    ImGui::PushFont(fonts::Mono, fonts::TerminalSize);
    const float sampleLine = ImGui::GetTextLineHeightWithSpacing(); // grows with the size slider
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::Dark);
    ImGui::BeginChild("##terminal_font_sample", {0, sampleLine * 3.0f + ImGui::GetStyle().WindowPadding.y * 2.0f},
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(fonts::Mono, fonts::TerminalSize);
    ImGui::TextColored(theme::palette::LightGray, "The quick brown fox 0O1lI |  Съешь ещё этих мягких булок");
    ImGui::TextColored(theme::palette::LightGray, "\xe2\x9d\xaf \xe2\x8f\xba \xe2\x9c\xbb \xe2\x8e\xbf \xe2\x9c\x93 \xe2\x86\x91\xe2\x86\x93 \xe2\x80\xa6 \xe2\x80\x94 \xc2\xab\xc2\xbb");
    ImGui::PushFont(fonts::MonoBold, fonts::TerminalSize);
    ImGui::TextColored(theme::palette::Orange, "Bold: Hello, мир");
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SettingsPanel::DrawAppSection()
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
    if (!legacyVisible && !fontVisible && !usageVisible) return;

    if (!ImGui::CollapsingHeader("ClaudeUI", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SameLine();
    ImGui::TextDisabled("— настройки самой программы, в файлы Claude Code не пишутся");

    if (legacyVisible)
    {
        bool legacy = showLegacy;
        if (ImGui::Checkbox(kLegacyLabel, &legacy))
        {
            std::string err;
            if (app_.SetShowLegacyModels(legacy, &err))
            {
                error_.clear();
                notice_ = legacy ? "Устаревшие модели добавлены в выбор модели" : "Устаревшие модели скрыты";
                noticeUntil_ = ImGui::GetTime() + 4.0;
            }
            else
            {
                error_ = "ClaudeUI: " + err;
            }
        }
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted("Добавляет в подменю Sonnet и Haiku блок «Устаревшие» с моделями поколения 3.x из "
                                   "реестра установленного Claude Code. API может уже не обслуживать их — тогда Claude "
                                   "Code сообщит об ошибке при первом запросе.");
            ImGui::TextColored(theme::palette::MidGray, "Хранится в %s", app_.Path().c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (usageVisible)
    {
        bool on = autoUsage;
        if (ImGui::Checkbox(kUsageLabel, &on))
        {
            std::string err;
            if (app_.SetUsageAutoRefresh(on, &err))
            {
                error_.clear();
                notice_ = on ? "Лимиты будут обновляться в фоне" : "Фоновое обновление лимитов выключено";
                noticeUntil_ = ImGui::GetTime() + 4.0;
            }
            else
            {
                error_ = "ClaudeUI: " + err;
            }
        }
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted("Раз в 5 минут, пока окно ClaudeUI в фокусе, запускает `claude -p /usage`: это локальная "
                                   "команда Claude Code, модель не вызывается и токены не тратятся. Она сама обновляет "
                                   "кэш лимитов в ~/.claude.json, откуда панель «Лимиты и расход» и берёт цифры. "
                                   "Без этого данные обновляются только кнопкой в панели.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (fontVisible) DrawTerminalFont();
    if (!app_.LoadError().empty())
        ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "%s", app_.LoadError().c_str());
    ImGui::Spacing();
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

    // ── header ──
    ImGui::TextColored(theme::palette::MidGray, "Настройки Claude Code (/config)");
    ImGui::SameLine();
    HelpMarker("Значения пишутся туда же, куда их пишет сам Claude Code: ~/.claude/settings.json или ~/.claude.json. "
               "Запись идёт под тем же lock-файлом, что использует Claude Code, атомарно (временный файл + rename), "
               "все прочие ключи сохраняются. Перед первым изменением за запуск делается копия в "
               "~/.claude/claudeui-backups (хранятся последние 10).\n\n"
               "Новые сессии всегда стартуют с новыми значениями; уже запущенные могут подхватить не все сразу.");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                             ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("settings.json").x -
                                 ImGui::CalcTextSize(".claude.json").x - ImGui::GetStyle().FramePadding.x * 4 -
                                 ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::SmallButton("settings.json")) OpenWithDesktop(store_.PathOf(config::Store::UserSettings));
    ImGui::SetItemTooltip("Открыть %s в редакторе", store_.PathOf(config::Store::UserSettings).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(".claude.json")) OpenWithDesktop(store_.PathOf(config::Store::GlobalConfig));
    ImGui::SetItemTooltip("Открыть %s в редакторе", store_.PathOf(config::Store::GlobalConfig).c_str());

    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Только заданные").x - ImGui::GetFrameHeight() -
                            ImGui::GetStyle().ItemSpacing.x * 2);
    filter_.Draw("##settings_search", 0.0f);
    if (!filter_.IsActive() && !ImGui::IsItemActive())
    {
        ImVec2 r = ImGui::GetItemRectMin();
        const auto& fp = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText({r.x + fp.x, r.y + fp.y},
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), icons::Label(icons::Icon::Search, "Поиск настройки"));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Только заданные", &onlyChanged_);

    for (auto store : {config::Store::UserSettings, config::Store::GlobalConfig})
        if (const auto& e = store_.LoadError(store); !e.empty())
            ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "Не удалось прочитать %s: %s", FileLabel(store), e.c_str());
    if (!error_.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "%s", error_.c_str());
        ImGui::PopTextWrapPos();
    }
    else if (ImGui::GetTime() < noticeUntil_)
    {
        ImGui::TextColored(theme::palette::Green, "%s", notice_.c_str());
    }
    ImGui::Separator();

    // ── rows ──
    ImGui::BeginChild("##settings_rows");
    DrawAppSection();
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

        if (!ImGui::CollapsingHeader(group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        if (ImGui::BeginTable(group.c_str(), 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
            ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("умолч. Сброс").x + ImGui::GetStyle().FramePadding.x * 2 +
                                        ImGui::GetStyle().ItemSpacing.x);
            for (const auto* def : rows) DrawRow(*def);
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}
} // namespace ui
