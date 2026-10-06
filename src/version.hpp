#pragma once

#include <iostream>
#include <string_view>

namespace tuntom {
// Protocol generation, compatibility revision (AUTH), release/build number.
inline constexpr std::string_view version = "5.1.123";

// Handle this before runtime initialization; no privileges or configuration needed.
inline bool print_version_if_requested(int argc, char** argv, std::string_view component) {
    if (argc != 2 || std::string_view(argv[1]) != "--version") return false;
    std::cout << component << ' ' << version << '\n';
    return true;
}
} // namespace tuntom
