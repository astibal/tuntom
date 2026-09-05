#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace tuntom {
inline void secure_zero(void* pointer, std::size_t size) noexcept {
    auto* bytes = static_cast<volatile std::uint8_t*>(pointer);
    while (size--) *bytes++ = 0;
}
// Wipes stack temporaries on both normal return and exception unwinding.
class WipeGuard {
public:
    WipeGuard(void* data, std::size_t size) : data_(data), size_(size) {}
    ~WipeGuard() { secure_zero(data_, size_); }
    WipeGuard(const WipeGuard&) = delete;
    WipeGuard& operator=(const WipeGuard&) = delete;
private:
    void* data_;
    std::size_t size_;
};
template<std::size_t N> struct Secret {
    std::array<std::uint8_t, N> bytes {};
    Secret() = default;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret() { clear(); }
    void clear() noexcept { secure_zero(bytes.data(), bytes.size()); }
};
} // namespace tuntom
