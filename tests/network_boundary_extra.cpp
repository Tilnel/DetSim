#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static sockaddr_in address(int node, int port) {
  sockaddr_in result{}; result.sin_family = AF_INET; result.sin_port = htons(port);
  inet_pton(AF_INET, ptmc_state.addrs[node].c_str(), &result.sin_addr); return result;
}
static void reset() {
  for (int node = 0; node < NP; ++node) {
    ptmc_state.fd_managers[node] = std::make_shared<FdManager>();
    ptmc_state.sock_states[node] = SockState(ptmc_state.fd_managers[node]);
    ptmc_state.fs_states[node] = FileSystemState();
    ptmc_state.fs_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
    ptmc_state.pids[node] = getpid();
    ptmc_state.addrs[node] = node ? "192.168.0.2" : "192.168.0.1";
  }
  ptmc_state.cursor = 0;
}
static std::string archive(SockState &state) {
  std::ostringstream stream; { cereal::BinaryOutputArchive out(stream); out(state); } return stream.str();
}
static int listener(int node, int port, bool wildcard = false) {
  ptmc_state.cursor = node;
  auto &state = ptmc_state.sock_states[node];
  int fd = state.do_socket(AF_INET, SOCK_STREAM, 0);
  auto addr = address(node, port); if (wildcard) addr.sin_addr.s_addr = INADDR_ANY;
  require(state.do_bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "listener bind");
  require(state.do_listen(fd, 32) == 0, "listener listen"); return fd;
}
static uint64_t endpoint_key(const Socket *socket) {
  return (uint64_t(socket->local_addr.sin_addr.s_addr) << 32) | socket->local_addr.sin_port;
}
static void full_connection_identity() {
  reset(); int first_listener = listener(1, 9000); int second_listener = listener(1, 9001);
  auto first_dest = address(1,9000), second_dest = address(1,9001);
  ptmc_state.cursor = 0; int first = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(first,reinterpret_cast<sockaddr*>(&first_dest),sizeof(first_dest)) == 0,"historical peer connect");
  uint64_t key = endpoint_key(ptmc_state.sock_states[0].get_socket(first));
  require(ptmc_state.sock_states[0].do_close(first) == 0,"historical peer close");
  int second = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(second,reinterpret_cast<sockaddr*>(&second_dest),sizeof(second_dest)) == 0,"reused source at another destination");
  require(endpoint_key(ptmc_state.sock_states[0].get_socket(second)) == key,"fixture reused source endpoint");
  char payload = 'b';
  require(emu_send(second,&payload,1,0) == 1,"send to second pending destination");
  const auto &first_queue = ptmc_state.sock_states[1].get_socket(first_listener)->established_connections.at(key).recv_buffer;
  const auto &second_queue = ptmc_state.sock_states[1].get_socket(second_listener)->established_connections.at(key).recv_buffer;
  require(first_queue.size() == 1 && first_queue.front().empty() &&
          second_queue.size() == 1 && second_queue.front() == "b", "pending send matches both source and destination");
  require(ptmc_state.sock_states[0].do_close(second) == 0,"second pending close");
  require(first_queue.size() == 1 && second_queue.size() == 2 && second_queue.back().empty(),
          "pending close matches both source and destination");
  reset(); int server = listener(1,9000); auto dest = address(1,9000);
  ptmc_state.cursor = 0; first = emu_socket(AF_INET,SOCK_STREAM,0); second = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(first,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) == 0 &&
          emu_connect(second,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) == 0,"two peers for one listener");
  ptmc_state.cursor = 1; int accepted1 = emu_accept(server,nullptr,nullptr), accepted2 = emu_accept(server,nullptr,nullptr);
  require(accepted1 >= 0 && accepted2 >= 0,"accept both peers");
  require(ptmc_state.sock_states[1].do_close(accepted2) == 0,"close second accepted peer");
  require(ptmc_state.sock_states[0].tcp_connections().at(first).recv_buffer.empty() &&
          ptmc_state.sock_states[0].tcp_connections().at(second).recv_buffer.size() == 1,
          "accepted close targets its exact reciprocal connection");
}

static void held_peer_endpoint_reuse() {
  reset(); int server = listener(1,9000); auto dest = address(1,9000);
  ptmc_state.cursor = 0; int first = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(first,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) == 0,"held peer first connect");
  int first_port = ptmc_state.sock_states[0].get_socket(first)->local_addr.sin_port;
  ptmc_state.cursor = 1; int accepted1 = emu_accept(server,nullptr,nullptr);
  require(accepted1 >= 0,"held peer first accept");
  ptmc_state.cursor = 0; require(ptmc_state.sock_states[0].do_close(first) == 0,"held peer close client");
  int second = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(second,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) == 0,"held peer reconnect");
  require(ptmc_state.sock_states[0].get_socket(second)->local_addr.sin_port != first_port,
          "ephemeral selection excludes surviving reciprocal tuple");
  char payload = 'n'; require(emu_send(second,&payload,1,0) == 1,"held peer send reconnect payload");
  const auto &old_queue = ptmc_state.sock_states[1].tcp_connections().at(accepted1).recv_buffer;
  require(old_queue.size() == 1 && old_queue.front().empty(),"old accepted peer retains only EOF");
  ptmc_state.cursor = 1; int accepted2 = emu_accept(server,nullptr,nullptr); char received = 0;
  require(accepted2 >= 0 && emu_recv(accepted2,&received,1,0) == 1 && received == payload,
          "reconnect payload reaches new accepted peer");
  require(emu_recv(accepted1,&received,1,0) == 0,"old peer EOF persists after reconnect");
  ptmc_state.cursor = 0; require(ptmc_state.sock_states[0].do_close(second) == 0,"held peer second close");
  ptmc_state.cursor = 1;
  require(emu_recv(accepted2,&received,1,0) == 0 && old_queue.size() == 1,
          "reconnect EOF does not alter historical peer");
  ptmc_state.cursor = 0; int explicit_reuse = emu_socket(AF_INET,SOCK_STREAM,0);
  auto explicit_source = address(0,ntohs(first_port));
  require(emu_bind(explicit_reuse,reinterpret_cast<sockaddr*>(&explicit_source),sizeof(explicit_source)) == 0,
          "explicit historical endpoint bind fixture");
  auto before = archive(ptmc_state.sock_states[0]); bool rejected = false;
  try {emu_connect(explicit_reuse,reinterpret_cast<sockaddr*>(&dest),sizeof(dest));}
  catch (const std::runtime_error&) {rejected = true;}
  require(rejected && archive(ptmc_state.sock_states[0]) == before,
          "explicit bound reuse of surviving tuple fails before mutation");
}

static void stream_syscall_and_signal_boundaries() {
  reset(); int server = listener(1,9000); auto dest = address(1,9000);
  ptmc_state.cursor = 0; int client = emu_socket(AF_INET,SOCK_STREAM,0);
  require(emu_connect(client,reinterpret_cast<sockaddr*>(&dest),sizeof(dest)) == 0,"stream syscall connect");
  ptmc_state.cursor = 1; int accepted = emu_accept(server,nullptr,nullptr);
  ptmc_state.cursor = 0; char payload = 's';
  require(emu_sendto(client,&payload,1,0,nullptr,0) == 1,"TCP sendto with NULL destination");
  auto before = archive(ptmc_state.sock_states[1]);
  bool rejected = false;
  try {emu_sendto(client,&payload,1,0,reinterpret_cast<sockaddr*>(&dest),sizeof(dest));}
  catch (const std::runtime_error&) {rejected = true;}
  require(rejected && archive(ptmc_state.sock_states[1]) == before,"unsupported TCP destination preserves queues");
  ptmc_state.cursor = 1; char received = 0; socklen_t length = sizeof(dest); rejected = false;
  try {emu_recvfrom(accepted,&received,1,0,reinterpret_cast<sockaddr*>(&dest),&length);}
  catch (const std::runtime_error&) {rejected = true;}
  require(rejected && archive(ptmc_state.sock_states[1]) == before,"unsupported TCP source query preserves queues");
  require(emu_recvfrom(accepted,&received,1,0,nullptr,nullptr) == 1 && received == payload,
          "TCP recvfrom with NULL source");
  int epoll = emu_epoll_create1(0); sigset_t mask{}; epoll_event event{};
  rejected = false; try {emu_epoll_pwait(epoll,&event,1,0,&mask,sizeof(mask));}
  catch (const std::runtime_error&) {rejected = true;}
  require(rejected,"pwait refuses unmodeled signal mask");
  rejected = false; try {emu_epoll_pwait2(epoll,&event,1,nullptr,&mask,sizeof(mask));}
  catch (const std::runtime_error&) {rejected = true;}
  require(rejected,"pwait2 refuses unmodeled signal mask");
}

int main() {
  loglevel = 4;
  try {
    reset(); int fd = emu_socket(AF_INET, SOCK_STREAM, 0); auto dest = address(1, 9000);
    auto before = archive(ptmc_state.sock_states[0]);
    require(emu_connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == -ECONNREFUSED &&
            before == archive(ptmc_state.sock_states[0]), "refused connection preserves socket");
    int server = listener(1, 9000, true); ptmc_state.cursor = 0;
    auto wrong = address(0, 9000);
    require(emu_connect(fd, reinterpret_cast<sockaddr*>(&wrong), sizeof(wrong)) == -ECONNREFUSED,
            "same port wrong node must not connect");
    require(emu_connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0,
            "wildcard listener routes by configured node IP");
    require(emu_connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == -EISCONN, "repeated connect rejected");
    ptmc_state.cursor = 1; int duplicate = emu_socket(AF_INET, SOCK_STREAM, 0);
    require(emu_bind(duplicate, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == -EADDRINUSE,
            "wildcard and concrete binds cannot alias");
    reset(); server = listener(0, 9000); dest = address(0, 9000);
    int local_client = emu_socket(AF_INET, SOCK_STREAM, 0);
    require(emu_connect(local_client, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0, "local client connect");
    ptmc_state.cursor = 1; emu_socket(AF_INET, SOCK_STREAM, 0);
    int remote_client = emu_socket(AF_INET, SOCK_STREAM, 0);
    require(local_client == remote_client, "fixture duplicated process-local fd");
    require(emu_connect(remote_client, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0, "remote client connect");
    ptmc_state.cursor = 0; int accepted1 = emu_accept(server, nullptr, nullptr);
    int accepted2 = emu_accept(server, nullptr, nullptr);
    require(accepted1 >= 0 && accepted2 >= 0 &&
            ptmc_state.sock_states[0].get_socket(accepted1)->peer_addr.sin_addr.s_addr == address(0,0).sin_addr.s_addr &&
            ptmc_state.sock_states[0].get_socket(accepted2)->peer_addr.sin_addr.s_addr == address(1,0).sin_addr.s_addr,
            "accept FIFO distinguishes identical fds on separate nodes");
    sockaddr_in queried{}; socklen_t query_size = sizeof(queried);
    require(emu_getsockname(accepted2, reinterpret_cast<sockaddr*>(&queried), &query_size) == 0 &&
            queried.sin_addr.s_addr == dest.sin_addr.s_addr && queried.sin_port == dest.sin_port,
            "getsockname returns virtual server endpoint");
    query_size = sizeof(queried);
    require(emu_getpeername(accepted2, reinterpret_cast<sockaddr*>(&queried), &query_size) == 0 &&
            queried.sin_addr.s_addr == address(1,0).sin_addr.s_addr,
            "getpeername returns virtual client endpoint");
    query_size = 2; queried = {};
    require(emu_getsockname(accepted2, reinterpret_cast<sockaddr*>(&queried), &query_size) == 0 &&
            queried.sin_family == AF_INET && query_size == sizeof(queried), "truncated address reports full length");
    require(emu_getpeername(accepted2, reinterpret_cast<sockaddr*>(&queried), reinterpret_cast<socklen_t*>(1)) == -EFAULT,
            "address length pointer is checked");
    int file = ptmc_state.fs_states[0].do_open("/ordinary", O_CREAT | O_RDWR, 0600);
    require(emu_bind(file, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == -ENOTSOCK,
            "socket operation on live regular file returns ENOTSOCK");
    int enabled = 1;
    require(emu_setsockopt(accepted1, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0, "set modeled TCP option");
    for (const auto &[option, expected] : {std::pair{TCP_KEEPIDLE, 7200}, std::pair{TCP_KEEPINTVL, 75}, std::pair{TCP_KEEPCNT, 9}}) {
      int default_value = 0; socklen_t default_size = sizeof(default_value);
      require(emu_getsockopt(accepted1, IPPROTO_TCP, option, &default_value, &default_size) == 0 && default_value == expected,
              "keepalive defaults deterministic");
    }
    for (const auto &[option, maximum] : {std::pair{TCP_KEEPIDLE, 32767}, std::pair{TCP_KEEPINTVL, 32767}, std::pair{TCP_KEEPCNT, 127}}) {
      before = archive(ptmc_state.sock_states[0]);
      for (int invalid : {0, -1, maximum + 1}) {
        require(emu_setsockopt(accepted1, IPPROTO_TCP, option, &invalid, sizeof(invalid)) == -EINVAL &&
                archive(ptmc_state.sock_states[0]) == before, "keepalive range rejection preserves state");
      }
      require(emu_setsockopt(accepted1, IPPROTO_TCP, option, reinterpret_cast<void*>(1), sizeof(int)) == -EFAULT &&
              archive(ptmc_state.sock_states[0]) == before, "keepalive pointer failure preserves state");
      require(emu_setsockopt(accepted1, IPPROTO_TCP, option, &maximum, sizeof(maximum)) == 0, "keepalive upper bound accepted");
    }
    before = archive(ptmc_state.sock_states[0]);
    require(emu_setsockopt(accepted1, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<void*>(1), sizeof(enabled)) == -EFAULT &&
            before == archive(ptmc_state.sock_states[0]), "bad option copy leaves state unchanged");
    std::istringstream input(before); SockState restored; {cereal::BinaryInputArchive in(input); in(restored);}
    ptmc_state.sock_states[0] = restored;
    int value = 0; socklen_t size = sizeof(value);
    require(emu_getsockopt(accepted1, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 && value == 1 && size == sizeof(value),
            "socket options survive serialization");
    for (const auto &[option, expected] : {std::pair{TCP_KEEPIDLE, 32767}, std::pair{TCP_KEEPINTVL, 32767}, std::pair{TCP_KEEPCNT, 127}}) {
      value = 0; size = sizeof(value);
      require(emu_getsockopt(accepted1, IPPROTO_TCP, option, &value, &size) == 0 && value == expected,
              "keepalive tuning survives serialization");
    }
    size = 2;
    require(emu_getsockopt(accepted1, IPPROTO_TCP, TCP_KEEPCNT, &value, &size) == 0 && size == 2,
            "truncated TCP option reports bytes copied");
    bool rejected = false; try {emu_setsockopt(accepted1, SOL_SOCKET, SO_RCVTIMEO, &enabled, sizeof(enabled));}
    catch (const std::runtime_error&) {rejected = true;} require(rejected, "unsupported option fails explicitly");
    full_connection_identity();
    stream_syscall_and_signal_boundaries();
    held_peer_endpoint_reuse();
    puts("network endpoint and option boundary regressions: PASS"); return 0;
  } catch (const std::exception &error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
