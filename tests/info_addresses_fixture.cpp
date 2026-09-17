// Deterministic interface enumeration for the rootless INFO process test.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <cerrno>

extern "C" int __real_getifaddrs(ifaddrs**);
extern "C" void __real_freeifaddrs(ifaddrs*);
static ifaddrs entries[3];
static sockaddr_in addresses[3];
extern "C" int __wrap_getifaddrs(ifaddrs** result) {
    const auto* path = std::getenv("TUNTOM_TEST_INFO_ADDRESSES");
    if (!path) return __real_getifaddrs(result);
    std::ifstream input(path);
    if (!input) { errno = EIO; return -1; }
    std::string address;
    unsigned count = 0;
    while (input >> address) {
        if (count == 3 || inet_pton(AF_INET, address.c_str(), &addresses[count].sin_addr) != 1) {
            errno = EINVAL; return -1;
        }
        addresses[count].sin_family = AF_INET;
        entries[count] = {};
        entries[count].ifa_flags = IFF_LOOPBACK;
        entries[count].ifa_addr = reinterpret_cast<sockaddr*>(&addresses[count]);
        if (count) entries[count - 1].ifa_next = &entries[count];
        ++count;
    }
    *result = count ? entries : nullptr;
    return 0;
}
extern "C" void __wrap_freeifaddrs(ifaddrs* value) {
    if (value && value != entries) __real_freeifaddrs(value);
}
