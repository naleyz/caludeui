#include "net/egress.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>

namespace net
{
namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kPassTtl = 30s; // how long a passing result is trusted

std::string Trim(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

bool ValidCountryCode(const std::string& c)
{
    return c.size() == 2 && std::all_of(c.begin(), c.end(), [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
}

std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// ── TCP probe of the proxy ──────────────────────────────────────────────
// curl reports a dead proxy and a dead destination alike; telling the user
// "the VPN client isn't listening" needs its own connect attempt.
bool ProxyReachable(const std::string& proxyUrl, const std::atomic<bool>& cancel, std::string* why)
{
    CURLU* u = curl_url();
    if (!u) return true; // can't tell — let the real request decide
    struct Free
    {
        CURLU* u;
        ~Free() { curl_url_cleanup(u); }
    } freeUrl{u};

    if (curl_url_set(u, CURLUPART_URL, proxyUrl.c_str(), CURLU_GUESS_SCHEME | CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        return true;
    char* host = nullptr;
    char* port = nullptr;
    if (curl_url_get(u, CURLUPART_HOST, &host, 0) != CURLUE_OK || !host) return true;
    std::string hostStr = host;
    curl_free(host);
    if (curl_url_get(u, CURLUPART_PORT, &port, 0) != CURLUE_OK || !port) port = nullptr;
    std::string portStr = port ? port : "1080"; // libcurl's default when a proxy URL has no port
    if (port) curl_free(port);
    if (hostStr.size() > 2 && hostStr.front() == '[' && hostStr.back() == ']') hostStr = hostStr.substr(1, hostStr.size() - 2);

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (int rc = ::getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &res); rc != 0 || !res)
    {
        if (why) *why = "адрес прокси не найден: " + std::string(::gai_strerror(rc));
        return false;
    }
    struct FreeAi
    {
        addrinfo* r;
        ~FreeAi() { ::freeaddrinfo(r); }
    } freeAi{res};

    std::string lastError = "нет соединения";
    for (addrinfo* ai = res; ai; ai = ai->ai_next)
    {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS)
        {
            // Poll in slices so a cancel (app quit) is honoured within 100 ms.
            pollfd pfd{fd, POLLOUT, 0};
            int    ready = 0;
            for (int waited = 0; waited < 2000 && !cancel.load() && ready == 0; waited += 100) ready = ::poll(&pfd, 1, 100);
            if (ready > 0)
            {
                int err = 0;
                socklen_t len = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = err == 0 ? 0 : -1;
                errno = err;
            }
            else rc = -1, errno = ETIMEDOUT;
        }
        ::close(fd);
        if (rc == 0) return true;
        lastError = std::strerror(errno);
    }
    if (why) *why = "не отвечает (" + lastError + ")";
    return false;
}

// ── HTTP ────────────────────────────────────────────────────────────────
size_t WriteBody(char* data, size_t size, size_t nmemb, void* user)
{
    auto* body = static_cast<std::string*>(user);
    const size_t n = size * nmemb;
    if (body->size() + n > 64 * 1024) return 0; // a geo answer is a few hundred bytes
    body->append(data, n);
    return n;
}

int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::atomic<bool>*>(user)->load() ? 1 : 0;
}

void EnsureCurlInit()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::optional<std::string> HttpGet(const std::string& url, const Route& route, const std::atomic<bool>& cancel,
                                   std::string* error)
{
    EnsureCurlInit();

    CURL* h = curl_easy_init();
    if (!h)
    {
        *error = "curl_easy_init";
        return std::nullopt;
    }
    struct Free
    {
        CURL* h;
        ~Free() { curl_easy_cleanup(h); }
    } freeHandle{h};

    std::string body;
    char        errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    // Claude Code's route, exactly. An empty string switches off curl's own
    // environment-proxy lookup, so a direct/tunnel route can't pick one up,
    // and a proxy that's down fails instead of falling back to a direct
    // connection (which would report — and expose — the real address).
    curl_easy_setopt(h, CURLOPT_PROXY, route.kind == RouteKind::Proxy ? route.proxyUrl.c_str() : "");
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 6000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_USERAGENT, "ClaudeUI");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &cancel);

    const CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK)
    {
        *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return std::nullopt;
    }
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200)
    {
        *error = "HTTP " + std::to_string(status);
        return std::nullopt;
    }
    return body;
}

struct Provider
{
    const char* source;
    const char* url;
    std::optional<GeoAnswer> (*parse)(std::string_view);
};

// Order matters. api.anthropic.com sits behind Cloudflare, whose /cdn-cgi/trace
// says which country *that host* sees the connection from — the very thing
// Anthropic's own region check acts on. A VPN client that routes per
// destination can give other hosts a different exit, so third-party services
// only stand in when that one can't be reached.
constexpr Provider kProviders[] = {
    {"api.anthropic.com", "https://api.anthropic.com/cdn-cgi/trace", ParseCloudflareTrace},
    {"cloudflare.com", "https://www.cloudflare.com/cdn-cgi/trace", ParseCloudflareTrace},
    {"ipwho.is", "https://ipwho.is/", ParseIpwhoIs},
};

std::string CountryName(const std::string& code)
{
    return code == "RU" ? "Russian Federation" : "";
}
} // namespace

// ── parsing ─────────────────────────────────────────────────────────────
std::optional<GeoAnswer> ParseCloudflareTrace(std::string_view body)
{
    GeoAnswer a;
    std::istringstream in{std::string(body)};
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = Trim(line.substr(eq + 1));
        if (key == "ip") a.ip = value;
        else if (key == "loc") a.countryCode = Upper(value);
    }
    if (!ValidCountryCode(a.countryCode)) return std::nullopt; // "XX" passes: it just isn't RU
    a.country = CountryName(a.countryCode);
    return a;
}

std::optional<GeoAnswer> ParseIpwhoIs(std::string_view body)
{
    const auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.value("success", false)) return std::nullopt;
    GeoAnswer a;
    a.ip          = j.value("ip", "");
    a.countryCode = Upper(j.value("country_code", ""));
    a.country     = j.value("country", "");
    if (!ValidCountryCode(a.countryCode)) return std::nullopt;
    return a;
}

bool IsRussia(const GeoAnswer& a)
{
    return a.countryCode == "RU";
}

std::string EgressCheck::Summary() const
{
    switch (outcome)
    {
    case Outcome::NoVpn:     return "VPN не обнаружен";
    case Outcome::Ok:
    case Outcome::Russia:
    {
        std::string s = countryCode;
        if (!country.empty()) s += " (" + country + ")";
        if (!ip.empty()) s += " · " + ip;
        return s + " · " + source;
    }
    case Outcome::ProxyDown: return route.Describe() + ": " + detail;
    case Outcome::Unknown:   return "страну выхода определить не удалось: " + detail;
    }
    return {};
}

// ── lookup ──────────────────────────────────────────────────────────────
EgressCheck CheckEgress(const Route& route, const std::atomic<bool>& cancel)
{
    EgressCheck r;
    r.route = route;
    if (!route.ViaVpn()) return r; // NoVpn

    if (route.kind == RouteKind::Proxy)
    {
        std::string why;
        if (!ProxyReachable(route.proxyUrl, cancel, &why))
        {
            r.outcome = EgressCheck::Outcome::ProxyDown;
            r.detail  = why;
            return r;
        }
    }

    std::string failures;
    for (const auto& p : kProviders)
    {
        if (cancel.load()) break;
        std::string error;
        auto body = HttpGet(p.url, route, cancel, &error);
        std::optional<GeoAnswer> answer;
        if (body) answer = p.parse(*body);
        if (!answer)
        {
            failures += std::string(failures.empty() ? "" : "; ") + p.source + ": " + (body ? "непонятный ответ" : error);
            continue;
        }
        r.ip          = answer->ip;
        r.countryCode = answer->countryCode;
        r.country     = answer->country;
        r.source      = p.source;
        r.outcome     = IsRussia(*answer) ? EgressCheck::Outcome::Russia : EgressCheck::Outcome::Ok;
        return r;
    }
    r.outcome = EgressCheck::Outcome::Unknown;
    r.detail  = failures.empty() ? "отменено" : failures;
    return r;
}

// ── guard ───────────────────────────────────────────────────────────────
EgressGuard::~EgressGuard()
{
    cancel_.store(true);
    for (auto& w : workers_)
        if (w.thread.joinable()) w.thread.join();
}

void EgressGuard::Reap()
{
    for (auto it = workers_.begin(); it != workers_.end();)
    {
        if (it->done->load())
        {
            it->thread.join();
            it = workers_.erase(it);
        }
        else ++it;
    }
}

EgressGuard::Future EgressGuard::Check(bool forceFresh)
{
    Route route = DetectRoute();

    if (!route.ViaVpn())
    {
        std::promise<EgressCheck> p;
        EgressCheck c;
        c.route = std::move(route);
        p.set_value(std::move(c));
        return p.get_future().share();
    }

    std::lock_guard lock(mu_);
    Reap();
    const std::string signature = route.Signature();
    if (!forceFresh && current_.valid() && signature == currentSignature_)
    {
        if (current_.wait_for(0s) != std::future_status::ready) return current_; // join the lookup in flight
        if (current_.get().outcome == EgressCheck::Outcome::Ok && Clock::now() - startedAt_ < kPassTtl) return current_;
    }

    auto promise = std::make_shared<std::promise<EgressCheck>>();
    auto done    = std::make_shared<std::atomic<bool>>(false);
    current_          = promise->get_future().share();
    currentSignature_ = signature;
    startedAt_        = Clock::now();
    workers_.push_back({std::thread([this, route = std::move(route), promise, done] {
                            EgressCheck c;
                            try
                            {
                                c = CheckEgress(route, cancel_);
                            }
                            catch (const std::exception& e)
                            {
                                c.route   = route;
                                c.outcome = EgressCheck::Outcome::Unknown;
                                c.detail  = e.what();
                            }
                            promise->set_value(std::move(c));
                            done->store(true);
                        }),
                        done});
    return current_;
}

EgressGuard& Guard()
{
    // OpenSSL registers its atexit cleanup when curl first initialises. Doing that
    // before the guard exists makes the guard (and its worker) go first at exit,
    // rather than OpenSSL tearing down under a request still in flight.
    EnsureCurlInit();
    static EgressGuard guard;
    return guard;
}
} // namespace net
