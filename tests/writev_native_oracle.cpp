/* Standalone Linux x86-64 writev ABI oracle. No DetSim code is exercised.
 * Signature/reference: Linux v6.12 fs/read_write.c and lib/iov_iter.c at
 * https://github.com/torvalds/linux/tree/v6.12 . Raw syscall arguments are
 * intentional: libc writev(int iovcnt) cannot express the high-bit probes.
 * Every case runs in its own fork, with a bounded alarm and fresh descriptor.
 * /dev/null cap probes neither allocate nor copy the advertised multi-GB data.
 */
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
enum class Kind { file, readonly_file, directory, epoll, pipe, broken_pipe, tcp, unix_stream, null_sink };
const char* kind_name(Kind k) {
    switch (k) {
    case Kind::file: return "regular-file";
    case Kind::readonly_file: return "readonly-regular-file";
    case Kind::directory: return "readonly-directory";
    case Kind::epoll: return "epoll";
    case Kind::pipe: return "nonblocking-pipe";
    case Kind::broken_pipe: return "nonblocking-pipe-no-readers";
    case Kind::tcp: return "connected-loopback-TCP";
    case Kind::unix_stream: return "connected-AF_UNIX-stream";
    case Kind::null_sink: return "dev-null-no-data-copy";
    }
    return "unknown";
}
enum class Input { valid, zero, null_meta, bad_meta, cross_meta, oversized_len,
    late_oversized_len, negative_unreadable_base, negative_cross_meta, bad_payload_late_oversized, zero_bad_base, zero_kernel_base, fault_first,
    fault_second, data_cross_page, page_then_fault, large_fault, sum_overflow, single_max, high_metadata, high_zero, cap };
constexpr long observed_only = LONG_MIN;
struct Case { const char* name; Input input; unsigned long count; long expected; bool bad_fd = false; bool fd_alias = false; };
const Case cases[] = {
    {"two-readable-segments", Input::valid, 2, 6},
    {"bad-fd-before-invalid-metadata", Input::bad_meta, 1, -EBADF, true},
    {"bad-fd-before-count-overflow", Input::bad_meta, ULONG_MAX, -EBADF, true},
    {"bad-fd-before-zero-count", Input::bad_meta, 0, -EBADF, true},
    {"zero-count-null-metadata", Input::null_meta, 0, 0},
    {"zero-count-bad-metadata", Input::bad_meta, 0, 0},
    {"negative-count-all-bits", Input::valid, ULONG_MAX, -EINVAL},
    {"negative-count-low32", Input::valid, 0xffffffffUL, -EINVAL},
    {"high32-count-alias-zero", Input::bad_meta, 0x100000000UL, 0},
    {"high32-count-alias-one", Input::valid, 0x100000001UL, 3},
    {"high32-count-all-ones-alias-one", Input::valid, 0xffffffff00000001UL, 3},
    {"high32-fd-alias", Input::valid, 1, 3, false, true},
    {"1024-zero-segments", Input::zero, 1024, 0},
    {"1025-segments-rejected", Input::zero, 1025, -EINVAL},
    {"high32-count-alias-1024", Input::zero, 0x100000400UL, 0},
    {"high32-count-alias-1025", Input::zero, 0x100000401UL, -EINVAL},
    {"1025-before-bad-metadata", Input::bad_meta, 1025, -EINVAL},
    {"null-metadata", Input::null_meta, 1, -EFAULT},
    {"bad-metadata", Input::bad_meta, 1, -EFAULT},
    {"metadata-crosses-protected-page", Input::cross_meta, 2, -EFAULT},
    {"segment-exceeds-SSIZE_MAX", Input::oversized_len, 1, -EINVAL},
    {"late-segment-exceeds-SSIZE_MAX", Input::late_oversized_len, 2, -EINVAL},
    {"unreadable-base-before-same-item-negative-length", Input::negative_unreadable_base, 1, -EFAULT},
    {"negative-length-before-later-metadata-fault", Input::negative_cross_meta, 2, -EINVAL},
    {"later-negative-length-before-first-payload-range-fault", Input::bad_payload_late_oversized, 2, -EINVAL},
    {"zero-length-unmapped-base", Input::zero_bad_base, 2, 3},
    {"zero-length-kernel-base", Input::zero_kernel_base, 2, -EFAULT},
    {"faulting-first-segment", Input::fault_first, 2, observed_only},
    {"faulting-second-segment", Input::fault_second, 2, observed_only},
    {"single-segment-crosses-protected-page", Input::data_cross_page, 1, observed_only},
    {"page-segment-then-fault", Input::page_then_fault, 2, observed_only},
    {"large-length-fault-priority", Input::large_fault, 1, observed_only},
    {"sum-overflow-fault-priority", Input::sum_overflow, 2, -EFAULT},
};
struct Observation {
    long result = 0;
    int setup_error = 0, captured_errno = 0, skipped_errno = 0;
    int64_t offset = -1, size = -1;
    uint64_t bytes = 0, hash = 14695981039346656037ull;
    int mtime_changed = 0;
};
static_assert(sizeof(Observation) < 512);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int descriptor_count() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int n = 0;
    while (auto* e = readdir(dir)) if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) ++n;
    return closedir(dir) ? -1 : n;
}
void nonblocking(int fd) { require(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0, "nonblocking"); }
struct Endpoint { int fd = -1, reader = -1; };
Endpoint endpoint(Kind kind) {
    Endpoint e;
    if (kind == Kind::file || kind == Kind::readonly_file) {
        int created = open("file", O_CREAT | O_EXCL | O_RDWR, 0600);
        require(created >= 0, "create file");
        timespec times[2]{{1, 0}, {1, 0}};
        require(futimens(created, times) == 0, "set initial mtime");
        if (kind == Kind::file) e.fd = created;
        else { require(close(created) == 0, "close initial file"); e.fd = open("file", O_RDONLY); }
    } else if (kind == Kind::directory) e.fd = open(".", O_RDONLY | O_DIRECTORY);
    else if (kind == Kind::epoll) e.fd = epoll_create1(0);
    else if (kind == Kind::null_sink) e.fd = open("/dev/null", O_WRONLY);
    else if (kind == Kind::pipe || kind == Kind::broken_pipe) {
        int pair[2]; require(pipe2(pair, O_NONBLOCK) == 0, "pipe"); e.fd = pair[1]; e.reader = pair[0];
        if (kind == Kind::broken_pipe) { require(close(e.reader) == 0, "close pipe reader"); e.reader = -1; }
    } else if (kind == Kind::unix_stream) {
        int pair[2]; require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "unix stream");
        e.fd = pair[0]; e.reader = pair[1]; nonblocking(e.fd); nonblocking(e.reader);
    } else {
        int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(listener >= 0, "TCP listener");
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(listener, 1) == 0, "TCP listen");
        socklen_t size = sizeof addr;
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &size) == 0, "TCP port");
        e.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(e.fd >= 0 && connect(e.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0, "TCP connect");
        e.reader = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        require(e.reader >= 0 && close(listener) == 0, "TCP accept");
        nonblocking(e.fd); nonblocking(e.reader);
    }
    require(e.fd >= 0, "endpoint descriptor");
    return e;
}
void collect(int fd, Observation& out, bool regular) {
    std::array<unsigned char, 4096> data{};
    for (;;) {
        ssize_t n = regular ? pread(fd, data.data(), data.size(), out.bytes) : read(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLIN, 0};
            require(poll(&p, 1, 1000) > 0, "drain timed out");
            continue;
        }
        require(n >= 0, "drain read");
        if (!n) break;
        out.bytes += static_cast<uint64_t>(n);
        require(out.bytes <= 65536, "unexpected large data commit");
        for (ssize_t i = 0; i < n; ++i) { out.hash ^= data[static_cast<size_t>(i)]; out.hash *= 1099511628211ull; }
    }
}
Observation run_child(Kind kind, const Case& c, size_t page) {
    Observation out;
    auto e = endpoint(kind);
    auto* metadata = static_cast<unsigned char*>(mmap(nullptr, 2 * page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<unsigned char*>(mmap(nullptr, 2 * page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(metadata != MAP_FAILED && data != MAP_FAILED, "probe mappings");
    std::memset(data, 'P', page);
    require(mprotect(metadata + page, page, PROT_NONE) == 0 &&
            mprotect(data + page, page, PROT_NONE) == 0, "guard pages");
    char a[] = "ABC", b[] = "DEF";
    std::array<iovec, 1025> vec{};
    vec[0] = {a, 3}; vec[1] = {b, 3};
    const iovec* arg = vec.data();
    switch (c.input) {
    case Input::valid: break;
    case Input::zero: for (auto& v : vec) v = {reinterpret_cast<void*>(1), 0}; break;
    case Input::null_meta: arg = nullptr; break;
    case Input::bad_meta: arg = reinterpret_cast<iovec*>(1); break;
    case Input::cross_meta:
        std::memcpy(metadata + page - sizeof(iovec), vec.data(), sizeof(iovec));
        arg = reinterpret_cast<iovec*>(metadata + page - sizeof(iovec)); break;
    case Input::oversized_len: vec[0] = {reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX) + 1}; break;
    case Input::late_oversized_len: vec[1] = {reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX) + 1}; break;
    case Input::negative_unreadable_base: {
        require(mprotect(metadata, page, PROT_NONE) == 0 &&
                mprotect(metadata + page, page, PROT_READ | PROT_WRITE) == 0, "inverted metadata guard");
        const size_t negative = static_cast<size_t>(SSIZE_MAX) + 1;
        std::memcpy(metadata + page, &negative, sizeof negative);
        arg = reinterpret_cast<iovec*>(metadata + page - sizeof(void*)); break;
    }
    case Input::negative_cross_meta:
        vec[0].iov_len = static_cast<size_t>(SSIZE_MAX) + 1;
        std::memcpy(metadata + page - sizeof(iovec), vec.data(), sizeof(iovec));
        arg = reinterpret_cast<iovec*>(metadata + page - sizeof(iovec)); break;
    case Input::bad_payload_late_oversized:
        vec[0] = {reinterpret_cast<void*>(UINTPTR_MAX), 1};
        vec[1] = {b, static_cast<size_t>(SSIZE_MAX) + 1}; break;
    case Input::zero_bad_base: vec[0] = {reinterpret_cast<void*>(1), 0}; vec[1] = {b, 3}; break;
    case Input::zero_kernel_base: vec[0] = {reinterpret_cast<void*>(UINTPTR_MAX), 0}; vec[1] = {b, 3}; break;
    case Input::fault_first: vec[0] = {data + page, 1}; break;
    case Input::fault_second: vec[1] = {data + page, 1}; break;
    case Input::data_cross_page: vec[0] = {data, page + 1}; break;
    case Input::page_then_fault: vec[0] = {data, page}; vec[1] = {data + page, 1}; break;
    case Input::large_fault: vec[0] = {data + page, 0x80001000ULL}; break;
    case Input::sum_overflow:
        vec[0] = {reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX)};
        vec[1] = vec[0]; break;
    case Input::single_max:
        vec[0] = {reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX)};
        vec[1] = {nullptr, 0}; break;
    case Input::high_zero:
        vec[0] = {reinterpret_cast<void*>(0x00007ffffffff001ULL), 0}; break;
    case Input::high_metadata: {
        void* wanted = reinterpret_cast<void*>(0x00007fffffffe000ULL);
        void* mapped = mmap(wanted, page, PROT_READ | PROT_WRITE,
            MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) { out.skipped_errno = errno; return out; }
        require(mapped == wanted, "fixed high metadata address");
        const iovec negative{reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX) + 1};
        std::memcpy(mapped, &negative, sizeof negative);
        arg = static_cast<const iovec*>(mapped); break;
    }
    case Input::cap:
        vec[0] = {reinterpret_cast<void*>(0x10000), 0x80001000ULL};
        vec[1] = vec[0]; break;
    }
    unsigned long fd_arg = c.bad_fd ? ULONG_MAX : static_cast<unsigned long>(e.fd);
    if (c.fd_alias) fd_arg |= 0x100000000UL;
    errno = 0;
    const long returned = syscall(SYS_writev, fd_arg, arg, c.count);
    out.captured_errno = returned < 0 ? errno : 0;
    out.result = returned < 0 ? -errno : returned;
    if (kind == Kind::file || kind == Kind::readonly_file) {
        struct stat st{}; require(fstat(e.fd, &st) == 0, "stat result file");
        out.offset = lseek(e.fd, 0, SEEK_CUR); out.size = st.st_size;
        out.mtime_changed = st.st_mtim.tv_sec != 1 || st.st_mtim.tv_nsec != 0;
        collect(e.fd, out, true);
    } else if (e.reader >= 0) {
        if (kind == Kind::tcp || kind == Kind::unix_stream) require(shutdown(e.fd, SHUT_WR) == 0, "shutdown sender");
        require(close(e.fd) == 0, "close sender"); e.fd = -1;
        collect(e.reader, out, false);
    }
    if (e.fd >= 0) require(close(e.fd) == 0, "close endpoint");
    if (e.reader >= 0) require(close(e.reader) == 0, "close peer");
    require(munmap(metadata, 2 * page) == 0 && munmap(data, 2 * page) == 0, "unmap probes");
    return out;
}
Observation isolated(Kind kind, const Case& c, size_t page) {
    int report[2]; require(pipe2(report, O_CLOEXEC) == 0, "report pipe");
    require(unlink("file") == 0 || errno == ENOENT, "remove prior fixture file");
    const pid_t owner = getpid();
    const pid_t child = fork(); require(child >= 0, "fork case");
    if (!child) {
        close(report[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner) _exit(93);
        alarm(5); signal(SIGPIPE, SIG_IGN);
        Observation out;
        try { out = run_child(kind, c, page); }
        catch (const std::exception& error) { std::fprintf(stderr, "%s/%s: %s\n", kind_name(kind), c.name, error.what()); out.setup_error = 1; }
        const ssize_t n = write(report[1], &out, sizeof out);
        close(report[1]); _exit(n == static_cast<ssize_t>(sizeof out) ? 0 : 94);
    }
    close(report[1]);
    pollfd p{report[0], POLLIN, 0};
    const int polled = poll(&p, 1, 7000);
    Observation out;
    const ssize_t n = polled > 0 ? read(report[0], &out, sizeof out) : -1;
    close(report[0]);
    if (n != static_cast<ssize_t>(sizeof out)) kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    require(n == static_cast<ssize_t>(sizeof out) && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !out.setup_error,
            "case failed, signaled or timed out");
    return out;
}
long expected(Kind kind, const Case& c) {
    if (c.bad_fd) return -EBADF;
    if (kind == Kind::readonly_file || kind == Kind::directory) return -EBADF;
    if (kind == Kind::epoll) return -EINVAL;
    if (kind == Kind::broken_pipe && c.expected > 0) return -EPIPE;
    return c.expected;
}
}
int main() {
    const auto cwd_fd = open(".", O_RDONLY | O_DIRECTORY);
    char directory[] = "/tmp/detsim-writev-native-XXXXXX";
    try {
        require(cwd_fd >= 0 && mkdtemp(directory) && chdir(directory) == 0, "disposable directory");
        const int descriptors = descriptor_count();
        const long page_size = sysconf(_SC_PAGESIZE);
        require(page_size > 0, "page size");
        utsname host{}; require(uname(&host) == 0, "kernel identity");
        std::printf("{\"kernel\":\"%s\",\"page_size\":%ld,\"observations\":[\n", host.release, page_size);
        size_t count = 0, measurements = 0;
        const std::array kinds{Kind::file, Kind::readonly_file, Kind::directory, Kind::epoll,
            Kind::pipe, Kind::broken_pipe, Kind::tcp, Kind::unix_stream, Kind::null_sink};
        for (Kind kind : kinds) for (const auto& c : cases) {
            const auto out = isolated(kind, c, static_cast<size_t>(page_size));
            const long baseline = expected(kind, c);
            if (baseline != observed_only && out.result != baseline) {
                std::fprintf(stderr, "%s/%s expected %ld got %ld\n", kind_name(kind), c.name, baseline, out.result);
                throw std::runtime_error("native ABI baseline mismatch");
            }
            if (baseline == observed_only) ++measurements;
            if (baseline != observed_only &&
                (kind == Kind::file || kind == Kind::pipe || kind == Kind::tcp || kind == Kind::unix_stream)) {
                const auto written = baseline > 0 ? static_cast<uint64_t>(baseline) : 0;
                require(out.bytes == written, "returned count differs from destination bytes");
                const char* payload = c.input == Input::zero_bad_base ? "DEF" : "ABCDEF";
                uint64_t hash = 14695981039346656037ull;
                for (uint64_t byte = 0; byte < written; ++byte) {
                    hash ^= static_cast<unsigned char>(payload[byte]); hash *= 1099511628211ull;
                }
                require(out.hash == hash, "concatenated vector payload differs");
                if (kind == Kind::file)
                    require(out.offset == static_cast<int64_t>(written) && out.size == out.offset &&
                            out.mtime_changed == (written > 0), "regular file offset/size/mtime differs");
            }
            // Invalid vector metadata cannot publish an earlier vector's bytes.
            if (c.input == Input::cross_meta || c.input == Input::late_oversized_len ||
                c.input == Input::oversized_len || c.input == Input::negative_unreadable_base ||
                c.input == Input::negative_cross_meta ||
                c.input == Input::bad_payload_late_oversized || c.input == Input::bad_meta || c.input == Input::null_meta)
                require(out.bytes == 0 && out.size <= 0, "metadata failure mutated destination");
            std::printf("%s{\"fd_kind\":\"%s\",\"case\":\"%s\",\"count\":%lu,\"result\":%ld,\"errno\":%d,"
                "\"offset\":%lld,\"size\":%lld,\"committed_bytes\":%llu,\"hash\":\"%016llx\",\"mtime_changed\":%d,\"asserted_baseline\":%s}",
                count++ ? ",\n" : "", kind_name(kind), c.name, c.count, out.result, out.captured_errno,
                static_cast<long long>(out.offset), static_cast<long long>(out.size),
                static_cast<unsigned long long>(out.bytes), static_cast<unsigned long long>(out.hash),
                out.mtime_changed, baseline == observed_only ? "false" : "true");
        }
        // /dev/null's write_iter consumes counts without accessing payload bytes.
        // This establishes MAX_RW_COUNT clipping without allocating/writing 2 GiB.
        for (unsigned long segments : {1UL, 2UL}) {
            const Case c{"dev-null-MAX_RW_COUNT-cap", Input::cap, segments, 0x7ffff000L};
            const auto out = isolated(Kind::null_sink, c, static_cast<size_t>(page_size));
            require(out.result == c.expected, "MAX_RW_COUNT clipping differs");
            std::printf(",\n{\"fd_kind\":\"dev-null-no-data-copy\",\"case\":\"%s\",\"count\":%lu,\"result\":%ld,\"asserted_baseline\":true}", c.name, segments, out.result);
            ++count;
        }
        // Single-iov import_ubuf clips before access_ok, unlike the multi-iov
        // raw-range pass. These /dev/null calls never copy the huge payload.
        const Case targeted[] = {
            {"single-SSIZE_MAX-clipped-before-range", Input::single_max, 1, 0x7ffff000L},
            {"multiple-SSIZE_MAX-range-before-clip", Input::single_max, 2, -EFAULT},
            {"zero-length-base-in-user-guard", Input::high_zero, 1, -EFAULT},
            {"metadata-span-before-negative-length-count256", Input::high_metadata, 256, -EINVAL},
            {"metadata-span-before-negative-length-count257", Input::high_metadata, 257, -EFAULT},
            {"metadata-span-before-negative-length-count1024", Input::high_metadata, 1024, -EFAULT},
        };
        size_t skipped = 0;
        for (const auto& c : targeted) {
            const auto out = isolated(Kind::null_sink, c, static_cast<size_t>(page_size));
            if (out.skipped_errno) ++skipped;
            else require(out.result == c.expected, "targeted import boundary differs");
            std::printf(",\n{\"fd_kind\":\"dev-null-no-data-copy\",\"case\":\"%s\",\"count\":%lu,\"result\":%ld,"
                "\"expected\":%ld,\"skipped_errno\":%d,\"asserted_baseline\":%s}",
                c.name, c.count, out.result, c.expected, out.skipped_errno, out.skipped_errno ? "false" : "true");
            ++count;
        }
        require(descriptor_count() == descriptors, "parent descriptor leak");
        errno = 0; require(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD, "unreaped child");
        require(unlink("file") == 0 || errno == ENOENT, "final fixture cleanup");
        require(fchdir(cwd_fd) == 0 && close(cwd_fd) == 0 && rmdir(directory) == 0, "directory cleanup");
        std::printf("\n],\"passed\":true,\"cases\":%zu,\"observation_only_cases\":%zu,"
            "\"children_reaped\":%zu,\"skipped_fixed_address_cases\":%zu,\"parent_fd_delta\":0,\"unverified\":\"large real destinations, blocked/full endpoints, UDP, filesystem-specific fault guarantees\"}\n", count, measurements, count, skipped);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "writev native oracle: %s\n", error.what());
        unlink("file");
        if (cwd_fd >= 0) { fchdir(cwd_fd); close(cwd_fd); }
        rmdir(directory); return 1;
    }
}
