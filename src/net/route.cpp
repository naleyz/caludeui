#include "net/route.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace net
{
namespace
{
// The host whose traffic matters: Claude Code's API endpoint.
constexpr std::string_view kApiHost = "api.anthropic.com";

std::string Env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool ValidIfaceName(const std::string& n)
{
    return !n.empty() && n.size() < 16 &&
           std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

// "ip -o route get 1.1.1.1" honours policy routing (wg-quick keeps its default
// route in a side table), which /proc/net/route does not show.
std::string InterfaceFromIpRoute()
{
    FILE* p = ::popen("ip -o route get 1.1.1.1 2>/dev/null", "r");
    if (!p) return {};
    std::array<char, 512> buf{};
    std::string line;
    if (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) line = buf.data();
    ::pclose(p);

    std::istringstream in(line);
    for (std::string tok, prev; in >> tok; prev = tok)
        if (prev == "dev") return ValidIfaceName(tok) ? tok : std::string{};
    return {};
}

// Fallback when `ip` is missing: the main table only. VPN clients that split
// the default route in two halves (0/1 + 128/1) are recognised by their halves.
std::string InterfaceFromProcRoute()
{
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line); // header
    std::string best;
    long bestMetric = -1;
    while (std::getline(in, line))
    {
        std::istringstream ls(line);
        std::string iface, dest, gw, flags, ref, use, metric, mask;
        if (!(ls >> iface >> dest >> gw >> flags >> ref >> use >> metric >> mask)) continue;
        const bool full  = dest == "00000000" && mask == "00000000";
        const bool half  = (dest == "00000000" || dest == "00000080") && mask == "00000080"; // 0.0.0.0/1, 128.0.0.0/1
        if (!full && !half) continue;
        if (half && IsTunnelInterface(iface)) return iface;
        if (!full) continue;
        const long m = std::strtol(metric.c_str(), nullptr, 10);
        if (bestMetric < 0 || m < bestMetric)
        {
            best = iface;
            bestMetric = m;
        }
    }
    return best;
}

std::string EgressInterface()
{
    if (auto i = InterfaceFromIpRoute(); !i.empty()) return i;
    return InterfaceFromProcRoute();
}
} // namespace

std::string RedactProxy(std::string_view url)
{
    const auto scheme = url.find("://");
    const std::size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const auto slash = url.find('/', start);
    const auto at = url.rfind('@', slash == std::string_view::npos ? url.size() : slash);
    if (at == std::string_view::npos || at < start) return std::string(url);
    return std::string(url.substr(0, start)) + std::string(url.substr(at + 1));
}

bool NoProxyMatches(std::string_view noProxy, std::string_view host)
{
    const std::string h = Lower(host);
    std::string token;
    auto matches = [&](std::string t) {
        t = Lower(t);
        if (t.empty()) return false;
        if (t == "*") return true;
        if (const auto colon = t.rfind(':'); colon != std::string::npos && t.find(':') == colon) t.erase(colon); // host:port
        if (t.starts_with("*.")) t.erase(0, 2);
        else if (t.starts_with('.')) t.erase(0, 1);
        return h == t || (h.size() > t.size() && h.ends_with(t) && h[h.size() - t.size() - 1] == '.');
    };
    for (char c : noProxy)
    {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c)))
        {
            if (matches(token)) return true;
            token.clear();
        }
        else token.push_back(c);
    }
    return matches(token);
}

bool IsTunnelInterface(const std::string& name)
{
    if (!ValidIfaceName(name) || name == "lo") return false;

    // ARPHRD_NONE (65534) is what tun and wireguard devices report; PPP is 512.
    // Ethernet-typed devices (bridges, veth, physical NICs) never match.
    std::ifstream type("/sys/class/net/" + name + "/type");
    int t = -1;
    if (type >> t && (t == 65534 || t == 512)) return true;

    const std::string n = Lower(name);
    for (const char* prefix : {"tun", "tap", "wg", "ppp", "vpn", "nordlynx", "proton", "mullvad", "warp", "tailscale"})
        if (n.starts_with(prefix)) return true;
    return false;
}

std::string Route::Describe() const
{
    switch (kind)
    {
    case RouteKind::Proxy:  return "прокси " + RedactProxy(proxyUrl);
    case RouteKind::Tunnel: return "туннель " + iface;
    default:                return "без VPN";
    }
}

std::string Route::Signature() const
{
    switch (kind)
    {
    case RouteKind::Proxy:  return "proxy|" + proxyUrl;
    case RouteKind::Tunnel: return "tunnel|" + iface;
    default:                return "direct";
    }
}

Route DetectRoute(const RouteInputs& in, std::string_view apiHost)
{
    Route r;

    // Claude Code's own precedence (https_proxy → HTTPS_PROXY → http_proxy → HTTP_PROXY).
    const std::array<std::pair<const char*, const std::string*>, 4> vars = {{
        {"https_proxy", &in.httpsProxy},
        {"HTTPS_PROXY", &in.httpsProxyUpper},
        {"http_proxy", &in.httpProxy},
        {"HTTP_PROXY", &in.httpProxyUpper},
    }};
    for (const auto& [name, value] : vars)
    {
        if (value->empty()) continue;
        // A NO_PROXY hit sends Claude Code straight out — the proxy is irrelevant then.
        if (NoProxyMatches(in.noProxy, apiHost) || NoProxyMatches(in.noProxyUpper, apiHost)) break;
        r.kind     = RouteKind::Proxy;
        r.proxyUrl = *value;
        r.proxyVar = name;
        return r;
    }

    if (IsTunnelInterface(in.egressIface))
    {
        r.kind  = RouteKind::Tunnel;
        r.iface = in.egressIface;
    }
    return r;
}

namespace
{
std::mutex   gProxyMu;
SessionProxy gProxy;
} // namespace

void SetSessionProxy(SessionProxy proxy)
{
    std::lock_guard lock(gProxyMu);
    gProxy = std::move(proxy);
}

SessionProxy GetSessionProxy()
{
    std::lock_guard lock(gProxyMu);
    return gProxy;
}

ProxyEnv SessionProxyEnv()
{
    const SessionProxy p = GetSessionProxy();
    ProxyEnv env;
    if (p.mode == ProxyMode::System) return env;
    env.unset = kProxyVars;
    if (p.mode == ProxyMode::Custom && !p.url.empty())
    {
        // Claude Code reads HTTPS_PROXY/HTTP_PROXY; git, curl and friends it
        // runs also look at ALL_PROXY — set them all so nothing leaks around it.
        for (const char* k : {"https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"})
            env.set.emplace_back(k, p.url);
        if (!p.noProxy.empty())
            for (const char* k : {"no_proxy", "NO_PROXY"}) env.set.emplace_back(k, p.noProxy);
    }
    return env;
}

std::string CheckProxyUrl(std::string_view url)
{
    if (url.empty()) return "укажите адрес прокси";
    const std::string lower = Lower(url);
    if (lower.starts_with("socks")) return "SOCKS Claude Code не поддерживает — нужен HTTP-прокси (http://хост:порт)";
    if (!lower.starts_with("http://") && !lower.starts_with("https://")) return "адрес должен начинаться с http:// или https://";
    const std::string_view rest = url.substr(url.find("://") + 3);
    if (rest.empty() || rest.front() == ':' || rest.front() == '/') return "не указан хост";
    return {};
}

Route DetectRoute()
{
    RouteInputs in;
    const SessionProxy p = GetSessionProxy();
    if (p.mode == ProxyMode::System)
    {
        in.httpsProxy      = Env("https_proxy");
        in.httpsProxyUpper = Env("HTTPS_PROXY");
        in.httpProxy       = Env("http_proxy");
        in.httpProxyUpper  = Env("HTTP_PROXY");
        in.noProxy         = Env("no_proxy");
        in.noProxyUpper    = Env("NO_PROXY");
    }
    else if (p.mode == ProxyMode::Custom)
    {
        in.httpsProxy = p.url;
        in.noProxy    = p.noProxy;
    }
    // A proxy that applies to the API host decides the route by itself; only
    // otherwise is the (subprocess-backed) interface lookup worth doing.
    Route r = DetectRoute(in, kApiHost);
    if (r.kind == RouteKind::Proxy && p.mode == ProxyMode::Custom) r.proxyVar = "настройки ClaudeUI";
    if (r.kind != RouteKind::Direct) return r;
    in.egressIface = EgressInterface();
    return DetectRoute(in, kApiHost);
}
} // namespace net
