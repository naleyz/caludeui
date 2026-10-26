#include "usage/model_usage.hpp"
#include "config/paths.hpp"
#include "usage/usage.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <unordered_set>

namespace usage
{
namespace
{
std::int64_t Int(const nlohmann::json& o, const char* key)
{
    auto it = o.find(key);
    return it != o.end() && it->is_number_integer() ? it->get<std::int64_t>() : 0;
}

std::int64_t FileMtimeMs(const std::filesystem::directory_entry& e)
{
    std::error_code ec;
    const auto ft = e.last_write_time(ec);
    if (ec) return 0;
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}
} // namespace

std::optional<ModelEvent> ParseAssistantLine(std::string_view line, std::string* key)
{
    // Cheap reject first: most lines are user turns, tool results, metadata.
    if (line.find("\"type\":\"assistant\"") == std::string_view::npos) return std::nullopt;
    const auto o = nlohmann::json::parse(line, nullptr, false);
    if (o.is_discarded() || !o.is_object() || o.value("type", "") != "assistant") return std::nullopt;
    const auto msg = o.find("message");
    if (msg == o.end() || !msg->is_object()) return std::nullopt;
    const auto u = msg->find("usage");
    if (u == msg->end() || !u->is_object()) return std::nullopt;

    ModelEvent e;
    e.model = msg->value("model", "");
    if (e.model.empty() || e.model.front() == '<') return std::nullopt; // "<synthetic>": no model call behind it
    e.atMs = ParseIso8601Ms(o.value("timestamp", ""));
    if (e.atMs == 0) return std::nullopt;
    e.tokens.input      = Int(*u, "input_tokens");
    e.tokens.cacheWrite = Int(*u, "cache_creation_input_tokens");
    e.tokens.cacheRead  = Int(*u, "cache_read_input_tokens");
    e.tokens.output     = Int(*u, "output_tokens");
    if (key) *key = msg->value("id", "") + "|" + o.value("requestId", "");
    return e;
}

TranscriptUsage::TranscriptUsage() : worker_([this] { Worker(); }) {}

TranscriptUsage::~TranscriptUsage()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void TranscriptUsage::Refresh(bool force)
{
    {
        std::lock_guard lock(mutex_);
        if (scanning_ || requested_) return;
        if (!force && scannedAtMs_ > 0 && NowMs() - scannedAtMs_ < 30'000) return;
        requested_ = true;
    }
    wake_.notify_all();
}

std::vector<ModelEvent> TranscriptUsage::Since(std::int64_t sinceMs) const
{
    std::lock_guard lock(mutex_);
    auto first = std::lower_bound(events_.begin(), events_.end(), sinceMs,
                                  [](const ModelEvent& e, std::int64_t t) { return e.atMs < t; });
    return {first, events_.end()};
}

bool TranscriptUsage::Scanning() const
{
    std::lock_guard lock(mutex_);
    return scanning_ || requested_;
}

std::int64_t TranscriptUsage::ScannedAtMs() const
{
    std::lock_guard lock(mutex_);
    return scannedAtMs_;
}

void TranscriptUsage::Worker()
{
    std::unique_lock lock(mutex_);
    while (true)
    {
        wake_.wait(lock, [&] { return stop_ || requested_; });
        if (stop_) return;
        requested_ = false;
        scanning_  = true;
        lock.unlock();
        Scan();
        lock.lock();
        scanning_    = false;
        scannedAtMs_ = NowMs();
    }
}

void TranscriptUsage::Scan()
{
    namespace fs = std::filesystem;
    const std::int64_t cutoff = NowMs() - kKeepMs;
    const fs::path root = config::ClaudeDir() / "projects";

    std::unordered_set<std::string> seen;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() != ".jsonl") continue;
        if (FileMtimeMs(*it) < cutoff) continue; // nothing in it can be recent
        const std::string path = it->path().string();
        seen.insert(path);

        const std::uintmax_t size = it->file_size(ec);
        if (ec) continue;
        FileState& st = files_[path];
        if (size < st.offset) st = {}; // rewritten or truncated: start over
        if (size == st.offset) continue;

        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        in.seekg(static_cast<std::streamoff>(st.offset));
        std::string buf(size - st.offset, '\0');
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        buf.resize(static_cast<std::size_t>(in.gcount()));

        // Only complete lines: a reply being written right now is picked up next time.
        const std::size_t lastNl = buf.rfind('\n');
        if (lastNl == std::string::npos) continue;
        std::string_view view(buf.data(), lastNl + 1);
        std::string key;
        for (std::size_t pos = 0; pos < view.size();)
        {
            const std::size_t nl = view.find('\n', pos);
            const std::string_view line = view.substr(pos, nl - pos);
            pos = nl + 1;
            if (auto e = ParseAssistantLine(line, &key); e && e->atMs >= cutoff) st.events[key] = std::move(*e);
        }
        st.offset += lastNl + 1;
    }

    // Forget files that are gone or went quiet, and events that aged out.
    // A forked or continued session can carry the same replies in two
    // transcripts, so deduplicate across files too.
    std::unordered_map<std::string, const ModelEvent*> unique;
    for (auto it = files_.begin(); it != files_.end();)
    {
        if (!seen.contains(it->first))
        {
            it = files_.erase(it);
            continue;
        }
        std::erase_if(it->second.events, [&](const auto& kv) { return kv.second.atMs < cutoff; });
        for (const auto& [k, e] : it->second.events) unique.try_emplace(k, &e);
        ++it;
    }
    std::vector<ModelEvent> merged;
    merged.reserve(unique.size());
    for (const auto& [k, e] : unique) merged.push_back(*e);
    std::sort(merged.begin(), merged.end(), [](const ModelEvent& a, const ModelEvent& b) { return a.atMs < b.atMs; });

    std::lock_guard lock(mutex_);
    events_ = std::move(merged);
}
} // namespace usage
