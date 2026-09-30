#pragma once

#include "v5_extension.hpp"
#include <stdexcept>

namespace tuntom::v5ext {

// Transport-independent AUTH gate. The server is the only side allowed to
// originate a challenge. Every accepted response ends in exactly AUTH_OK or
// AUTH_FAILED; silence is a protocol failure handled by the caller's timeout.
class AuthState {
public:
    enum class Role { client, server };
    enum class State { open, need_challenge, pending, verifying, waiting_result, authenticated, rejected };
    AuthState(Role role, bool required = false) : role_(role), state_(required ? State::need_challenge : State::open) {
        if (role == Role::client && required) throw std::runtime_error("client cannot require server AUTH");
    }
    State state() const { return state_; }
    bool allows_data() const { return state_ == State::open || state_ == State::authenticated; }
    bool terminal_failure() const { return state_ == State::rejected; }
    bool challenged() const { return id_ != 0; }
    bool send_challenge(std::uint64_t id) {
        if(role_!=Role::server||state_!=State::need_challenge||!id)return false;
        id_=id;state_=State::pending;return true;
    }
    bool receive_challenge(const AuthChallenge& c) {
        if(role_!=Role::client||(state_!=State::open&&state_!=State::pending)||!c.id)return false;
        if(state_==State::pending&&id_==c.id)return true; // exact retransmit checked by caller
        if(state_!=State::open)return false;
        id_=c.id;state_=State::pending;return true;
    }
    bool send_response(std::uint64_t id) {
        if(role_!=Role::client||state_!=State::pending||id!=id_)return false;
        state_=State::waiting_result;return true;
    }
    bool receive_response(const AuthResponse& r) {
        if(role_!=Role::server||state_!=State::pending||r.id!=id_)return false;
        state_=State::verifying;return true;
    }
    bool verified(bool allow) {
        if(role_!=Role::server||state_!=State::verifying)return false;
        state_=allow?State::authenticated:State::rejected;return true;
    }
    bool receive_ok(const AuthOk& v) {
        if(role_!=Role::client||state_!=State::waiting_result||v.id!=id_)return false;
        state_=State::authenticated;return true;
    }
    bool receive_failed(const AuthFailed& v) {
        if(role_!=Role::client||state_!=State::waiting_result||v.id!=id_)return false;
        state_=State::rejected;return true;
    }
private:
    Role role_;State state_;std::uint64_t id_=0;
};

} // namespace tuntom::v5ext
