#pragma once

#include <string>
#include <string_view>

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

// Looks at the environment this process — and so the `claude` it spawns —
// runs in. Cheap (a few file reads and one `ip route get`), safe on the UI thread.
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
