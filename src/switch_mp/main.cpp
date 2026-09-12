#include "../common.hpp"
#include "../control_socket.hpp"
#include "../runtime_recovery.hpp"
#include "../throughput_stats.hpp"
#include "config.hpp"
#include "runtime.hpp"
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
};
struct AdmissionStats {
    std::uint64_t accepted = 0, registered = 0, invalid = 0, timed_out = 0, rejected = 0;
};
} // namespace

int main(int argc, char **argv) {
    (void)::prctl(PR_SET_NAME, "tomtom-switch", 0UL, 0UL, 0UL);
    tuntom::logger.ignore_sigpipe();
    try {
        const auto config = parse_config(argc, argv);
        if (config.help) {
            usage(std::cout, argv[0]);
            return 0;
        }
        const auto hardware = detect_hardware();
        const auto budget =
            config.workers ? std::min(config.workers, hardware.limit()) : hardware.limit();
        Listener listener(config.socket);
        std::unique_ptr<tuntom::ControlSocket> control;
        if (!config.control.empty())
            control = std::make_unique<tuntom::ControlSocket>(config.control);
        Engine engine(config, budget); // Allocate eventfds before calculating FD capacity.
        const auto capacity = config.capacity.for_process();
        std::vector<Pending> pending;
        std::vector<pollfd> descriptors;
        pending.reserve(capacity.pending);
        descriptors.reserve(capacity.ports + capacity.pending + 3);
        std::array<std::uint8_t,
                   tuntom::switch_registration_header_size + tuntom::switch_max_port_id_size>
            registration;
        const auto started_at = Clock::now();
        tuntom::SwitchAdmission admission(started_at);
        tuntom::RuntimeRecovery recovery;
        tuntom::ThroughputStats throughput({"switch_rx", "switch_tx"});
        throughput.update(started_at, {{0, 0}, {0, 0}});
        AdmissionStats stats;
        std::uint64_t generation = 0;

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
        const auto handle_control = [&] {
            if (!control)
                return;
            control->handle([&] {
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
                    << "\nconnections_fd_reserve=" << tuntom::SwitchCapacity::fd_reserve
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
                engine.write_stats(out);
                recovery.write_stats(out);
                admission.write_stats(out, now);
                control->write_stats(out);
                tuntom::logger.write_stats(out);
                throughput.write(out);
                return out.str();
            });
        };

        while (!stop_requested) {
            try {
                const auto now = Clock::now();
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                                             [&](const auto &entry) {
                                                 if (entry.fd.get() < 0)
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
                descriptors.clear();
                descriptors.push_back(
                    {pending.size() < capacity.pending && admission.ready(now) ? listener.fd() : -1,
                     POLLIN, 0});
                descriptors.push_back({control ? control->poll_fd() : -1, POLLIN, 0});
                descriptors.push_back({engine.event_fd(), POLLIN, 0});
                for (const auto &entry : pending)
                    descriptors.push_back({entry.fd.get(), POLLIN, 0});
                // HUP monitoring also retires a closed ingress whose pool is full.
                for (const auto &port : engine.plan().ports)
                    descriptors.push_back({port->fd.get(), 0, 0});
                auto timeout = admission.poll_timeout_ms(now, 100);
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
                        entry.fd = Fd();
                        ++stats.timed_out;
                        continue;
                    }
                    const auto received = ::recv(entry.fd.get(), registration.data(),
                                                 registration.size(), MSG_DONTWAIT | MSG_TRUNC);
                    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        continue;
                    if (received <= 0) {
                        entry.fd = Fd();
                        continue;
                    }
                    try {
                        std::string id;
                        if (static_cast<std::size_t>(received) > registration.size() ||
                            !tuntom::decode_switch_registration(
                                registration.data(), static_cast<std::size_t>(received), id)) {
                            ++stats.invalid;
                            entry.fd = Fd();
                            continue;
                        }
                        auto ports = engine.plan().ports;
                        auto old = std::find_if(ports.begin(), ports.end(),
                                                [&](const auto &port) { return port->name == id; });
                        if (old == ports.end() && ports.size() >= capacity.ports) {
                            ++stats.rejected;
                            entry.fd = Fd();
                            continue;
                        }
                        auto port = std::make_shared<Port>(std::move(entry.fd), id, ++generation,
                                                           config.kind(id), config.pool_size);
                        if (old == ports.end())
                            ports.push_back(std::move(port));
                        else
                            *old = std::move(port);
                        engine.replace(std::move(ports));
                        ++stats.registered;
                    } catch (...) {
                        // The registration record is consumed; force a reconnect
                        // on allocation failure, preserving any old live port.
                        entry.fd = Fd();
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
                                 accepted_at + tuntom::SwitchAdmission::registration_timeout});
                        }
                    }
                }
                handle_control();
                update_throughput();
            } catch (const std::bad_alloc &) {
                recovery.allocation_failed();
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
