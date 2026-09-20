// Rootless lab launcher: packet/crypto/IPC code is the real src/main.cpp.
// A single-ID user namespace cannot drop to nobody. Replace ONLY that startup
// policy, and refuse to run outside an isolated single-ID user/net namespace.
#define drop_privileges production_drop_privileges
#define harden_unprivileged_process production_harden_unprivileged_process
#include "privileges.hpp"
#undef drop_privileges
#undef harden_unprivileged_process
#include <fstream>

namespace tuntom {
inline void require_lab_namespace() {
    std::ifstream map("/proc/self/uid_map");
    unsigned long inside = 1, outside = 0, count = 0;
    std::string extra;
    map >> inside >> outside >> count;
    if (!map || inside != 0 || outside == 0 || count != 1 || (map >> extra) || ::geteuid() != 0)
        throw std::runtime_error("lab launcher requires unshare --user --map-root-user");
    struct stat own{}, initial{};
    if (::stat("/proc/self/ns/net", &own) || ::stat("/proc/1/ns/net", &initial) ||
        (own.st_dev == initial.st_dev && own.st_ino == initial.st_ino))
        throw std::runtime_error("lab launcher requires a separate network namespace");
}
inline void drop_privileges() {
    require_lab_namespace();
    harden_process_before_privilege_drop();
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0))
        throw std::runtime_error("lab process hardening failed");
}
inline void harden_unprivileged_process() { drop_privileges(); }
}

#define main production_tuntom_main
#include "../../src/main.cpp"
#undef main

int main(int argc, char** argv) {
    try {
        tuntom::require_lab_namespace(); // Before TUN/socket creation.
        return production_tuntom_main(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "rootless lab launcher: " << error.what() << '\n';
        return 1;
    }
}
