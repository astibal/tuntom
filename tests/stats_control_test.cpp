#include "../src/cli.hpp"
#include "../src/stats_control.hpp"
#include <iostream>
using namespace tuntom;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() {
    for (bool reverse : {false, true}) {
        Options options;
        const char* a[] = {"tuntom", "--no-stats", "--stats-file", "/tmp/example.stats"};
        const char* b[] = {"tuntom", "--stats-file", "/tmp/example.stats", "--no-stats"};
        parse_options(4, const_cast<char**>(reverse ? b : a), 1, options);
        require(options.stats_disabled && options.stats_file == "/tmp/example.stats", "no-stats preserves path, independent of ordering");
    }
    Options options;
    const char* a[] = {"tuntom", "--no-stats"};
    parse_options(2, const_cast<char**>(a), 1, options);
    require(options.stats_disabled && options.stats_file.empty(), "no path required");
    struct sigaction before1 {}, before2 {}, after1 {}, after2 {};
    sigaction(SIGUSR1, nullptr, &before1);
    sigaction(SIGUSR2, nullptr, &before2);
    {
        StatsSignals signals;
        auto request = take_stats_requests();
        require(!request.toggle && !request.snapshot, "no initial requests");
        raise(SIGUSR1);
        request = take_stats_requests();
        require(request.toggle && !request.snapshot, "toggle request");
        require(!take_stats_requests().toggle, "toggle consumed once");
        raise(SIGUSR1); raise(SIGUSR1);
        require(!take_stats_requests().toggle, "two delivered toggles cancel");
        raise(SIGUSR2);
        request = take_stats_requests();
        require(!request.toggle && request.snapshot, "snapshot does not toggle");
        require(!take_stats_requests().snapshot, "snapshot consumed once");
        raise(SIGUSR1); raise(SIGUSR2);
        request = take_stats_requests();
        require(request.toggle && request.snapshot, "independent requests");
        // Consumption must preserve the caller's mask, including pending signals.
        sigset_t mask, previous;
        sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
        sigprocmask(SIG_BLOCK, &mask, &previous);
        raise(SIGUSR1); raise(SIGUSR2);
        request = take_stats_requests();
        require(!request.toggle && request.snapshot, "blocked toggle remains pending");
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        require(take_stats_requests().toggle, "pending toggle is not lost");
        require(!stats_write_requested(true, true), "disabled periodic write");
        require(stats_write_requested(true, true, true), "snapshot bypasses no-stats");
        require(stats_write_requested(true, false), "enabled periodic write");
        require(!stats_write_requested(false, false, true), "snapshot requires a path");
    }
    sigaction(SIGUSR1, nullptr, &after1);
    sigaction(SIGUSR2, nullptr, &after2);
    require(before1.sa_handler == after1.sa_handler && before2.sa_handler == after2.sa_handler, "handler restoration");
    std::cout << "PASS: no-stats CLI precedence, retained path, real toggle/snapshot signals and handler restoration\n";
}
