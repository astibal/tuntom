#include "../src/packet_classifier.hpp"
#include "../src/control_dispatcher.hpp"
#include <fstream>
#include <cstdarg>
#include <iostream>


// Fault injection exercises failures without changing the production helper.
std::string fault;
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __wrap_read(int fd, void* data, size_t size) {

    if (fault == "read" || fault == "read-interrupted") {
        errno = fault == "read" ? EIO : EINTR; fault.clear(); return -1;
    }
    return __real_read(fd, data, size);
}
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {

    if (fault == "write-zero") { fault.clear(); return 0; }

    if (fault == "write" || fault == "write-interrupted") {
        errno = fault == "write" ? ENOSPC : EINTR; fault.clear(); return -1;
    }
    return __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat info{}; __real_fstat(fd, &info);

    if (fault == "fsync" || (fault == "directory-sync" && S_ISDIR(info.st_mode))) {
        fault.clear(); errno = EIO; return -1;
    }
    return __real_fsync(fd);
}
extern "C" int __wrap_renameat(int a, const char* b, int c, const char* d) {

    if (fault == "rename") { fault.clear(); errno = EIO; return -1; }
    return __real_renameat(a,b,c,d);
}
extern "C" int __wrap_fstat(int fd, struct stat* info) {

    if (fault == "stat") { fault.clear(); errno = EIO; return -1; }
    return __real_fstat(fd,info);
}
extern "C" int __wrap_openat(int fd, const char* path, int flags, ...) {

    if (fault == "open" || (fault == "create" && (flags & O_CREAT))) {
        fault.clear(); errno = EACCES; return -1;
    }

    if (flags & O_CREAT) {
        va_list args; va_start(args,flags); const auto mode = va_arg(args,int); va_end(args);
        return __real_openat(fd,path,flags,mode);
    }
    return __real_openat(fd,path,flags);
}

using namespace tuntom;
void require(bool value) { if (!value) throw std::runtime_error("assertion failed"); }
template<class Fn> void rejects(Fn fn) {
    bool failed = false;
    try { fn(); } catch (const std::runtime_error&) { failed = true; }
    require(failed);
}
int main() {
    char path[] = "/tmp/tuntom-state-test-XXXXXX";
    require(::mkdtemp(path));
    const std::string file = std::string(path) + "/classifier";
    const std::string a = "format 1\nclassify to [12]\n", b = "format 1\nclassify to [23]\n";
    try {
        ConfigState state(file);
        require(!state.read());
        ConfigState relative("relative");
        rejects([] { ConfigState("/"); });
        rejects([&] { ConfigState{}.save(a); });
        std::ofstream(file) << "# classifier disabled\n";
        auto c = PacketClassifier::from_file(file);
        require(c.control("state", "", []{}) == "configured=1\nsaved=1\nmatches_active=1\n");
        c.control("load-save", a, []{});
        require(state.read() == a);
        auto restarted = PacketClassifier::from_file(file);
        require(restarted.control("show", "", []{}) == a);
        restarted.control("load", b, []{});
        require(state.read() == a);
        require(restarted.control("state", "", []{}).find("matches_active=0") != std::string::npos);
        rejects([&] { restarted.control("load-save", "invalid", []{}); });
        require(state.read() == a);
        restarted.control("load-flush-save", b, []{});
        require(ConfigState(file + ".previous").read() == a);
        restarted.control("disable-save", "", []{});
        require(PacketClassifier::from_file(file).control("show", "", []{}) == "# classifier disabled\n");
        require(ConfigState(file + ".previous").read() == b);
        std::filesystem::remove(file);
        std::filesystem::create_symlink(file + ".previous", file);
        rejects([&] { state.save(a); });
        require(ConfigState(file + ".previous").read() == b);
        std::filesystem::remove(file);
        std::filesystem::create_directory(file);
        rejects([&] { state.read(); });
        std::filesystem::remove(file);
        std::ofstream(file) << "broken state";
        rejects([&] { PacketClassifier::from_file(file); });
        rejects([&] { state.save(std::string(1024 * 1024 + 1, 'x')); });
        rejects([&] { ConfigState(file + "/missing/state").read(); });
        rejects([&] { ConfigState(file + "/missing/state").save(a); });
        for (const auto* failure : {"read", "stat", "open"}) {
            fault = failure;
            rejects([&] { state.read(); });
        }
        fault = "read-interrupted";
        require(state.read() == "broken state");
        for (const auto* failure : {"write", "write-zero", "fsync", "rename", "create"}) {
            fault = failure;
            rejects([&] { state.save(a); });
            require(state.read() == "broken state");
        }
        fault = "write-interrupted";
        state.save(a);
        require(state.read() == a);
        fault = "directory-sync";
        rejects([&] { state.save(b); });
        require(state.read() == b); // Renamed state is visible; durability was not confirmed.
        std::ofstream(file) << std::string(1024 * 1024 + 1, 'x');
        rejects([&] { state.read(); });
        ControlDispatcher dispatcher;
        unsigned calls = 0;
        dispatcher.classifier = [&](auto&, auto&) { ++calls; return "ok"; };
        require(!dispatcher.execute({"classifier load-save 0", ""}, {1}).success);
        require(calls == 0);
        require(dispatcher.execute({"classifier load-save 0", ""}, ControlAccess::all()).success);
        require(calls == 1);
        require(!dispatcher.execute({"classifier disable-save 1", "x"}, ControlAccess::all()).success);
        std::filesystem::remove_all(path);
        std::cout << "PASS: restart, precedence, temporary changes, disable, backup, unsafe files, validation and permissions\n";
    } catch (...) { std::filesystem::remove_all(path); throw; }
}
