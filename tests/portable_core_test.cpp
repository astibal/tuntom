#include "session.hpp"
#include "control_auth.hpp"
#include <iostream>

namespace {
namespace ca = tuntom::control_auth;
using Session = tuntom::SessionProtocol;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void control_authentication() {
    auto signer = std::make_shared<ca::SigningKey>();
    ca::random(signer->secret.bytes);
    tuntom::x25519::public_key(signer->grant.pub, signer->secret.bytes);
    signer->grant.caps = 5;
    signer->grant.levels.fill(7);
    ca::Config node_config, signer_config;
    node_config.allow_trusted = true;
    node_config.level = 3;
    node_config.trusted.emplace(signer->grant.pub, signer->grant);
    signer_config.allow_trusted = true;
    signer_config.signing.push_back(signer);
    ca::Auth node, authority;
    ca::Id origin{}, request{};
    origin[0] = 9;
    request[0] = 1;
    node.configure(node_config, origin);
    authority.configure(signer_config, origin);
    const auto now = ca::Clock::now();
    const auto challenge = node.challenge(request, 5, now);
    require(challenge.size() == ca::challenge_size, "CONTROL challenge generation failed");
    require(authority.accept_challenge(request, challenge, now), "CONTROL challenge rejected");
    const ca::Bytes message{1, 2, 3};
    const auto proof = authority.protect(request, message, false);
    require(proof.size() == ca::proof_size, "CONTROL proof generation failed");
    auto altered = proof;
    altered.back() ^= 1;
    ca::Key principal{};
    require(!node.verify(request, message, altered, 5, principal), "Tampered CONTROL proof accepted");
    require(node.verify(request, message, proof, 5, principal), "Valid CONTROL proof rejected");
    require(principal == signer->grant.pub, "Wrong CONTROL principal");
    require(!node.verify(request, message, proof, 5, principal), "Replayed CONTROL proof accepted");
}

void encrypted_session() {
    tuntom::ascon::key_type key{};
    require(tuntom::platform::random_bytes(key.data(), key.size()), "Session entropy failed");
    Session client(42, key, false, 1500, true, 300, true);
    Session server(42, key, true, 1500, true, 300, true);
    tuntom::secure_zero(key.data(), key.size());
    tuntom::Packet decoded;
    std::vector<std::uint8_t> scratch;
    const auto receive = [&](Session& session, const std::vector<std::uint8_t>& wire) {
        require(!wire.empty(), "Missing handshake flight");
        return session.receive(wire.data(), wire.size(), decoded, scratch, Session::Clock::now());
    };
    auto response = receive(server, client.begin(Session::Clock::now())).reply;
    auto confirmation = receive(client, response).reply;
    auto ack = receive(server, confirmation).reply;
    require(receive(client, ack).activated, "Session activation failed");
    tuntom::Packet packet;
    packet.tunnel_id = 42;
    packet.message_id = 1;
    packet.payload.assign(128, 0x45);
    packet.original_length = static_cast<uint32_t>(packet.payload.size());
    const auto wire = client.encode(packet);
    require(receive(server, wire).data && decoded.payload == packet.payload, "Encrypted packet failed");
    require(receive(server, wire).replay_drop, "Session replay accepted");
}
} // namespace

int main() {
    try {
        require(tuntom::platform::random_bytes(nullptr, 0), "Empty entropy request failed");
        require(!tuntom::platform::random_bytes(nullptr, 1), "Invalid entropy buffer accepted");
        control_authentication();
        encrypted_session();
        std::cout << "PASS: portable CONTROL authentication and encrypted PFS session\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
