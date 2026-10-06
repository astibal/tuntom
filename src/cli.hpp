#pragma once

#include "packet.hpp"
#include "info_message.hpp"
#include "privileges.hpp"
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
        << "  " << program_name << " ctl [remote] [control options] [---] <command>\n"
        << "\n"
        << "Tunnel IDs: 1..255, optionally followed by _1.._63 for a group member\n"
        << "\n"
        << "Transport options:\n"
        << "  --mtu <n>             TUN/inner MTU (default 1500)\n"
        << "  --tun-socket <path>   Receive an already-created TUN fd over a Unix socket\n"
        << "  --tun-netns <target>  Fork a provider and create TUN in name, path, or pid:<PID> netns\n"
        << "  --tun-up              Bring an internally provided TUN up before fd transfer\n"
        << privilege_options_help
        << "  --transport-mtu <n>   Transport MTU / initial PMTUD target "
           "(default 1400)\n"
        << "  --udp-send-buffer <n> Effective SO_SNDBUF bytes (default 2097152; 0 = OS default)\n"
        << "  --udp-receive-buffer <n> Effective SO_RCVBUF bytes (default 2097152; 0 = OS default)\n"
        << "  --pmtud               Enable automatic PMTUD (default)\n"
        << "  --no-pmtud            Disable PMTUD and keep --transport-mtu fixed\n"
        << "  --no-ttl-compensate   Do not compensate the extra "
           "tuntom routing hop\n"
        << "  --relay-connect <path>  Multiplex IPC to a local switch (no TUN)\n"
        << "  --relay-port-id <name>  Identity of the relay connection at the switch\n"
        << "  --relay-listen <path>   Listen for local adapter IPC clients (no TUN)\n"
        << "  --switch-socket <path>  Exchange labeled packets with tuntom-switch\n"
        << "  --switch-port-id <name> Stable identity of this switch connection\n"
        << "  --switch-ipc <mode>    auto (default), v1, inline (V2 without mmap)\n"
        << "  --switch-ipc-batch <n> Maximum references per record, 1..16 (default 8)\n"
        << "  --switch-label <n>      Label assigned to DATA received from UDP\n"
        << "  --switch-stack <a,b,..> Trusted ingress stack prepended to classifier labels\n"
        << "  --classifier-file <path> L3/L4 rules assigning stacks to received DATA\n"
        << "  --switch-exit-node      Allow IPC EXIT delivery through a local TUN\n"
        << "\n"
        << "  --info-msg-enable     Advertise loopback IPv4 addresses after each handshake\n"
        << "  --info-field <key=value>  Add a custom INFO field (repeatable; access is reserved)\n"
        << "Statistics:\n"
        << "  --no-stats             Disable periodic stats file writes; keep metrics/socket\n"
        << "  SIGUSR1 / SIGUSR2       Toggle file writes / write snapshot (needs --stats-file)\n"
        << "  --stats-file <path>    Export runtime statistics to file\n"
        << control_auth::help
        << "  --control-socket <path>  Local tuntomctl socket\n"
        << "  --stats-format <fmt>   Statistics format; currently: txt\n"
        << "\n"
        << "Cryptography:\n"
        << "  default               X25519 + AKDF + Ascon-AEAD128 (suite 2)\n"
        << "  --crypto-auth-only    Plaintext payload with AMAC authentication (suite 0)\n"
        << "\n"
        << "  --init-window <s>     Total INIT time window, even 2..86400 (default 300 = +/-150s)\n"
        << "  --auth-command <path> Server-side external AUTH verifier\n"
        << "  --auth-config <path>  Extra configuration argument for AUTH verifier\n"
        << "  --auth-username <name> Client identity for PASSWORD_CHALLENGE\n"
        << "  --auth-response-command <path> Client credential responder (secret on stdin/stdout)\n"
        << "  --auth-timeout <s>    AUTH helper/result timeout, 1..60 (default 10)\n"
        << "  --auth-max-children <n> Concurrent relocated workers, 1..4096 (default 256)\n"
        << "  --config-command <path> Client OS configurator; CONFIG snapshot on stdin\n"
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

        if(control_auth::option(options.control_auth,option,i,argc,argv))continue;
        if(privilege_option(options.privileges,option,i,argc,argv))continue;
        if (option == "--auth-command" || option == "--auth-config" || option == "--auth-username" || option == "--auth-response-command" || option == "--config-command") {
            if(++i>=argc||!argv[i][0])throw std::runtime_error(option+" requires a value");
            auto& target=option=="--auth-command"?options.auth_command:option=="--auth-config"?options.auth_config:option=="--auth-username"?options.auth_username:
                option=="--auth-response-command"?options.auth_response_command:options.config_command;
            if(!target.empty())throw std::runtime_error("duplicate "+option);
            target=argv[i];
        } else if (option == "--auth-timeout") {
            if(++i>=argc)throw std::runtime_error("--auth-timeout requires a value");
            options.auth_timeout_seconds=parse_size_option(option,argv[i],1,60);
        } else if (option == "--auth-max-children") {
            if(++i>=argc)throw std::runtime_error("--auth-max-children requires a value");
            options.auth_max_children=parse_size_option(option,argv[i],1,4096);
        } else if (option == "--crypto-auth-only") {
            options.pfs = options.encrypt_ascon = false;
        } else if (option == "--info-msg-enable") {
            options.info_msg_enable = true;
        } else if (option == "--info-field" || option.rfind("--info-field=", 0) == 0) {
            std::string field;
            if (option == "--info-field") {
                if (++i >= argc) throw std::runtime_error("--info-field requires key=value");
                field = argv[i];
            } else field = option.substr(13);
            const auto equal = field.find('=');
            if (equal == std::string::npos) throw std::runtime_error("--info-field requires key=value");
            const auto key = field.substr(0, equal);
            const auto value = field.substr(equal + 1);
            if (key == "access") throw std::runtime_error("--info-field: access is supplied automatically");
            // Validate without repairing keys. Value sanitization is shared with the collector.
            (void)info::encode({{key, value}});
            if (!options.info_fields.emplace(key, value).second)
                throw std::runtime_error("duplicate --info-field key: " + key);
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
        } else if (option == "--relay-connect" || option == "--relay-listen" || option == "--relay-port-id") {
            if (++i >= argc || !argv[i][0]) throw std::runtime_error(option + " requires a value");
            auto& value = option == "--relay-connect" ? options.relay_connect : option == "--relay-listen" ? options.relay_listen : options.relay_port_id;
            if (!value.empty()) throw std::runtime_error("duplicate " + option);
            value = argv[i];
        } else if (option == "--switch-socket") {
            if (++i >= argc) throw std::runtime_error("--switch-socket requires a value");
            options.switch_socket = argv[i];
            if (options.switch_socket.empty())
                throw std::runtime_error("--switch-socket must not be empty");
        } else if (option == "--switch-ipc") {
            if (++i >= argc) throw std::runtime_error("--switch-ipc requires a value");
            options.switch_ipc.mode = ipc::parse_mode(argv[i]);
        } else if (option == "--switch-ipc-batch") {
            if (++i >= argc) throw std::runtime_error("--switch-ipc-batch requires a value");
            options.switch_ipc.batch = static_cast<std::uint32_t>(parse_size_option(option, argv[i], 1, ipc::max_batch));
        } else if (option == "--switch-label") {
            if (++i >= argc) throw std::runtime_error("--switch-label requires a value");
            if (argv[i][0] == '-') throw std::runtime_error("Invalid --switch-label");
            std::size_t used = 0;
            options.switch_label = std::stoull(argv[i], &used, 0);
            if (argv[i][used] != '\0') throw std::runtime_error("Invalid --switch-label");
            options.switch_label_set = true;
        } else if (option == "--switch-stack") {
            if (++i >= argc || !argv[i][0]) throw std::runtime_error("--switch-stack requires labels");
            if (!options.switch_stack.empty()) throw std::runtime_error("duplicate --switch-stack");
            std::string value=argv[i];std::size_t at=0;
            while(at<value.size()) {
                const auto comma=value.find(',',at);const auto part=value.substr(at,comma-at);
                if(part.empty()||part.front()=='-')throw std::runtime_error("Invalid --switch-stack");
                std::size_t used=0;const auto label=std::stoull(part,&used,0);
                if(used!=part.size())throw std::runtime_error("Invalid --switch-stack");
                options.switch_stack.push_back(label);
                if(options.switch_stack.size()>switch_max_labels)throw std::runtime_error("--switch-stack exceeds 8 labels");
                if(comma==std::string::npos)break;
                at=comma+1;
            }
        } else if (option == "--classifier-file") {
            if (++i >= argc) throw std::runtime_error("--classifier-file requires a value");
            options.classifier_file = argv[i];
            if (options.classifier_file.empty()) throw std::runtime_error("--classifier-file must not be empty");
        } else if (option == "--switch-port-id") {
            if (++i >= argc) throw std::runtime_error("--switch-port-id requires a value");
            options.switch_port_id = argv[i];
            if (options.switch_port_id.empty())
                throw std::runtime_error("--switch-port-id must not be empty");
        } else if (option == "--switch-exit-node") {
            options.switch_exit_node = true;
        } else if (option == "--udp-send-buffer" || option == "--udp-receive-buffer") {
            if (++i >= argc) throw std::runtime_error(option + " requires a value");
            const auto bytes = parse_size_option(option, argv[i], 0, 64 * 1024 * 1024);
            if (option == "--udp-send-buffer") options.udp_send_buffer = bytes;
            else options.udp_receive_buffer = bytes;
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
        } else if (option == "--tun-socket") {
            if (++i >= argc || !argv[i][0]) throw std::runtime_error("--tun-socket requires a path");
            if (!options.tun_socket.empty()) throw std::runtime_error("duplicate --tun-socket");
            options.tun_socket = argv[i];
        } else if (option == "--tun-netns") {
            if (++i >= argc || !argv[i][0]) throw std::runtime_error("--tun-netns requires a target");
            if (!options.tun_netns.empty()) throw std::runtime_error("duplicate --tun-netns");
            options.tun_netns = argv[i];
        } else if (option == "--tun-up") {
            options.tun_up = true;
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
        } else if (option == "--control-socket") {
            if (++i >= argc) throw std::runtime_error("--control-socket requires a value");
            options.control_socket = argv[i];
            if (options.control_socket.empty())
                throw std::runtime_error("--control-socket must not be empty");
        } else {
            throw std::runtime_error(
                "Unknown option: " + option);
        }
    }

    if (options.relay_mode()) {
        if ((!options.relay_connect.empty() && !options.relay_listen.empty()) ||
            (options.relay_connect.empty() != options.relay_port_id.empty()) ||
            !options.switch_socket.empty() || !options.switch_port_id.empty() || options.switch_label_set ||
            options.switch_exit_node || !options.classifier_file.empty())
            throw std::runtime_error("relay requires connect + port ID or listen, exclusively of switch/classifier mode");
        if (!options.relay_port_id.empty()) (void)encode_switch_registration(options.relay_port_id);
    } else if (!options.relay_port_id.empty()) throw std::runtime_error("--relay-port-id requires --relay-connect");
    if (!options.tun_socket.empty() &&
        (options.relay_mode() || (!options.switch_socket.empty() && !options.switch_exit_node)))
        throw std::runtime_error("--tun-socket requires a mode which uses a TUN");
    if (!options.tun_netns.empty() && !options.tun_socket.empty())
        throw std::runtime_error("--tun-netns and --tun-socket are mutually exclusive");
    if ((!options.tun_netns.empty() || options.tun_up) &&
        (options.relay_mode() || (!options.switch_socket.empty() && !options.switch_exit_node)))
        throw std::runtime_error("--tun-netns/--tun-up require a mode which uses a TUN");
    if (options.tun_up && options.tun_netns.empty())
        throw std::runtime_error("--tun-up requires --tun-netns");
    if (options.switch_socket.empty() and
        (options.switch_label_set or not options.switch_stack.empty() or options.switch_exit_node or
         not options.switch_port_id.empty() or not options.classifier_file.empty())) {
        throw std::runtime_error(
            "--switch-label, --switch-exit-node and --classifier-file require --switch-socket");
    }
    if (options.switch_label_set and not options.switch_stack.empty()) {
        throw std::runtime_error("--switch-label and --switch-stack are mutually exclusive");
    }
    if (not options.switch_socket.empty() and not options.switch_label_set and options.switch_stack.empty()) {
        throw std::runtime_error("--switch-socket requires --switch-label or --switch-stack");
    }
    if (not options.switch_socket.empty() and options.switch_port_id.empty()) {
        throw std::runtime_error("--switch-socket requires --switch-port-id");
    }
    control_auth::validate(options.control_auth);
    if(options.auth_username.empty()!=options.auth_response_command.empty())
        throw std::runtime_error("--auth-username and --auth-response-command require each other");
    if(!options.auth_config.empty()&&options.auth_command.empty())
        throw std::runtime_error("--auth-config requires --auth-command");
    (void)info::encode_access({}, options.info_fields);

}

inline std::uint16_t parse_tunnel_id(const char* value) {
    const std::string text(value);
    const auto separator = text.find('_');
    const auto parse_part = [](const std::string& part, unsigned maximum) {
        if (part.empty() or part.size() > 3 or part.front() == '0' or
            part.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Invalid tunnel ID; use 1..255 or ID_1..ID_63");
        const auto number = std::stoul(part);
        if (number > maximum)
            throw std::runtime_error("Invalid tunnel ID; use 1..255 or ID_1..ID_63");
        return number;
    };
    const auto group = parse_part(text.substr(0, separator), 255);
    const auto member = separator == std::string::npos ? 0UL :
        parse_part(text.substr(separator + 1), 63);
    return static_cast<std::uint16_t>(group + 256 * member);
}

} // namespace tuntom
