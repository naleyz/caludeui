// Headless check for the Terminal class, no GUI involved.
//
//   term_probe replay <file> [cols rows]   feed captured PTY bytes, dump the grid
//   term_probe live <cwd> [seconds]        run `claude` in <cwd>, dump the grid,
//                                          send /exit and report how it ended
#include "term/terminal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace
{
void Dump(Terminal& t, int rows)
{
    auto snap = t.snapshot(0, rows);
    std::printf("── %dx%d alt=%d mouse=%d cursor=(%d,%d) visible=%d title=\"%s\"\n", snap.cols, snap.screenRows,
                snap.altScreen, snap.mouseTracking, snap.cursorX, snap.cursorY, snap.cursorVisible,
                t.title().c_str());
    for (std::size_t i = 0; i < snap.rows.size(); ++i)
    {
        auto line = t.textRange(snap.firstAbsLine + static_cast<std::int64_t>(i), 0,
                                snap.firstAbsLine + static_cast<std::int64_t>(i), snap.cols);
        std::printf("%3zu|%s\n", i, line.c_str());
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s replay <file> [cols rows] | live <cwd> [seconds]\n", argv[0]);
        return 2;
    }
    std::string mode = argv[1];

    if (mode == "live")
    {
        int seconds = argc > 3 ? std::atoi(argv[3]) : 8;
        Terminal t;
        Terminal::SpawnOptions opts;
        opts.argv     = {"claude"};
        opts.cwd      = argv[2];
        opts.cols     = 120;
        opts.rows     = 40;
        opts.unsetEnv = {"CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT", "CLAUDE_CODE_SESSION_ID",
                         "CLAUDE_CODE_MESSAGING_SOCKET", "CLAUDE_CODE_MESSAGING_TOKEN",
                         "CLAUDE_CODE_BRIDGE_SESSION_ID", "CLAUDE_CODE_CHILD_SESSION",
                         "CLAUDE_CODE_SESSION_ATTENDED", "CLAUDE_PID", "CLAUDE_EFFORT", "CLAUDE_CODE_EXECPATH"};
        std::string err;
        if (!t.start(opts, &err))
        {
            std::fprintf(stderr, "start failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("pid %d\n", t.pid());
        for (int i = 0; i < seconds * 10; ++i)
        {
            t.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Dump(t, 40);

        t.sendInput("/exit\r");
        auto t0 = std::chrono::steady_clock::now();
        while (t.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
        {
            t.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (t.running())
        {
            std::printf("still running after /exit — stopping\n");
            t.stop();
            return 1;
        }
        std::printf("exited after /exit in %lld ms, code %d\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count()),
                    t.exitCode().value_or(-1));
        Dump(t, 40);
        return 0;
    }

    if (mode == "replay")
    {
        int cols = argc > 4 ? std::atoi(argv[3]) : 120;
        int rows = argc > 4 ? std::atoi(argv[4]) : 40;
        std::ifstream f(argv[2], std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        // `cat` a file into the PTY so the bytes go through the real parser path.
        Terminal t;
        Terminal::SpawnOptions opts;
        opts.argv = {"cat", argv[2]};
        opts.cols = cols;
        opts.rows = rows;
        std::string err;
        if (!t.start(opts, &err))
        {
            std::fprintf(stderr, "start failed: %s\n", err.c_str());
            return 1;
        }
        for (int i = 0; i < 20 && t.running(); ++i)
        {
            t.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        t.poll();
        std::printf("%zu bytes\n", data.size());
        Dump(t, rows);
        return 0;
    }
    return 2;
}
