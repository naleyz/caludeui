#include "ui/model_picker.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>

namespace ui
{
namespace
{
constexpr std::string_view k1mSuffix = "[1m]";

struct Parsed
{
    enum class Kind { Unset, Alias, Exact, Special, Custom } kind = Kind::Unset;
    std::string family;                       // Alias / Exact
    const config::ModelInfo* model = nullptr; // Exact
    std::string base;                         // value without [1m]
    bool oneM = false;
};

std::string Capitalized(std::string s)
{
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

Parsed Parse(const std::string& value, const config::ModelCatalog& catalog, const char* unsetValue)
{
    Parsed p;
    if (value.empty() || value == unsetValue) return p;

    p.base = value;
    if (p.base.size() > k1mSuffix.size())
    {
        std::string tail = p.base.substr(p.base.size() - k1mSuffix.size());
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
        if (tail == k1mSuffix)
        {
            p.oneM = true;
            p.base.resize(p.base.size() - k1mSuffix.size());
        }
    }

    if (config::FamilyHasAlias(p.base))
    {
        p.kind = Parsed::Kind::Alias;
        p.family = p.base;
        return p;
    }
    // Also recognises date-stamped provider ids, e.g. claude-haiku-4-5-20251001.
    const std::string canonical = config::CanonicalModelId(p.base);
    if (auto* m = catalog.Find(canonical); m || (m = catalog.Find(canonical + "-0")))
    {
        p.kind = Parsed::Kind::Exact;
        p.model = m;
        p.family = m->family;
        return p;
    }
    p.kind = p.base == "opusplan" || p.base == "best" ? Parsed::Kind::Special : Parsed::Kind::Custom;
    return p;
}

// Whether "[1m]" means anything for this choice. Claude Code ships
// opus[1m] / sonnet[1m] / fable[1m] aliases, and per-model support flags.
bool Can1M(const Parsed& p)
{
    if (p.kind == Parsed::Kind::Alias) return p.family != "haiku";
    if (p.kind == Parsed::Kind::Exact) return p.model->suffix1m && !p.model->native1m;
    return false;
}

std::string Compose(const Parsed& p)
{
    return p.base + (p.oneM && Can1M(p) ? std::string(k1mSuffix) : std::string{});
}

std::string Preview(const Parsed& p, const char* unsetLabel, bool unavailable)
{
    std::string s;
    switch (p.kind)
    {
    case Parsed::Kind::Unset:   return unsetLabel;
    case Parsed::Kind::Alias:   s = Capitalized(p.family) + " (последняя)"; break;
    case Parsed::Kind::Exact:   s = p.model->displayName; break;
    case Parsed::Kind::Special:
    case Parsed::Kind::Custom:  s = p.base; break;
    }
    if (p.oneM) s += " · 1M";
    if (unavailable) s += " (недоступна)";
    return s;
}
} // namespace

bool ModelCombo(const char* id, std::string& value, const config::ModelCatalog& catalog,
                const config::ModelAccess& access, const char* unsetLabel, const char* unsetValue, bool showLegacy)
{
    Parsed cur = Parse(value, catalog, unsetValue);
    const std::optional<std::string> curUnavailable =
        cur.kind == Parsed::Kind::Exact ? access.Unavailable(*cur.model) : std::nullopt;
    std::string preview = Preview(cur, unsetLabel, curUnavailable.has_value());
    bool changed = false;
    auto commit = [&](std::string next) {
        if (next != value)
        {
            value = std::move(next);
            changed = true;
        }
    };
    // A family switch keeps the 1M choice where the new target supports it.
    auto choose = [&](Parsed next) {
        next.oneM = cur.oneM;
        commit(Compose(next));
    };

    ImGui::PushID(id);
    if (ImGui::BeginCombo("##model", preview.c_str(), ImGuiComboFlags_HeightLarge))
    {
        static std::unordered_map<ImGuiID, std::string> customBuffers;
        std::string& custom = customBuffers[ImGui::GetID("custom")];
        if (ImGui::IsWindowAppearing()) custom = cur.kind == Parsed::Kind::Custom ? cur.base : std::string{};

        if (ImGui::Selectable(unsetLabel, cur.kind == Parsed::Kind::Unset)) commit(unsetValue);
        ImGui::Separator();

        for (const auto& family : config::ModelFamilies())
        {
            auto current = catalog.Family(family, false);
            std::vector<const config::ModelInfo*> legacy;
            if (showLegacy)
                for (const auto* m : catalog.Family(family, true))
                    if (m->legacy) legacy.push_back(m);
            if (current.empty() && legacy.empty()) continue;

            // A family is greyed out when Claude Code would refuse every version in it;
            // its menu still opens, so the reason can be read on each item.
            std::optional<std::string> familyBlocked;
            bool anyAvailable = false;
            for (const auto* list : {&current, &legacy})
                for (const auto* m : *list)
                {
                    if (auto why = access.Unavailable(*m)) familyBlocked = familyBlocked.value_or(*why);
                    else anyAvailable = true;
                }
            if (anyAvailable) familyBlocked.reset();

            const bool inFamily = (cur.kind == Parsed::Kind::Alias || cur.kind == Parsed::Kind::Exact) &&
                                  cur.family == family;
            const std::string menuLabel = Capitalized(family) + (inFamily ? "  \xe2\x9c\x93" : "") +
                                          (familyBlocked ? "  — недоступен" : "") + "###" + family;
            if (familyBlocked) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool open = ImGui::BeginMenu(menuLabel.c_str());
            if (familyBlocked) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !open)
            {
                if (familyBlocked) ImGui::SetTooltip("%s", familyBlocked->c_str());
                else if (family == "fable")
                    ImGui::SetTooltip("Для Fable Claude Code может попросить согласие на usage credits — подтвердите в /model.");
            }
            if (!open) continue;

            auto versionItem = [&](const config::ModelInfo* m) {
                const auto why = access.Unavailable(*m);
                const char* ctx = m->native1m ? "1M" : m->suffix1m ? "1M по [1m]" : "200K";
                if (ImGui::MenuItem(m->displayName.c_str(), ctx, cur.kind == Parsed::Kind::Exact && cur.model == m,
                                    !why.has_value()))
                {
                    Parsed next;
                    next.kind = Parsed::Kind::Exact;
                    next.model = m;
                    next.family = m->family;
                    next.base = m->id;
                    choose(next);
                }
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                    ImGui::Text("%s", m->id.c_str());
                    ImGui::TextDisabled("Контекст: %s",
                                        m->native1m ? "1M встроен" : m->suffix1m ? "200K, 1M с суффиксом [1m]" : "200K");
                    if (m->legacy)
                        ImGui::TextDisabled("Устаревшее поколение: API может уже не обслуживать эту модель — тогда "
                                            "Claude Code сообщит об ошибке при первом запросе.");
                    if (why) ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "Недоступна: %s", why->c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            };

            if (!current.empty())
            {
                if (config::FamilyHasAlias(family))
                {
                    std::string latest = "Последняя — " + current.front()->displayName + " (алиас «" + family + "»)";
                    if (ImGui::MenuItem(latest.c_str(), nullptr, cur.kind == Parsed::Kind::Alias && cur.family == family,
                                        !familyBlocked.has_value()))
                    {
                        Parsed next;
                        next.kind = Parsed::Kind::Alias;
                        next.family = next.base = family;
                        choose(next);
                    }
                    ImGui::SetItemTooltip("Всегда самая новая версия %s: после выхода новой модели переключится сам.",
                                          Capitalized(family).c_str());
                    ImGui::Separator();
                }
                for (const auto* m : current) versionItem(m);
            }
            if (!legacy.empty())
            {
                ImGui::SeparatorText("Устаревшие");
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                for (const auto* m : legacy) versionItem(m);
                ImGui::PopStyleColor();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("opusplan", "Opus в plan mode, иначе Sonnet", cur.base == "opusplan"))
            commit("opusplan");
        if (ImGui::MenuItem("best", nullptr, cur.base == "best")) commit("best");

        ImGui::Separator();
        bool oneM = cur.oneM;
        const bool can = Can1M(cur);
        ImGui::BeginDisabled(!can);
        if (ImGui::Checkbox("1M контекст ([1m])", &oneM) && can)
        {
            cur.oneM = oneM;
            commit(Compose(cur));
        }
        ImGui::EndDisabled();
        if (cur.kind == Parsed::Kind::Exact && cur.model->native1m)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("у %s 1M встроен", cur.model->displayName.c_str());
        }

        ImGui::Separator();
        ImGui::TextDisabled("Свой ID модели (Enter):");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::InputTextWithHint("##custom", "claude-… или ARN", &custom, ImGuiInputTextFlags_EnterReturnsTrue) &&
            !custom.empty())
        {
            commit(custom);
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Список моделей: %s", catalog.source.c_str());
        ImGui::EndCombo();
    }
    if (curUnavailable) ImGui::SetItemTooltip("%s\nНедоступна: %s", value.c_str(), curUnavailable->c_str());
    else if (cur.kind == Parsed::Kind::Exact || cur.kind == Parsed::Kind::Alias)
        ImGui::SetItemTooltip("%s", value.c_str());
    ImGui::PopID();
    return changed;
}
} // namespace ui
