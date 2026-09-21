#include "../routed_control.hpp"
#include "flows.hpp"
#include "paths.hpp"
#include "shared_flows.hpp"
#include "../via/adapter.hpp"
#include "../via/registration.hpp"
#include "../common.hpp"
#include "../switch_client.hpp"
#include "../tun_device.hpp"
#include "../control_socket.hpp"
#include "../runtime_recovery.hpp"
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <sys/prctl.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
std::size_t number(const std::string& value, std::size_t lo, std::size_t hi) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected an unsigned number");
    const auto n = std::stoull(value);
    if (n < lo || n > hi) throw std::runtime_error("numeric argument outside permitted range");
    return static_cast<std::size_t>(n);
}
void usage(const char* program) {
    std::cerr << "Usage: " << program << " TUN-IN TUN-OUT [--switch-socket PATH | --relay-path ID=SOCKET ...] (--cookie ABC | --via-instance ID) [options]\n"
        << "  --via-instance ID         opt-in VIA; replaces --cookie, ID shared by both sides\n"
        << "  --relay-path ID=SOCKET    VIA only; up to 16 independent IPC pairs sharing the TUNs\n"
        << "                           registers NAME.ID~via:c|s:INSTANCE#path-ID\n"
        << "  --shared-flows PATH      shared VIA flow/admission table; exactly one --relay-path\n"
        << "                           one process per path, common multiqueue TUN pair\n"
        << "  --side both|in|out       default both; in/out requires --shared-flows\n"
        << "                           opens only that side's TUN and relay channel\n"
        << "                           registers NAME.ID~via:c|s:INSTANCE (no path suffix)\n"
        << "  --admission immediate|warmup  default immediate for VIA, warmup for legacy\n"
        << "  --divert-in-port NAME      default divert-in\n"
        << "  --divert-out-port NAME     default divert-out\n"
        << "  --control-socket PATH     tuntomctl PATH show stats\n"
        << tuntom::control_auth::help
        << "  --switch-ipc auto|v1|inline  default auto\n"
        << "  --switch-ipc-batch N      1..16, default 8\n"
        << "  --mtu N                   576..65535, default 1500\n"
        << "  --flow-capacity N         1..100000000, default 100000\n"
        << "  --flow-idle-seconds N     1..604800, default 86400\n"
        << "  --admission-capacity N    entries per learning set, default 100000\n"
        << "Creates/opens TUNs in the current network namespace; does not configure routes or VRFs.\n";
}
struct Stats {
    std::uint64_t switch_rx = 0, switch_tx = 0, tun_rx = 0, tun_tx = 0, bypass = 0;
    std::uint64_t invalid = 0, unsupported = 0, capacity = 0, conflict = 0, miss = 0;
    std::uint64_t disconnected = 0, reconnects = 0, send_errors = 0, backpressure = 0, tun_errors = 0;
    void write(std::ostream& out) const {
        out << "switch_rx_packets=" << switch_rx << "\nswitch_tx_packets=" << switch_tx
            << "\ntun_rx_packets=" << tun_rx << "\ntun_tx_packets=" << tun_tx << "\nbypass_packets=" << bypass
            << "\ninvalid_drops=" << invalid << "\nunsupported_drops=" << unsupported
            << "\ncapacity_drops=" << capacity << "\ncontext_conflict_drops=" << conflict
            << "\ncontext_miss_drops=" << miss << "\ndisconnected_drops=" << disconnected
            << "\nswitch_reconnects=" << reconnects << "\nsend_error_drops=" << send_errors
            << "\nbackpressure_drops=" << backpressure << "\ntun_errors=" << tun_errors << '\n';
    }
};
}

int main(int argc, char** argv) {
    using namespace tuntom;
    using namespace tuntom::divert;
    (void)::prctl(PR_SET_NAME, "tuntom-divert", 0UL, 0UL, 0UL);
    logger.ignore_sigpipe();
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(argv[0]); return 0; }
        if (argc < 3) { usage(argv[0]); return 1; }
        const std::string in_name = argv[1], out_name = argv[2];
        if (in_name.empty() || out_name.empty() || in_name == out_name || in_name.size() >= IFNAMSIZ || out_name.size() >= IFNAMSIZ)
            throw std::runtime_error("two distinct TUN names of at most 15 bytes are required");
        std::string socket, cookie, control_path, in_port = "divert-in", out_port = "divert-out";
        std::string instance, admission_mode, shared_path, worker_side = "both";
        std::vector<std::string> relay_paths;
        std::size_t mtu = 1500, capacity = 100000, admission_capacity = 100000, idle = 86400;
        ipc::Options options;
        tuntom::control_auth::Config control_auth_config;
        RoutedControl routed_control("divert-adapter");
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help" || option == "-h") { usage(argv[0]); return 0; }
            if(tuntom::control_auth::option(control_auth_config,option,i,argc,argv))continue;
            if (++i >= argc) throw std::runtime_error(option + " requires a value");
            const std::string value = argv[i];
            if (option == "--switch-socket") socket = value;
            else if (option == "--relay-path") relay_paths.push_back(value);
            else if (option == "--side") worker_side = value;
            else if (option == "--shared-flows") {
                if (value.empty()) throw std::runtime_error("--shared-flows requires a nonempty path");
                shared_path = value;
            }
            else if (option == "--cookie") cookie = value;
            else if (option == "--via-instance") instance = value;
            else if (option == "--admission") admission_mode = value;
            else if (option == "--control-socket") control_path = value;
            else if (option == "--divert-in-port") in_port = value;
            else if (option == "--divert-out-port") out_port = value;
            else if (option == "--switch-ipc") options.mode = ipc::parse_mode(value);
            else if (option == "--switch-ipc-batch") options.batch = static_cast<std::uint32_t>(number(value, 1, ipc::max_batch));
            else if (option == "--mtu") mtu = number(value, 576, 65535);
            else if (option == "--flow-capacity") capacity = number(value, 1, 100000000);
            else if (option == "--admission-capacity") admission_capacity = number(value, 1, 100000000);
            else if (option == "--flow-idle-seconds") idle = number(value, 1, 604800);
            else throw std::runtime_error("unknown option: " + option);
        }
        if (worker_side != "both" && worker_side != "in" && worker_side != "out")
            throw std::runtime_error("--side expects both, in or out");
        const bool split = worker_side != "both";
        if (split && shared_path.empty()) throw std::runtime_error("--side in/out requires --shared-flows");
        const auto has_side = [&](unsigned side) { return !split || side == (worker_side == "in" ? 0U : 1U); };
        const auto paths = adapter_paths(socket, relay_paths, instance, in_port, out_port, split);
        const bool via_mode = !instance.empty();
        if (admission_mode.empty()) admission_mode = via_mode ? "immediate" : "warmup";
        if (admission_mode != "immediate" && admission_mode != "warmup") throw std::runtime_error("expected immediate or warmup admission");
        via::AdapterCodec codec(via_mode, cookie);
        std::unique_ptr<SharedFlows> shared;
        if (!shared_path.empty()) {
            if (!via_mode || relay_paths.size() != 1)
                throw std::runtime_error("--shared-flows requires VIA and exactly one --relay-path");
            std::vector<std::string> group{instance, in_name, out_name, in_port, out_port, std::to_string(mtu)};
            if (split) group.push_back("split-sides");
            shared = std::make_unique<SharedFlows>(shared_path, SharedFlows::group(group),
                split ? worker_side + ":" + paths[0].id : paths[0].id,
                capacity, std::chrono::seconds(idle), admission_capacity, admission_mode == "warmup");
        }
        std::vector<std::unique_ptr<SwitchClient>> clients;
        for (const auto& path : paths) {
            clients.push_back(has_side(0) ? std::make_unique<SwitchClient>(path.socket, path.input, options) : nullptr);
            clients.push_back(has_side(1) ? std::make_unique<SwitchClient>(path.socket, path.output, options) : nullptr);
        }
        std::unique_ptr<ControlSocket> control;
        if (!control_path.empty()) control = std::make_unique<ControlSocket>(control_path);
        std::unique_ptr<TunDevice> tuns[2];
        for (unsigned side = 0; side < 2; ++side) if (has_side(side)) {
            tuns[side] = std::make_unique<TunDevice>(side ? out_name : in_name, mtu, bool(shared));
            if (shared) tuns[side]->set_queue(false);
            tuns[side]->set_up();
        }
        std::vector<Clock::time_point> next_connect(clients.size());
        std::vector<pollfd> fds(clients.size() + 3);
        // Each path gets one visit to its active TUNs as well as its IPC sockets.
        // Otherwise N busy IPC pairs could inject N times faster than we drain TUNs.
        const auto sources = paths.size() * 4;
        Clock::time_point activated{};
        bool active = false;
        bool queues_active = !shared;
        Admission admission(admission_capacity);
        BasicRoutes<via::AdapterContext> routes(capacity, std::chrono::seconds(idle), via_mode);
        Stats stats;
        RuntimeRecovery recovery;
        std::array<std::uint8_t, ipc::max_frame> buffer{};
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        const auto seconds = [&](Clock::time_point now) {
            if (shared) return shared->seconds(now);
            return active ? std::chrono::duration<double>(now - activated).count() : 0.0;
        };
        const auto connected = [&](std::size_t index) { return clients[index] && clients[index]->connected(); };
        const auto path_ready = [&](std::size_t path) {
            return (!has_side(0) || connected(path * 2)) && (!has_side(1) || connected(path * 2 + 1));
        };
        const auto update_queues = [&] {
            if (!shared) return;
            const bool ready = path_ready(0);
            if (ready != queues_active) {
                for (const auto& tun : tuns) if (tun) tun->set_queue(ready);
                queues_active = ready;
            }
        };
        const auto disconnect = [&](std::size_t index) {
            stats.disconnected += clients[index]->discard_staged().drops;
            clients[index]->disconnect();
            next_connect[index] = Clock::now() + std::chrono::seconds(1);
            update_queues();
        };
        const auto account = [&](std::size_t index, const SwitchClient::Outcome& result) {
            stats.switch_tx += result.frames;
            stats.backpressure += result.backpressure;
            stats.send_errors += result.drops - result.backpressure;
            if (result.error) disconnect(index);
        };
        const auto send = [&](std::size_t path, unsigned side, const via::AdapterContext& env, const std::uint8_t* payload, std::size_t size) {
            Stack labels;
            if (!codec.attach(env, labels)) { ++stats.invalid; return; }
            const auto index = path * 2 + side;
            if (!connected(index)) { ++stats.disconnected; return; }
            account(index, clients[index]->append_frame(SwitchOpcode::switch_packet,
                labels.values.data(), labels.size, payload, size));
        };
        ControlDispatcher control_dispatcher;
        control_dispatcher.stats = [&] {
                std::ostringstream out;
                std::size_t ready_paths = 0, pairs = 0;
                for (std::size_t path = 0; path < paths.size(); ++path) {
                    ready_paths += path_ready(path);
                    pairs += connected(path * 2) && connected(path * 2 + 1);
                }
                out << "format=txt\nformat_version=1\ncomponent=divert-adapter\n"
                    << "divert_in_connected=" << connected(0) << "\ndivert_out_connected=" << connected(1)
                    << "\nrelay_paths=" << relay_paths.size() << "\nconnected_pairs=" << pairs
                    << "\nconnected_paths=" << ready_paths << "\nworker_side=" << worker_side
                    << "\nvia_instance=" << instance << "\nadmission_mode=" << admission_mode
                    << "\nadmission_active=" << (shared ? shared->active() : active) << "\nadmission_seconds=" << seconds(Clock::now()) << '\n';
                stats.write(out);
                out << "tun_queues_active=" << queues_active << '\n';
                if (shared) shared->stats(out);
                else { out << "shared_flows=0\n"; admission.stats(out); routes.stats(out); }
                if (relay_paths.empty()) {
                    clients[0]->write_stats(out, "divert_in_ipc_"); clients[1]->write_stats(out, "divert_out_ipc_");
                } else for (std::size_t path = 0; path < paths.size(); ++path) {
                    const auto prefix = "relay_path_" + paths[path].id + "_";
                    out << prefix << "in_connected=" << connected(path * 2) << '\n'
                        << prefix << "out_connected=" << connected(path * 2 + 1) << '\n';
                    if (clients[path * 2]) clients[path * 2]->write_stats(out, prefix + "in_ipc_");
                    if (clients[path * 2 + 1]) clients[path * 2 + 1]->write_stats(out, prefix + "out_ipc_");
                }
                control_auth_config.write_stats(out);
                recovery.write_stats(out);
                return out.str();
        };
        control_dispatcher.flows = [&] {
                const auto now = Clock::now();
                FlowDump dump(shared ? "shared_shards" : "retained");
                if (shared) shared->dump_flows(dump, now);
                else {
                    routes.dump_flows(dump, now);
                    admission.dump_flows(dump, seconds(now));
                }
                return dump.finish();
        };
        tuntom::control_auth::validate(control_auth_config);
        routed_control.configure_auth(control_auth_config);
        routed_control.configure(control_auth_config.allow_all,control_dispatcher);
        if (control) control->set_routed(routed_control.local_submit());
        const auto refresh_control_edges = [&] {
            for (std::size_t i=0;i<clients.size();++i) {
                const auto cookie = connected(i) ? control_socket_generation(clients[i]->fd()) : 0;
                routed_control.router().edge("ipc:"+std::to_string(i),cookie,false,control_route::max_frame,[&,i,cookie](const auto& bytes) {
                    return connected(i) && control_socket_generation(clients[i]->fd())==cookie &&
                        clients[i]->send(bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size());
                });
            }
        };
        const auto control_step = [&] {
            if (control) control->handle_dispatch(control_dispatcher);
        };
        const auto handle = [&](std::size_t source) {
            const unsigned side = static_cast<unsigned>(source % 2);
            if (!has_side(side)) return false;
            std::size_t path = source / 4;
            const auto index = path * 2 + side;
            auto& tun = *tuns[side];
            const bool from_switch = source % 4 < 2;
            if (!from_switch && !queues_active) return false;
            if (from_switch && !clients[index]->connected()) return false;
            const auto n = from_switch ? clients[index]->receive(buffer.data(), buffer.size()) : tun.read_packet(buffer.data(), buffer.size());
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return false;
            if (n <= 0 || static_cast<std::size_t>(n) > buffer.size()) {
                if (from_switch) disconnect(index); else ++stats.tun_errors;
                return true;
            }
            const auto now = Clock::now();
            if (from_switch && control_route::marked(buffer.data(),static_cast<std::size_t>(n))) {
                refresh_control_edges();
                routed_control.router().receive("ipc:"+std::to_string(index),buffer.data(),static_cast<std::size_t>(n),now);
                return true;
            }
            PacketInfo packet;
            via::AdapterContext env;
            if (from_switch) {
                ++stats.switch_rx;
                SwitchFrameView frame;
                if (!decode_switch_frame(buffer.data(), static_cast<std::size_t>(n), frame) ||
                    frame.opcode != SwitchOpcode::exit_packet || !codec.receive(labels_of(frame), side, env)) { ++stats.invalid; return true; }
                if (!packet_info(frame.payload, frame.payload_size, packet)) { ++stats.unsupported; return true; }
                if (!path_ready(path)) { ++stats.disconnected; return true; }
                if (side == 0) {
                    if (!active) { activated = now; active = true; }
                    const auto decision = shared ? shared->classify(packet, now) :
                        admission_mode == "immediate" ? AdmissionResult::proxy : admission.classify(packet, seconds(now));
                    if (decision == AdmissionResult::full) { ++stats.capacity; return true; }
                    if (decision == AdmissionResult::bypass) {
                        codec.bypass(env);
                        send(path, side, env, frame.payload, frame.payload_size);
                        ++stats.bypass; return true;
                    }
                }
                // Shared learn publishes and unlocks before the packet can cause a TUN reply.
                const auto learned = shared ? shared->learn(packet.flow, env, side == 0, now) : routes.learn(packet.flow, env, side == 0, now, path);
                if (learned != Learn::ok) {
                    if (learned == Learn::full) ++stats.capacity;
                    else if (learned == Learn::conflict) ++stats.conflict;
                    else ++stats.miss;
                    return true;
                }
                if (tun.write_packet(frame.payload, frame.payload_size) == static_cast<ssize_t>(frame.payload_size)) ++stats.tun_tx;
                else ++stats.tun_errors;
            } else {
                ++stats.tun_rx;
                if (!packet_info(buffer.data(), static_cast<std::size_t>(n), packet)) { ++stats.unsupported; return true; }
                const bool found = shared ? shared->lookup(packet.flow, side == 0, now, env) : routes.lookup(packet.flow, side == 0, now, env, &path);
                if (!found) { ++stats.miss; return true; }
                // A shared worker returns through its own side's channel, irrespective of the ingress worker.
                codec.onward(env, side);
                send(path, side, env, buffer.data(), static_cast<std::size_t>(n));
            }
            return true;
        };
        logger.start();
        log_info("tuntom-divert-adapter ready; admission starts with the first offered packet");
        std::size_t cursor = 0;
        while (!stopping) {
            try {
                if (recovery.wait_for_retry(control ? control->poll_fd() : -1)) { control_step(); continue; }
                const auto now = Clock::now();
                if (shared) shared->maintain(now);
                else { routes.maintain(now); if (active) admission.maintain(seconds(now)); }
                for (std::size_t index = 0; index < clients.size(); ++index) {
                    if (!clients[index]) { fds[index] = {-1, 0, 0}; continue; }
                    auto& client = *clients[index];
                    if (!client.connected() && !client.connecting() && now >= next_connect[index]) {
                        client.start_connect(now);
                        if (client.connected()) ++stats.reconnects;
                        else if (!client.connecting()) next_connect[index] = now + std::chrono::seconds(1);
                    }
                    fds[index] = {client.fd(), client.poll_events(), 0};
                }
                update_queues();
                for (unsigned side = 0; side < 2; ++side)
                    fds[clients.size() + side] = {queues_active && tuns[side] ? tuns[side]->fd() : -1, POLLIN, 0};
                fds.back() = {control ? control->poll_fd() : -1, POLLIN, 0};
                refresh_control_edges();
                routed_control.tick();
                if (routed_control.active()) control_step();
                int timeout = routed_control.active() ? 10 : 1000;
                for (const auto& client : clients) if (client) timeout = client->poll_timeout_ms(now, timeout);
                if (control) timeout = control->poll_timeout_ms(now, timeout);
                const auto ready = ::poll(fds.data(), fds.size(), timeout);
                if (ready < 0) { if (errno != EINTR) recovery.poll_failed(errno); continue; }
                for (std::size_t index = 0; index < clients.size(); ++index) {
                    if (!clients[index]) continue;
                    auto& client = *clients[index];
                    if (client.connecting()) {
                        client.advance_connect(Clock::now(), fds[index].revents);
                        if (client.connected()) ++stats.reconnects;
                        else if (!client.connecting()) next_connect[index] = Clock::now() + std::chrono::seconds(1);
                    } else if (fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) disconnect(index);
                }
                update_queues();
                if (fds.back().revents & POLLIN) control_step();
                const auto slice = Clock::now();
                for (unsigned round = 0; round < 64; ++round) {
                    bool progress = false;
                    for (std::size_t offset = 0; offset < sources; ++offset) progress |= handle((cursor + offset) % sources);
                    cursor = (cursor + 1) % sources;
                    if (!progress || Clock::now() - slice >= std::chrono::milliseconds(1)) break;
                }
                for (std::size_t index = 0; index < clients.size(); ++index)
                    if (connected(index)) account(index, clients[index]->flush());
            } catch (const std::bad_alloc&) {
                for (const auto& client : clients) if (client) stats.send_errors += client->discard_staged().drops;
                recovery.allocation_failed();
            }
        }
        return 0;
    } catch (const std::exception& error) { log_fatal(error.what()); return 1; }
}
