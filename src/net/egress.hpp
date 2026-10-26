#pragma once

#include "net/route.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace net
{
// Where Claude Code's traffic comes out, as seen from the API side.
struct EgressCheck
{
    enum class Outcome
    {
        NoVpn,     // direct connection: nothing to check, nothing to block
        Ok,        // through a VPN, exit country known and allowed
        Russia,    // through a VPN, exit country is RU — launching is refused
        ProxyDown, // the proxy from the environment doesn't accept connections (VPN client off?)
        Unknown,   // route is up but no service could tell the country
    };

    Route       route;
    Outcome     outcome = Outcome::NoVpn;
    std::string ip, countryCode, country;
    std::string source; // endpoint that answered: api.anthropic.com, ipwho.is, …
    std::string detail; // ProxyDown / Unknown: what went wrong

    bool Blocks() const { return outcome == Outcome::Russia; }
    // "FR · 194.24.161.128 · api.anthropic.com", or the failure for ProxyDown/Unknown.
    std::string Summary() const;
};

struct GeoAnswer
{
    std::string ip, countryCode, country;
};
// Parsers for the two response shapes we use; nullopt if the body isn't one.
std::optional<GeoAnswer> ParseCloudflareTrace(std::string_view body); // "ip=…\nloc=RU\n…"
std::optional<GeoAnswer> ParseIpwhoIs(std::string_view body);         // {"success":true,"country_code":"RU",…}
bool IsRussia(const GeoAnswer& a);

// Blocking: probes the proxy, then asks endpoints for the exit country — over
// the same route Claude Code takes, never around it. `cancel` aborts in-flight
// requests. Does nothing (NoVpn) for a direct route.
EgressCheck CheckEgress(const Route& route, const std::atomic<bool>& cancel);

// Runs CheckEgress off the UI thread and shares one lookup between tabs.
class EgressGuard
{
public:
    using Future = std::shared_future<EgressCheck>;

    EgressGuard() = default;
    ~EgressGuard();
    EgressGuard(const EgressGuard&)            = delete;
    EgressGuard& operator=(const EgressGuard&) = delete;

    // Never blocks. A direct route is answered at once; over a VPN a passing
    // result is reused for a short while, anything else is looked up afresh.
    // `forceFresh` skips the reuse (the "check again" button).
    Future Check(bool forceFresh = false);

private:
    struct Worker
    {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    void Reap();

    std::mutex        mu_;
    Future            current_;
    std::string       currentSignature_;
    std::chrono::steady_clock::time_point startedAt_{};
    std::vector<Worker> workers_;
    std::atomic<bool> cancel_{false};
};

EgressGuard& Guard();
} // namespace net
