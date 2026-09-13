#pragma once

#include "switch_transport.hpp"
#include <memory>
#include <poll.h>

namespace tuntom::ipc {
// One step does at most one nonblocking socket operation. The owner supplies the
// fixed registration deadline and keeps named-port publication outside this FSM.
class ServerHandshake {
    enum class State { first, welcome, registration, identified, setup, ready, activate, failed };
    State state_ = State::first;
    Parameters parameters_{};
public:
    std::string id;
    bool legacy = false;
    std::unique_ptr<Transport> transport;
    bool identified() const { return state_ == State::identified; }
    bool activation_ready() const { return state_ == State::activate; }
    bool failed() const { return state_ == State::failed; }
    const Parameters &parameters() const { return parameters_; }
    short events() const {
        return state_ == State::welcome || state_ == State::setup || state_ == State::activate ? POLLOUT : POLLIN;
    }
    void step(int fd, const Options &options, std::uint64_t &epoch) {
        if (state_ == State::welcome || state_ == State::setup) {
            const bool setup_state = state_ == State::setup;
            const auto record = setup_state ? setup(parameters_) : welcome(parameters_);
            const auto sent = send_record(fd, record,
                setup_state && parameters_.caps ? transport->input_fd() : -1,
                setup_state && parameters_.caps ? transport->output_fd() : -1);
            if (sent == static_cast<ssize_t>(record.size))
                state_ = setup_state ? State::ready : State::registration;
            else if (sent >= 0 || !retry_error()) state_ = State::failed;
            return;
        }
        if (state_ != State::first && state_ != State::registration && state_ != State::ready) return;
        std::array<std::uint8_t, max_record> buffer{};
        const auto record = receive_record(fd, buffer.data(), buffer.size());
        if (record.size < 0 && retry_error()) return;
        if (record.size <= 0 || !record.valid || record.count || (record.flags & MSG_TRUNC)) {
            state_ = State::failed; return;
        }
        const auto n = static_cast<std::size_t>(record.size);
        if (state_ == State::first && buffer[0] == 'T' && buffer[1] == 'T' && buffer[2] == 'X') {
            if (options.mode == Mode::legacy || !decode_hello(buffer.data(), n, parameters_) || epoch == UINT64_MAX) {
                state_ = State::failed; return;
            }
            parameters_.epoch = ++epoch;
            parameters_.frame_limit = std::min(parameters_.frame_limit, options.frame_limit);
            parameters_.caps &= known_caps;
            if (options.mode != Mode::automatic || !supported_abi() || parameters_.abi != pool_abi)
                parameters_.inline_only();
            if (parameters_.caps & mmap_ref) {
                parameters_.slots = std::min({parameters_.slots, options.slots, max_slots});
                parameters_.capacity = std::min(parameters_.frame_limit, options.frame_capacity);
                parameters_.batch = parameters_.caps & mmap_batch ? std::min(parameters_.batch, options.batch) : 1;
            } else parameters_.inline_only();
            state_ = State::welcome;
        } else if (state_ == State::first || state_ == State::registration) {
            if (!decode_switch_registration(buffer.data(), n, id)) { state_ = State::failed; return; }
            legacy = state_ == State::first;
            state_ = State::identified;
        } else {
            std::uint32_t caps = 0, status = 0;
            if (!decode_state(buffer.data(), n, Type::ready, parameters_, caps, status) ||
                status > 1 || (status == 1 && caps) || (status == 0 && caps != parameters_.caps)) {
                state_ = State::failed; return;
            }
            if (!caps) { transport->disable_mmap(); parameters_.inline_only(); }
            state_ = State::activate;
        }
    }
    // Admission has checked the ID before any pool allocation. Memory is bounded
    // over active and pending connections, including same-ID replacements.
    void prepare(std::uint64_t available_bytes) {
        transport = std::make_unique<Transport>();
        if (legacy) { state_ = State::activate; return; }
        if (parameters_.caps & mmap_ref) {
            parameters_.stride = (64ULL + parameters_.capacity + 63) & ~63ULL;
            parameters_.mapping_size = 4096 + parameters_.slots * parameters_.stride;
            if (parameters_.mapping_size > available_bytes / 2) parameters_.inline_only();
        }
        if (parameters_.caps) {
            try {
                auto input = Mapping::create(parameters_, 1);
                auto output = Mapping::create(parameters_, 2);
                transport->configure(parameters_, std::move(input), std::move(output));
            } catch (const MappingError &) {
                parameters_.inline_only(); transport->configure(parameters_);
            }
        } else transport->configure(parameters_);
        state_ = State::setup;
    }
};
} // namespace tuntom::ipc
