#include "../src/x25519.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
using tuntom::x25519::Bytes;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
Bytes hex(const char* value) {
    Bytes result {};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(std::stoul(std::string(value + 2*i, 2), nullptr, 16));
    return result;
}
int main() {
    using namespace tuntom::x25519;
    // RFC 7748 sections 5.2 and 6.1: https://www.rfc-editor.org/rfc/rfc7748
    const auto a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto b = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const auto ap = hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const auto bp = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const auto z = hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    Bytes out {}, other {};
    public_key(out, a); require(out == ap, "Alice public");
    public_key(out, b); require(out == bp, "Bob public");
    require(shared(out, a, bp) && out == z, "Alice shared");
    require(shared(out, b, ap) && out == z, "Bob shared");
    auto high = bp; high[31] ^= 0x80;
    require(shared(out, a, high) && out == z, "public high bit masking");
    auto unclamped = a; unclamped[0] ^= 7; unclamped[31] ^= 0xc0;
    require(shared(out, unclamped, bp) && out == z, "scalar clamping");
    for (const auto* low : {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"}) {
        out.fill(42);
        require(!shared(out, a, hex(low)) && out == Bytes{}, "low-order input");
    }
    auto noncanonical = hex("f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
    Bytes base {}; base[0] = 9;
    require(shared(out, a, noncanonical) && shared(other, a, base) && out == other,
            "noncanonical basepoint");
    Bytes k = base, u = base;
    for (int i = 0; i < 1000; ++i) {
        require(shared(out, k, u), "iteration zero");
        u = k; k = out;
        if (i == 0) require(k == hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"), "iteration 1");
    }
    require(k == hex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"), "iteration 1000");
    std::cout << "PASS: X25519 RFC vectors, 1000 iterations, clamping, noncanonical and low-order inputs\n";
}
