#pragma once

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tuntom {

// Save back to the file used at startup. CONTROL never supplies a path.
// The caller validates the candidate before saving and activates it afterwards.
class ConfigState {
    struct File {
        int fd;
        // Take ownership of a descriptor, including -1 after a failed open.
        explicit File(int value) : fd(value) {}
        // Close on every exit path without replacing an exception already in flight.
        ~File() {

            if (fd >= 0) ::close(fd);
        }
        File(const File&) = delete;
        File& operator=(const File&) = delete;
    };

    std::string path_;
    static constexpr std::size_t limit = 1024 * 1024;

    // Translate the errno from the immediately preceding syscall into a CONTROL error.
    [[noreturn]] static void fail(const std::string& action) {
        throw std::runtime_error("configuration state " + action + ": " + std::strerror(errno));
    }

    // Read a bounded regular file relative to an already opened directory.
    // Only a missing file returns nullopt; symlinks, special files and I/O errors fail.
    static std::optional<std::string> read_at(int directory, const std::string& name) {
        File file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));

        if (file.fd < 0) {
            if (errno == ENOENT) return std::nullopt;
            fail("open");
        }

        struct stat info{};

        if (::fstat(file.fd, &info) < 0) fail("stat");
        const bool regular_file = S_ISREG(info.st_mode);
        const bool valid_size = info.st_size >= 0 && info.st_size <= static_cast<off_t>(limit);

        if (!regular_file || !valid_size) {
            throw std::runtime_error("configuration state must be a regular file of at most 1 MiB");
        }

        std::string result;
        char buffer[4096];
        for (;;) {
            const auto count = ::read(file.fd, buffer, sizeof(buffer));

            if (count < 0) {
                const bool interrupted = errno == EINTR;

                if (interrupted) continue;
                fail("read");
            }
            const bool reached_end = count == 0;

            if (reached_end) break;

            result.append(buffer, static_cast<std::size_t>(count));

            if (result.size() > limit) throw std::runtime_error("configuration state exceeds 1 MiB");
        }
        return result;
    }

    // Write and sync a private temporary file, then atomically replace one directory entry.
    // The caller syncs the directory after the backup and configuration replacements.
    static void replace_at(int directory, const std::string& name, const std::string& text) {
        // O_EXCL avoids following an attacker-created temporary symlink.
        const auto temporary = name + ".tmp-" + std::to_string(::getpid());
        constexpr int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
        File file(::openat(directory, temporary.c_str(), flags, 0600));

        if (file.fd < 0) fail("create temporary file");

        try {
            std::size_t offset = 0;
            while (offset < text.size()) {
                const auto count = ::write(file.fd, text.data() + offset, text.size() - offset);

                if (count < 0) {
                    const bool interrupted = errno == EINTR;

                    if (interrupted) continue;
                    fail("write");
                }
                const bool no_progress = count == 0;

                if (no_progress) throw std::runtime_error("configuration state short write");
                offset += static_cast<std::size_t>(count);
            }

            if (::fsync(file.fd) < 0) fail("fsync");

            if (::renameat(directory, temporary.c_str(), directory, name.c_str()) < 0) fail("rename");
        } catch (...) {
            ::unlinkat(directory, temporary.c_str(), 0);
            throw;
        }
    }

public:
    // Bind persistence to the startup configuration path; empty means runtime-only.
    // Resolve relative paths now so later changes of working directory cannot redirect writes.
    explicit ConfigState(std::string path = {}) : path_(std::move(path)) {

        if (path_.empty()) return;

        const std::filesystem::path file(path_);
        const bool missing_name = file.filename().empty() || file.filename() == "." || file.filename() == "..";

        if (missing_name) throw std::runtime_error("configuration requires a file path");
        // Preserve support for relative --rules-file and --classifier-file paths.
        path_ = std::filesystem::absolute(file).string();
    }

    // Read the configured file without creating it. Missing/unconfigured is distinct from
    // an unreadable or unsafe file, which must be reported rather than silently ignored.
    std::optional<std::string> read() const {

        if (path_.empty()) return std::nullopt;

        const std::filesystem::path path(path_);
        File directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));

        if (directory.fd < 0) fail("open directory");
        return read_at(directory.fd, path.filename().string());
    }

    // Report CONTROL key/value fields and compare disk content with the active export.
    // Equality is textual; this does not parse or activate configuration.
    std::string status(const std::string& active) const {
        const auto saved = read();
        const bool configured = !path_.empty();
        const bool matches_active = saved && *saved == active;

        return "configured=" + std::to_string(configured) +
               "\nsaved=" + std::to_string(saved.has_value()) +
               "\nmatches_active=" + std::to_string(matches_active) + "\n";
    }

    // Preserve old content as .previous, then durably replace the configured file.
    // The caller must validate text first and activate it only after this returns.
    // Failure after rename can leave new disk content despite unchanged runtime.
    void save(const std::string& text) const {

        if (path_.empty()) {
            throw std::runtime_error(
                "persistence unavailable: configure --rules-file or --classifier-file first; runtime unchanged");
        }

        if (text.size() > limit) throw std::runtime_error("configuration state exceeds 1 MiB");

        const std::filesystem::path path(path_);
        File directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));

        if (directory.fd < 0) fail("open directory");

        // Keep the previous saved version before replacing the active state file.
        const auto name = path.filename().string();
        const auto previous = read_at(directory.fd, name);

        if (previous) replace_at(directory.fd, name + ".previous", *previous);
        replace_at(directory.fd, name, text);

        // Rename has happened: report uncertainty rather than claiming rollback.

        if (::fsync(directory.fd) < 0) {
            throw std::runtime_error(
                "state renamed but directory fsync failed; saved durability uncertain, "
                "runtime unchanged; inspect state before retry");
        }
    }
};
} // namespace tuntom
