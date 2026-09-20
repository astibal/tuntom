#include "cli.hpp"
#include "tunnel.hpp"
#include "stats_control.hpp"

int main(int argc, char** argv) {
    tuntom::logger.ignore_sigpipe();
    using namespace tuntom;
    try {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const std::string mode = argv[1];
        const std::uint16_t tunnel_id =
            parse_tunnel_id(argv[2]);
        const std::string interface_name = argv[3];

        if (mode == "server") {
            Options options;
            parse_options(
                argc,
                argv,
                4,
                options);

            StatsSignals stats_signals;
            Tunnel tunnel(
                tunnel_id,
                true,
                interface_name,
                "",
                options);

            tunnel.run();
            return 0;
        }

        if (mode == "client") {
            if (argc < 5) {
                usage(argv[0]);
                return 1;
            }

            Options options;
            parse_options(
                argc,
                argv,
                5,
                options);

            StatsSignals stats_signals;
            Tunnel tunnel(
                tunnel_id,
                false,
                interface_name,
                argv[4],
                options);

            tunnel.run();
            return 0;
        }

        usage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        log_fatal(error.what());

        return 1;
    }
}
