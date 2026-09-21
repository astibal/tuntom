#include "../routed_control.hpp"
#include "../control_ports.hpp"
#include "../common.hpp"
#include "../control_socket.hpp"
#include "../runtime_recovery.hpp"
#include "../throughput_stats.hpp"
#include "config.hpp"
#include "runtime.hpp"
#include "../ipc/switch_handshake.hpp"
#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <vector>

namespace {
using namespace tuntom::mp;
volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

class Listener {
    std::string path_;
    Fd fd_;

  public:
    explicit Listener(const std::string &path) : path_(path) {
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("Invalid Unix socket path");
        fd_ = Fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (fd_.get() < 0)
            throw std::runtime_error("Cannot create switch socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd_.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            throw std::runtime_error("bind(" + path + "): " + std::strerror(errno));
        if (::chmod(path.c_str(), 0660) < 0 || ::listen(fd_.get(), 64) < 0) {
            ::unlink(path.c_str());
            throw std::runtime_error("Cannot prepare switch socket");
        }
    }
    ~Listener() { ::unlink(path_.c_str()); }
    int fd() const { return fd_.get(); }
};

struct Pending {
    Fd fd;
    Clock::time_point deadline;
    tuntom::ipc::ServerHandshake handshake;
    std::shared_ptr<Port> candidate;
    int socket() const { return candidate ? candidate->fd.get() : fd.get(); }
    void discard() { fd = Fd(); candidate.reset(); handshake = tuntom::ipc::ServerHandshake(); }
    std::uint64_t mapping_bytes() const {
        const auto *transport = candidate ? candidate->transport.get() : handshake.transport.get();
        return transport ? transport->mapping_bytes() : 0;
    }
};
struct AdmissionStats {
    std::uint64_t accepted = 0, registered = 0, invalid = 0, timed_out = 0, rejected = 0;
};
} // namespace

int main(int argc, char **argv) {
    (void)::prctl(PR_SET_NAME, "tomtom-switch", 0UL, 0UL, 0UL);
    tuntom::logger.ignore_sigpipe();
    try {
        auto config = parse_config(argc, argv);
        if (config.help) {
            usage(std::cout, argv[0]);
            return 0;
        }
        if (!config.ruleset && config.routes.empty() && config.exits.empty() && config.trunks.empty() && !config.default_back)
            config.ruleset = tuntom::parse_switch_ruleset("format 1\nserial 0\n");
        const auto hardware = detect_hardware();
        const auto budget =
            config.workers ? std::min(config.workers, hardware.limit()) : hardware.limit();
        Listener listener(config.socket);
        std::unique_ptr<tuntom::ControlSocket> control;
        if (!config.control.empty())
            control = std::make_unique<tuntom::ControlSocket>(config.control);
        Engine engine(config, budget); // Count current eventfds/epolls in FD capacity.
        const auto capacity = config.capacity.for_process(budget, config.ipc.mode == tuntom::ipc::Mode::automatic ? 3 : 1); // Reserve the next plan's epolls.
        std::vector<Pending> pending;
        std::vector<pollfd> descriptors;
        pending.reserve(capacity.pending);
        descriptors.reserve(capacity.ports + capacity.pending + 3);
        const auto started_at = Clock::now();
        tuntom::SwitchAdmission admission(started_at);
        tuntom::RuntimeRecovery recovery;
        tuntom::ThroughputStats throughput({"switch_rx", "switch_tx"});
        throughput.update(started_at, {{0, 0}, {0, 0}});
        AdmissionStats stats;
        std::uint64_t generation = 0, epoch = 0;
        const auto mapping_bytes = [&] {
            std::uint64_t bytes = 0;
            for (const auto &port : engine.plan().ports)
                if (port->transport) bytes += port->transport->mapping_bytes();
            for (const auto &entry : pending) bytes += entry.mapping_bytes();
            return bytes;
        };

        struct sigaction action{};
        action.sa_handler = request_stop;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);
        engine.start();
        tuntom::logger.start();
        tuntom::log_info("tomtom-switch-mp ready: ", config.socket, ", data workers=", budget);

        const auto update_throughput = [&] {
            throughput.update(Clock::now(), {{engine.frames_rx(), engine.bytes_rx()},
                                             {engine.frames_tx(), engine.bytes_tx()}});
        };
        tuntom::RoutedControl routed_control("switch");
        tuntom::ControlDispatcher control_dispatcher;
        control_dispatcher.stats = [&] {
                update_throughput();
                const auto now = Clock::now();
                std::ostringstream out;
                out.exceptions(std::ios::badbit);
                out << "format=txt\nformat_version=1\ncomponent=switch\nimplementation=tomtom-"
                       "switch-mp\n"
                    << "pid=" << ::getpid() << "\nuptime_seconds="
                    << std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count()
                    << "\nconnections_current=" << engine.plan().ports.size()
                    << "\nconnections_pending=" << pending.size()
                    << "\nconnections_total=" << engine.plan().ports.size() + pending.size()
                    << "\nconnections_limit_ports_configured=" << config.capacity.ports
                    << "\nconnections_limit_pending_configured=" << config.capacity.pending
                    << "\nconnections_limit_ports=" << capacity.ports
                    << "\nconnections_limit_pending=" << capacity.pending
                    << "\nconnections_fd_reserve=" << tuntom::SwitchCapacity::fd_reserve + budget
                    << "\nconnections_accepted=" << stats.accepted
                    << "\nregistrations_ok=" << stats.registered
                    << "\nregistrations_invalid=" << stats.invalid
                    << "\nregistrations_timed_out=" << stats.timed_out
                    << "\nregistrations_capacity_rejected=" << stats.rejected
                    << "\nhardware_logical_cpus=" << hardware.logical
                    << "\nhardware_physical_cores=" << hardware.physical
                    << "\nhardware_quota_cpus=" << hardware.quota
                    << "\nhardware_worker_limit=" << hardware.limit()
                    << "\nworkers_requested=" << config.workers
                    << "\nwork_per_thread=" << config.policy.work_per_thread
                    << "\nrx_weight=" << config.policy.rx_weight
                    << "\ntx_weight=" << config.policy.tx_weight
                    << "\nadapter_weight=" << config.policy.adapter_weight
                    << "\ntrunk_weight=" << config.policy.trunk_weight
                    << "\npool_size=" << config.pool_size << "\nqueue_size=" << config.queue_size
                    << '\n';
                out << "ipc_version_max=2\nipc_memory_budget=" << config.mmap_budget
                    << "\nipc_mapping_bytes=" << mapping_bytes() << '\n';
                engine.write_stats(out);
                if (config.ruleset) out << "ruleset_format=" << config.ruleset->format << "\nruleset_serial=" << config.ruleset->serial << '\n';
                recovery.write_stats(out);
                admission.write_stats(out, now);
                if (control) control->write_stats(out);
                tuntom::logger.write_stats(out);
                throughput.write(out);
                return out.str();
            };
        control_dispatcher.rules = [&](const std::string &operation, const std::string &body) {
                if (operation.compare(0, 7, "divert.") == 0) {
                    if (!config.divert_config) throw std::runtime_error("divert is not configured");
                    if (operation == "divert.enable" && !config.divert_config->via) {
                        for (const auto& name : {config.divert_config->input, config.divert_config->output}) {
                            const auto found = engine.plan().divert_ports.find(name);
                            if (found == engine.plan().divert_ports.end() || found->second->disconnected.load())
                                throw std::runtime_error("both divert adapter ports must be connected");
                        }
                    }
                    return config.divert_config->command(operation);
                }
                if (operation == "show") {
                    if (!config.ruleset) throw std::runtime_error("legacy CLI routes are active; load a versioned ruleset to enable export");
                    return config.ruleset->text();
                }
                auto next = tuntom::parse_switch_ruleset(body);
                const bool changed = tuntom::ruleset_changed(config.ruleset, *next);
                auto response = std::string(operation == "check" ? "checked" : changed ? "applied" : "unchanged") +
                    " serial=" + std::to_string(next->serial) + "\n";
                if (changed || operation == "check") {
                    auto plan = engine.prepare_rules(next);
                    if (operation == "load") {
                        engine.publish(std::move(plan), true);
                        config.ruleset.swap(next);
                        config.routes.clear(); config.exits.clear(); config.trunks.clear(); config.default_back = false;
                    }
                }
                return response;
            };
        control_dispatcher.flows = [] { return tuntom::FlowDump("none").finish(); };
        control_dispatcher.ports = [&] {
                tuntom::ControlPorts ports;
                for (const auto& port : engine.plan().ports) {
                    if (port->disconnected.load(std::memory_order_relaxed)) continue;
                    ports.direct(port->name);
                    if (port->relay_registry.live())
                        for (const auto& channel : port->relay_registry.directory.channels)
                            ports.relayed(channel.second.name, port->name);
                }
                return ports.finish();
            };
        routed_control.configure(config.allow_control_all,control_dispatcher);
        const auto refresh_control_edges = [&] {
            std::set<std::string> names;
            for (const auto& port:engine.plan().ports) if(!port->disconnected.load()) {
                names.insert(port->name);
                routed_control.router().edge(port->name,port->generation,false,tuntom::control_route::max_frame,
                    [&,name=port->name,generation=port->generation](const auto& bytes) { return engine.control_send(name,generation,bytes); });
            }
            routed_control.router().retain(names);
        };
        engine.control_receive = [&](const auto& name,const auto* data,std::size_t size) {
            refresh_control_edges();
            routed_control.router().receive(name,data,size,tuntom::ControlRouter::Clock::now());
        };
        if (control) control->set_routed([&](tuntom::ControlRequest request,unsigned retries,unsigned wait) {
            refresh_control_edges();
            return routed_control.local_submit()(std::move(request),retries,wait);
        });
        const auto handle_control = [&] {
            if (control) control->handle_dispatch(control_dispatcher);
        };

        while (!stop_requested) {
            try {
                const auto now = Clock::now();
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                                             [&](const auto &entry) {
                                                 if (entry.socket() < 0)
                                                     return true;
                                                 if (now < entry.deadline)
                                                     return false;
                                                 ++stats.timed_out;
                                                 return true;
                                             }),
                              pending.end());
                if (recovery.wait_for_retry(control ? control->poll_fd() : -1)) {
                    handle_control();
                    continue;
                }
                if (std::any_of(engine.plan().ports.begin(), engine.plan().ports.end(),
                                [](const auto &port) { return port->disconnected.load(); })) {
                    auto ports = engine.plan().ports;
                    ports.erase(
                        std::remove_if(ports.begin(), ports.end(),
                                       [](const auto &port) { return port->disconnected.load(); }),
                        ports.end());
                    engine.replace(std::move(ports));
                }
                engine.drain_events();
                engine.relay_maintenance();
                descriptors.clear();
                descriptors.push_back(
                    {pending.size() < capacity.pending && admission.ready(now) ? listener.fd() : -1,
                     POLLIN, 0});
                descriptors.push_back({control ? control->poll_fd() : -1, POLLIN, 0});
                descriptors.push_back({engine.event_fd(), POLLIN, 0});
                for (const auto &entry : pending)
                    descriptors.push_back({entry.socket(), entry.handshake.events(), 0});
                // HUP monitoring also retires a closed ingress whose pool is full.
                for (const auto &port : engine.plan().ports)
                    descriptors.push_back({port->fd.get(), static_cast<short>(port->relay_ack_retry.writable(now) ? POLLOUT : 0), 0});
                if (routed_control.active()) refresh_control_edges();
                routed_control.tick();
                auto timeout = admission.poll_timeout_ms(now, routed_control.active() ? 10 : 100);
                for (const auto& port : engine.plan().ports) timeout = port->relay_ack_retry.timeout(now,timeout);
                if (control)
                    timeout = control->poll_timeout_ms(now, timeout);
                const auto ready = ::poll(descriptors.data(), descriptors.size(), timeout);
                if (ready < 0) {
                    if (errno != EINTR)
                        recovery.poll_failed(errno);
                    continue;
                }
                for (std::size_t i = 0; i < engine.plan().ports.size(); ++i)
                    if (descriptors[3 + pending.size() + i].revents &
                        (POLLHUP | POLLERR | POLLNVAL))
                        engine.plan().ports[i]->disconnected.store(true);

                for (std::size_t i = 0; i < pending.size(); ++i) {
                    auto &entry = pending[i];
                    if (!descriptors[3 + i].revents)
                        continue;
                    if (Clock::now() >= entry.deadline) {
                        entry.discard(); ++stats.timed_out; continue;
                    }
                    try {
                        auto &handshake = entry.handshake;
                        if (!handshake.activation_ready()) handshake.step(entry.socket(), config.ipc, epoch);
                        if (handshake.failed()) { ++stats.invalid; entry.discard(); continue; }
                        const auto via_allowed = [&] {
                            if (!tuntom::via::enabled(config.ruleset)) return true;
                            if (!tuntom::via::accepted(*config.ruleset, handshake.id)) return false;
                            for (const auto& port : engine.plan().ports)
                                if (!port->disconnected.load() && !tuntom::via::compatible(*config.ruleset, handshake.id, entry.socket(), port->name, port->fd.get())) return false;
                            return true;
                        };
                        const auto admission_allowed = [&] {
                            const auto &ports = engine.plan().ports;
                            return ports.size() < capacity.ports || std::any_of(ports.begin(), ports.end(),
                                [&](const auto &port) { return port->name == handshake.id; });
                        };
                        if (handshake.identified()) {
                            if (!via_allowed() || !admission_allowed()) { ++stats.rejected; entry.discard(); continue; }
                            const auto used = mapping_bytes();
                            handshake.prepare(config.mmap_budget - std::min(used, config.mmap_budget));
                        }
                        if (!handshake.activation_ready()) continue;
                        // Recheck capacity: another pending registration may have
                        // activated while this peer was mapping its pools.
                        if (!via_allowed() || !admission_allowed()) { ++stats.rejected; entry.discard(); continue; }
                        if (!entry.candidate) {
                            entry.candidate = std::make_shared<Port>(std::move(entry.fd), handshake.id, ++generation,
                                config.kind(handshake.id), config.pool_size, std::move(handshake.transport));
                        }
                        auto ports = engine.plan().ports;
                        auto old = std::find_if(ports.begin(), ports.end(),
                            [&](const auto &port) { return port->name == handshake.id; });
                        if (old == ports.end()) ports.push_back(entry.candidate);
                        else *old = entry.candidate;
                        // Prepare against the CURRENT plan on every attempt. An
                        // EAGAIN cannot leave a stale plan that revives retired ports.
                        auto next = engine.prepare(std::move(ports));
                        if (Clock::now() >= entry.deadline) {
                            ++stats.timed_out; entry.discard(); continue;
                        }
                        if (!handshake.legacy) {
                            const auto active = tuntom::ipc::state(tuntom::ipc::Type::active, handshake.parameters());
                            const auto sent = tuntom::ipc::send_record(entry.socket(), active);
                            if (sent < 0 && tuntom::ipc::retry_error()) continue;
                            if (sent != static_cast<ssize_t>(active.size)) { entry.discard(); continue; }
                        }
                        entry.candidate->transport->close_pool_fds();
                        engine.publish(std::move(next)); // Allocation-free activation after ACTIVE.
                        entry.candidate.reset();
                        ++stats.registered;
                    } catch (...) {
                        // No unsuccessful handshake can replace the old named port.
                        entry.discard();
                        throw;
                    }
                }

                // Accept after processing poll indices, since this grows pending.
                if (descriptors[0].revents & POLLIN) {
                    const auto accepted_at = Clock::now();
                    if (admission.ready(accepted_at) && pending.size() < capacity.pending) {
                        admission.attempted(accepted_at);
                        const int fd = ::accept4(listener.fd(), nullptr, nullptr,
                                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (fd < 0)
                            admission.failed(errno, accepted_at);
                        else {
                            ++stats.accepted;
                            pending.push_back(
                                {Fd(fd),
                                 accepted_at + tuntom::SwitchAdmission::registration_timeout, {}, {}});
                        }
                    }
                }
                handle_control();
                update_throughput();
            } catch (const std::bad_alloc &) {
                recovery.allocation_failed();
            } catch (const PollSetupError &error) {
                // The unpublished epoll sets are RAII-owned; the old plan is intact.
                recovery.poll_failed(error.error);
            } catch (const RulesPlanError &) {
                // A new registration can exceed the compiled-plan bound even
                // when the active ruleset fitted the previous topology.
                ++stats.rejected;
            }
        }
        engine.stop();
        tuntom::log_info("tomtom-switch-mp stopped: RX=", engine.frames_rx(),
                         ", TX=", engine.frames_tx());
        return 0;
    } catch (const std::exception &error) {
        tuntom::log_fatal(error.what());
        return 1;
    }
}
