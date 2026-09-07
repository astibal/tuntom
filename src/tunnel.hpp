#pragma once

#include "privileges.hpp"
#include "tun_device.hpp"
#include "udp_endpoint.hpp"
#include "switch_client.hpp"
#include "ipc/switch_protocol.hpp"
#include "session.hpp"
#include "ip.hpp"
#include "fragmentation.hpp"
#include "processing_stats.hpp"
#include "throughput_stats.hpp"
#include "adaptive_polling.hpp"
#include "stats_control.hpp"
#include "control_socket.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <poll.h>
#include <netdb.h>

namespace tuntom {

class Tunnel {
public:
    Tunnel(
        std::uint16_t tunnel_id,
        bool server_mode,
        const std::string& interface_name,
        const std::string& remote_host,
        const Options& options)
        : tunnel_id_(tunnel_id),
          server_mode_(server_mode),
          options_(options),
          master_key_(parse_master_key()),
          protocol_v5_(tunnel_id, master_key_, server_mode, options.tun_mtu, options.encrypt_ascon, options.init_window, options.pfs) {

        options_.encrypt_ascon = options_.encrypt_ascon or options_.pfs;

        const std::uint16_t port =
            static_cast<std::uint16_t>(40000 + tunnel_id_);

        if (server_mode_) {
            udp_.open_server(port);
        } else {
            udp_.open_client(remote_host, port);
        }

        if (options_.switch_socket.empty() or options_.switch_exit_node) {
            tun_ = std::make_unique<TunDevice>(interface_name, options_.tun_mtu);
        }
        if (not options_.switch_socket.empty()) {
            switch_ = std::make_unique<SwitchClient>(
                options_.switch_socket, options_.switch_port_id);
        }
        if (not options_.control_socket.empty())
            control_ = std::make_unique<ControlSocket>(options_.control_socket);

        active_transport_mtu_ =
            options_.pmtud_auto
                ? min_transport_mtu
                : options_.transport_mtu;

        validate_fragment_capacity();
        reserve_hot_path_buffers();

        if (options_.switch_socket.empty()) drop_privileges();
        else harden_unprivileged_process();

        // The initial check must use the final runtime identity. Connecting as
        // root would hide permissions that make all later reconnects fail.
        try_switch_reconnect(std::chrono::steady_clock::now(), true);

        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "tuntom id=" << tunnel_id_
                << " tun-mtu=" << options_.tun_mtu
                << " transport-mtu-configured=" << options_.transport_mtu
                << " transport-mtu-active=" << active_transport_mtu_
                << " pfs=" << (options_.pfs ? "x25519-akdf" : "off")
                << " encryption=" << (options_.encrypt_ascon ? "ascon-aead128" : "off")
                << " pmtud=" << (options_.pmtud_auto ? "auto" : "off")
                << " max-fragment-payload="
                << maximum_fragment_payload()
                << " ttl-compensate="
                << (options_.ttl_compensate ? "yes" : "no")
                << " stats="
                << (stats_file_enabled() ? options_.stats_file : "off")
                << " switch="
                << (switch_ ? options_.switch_socket : "off")
                << " switch-exit="
                << (options_.switch_exit_node ? "on" : "off")
                << "\n";
        }
    }

    virtual ~Tunnel() = default;

    void run() {
        const auto started_now =
            std::chrono::steady_clock::now();

        throughput_.update(started_now, {
            stats_.tun_rx_bytes, stats_.tun_tx_bytes,
            stats_.udp_rx_bytes, stats_.udp_tx_bytes});

        if (not server_mode_) {
            send_handshake(protocol_v5_.begin(started_now));
            next_rtt_probe_ =
                started_now +
                std::chrono::seconds(rtt_probe_interval_seconds);
            rtt_probe_schedule_active_ = true;

        }

        auto last_keepalive = started_now;
        auto last_reassembly_cleanup =
            std::chrono::steady_clock::now();
        auto last_stats_write =
            std::chrono::steady_clock::now();

        write_stats();

        while (true) {
            update_stats_control();
            pollfd descriptors[4] {};
            descriptors[0].fd = tun_ ? tun_->fd() : -1;
            descriptors[0].events = POLLIN;
            descriptors[1].fd = udp_.fd();
            descriptors[1].events = POLLIN;
            descriptors[2].fd = switch_ and switch_->connected() ? switch_->fd() : -1;
            descriptors[2].events = POLLIN;
            descriptors[3].fd = control_ ? control_->fd() : -1;
            descriptors[3].events = POLLIN;

            const auto poll_started = AdaptivePolling::Clock::now();
            const int rc = ::poll(descriptors, 4, 1000);
            const auto poll_finished = AdaptivePolling::Clock::now();

            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    "poll() failed: " +
                    std::string(std::strerror(errno)));
            }

            adaptive_polling_.observe_poll(poll_finished - poll_started);

            // Control traffic is never held behind an overload data slice.
            if (control_ and (descriptors[3].revents & POLLIN)) {
                handle_control_request();
            }

            const bool initially_ready[3] {
                (descriptors[0].revents & POLLIN) != 0,
                (descriptors[1].revents & POLLIN) != 0,
                (descriptors[2].revents & POLLIN) != 0,
            };
            const unsigned rounds = adaptive_polling_.batch_size();
            const auto slice_started = AdaptivePolling::Clock::now();

            for (unsigned round = 0; round < rounds; ++round) {
                bool progress = false;
                for (unsigned offset = 0; offset < 3; ++offset) {
                    const unsigned source = (next_data_source_ + offset) % 3;
                    if (round == 0 and not initially_ready[source]) continue;
                    if (source == 0) progress |= try_handle_tun_packet();
                    else if (source == 1) progress |= try_handle_udp_packet();
                    else progress |= try_handle_switch_packet();
                }
                next_data_source_ = (next_data_source_ + 1) % 3;
                if (not progress) break;
                if (
                    AdaptivePolling::Clock::now() - slice_started >=
                    AdaptivePolling::processing_slice) {

                    adaptive_polling_.note_slice_limit();
                    break;
                }
            }

            if (switch_ and
                (descriptors[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                disconnect_switch(ECONNRESET);
            }

            if (adaptive_polling_.should_check_backlog()) {
                adaptive_polling_.observe_backlog(
                    data_backlog_ready(),
                    AdaptivePolling::Clock::now());
            }

            const auto now = std::chrono::steady_clock::now();
            try_switch_reconnect(now);
            throughput_.update(now, {
                stats_.tun_rx_bytes, stats_.tun_tx_bytes,
                stats_.udp_rx_bytes, stats_.udp_tx_bytes});
            send_handshake(protocol_v5_.tick(now));

            if (
                not server_mode_ and
                now - last_keepalive >=
                    std::chrono::seconds(keepalive_seconds)) {

                send_control(PacketType::keepalive);
                last_keepalive = now;
            }

            if (
                rtt_probe_schedule_active_ and
                now >= next_rtt_probe_) {

                send_rtt_probe();
                next_rtt_probe_ +=
                    std::chrono::seconds(
                        rtt_probe_interval_seconds);
            }

            cleanup_rtt_probes(now);

            if (options_.pmtud_auto) {
                handle_pmtud_timeout(now);
            }

            if (
                now - last_reassembly_cleanup >=
                std::chrono::seconds(1)) {

                protocol_v5_.cleanup();
                last_reassembly_cleanup = now;
            }

            if (
                now - last_stats_write >=
                    std::chrono::seconds(1)) {

                write_stats();
                last_stats_write = now;
            }
        }
    }

protected:
    virtual bool process(Packet&, Direction) {
        // Future packet processing hooks (e.g. smithproxy integration)
        // can override this.
        return true;
    }

private:
    void handle_control_request() {
        control_->handle([this] {
            std::ostringstream output;
            format_stats(output);
            return output.str();
        });
    }

    bool try_handle_tun_packet() {
        if (not tun_) return false;
        const ssize_t received = tun_->read_packet(
            tun_rx_buffer_.data(), tun_rx_buffer_.size());
        if (received <= 0) return false;

        tx_sample_active_ = tx_processing_.select();
        if (tx_sample_active_) tx_sample_start_ = ProcessingStats::Clock::now();
        const std::size_t packet_size = static_cast<std::size_t>(received);
        ++stats_.tun_rx_packets;
        stats_.tun_rx_bytes += packet_size;
        dump_bytes("TUN read", tun_rx_buffer_.data(), packet_size, 20);

        if (packet_size > options_.tun_mtu) {
            ++stats_.drops_mtu;
            log_info("DROP TUN packet larger than configured MTU");
        } else {
            send_data(tun_rx_buffer_.data(), packet_size);
        }
        return true;
    }

    bool try_handle_udp_packet() {
        sockaddr_storage source {};
        socklen_t source_length = 0;
        const ssize_t received = udp_.receive(
            udp_rx_buffer_.data(), udp_rx_buffer_.size(), source, source_length);
        if (received <= 0) return false;

        rx_sample_active_ = rx_processing_.select();
        if (rx_sample_active_) rx_sample_start_ = ProcessingStats::Clock::now();
        ++stats_.udp_rx_packets;
        stats_.udp_rx_bytes += static_cast<std::uint64_t>(received);
        handle_udp_packet(
            udp_rx_buffer_.data(), static_cast<std::size_t>(received),
            source, source_length);
        return true;
    }

    bool data_backlog_ready() const {
        pollfd descriptors[3] {
            {tun_ ? tun_->fd() : -1, POLLIN, 0},
            {udp_.fd(), POLLIN, 0},
            {switch_ and switch_->connected() ? switch_->fd() : -1, POLLIN, 0},
        };
        const int ready = ::poll(descriptors, 3, 0);
        if (ready <= 0) return false;
        for (const auto& descriptor : descriptors) {
            if ((descriptor.revents & POLLIN) != 0) return true;
        }
        return false;
    }

    void send_handshake(const std::vector<std::uint8_t>& wire,
                        const sockaddr_storage* source = nullptr,
                        socklen_t source_length = 0) {
        if (wire.empty()) return;
        const auto n = source ? udp_.send_to(wire.data(), wire.size(), *source, source_length)
                              : udp_.send(wire.data(), wire.size());
        if (n < 0) { ++stats_.udp_send_errors; return; }
        ++stats_.udp_tx_packets;
        stats_.udp_tx_bytes += static_cast<std::uint64_t>(n);
    }

    void session_activated() {
        log_info("V5 session confirmed");
        rtt_probes_.clear();
        send_rtt_probe();
        next_rtt_probe_ = std::chrono::steady_clock::now() +
            std::chrono::seconds(rtt_probe_interval_seconds);
        rtt_probe_schedule_active_ = true;
        if (options_.pmtud_auto) restart_pmtud("session confirmed");
    }

    void reserve_hot_path_buffers() {
        const std::size_t maximum_payload =
            maximum_fragment_payload();

        tun_rx_buffer_.resize(options_.tun_mtu);
        udp_rx_buffer_.resize(buffer_size);
        switch_rx_buffer_.resize(
            switch_base_header_size +
            switch_max_labels * switch_label_size +
            options_.tun_mtu);

        tx_logical_packet_.payload.reserve(options_.tun_mtu);
        tx_fragment_packet_.payload.reserve(maximum_payload);
        rx_packet_.payload.reserve(options_.tun_mtu);
        rx_logical_packet_.payload.reserve(options_.tun_mtu);

        tx_encoded_buffer_.reserve(
            protocol_fragment_v5_size + maximum_payload);
        tx_mac_buffer_.reserve(32 + maximum_payload);
        rx_mac_buffer_.reserve(32 + options_.tun_mtu);
        reassembled_packet_.reserve(options_.tun_mtu);
    }

    std::size_t maximum_fragment_payload() const {
        const std::size_t overhead =
            udp_.outer_ip_header_size() +
            udp_header_size +
            protocol_fragment_v5_size;

        if (active_transport_mtu_ <= overhead) {
            throw std::runtime_error(
                "Transport MTU is too small for tuntom V5");
        }

        return active_transport_mtu_ - overhead;
    }

    void validate_fragment_capacity() const {
        const std::size_t maximum_payload =
            maximum_fragment_payload();

        const std::size_t fragment_count =
            (options_.tun_mtu + maximum_payload - 1) /
            maximum_payload;

        if (fragment_count > max_fragments_per_packet) {
            throw std::runtime_error(
                "Configured MTU/transport-MTU combination may require "
                "more than 64 fragments");
        }
    }

    void send_data(
        const std::uint8_t* data,
        std::size_t size) {

        if (not protocol_v5_.ready()) return;
        Packet& logical_packet = tx_logical_packet_;
        logical_packet.type = PacketType::data;
        logical_packet.tunnel_id = tunnel_id_;
        logical_packet.protocol_version = protocol_version_v5;
        logical_packet.sequence = 0;
        logical_packet.message_id = 0;
        logical_packet.fragment_offset = 0;
        logical_packet.original_length =
            static_cast<std::uint32_t>(size);
        logical_packet.payload.assign(data, data + size);

        if (not process(logical_packet, Direction::tun_to_udp)) {
            ++stats_.drops_process;
            return;
        }

        if (logical_packet.payload.size() > options_.tun_mtu) {
            ++stats_.drops_mtu;
            log_info("DROP processed packet larger than configured MTU");
            return;
        }

        ++stats_.data_tx_packets;

        const FragmentPlan plan = make_v5_fragment_plan(
            logical_packet.payload.size(), maximum_fragment_payload());

        const std::uint64_t message_id =
            message_id_generator_.next();

        std::size_t offset = 0;

        for (std::size_t index = 0; index < plan.count; ++index) {
            const std::size_t fragment_size =
                plan.base_size +
                (index < plan.larger_fragments ? 1 : 0);

            Packet& fragment = tx_fragment_packet_;
            fragment.type = PacketType::data;
            fragment.tunnel_id = tunnel_id_;
            fragment.protocol_version = protocol_version_v5;
            fragment.message_id = message_id;
            fragment.fragment_offset =
                static_cast<std::uint32_t>(offset);
            fragment.original_length =
                static_cast<std::uint32_t>(
                    logical_packet.payload.size());

            fragment.payload.assign(
                logical_packet.payload.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                logical_packet.payload.begin() +
                    static_cast<std::ptrdiff_t>(
                        offset + fragment_size));

            if (not protocol_v5_.encode_into(
                fragment, tx_encoded_buffer_, tx_mac_buffer_)) return;

            const ssize_t sent =
                udp_.send(
                    tx_encoded_buffer_.data(),
                    tx_encoded_buffer_.size());

            if (sent < 0) {
                ++stats_.udp_send_errors;
                const int send_error = errno;

                if (log_enabled(LogLevel::info)) {
                    std::cerr
                        << "UDP send failed: "
                        << std::strerror(send_error)
                        << "\n";
                }

                if (
                    options_.pmtud_auto and
                    send_error == EMSGSIZE) {

                    restart_pmtud(
                        "data datagram exceeded path MTU");
                }

                return;
            }

            if (tx_sample_active_ and index + 1 == plan.count) {
                tx_processing_.finish(tx_sample_start_);
            }
            ++stats_.udp_tx_packets;
            stats_.udp_tx_bytes +=
                static_cast<std::uint64_t>(sent);
            ++stats_.fragments_tx;

            if (log_enabled(LogLevel::debug)) {
                std::cerr
                    << "FRAGMENT "
                    << (index + 1) << "/" << plan.count
                    << " msg=" << message_id
                    << " offset=" << offset
                    << " size=" << fragment_size
                    << "\n";
            }

            offset += fragment_size;
        }
    }

    void handle_udp_packet(
        const std::uint8_t* data,
        std::size_t size,
        const sockaddr_storage& source,
        socklen_t source_length) {

        dump_bytes("UDP recv", data, size, 40);

        Packet& packet = rx_packet_;
        packet.payload.clear();

        SessionProtocol::Session* receive_session = nullptr;
        bool session_update_peer = false;
        bool session_activated_now = false;

        {
            auto result = protocol_v5_.receive(data, size, packet, rx_mac_buffer_,
                                               std::chrono::steady_clock::now());
            if (result.timestamp_rejected) ++stats_.init_timestamp_rejected;
            if (result.nonce_capacity) ++stats_.init_nonce_capacity_rejected;
            if (result.clock_warning or result.nonce_capacity) {
                const auto now = std::chrono::steady_clock::now();
                if (init_warning_next_ == SessionProtocol::Time{} or now >= init_warning_next_) {
                    if (log_level != LogLevel::quiet) {
                        char host[NI_MAXHOST] {}, service[NI_MAXSERV] {};
                        ::getnameinfo(reinterpret_cast<const sockaddr*>(&source), source_length,
                            host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
                        std::cerr << "WARN INIT tunnel=" << packet.tunnel_id
                            << " peer=[" << host << "]:" << service
                            << " observed_clock_offset=" << result.clock_offset
                            << "s allowed=+/-" << options_.init_window / 2 << "s"
                            << " rejected=" << (result.timestamp_rejected or result.nonce_capacity)
                            << " nonce_capacity=" << result.nonce_capacity
                            << " suppressed=" << init_warning_suppressed_
                            << (result.nonce_capacity ? "; nonce history full; handshake may fail;"
                                : "; handshake may fail; check clock synchronization on both peers;")
                            << " delayed/replayed INITs can also cause clock warnings\n";
                    }
                    init_warning_suppressed_ = 0;
                    init_warning_next_ = now + std::chrono::seconds(30);
                } else ++init_warning_suppressed_;
            }
            // Replies to unconfirmed INIT go directly to its source, without
            // changing the active return path. Clients retain their configured peer.
            send_handshake(result.reply, server_mode_ ? &source : nullptr, source_length);
            session_update_peer = result.update_peer;
            session_activated_now = result.activated;
            if (session_activated_now and session_update_peer) udp_.set_peer(source, source_length);
            if (session_activated_now) session_activated();
            if (not result.data) {
                if (result.replay_drop) ++stats_.drops_replay;
                else if (not result.control) ++stats_.drops_protocol;
                return;
            }
            receive_session = result.session;
        }

        // Server learns/updates the NAT peer only after successful
        // authentication in an established session.
        if (server_mode_ and
            session_update_peer) {
            const bool peer_changed =
                udp_.set_peer(source, source_length);

            if (
                options_.pmtud_auto and
                peer_changed) {

                restart_pmtud("UDP peer changed");
            }
        }

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "ACCEPT v"
                << static_cast<unsigned>(packet.protocol_version)
                << " seq=" << packet.sequence
                << " type=" << static_cast<unsigned>(packet.type)
                << " msg=" << packet.message_id
                << " offset=" << packet.fragment_offset
                << " original=" << packet.original_length
                << " payload=" << packet.payload.size()
                << "\n";
        }

        if (packet.type == PacketType::ping) {
            if (
                server_mode_ and
                not rtt_probe_schedule_active_) {

                const auto now =
                    std::chrono::steady_clock::now();

                next_rtt_probe_ =
                    now +
                    std::chrono::milliseconds(
                        (rtt_probe_interval_seconds * 1000) / 2);

                rtt_probe_schedule_active_ = true;
            }

            send_probe_reply(packet.message_id);
            return;
        }

        if (packet.type == PacketType::pong) {
            handle_probe_reply(packet.message_id);
            return;
        }

        if (packet.type == PacketType::mtu_probe) {
            if (options_.pmtud_auto) {
                handle_mtu_probe(packet, size, source);
            }
            return;
        }

        if (packet.type == PacketType::mtu_reply) {
            if (options_.pmtud_auto) {
                handle_mtu_reply(packet);
            }
            return;
        }

        if (packet.type != PacketType::data) {
            return;
        }

        // Include decode time captured at receive, but exclude control traffic.
        struct RxMeasurement {
            Tunnel& tunnel;
            ~RxMeasurement() {
                if (tunnel.rx_sample_active_) {
                    tunnel.rx_processing_.finish(tunnel.rx_sample_start_);
                }
            }
        } measurement { *this };

        ++stats_.fragments_rx;

        reassembled_packet_.clear();

        if (
            not receive_session->reassembly.accept(
                packet,
                reassembled_packet_,
                &reassembly_span_)) {

            return;
        }

        Packet& logical_packet = rx_logical_packet_;
        logical_packet.type = PacketType::data;
        logical_packet.tunnel_id = tunnel_id_;
        logical_packet.protocol_version = protocol_version_v5;
        logical_packet.sequence = packet.sequence;
        logical_packet.message_id = packet.message_id;
        logical_packet.fragment_offset = 0;
        logical_packet.original_length =
            static_cast<std::uint32_t>(
                reassembled_packet_.size());

        logical_packet.payload.swap(reassembled_packet_);
        deliver_received_data(logical_packet);

        // Recycle the storage used by the completed logical packet.
        logical_packet.payload.swap(reassembled_packet_);
        reassembled_packet_.clear();
    }

    void deliver_received_data(Packet& packet) {
        if (not process(packet, Direction::udp_to_tun)) {
            ++stats_.drops_process;
            return;
        }

        if (packet.payload.size() > options_.tun_mtu) {
            ++stats_.drops_mtu;
            log_info("DROP reassembled packet larger than configured MTU");
            return;
        }

        if (switch_) {
            if (not switch_->connected()) {
                ++stats_.switch_drops;
                return;
            }
            const auto frame = encode_switch_frame(
                SwitchOpcode::switch_packet,
                {options_.switch_label},
                packet.payload.data(),
                packet.payload.size());
            const ssize_t written = switch_->send(frame.data(), frame.size());
            if (written != static_cast<ssize_t>(frame.size())) {
                const int error = written < 0 ? errno : EIO;
                ++stats_.switch_send_errors;
                ++stats_.switch_drops;
                disconnect_switch(error);
                return;
            }
            ++stats_.switch_tx_packets;
            stats_.switch_tx_bytes += static_cast<std::uint64_t>(written);
            ++stats_.data_rx_packets;
            return;
        }

        deliver_to_tun(packet);
    }

    void deliver_to_tun(Packet& packet) {
        if (not tun_) {
            ++stats_.switch_drops;
            return;
        }

        if (options_.ttl_compensate) {
            compensate_ip_hop(packet.payload);
        }

        dump_bytes(
            "TUN write",
            packet.payload.data(),
            packet.payload.size(),
            20);

        const ssize_t written =
            tun_->write_packet(
                packet.payload.data(),
                packet.payload.size());

        if (written < 0) {
            ++stats_.tun_write_errors;

            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "TUN write failed: "
                    << std::strerror(errno)
                    << "\n";
            }
        } else {
            ++stats_.tun_tx_packets;
            stats_.tun_tx_bytes +=
                static_cast<std::uint64_t>(written);
            ++stats_.data_rx_packets;
        }
    }

    bool try_handle_switch_packet() {
        if (not switch_ or not switch_->connected()) return false;
        const ssize_t received = switch_->receive(
            switch_rx_buffer_.data(), switch_rx_buffer_.size());
        if (received == 0) {
            disconnect_switch(ECONNRESET);
            return true;
        }
        if (received < 0) {
            if (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)
                return false;
            disconnect_switch(errno);
            return true;
        }
        if (static_cast<std::size_t>(received) > switch_rx_buffer_.size()) {
            ++stats_.switch_drops;
            return true;
        }

        ++stats_.switch_rx_packets;
        stats_.switch_rx_bytes += static_cast<std::uint64_t>(received);

        SwitchFrameView frame;
        if (not decode_switch_frame(
                switch_rx_buffer_.data(),
                static_cast<std::size_t>(received),
                frame)) {
            ++stats_.switch_drops;
            return true;
        }

        if (frame.payload_size > options_.tun_mtu) {
            ++stats_.drops_mtu;
            ++stats_.switch_drops;
            return true;
        }

        if (frame.opcode == SwitchOpcode::switch_packet) {
            send_data(frame.payload, frame.payload_size);
            return true;
        }

        if (not options_.switch_exit_node) {
            ++stats_.switch_drops;
            return true;
        }

        Packet packet;
        packet.type = PacketType::data;
        packet.tunnel_id = tunnel_id_;
        packet.protocol_version = protocol_version_v5;
        packet.original_length = static_cast<std::uint32_t>(frame.payload_size);
        packet.payload.assign(frame.payload, frame.payload + frame.payload_size);
        deliver_to_tun(packet);
        return true;
    }

    void record_switch_error(int error) {
        stats_.switch_last_error_ts = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        stats_.switch_last_error_no = error;
    }

    void disconnect_switch(int error) {
        if (not switch_ or not switch_->connected()) return;
        record_switch_error(error);
        switch_->disconnect();
        ++stats_.switch_disconnects;
        next_switch_reconnect_ =
            std::chrono::steady_clock::now() + switch_reconnect_interval_;
        log_info("Switch socket disconnected; DATA will be dropped until reconnect");
    }

    void try_switch_reconnect(
        std::chrono::steady_clock::time_point now,
        bool startup = false) {

        if (not switch_ or switch_->connected() or now < next_switch_reconnect_) return;
        ++stats_.switch_reconnect_attempts;
        if (switch_->connect_now()) {
            ++stats_.switch_reconnects;
            last_switch_connect_error_ = 0;
            log_info("Switch socket connected");
            return;
        }

        const int error = switch_->last_error();
        ++stats_.switch_socket_errors;
        record_switch_error(error);
        if (error == EACCES or error == EPERM) {
            ++stats_.switch_socket_eacces;
        } else if (error == ENOENT) {
            ++stats_.switch_socket_enoent;
        } else if (error == ECONNREFUSED) {
            ++stats_.switch_socket_econnrefused;
        } else {
            ++stats_.switch_socket_other_errors;
        }
        if (startup and (error == EACCES or error == EPERM)) {
            throw std::runtime_error(
                "Cannot access switch socket " + options_.switch_socket +
                " as runtime user: " + std::strerror(error));
        }
        if (error != last_switch_connect_error_) {
            log_info(
                "Switch reconnect failed: " +
                std::string(std::strerror(error)));
            last_switch_connect_error_ = error;
        }
        next_switch_reconnect_ = now + switch_reconnect_interval_;
    }

    bool send_control_packet(
        PacketType type,
        std::uint64_t message_id = 0) {

        Packet packet;
        packet.type = type;
        packet.tunnel_id = tunnel_id_;
        packet.protocol_version = protocol_version_v5;
        packet.message_id = message_id;

        if (not protocol_v5_.encode_into(
            packet, tx_encoded_buffer_, tx_mac_buffer_)) return false;

        const ssize_t sent =
            udp_.send(
                tx_encoded_buffer_.data(),
                tx_encoded_buffer_.size());

        if (sent < 0) {
            ++stats_.udp_send_errors;
            return false;
        }

        ++stats_.udp_tx_packets;
        stats_.udp_tx_bytes +=
            static_cast<std::uint64_t>(sent);

        return true;
    }

    void send_control(PacketType type) {
        send_control_packet(type);
    }

    void send_rtt_probe() {
        const std::uint64_t probe_id =
            message_id_generator_.next();

        const auto sent_at =
            std::chrono::steady_clock::now();

        if (send_control_packet(PacketType::ping, probe_id)) {
            rtt_probes_[probe_id] = ProbeState { sent_at };
        }
    }

    void send_probe_reply(std::uint64_t probe_id) {
        if (probe_id == 0) {
            return;
        }

        send_control_packet(PacketType::pong, probe_id);
    }

    void handle_probe_reply(std::uint64_t probe_id) {
        const auto it =
            rtt_probes_.find(probe_id);

        if (it == rtt_probes_.end()) {
            return;
        }

        const auto now =
            std::chrono::steady_clock::now();

        const double rtt_ms =
            std::chrono::duration<double, std::milli>(
                now - it->second.sent_at).count();

        rtt_probes_.erase(it);

        stats_.rtt_last_ms = rtt_ms;

        if (stats_.rtt_samples == 0) {
            stats_.rtt_min_ms = rtt_ms;
            stats_.rtt_max_ms = rtt_ms;
            stats_.rtt_avg_ms = rtt_ms;
            stats_.rtt_jitter_ms = 0.0;
        } else {
            stats_.rtt_min_ms =
                std::min(stats_.rtt_min_ms, rtt_ms);
            stats_.rtt_max_ms =
                std::max(stats_.rtt_max_ms, rtt_ms);

            const double previous_average =
                stats_.rtt_avg_ms;

            stats_.rtt_avg_ms +=
                (rtt_ms - stats_.rtt_avg_ms) /
                static_cast<double>(stats_.rtt_samples + 1);

            const double deviation =
                std::abs(rtt_ms - previous_average);

            stats_.rtt_jitter_ms +=
                (deviation - stats_.rtt_jitter_ms) / 16.0;
        }

        ++stats_.rtt_samples;
    }

    void cleanup_rtt_probes(
        const std::chrono::steady_clock::time_point& now) {

        for (auto it = rtt_probes_.begin();
             it != rtt_probes_.end();) {

            if (
                now - it->second.sent_at >=
                    std::chrono::seconds(
                        rtt_probe_timeout_seconds)) {

                ++stats_.rtt_lost;
                it = rtt_probes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t pmtud_upper_bound() const {
        return std::max(
            pmtud_upper_mtu,
            options_.transport_mtu);
    }

    std::size_t source_ip_header_size(
        const sockaddr_storage& source) const {

        if (source.ss_family == AF_INET) {
            return ipv4_header_min_size;
        }

        if (source.ss_family == AF_INET6) {
            const auto* address =
                reinterpret_cast<const sockaddr_in6*>(&source);

            return
                IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)
                    ? ipv4_header_min_size
                    : ipv6_header_size;
        }

        return udp_.outer_ip_header_size();
    }

    void start_pmtud() {
        if (pmtud_started_) {
            log_info("PMTUD start skipped: already started");
            return;
        }

        pmtud_started_ = true;
        pmtud_known_good_ = min_transport_mtu;
        pmtud_known_bad_ = 0;
        active_transport_mtu_ = pmtud_known_good_;

        const std::size_t first =
            std::clamp(
                options_.transport_mtu,
                min_transport_mtu + 1,
                pmtud_upper_bound());

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "PMTUD start: known-good="
                << pmtud_known_good_
                << " first-probe="
                << first
                << " upper-bound="
                << pmtud_upper_bound()
                << "\n";
        }

        send_mtu_probe(first);
    }

    void restart_pmtud(const char* reason) {
        pmtud_started_ = false;
        pmtud_probe_pending_ = false;
        pmtud_probe_id_ = 0;
        pmtud_probe_size_ = 0;
        pmtud_known_good_ = min_transport_mtu;
        pmtud_known_bad_ = 0;
        active_transport_mtu_ = min_transport_mtu;

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "PMTUD restart: "
                << reason
                << "\n";
        }

        start_pmtud();
    }

    void send_mtu_probe(std::size_t target_mtu) {
        if (not protocol_v5_.ready()) return;
        if (pmtud_probe_pending_) {
            if (log_enabled(LogLevel::debug)) {
                std::cerr
                    << "PMTUD send skipped: probe pending id="
                    << pmtud_probe_id_
                    << " mtu="
                    << pmtud_probe_size_
                    << "\n";
            }
            return;
        }

        const std::size_t overhead =
            udp_.outer_ip_header_size() +
            udp_header_size +
            protocol_probe_v5_size;

        if (target_mtu <= overhead) {
            if (log_enabled(LogLevel::debug)) {
                std::cerr
                    << "PMTUD send skipped: target-mtu="
                    << target_mtu
                    << " overhead="
                    << overhead
                    << "\n";
            }
            return;
        }

        Packet packet;
        packet.type = PacketType::mtu_probe;
        packet.tunnel_id = tunnel_id_;
        packet.protocol_version = protocol_version_v5;
        packet.message_id = message_id_generator_.next();
        packet.original_length =
            static_cast<std::uint32_t>(target_mtu);
        packet.payload.resize(target_mtu - overhead);

        const auto encoded = protocol_v5_.encode(packet);
        if (encoded.empty()) return;

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "PMTUD send probe: id="
                << packet.message_id
                << " target-mtu="
                << target_mtu
                << " udp-payload="
                << encoded.size()
                << " outer-ip-overhead="
                << (overhead - protocol_probe_v5_size)
                << "\n";
        }

        const ssize_t sent =
            udp_.send(
                encoded.data(),
                encoded.size());

        const int send_error =
            sent < 0 ? errno : 0;

        ++stats_.pmtud_probes_sent;

        if (sent < 0) {
            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "PMTUD probe send failed: id="
                    << packet.message_id
                    << " target-mtu="
                    << target_mtu
                    << " errno="
                    << send_error
                    << " ("
                    << std::strerror(send_error)
                    << ")\n";
            }

            ++stats_.udp_send_errors;

            // Regardless of errno, this process cannot currently transmit
            // the candidate size. Treat it as unusable and keep searching
            // below it. In particular, a local firewall can report EPERM
            // instead of silently dropping an oversized test datagram.
            pmtud_known_bad_ = target_mtu;

            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "PMTUD continuing below failed mtu="
                    << target_mtu
                    << "\n";
            }

            advance_pmtud_search();
            return;
        }

        ++stats_.udp_tx_packets;
        stats_.udp_tx_bytes +=
            static_cast<std::uint64_t>(sent);

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "PMTUD probe sent: id="
                << packet.message_id
                << " target-mtu="
                << target_mtu
                << " udp-bytes="
                << sent
                << "\n";
        }

        pmtud_probe_pending_ = true;
        pmtud_probe_id_ = packet.message_id;
        pmtud_probe_size_ = target_mtu;
        pmtud_probe_sent_at_ =
            std::chrono::steady_clock::now();
    }

    void handle_mtu_probe(
        const Packet& packet,
        std::size_t udp_payload_size,
        const sockaddr_storage& source) {

        const std::size_t observed_outer_size =
            source_ip_header_size(source) +
            udp_header_size +
            udp_payload_size;

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "PMTUD recv probe: id="
                << packet.message_id
                << " declared-mtu="
                << packet.original_length
                << " observed-mtu="
                << observed_outer_size
                << " udp-bytes="
                << udp_payload_size
                << "\n";
        }

        if (
            observed_outer_size !=
                static_cast<std::size_t>(packet.original_length)) {

            log_info("PMTUD probe ignored: declared/observed MTU mismatch");
            return;
        }

        Packet reply;
        reply.type = PacketType::mtu_reply;
        reply.tunnel_id = tunnel_id_;
        reply.protocol_version = protocol_version_v5;
        reply.message_id = packet.message_id;
        reply.original_length =
            static_cast<std::uint32_t>(observed_outer_size);

        const auto encoded = protocol_v5_.encode(reply);
        if (encoded.empty()) return;

        const ssize_t sent =
            udp_.send(
                encoded.data(),
                encoded.size());

        const int send_error =
            sent < 0 ? errno : 0;

        if (sent < 0) {
            ++stats_.udp_send_errors;
            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "PMTUD reply send failed: id="
                    << reply.message_id
                    << " mtu="
                    << reply.original_length
                    << " errno="
                    << send_error
                    << " ("
                    << std::strerror(send_error)
                    << ")\n";
            }
            return;
        }

        ++stats_.udp_tx_packets;
        stats_.udp_tx_bytes +=
            static_cast<std::uint64_t>(sent);

        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "PMTUD reply sent: id="
                << reply.message_id
                << " mtu="
                << reply.original_length
                << " udp-bytes="
                << sent
                << "\n";
        }
    }

    void handle_mtu_reply(const Packet& packet) {
        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "PMTUD recv reply: id="
                << packet.message_id
                << " mtu="
                << packet.original_length
                << " pending="
                << (pmtud_probe_pending_ ? "yes" : "no")
                << " expected-id="
                << pmtud_probe_id_
                << " expected-mtu="
                << pmtud_probe_size_
                << "\n";
        }

        if (
            not pmtud_probe_pending_ or
            packet.message_id != pmtud_probe_id_ or
            packet.original_length != pmtud_probe_size_) {

            log_info("PMTUD reply ignored: no matching pending probe");
            return;
        }

        pmtud_probe_pending_ = false;
        ++stats_.pmtud_probes_ok;

        pmtud_known_good_ =
            std::max(
                pmtud_known_good_,
                pmtud_probe_size_);

        active_transport_mtu_ =
            pmtud_known_good_;

        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "PMTUD probe confirmed: id="
                << packet.message_id
                << " mtu="
                << pmtud_known_good_
                << "\n";
        }

        advance_pmtud_search();
    }

    void handle_pmtud_timeout(
        const std::chrono::steady_clock::time_point& now) {

        if (
            not pmtud_probe_pending_ or
            now - pmtud_probe_sent_at_ <
                std::chrono::seconds(
                    pmtud_probe_timeout_seconds)) {

            return;
        }

        pmtud_probe_pending_ = false;
        ++stats_.pmtud_probes_lost;

        pmtud_known_bad_ =
            pmtud_probe_size_;

        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "PMTUD probe timeout: id="
                << pmtud_probe_id_
                << " mtu="
                << pmtud_probe_size_
                << "\n";
        }

        advance_pmtud_search();
    }

    void advance_pmtud_search() {
        if (pmtud_probe_pending_) {
            return;
        }

        if (
            pmtud_known_good_ >= pmtud_upper_bound() or
            (
                pmtud_known_bad_ != 0 and
                pmtud_known_bad_ <=
                    pmtud_known_good_ + 1)) {

            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "PMTUD complete: outer-mtu="
                    << active_transport_mtu_
                    << " max-fragment-payload="
                    << maximum_fragment_payload()
                    << "\n";
            }

            return;
        }

        std::size_t candidate = 0;

        if (pmtud_known_bad_ == 0) {
            candidate = pmtud_upper_bound();
        } else {
            candidate =
                pmtud_known_good_ +
                (pmtud_known_bad_ -
                 pmtud_known_good_) / 2;
        }

        if (candidate <= pmtud_known_good_) {
            candidate = pmtud_known_good_ + 1;
        }

        send_mtu_probe(candidate);
    }

    bool stats_file_enabled() const {
        return not options_.stats_disabled and not options_.stats_file.empty();
    }

    void update_stats_control() {
        const auto request = take_stats_requests();
        if (request.toggle)
            options_.stats_disabled = not options_.stats_disabled;
        if (request.snapshot or (request.toggle and stats_file_enabled()))
            write_stats(request.snapshot);
    }

    // One in-memory snapshot format for both the socket and optional file export.
    void format_stats(std::ostream& output) {
        const auto now_steady = std::chrono::steady_clock::now();
        throughput_.update(now_steady, {
            stats_.tun_rx_bytes, stats_.tun_tx_bytes,
            stats_.udp_rx_bytes, stats_.udp_tx_bytes});
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(
                now_steady - started_at_).count();

        const auto now_system =
            std::chrono::system_clock::now();
        const auto updated_unix =
            std::chrono::duration_cast<std::chrono::seconds>(
                now_system.time_since_epoch()).count();

        output
            << "format=txt\n"
            << "format_version=1\n"
            << "pid=" << ::getpid() << "\n"
            << "tunnel_id=" << tunnel_id_ << "\n"
            << "mode=" << (server_mode_ ? "server" : "client") << "\n"
            << "updated_unix=" << updated_unix << "\n"
            << "uptime_seconds=" << uptime << "\n"
            << "stats_enabled=" << (stats_file_enabled() ? 1 : 0) << "\n"
            << "tun_rx_packets=" << stats_.tun_rx_packets << "\n"
            << "tun_rx_bytes=" << stats_.tun_rx_bytes << "\n"
            << "tun_tx_packets=" << stats_.tun_tx_packets << "\n"
            << "tun_tx_bytes=" << stats_.tun_tx_bytes << "\n"
            << "udp_rx_packets=" << stats_.udp_rx_packets << "\n"
            << "udp_rx_bytes=" << stats_.udp_rx_bytes << "\n"
            << "udp_tx_packets=" << stats_.udp_tx_packets << "\n"
            << "udp_tx_bytes=" << stats_.udp_tx_bytes << "\n"
            << "data_tx_packets=" << stats_.data_tx_packets << "\n"
            << "data_rx_packets=" << stats_.data_rx_packets << "\n"
            << "fragments_tx=" << stats_.fragments_tx << "\n"
            << "fragments_rx=" << stats_.fragments_rx << "\n"
            << "drops_protocol=" << stats_.drops_protocol << "\n"
            << "drops_tunnel_id=" << stats_.drops_tunnel_id << "\n"
            << "drops_replay=" << stats_.drops_replay << "\n"
            << "init_timestamp_rejected=" << stats_.init_timestamp_rejected << "\n"
            << "init_nonce_capacity_rejected=" << stats_.init_nonce_capacity_rejected << "\n"
            << "drops_mtu=" << stats_.drops_mtu << "\n"
            << "drops_process=" << stats_.drops_process << "\n"
            << "udp_send_errors=" << stats_.udp_send_errors << "\n"
            << "tun_write_errors=" << stats_.tun_write_errors << "\n"
            << "switch_rx_packets=" << stats_.switch_rx_packets << "\n"
            << "switch_rx_bytes=" << stats_.switch_rx_bytes << "\n"
            << "switch_tx_packets=" << stats_.switch_tx_packets << "\n"
            << "switch_tx_bytes=" << stats_.switch_tx_bytes << "\n"
            << "switch_drops=" << stats_.switch_drops << "\n"
            << "switch_send_errors=" << stats_.switch_send_errors << "\n"
            << "switch_connected=" << (switch_ and switch_->connected() ? 1 : 0) << "\n"
            << "switch_disconnects=" << stats_.switch_disconnects << "\n"
            << "switch_reconnect_attempts=" << stats_.switch_reconnect_attempts << "\n"
            << "switch_reconnects=" << stats_.switch_reconnects << "\n"
            << "switch_socket_errors=" << stats_.switch_socket_errors << "\n"
            << "switch_socket_eacces=" << stats_.switch_socket_eacces << "\n"
            << "switch_socket_enoent=" << stats_.switch_socket_enoent << "\n"
            << "switch_socket_econnrefused=" << stats_.switch_socket_econnrefused << "\n"
            << "switch_socket_other_errors=" << stats_.switch_socket_other_errors << "\n"
            << "switch_last_error_ts=" << stats_.switch_last_error_ts << "\n"
            << "switch_last_error_no=" << stats_.switch_last_error_no << "\n"
            << "event_poll_overload=" << (adaptive_polling_.overloaded() ? 1 : 0) << "\n"
            << "event_poll_busy_streak=" << adaptive_polling_.busy_streak() << "\n"
            << "event_poll_batch=" << adaptive_polling_.batch_size() << "\n"
            << "event_poll_overload_entries=" << adaptive_polling_.overload_entries() << "\n"
            << "event_poll_backlog_confirmations=" << adaptive_polling_.backlog_confirmations() << "\n"
            << "event_poll_slice_limit_hits=" << adaptive_polling_.slice_limit_hits() << "\n"
            << std::fixed << std::setprecision(3)
            << "rtt_last_ms=" << stats_.rtt_last_ms << "\n"
            << "rtt_min_ms=" << stats_.rtt_min_ms << "\n"
            << "rtt_max_ms=" << stats_.rtt_max_ms << "\n"
            << "rtt_avg_ms=" << stats_.rtt_avg_ms << "\n"
            << "rtt_jitter_ms=" << stats_.rtt_jitter_ms << "\n"
            << std::defaultfloat
            << "rtt_samples=" << stats_.rtt_samples << "\n"
            << "rtt_lost=" << stats_.rtt_lost << "\n"
            << "pmtud=" << (options_.pmtud_auto ? "auto" : "off") << "\n"
            << "transport_mtu_configured=" << options_.transport_mtu << "\n"
            << "transport_mtu_active=" << active_transport_mtu_ << "\n"
            << "pmtud_known_good=" << pmtud_known_good_ << "\n"
            << "pmtud_known_bad=" << pmtud_known_bad_ << "\n"
            << "pmtud_probes_sent=" << stats_.pmtud_probes_sent << "\n"
            << "pmtud_probes_ok=" << stats_.pmtud_probes_ok << "\n"
            << "pmtud_probes_lost=" << stats_.pmtud_probes_lost << "\n";

        protocol_v5_.write_stats(output, now_steady);
        output << "processing_sample_interval=" << ProcessingStats::sample_interval << "\n";
        throughput_.write(output);
        tx_processing_.write(output, "tx_processing");
        rx_processing_.write(output, "rx_processing");
        reassembly_span_.write(output, "reassembly_span");
    }

    void write_stats(bool snapshot = false) {
        if (not stats_write_requested(not options_.stats_file.empty(),
                                      options_.stats_disabled, snapshot)) return;

        const std::string temporary_file =
            options_.stats_file +
            ".tmp." +
            std::to_string(static_cast<long long>(::getpid()));

        {
            std::ofstream output(
                temporary_file,
                std::ios::out | std::ios::trunc);

            if (not output) {
                log_info("Unable to open stats file " + temporary_file);
                return;
            }

            format_stats(output);
            output.flush();

            if (not output) {
                log_info("Unable to write stats file " + temporary_file);
                return;
            }
        }

        if (
            std::rename(
                temporary_file.c_str(),
                options_.stats_file.c_str()) != 0) {

            log_info(
                "Unable to publish stats file " +
                options_.stats_file +
                ": " +
                std::strerror(errno));
            std::remove(temporary_file.c_str());
        }
    }

    std::uint16_t tunnel_id_ = 0;
    bool server_mode_ = false;
    Options options_;

    std::unique_ptr<TunDevice> tun_;
    UdpEndpoint udp_;
    std::unique_ptr<SwitchClient> switch_;
    std::unique_ptr<ControlSocket> control_;
    static constexpr auto switch_reconnect_interval_ = std::chrono::seconds(1);
    std::chrono::steady_clock::time_point next_switch_reconnect_ {};
    int last_switch_connect_error_ = 0;

    const ascon::key_type master_key_;
    SessionProtocol protocol_v5_;
    SessionProtocol::Time init_warning_next_ {};
    std::uint64_t init_warning_suppressed_ = 0;

    SequenceGenerator message_id_generator_;

    // Hot-path storage is allocated/reserved once during construction and
    // reused for subsequent packets. This removes allocator churn without
    // changing Packet/process() semantics or the wire format.
    std::vector<std::uint8_t> tun_rx_buffer_;
    std::vector<std::uint8_t> udp_rx_buffer_;
    std::vector<std::uint8_t> switch_rx_buffer_;

    Packet tx_logical_packet_;
    Packet tx_fragment_packet_;
    Packet rx_packet_;
    Packet rx_logical_packet_;

    std::vector<std::uint8_t> tx_encoded_buffer_;
    std::vector<std::uint8_t> tx_mac_buffer_;
    std::vector<std::uint8_t> rx_mac_buffer_;
    std::vector<std::uint8_t> reassembled_packet_;

    ThroughputStats throughput_;
    ProcessingStats tx_processing_;
    ProcessingStats rx_processing_;
    ProcessingStats reassembly_span_;
    bool tx_sample_active_ = false;
    bool rx_sample_active_ = false;
    ProcessingStats::Clock::time_point tx_sample_start_ {};
    ProcessingStats::Clock::time_point rx_sample_start_ {};
    std::unordered_map<std::uint64_t, ProbeState> rtt_probes_;

    AdaptivePolling adaptive_polling_;
    unsigned next_data_source_ = 0;

    std::size_t active_transport_mtu_ = min_transport_mtu;
    bool pmtud_started_ = false;
    bool pmtud_probe_pending_ = false;
    std::size_t pmtud_known_good_ = min_transport_mtu;
    std::size_t pmtud_known_bad_ = 0;
    std::uint64_t pmtud_probe_id_ = 0;
    std::size_t pmtud_probe_size_ = 0;
    std::chrono::steady_clock::time_point pmtud_probe_sent_at_ {};

    bool rtt_probe_schedule_active_ = false;
    std::chrono::steady_clock::time_point next_rtt_probe_ {};
    Stats stats_;
    const std::chrono::steady_clock::time_point started_at_ =
        std::chrono::steady_clock::now();
};

} // namespace tuntom
