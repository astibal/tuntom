#include "../src/ascon.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
Bytes hex(const std::string& s) {
    Bytes b;
    for (std::size_t i = 0; i < s.size(); i += 2)
        b.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return b;
}
void require(bool ok) { if (!ok) throw std::runtime_error("AEAD vector failed"); }
int main() {
    std::ifstream file(std::string(__FILE__).substr(0, std::string(__FILE__).find_last_of('/')) + "/ascon-kat.txt");
    require(bool(file));
    std::string line;
    Bytes key, nonce, pt, ad;
    std::size_t count = 0;
    while (std::getline(file, line)) {
        auto eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        auto name = line.substr(0, eq), value = line.substr(eq + 3);
        if (name == "Key") key = hex(value);
        if (name == "Nonce") nonce = hex(value);
        if (name == "PT") pt = hex(value);
        if (name == "AD") ad = hex(value);
        if (name != "CT") continue;
        tuntom::ascon::key_type k;
        std::copy(key.begin(), key.end(), k.begin());
        tuntom::ascon::Aead128 aead(k);
        Bytes result(pt.size() + 16), plain(pt.size());
        aead.encrypt(nonce.data(), ad.data(), ad.size(), pt.data(), pt.size(), result.data(), result.data() + pt.size());
        require(result == hex(value));
        require(aead.decrypt(nonce.data(), ad.data(), ad.size(), result.data(), pt.size(), result.data() + pt.size(), plain.data()));
        require(plain == pt);
        auto in_place = pt;
        std::array<std::uint8_t,16> tag;
        aead.encrypt(nonce.data(), ad.data(), ad.size(), in_place.data(), in_place.size(), in_place.data(), tag.data());
        require(aead.decrypt(nonce.data(), ad.data(), ad.size(), in_place.data(), in_place.size(), tag.data(), in_place.data()));
        require(in_place == pt);
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] ^= 1;
            require(!aead.decrypt(nonce.data(), ad.data(), ad.size(), result.data(), pt.size(), result.data() + pt.size(), plain.data()));
            require(std::all_of(plain.begin(), plain.end(), [](auto x) { return x == 0; }));
            result[i] ^= 1;
        }
        ++count;
    }
    require(count == 1089);
    std::cout << "PASS: " << count << " official Ascon-AEAD128 KATs, in-place, tampering and failure wipe\n";
}
