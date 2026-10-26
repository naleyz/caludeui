// Exercises SettingsStore against whatever $HOME points at. Run it with HOME
// set to a scratch copy of ~/.claude.json + ~/.claude/settings.json.
//
//   config_probe dump        print every setting's effective value and source
//   config_probe roundtrip   no-op rewrite must leave both files byte-identical
//   config_probe mutate      set/reset a few settings and verify other keys survive
#include "config/json_file.hpp"
#include "config/model_access.hpp"
#include "config/model_catalog.hpp"
#include "config/settings_store.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string Slurp(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const char* SourceName(config::SettingsStore::Source s)
{
    using S = config::SettingsStore::Source;
    switch (s)
    {
    case S::Default:      return "default";
    case S::UserSettings: return "settings.json";
    case S::GlobalConfig: return ".claude.json";
    case S::LegacyGlobal: return ".claude.json (legacy)";
    case S::ForcedOff:    return "forced off";
    }
    return "?";
}

const config::SettingDef& Def(const std::string& id)
{
    for (const auto& d : config::AllSettings())
        if (d.id == id) return d;
    std::fprintf(stderr, "no setting %s\n", id.c_str());
    std::exit(2);
}

int Fail(const char* what)
{
    std::printf("FAIL: %s\n", what);
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "dump";
    config::SettingsStore store;
    auto userPath   = store.PathOf(config::Store::UserSettings);
    auto globalPath = store.PathOf(config::Store::GlobalConfig);

    if (mode == "dump")
    {
        for (const auto& def : config::AllSettings())
        {
            auto v = store.Get(def);
            std::printf("%-42s %-24s %s%s\n", def.label.c_str(), v.value.dump().c_str(), SourceName(v.source),
                        v.invalid ? "  [invalid]" : "");
        }
        return 0;
    }

    if (mode == "models")
    {
        auto t0 = std::chrono::steady_clock::now();
        auto catalog = config::LoadModelCatalog();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::string err;
        auto global = config::ReadJsonObject(store.PathOf(config::Store::GlobalConfig), &err).value_or(config::Json::object());
        auto user   = config::ReadJsonObject(store.PathOf(config::Store::UserSettings), &err).value_or(config::Json::object());
        auto access = config::ReadModelAccess(global, user);
        std::printf("source: %s (%lld ms); provider switch: %s; offers:", catalog.source.c_str(),
                    static_cast<long long>(ms), access.providerSwitch.empty() ? "none" : access.providerSwitch.c_str());
        for (const auto& o : access.offers) std::printf(" %s%s", o.modelId.c_str(), o.disabled ? "(disabled)" : "");
        std::printf("\n");
        for (const auto& m : catalog.models)
        {
            auto why = access.Unavailable(m);
            std::printf("  %-8s %-20s %-12s native1m=%d suffix1m=%d legacy=%d %s%s\n", m.family.c_str(), m.id.c_str(),
                        m.displayName.c_str(), m.native1m, m.suffix1m, m.legacy, why ? "UNAVAILABLE: " : "available",
                        why ? why->c_str() : "");
        }
        return catalog.models.empty() ? 1 : 0;
    }

    if (mode == "roundtrip")
    {
        for (const auto& p : {userPath, globalPath})
        {
            std::string before = Slurp(p);
            std::string err;
            bool ok = config::UpdateJsonFile(p, [](config::Json&) { return true; }, &err);
            std::string after = Slurp(p);
            std::printf("%s: ok=%d identical=%d %s\n", p.c_str(), ok, before == after, err.c_str());
            if (!ok || before != after) return 1;
        }
        return 0;
    }

    if (mode == "mutate")
    {
        std::string err;
        auto globalBefore = config::ReadJsonObject(globalPath, &err).value();
        auto userBefore   = config::ReadJsonObject(userPath, &err).value();

        // Nested key + OmitDefault + Global store + clearAlso.
        if (!store.Set(Def("permissionMode"), "plan", &err)) return Fail(err.c_str());
        if (store.Get(Def("permissionMode")).value != "plan") return Fail("permissionMode not plan");
        if (!store.Set(Def("thinking"), false, &err)) return Fail(err.c_str());
        if (store.Get(Def("thinking")).value != false) return Fail("thinking not false");
        if (!store.Set(Def("thinking"), true, &err)) return Fail(err.c_str());
        {
            auto u = config::ReadJsonObject(userPath, &err).value();
            if (u.contains("alwaysThinkingEnabled")) return Fail("OmitDefault left the key behind");
        }
        if (!store.Set(Def("copyOnSelect"), false, &err)) return Fail(err.c_str());
        if (!store.Set(Def("workflows"), true, &err)) return Fail(err.c_str());
        if (!store.Set(Def("language"), "russian", &err)) return Fail(err.c_str());

        auto g = config::ReadJsonObject(globalPath, &err).value();
        if (g["copyOnSelect"] != false) return Fail("copyOnSelect not written to .claude.json");
        for (auto it = globalBefore.begin(); it != globalBefore.end(); ++it)
            if (it.key() != "copyOnSelect" && (!g.contains(it.key()) || g[it.key()] != it.value()))
                return Fail(("global key changed: " + it.key()).c_str());

        // Reset everything we touched: files must match the originals semantically.
        for (const char* id : {"permissionMode", "copyOnSelect", "workflows", "language"})
            if (!store.Reset(Def(id), &err)) return Fail(err.c_str());

        auto u = config::ReadJsonObject(userPath, &err).value();
        g      = config::ReadJsonObject(globalPath, &err).value();
        if (userBefore.contains("copyOnSelect") || globalBefore.contains("copyOnSelect"))
            std::printf("note: copyOnSelect existed before; reset removed it (expected)\n");
        userBefore.erase("permissions");
        u.erase("permissions");
        globalBefore.erase("copyOnSelect");
        if (u != userBefore) return Fail("settings.json differs after reset");
        if (g != globalBefore) return Fail("claude.json differs after reset");
        std::printf("mutate: OK\n");
        return 0;
    }
    return 2;
}
