#include "cli.hpp"
#include "control_cli.hpp"
#include "tunnel.hpp"
#include "stats_control.hpp"

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "ctl") return tuntom_control_main(argc - 1, argv + 1);
    tuntom::logger.ignore_sigpipe();
    using namespace tuntom;
    try {
        if (argc == 2 && std::string(argv[1]) == "access-worker") {
            std::array<std::uint8_t, 65536> wire{};
            const auto n = ::recv(child::bootstrap_fd, wire.data(), wire.size(), 0);
            child::Bootstrap profile;
            if (n <= 0 || !child::decode({wire.begin(), wire.begin() + n}, profile))
                throw std::runtime_error("invalid access-worker bootstrap");
            Options options;
            options.switch_socket = profile.switch_socket;
            options.switch_port_id = profile.port_id;
            options.switch_stack = profile.ingress_stack;
            Tunnel tunnel(profile.tunnel_id, true, "", "", options,
                          child::udp_fd, &profile);
            if (::send(child::bootstrap_fd, "READY", 5, MSG_NOSIGNAL) != 5)
                throw std::runtime_error("access-worker readiness failed");
            tunnel.run();
            return 0;
        }
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
            if (!options.auth_username.empty() || !options.config_command.empty()) throw std::runtime_error("client AUTH/CONFIG options are invalid in server mode");

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
            if (!options.auth_command.empty() || !options.auth_config.empty()) throw std::runtime_error("--auth-command/--auth-config are valid only in server mode");

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
        std::cerr << "FATAL: " << error.what() << '\n';

        return 1;
    }
}
