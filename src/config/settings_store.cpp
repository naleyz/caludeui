#include "config/settings_store.hpp"
#include "config/paths.hpp"

namespace config
{
namespace
{
using namespace std::chrono_literals;

const Json* Lookup(const Json& root, const KeyPath& path)
{
    const Json* cur = &root;
    for (const auto& key : path)
    {
        if (!cur->is_object()) return nullptr;
        auto it = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &*it;
    }
    return cur;
}

// Removes the leaf, then any parent objects the removal left empty.
void ErasePath(Json& root, const KeyPath& path)
{
    if (path.empty()) return;
    std::vector<Json*> chain{&root};
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
    {
        Json* cur = chain.back();
        if (!cur->is_object()) return;
        auto it = cur->find(path[i]);
        if (it == cur->end()) return;
        chain.push_back(&*it);
    }
    if (!chain.back()->is_object() || chain.back()->erase(path.back()) == 0) return;
    for (std::size_t i = chain.size() - 1; i > 0; --i)
    {
        if (!chain[i]->is_object() || !chain[i]->empty()) break;
        chain[i - 1]->erase(path[i - 1]);
    }
}

bool SetPath(Json& root, const KeyPath& path, Json value, std::string* error)
{
    Json* cur = &root;
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
    {
        auto it = cur->find(path[i]);
        if (it == cur->end()) it = cur->emplace(path[i], Json::object()).first;
        if (!it->is_object())
        {
            *error = "ключ «" + path[i] + "» в файле не является объектом — правьте файл вручную";
            return false;
        }
        cur = &*it;
    }
    (*cur)[path.back()] = std::move(value);
    return true;
}

bool TypeMatches(const SettingDef& def, const Json& v)
{
    return def.kind == Kind::Bool ? v.is_boolean() : v.is_string();
}

nlohmann::json ToPlain(const Json& v)
{
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) return v.get<std::string>();
    return nullptr;
}

Json ToOrdered(const nlohmann::json& v)
{
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) return v.get<std::string>();
    return nullptr;
}
} // namespace

SettingsStore::SettingsStore()
    : userSettingsPath_(ClaudeDir() / "settings.json"), globalConfigPath_(GlobalConfigFile())
{
    Reload();
}

SettingsStore::FileStamp SettingsStore::StampOf(const std::filesystem::path& p)
{
    FileStamp s;
    std::error_code ec;
    auto status = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::exists(status)) return s;
    s.exists = true;
    s.mtime  = std::filesystem::last_write_time(p, ec);
    s.size   = std::filesystem::file_size(p, ec);
    return s;
}

void SettingsStore::LoadOne(Store store)
{
    const auto& path  = PathOf(store);
    auto&       json  = store == Store::UserSettings ? user_ : global_;
    auto&       err   = store == Store::UserSettings ? userLoadError_ : globalLoadError_;
    auto&       stamp = store == Store::UserSettings ? userStamp_ : globalStamp_;

    stamp = StampOf(path);
    ++revision_;
    std::string e;
    if (auto j = ReadJsonObject(path, &e))
    {
        json = std::move(*j);
        err.clear();
    }
    else
    {
        err = e; // keep the last good copy for display
    }
}

void SettingsStore::Reload()
{
    LoadOne(Store::UserSettings);
    LoadOne(Store::GlobalConfig);
}

bool SettingsStore::PollExternalChanges()
{
    auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ < 1s) return false;
    lastPoll_ = now;

    bool reloaded = false;
    if (StampOf(userSettingsPath_) != userStamp_)
    {
        LoadOne(Store::UserSettings);
        reloaded = true;
    }
    if (StampOf(globalConfigPath_) != globalStamp_)
    {
        LoadOne(Store::GlobalConfig);
        reloaded = true;
    }
    return reloaded;
}

SettingsStore::Value SettingsStore::Get(const SettingDef& def) const
{
    Value out;
    if (!def.forcedOffBy.empty())
    {
        const Json& root = def.store == Store::UserSettings ? user_ : global_;
        if (const Json* off = Lookup(root, def.forcedOffBy); off && off->is_boolean() && off->get<bool>())
        {
            out.value  = false;
            out.source = Source::ForcedOff;
            return out;
        }
    }

    const Json& primary = def.store == Store::UserSettings ? user_ : global_;
    if (const Json* v = Lookup(primary, def.path))
    {
        if (TypeMatches(def, *v))
        {
            out.value  = ToPlain(*v);
            out.source = def.store == Store::UserSettings ? Source::UserSettings : Source::GlobalConfig;
            return out;
        }
        out.invalid = true;
    }
    if (def.legacyGlobalFallback && def.store == Store::UserSettings)
    {
        if (const Json* v = Lookup(global_, def.path); v && TypeMatches(def, *v))
        {
            out.value  = ToPlain(*v);
            out.source = Source::LegacyGlobal;
            return out;
        }
    }
    out.value  = def.defaultValue;
    out.source = Source::Default;
    return out;
}

bool SettingsStore::Set(const SettingDef& def, const nlohmann::json& value, std::string* error)
{
    error->clear();
    if (!LoadError(def.store).empty())
    {
        LoadOne(def.store);
        if (!LoadError(def.store).empty())
        {
            *error = LoadError(def.store);
            return false;
        }
    }

    bool ok = UpdateJsonFile(
        PathOf(def.store),
        [&](Json& root) {
            for (const auto& extra : def.clearAlso) ErasePath(root, extra);
            const bool isDefault = !def.defaultValue.is_null() && value == def.defaultValue;
            if (def.writeMode == WriteMode::OmitDefault && isDefault)
            {
                ErasePath(root, def.path);
                return true;
            }
            return SetPath(root, def.path, ToOrdered(value), error);
        },
        error);
    LoadOne(def.store);
    return ok && error->empty();
}

bool SettingsStore::Reset(const SettingDef& def, std::string* error)
{
    error->clear();
    bool ok = UpdateJsonFile(
        PathOf(def.store),
        [&](Json& root) {
            for (const auto& extra : def.clearAlso) ErasePath(root, extra);
            ErasePath(root, def.path);
            return true;
        },
        error);
    LoadOne(def.store);
    return ok;
}
} // namespace config
