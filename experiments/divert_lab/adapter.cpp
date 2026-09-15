#include "common.hpp"
#include "switch_client.hpp"
#include "tun_device.hpp"
#include <csignal>
#include <iostream>
#include <unordered_map>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}

int main(int argc, char** argv) {
    using namespace divert_lab;
    using tuntom::SwitchOpcode;
    try {
        if (argc != 7) throw std::runtime_error("usage: adapter SOCKET TUN-IN TUN-OUT COOKIE TRACE MTU");
        Codec codec(argv[4]);
        Trace trace(argv[5]);
        const auto mtu = std::stoul(argv[6]);
        if (mtu < 1280 || mtu > 65535) throw std::runtime_error("invalid MTU");
        tuntom::TunDevice tun_in(argv[2], mtu), tun_out(argv[3], mtu);
        tun_in.set_up(); tun_out.set_up();
        tuntom::ipc::Options options;
        options.mode = tuntom::ipc::Mode::legacy;
        tuntom::SwitchClient input(argv[1], "divert-in", options), output(argv[1], "divert-out", options);
        for (auto* client : {&input, &output}) {
            client->start_connect(tuntom::SwitchClient::Clock::now());
            while (client->connecting()) {
                pollfd descriptor{client->fd(), client->poll_events(), 0};
                ::poll(&descriptor, 1, 100);
                client->advance_connect(tuntom::SwitchClient::Clock::now(), descriptor.revents);
            }
            if (!client->connected()) throw std::runtime_error("cannot connect adapter switch port");
        }
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop); std::signal(SIGPIPE, SIG_IGN);
        std::unordered_map<tuntom::FlowKey, Labels, tuntom::FlowHash> routes;
        Admission admission;
        const auto started = std::chrono::steady_clock::now();
        auto remember = [&](const tuntom::FlowKey& key, const Labels& labels) {
            const auto existing = routes.find(key);
            if (existing != routes.end() && codec.split(existing->second).origin() != codec.split(labels).origin())
                throw std::runtime_error("overlapping flow identities are outside this lab");
            if (existing == routes.end() && routes.size() >= 100000)
                throw std::runtime_error("lab label cache full; no active entries evicted");
            routes[key] = labels;
        };
        auto send = [&](tuntom::SwitchClient& client, const Labels& labels,
                        const std::uint8_t* payload, std::size_t size, const std::string& side) {
            const auto result = client.send_frame(SwitchOpcode::switch_packet, labels.data(), labels.size(), payload, size);
            if (result < 0) throw std::runtime_error("adapter send failed");
            trace.event("to-switch", side, "switch", labels, payload, size);
        };
        trace.event("ready", "adapter", "");
        while (!stopping) {
            pollfd descriptors[] = {{input.fd(), POLLIN, 0}, {output.fd(), POLLIN, 0},
                                    {tun_in.fd(), POLLIN, 0}, {tun_out.fd(), POLLIN, 0}};
            if (::poll(descriptors, 4, 100) < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("adapter poll failed");
            }
            for (int i = 0; i < 4; ++i) {
                if (!descriptors[i].revents) continue;
                std::array<std::uint8_t, 66000> buffer{};
                auto& client = (i % 2 == 0) ? input : output;
                auto& tun = (i % 2 == 0) ? tun_in : tun_out;
                const std::string side = (i % 2 == 0) ? "divert-in" : "divert-out";
                const auto n = i < 2 ? client.receive(buffer.data(), buffer.size()) : tun.read_packet(buffer.data(), buffer.size());
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (n <= 0) throw std::runtime_error("adapter endpoint disconnected");
                if (i < 2) {
                    tuntom::SwitchFrameView frame;
                    if (!tuntom::decode_switch_frame(buffer.data(), n, frame) || frame.opcode != SwitchOpcode::exit_packet)
                        throw std::runtime_error("adapter received invalid EXIT");
                    auto labels = labels_of(frame);
                    auto env = codec.split(labels);
                    if (!env.present) throw std::runtime_error("adapter missing divert envelope");
                    PacketInfo packet;
                    if (!packet_info(frame.payload, frame.payload_size, packet)) {
                        trace.event("unsupported_drop", side, "", labels); continue;
                    }
                    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    if (i == 0 && !admission.proxy(packet, seconds)) {
                        env.body[1] = bypass;
                        send(client, codec.attach(env.base, env.body), frame.payload, frame.payload_size, side);
                        trace.event("existing", side, "switch", labels, frame.payload, frame.payload_size);
                        continue;
                    }
                    remember(packet.flow, labels);
                    // Needed for a proxy-generated client SYN/ACK before any server reply.
                    if (i == 0 && !routes.count(tuntom::reverse_key(packet.flow)))
                        remember(tuntom::reverse_key(packet.flow), labels);
                    if (tun.write_packet(frame.payload, frame.payload_size) != static_cast<ssize_t>(frame.payload_size))
                        throw std::runtime_error("TUN injection failed");
                    trace.event("to-router", side, i == 0 ? "di0" : "do0", labels, frame.payload, frame.payload_size);
                } else {
                    PacketInfo packet;
                    if (!packet_info(buffer.data(), n, packet)) { trace.event("unsupported_drop", side, ""); continue; }
                    const auto found = routes.find(packet.flow);
                    if (found == routes.end()) throw std::runtime_error("TUN packet has no shared label context");
                    auto env = codec.split(found->second);
                    env.body[1] = i == 2 ? to_client : onward;
                    const auto labels = codec.attach(env.base, env.body);
                    trace.event("from-router", i == 2 ? "di0" : "do0", side, labels, buffer.data(), n);
                    send(client, labels, buffer.data(), n, side);
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "divert lab adapter: " << error.what() << '\n';
        return 1;
    }
}
