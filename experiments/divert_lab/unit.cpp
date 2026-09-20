#include "common.hpp"
#include <iostream>
#include <functional>

using namespace divert_lab;
void require(bool result) { if (!result) throw std::runtime_error("unit assertion failed"); }
void rejects(const std::function<void()>& fn) {
    try { fn(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("expected rejection");
}

int main() {
    Codec codec("hTX");
    for (const Labels& base : {Labels{17}, Labels{17, 42}}) {
        auto stack = codec.offer(base, 123);
        auto env = codec.split(stack);
        require(env.base == base && env.original() == base && env.origin() == 123);
        require(pretty_label(stack[base.size()]) == (base.size() == 1 ? "\"hTX3\"" : "\"hTX4\""));
        env.body[1] = onward;
        env.base[0] = 99;
        const auto changed = codec.split(codec.attach(env.base, env.body));
        require(changed.original() == base && changed.base[0] == 99);
        stack.pop_back();
        rejects([&] { codec.split(stack); });
    }
    auto embedded = codec.offer({17}, 123);
    embedded.push_back(42);
    require(codec.split(embedded).base == Labels({17, 42}));
    require(!codec.split({17, text_label("DVRT"), 123}).present);
    require(!codec.split(Codec("AbC").offer({17}, 123)).present);
    rejects([&] { Codec("A12"); });
    rejects([&] { codec.offer({1, 2, 3}, 123); }); // Production IPC cap, not fixed body length.
    rejects([&] { codec.split({17, text_label("hTX:"), text_label("DVRT"), 123}); });
    rejects([&] { codec.split({17, text_label("hTX0"), text_label("DVRT")}); });
    auto invalid = codec.offer({17}, 123);
    invalid[4] = 999;
    rejects([&] { codec.split(invalid); });
    require(pretty_label(65) == "65" && pretty_label(0) == "0");
    require(pretty_label(text_label("DVRT")) == "\"DVRT\"");
    require(pretty_label(text_label("hTX2")) == "\"hTX2\"");
    require(pretty_label(text_label("a\"b")) == "\"a\\\"b\"");

    Admission admission;
    PacketInfo old, fresh, silent;
    old.flow.version = 4; old.flow.protocol = 6;
    old.flow.source[0] = 10; old.flow.destination[0] = 20;
    old.flow.source_port = 40000; old.flow.destination_port = 8080; old.flags = 0x10;
    fresh = old; fresh.flow.source_port = 40001; fresh.flags = 2;
    silent = old; silent.flow.source_port = 40002;
    require(!admission.proxy(old, 0));
    require(admission.proxy(fresh, 1));
    fresh.flags = 0x10;
    require(admission.proxy(fresh, 2));
    auto reply = fresh; reply.flow = tuntom::reverse_key(fresh.flow); reply.flags = 0x12;
    require(admission.proxy(reply, 3));
    require(!admission.proxy(silent, 3599));
    require(admission.proxy(fresh, 3600)); // Discarding DIVERTED preserves behavior.
    require(!admission.proxy(old, 86399));
    require(admission.proxy(old, 86400));
    Admission udp;
    old.flow.protocol = fresh.flow.protocol = 17;
    require(!udp.proxy(old, 0));
    require(!udp.proxy(old, 60));
    require(udp.proxy(fresh, 60));
    require(!udp.proxy(old, 3599));
    require(udp.proxy(old, 3600));
    std::cout << "PASS: cookie framing, variable body, malformed frames, pretty labels, TCP/UDP phase boundaries\n";
}
