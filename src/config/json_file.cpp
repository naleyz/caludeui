#include "config/json_file.hpp"
#include "config/paths.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace config
{
namespace
{
using namespace std::chrono_literals;

// proper-lockfile's default: a lock whose mtime is older than this is stale.
constexpr auto kStaleLock   = 10s;
constexpr auto kLockTimeout = 3s;

std::string Errno(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

std::optional<std::string> ReadFile(const std::filesystem::path& p, bool* missing)
{
    *missing = false;
    std::ifstream f(p, std::ios::binary);
    if (!f)
    {
        std::error_code ec;
        *missing = !std::filesystem::exists(p, ec);
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

class DirLock
{
public:
    explicit DirLock(std::filesystem::path path) : path_(std::move(path)) {}
    ~DirLock()
    {
        if (held_) ::rmdir(path_.c_str());
    }

    bool Acquire(std::string* error)
    {
        auto deadline = std::chrono::steady_clock::now() + kLockTimeout;
        while (true)
        {
            if (::mkdir(path_.c_str(), 0777) == 0)
            {
                held_ = true;
                return true;
            }
            if (errno != EEXIST)
            {
                *error = Errno(("mkdir " + path_.string()).c_str());
                return false;
            }
            struct stat st{};
            if (::stat(path_.c_str(), &st) == 0)
            {
                auto age = std::chrono::seconds(std::time(nullptr) - st.st_mtime);
                if (age > kStaleLock)
                {
                    ::rmdir(path_.c_str()); // abandoned by a crashed process
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                *error = "файл занят другим процессом Claude Code (" + path_.string() + "), попробуйте ещё раз";
                return false;
            }
            std::this_thread::sleep_for(25ms);
        }
    }

private:
    std::filesystem::path path_;
    bool held_ = false;
};

std::filesystem::path BackupDir()
{
    return ClaudeDir() / "claudeui-backups";
}

// One backup per file per program run; keeps the newest 10 per file name.
void BackupOnce(const std::filesystem::path& file, const std::string& contents)
{
    static std::mutex mu;
    static std::set<std::string> done;
    std::lock_guard lock(mu);
    if (!done.insert(file.string()).second) return;

    std::error_code ec;
    auto dir = BackupDir();
    std::filesystem::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);

    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string base = file.filename().string();
    auto target = dir / (base + "." + stamp + ".bak");

    int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    (void)!::write(fd, contents.data(), contents.size());
    ::close(fd);

    std::vector<std::filesystem::path> mine;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        auto name = e.path().filename().string();
        if (name.starts_with(base + ".") && name.ends_with(".bak")) mine.push_back(e.path());
    }
    std::sort(mine.begin(), mine.end()); // timestamps sort lexicographically
    while (mine.size() > 10)
    {
        std::filesystem::remove(mine.front(), ec);
        mine.erase(mine.begin());
    }
}
} // namespace

namespace
{
// Number formatting as JS's Number.prototype.toString: shortest round-trip
// digits, exponent form only outside [1e-6, 1e21). nlohmann's dump prints 17
// significant digits instead, which would rewrite e.g. 2.582067000000001 as
// 2.5820670000000008 in a file we only meant to change one key in.
void AppendJsNumber(std::string& out, double v)
{
    if (v == 0.0)
    {
        out += '0';
        return;
    }
    char buf[64];
    const double mag = v < 0 ? -v : v;
    const auto fmt   = (mag >= 1e-6 && mag < 1e21) ? std::chars_format::fixed : std::chars_format::scientific;
    auto [end, ec]   = std::to_chars(buf, buf + sizeof(buf), v, fmt);
    std::string s(buf, ec == std::errc() ? end : buf);
    if (fmt == std::chars_format::scientific)
    {
        auto e = s.find('e');
        if (e != std::string::npos)
        {
            std::string mant = s.substr(0, e);
            char sign = s[e + 1] == '-' ? '-' : '+';
            std::size_t digits = s.find_first_not_of("+-0", e + 1);
            s = mant + "e" + sign + (digits == std::string::npos ? "0" : s.substr(digits));
        }
    }
    out += s;
}

void AppendJsString(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void AppendJs(std::string& out, const Json& j, int depth)
{
    auto newline = [&](int d) {
        out += '\n';
        out.append(static_cast<std::size_t>(d) * 2, ' ');
    };
    switch (j.type())
    {
    case Json::value_t::object:
    {
        if (j.empty())
        {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (!first) out += ',';
            first = false;
            newline(depth + 1);
            AppendJsString(out, it.key());
            out += ": ";
            AppendJs(out, it.value(), depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    case Json::value_t::array:
    {
        if (j.empty())
        {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < j.size(); ++i)
        {
            if (i) out += ',';
            newline(depth + 1);
            AppendJs(out, j[i], depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    }
    case Json::value_t::string:          AppendJsString(out, j.get_ref<const std::string&>()); break;
    case Json::value_t::boolean:         out += j.get<bool>() ? "true" : "false"; break;
    case Json::value_t::number_integer:  out += std::to_string(j.get<std::int64_t>()); break;
    case Json::value_t::number_unsigned: out += std::to_string(j.get<std::uint64_t>()); break;
    case Json::value_t::number_float:    AppendJsNumber(out, j.get<double>()); break;
    default:                             out += "null"; break;
    }
}
} // namespace

std::string Dump(const Json& j)
{
    std::string out;
    AppendJs(out, j, 0);
    return out;
}

std::optional<Json> ReadJsonObject(const std::filesystem::path& path, std::string* error)
{
    bool missing = false;
    auto text = ReadFile(path, &missing);
    if (!text)
    {
        if (missing) return Json::object();
        if (error) *error = Errno(("не удалось прочитать " + path.string()).c_str());
        return std::nullopt;
    }
    if (text->find_first_not_of(" \t\r\n") == std::string::npos) return Json::object();

    auto j = Json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        if (error) *error = path.string() + ": некорректный JSON";
        return std::nullopt;
    }
    return j;
}

bool UpdateJsonFile(const std::filesystem::path& path, const std::function<bool(Json&)>& mutate,
                    std::string* error, bool backup)
{
    std::string localErr;
    std::string& err = error ? *error : localErr;

    std::error_code ec;
    // Write through symlinks (dotfile managers) instead of replacing the link.
    std::filesystem::path target = path;
    if (std::filesystem::is_symlink(path, ec))
    {
        target = std::filesystem::canonical(path, ec);
        if (ec)
        {
            err = "не удалось разрешить ссылку " + path.string() + ": " + ec.message();
            return false;
        }
    }
    ec.clear(); // is_symlink reports ENOENT for a file that doesn't exist yet — that's fine, we create it
    std::filesystem::create_directories(target.parent_path(), ec);

    DirLock lock(path.string() + ".lock");
    if (!lock.Acquire(&err)) return false;

    bool missing = false;
    auto original = ReadFile(target, &missing);
    if (!original && !missing)
    {
        err = Errno(("не удалось прочитать " + target.string()).c_str());
        return false;
    }

    Json j = Json::object();
    if (original && original->find_first_not_of(" \t\r\n") != std::string::npos)
    {
        j = Json::parse(*original, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            err = target.string() + " содержит некорректный JSON — запись отменена, чтобы не потерять данные";
            return false;
        }
    }

    if (!mutate(j)) return true;

    std::string out = Dump(j);
    const bool trailingNewline = original ? (!original->empty() && original->back() == '\n') : true;
    if (trailingNewline) out.push_back('\n');
    if (original && out == *original) return true; // nothing actually changed

    if (original && backup) BackupOnce(target, *original);

    mode_t mode = 0644;
    struct stat st{};
    if (::stat(target.c_str(), &st) == 0) mode = st.st_mode & 07777;
    else if (target.filename() == ".claude.json") mode = 0600;

    auto tmp = target.parent_path() / ("." + target.filename().string() + ".claudeui-" + std::to_string(::getpid()));
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
    {
        err = Errno(("не удалось создать " + tmp.string()).c_str());
        return false;
    }
    ::fchmod(fd, mode); // umask may have narrowed it

    std::size_t done = 0;
    while (done < out.size())
    {
        ssize_t n = ::write(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
        {
            err = Errno("запись во временный файл");
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    const bool synced    = ::fsync(fd) == 0;
    const int  syncErrno = errno;
    const bool closed    = ::close(fd) == 0; // close even when fsync failed, or the fd leaks
    if (!synced || !closed)
    {
        if (!synced) errno = syncErrno;
        err = Errno(synced ? "close" : "fsync");
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), target.c_str()) != 0)
    {
        err = Errno(("rename в " + target.string()).c_str());
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}
} // namespace config
