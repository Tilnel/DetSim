#ifdef DETSIM_WRITEV_GUEST
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
int main() {
    char first[] = "WRITEV_", second[] = "OK\n";
    iovec vectors[]{{first, 7}, {second, 3}};
    return syscall(SYS_writev, 1, vectors, 2UL) == 10 ? 0 : 71;
}
#else
#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "io.h"
#include "sockstate.h"
#include "cereal/archives/binary.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <array>
#include <climits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
namespace {
char bytes[] = "ABCDEF";
sockaddr_in address{};
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
std::string resources() {
    std::ostringstream result;
    cereal::BinaryOutputArchive archive(result);
    for (int node = 0; node < NP; ++node)
        archive(ptmc_state.fs_states[node], ptmc_state.sock_states[node], *ptmc_state.fd_managers[node]);
    return result.str();
}
void reset() {
    for (int node = 0; node < NP; ++node) {
        ptmc_state.pids[node] = getpid();
        ptmc_state.fd_managers[node] = std::make_shared<FdManager>();
        ptmc_state.fs_states[node] = FileSystemState(ptmc_state.fd_managers[node]);
        ptmc_state.sock_states[node] = SockState(ptmc_state.fd_managers[node]);
        ptmc_state.addrs[node] = node ? "192.168.0.2" : "192.168.0.1";
    }
    ptmc_state.cursor = 0;
}
struct Connection { int client, receiver; bool accepted; };
Connection connected(bool accept) {
    reset();
    const int listener = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_STREAM, 0);
    address = {}; address.sin_family = AF_INET; address.sin_port = htons(9015);
    inet_pton(AF_INET, "192.168.0.1", &address.sin_addr);
    require(ptmc_state.sock_states[0].do_bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
            ptmc_state.sock_states[0].do_listen(listener, 4) == 0, "listen");
    ptmc_state.cursor = 1;
    const int client = ptmc_state.sock_states[1].do_socket(AF_INET, SOCK_STREAM, 0);
    require(ptmc_state.sock_states[1].do_connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "connect");
    ptmc_state.cursor = 0;
    const int receiver = accept ? ptmc_state.sock_states[0].do_accept(listener, nullptr, nullptr) : listener;
    require(receiver >= 0, "accept");
    ptmc_state.cursor = 1;
    return {client, receiver, accept};
}
const std::deque<std::string>& queued(const Connection& c) {
    const auto& server = ptmc_state.sock_states[0];
    if (c.accepted) return server.tcp_connections().at(c.receiver).recv_buffer;
    const auto* listener = server.get_socket(c.receiver);
    require(listener && listener->established_connections.size() == 1, "pending connection count");
    return listener->established_connections.begin()->second.recv_buffer;
}
template<class Action> void unchanged_error(Action action, long expected, const char* why) {
    const auto before = resources();
    require(action() == expected, why);
    require(resources() == before, "error changed resource state");
}
template<class Action> void unsupported_unchanged(Action action, const char* why) {
    const auto before = resources(); bool rejected = false;
    try { (void)action(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, why);
    require(resources() == before, "unsupported operation changed resource state");
}
void scalar_send_validation_order() {
    reset();
    auto& network = ptmc_state.sock_states[0];
    unsupported_unchanged([&] { return network.do_send(-1, reinterpret_cast<void*>(1), 1, MSG_OOB); },
                          "scalar flags no longer precede descriptor validation");
    unchanged_error([&] { return network.do_send(-1, reinterpret_cast<void*>(1), 1, 0); }, -EBADF,
                    "scalar invalid descriptor did not precede guest read");
    unchanged_error([&] { return network.do_send(1, reinterpret_cast<void*>(1), 1, 0); }, -ENOTSOCK,
                    "scalar nonsocket descriptor did not precede guest read");
    const int idle = network.do_socket(AF_INET, SOCK_STREAM, 0);
    for (size_t count : {size_t(0), size_t(1)})
        unchanged_error([&] { return network.do_send(idle, reinterpret_cast<void*>(1), count, 0); }, -ENOTCONN,
                        "scalar unconnected state no longer precedes zero length or guest read");
    auto connection = connected(true);
    auto& sender = ptmc_state.sock_states[1];
    unchanged_error([&] { return sender.do_send(connection.client, reinterpret_cast<void*>(1), 0, 0); }, 0,
                    "scalar zero length dereferenced guest bytes");
    unchanged_error([&] { return sender.do_send(connection.client, reinterpret_cast<void*>(1), 1, 0); }, -EFAULT,
                    "scalar payload fault changed result");
    sender.sockets().at(connection.client).peer_addr.sin_port = htons(9999);
    unchanged_error([&] { return sender.do_send(connection.client, reinterpret_cast<void*>(1), 1, 0); }, -EFAULT,
                    "scalar route lookup moved before guest read");
    unchanged_error([&] { return sender.do_send(connection.client, bytes, 1, 0); }, -ECONNRESET,
                    "scalar absent route changed result");
    unchanged_error([&] { return sender.do_send(connection.client, reinterpret_cast<void*>(1), 0, 0); }, 0,
                    "scalar absent route now rejects empty send");
    puts("PASS scalar TCP flags/descriptor/connection/zero/payload/routing error order");
}
void scalar_send_single_chunk(bool accept) {
    const auto connection = connected(accept);
    auto& sender = ptmc_state.sock_states[1];
    require(sender.do_send(connection.client, bytes, 6, MSG_NOSIGNAL | MSG_DONTWAIT) == 6, "scalar send length");
    const auto& queue = queued(connection);
    require(queue.size() == 1 && queue.front() == "ABCDEF", "scalar send no longer publishes one TCP chunk");
    printf("PASS scalar TCP %s publishes one complete chunk\n", accept ? "accepted" : "preaccept");
}
#ifndef DETSIM_WRITEV_SCALAR_ONLY
void restore_resources(const std::string& bytes) {
    std::istringstream input(bytes);
    cereal::BinaryInputArchive archive(input);
    for (int node = 0; node < NP; ++node) {
        FileSystemState fs;
        SockState network;
        auto manager = std::make_shared<FdManager>();
        archive(fs, network, *manager);
        ptmc_state.fd_managers[node] = manager;
        ptmc_state.fs_states[node] = std::move(fs);
        ptmc_state.sock_states[node] = std::move(network);
        ptmc_state.fs_states[node].set_fd_manager(manager);
        ptmc_state.sock_states[node].set_fd_manager(manager);
    }
}
std::string file_bytes(int node, int fd) {
    const auto& fs = ptmc_state.fs_states[node];
    const auto& content = fs.filesystem.at(fs.open_files.at(fd).path).content;
    return std::string(reinterpret_cast<const char*>(content.data()), content.size());
}
struct GuardPages {
    unsigned char* data;
    GuardPages() : data(static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))) {
        require(data != MAP_FAILED, "guard mapping");
        std::memset(data, 'P', 4096);
        require(mprotect(data + 4096, 4096, PROT_NONE) == 0, "guard protection");
    }
    ~GuardPages() { munmap(data, 8192); }
};
void writev_descriptor_and_count_errors() {
    reset();
    auto& fs = ptmc_state.fs_states[0];
    const int file = fs.do_open("/writev", O_CREAT | O_RDWR, 0600);
    const int readonly = fs.do_open("/writev", O_RDONLY, 0);
    const int directory = fs.do_open("/", O_RDONLY | O_DIRECTORY, 0);
    const int epoll = ptmc_state.sock_states[0].do_epoll_create1(0);
    require(file >= 0 && readonly >= 0 && directory >= 0 && epoll >= 0, "descriptor fixtures");
    const auto bad = reinterpret_cast<iovec*>(1);
    for (size_t count : {size_t(0), size_t(1), size_t(1025), size_t(-1)}) {
        for (int fd : {-1, readonly, directory})
            unchanged_error([&] { return emu_writev(fd, bad, count); }, -EBADF,
                            "descriptor access did not precede metadata/count errors");
        unchanged_error([&] { return emu_writev(epoll, bad, count); }, -EINVAL,
                        "epoll error did not precede metadata/count errors");
    }
    for (size_t count : {size_t(0), size_t(0x100000000ULL)})
        unchanged_error([&] { return emu_writev(file, bad, count); }, 0,
                        "zero low32 count accessed metadata");
    for (size_t count : {size_t(1025), size_t(-1), size_t(0xffffffffULL), size_t(0x100000401ULL)})
        unchanged_error([&] { return emu_writev(file, bad, count); }, -EINVAL,
                        "low32 invalid count did not precede metadata read");
    std::array<iovec, 1024> zero{};
    for (auto& item : zero) item = {reinterpret_cast<void*>(1), 0};
    for (size_t count : {size_t(1024), size_t(0x100000400ULL)})
        unchanged_error([&] { return emu_writev(file, zero.data(), count); }, 0,
                        "1024 zero-length vectors or high32 count alias rejected");
    puts("PASS writev descriptor, low32 count, zero-count and 1024-vector ABI priorities");
}
void writev_metadata_import_priorities() {
    reset();
    const int file = ptmc_state.fs_states[0].do_open("/writev", O_CREAT | O_RDWR, 0600);
    require(file >= 0, "metadata file");
    GuardPages metadata;
    auto* crossing = reinterpret_cast<iovec*>(metadata.data + 4096 - sizeof(iovec));
    iovec valid{bytes, 3};
    std::memcpy(crossing, &valid, sizeof valid);
    for (const iovec* address : {static_cast<const iovec*>(nullptr),
                                reinterpret_cast<const iovec*>(1), static_cast<const iovec*>(crossing)}) {
        const size_t count = address == crossing ? 2 : 1;
        unchanged_error([&] { return emu_writev(file, address, count); }, -EFAULT,
                        "metadata fault did not return EFAULT without partial commit");
    }
    GuardPages base_fault;
    require(mprotect(base_fault.data, 4096, PROT_NONE) == 0 &&
            mprotect(base_fault.data + 4096, 4096, PROT_READ | PROT_WRITE) == 0,
            "inverted metadata protection");
    const size_t negative_length = static_cast<size_t>(SSIZE_MAX) + 1;
    std::memcpy(base_fault.data + 4096, &negative_length, sizeof negative_length);
    const auto* unreadable_base = reinterpret_cast<const iovec*>(base_fault.data + 4096 - sizeof(void*));
    unchanged_error([&] { return emu_writev(file, unreadable_base, 1); }, -EFAULT,
                    "same-item unreadable base lost priority over readable negative length");
    iovec negative{reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX) + 1};
    std::memcpy(crossing, &negative, sizeof negative);
    unchanged_error([&] { return emu_writev(file, crossing, 2); }, -EINVAL,
                    "earlier negative length lost priority over later metadata fault");
    iovec mixed[]{{reinterpret_cast<void*>(UINTPTR_MAX), 1}, negative};
    unchanged_error([&] { return emu_writev(file, mixed, 2); }, -EINVAL,
                    "payload range checked before complete metadata import");
    iovec kernel_zero[]{{reinterpret_cast<void*>(UINTPTR_MAX), 0}, valid};
    unchanged_error([&] { return emu_writev(file, kernel_zero, 2); }, -EFAULT,
                    "zero-length kernel base skipped address-range validation");
    for (size_t count : {size_t(0), size_t(1)}) {
        iovec high{reinterpret_cast<void*>(0x0000800000000000ULL), count};
        unsupported_unchanged([&] { return emu_writev(file, &high, 1); },
                              "unknown L4/L5 user boundary was silently accepted or assigned fake errno");
        iovec high_mixed[]{high, negative};
        unchanged_error([&] { return emu_writev(file, high_mixed, 2); }, -EINVAL,
                        "high address unsupported policy masked later invalid length");
    }
    puts("PASS writev sequential len/base import and metadata-before-payload range priorities");
}
void writev_import_range_boundaries() {
    reset();
    const int file = ptmc_state.fs_states[0].do_open("/writev", O_CREAT | O_RDWR, 0600);
    require(file >= 0, "range boundary file");
    unsigned failures = 0;
    auto probe = [&](const char* name, auto action) {
        try { action(); printf("PASS %s\n", name); }
        catch (const std::exception& error) {
            fprintf(stderr, "FAIL %s: %s\n", name, error.what()); ++failures;
        }
    };
    const auto* metadata = reinterpret_cast<const iovec*>(0x00007fffffffe000ULL);
    probe("writev metadata whole span crossing user guard is unsupported before guest import", [&] {
        unsupported_unchanged([&] { return emu_writev(file, metadata, 257); },
                              "whole metadata span did not preflight the conservative user boundary");
    });
    probe("writev metadata whole span at low-domain edge still performs guest import", [&] {
        unchanged_error([&] { return emu_writev(file, metadata, 256); }, -EFAULT,
                        "fully low-domain unreadable metadata did not return EFAULT");
    });
    probe("writev zero count ignores metadata in user guard", [&] {
        unchanged_error([&] { return emu_writev(file, metadata, 0); }, 0,
                        "zero count inspected metadata range");
    });
    const iovec guard_zero{reinterpret_cast<void*>(0x00007ffffffff001ULL), 0};
    probe("writev zero-length payload in user guard is unsupported", [&] {
        unsupported_unchanged([&] { return emu_writev(file, &guard_zero, 1); },
                              "zero-length payload bypassed the user guard boundary");
    });
    const iovec maximum[]{{reinterpret_cast<void*>(1), static_cast<size_t>(SSIZE_MAX)}, {nullptr, 0}};
    probe("writev single iovec clips before range and reaches explicit staging limit", [&] {
        unsupported_unchanged([&] { return emu_writev(file, maximum, 1); },
                              "single iovec raw length incorrectly returned range EFAULT before clipping");
    });
    probe("writev multiple iovecs validate raw range before clipping", [&] {
        unchanged_error([&] { return emu_writev(file, maximum, 2); }, -EFAULT,
                        "multi-iovec range did not precede clipping");
    });
    require(!failures, "writev import boundary regressions");
}
void writev_file_and_stdio_match_flattened_write(int fd) {
    reset();
    if (fd != 1) {
        fd = ptmc_state.fs_states[0].do_open("/writev", O_CREAT | O_RDWR, 0600);
        require(fd >= 0, "projection file");
    }
    const auto source = resources();
    iovec vectors[]{{bytes, 3}, {reinterpret_cast<void*>(1), 0}, {bytes + 3, 3}};
    require(emu_writev(fd, vectors, 3) == 6, "writev did not flatten fully readable vectors");
    const auto expected = resources();
    require(file_bytes(0, fd) == "ABCDEF" && ptmc_state.fs_states[0].open_files.at(fd).offset == 6,
            "writev file/stdio output or offset differs");
    restore_resources(source);
    require(emu_write(fd, bytes, 6) == 6 && resources() == expected,
            "writev resource projection differs from one flattened scalar write");
    for (int replay = 0; replay < 3; ++replay) {
        restore_resources(source);
        require(emu_writev(fd, vectors, 0x100000003ULL) == 6 && resources() == expected,
                "archived writev source replay changed resource projection");
        restore_resources(source);
        require(emu_writev(fd, vectors + 2, 1) == 3 && file_bytes(0, fd) == "DEF",
                "independent branch did not retain its own payload");
        restore_resources(expected);
        require(file_bytes(0, fd) == "ABCDEF", "another branch modified saved file bytes");
    }
    printf("PASS writev %s equals flattened scalar write and isolated archive replay\n", fd == 1 ? "stdio" : "file");
}
void writev_tcp_is_one_flattened_chunk(bool accept) {
    const auto connection = connected(accept);
    const auto source = resources();
    iovec vectors[]{{bytes, 2}, {bytes + 2, 4}};
    require(emu_writev(connection.client, vectors, 2) == 6, "TCP writev result");
    require(queued(connection).size() == 1 && queued(connection).front() == "ABCDEF",
            "TCP writev split vectors into independent chunks");
    const auto expected = resources();
    restore_resources(source);
    require(emu_write(connection.client, bytes, 6) == 6 && resources() == expected,
            "TCP writev differs from one flattened scalar write");
    for (int replay = 0; replay < 3; ++replay) {
        restore_resources(source);
        require(emu_writev(connection.client, vectors, 2) == 6 && resources() == expected,
                "TCP archive replay changed a peer queue");
        restore_resources(source);
        require(emu_writev(connection.client, vectors + 1, 1) == 4 &&
                queued(connection).size() == 1 && queued(connection).front() == "CDEF",
                "TCP alternate branch did not isolate its chunk");
        restore_resources(expected);
        require(queued(connection).size() == 1 && queued(connection).front() == "ABCDEF",
                "TCP alternate branch mutated saved output");
    }
    printf("PASS writev TCP %s single chunk, scalar equivalence and isolated archive replay\n", accept ? "accepted" : "preaccept");
}
void writev_unsupported_inputs_do_not_mutate_resources() {
    reset();
    auto& fs = ptmc_state.fs_states[0];
    const int file = fs.do_open("/writev", O_CREAT | O_RDWR, 0600);
    int pipe[2]; require(fs.do_pipe(pipe, 0) == 0, "unsupported pipe");
    const int udp = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_DGRAM, 0);
    const int idle = ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_STREAM, 0);
    iovec valid[]{{bytes, 3}, {bytes + 3, 3}};
    for (int fd : {pipe[1], udp, idle})
        unsupported_unchanged([&] { return emu_writev(fd, valid, 2); },
                              "unsupported pipe/UDP/unconnected TCP returned a fabricated result");
    GuardPages payload;
    iovec fault_after_prefix[]{{bytes, 3}, {payload.data + 4096, 1}};
    iovec crossing{payload.data, 4097};
    for (int fd : {file, 1}) {
        unsupported_unchanged([&] { return emu_writev(fd, fault_after_prefix, 2); },
                              "faulting payload pretended to return Linux errno/partial result");
        unsupported_unchanged([&] { return emu_writev(fd, &crossing, 1); },
                              "cross-page payload fault partially committed");
        iovec too_large{bytes, 64ULL * 1024 * 1024 + 1};
        unsupported_unchanged([&] { return emu_writev(fd, &too_large, 1); },
                              "staging limit became a fabricated Linux result");
    }
    for (bool accept : {true, false}) {
        auto connection = connected(accept);
        unsupported_unchanged([&] { return emu_writev(connection.client, fault_after_prefix, 2); },
                              "faulting TCP payload committed a prefix");
        ptmc_state.sock_states[1].sockets().at(connection.client).peer_addr.sin_port = htons(9999);
        unsupported_unchanged([&] { return emu_writev(connection.client, valid, 2); },
                              "non-routable TCP writev silently succeeded");
    }
    puts("PASS unsupported writev descriptor/payload/routing/staging branches preserve every resource");
}
#endif

}
int main(int argc, char** argv) {
    try {
        loglevel = 4;
#ifndef DETSIM_WRITEV_SCALAR_ONLY
        if (argc == 2 && !std::strcmp(argv[1], "--import-boundaries")) {
            writev_import_range_boundaries(); return 0;
        }
#else
        (void)argc; (void)argv;
#endif
        scalar_send_validation_order();
        scalar_send_single_chunk(true); scalar_send_single_chunk(false);
#ifndef DETSIM_WRITEV_SCALAR_ONLY
        writev_descriptor_and_count_errors();
        writev_metadata_import_priorities();
        writev_import_range_boundaries();
        writev_file_and_stdio_match_flattened_write(3);
        writev_file_and_stdio_match_flattened_write(1);
        writev_tcp_is_one_flattened_chunk(true); writev_tcp_is_one_flattened_chunk(false);
        writev_unsupported_inputs_do_not_mutate_resources();
#endif
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "writev integration: %s\n", error.what()); return 1;
    }
}
#endif
