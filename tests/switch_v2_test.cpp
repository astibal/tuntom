#include "../src/ipc/switch_handshake.hpp"
#include "../src/switch_client.hpp"
#include <dirent.h>
#include <iostream>
#include <sys/wait.h>

using namespace tuntom;
namespace v2 = tuntom::ipc;
static void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
static std::vector<std::uint8_t> frame(std::size_t n = 1500, std::uint64_t label = 77) {
    std::vector<std::uint8_t> payload(n);
    for (std::size_t i = 0; i < n; ++i) payload[i] = static_cast<std::uint8_t>(i ^ label);
    return encode_switch_frame(SwitchOpcode::switch_packet, {label}, payload.data(), payload.size());
}
static v2::Parameters geometry(unsigned slots = 16, unsigned batch = 8) {
    v2::Parameters p;
    p.epoch = 345; p.caps = v2::known_caps; p.abi = v2::pool_abi;
    p.slots = slots; p.batch = batch; p.capacity = p.frame_limit = v2::max_frame;
    p.stride = (64ULL + p.capacity + 63) & ~63ULL;
    p.mapping_size = 4096 + p.slots * p.stride;
    return p;
}
struct Pair {
    v2::File a, b;
    v2::Transport producer, consumer;
    v2::Parameters p;
    explicit Pair(unsigned slots = 16, unsigned batch = 8) : p(geometry(slots, batch)) {
        int fds[2]; require(!::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), "socketpair");
        a = v2::File(fds[0]); b = v2::File(fds[1]);
        auto source = v2::Mapping::create(p, 1);
        auto target = v2::Mapping::attach(v2::File(::dup(source.fd())), p, 1);
        source.close_fd();
        producer.configure(p, {}, std::move(source));
        consumer.configure(p, std::move(target), {});
    }
};
static std::size_t fd_count() {
    DIR *d = ::opendir("/proc/self/fd"); require(d, "fd directory");
    std::size_t n = 0; while (::readdir(d)) ++n; ::closedir(d); return n;
}
static void codecs() {
    auto r = v2::hello({}); v2::Parameters p;
    require(v2::decode_hello(r.bytes.data(), r.size, p), "HELLO");
    require(r.bytes[3] == 2 && r.size == 32, "V2 wire assignment");
    for (std::size_t n = 0; n < r.size; ++n) require(!v2::decode_hello(r.bytes.data(), n, p), "truncated HELLO");
    r.bytes[28] = 1; require(!v2::decode_hello(r.bytes.data(), r.size, p), "reserved HELLO");
    p = geometry(); r = v2::welcome(p); v2::Parameters copy;
    require(v2::decode_welcome(r.bytes.data(), r.size, copy), "WELCOME");
    r = v2::setup(p); require(v2::decode_setup(r.bytes.data(), r.size, copy, copy), "SETUP");
    r.bytes[40] ^= 1; require(!v2::decode_setup(r.bytes.data(), r.size, p, copy), "overflow geometry");
    v2::Ref ref{0, 17, 1}; r = v2::references(p, 1, &ref, 1);
    std::array<v2::Ref, v2::max_batch> refs; std::size_t count = 0;
    require(v2::decode_references(r.bytes.data(), r.size, p, 1, refs, count) && count == 1, "BATCH one frame");
    r.bytes[22] = 1; require(!v2::decode_references(r.bytes.data(), r.size, p, 1, refs, count), "reserved batch");
    for (const auto caps : {0U, v2::mmap_ref, v2::known_caps}) {
        auto g = geometry(); g.caps = caps;
        if (!caps) g.inline_only();
        if (caps == v2::mmap_ref) g.batch = 1;
        r = v2::welcome(g); require(v2::decode_welcome(r.bytes.data(), r.size, copy), "capability combinations");
    }
}
static void mixed_transport() {
    Pair pair(4, 4);
    const auto packet = frame();
    std::array<v2::FrameParts, 4> frames;
    frames.fill({packet.data(), packet.size()});
    require(pair.producer.send_batch(pair.a.get(), frames.data(), frames.size()) == 4, "batched send");
    require(pair.producer.tx.records == 1 && pair.producer.tx.mapped_frames == 4, "logical vs records");
    require(pair.producer.send(pair.a.get(), frames[0]) == static_cast<ssize_t>(packet.size()), "full pool inline fallback");
    require(pair.producer.tx.pool_fallback == 1 && pair.producer.tx.inline_frames == 1, "fallback stats");
    std::array<std::uint8_t, v2::max_frame> buffer;
    for (unsigned i = 0; i < 5; ++i) {
        require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) == static_cast<ssize_t>(packet.size()), "mixed receive");
        require(std::equal(packet.begin(), packet.end(), buffer.begin()), "mixed data exact");
        require(pair.consumer.receive_pending() == (i < 3), "pending refs survive empty socket");
    }
    require(pair.producer.send_batch(pair.a.get(), frames.data(), frames.size()) == 4, "reused generations");
    for (unsigned i = 0; i < 4; ++i) require(pair.consumer.receive(pair.b.get(), buffer.data(), 9) == static_cast<ssize_t>(packet.size()), "small buffer full logical length");
    require(pair.producer.send_batch(pair.a.get(), frames.data(), frames.size()) == 4, "truncation releases slots");
    for (unsigned i = 0; i < 4; ++i) require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) > 0, "drain");
    v2::Transport::Outcome total;
    for (unsigned i = 0; i < 3; ++i) total.add(pair.producer.append(pair.a.get(), frames[0]));
    require(total.frames == 0, "staging does not inflate sent stats");
    total.add(pair.producer.flush(pair.a.get()));
    require(total.frames == 3 && total.bytes == packet.size() * 3 && !total.drops, "partial flush without timer");
    for (unsigned i = 0; i < 3; ++i) require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) > 0, "staged receive");
}
static void backpressure() {
    Pair pair(8, 8);
    const auto packet = frame(1500);
    int small = 4096; require(!::setsockopt(pair.a.get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), "sndbuf");
    unsigned accepted = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        const auto n = pair.producer.send(pair.a.get(), {packet.data(), packet.size()});
        if (n < 0) { require(v2::retry_error(), "backpressure error"); break; }
        ++accepted;
    }
    require(accepted && accepted < 10000, "socket bounded");
    std::array<std::uint8_t, v2::max_frame> buffer;
    for (unsigned i = 0; i < accepted; ++i) require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) == static_cast<ssize_t>(packet.size()), "accepted frames no replay");
    std::array<v2::FrameParts, 8> frames; frames.fill({packet.data(), packet.size()});
    require(pair.producer.send_batch(pair.a.get(), frames.data(), frames.size()) == 8, "failed send returned every slot");
    for (unsigned i = 0; i < 8; ++i) require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) > 0, "post-backpressure drain");
}
static void deferred_batch() {
    Pair pair(8,8);
    int small=4096;
    require(!setsockopt(pair.a.get(),SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)),"sndbuf");
    const auto packet=frame();
    while (::send(pair.a.get(),packet.data(),packet.size(),MSG_DONTWAIT|MSG_NOSIGNAL)>=0) {}
    require(errno==EAGAIN,"fill socket");
    v2::Transport::Outcome total;
    for(unsigned i=0;i<3;++i) total.add(pair.producer.append(pair.a.get(),{packet.data(),packet.size()}));
    total.add(pair.producer.flush(pair.a.get()));
    require(!total.frames && !total.drops,"EAGAIN retains batch");
    std::array<uint8_t,v2::max_frame> buffer;
    while (::recv(pair.b.get(),buffer.data(),buffer.size(),MSG_DONTWAIT)>0) {}
    ::usleep(2000);
    total.add(pair.producer.flush(pair.a.get()));
    require(total.frames==3 && !total.drops,"retry submits batch exactly once");
    for(unsigned i=0;i<3;++i) {
        require(pair.consumer.receive(pair.b.get(),buffer.data(),buffer.size())==static_cast<ssize_t>(packet.size()),"retry receive");
        require(std::equal(packet.begin(),packet.end(),buffer.begin()),"retry payload preserved");
    }
    require(pair.consumer.receive(pair.b.get(),buffer.data(),buffer.size())<0 && errno==EAGAIN,"no duplicates");
    std::array<v2::FrameParts,8> frames; frames.fill({packet.data(),packet.size()});
    require(pair.producer.send_batch(pair.a.get(),frames.data(),8)==8,"all mmap slots reclaimed");
}
static void invalid_references() {
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Pair pair;
        const auto packet = frame(64);
        require(pair.producer.send(pair.a.get(), {packet.data(), packet.size()}) > 0, "prepare reference");
        v2::Record r;
        auto n = ::recv(pair.b.get(), r.bytes.data(), r.bytes.size(), 0);
        require(n > 0, "intercept ref"); r.size = static_cast<std::size_t>(n);
        switch (mutation) {
        case 0: r.bytes[8] ^= 1; break; // epoch
        case 1: r.bytes[19] = 2; break; // wrong direction
        case 2: r.bytes[24] = 255; break; // slot bounds
        case 3: r.bytes[28] = 255; break; // frame bounds
        case 4: r.bytes[39] += 2; break; // stale token
        case 5: r.bytes[39] = 0; break; // even token
        case 6: r.bytes[22] = 1; break; // reserved
        case 7: r.size -= 1; break;
        case 8:
            std::copy_n(r.bytes.data() + 24, 16, r.bytes.data() + 40);
            r.size = 56; store_be16(r.bytes.data() + 6, 56); store_be16(r.bytes.data() + 20, 2); break;
        }
        require(v2::send_record(pair.a.get(), r) == static_cast<ssize_t>(r.size), "inject ref");
        std::array<std::uint8_t, v2::max_frame> buffer;
        require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) == -1 && errno == EPROTO, "invalid ref rejected");
    }
    Pair pair;
    const auto packet = frame(64);
    require(pair.producer.send(pair.a.get(), {packet.data(), packet.size()}) > 0, "duplicate setup");
    v2::Record r; r.size = static_cast<std::size_t>(::recv(pair.b.get(), r.bytes.data(), r.bytes.size(), 0));
    require(v2::send_record(pair.a.get(), r) > 0, "first ref replay");
    std::array<std::uint8_t, v2::max_frame> buffer;
    require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) > 0, "first ref valid");
    require(v2::send_record(pair.a.get(), r) > 0, "duplicate ref");
    require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) < 0 && errno == EPROTO, "duplicate rejected");
}
static void invalid_mappings_and_fds() {
    const auto baseline = fd_count();
    {
        const auto p = geometry();
        auto pool = v2::Mapping::create(p, 1);
        const auto path = "/proc/self/fd/" + std::to_string(pool.fd());
        v2::File read_only(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        require(read_only.get() >= 0, "readonly pool handle");
        bool rejected = false;
        try { auto mapped = v2::Mapping::attach(std::move(read_only), p, 1); }
        catch (const v2::MappingError &e) { rejected = e.invalid; }
        require(rejected, "readonly descriptor is a protocol error, not mmap resource fallback");
        require(::ftruncate(pool.fd(), 0) < 0 && errno == EPERM, "size seal prevents shrink");
    }
    for (unsigned i = 0; i < 10; ++i) {
        const auto p = geometry();
        v2::File fd(::memfd_create("bad-map", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        require(fd.get() >= 0 && !::ftruncate(fd.get(), static_cast<off_t>(p.mapping_size)), "bad memfd");
        bool rejected = false;
        try { auto m = v2::Mapping::attach(std::move(fd), p, 1); }
        catch (const v2::MappingError &e) { rejected = e.invalid; }
        require(rejected, "unsealed fd rejected");
        Pair pair;
        auto r = v2::header(v2::Type::error, 16);
        v2::File extra(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        require(v2::send_record(pair.a.get(), r, extra.get(), extra.get()) > 0, "unexpected rights");
        std::array<std::uint8_t, v2::max_frame> buffer;
        const auto count = fd_count();
        require(pair.consumer.receive(pair.b.get(), buffer.data(), buffer.size()) == -1 && errno == EPROTO, "FD injection rejected");
        require(fd_count() == count, "received rights closed");
    }
    require(fd_count() == baseline, "mapping and socket cleanup");
}
struct Listener {
    char directory[64] = "/tmp/tuntom-v2-test.XXXXXX";
    std::string path;
    v2::File fd;
    Listener() {
        require(::mkdtemp(directory), "mkdtemp"); path = std::string(directory) + "/socket";
        fd = v2::File(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_un address{}; address.sun_family = AF_UNIX; std::strcpy(address.sun_path, path.c_str());
        require(!::bind(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) && !::listen(fd.get(), 8), "bind/listen");
    }
    v2::File accept() { return v2::File(::accept4(fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)); }
    ~Listener() { ::unlink(path.c_str()); ::rmdir(directory); }
};
static void handshake(v2::Mode mode, std::uint64_t memory, bool v1_server = false) {
    Listener listener;
    SwitchClient client(listener.path, "port", {mode});
    auto now = SwitchClient::Clock::now(); client.start_connect(now);
    auto peer = listener.accept(); require(peer.get() >= 0, "accept handshake");
    v2::ServerHandshake server; std::uint64_t epoch = 1; bool active_sent = false;
    v2::Options server_options; if (v1_server) server_options.mode = v2::Mode::legacy;
    for (unsigned i = 0; i < 50 && !client.connected(); ++i) {
        pollfd fds[]{{client.fd(), client.poll_events(), 0}, {peer.get(), server.events(), 0}};
        require(::poll(fds, 2, 10) >= 0, "handshake poll");
        now = SwitchClient::Clock::now();
        if (fds[1].revents) server.step(peer.get(), server_options, epoch);
        if (server.failed()) {
            require(v1_server && server.id.empty(), "legacy rejects unnamed probe only");
            peer.reset(); server = v2::ServerHandshake();
            client.advance_connect(now, POLLHUP);
            peer = listener.accept(); require(peer.get() >= 0, "legacy fallback reconnect");
            continue;
        }
        if (server.identified()) server.prepare(memory);
        if (server.activation_ready() && !server.legacy && !active_sent) {
            const auto r = v2::state(v2::Type::active, server.parameters());
            require(v2::send_record(peer.get(), r) == static_cast<ssize_t>(r.size), "ACTIVE");
            server.transport->close_pool_fds(); active_sent = true;
        }
        client.advance_connect(now, fds[0].revents);
    }
    require(client.connected(), "handshake connected");
    require(client.transport().mapped() == (mode == v2::Mode::automatic && memory > 0 && !v1_server), "negotiated transport");
    // A V1 client can connect synchronously before the server has registered it.
    if (!server.activation_ready()) { server.step(peer.get(), server_options, epoch); require(server.identified(), "v1 registration"); server.prepare(memory); }
    const auto packet = frame();
    require(client.send(packet.data(), packet.size()) > 0, "client payload");
    std::array<std::uint8_t, v2::max_frame> buffer;
    require(server.transport->receive(peer.get(), buffer.data(), buffer.size()) == static_cast<ssize_t>(packet.size()), "server payload");
    require(std::equal(packet.begin(), packet.end(), buffer.begin()), "negotiated exact payload");
    std::array<v2::FrameParts, 8> frames; frames.fill({packet.data(), packet.size()});
    const auto sent = server.transport->send_batch(peer.get(), frames.data(), 8);
    require(sent > 0, "server batch");
    for (ssize_t i = 0; i < sent; ++i) require(client.receive(buffer.data(), buffer.size()) == static_cast<ssize_t>(packet.size()), "client batch receive");
}
static void client_rejects_positive_corruption() {
    const auto before = fd_count();
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Listener listener;
        SwitchClient client(listener.path, "same");
        const auto now = SwitchClient::Clock::now(); client.start_connect(now);
        auto peer = listener.accept();
        std::array<std::uint8_t, 300> scratch;
        require(::recv(peer.get(), scratch.data(), scratch.size(), 0) == 32, "client HELLO");
        auto p = geometry(); auto reply = v2::welcome(p);
        if (scenario == 0) reply.bytes[36] = 1;
        require(v2::send_record(peer.get(), reply) > 0, "WELCOME response");
        client.advance_connect(now, POLLIN);
        if (scenario) {
            require(client.connecting(), "valid positive WELCOME");
            client.advance_connect(now, POLLOUT);
            require(::recv(peer.get(), scratch.data(), scratch.size(), 0) > 0, "named registration after WELCOME");
            auto a = v2::Mapping::create(p, 1); auto b = v2::Mapping::create(p, 2);
            reply = v2::setup(p);
            if (scenario == 1) reply.bytes[52] = 1;
            if (scenario == 3) { auto wrong = p; wrong.epoch++; reply = v2::setup(wrong); }
            if (scenario == 4) { auto wrong = p; wrong.inline_only(); reply = v2::setup(wrong); }
            require(v2::send_record(peer.get(), reply, a.fd(), scenario == 2 ? a.fd() : b.fd()) > 0, "corrupt SETUP");
            client.advance_connect(now, POLLIN);
        }
        require(!client.connected() && !client.connecting() && client.last_error() == EPROTO, "malformed positive reply is fatal");
        require(listener.accept().get() < 0 && errno == EAGAIN, "no downgrade after positive corruption");
    }
    require(fd_count() == before, "invalid client setup FD cleanup");
    Listener listener;
    SwitchClient client(listener.path, "slow");
    const auto now = SwitchClient::Time{};
    client.start_connect(now); auto peer = listener.accept();
    require(client.poll_timeout_ms(now, 5000) == 1000, "HELLO probe deadline");
    client.advance_connect(now + std::chrono::seconds(1), 0);
    require(client.connected(), "one legacy retry after unnamed probe timeout");
    auto legacy = listener.accept(); require(legacy.get() >= 0, "fallback socket");
    std::array<std::uint8_t, 300> bytes;
    const auto n = ::recv(legacy.get(), bytes.data(), bytes.size(), 0);
    std::string id; require(decode_switch_registration(bytes.data(), static_cast<std::size_t>(n), id) && id == "slow", "legacy registration after timeout");
}
static void private_geometry() {
    const auto p = geometry();
    auto owner = v2::Mapping::create(p, 1);
    auto consumer = v2::Mapping::attach(v2::File(::dup(owner.fd())), p, 1);
    const auto packet = frame(); v2::Ref ref;
    require(owner.reserve(static_cast<std::uint32_t>(packet.size()), ref), "reserve for geometry test");
    std::memcpy(owner.data(ref.slot), packet.data(), packet.size()); owner.publish(ref);
    auto *header = owner.data(0) - 64 - 4096;
    std::memset(header + 24, 255, 24); // Peer mutates slots/capacity/stride/length AFTER setup.
    require(consumer.token(ref.slot) == ref.token, "private slot arithmetic");
    require(!std::memcmp(consumer.data(ref.slot), packet.data(), packet.size()), "private geometry payload");
    consumer.release(ref); require(!(owner.token(ref.slot) & 1), "private geometry release");
    for (std::uint32_t i = 0; i < p.slots; ++i) owner.publish({i, 17, UINT64_MAX - 1});
    require(!owner.reserve(17, ref) && errno == EOVERFLOW, "exhausted generation never wraps");
}
static void forked_transfer() {
    Pair pair(32, 16);
    constexpr unsigned total = 32768;
    const auto child = ::fork(); require(child >= 0, "fork");
    if (!child) {
        pair.a.reset();
        try {
            std::array<std::uint8_t, v2::max_frame> output;
            const auto deadline = SwitchClient::Clock::now() + std::chrono::seconds(15);
            for (unsigned i = 0; i < total;) {
                const auto n = pair.consumer.receive(pair.b.get(), output.data(), output.size());
                if (n < 0 && v2::retry_error()) {
                    require(SwitchClient::Clock::now() < deadline, "child deadline");
                    pollfd fd{pair.b.get(), POLLIN, 0}; ::poll(&fd, 1, 10); continue;
                }
                const auto expected = frame(i % 3 ? 9000 : 64, i + 1);
                require(n == static_cast<ssize_t>(expected.size()) && std::equal(expected.begin(), expected.end(), output.begin()), "forked exact frame/order");
                ++i;
            }
            ::_exit(0);
        } catch (...) { ::_exit(1); }
    }
    pair.b.reset();
    std::array<std::vector<std::uint8_t>, 16> storage;
    std::array<v2::FrameParts, 16> frames;
    for (unsigned i = 0; i < total;) {
        const auto count = std::min(16U, total - i);
        for (unsigned j = 0; j < count; ++j) {
            storage[j] = frame((i + j) % 3 ? 9000 : 64, i + j + 1);
            frames[j] = {storage[j].data(), storage[j].size()};
        }
        const auto n = pair.producer.send_batch(pair.a.get(), frames.data(), count);
        if (n < 0 && v2::retry_error()) { pollfd fd{pair.a.get(), POLLOUT, 0}; ::poll(&fd, 1, 10); continue; }
        require(n > 0, "forked send"); i += static_cast<unsigned>(n);
    }
    int status = 0; require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent process transfer");
}

int main() {
    try {
        client_rejects_positive_corruption(); private_geometry(); forked_transfer();
        codecs(); mixed_transport(); backpressure(); deferred_batch(); invalid_references(); invalid_mappings_and_fds();
        handshake(v2::Mode::automatic, 64 * 1024 * 1024);
        handshake(v2::Mode::automatic, 0);
        handshake(v2::Mode::inline_only, 64 * 1024 * 1024);
        handshake(v2::Mode::legacy, 0);
        handshake(v2::Mode::automatic, 0, true);
        std::cout << "V2 codecs, pools, batching, fallback, ownership and handshake: OK\n";
    } catch (const std::exception &e) { std::cerr << e.what() << " (errno=" << errno << ")\n"; return 1; }
}
