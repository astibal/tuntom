#pragma once

#include "common.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tuntom {

inline void harden_process_before_privilege_drop() {
    ::umask(0077);

    rlimit core_limit {};
    core_limit.rlim_cur = 0;
    core_limit.rlim_max = 0;

    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        throw std::runtime_error(
            "setrlimit(RLIMIT_CORE) failed: " +
            std::string(std::strerror(errno)));
    }
}

inline void verify_supplementary_groups(gid_t expected_gid) {
    const int group_count = ::getgroups(0, nullptr);

    if (group_count < 0) {
        throw std::runtime_error(
            "getgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    std::vector<gid_t> groups(
        static_cast<std::size_t>(group_count));

    if (
        group_count > 0 and
        ::getgroups(group_count, groups.data()) < 0) {

        throw std::runtime_error(
            "getgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    for (const gid_t group_id : groups) {
        if (group_id == 0) {
            throw std::runtime_error(
                "Privilege drop left root supplementary group");
        }
    }

    if (
        not groups.empty() and
        std::find(
            groups.begin(),
            groups.end(),
            expected_gid) == groups.end()) {

        throw std::runtime_error(
            "Runtime group missing from supplementary groups");
    }
}

inline void drop_privileges() {
    if (::geteuid() != 0) {
        throw std::runtime_error(
            "tuntom must start as root in order to initialize networking "
            "and drop privileges");
    }

    harden_process_before_privilege_drop();

    passwd* user = ::getpwnam(runtime_user);
    if (user == nullptr) {
        throw std::runtime_error(
            std::string("Runtime user does not exist: ") + runtime_user);
    }

    group* runtime_group_entry = ::getgrnam(runtime_group);
    if (runtime_group_entry == nullptr) {
        throw std::runtime_error(
            std::string("Runtime group does not exist: ") + runtime_group);
    }

    const uid_t uid = user->pw_uid;
    const gid_t gid = runtime_group_entry->gr_gid;

    if (::initgroups(runtime_user, gid) != 0) {
        throw std::runtime_error(
            "initgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    if (::setresgid(gid, gid, gid) != 0) {
        throw std::runtime_error(
            "setresgid() failed: " +
            std::string(std::strerror(errno)));
    }

    if (::setresuid(uid, uid, uid) != 0) {
        throw std::runtime_error(
            "setresuid() failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::prctl(
            PR_SET_NO_NEW_PRIVS,
            1,
            0,
            0,
            0) != 0) {

        throw std::runtime_error(
            "prctl(PR_SET_NO_NEW_PRIVS) failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::prctl(
            PR_SET_DUMPABLE,
            0,
            0,
            0,
            0) != 0) {

        throw std::runtime_error(
            "prctl(PR_SET_DUMPABLE) failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::getuid() != uid or
        ::geteuid() != uid or
        ::getgid() != gid or
        ::getegid() != gid) {

        throw std::runtime_error(
            "Privilege drop verification failed");
    }

    verify_supplementary_groups(gid);

    log_info(
        std::string("Privileges dropped and hardened as ") +
        runtime_user + ":" + runtime_group);
}

} // namespace tuntom
