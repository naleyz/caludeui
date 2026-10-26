#include "config/model_access.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace config
{
namespace
{
// Environment switches that move Claude Code off the first-party API.
constexpr const char* kProviderSwitches[] = {
    "CLAUDE_CODE_USE_BEDROCK",       "CLAUDE_CODE_USE_VERTEX",  "CLAUDE_CODE_USE_FOUNDRY",
    "CLAUDE_CODE_USE_ANTHROPIC_AWS", "CLAUDE_CODE_USE_ANTHROPIC_GOOGLE_CLOUD",
    "CLAUDE_CODE_USE_GATEWAY",       "CLAUDE_CODE_USE_MANTLE",
};

bool Truthy(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    return !v.empty() && v != "0" && v != "false" && v != "no" && v != "off";
}
} // namespace

std::string CanonicalModelId(std::string id)
{
    if (id.size() > 4)
    {
        std::string tail = id.substr(id.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
        if (tail == "[1m]") id.resize(id.size() - 4);
    }
    if (id.size() > 9 && id[id.size() - 9] == '-' &&
        std::all_of(id.end() - 8, id.end(), [](unsigned char c) { return std::isdigit(c); }))
        id.resize(id.size() - 9);
    return id;
}

ModelAccess ReadModelAccess(const Json& globalConfig, const Json& userSettings)
{
    ModelAccess access;

    // Provider: process environment first, then the settings.json "env" block
    // Claude Code applies at startup.
    for (const char* name : kProviderSwitches)
    {
        std::string value;
        if (const char* v = std::getenv(name)) value = v;
        else if (auto env = userSettings.find("env"); env != userSettings.end() && env->is_object())
            if (auto it = env->find(name); it != env->end() && it->is_string()) value = it->get<std::string>();
        if (Truthy(value))
        {
            access.providerSwitch = name;
            break;
        }
    }

    if (auto cache = globalConfig.find("additionalModelOptionsCache");
        cache != globalConfig.end() && cache->is_array())
    {
        for (const auto& entry : *cache)
        {
            if (!entry.is_object()) continue;
            auto value = entry.find("value");
            if (value == entry.end() || !value->is_string()) continue;
            ModelAccess::Offer offer;
            offer.modelId = CanonicalModelId(value->get<std::string>());
            if (auto label = entry.find("label"); label != entry.end() && label->is_string())
                offer.label = label->get<std::string>();
            if (auto d = entry.find("disabled"); d != entry.end() && d->is_boolean()) offer.disabled = d->get<bool>();
            access.offers.push_back(std::move(offer));
        }
    }
    return access;
}

std::optional<std::string> ModelAccess::Unavailable(const ModelInfo& model) const
{
    if (model.family == "mythos")
    {
        if (!providerSwitch.empty())
            return "Mythos доступен только через API Anthropic напрямую, а сейчас включён " + providerSwitch + ".";

        // Offers for this exact version decide; otherwise any Mythos offer does.
        std::vector<const Offer*> exact, family;
        for (const auto& o : offers)
        {
            if (!o.modelId.starts_with("claude-mythos-")) continue;
            family.push_back(&o);
            if (o.modelId == model.id) exact.push_back(&o);
        }
        const auto& decisive = exact.empty() ? family : exact;
        if (decisive.empty())
            return "Claude Code не предлагает Mythos этому аккаунту: модель доступна только одобренным организациям. "
                   "Общедоступная версия той же модели — Fable.";
        if (std::none_of(decisive.begin(), decisive.end(), [](const Offer* o) { return !o->disabled; }))
            return "Claude Code отметил Mythos как недоступный для этого аккаунта.";
        return std::nullopt;
    }

    for (const auto& o : offers)
        if (o.modelId == model.id && o.disabled) return "Claude Code отметил эту модель как недоступную для аккаунта.";
    return std::nullopt;
}
} // namespace config
