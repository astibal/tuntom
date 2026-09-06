#include "exit_adapter.hpp"
#include "../ipc/switch_protocol.hpp"
#include "../switch_client.hpp"
#include "../tun_device.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

std::size_t parse_size(
    const std::string& option, const char* value,
    std::size_t minimum, std::size_t maximum) {
    if (value[0] == '-') throw std::runtime_error("Invalid " + option);
    std::size_t used = 0;
    const auto parsed = std::stoull(value, &used, 10);
    if (value[used] != '\0' or parsed < minimum or parsed > maximum)
        throw std::runtime_error("Invalid " + option);
    return static_cast<std::size_t>(parsed);
}

void usage(const char* program) {
    std::cerr
        << "Usage: " << program << " <ifname> --switch-socket <path>"
        << " --switch-port-id <name> [options]\n"
        << "  --mtu <n>             TUN MTU (default 1500)\n"
        << "  --l4-capacity <n>     L4 LRU entries (default 1000000)\n"
        << "  --l3-capacity <n>     L3 LRU entries (default 250000)\n"
        << "  --l4-timeout <s>      L4 idle timeout (default 120)\n"
        << "  --l3-timeout <s>      L3 idle timeout (default 30)\n";
}

} // namespace

int main(int argc, char** argv) {
    using namespace tuntom;
    try {
        if (argc < 2) {
            usage(argv[0]);
            return 1;
        }
        const std::string interface_name = argv[1];
        std::string socket_path;
        std::string port_id;
        std::size_t mtu = 1500;
        std::size_t l4_capacity = 1000000;
        std::size_t l3_capacity = 250000;
        std::size_t l4_timeout = 120;
        std::size_t l3_timeout = 30;
        for (int index = 2; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" or option == "-h") {
                usage(argv[0]);
                return 0;
            }
            if (++index >= argc) throw std::runtime_error(option + " requires a value");
            if (option == "--switch-socket") socket_path = argv[index];
            else if (option == "--switch-port-id") port_id = argv[index];
            else if (option == "--mtu") mtu = parse_size(option, argv[index], 576, 65535);
            else if (option == "--l4-capacity")
                l4_capacity = parse_size(option, argv[index], 1, 100000000);
            else if (option == "--l3-capacity")
                l3_capacity = parse_size(option, argv[index], 1, 100000000);
            else if (option == "--l4-timeout")
                l4_timeout = parse_size(option, argv[index], 1, 86400);
            else if (option == "--l3-timeout")
                l3_timeout = parse_size(option, argv[index], 1, 86400);
            else throw std::runtime_error("Unknown option: " + option);
        }
        if (socket_path.empty() or port_id.empty())
            throw std::runtime_error("--switch-socket and --switch-port-id are required");

        TunDevice tun(interface_name, mtu);
        tun.set_up();
        SwitchClient switch_client(socket_path, port_id);
        ExitAdapterRoutes routes(
            l3_capacity, l4_capacity,
            std::chrono::seconds(l3_timeout),
            std::chrono::seconds(l4_timeout));

        struct sigaction action {};
        action.sa_handler = request_stop;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);

        std::vector<std::uint8_t> packet(65535);
        std::vector<std::uint8_t> frame_buffer(
            switch_base_header_size + switch_max_labels * switch_label_size +
            std::numeric_limits<std::uint16_t>::max());
        auto next_connect = std::chrono::steady_clock::now();

        while (not stop_requested) {
            const auto now = std::chrono::steady_clock::now();
            if (not switch_client.connected() and now >= next_connect) {
                switch_client.connect_now();
                next_connect = now + std::chrono::seconds(1);
            }

            pollfd descriptors[2] {
                {tun.fd(), POLLIN, 0},
                {switch_client.fd(), POLLIN, 0},
            };
            const int ready = ::poll(descriptors, 2, 1000);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
            }

            if (descriptors[0].revents & POLLIN) {
                const ssize_t size = tun.read_packet(packet.data(), packet.size());
                std::vector<std::uint64_t> labels;
                if (size > 0 and switch_client.connected() and
                    routes.lookup(packet.data(), static_cast<std::size_t>(size), labels)) {
                    const auto frame = encode_switch_frame(
                        SwitchOpcode::switch_packet, labels, packet.data(),
                        static_cast<std::size_t>(size));
                    if (switch_client.send(frame.data(), frame.size()) !=
                        static_cast<ssize_t>(frame.size())) switch_client.disconnect();
                }
            }

            if (switch_client.connected() and
                (descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                switch_client.disconnect();
                next_connect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            } else if (switch_client.connected() and (descriptors[1].revents & POLLIN)) {
                const ssize_t size = switch_client.receive(frame_buffer.data(), frame_buffer.size());
                if (size <= 0 or static_cast<std::size_t>(size) > frame_buffer.size()) {
                    switch_client.disconnect();
                    next_connect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    continue;
                }
                SwitchFrameView frame;
                if (decode_switch_frame(frame_buffer.data(), static_cast<std::size_t>(size), frame) and
                    frame.opcode == SwitchOpcode::exit_packet) {
                    std::vector<std::uint64_t> labels;
                    labels.reserve(frame.label_count);
                    for (std::size_t index = 0; index < frame.label_count; ++index)
                        labels.push_back(frame.label(index));
                    if (routes.learn(frame.payload, frame.payload_size, labels) and
                        tun.write_packet(frame.payload, frame.payload_size) !=
                            static_cast<ssize_t>(frame.payload_size)) {
                        throw std::runtime_error("TUN write failed: " + std::string(std::strerror(errno)));
                    }
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
