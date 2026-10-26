#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net
{
// How `claude`'s traffic to the API leaves this machine.
enum class RouteKind
{
    Direct, // plain connection: no VPN as far as we can tell
    Proxy,  // through an HTTP(S) proxy taken from the environment (a local VPN client, typically)
    Tunnel, // through a tun/wireguard/ppp interface that owns the route to the internet
};

struct Route
{
    RouteKind   kind = RouteKind::Direct;
    std::string proxyUrl; // Proxy: exactly what Claude Code will use (may hold credentials — never display it raw)
    std::string proxyVar; // Proxy: variable it came from (https_proxy, HTTPS_PROXY, …)
    std::string iface;    // Tunnel: interface the route to the internet goes through

    bool ViaVpn() const { return kind != RouteKind::Direct; }
    // "прокси 127.0.0.1:10809", "туннель wg0", "без VPN" — credentials stripped.
    std::string Describe() const;
    // Identity of the route, to tell whether a cached lookup still applies.
    std::string Signature() const;
};

// Which proxy the `claude` processes ClaudeUI starts should use (Settings →
// ClaudeUI). System: whatever ClaudeUI's own environment has (https_proxy,
// all_proxy, …). Custom: `url` for every proxy variable, `noProxy` as NO_PROXY,
// whatever the environment says. None: every proxy variable removed.
enum class ProxyMode
{
    System,
    Custom,
    None,
};
struct SessionProxy
{
    ProxyMode   mode = ProxyMode::System;
    std::string url;     // Custom: "http://127.0.0.1:10809"
    std::string noProxy; // Custom: NO_PROXY for the session ("" = none)
    bool operator==(const SessionProxy&) const = default;
};
// Thread-safe; set by the UI whenever the setting changes.
void SetSessionProxy(SessionProxy proxy);
SessionProxy GetSessionProxy();

// The environment changes that apply GetSessionProxy() to a child process:
// variables to drop, then variables to set (empty for System).
struct ProxyEnv
{
    std::vector<std::string>                         unset;
    std::vector<std::pair<std::string, std::string>> set;
};
ProxyEnv SessionProxyEnv();

// Every variable that steers a proxy for Claude Code or the tools it runs.
inline const std::vector<std::string> kProxyVars = {"https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY",
                                                    "all_proxy",   "ALL_PROXY",   "no_proxy",   "NO_PROXY"};

// "" when `url` is usable as an HTTP(S) proxy for Claude Code, else why not.
std::string CheckProxyUrl(std::string_view url);

// The route the `claude` ClaudeUI spawns will take: the session proxy setting
// over ClaudeUI's own environment. Cheap (a few file reads and one
// `ip route get`), safe on the UI thread.
Route DetectRoute();

// Same detection with the inputs passed in, for tests and probes.
struct RouteInputs
{
    std::string httpsProxy, httpsProxyUpper, httpProxy, httpProxyUpper; // https_proxy, HTTPS_PROXY, http_proxy, HTTP_PROXY
    std::string noProxy, noProxyUpper;                                  // no_proxy, NO_PROXY
    std::string egressIface;                                            // interface the default route goes through, "" if unknown
};
Route DetectRoute(const RouteInputs& in, std::string_view apiHost);

// `host:port` of a proxy URL, "user:pass@" removed; the input if it doesn't parse.
std::string RedactProxy(std::string_view url);

// NO_PROXY semantics as Claude Code applies them: "*", exact host, or a
// domain suffix with or without a leading dot / "*.".
bool NoProxyMatches(std::string_view noProxy, std::string_view host);

// True for tun/tap/wireguard/ppp-style interfaces (by sysfs link type, then by name).
bool IsTunnelInterface(const std::string& name);
} // namespace net
