#include "sessions/session_scanner.hpp"
#include "config/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>

namespace sessions
{
namespace
{
constexpr std::uintmax_t kHeadBytes = 64 * 1024;
constexpr std::uintmax_t kTailBytes = 96 * 1024;
constexpr std::size_t    kPreviewMaxLen = 160;

std::filesystem::path ProjectsRoot()
{
    return config::ClaudeDir() / "projects";
}

// Reads at most maxBytes starting at offset.
std::string ReadChunk(const std::filesystem::path& p, std::uintmax_t offset, std::uintmax_t maxBytes)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string buf(static_cast<std::size_t>(maxBytes), '\0');
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return buf;
}

// Calls fn for every complete-looking line; partial lines at the chunk edges
// simply fail to parse and are skipped.
template <typename Fn>
void ForEachJsonLine(std::string_view chunk, Fn&& fn)
{
    while (!chunk.empty())
    {
        auto nl = chunk.find('\n');
        std::string_view line = chunk.substr(0, nl);
        if (!line.empty() && line.front() == '{')
        {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_discarded() && j.is_object()) fn(j);
        }
        if (nl == std::string_view::npos) break;
        chunk.remove_prefix(nl + 1);
    }
}

std::string Flatten(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (char c : s)
    {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (prevSpace || out.empty())) continue;
        prevSpace = c == ' ';
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();

    if (out.size() > kPreviewMaxLen)
    {
        std::size_t cut = kPreviewMaxLen;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut; // keep UTF-8 intact
        out.resize(cut);
        out += "\xe2\x80\xa6";
    }
    return out;
}

std::string StringField(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// message.content is either a plain string, or an array of content blocks
// (tool_result/image/text/...); pull the first text piece out of it.
std::string ExtractPreviewText(const nlohmann::json& content)
{
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array())
        for (const auto& block : content)
            if (block.is_object() && block.value("type", "") == "text")
                if (auto t = block.find("text"); t != block.end() && t->is_string()) return t->get<std::string>();
    return {};
}

// Slash-command plumbing and hook output aren't useful as a preview.
bool IsNoisePrompt(std::string_view text)
{
    return text.starts_with("<command-") || text.starts_with("<local-command") ||
           text.starts_with("<system-reminder>") || text.starts_with("Caveat:");
}

struct TitleState
{
    std::string custom, ai;
};

void ScanLine(const nlohmann::json& j, SessionInfo& info, TitleState& titles, bool head)
{
    if (auto ts = StringField(j, "timestamp"); !ts.empty())
    {
        if (head && info.firstTimestamp.empty()) info.firstTimestamp = ts;
        if (!head) info.lastTimestamp = ts;
    }
    if (info.cwd.empty()) info.cwd = StringField(j, "cwd");

    const std::string type = StringField(j, "type");
    if (type == "custom-title")
    {
        if (auto t = StringField(j, "customTitle"); !t.empty()) titles.custom = t;
    }
    else if (type == "ai-title")
    {
        if (auto t = StringField(j, "aiTitle"); !t.empty()) titles.ai = t;
    }
    else if (type == "last-prompt")
    {
        if (auto t = StringField(j, "lastPrompt"); !t.empty()) info.lastPrompt = Flatten(std::move(t));
    }
    else if (type == "user" && head && info.firstUserPreview.empty() && !j.value("isMeta", false))
    {
        if (auto msg = j.find("message"); msg != j.end() && msg->is_object())
            if (auto content = msg->find("content"); content != msg->end())
            {
                std::string text = ExtractPreviewText(*content);
                if (!text.empty() && !IsNoisePrompt(text)) info.firstUserPreview = Flatten(std::move(text));
            }
    }
}

SessionInfo ParseSessionFile(const std::filesystem::path& jsonlPath, const std::filesystem::path& projectDir,
                             std::uintmax_t size, std::filesystem::file_time_type mtime)
{
    SessionInfo info;
    info.id            = jsonlPath.stem().string();
    info.projectDir    = projectDir;
    info.filePath      = jsonlPath;
    info.fileSizeBytes = size;
    info.mtime         = mtime;

    TitleState titles;
    const std::uintmax_t headLen = std::min(size, kHeadBytes);
    ForEachJsonLine(ReadChunk(jsonlPath, 0, headLen), [&](const nlohmann::json& j) { ScanLine(j, info, titles, true); });
    if (size > headLen)
    {
        const std::uintmax_t tailLen = std::min(size - headLen, kTailBytes);
        ForEachJsonLine(ReadChunk(jsonlPath, size - tailLen, tailLen),
                        [&](const nlohmann::json& j) { ScanLine(j, info, titles, false); });
    }
    else
    {
        // Small file: the head pass saw everything, take the last timestamp from it too.
        ForEachJsonLine(ReadChunk(jsonlPath, 0, headLen), [&](const nlohmann::json& j) {
            if (auto ts = StringField(j, "timestamp"); !ts.empty()) info.lastTimestamp = ts;
        });
    }
    if (info.lastTimestamp.empty()) info.lastTimestamp = info.firstTimestamp;
    info.title = !titles.custom.empty() ? titles.custom : titles.ai;
    if (!info.title.empty()) info.title = Flatten(std::move(info.title));

    return info;
}
} // namespace

bool SessionScanner::TranscriptExists(const std::string& sessionId)
{
    if (sessionId.empty() || sessionId.find('/') != std::string::npos) return false;
    std::error_code ec;
    for (const auto& project : std::filesystem::directory_iterator(ProjectsRoot(), ec))
    {
        std::error_code fec;
        if (project.is_directory(fec) && std::filesystem::is_regular_file(project.path() / (sessionId + ".jsonl"), fec))
            return true;
    }
    return false;
}

std::vector<SessionInfo> SessionScanner::ScanAll()
{
    SessionScanner s;
    s.Rescan();
    return s.sessions_;
}

bool SessionScanner::Rescan()
{
    std::error_code ec;
    auto root = ProjectsRoot();
    bool changed = false;
    std::unordered_set<std::string> seen;

    if (std::filesystem::is_directory(root, ec))
    {
        for (const auto& projectEntry : std::filesystem::directory_iterator(root, ec))
        {
            std::error_code dec;
            if (!projectEntry.is_directory(dec)) continue;

            for (const auto& fileEntry : std::filesystem::directory_iterator(projectEntry.path(), dec))
            {
                std::error_code fec;
                if (!fileEntry.is_regular_file(fec) || fileEntry.path().extension() != ".jsonl") continue;

                auto size  = fileEntry.file_size(fec);
                if (fec) continue;
                auto mtime = fileEntry.last_write_time(fec);
                if (fec) continue;

                std::string key = fileEntry.path().string();
                seen.insert(key);

                auto it = cache_.find(key);
                if (it != cache_.end() && it->second.fileSizeBytes == size && it->second.mtime == mtime) continue;

                // Unparseable/empty files are cached too, so they aren't re-read every pass.
                cache_[key] = ParseSessionFile(fileEntry.path(), projectEntry.path(), size, mtime);
                changed = true;
            }
        }
    }

    for (auto it = cache_.begin(); it != cache_.end();)
    {
        if (!seen.contains(it->first))
        {
            it = cache_.erase(it);
            changed = true;
        }
        else ++it;
    }

    if (changed)
    {
        // Sessions with no timestamped entries at all are empty shells
        // (started and closed without a prompt) — nothing to resume.
        sessions_.clear();
        sessions_.reserve(cache_.size());
        for (const auto& [_, info] : cache_)
            if (!info.lastTimestamp.empty()) sessions_.push_back(info);
        std::sort(sessions_.begin(), sessions_.end(), [](const SessionInfo& a, const SessionInfo& b) {
            return a.lastTimestamp != b.lastTimestamp ? a.lastTimestamp > b.lastTimestamp : a.id < b.id;
        });
    }
    return changed;
}

bool RenameSessionFile(const std::filesystem::path& jsonlPath, const std::string& sessionId,
                       const std::string& newTitle, std::string* error)
{
    nlohmann::json j;
    j["type"]        = "custom-title";
    j["customTitle"] = newTitle;
    j["sessionId"]   = sessionId;
    const std::string line = j.dump() + "\n";

    // O_APPEND makes this single write() atomic relative to a `claude`
    // process that may be concurrently appending to the same transcript —
    // exactly what real /rename relies on.
    int fd = ::open(jsonlPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
    {
        if (error) *error = "не удалось открыть " + jsonlPath.string() + ": " + std::strerror(errno);
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < line.size())
    {
        ssize_t n = ::write(fd, line.data() + done, line.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
        {
            if (error) *error = "не удалось записать в " + jsonlPath.string() + ": " + std::strerror(errno);
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return ok;
}

bool DeleteSessionFile(const std::filesystem::path& jsonlPath, std::string* error)
{
    std::error_code ec;
    std::filesystem::remove(jsonlPath, ec);
    if (ec)
    {
        if (error) *error = jsonlPath.string() + ": " + ec.message();
        return false;
    }
    return true;
}
} // namespace sessions
