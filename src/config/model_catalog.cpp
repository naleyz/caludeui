#include "config/model_catalog.hpp"
#include "term/terminal.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <unordered_set>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace config
{
namespace
{
int FamilyRank(std::string_view f)
{
    const auto& fams = ModelFamilies();
    auto it = std::find(fams.begin(), fams.end(), f);
    return it == fams.end() ? 99 : static_cast<int>(it - fams.begin());
}

// Parses "1" or "1-2" (one- or two-digit parts) into major/minor. Rejects
// date stamps ("20251001"): the registry's canonical ids never carry them.
bool ParseVersionPart(std::string_view s, int& major, int& minor)
{
    major = minor = 0;
    std::size_t i = 0;
    auto number = [&](int& out) {
        std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') out = out * 10 + (s[i++] - '0');
        return i > start && i - start <= 2;
    };
    if (!number(major)) return false;
    if (i == s.size()) return true;
    if (s[i] != '-') return false;
    ++i;
    return number(minor) && i == s.size();
}

// Claude 4+ ids put the family first ("claude-opus-4-8", "claude-sonnet-5");
// the 3.x generation put the version first ("claude-3-7-sonnet").
bool ParseVersion(std::string_view id, std::string_view family, int& major, int& minor)
{
    const std::string familyFirst = "claude-" + std::string(family) + "-";
    if (id.starts_with(familyFirst)) return ParseVersionPart(id.substr(familyFirst.size()), major, minor);

    const std::string familyLast = "-" + std::string(family);
    if (id.starts_with("claude-") && id.ends_with(familyLast))
        return ParseVersionPart(id.substr(7, id.size() - 7 - familyLast.size()), major, minor);
    return false;
}

void Normalize(ModelCatalog& c)
{
    // Only families the pickers know. Pre-4 generations stay in, flagged as
    // legacy; whether an account may use a model is ModelAccess's call.
    std::erase_if(c.models, [](const ModelInfo& m) { return FamilyRank(m.family) == 99; });
    for (auto& m : c.models) m.legacy = m.major < 4;
    std::sort(c.models.begin(), c.models.end(), [](const ModelInfo& a, const ModelInfo& b) {
        if (a.family != b.family) return FamilyRank(a.family) < FamilyRank(b.family);
        if (a.major != b.major) return a.major > b.major;
        return a.minor > b.minor;
    });
}

// Pulls `key:"value"` out of s at pos, advancing pos past it.
bool Expect(std::string_view s, std::size_t& pos, std::string_view key, std::string& value)
{
    if (s.substr(pos, key.size()) != key) return false;
    pos += key.size();
    auto end = s.find('"', pos);
    if (end == std::string_view::npos || end - pos > 64) return false;
    value.assign(s.substr(pos, end - pos));
    pos = end + 1;
    return true;
}
} // namespace

const std::vector<std::string>& ModelFamilies()
{
    static const std::vector<std::string> kFamilies = {"opus", "sonnet", "haiku", "fable", "mythos"};
    return kFamilies;
}

bool FamilyHasAlias(std::string_view family)
{
    // Claude Code 2.1.x ships sonnet/opus/haiku/fable aliases, none for mythos.
    return family == "opus" || family == "sonnet" || family == "haiku" || family == "fable";
}

const ModelInfo* ModelCatalog::Find(std::string_view id) const
{
    for (const auto& m : models)
        if (m.id == id) return &m;
    return nullptr;
}

std::vector<const ModelInfo*> ModelCatalog::Family(std::string_view family, bool includeLegacy) const
{
    std::vector<const ModelInfo*> out;
    for (const auto& m : models)
        if (m.family == family && (includeLegacy || !m.legacy)) out.push_back(&m);
    return out;
}

ModelCatalog BuiltinModelCatalog()
{
    ModelCatalog c;
    c.source = "встроенный список (Claude Code 2.1.272)";
    auto add = [&](const char* id, const char* family, const char* name, int major, int minor, bool native, bool suffix) {
        c.models.push_back({id, family, name, major, minor, native, suffix});
    };
    add("claude-opus-5", "opus", "Opus 5", 5, 0, true, true);
    add("claude-opus-4-8", "opus", "Opus 4.8", 4, 8, true, true);
    add("claude-opus-4-7", "opus", "Opus 4.7", 4, 7, true, true);
    add("claude-opus-4-6", "opus", "Opus 4.6", 4, 6, false, true);
    add("claude-opus-4-5", "opus", "Opus 4.5", 4, 5, false, true);
    add("claude-opus-4-1", "opus", "Opus 4.1", 4, 1, false, true);
    add("claude-opus-4-0", "opus", "Opus 4", 4, 0, false, true);
    add("claude-sonnet-5", "sonnet", "Sonnet 5", 5, 0, true, false);
    add("claude-sonnet-4-6", "sonnet", "Sonnet 4.6", 4, 6, false, true);
    add("claude-sonnet-4-5", "sonnet", "Sonnet 4.5", 4, 5, false, true);
    add("claude-sonnet-4-0", "sonnet", "Sonnet 4", 4, 0, false, true);
    add("claude-haiku-4-5", "haiku", "Haiku 4.5", 4, 5, false, true);
    add("claude-fable-5-1", "fable", "Fable 5.1", 5, 1, true, false);
    add("claude-fable-5", "fable", "Fable 5", 5, 0, true, false);
    add("claude-mythos-5-1", "mythos", "Mythos 5.1", 5, 1, true, false);
    add("claude-mythos-5", "mythos", "Mythos 5", 5, 0, true, false);
    add("claude-3-7-sonnet", "sonnet", "Sonnet 3.7", 3, 7, false, false);
    add("claude-3-5-sonnet", "sonnet", "Sonnet 3.5", 3, 5, false, false);
    add("claude-3-5-haiku", "haiku", "Haiku 3.5", 3, 5, false, false);
    Normalize(c);
    return c;
}

ModelCatalog LoadModelCatalog()
{
    auto exe = Terminal::FindExecutable("claude");
    if (!exe) return BuiltinModelCatalog();
    std::error_code ec;
    auto path = std::filesystem::canonical(*exe, ec);
    if (ec) return BuiltinModelCatalog();

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return BuiltinModelCatalog();
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (1LL << 31))
    {
        ::close(fd);
        return BuiltinModelCatalog();
    }
    void* map = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return BuiltinModelCatalog();

    const std::string_view data(static_cast<const char*>(map), static_cast<std::size_t>(st.st_size));
    ModelCatalog c;
    std::unordered_set<std::string> seen;

    static constexpr std::string_view kMarker = "{id:\"claude-";
    const std::boyer_moore_horspool_searcher searcher(kMarker.begin(), kMarker.end());
    auto it = data.begin();
    while (true)
    {
        it = std::search(it, data.end(), searcher);
        if (it == data.end()) break;
        std::size_t pos = static_cast<std::size_t>(it - data.begin()) + 5; // at "claude-…"
        ++it;

        ModelInfo m;
        auto idEnd = data.find('"', pos);
        if (idEnd == std::string_view::npos || idEnd - pos > 64) continue;
        m.id.assign(data.substr(pos, idEnd - pos));
        pos = idEnd + 1;
        if (!Expect(data, pos, ",family:\"", m.family) || !Expect(data, pos, ",display_name:\"", m.displayName))
            continue;
        if (!ParseVersion(m.id, m.family, m.major, m.minor) || !seen.insert(m.id).second) continue;

        // Capabilities live in this entry's object, before the next entry starts.
        std::string_view entry = data.substr(pos, 4000);
        if (auto next = entry.find(kMarker); next != std::string_view::npos) entry = entry.substr(0, next);
        m.native1m = entry.find("native_1m:!0") != std::string_view::npos;
        m.suffix1m = entry.find("supports_1m_suffix:!0") != std::string_view::npos;
        c.models.push_back(std::move(m));
    }

    std::string version;
    if (auto v = data.find("// Version: "); v != std::string_view::npos)
    {
        std::size_t p = v + 12, e = p;
        while (e < data.size() && e - p < 32 && data[e] != '\n' && data[e] != '\r') ++e;
        version.assign(data.substr(p, e - p));
    }
    ::munmap(map, static_cast<std::size_t>(st.st_size));

    Normalize(c);
    if (c.models.empty()) return BuiltinModelCatalog();
    c.source = version.empty() ? "установленный claude" : "Claude Code " + version;
    return c;
}
} // namespace config
