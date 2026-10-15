/* Standalone Linux EOF-backing oracle; no DetSim implementation is exercised. */
#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <linux/memfd.h>

namespace {
constexpr unsigned max_pages = 4;
enum class Access { read, write, execute };
enum class Result : int { success, signal, setup_error, ready };
enum class Stage : int { map, fill, protect, overlay, close_fd, permissions, access, cleanup };
struct Scenario {
    size_t file_size, file_offset;
    unsigned pages;
    int protection;
    Access access;
    size_t probe;
};
struct Observation {
    Result result = Result::success;
    Stage stage = Stage::map;
    int error = 0, signal = 0, code = 0;
    int64_t address_offset = -1;
    int value = 0, descriptor_delta = 0;
    uint64_t byte_hash = 14695981039346656037ull;
    std::array<int, max_pages> permissions{};
};
static_assert(sizeof(Observation) <= 512, "report must fit the POSIX minimum PIPE_BUF");
volatile sig_atomic_t report_fd = -1;
volatile sig_atomic_t mapping_low = 0, mapping_high = 0;
Observation observation;

void emit(const Observation &result) {
    // Each fixed-size pipe write fits PIPE_BUF and is async-signal-safe.
    const ssize_t written = write(report_fd, &result, sizeof(result));
    if (written != static_cast<ssize_t>(sizeof(result))) _exit(90);
}
[[noreturn]] void report(const Observation &result = observation) {
    emit(result);
    _exit(0);
}
void fault(int signal, siginfo_t *info, void *) {
    // The handler never touches the setup observation or heap/stdio state.
    Observation result{};
    result.result = Result::signal;
    result.stage = Stage::access;
    result.signal = signal;
    result.code = info->si_code;
    const uintptr_t base = static_cast<uint32_t>(mapping_low) |
        (static_cast<uint64_t>(static_cast<uint32_t>(mapping_high)) << 32);
    const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
    result.address_offset = address >= base ? static_cast<int64_t>(address - base) : -1;
    report(result);
}
[[noreturn]] void fail(Stage stage, int error) {
    observation.result = Result::setup_error;
    observation.stage = stage;
    observation.error = error;
    report();
}
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int descriptor_count() {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (auto *entry = readdir(directory))
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    if (closedir(directory)) return -1;
    return count; // Includes the temporary directory descriptor in every count.
}
uint8_t file_byte(size_t offset, size_t page_size) {
    constexpr uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3}; // x86-64: mov eax,42; ret
    if (offset % page_size < sizeof(code)) return code[offset % page_size];
    return static_cast<uint8_t>((offset * 29 + offset / page_size * 17 + 11) & 255);
}
size_t valid_bytes(const Scenario &scenario) {
    return scenario.file_offset < scenario.file_size ? scenario.file_size - scenario.file_offset : 0;
}
size_t readable_extent(const Scenario &scenario, size_t page_size) {
    const size_t bytes = valid_bytes(scenario);
    return std::min(scenario.pages * page_size, ((bytes + page_size - 1) / page_size) * page_size);
}
int regular_file(const Scenario &scenario, size_t page_size) {
    char path[] = "/tmp/detsim-eof-native-XXXXXX";
    const int fd = mkstemp(path);
    require(fd >= 0, "mkstemp failed");
    if (unlink(path)) { close(fd); throw std::runtime_error("unlink temporary file failed"); }
    if (ftruncate(fd, static_cast<off_t>(scenario.file_size))) {
        close(fd); throw std::runtime_error("ftruncate failed");
    }
    std::vector<uint8_t> bytes(scenario.file_size);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = file_byte(i, page_size);
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = pwrite(fd, bytes.data() + written, bytes.size() - written, written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); throw std::runtime_error("pwrite failed"); }
        written += n;
    }
    return fd;
}
bool closed(int fd) {
    errno = 0;
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}
void inspect_permissions(uint8_t *mapping, const Scenario &scenario, size_t page_size) {
    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (!maps) fail(Stage::permissions, errno);
    observation.permissions.fill(-1);
    char line[1024];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long first = 0, last = 0;
        char flags[5]{};
        if (std::sscanf(line, "%lx-%lx %4s", &first, &last, flags) != 3) continue;
        for (unsigned i = 0; i < scenario.pages; ++i) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(mapping + i * page_size);
            if (address < first || address >= last) continue;
            observation.permissions[i] = (flags[0] == 'r' ? PROT_READ : 0) |
                (flags[1] == 'w' ? PROT_WRITE : 0) | (flags[2] == 'x' ? PROT_EXEC : 0);
            if (flags[3] != 'p') fail(Stage::permissions, EINVAL);
        }
    }
    if (std::fclose(maps)) fail(Stage::permissions, errno);
    for (unsigned i = 0; i < scenario.pages; ++i)
        if (observation.permissions[i] != scenario.protection) fail(Stage::permissions, EACCES);
}
[[noreturn]] void child(const Scenario &scenario, size_t page_size, int fd, bool synthetic, int pipe_fd) {
    report_fd = pipe_fd;
    observation = {};
    struct rlimit core{0, 0};
    if (setrlimit(RLIMIT_CORE, &core)) fail(Stage::map, errno);
    struct sigaction action{};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask)) fail(Stage::map, errno);
    for (int signal : {SIGBUS, SIGSEGV, SIGILL})
        if (sigaction(signal, &action, nullptr)) fail(Stage::map, errno);
    struct sigaction timeout_action{};
    timeout_action.sa_handler = SIG_DFL;
    if (sigemptyset(&timeout_action.sa_mask) || sigaction(SIGALRM, &timeout_action, nullptr) ||
        sigprocmask(SIG_SETMASK, &timeout_action.sa_mask, nullptr)) fail(Stage::map, errno);
    alarm(5);
    const int before_fds = descriptor_count();
    if (before_fds < 0) fail(Stage::close_fd, errno);
    const size_t length = scenario.pages * page_size;
    if (!length || scenario.pages > max_pages) fail(Stage::map, EINVAL);
    auto *mapping = static_cast<uint8_t *>(mmap(nullptr, length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | (synthetic ? MAP_ANONYMOUS : 0), synthetic ? -1 : fd,
        synthetic ? 0 : static_cast<off_t>(scenario.file_offset)));
    if (mapping == MAP_FAILED) fail(Stage::map, errno);
    const uint64_t base = reinterpret_cast<uintptr_t>(mapping);
    mapping_low = static_cast<sig_atomic_t>(static_cast<uint32_t>(base));
    mapping_high = static_cast<sig_atomic_t>(static_cast<uint32_t>(base >> 32));
    const size_t available = std::min(valid_bytes(scenario), length);
    const size_t extent = readable_extent(scenario, page_size);
    if (synthetic) {
        size_t copied = 0;
        while (copied < available) {
            const ssize_t n = pread(fd, mapping + copied, available - copied, scenario.file_offset + copied);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fail(Stage::fill, n < 0 ? errno : EIO);
            copied += n;
        }
    }
    // Inspect valid file bytes and the partial EOF page before applying protections.
    // Whole pages beyond EOF are never read here.
    observation.stage = Stage::fill;
    for (size_t i = 0; i < extent; ++i) {
        const uint8_t expected = i < available ? file_byte(scenario.file_offset + i, page_size) : 0;
        if (mapping[i] != expected) fail(Stage::fill, EIO);
        observation.byte_hash = (observation.byte_hash ^ mapping[i]) * 1099511628211ull;
    }
    if (mprotect(mapping, length, scenario.protection)) fail(Stage::protect, errno);
    if (synthetic && extent < length) {
        const int backing = static_cast<int>(syscall(SYS_memfd_create, "detsim-eof-fault", MFD_CLOEXEC));
        if (backing < 0) fail(Stage::overlay, errno);
        struct stat metadata{};
        if (fstat(backing, &metadata) || metadata.st_size != 0) fail(Stage::overlay, EINVAL);
        void *tail = mmap(mapping + extent, length - extent, scenario.protection,
                          MAP_PRIVATE | MAP_FIXED, backing, 0);
        const int error = errno;
        if (close(backing) || !closed(backing)) fail(Stage::close_fd, errno);
        if (tail == MAP_FAILED) fail(Stage::overlay, error);
        if (tail != mapping + extent) fail(Stage::overlay, EINVAL);
    }
    if (close(fd) || !closed(fd)) fail(Stage::close_fd, errno);
    observation.descriptor_delta = descriptor_count() - before_fds;
    if (observation.descriptor_delta != -1) fail(Stage::close_fd, EMFILE);
    inspect_permissions(mapping, scenario, page_size);
    observation.stage = Stage::access;
    observation.result = Result::ready;
    emit(observation); // Publish bytes/permissions/fd checks before the intentional fault.
    observation.result = Result::success;
    volatile uint8_t *address = mapping + scenario.probe;
    switch (scenario.access) {
    case Access::read: observation.value = *address; break;
    case Access::write: *address = 0xa7; observation.value = *address; break;
    case Access::execute:
#if defined(__x86_64__)
        observation.value = reinterpret_cast<int (*)()>(mapping + scenario.probe)();
#else
        fail(Stage::access, ENOTSUP);
#endif
        break;
    }
    if (munmap(mapping, length)) fail(Stage::cleanup, errno);
    observation.stage = Stage::access;
    report();
}
Observation run(const Scenario &scenario, size_t page_size, int fd, bool synthetic) {
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC)) throw std::runtime_error("pipe2 failed");
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]); close(pipe_fds[1]); throw std::runtime_error("fork failed");
    }
    if (!pid) { close(pipe_fds[0]); child(scenario, page_size, fd, synthetic, pipe_fds[1]); }
    close(pipe_fds[1]);
    Observation result{};
    auto receive = [&](Observation &record) {
        pollfd event{pipe_fds[0], POLLIN, 0};
        int ready;
        do { ready = poll(&event, 1, 6000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0) return false;
        ssize_t length;
        do { length = read(pipe_fds[0], &record, sizeof(record)); } while (length < 0 && errno == EINTR);
        return length == static_cast<ssize_t>(sizeof(record));
    };
    bool reported = receive(result);
    if (reported && result.result == Result::ready) {
        Observation access{};
        reported = receive(access);
        if (reported) {
            result.result = access.result; result.stage = access.stage; result.error = access.error;
            result.signal = access.signal; result.code = access.code;
            result.address_offset = access.address_offset; result.value = access.value;
        }
    }
    close(pipe_fds[0]);
    if (!reported) kill(pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid, "waitpid failed");
    require(reported, "child timed out or did not report");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child report/exit failed");
    return result;
}
void unchanged(int fd, const Scenario &scenario, size_t page_size) {
    struct stat metadata{};
    require(fstat(fd, &metadata) == 0 && static_cast<size_t>(metadata.st_size) == scenario.file_size,
            "private access changed file size");
    std::vector<uint8_t> bytes(scenario.file_size);
    const ssize_t n = pread(fd, bytes.data(), bytes.size(), 0);
    require(n == static_cast<ssize_t>(bytes.size()), "cannot verify private file bytes");
    for (size_t i = 0; i < bytes.size(); ++i)
        require(bytes[i] == file_byte(i, page_size), "private write changed original file bytes");
}
const char *access_name(Access access) {
    return access == Access::read ? "read" : access == Access::write ? "write" : "execute";
}
bool executable_policy_limit(const Scenario &scenario, const Observation &result) {
    return (scenario.protection & PROT_EXEC) && result.result == Result::setup_error &&
        (result.stage == Stage::protect || result.stage == Stage::overlay || result.stage == Stage::map) &&
        (result.error == EACCES || result.error == EPERM);
}
void verify(const Scenario &scenario, size_t page_size, const Observation &result) {
    if (result.result == Result::setup_error) {
        char message[160];
        std::snprintf(message, sizeof(message), "mapping setup failed at stage %d: %s",
                      static_cast<int>(result.stage), std::strerror(result.error));
        throw std::runtime_error(message);
    }
    require(result.stage == Stage::access && result.descriptor_delta == -1,
            "access occurred before setup/fd closure completed");
    for (unsigned i = 0; i < scenario.pages; ++i)
        require(result.permissions[i] == scenario.protection, "mapping permissions differ");
    const int needed = scenario.access == Access::read ? PROT_READ :
                       scenario.access == Access::write ? PROT_WRITE : PROT_EXEC;
    const bool denied = !(scenario.protection & needed);
    const bool past_eof = scenario.probe >= readable_extent(scenario, page_size);
    if (denied || past_eof) {
        require(result.result == Result::signal, "faulting access succeeded");
        // Instruction fetch can resolve a missing file page before enforcing NX.
        // Measure that host ordering and require exact baseline/backing agreement.
        if (scenario.access == Access::execute && denied && past_eof)
            require(result.signal == SIGBUS || result.signal == SIGSEGV, "unexpected EOF instruction fault");
        else
            require(result.signal == (denied ? SIGSEGV : SIGBUS),
                    "wrong read/write fault or permission priority");
        const int expected_code = result.signal == SIGSEGV ? static_cast<int>(SEGV_ACCERR) : static_cast<int>(BUS_ADRERR);
        require(result.code == expected_code, "wrong si_code");
        require(result.address_offset == static_cast<int64_t>(scenario.probe), "wrong normalized si_addr");
    } else {
        require(result.result == Result::success, "valid access faulted");
        const int expected = scenario.access == Access::execute ? 42 : scenario.access == Access::write ? 0xa7 :
            scenario.probe < valid_bytes(scenario) ? file_byte(scenario.file_offset + scenario.probe, page_size) : 0;
        require(result.value == expected, "wrong accessible/COW byte or instruction result");
    }
}
void compare(const Observation &normal, const Observation &synthetic) {
    require(normal.result == synthetic.result && normal.signal == synthetic.signal &&
        normal.code == synthetic.code && normal.address_offset == synthetic.address_offset &&
        normal.value == synthetic.value && normal.byte_hash == synthetic.byte_hash &&
        normal.permissions == synthetic.permissions && normal.descriptor_delta == synthetic.descriptor_delta,
        "normal and synthetic mapping observations differ");
}
} // namespace

int main() {
    try {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        const long queried = sysconf(_SC_PAGESIZE);
        require(queried > 0, "page size unavailable");
        const size_t page = static_cast<size_t>(queried);
        const int initial_fds = descriptor_count();
        require(initial_fds >= 0, "cannot count descriptors");
        char policy[64] = "unavailable";
        const int policy_fd = open("/proc/sys/vm/memfd_noexec", O_RDONLY | O_CLOEXEC);
        if (policy_fd >= 0) {
            const ssize_t length = read(policy_fd, policy, sizeof(policy) - 1);
            const int closed_policy = close(policy_fd);
            require(closed_policy == 0, "close host-policy descriptor failed");
            if (length > 0) policy[length] = 0;
            else std::strcpy(policy, "unreadable");
            policy[std::strcspn(policy, "\n")] = 0;
        }
        std::printf("HOST page_size=%zu memfd_noexec=%s memfd_flags=MFD_CLOEXEC architecture=%s\n", page, policy,
#if defined(__x86_64__)
                    "x86_64"
#else
                    "non-x86_64 (execute cases skipped)"
#endif
        );
        struct Layout { size_t size, offset; unsigned pages; };
        const std::array layouts{
            Layout{0, 0, 3}, Layout{page / 2, 0, 4}, Layout{page, 0, 3},
            Layout{page + page / 2, page, 3}, Layout{2 * page, page, 3},
            Layout{page / 2, page, 3}, Layout{page, page, 3}, Layout{page / 2, 2 * page, 4}};
        unsigned passed = 0, skipped_exec = 0, checked_exec = 0;
        for (unsigned round = 0; round < 2; ++round) {
            for (const auto &layout : layouts) {
                const std::array probes{size_t{0}, page / 2 - 1, page / 2, page - 1, page,
                                       page + 7, 2 * page + 19, (layout.pages - 1) * page + 23};
                for (int protection : {PROT_READ | PROT_WRITE, PROT_READ, PROT_NONE}) {
                    for (Access access : {Access::read, Access::write}) {
                        for (size_t probe : probes) {
                            const Scenario scenario{layout.size, layout.offset, layout.pages, protection, access, probe};
                            const int fd = regular_file(scenario, page);
                            Observation normal{}, synthetic{};
                            try {
                                normal = run(scenario, page, fd, false);
                                synthetic = run(scenario, page, fd, true);
                                verify(scenario, page, normal); verify(scenario, page, synthetic);
                                compare(normal, synthetic); unchanged(fd, scenario, page);
                            } catch (...) { close(fd); throw; }
                            require(close(fd) == 0, "close regular input failed");
                            require(descriptor_count() == initial_fds, "parent descriptor leak");
                            ++passed;
                            std::printf("PASS round=%u size=%zu offset=%zu pages=%u prot=%d %s@%zu result=%s signal=%d code=%d addr=%" PRId64 " value=%d hash=%016" PRIx64 " fd_delta=%d\n",
                                round, layout.size, layout.offset, layout.pages, protection, access_name(access), probe,
                                normal.result == Result::signal ? "signal" : "success", normal.signal, normal.code,
                                normal.address_offset, normal.value, normal.byte_hash, normal.descriptor_delta);
                        }
                    }
                }
#if defined(__x86_64__)
                for (int protection : {PROT_READ | PROT_EXEC, PROT_READ, PROT_NONE}) {
                    for (size_t probe : {size_t{0}, page, 2 * page}) {
                        const Scenario scenario{layout.size, layout.offset, layout.pages, protection, Access::execute, probe};
                        const int fd = regular_file(scenario, page);
                        Observation normal{}, synthetic{};
                        try {
                            normal = run(scenario, page, fd, false);
                            synthetic = run(scenario, page, fd, true);
                            if (executable_policy_limit(scenario, normal) || executable_policy_limit(scenario, synthetic)) {
                                ++skipped_exec;
                                std::printf("SKIP executable mapping policy size=%zu offset=%zu probe=%zu normal_stage=%d normal_errno=%d synthetic_stage=%d synthetic_errno=%d\n",
                                    layout.size, layout.offset, probe, static_cast<int>(normal.stage), normal.error,
                                    static_cast<int>(synthetic.stage), synthetic.error);
                            } else {
                                verify(scenario, page, normal); verify(scenario, page, synthetic);
                                compare(normal, synthetic); ++checked_exec;
                                std::printf("PASS execute round=%u size=%zu offset=%zu prot=%d probe=%zu signal=%d code=%d addr=%" PRId64 " return=%d\n",
                                    round, layout.size, layout.offset, protection, probe, normal.signal, normal.code,
                                    normal.address_offset, normal.value);
                            }
                            unchanged(fd, scenario, page);
                        } catch (...) { close(fd); throw; }
                        require(close(fd) == 0, "close executable input failed");
                        require(descriptor_count() == initial_fds, "executable parent descriptor leak");
                    }
                }
#endif
            }
        }
        int remaining_status = 0;
        errno = 0;
        require(waitpid(-1, &remaining_status, WNOHANG) == -1 && errno == ECHILD, "unreaped child remains");
        std::printf("PASS native EOF backing oracle comparisons=%u execute_checked=%u execute_policy_skips=%u rounds=2 parent_fds=%d children_reaped=%u; OS backing only, no DetSim claim\n",
                    passed, checked_exec, skipped_exec, initial_fds, 2 * (passed + checked_exec + skipped_exec));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL native EOF backing oracle: %s\n", error.what());
        return 1;
    }
}
