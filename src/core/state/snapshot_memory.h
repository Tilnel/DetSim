#pragma once

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace detsim {
// Snapshot-only access to hidden bytes of admitted private anonymous guards.
// Guest syscall copies must continue respecting the tracee's permissions.
class GuardMemoryReader {
    pid_t pid_;
    int fd_ = -1;
public:
    explicit GuardMemoryReader(pid_t pid) : pid_(pid) {}
    ~GuardMemoryReader() { if (fd_ >= 0) close(fd_); }
    GuardMemoryReader(const GuardMemoryReader&) = delete;
    GuardMemoryReader& operator=(const GuardMemoryReader&) = delete;

    void read(uint64_t address, void* data, size_t size) {
        const auto limit = uint64_t(std::numeric_limits<off_t>::max());
        if (size > size_t(SSIZE_MAX) || address > limit || size > limit - address)
            throw std::runtime_error("Invalid guard snapshot memory extent");
        if (fd_ < 0) {
            const auto path = "/proc/" + std::to_string(pid_) + "/mem";
            fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0) throw std::runtime_error("Cannot open guard snapshot memory");
        }
        ssize_t count;
        do { count = pread(fd_, data, size, off_t(address)); }
        while (count < 0 && errno == EINTR);
        if (count < 0 || size_t(count) != size)
            throw std::runtime_error("Guard snapshot memory read failed or short");
    }
};
} // namespace detsim
