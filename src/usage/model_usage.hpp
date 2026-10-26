#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// Token use per model, read from Claude Code's own transcripts
// (~/.claude/projects/**/*.jsonl, subagents included). Every assistant reply
// there carries its model, timestamp and usage counters. This covers this
// machine only, and it's raw token counts: the server weighs models
// differently when it computes the plan limits.
namespace usage
{
struct TokenCounts
{
    std::int64_t input      = 0;
    std::int64_t cacheWrite = 0;
    std::int64_t cacheRead  = 0;
    std::int64_t output     = 0;

    TokenCounts& operator+=(const TokenCounts& o)
    {
        input += o.input;
        cacheWrite += o.cacheWrite;
        cacheRead += o.cacheRead;
        output += o.output;
        return *this;
    }
};

struct ModelEvent
{
    std::int64_t atMs = 0;
    std::string  model; // "claude-opus-5-5"
    TokenCounts  tokens;
};

// One transcript line → an event, if it is an assistant reply with usage.
// Claude Code writes a reply once per content block with the same usage, so
// `key` ("<message id>|<request id>") is what to deduplicate on.
std::optional<ModelEvent> ParseAssistantLine(std::string_view line, std::string* key);

class TranscriptUsage
{
public:
    TranscriptUsage();
    ~TranscriptUsage();
    TranscriptUsage(const TranscriptUsage&)            = delete;
    TranscriptUsage& operator=(const TranscriptUsage&) = delete;

    // Rescans in the background, unless a scan finished less than 30 s ago.
    // Unchanged files are skipped and grown ones are read from where the last
    // scan stopped, so repeated calls are cheap.
    void Refresh(bool force = false);

    // Events at or after `sinceMs` (at most the last 8 days are kept), oldest first.
    std::vector<ModelEvent> Since(std::int64_t sinceMs) const;
    bool         Scanning() const;
    std::int64_t ScannedAtMs() const; // 0 until the first scan finishes

    static constexpr std::int64_t kKeepMs = 8LL * 24 * 3600 * 1000;

private:
    struct FileState
    {
        std::uintmax_t                              offset = 0; // bytes consumed (up to the last complete line)
        std::unordered_map<std::string, ModelEvent> events;     // by dedup key
    };

    void Worker();
    void Scan();

    std::unordered_map<std::string, FileState> files_; // worker thread only

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    bool                    requested_ = false;
    bool                    stop_      = false;
    bool                    scanning_  = false;
    std::int64_t            scannedAtMs_ = 0;
    std::vector<ModelEvent> events_; // merged, sorted by time
    std::thread             worker_;
};
} // namespace usage
