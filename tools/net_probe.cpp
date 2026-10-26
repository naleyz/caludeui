// Headless check of the VPN gate: what route Claude Code would take and which
// country the API side sees it from. Sends the same few requests a launch does.
//   net_probe            — the real environment
//   net_probe --parse    — offline self-test of the route/response parsers
#include "net/egress.hpp"
#include "net/route.hpp"

#include <cstdio>
#include <cstring>

namespace
{
int failures = 0;
void Expect(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int SelfTest()
{
    using namespace net;
    std::puts("route detection");
    {
        RouteInputs in;
        Expect(DetectRoute(in, "api.anthropic.com").kind == RouteKind::Direct, "nothing set → direct");
        in.httpProxy = "http://127.0.0.1:10809";
        auto r = DetectRoute(in, "api.anthropic.com");
        Expect(r.kind == RouteKind::Proxy && r.proxyVar == "http_proxy", "http_proxy → proxy");
        in.httpsProxyUpper = "http://upper:1";
        Expect(DetectRoute(in, "api.anthropic.com").proxyUrl == "http://upper:1", "HTTPS_PROXY outranks http_proxy");
        in.httpsProxy = "http://lower:2";
        Expect(DetectRoute(in, "api.anthropic.com").proxyUrl == "http://lower:2", "https_proxy outranks HTTPS_PROXY");
        in.noProxy = "localhost,.anthropic.com";
        Expect(DetectRoute(in, "api.anthropic.com").kind == RouteKind::Direct, "NO_PROXY suffix bypasses the proxy");
        in.noProxy = "example.com";
        Expect(DetectRoute(in, "api.anthropic.com").kind == RouteKind::Proxy, "unrelated NO_PROXY keeps the proxy");
        in = {};
        in.egressIface = "lo";
        Expect(DetectRoute(in, "api.anthropic.com").kind == RouteKind::Direct, "loopback is not a tunnel");
        in.egressIface = "tun0";
        auto t = DetectRoute(in, "api.anthropic.com");
        Expect(t.kind == RouteKind::Tunnel && t.iface == "tun0", "tun0 → tunnel");
    }
    std::puts("no_proxy matching");
    Expect(NoProxyMatches("*", "api.anthropic.com"), "*");
    Expect(NoProxyMatches("a.com, anthropic.com", "api.anthropic.com"), "bare domain covers subdomains");
    Expect(NoProxyMatches("*.anthropic.com", "api.anthropic.com"), "*. prefix");
    Expect(NoProxyMatches("api.anthropic.com:443", "api.anthropic.com"), "host:port");
    Expect(!NoProxyMatches("notanthropic.com", "api.anthropic.com"), "no partial-label match");
    std::puts("redaction");
    Expect(RedactProxy("http://user:secret@host:8080") == "http://host:8080", "credentials removed");
    Expect(RedactProxy("http://127.0.0.1:10809") == "http://127.0.0.1:10809", "plain URL untouched");
    std::puts("geo parsing");
    auto cf = ParseCloudflareTrace("fl=1\nip=185.3.33.87\nvisit_scheme=https\nloc=ru\ntls=TLSv1.3\n");
    Expect(cf && cf->countryCode == "RU" && cf->ip == "185.3.33.87" && IsRussia(*cf), "trace RU");
    auto fr = ParseCloudflareTrace("ip=194.24.161.128\nloc=FR\n");
    Expect(fr && fr->countryCode == "FR" && !IsRussia(*fr), "trace FR");
    Expect(!ParseCloudflareTrace("<html>blocked</html>"), "HTML rejected");
    Expect(!ParseCloudflareTrace("loc=RUS\n"), "bad code rejected");
    auto iw = ParseIpwhoIs(R"({"ip":"1.2.3.4","success":true,"country":"Russia","country_code":"RU"})");
    Expect(iw && IsRussia(*iw) && iw->country == "Russia", "ipwho.is RU");
    Expect(!ParseIpwhoIs(R"({"success":false,"message":"rate limit"})"), "ipwho.is failure rejected");
    Expect(!ParseIpwhoIs("not json"), "garbage rejected");
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--parse")) return SelfTest();

    const net::Route route = net::DetectRoute();
    std::printf("route:   %s\n", route.Describe().c_str());
    std::atomic<bool> cancel{false};
    const net::EgressCheck c = net::CheckEgress(route, cancel);
    const char* outcome[] = {"NoVpn", "Ok", "Russia (launch refused)", "ProxyDown", "Unknown"};
    std::printf("outcome: %s\n", outcome[static_cast<int>(c.outcome)]);
    std::printf("detail:  %s\n", c.Summary().c_str());
    return 0;
}
