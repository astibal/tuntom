#pragma once

#include "packet.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace tuntom {

inline void usage(const char* program_name) {
    std::cerr
        << "Usage:\n"
        << "  " << program_name
        << " server <id> <ifname> [options]\n"
        << "  " << program_name
        << " client <id> <ifname> <host> [options]\n"
        << "\n"
        << "Transport options:\n"
        << "  --mtu <n>             TUN/inner MTU (default 1500)\n"
        << "  --transport-mtu <n>   Transport MTU / initial PMTUD target "
           "(default 1400)\n"
        << "  --pmtud               Enable automatic PMTUD (default)\n"
        << "  --no-pmtud            Disable PMTUD and keep --transport-mtu fixed\n"
        << "  --no-ttl-compensate   Do not compensate the extra "
           "tuntom routing hop\n"
        << "  --switch-socket <path>  Exchange labeled packets with tuntom-switch\n"
        << "  --switch-label <n>      Label assigned to DATA received from UDP\n"
        << "  --switch-exit-node      Allow IPC EXIT delivery through a local TUN\n"
        << "\n"
        << "Statistics:\n"
        << "  --no-stats             Disable stats writes and optional sampling\n"
        << "  SIGUSR1 / SIGUSR2       Toggle stats / snapshot now (needs --stats-file)\n"
        << "  --stats-file <path>    Export runtime statistics to file\n"
        << "  --stats-format <fmt>   Statistics format; currently: txt\n"
        << "\n"
        << "Encryption (optional):\n"
        << "  --pfs                 Require X25519 + AKDF + Ascon-AEAD128 (suite 2)\n"
        << "  --encrypt-ascon       Require Ascon-AEAD128 payload encryption\n"
        << "\n"
        << "  --init-window <s>     Total INIT time window, even 2..86400 (default 300 = +/-150s)\n"
        << "Logging:\n"
        << "  default                informational drops/errors only\n"
        << "  --debug                packet/fragment protocol details\n"
        << "  --quiet                suppress non-fatal logging\n"
        << "\n"
        << "Environment:\n"
        << "  TUNTOM_SECRET          32 hex characters "
           "(128-bit master key)\n";
}

inline std::size_t parse_size_option(
    const std::string& option,
    const char* value,
    std::size_t minimum,
    std::size_t maximum) {

    std::size_t parsed_characters = 0;
    const unsigned long parsed =
        std::stoul(
            value,
            &parsed_characters,
            10);

    if (
        value[parsed_characters] != '\0' or
        parsed < minimum or
        parsed > maximum) {

        throw std::runtime_error(
            option + " must be in range " +
            std::to_string(minimum) +
            ".." +
            std::to_string(maximum));
    }

    return static_cast<std::size_t>(parsed);
}

inline void parse_options(
    int argc,
    char** argv,
    int first_option,
    Options& options) {

    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];

        if (option == "--pfs") {
            options.pfs = options.encrypt_ascon = true;
        } else if (option == "--encrypt-ascon") {
            options.encrypt_ascon = true;
        } else if (option == "--init-window") {
            if (++i >= argc) throw std::runtime_error("--init-window requires a value");
            options.init_window = parse_size_option(option, argv[i], 2, 86400);
            if (options.init_window % 2 != 0)
                throw std::runtime_error("--init-window must be an even number of seconds");
        } else if (option == "--allow-v1") {
            throw std::runtime_error("V5 does not support --allow-v1");
        } else if (option == "--allow-v2") {
            throw std::runtime_error("V5 does not support --allow-v2");
        } else if (option == "--debug") {
            log_level = LogLevel::debug;
        } else if (option == "--quiet") {
            log_level = LogLevel::quiet;
        } else if (option == "--no-pmtud") {
            options.pmtud_auto = false;
        } else if (option == "--pmtud") {
            options.pmtud_auto = true;
        } else if (option == "--no-ttl-compensate") {
            options.ttl_compensate = false;
        } else if (option == "--ttl-compensate") {
            options.ttl_compensate = true;
        } else if (option == "--switch-socket") {
            if (++i >= argc) throw std::runtime_error("--switch-socket requires a value");
            options.switch_socket = argv[i];
            if (options.switch_socket.empty())
                throw std::runtime_error("--switch-socket must not be empty");
        } else if (option == "--switch-label") {
            if (++i >= argc) throw std::runtime_error("--switch-label requires a value");
            if (argv[i][0] == '-') throw std::runtime_error("Invalid --switch-label");
            std::size_t used = 0;
            options.switch_label = std::stoull(argv[i], &used, 0);
            if (argv[i][used] != '\0') throw std::runtime_error("Invalid --switch-label");
            options.switch_label_set = true;
        } else if (option == "--switch-exit-node") {
            options.switch_exit_node = true;
        } else if (option == "--mtu") {
            if (++i >= argc) {
                throw std::runtime_error("--mtu requires a value");
            }

            options.tun_mtu =
                parse_size_option(
                    "--mtu",
                    argv[i],
                    576,
                    max_ip_packet_size);
        } else if (option == "--transport-mtu") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--transport-mtu requires a value");
            }

            options.transport_mtu =
                parse_size_option(
                    "--transport-mtu",
                    argv[i],
                    min_transport_mtu,
                    max_ip_packet_size);
        } else if (option == "--no-stats") {
            options.stats_disabled = true;
        } else if (option == "--stats-file") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--stats-file requires a value");
            }

            options.stats_file = argv[i];

            if (options.stats_file.empty()) {
                throw std::runtime_error(
                    "--stats-file must not be empty");
            }
        } else if (option == "--stats-format") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--stats-format requires a value");
            }

            const std::string format = argv[i];

            if (format == "txt") {
                options.stats_format = StatsFormat::txt;
            } else {
                throw std::runtime_error(
                    "Unsupported stats format: " + format +
                    " (currently supported: txt)");
            }
        } else {
            throw std::runtime_error(
                "Unknown option: " + option);
        }
    }

    if (options.switch_socket.empty() and
        (options.switch_label_set or options.switch_exit_node)) {
        throw std::runtime_error(
            "--switch-label and --switch-exit-node require --switch-socket");
    }
    if (not options.switch_socket.empty() and not options.switch_label_set) {
        throw std::runtime_error("--switch-socket requires --switch-label");
    }
}

inline std::uint16_t parse_tunnel_id(const char* value) {
    const unsigned long parsed = std::stoul(value);

    if (parsed == 0 or parsed > 255) {
        throw std::runtime_error(
            "Tunnel id must be in range 1..255");
    }

    return static_cast<std::uint16_t>(parsed);
}

} // namespace tuntom
