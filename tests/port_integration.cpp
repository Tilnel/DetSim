#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <unistd.h>

static_assert(NP == 2, "port regressions require two modeled nodes");
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static sockaddr_in peer;
static void reset() {
    for (int node = 0; node < NP; ++node) {
        ptmc_state.fd_managers[node] = std::make_shared<FdManager>();
        ptmc_state.sock_states[node] = SockState(ptmc_state.fd_managers[node]);
        ptmc_state.pids[node] = getpid();
        ptmc_state.addrs[node] = node == 0 ? "127.0.0.1" : "127.0.0.2";
    }
    // Reserve node 0 fd 3 so equivalent clients start with the same fd on
    // both nodes; node 1 owns the real listener at fd 3.
    require(ptmc_state.sock_states[0].do_socket(AF_INET, SOCK_STREAM, 0) == 3, "reserve matching fd");
    ptmc_state.cursor = 1;
    auto &server = ptmc_state.sock_states[1];
    int listener = server.do_socket(AF_INET, SOCK_STREAM, 0);
    require(server.do_bind(listener, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0,
            "real listener bind");
    require(server.do_listen(listener, 128) == 0, "real listener listen");
    ptmc_state.cursor = 0;
}
static int new_socket(int node) {
    int fd = ptmc_state.sock_states[node].do_socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket creation");
    return fd;
}
static int connect_socket(int node, int fd) {
    ptmc_state.cursor = node;
    auto &state = ptmc_state.sock_states[node];
    require(state.do_connect(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) == 0,
            "connect");
    return ntohs(state.get_socket(fd)->local_addr.sin_port);
}
static void replay() {
    reset();
    int fd = new_socket(0);
    SockState saved[NP];
    FdManager managers[NP];
    for (int node = 0; node < NP; ++node) {
        saved[node] = ptmc_state.sock_states[node];
        managers[node] = *ptmc_state.fd_managers[node];
    }
    auto restore = [&] {
        for (int node = 0; node < NP; ++node) {
            ptmc_state.fd_managers[node] = std::make_shared<FdManager>(managers[node]);
            ptmc_state.sock_states[node] = saved[node];
            ptmc_state.sock_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
        }
    };
    srand(1);
    int first = connect_socket(0, fd);
    for (unsigned seed : {2u, 123u, 444u}) {
        restore();
        srand(seed);
        for (int i = 0; i < 31; ++i) (void)rand();
        require(connect_socket(0, fd) == first, "restored socket chose a different port after search RNG changes");
    }
    restore();
    srand(777);
    int expected = rand();
    srand(777);
    connect_socket(0, fd);
    require(rand() == expected, "connect consumed the search RNG");
}
static void distinct_and_reuse() {
    reset();
    std::set<std::pair<uint32_t, int>> used;
    int first_fd = -1, first_port = -1;
    for (int i = 0; i < 40; ++i) {
        int node = i % NP;
        int fd = new_socket(node);
        int port = connect_socket(node, fd);
        require(port >= 10000 && port < 60000, "port outside modeled ephemeral range");
        auto ip = ptmc_state.sock_states[node].get_socket(fd)->local_addr.sin_addr.s_addr;
        require(used.insert({ip, port}).second, "live sockets share a virtual endpoint");
        if (i == 1) {
            require(fd == first_fd, "independent clients must have matching process-local fds");
            require(port == first_port, "independent nodes should reuse ports");
        }
        if (!i) { first_fd = fd; first_port = port; }
    }
    ptmc_state.cursor = 1;
    int accepted = ptmc_state.sock_states[1].do_accept(3, nullptr, nullptr);
    require(accepted >= 0, "accept first connection before endpoint reuse");
    require(ptmc_state.sock_states[1].do_close(accepted) == 0, "close accepted peer");
    ptmc_state.cursor = 0;
    require(ptmc_state.sock_states[0].do_close(first_fd) == 0, "close first client");
    require(connect_socket(0, new_socket(0)) == first_port, "released port was not deterministically reusable");
}
static void exhaustion() {
    reset();
    auto &state = ptmc_state.sock_states[0];
    int fd = new_socket(0);
    for (int port = 10000; port < 60000; ++port) {
        Socket socket;
        socket.fd = port;
        socket.domain = AF_INET;
        socket.type = SOCK_STREAM;
        socket.bound = true;
        socket.local_addr = {};
        socket.local_addr.sin_family = AF_INET;
        socket.local_addr.sin_port = htons(port);
        state.add_socket(socket);
    }
    require(state.do_connect(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) == -EADDRNOTAVAIL,
            "exhausted ports must fail explicitly");
    require(!state.get_socket(fd)->bound && !state.get_socket(fd)->connected && state.tcp_connections().empty(),
            "failed allocation changed socket state");
    require(state.do_close(12345) == 0, "release occupied port");
    require(connect_socket(0, fd) == 12345, "allocator failed to find the single free port");
}
static void source_address() {
    reset();
    auto &state = ptmc_state.sock_states[0];
    int fd = new_socket(0);
    ptmc_state.addrs[0] = "invalid-address";
    require(state.do_connect(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) == -EADDRNOTAVAIL,
            "invalid configured source address");
    require(!state.get_socket(fd)->bound && !state.get_socket(fd)->connected && state.tcp_connections().empty(),
            "invalid address changed state");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(0x7f000009);
    local.sin_port = htons(15000);
    require(state.do_bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0, "explicit bind");
    require(connect_socket(0, fd) == 15000 &&
            state.get_socket(fd)->local_addr.sin_addr.s_addr == local.sin_addr.s_addr,
            "explicit source endpoint changed");
    ptmc_state.addrs[0] = "127.0.0.1";
    fd = new_socket(0);
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    require(state.do_bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0, "wildcard bind");
    require(connect_socket(0, fd) == 10000 &&
            state.get_socket(fd)->local_addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
            "wildcard bind did not select a concrete endpoint");
}
int main() {
    loglevel = 4;
    peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(6379);
    peer.sin_addr.s_addr = htonl(0x7f000002);
    try {
        replay(); puts("port replay and independent search RNG: PASS");
        distinct_and_reuse(); puts("distinct live endpoints and reuse: PASS");
        exhaustion(); puts("exhaustion and retry: PASS");
        source_address(); puts("source address validation and explicit/wildcard bind: PASS");
    } catch (const std::exception &e) {
        fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
