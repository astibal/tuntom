#include "../src/udp_tx_queue.hpp"
#include <iostream>
#include <sstream>

using namespace tuntom;
using Wire = std::vector<std::uint8_t>;
using Q = UdpTxQueue;
using namespace std::chrono_literals;
static void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static std::uint64_t stat(const Q& q, const char* key) {
    std::ostringstream text; q.stats(text);
    std::istringstream in(text.str()); std::string line;
    while (std::getline(in, line)) {
        if (line.find(std::string(key) + '=') == 0) return std::stoull(line.substr(line.find('=') + 1));
    }
    throw std::runtime_error("missing counter");
}
int main() {
    try {
        Q q;
        const auto now = Q::Time{} + 1s;
        std::array<Wire, 3> packets {Wire{1,2,3}, Wire{4,5}, Wire{6}};
        require(q.append(packets.data(), 3, now), "enqueue");
        unsigned calls = 0;
        auto partial = [&](const std::uint8_t* data, std::size_t n) -> ssize_t {
            if (++calls == 2) { errno = EAGAIN; return -1; }
            require(data[0] == 1, "initial order");
            return static_cast<ssize_t>(n);
        };
        auto sent = q.flush(now, partial);
        require(sent.packets == 1 && sent.bytes == 3 && !sent.error, "successful prefix");
        require(stat(q,"udp_tx_queue_packets") == 2, "retain unsent suffix");
        require(!q.writable_interest(now) && q.poll_timeout(now,1000) == 1, "EAGAIN cooldown");
        q.flush(now, partial);
        require(calls == 2, "no spin during cooldown");
        packets[1][0] = 99; // Encoding buffers can be reused immediately.
        std::vector<unsigned> seen;
        auto send = [&](const std::uint8_t* data, std::size_t n) -> ssize_t {
            seen.push_back(data[0]); return static_cast<ssize_t>(n);
        };
        sent = q.flush(now + 1ms, send);
        require(sent.packets == 2 && seen == std::vector<unsigned>({4,6}) && q.empty(), "exact FIFO ciphertext copy");
        require(!q.writable_interest(now + 1ms), "no POLLOUT when empty");

        // Exercise wraparound, bounded admission and no successful-prefix replay.
        std::array<Wire, Q::capacity> full;
        for (unsigned i=0; i<full.size(); ++i) full[i] = Wire{static_cast<std::uint8_t>(i)};
        for (unsigned round=0; round<3; ++round) {
            seen.clear(); require(q.append(full.data(), full.size(), now + 2ms), "ring capacity");
            require(!q.append(full.data(), 1, now + 2ms), "bounded slot count");
            q.flush(now + 2ms, send);
            require(seen.size()==64 && seen.front()==0 && seen.back()==63, "wrapped FIFO order");
        }
        Wire large(65535, 7);
        std::array<Wire,5> oversized {large,large,large,large,large};
        require(!q.append(oversized.data(),5,now), "bounded bytes, atomic suffix admission");
        require(q.empty(), "overflow retained no partial packet");
        require(q.append(&large,1,now), "large datagram");
        require(q.poll_timeout(now,1000)==100, "expiration deadline");
        q.flush(now + 100ms, send);
        require(q.empty() && stat(q,"udp_tx_queue_expired")==1, "stale datagram never sent");
        require(q.append(packets.data(),3,now+200ms), "reset test");
        q.discard();
        require(q.empty() && stat(q,"udp_tx_queue_discarded")==3, "session/peer invalidation");
        require(q.append(packets.data(),1,now+200ms), "error test");
        sent=q.flush(now+200ms,[](const std::uint8_t*,std::size_t)->ssize_t { errno=EMSGSIZE; return -1; });
        require(sent.error==EMSGSIZE && q.empty(), "permanent error discarded, not retried");
        require(q.append(packets.data(),1,now+201ms), "interruption test");
        q.flush(now+201ms,[](const std::uint8_t*,std::size_t)->ssize_t { errno=EINTR; return -1; });
        require(!q.empty() && !q.writable_interest(now+201ms), "EINTR retained without spinning");
        q.flush(now+202ms,send);
        require(q.empty(), "retry after interruption");
        std::cout << "PASS: bounded UDP ring, FIFO retries, expiry and invalidation\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
