// Manual kernel regression: run in a disposable network namespace (see README).
#include "../src/tun_device.hpp"
#include <iostream>
#include <sstream>

static void require(bool condition, const char* message) {
    if (not condition) throw std::runtime_error(message);
}

int main() {
    try {
        require(::if_nametoindex("pf06down") == 0, "test interface name already in use");
        tuntom::TunDevice tun("pf06down", 1500);
        const int fd = tun.fd();
        const unsigned index = ::if_nametoindex("pf06down");
        require(index != 0, "interface must exist");
        const int ctl = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        require(ctl >= 0, "interface control socket");
        const std::uint8_t packet[] {
            0x45, 0, 0, 20, 0, 0, 0, 0, 64, 1, 0, 0,
            198, 18, 0, 1, 198, 18, 0, 2
        };
        tun.set_up();
        for (int cycle = 0; cycle < 3; ++cycle) {
            require(tun.write_packet(packet, sizeof(packet)) == sizeof(packet), "UP write succeeds");
            ifreq request {};
            std::strcpy(request.ifr_name, "pf06down");
            require(::ioctl(ctl, SIOCGIFFLAGS, &request) == 0, "read interface flags");
            request.ifr_flags = static_cast<short>(request.ifr_flags & ~IFF_UP);
            require(::ioctl(ctl, SIOCSIFFLAGS, &request) == 0, "set DOWN");
            for (int count = 0; count < 32; ++count) {
                require(tun.write_packet(packet, sizeof(packet)) == -1 and errno == EIO,
                    "DOWN kernel write returns EIO");
                require(tun.fd() == fd and ::fcntl(fd, F_GETFD) >= 0, "DOWN preserves descriptor");
            }
            require(::if_nametoindex("pf06down") == index, "DOWN preserves interface");
            pollfd pending {tun.poll_fd(), POLLIN, 0};
            require(::poll(&pending, 1, 0) >= 0, "DOWN poll succeeds");
            tun.poll_events(pending.revents);
            require(tun.fd() == fd, "DOWN poll does not retire interface");
            require(::ioctl(ctl, SIOCGIFFLAGS, &request) == 0 and !(request.ifr_flags & IFF_UP),
                "device remains administratively DOWN until explicitly enabled");
            tun.set_up();
            require(tun.write_packet(packet, sizeof(packet)) == sizeof(packet), "UP resumes writes");
        }
        std::ostringstream stats;
        tun.write_stats(stats);
        require(stats.str().find("tun_endpoint_failures=0\n") != std::string::npos, "no permanent failure");
        ::close(ctl);
        std::cout << "PASS: real kernel TUN, 3 DOWN/UP cycles, 96 dropped writes, same fd/interface, resumed writes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
