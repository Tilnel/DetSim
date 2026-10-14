#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include <arpa/inet.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}

// Reserve this range inaccessible in the parent, writable only in the child.
// Directly dereferencing guest pointers must fault even after fork().
struct Guest
{
  void *page =
      mmap(nullptr, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  pid_t pid = -1;
  Guest()
  {
    require(page != MAP_FAILED, "mmap");
    int ready[2];
    require(pipe(ready) == 0, "pipe");
    pid = fork();
    require(pid >= 0, "fork");
    if (!pid)
    {
      close(ready[0]);
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (mprotect(page, 8192, PROT_READ | PROT_WRITE))
        _exit(2);
      *reinterpret_cast<socklen_t *>(static_cast<char *>(page) + 4096) = 32;
      if (mprotect(static_cast<char *>(page) + 4096, 4096, PROT_READ))
        _exit(4);
      char c = 'r';
      if (write(ready[1], &c, 1) != 1)
        _exit(3);
      close(ready[1]);
      for (;;)
        pause();
    }
    close(ready[1]);
    char c = 0;
    ssize_t n = read(ready[0], &c, 1);
    close(ready[0]);
    require(n == 1 && c == 'r', "guest ready");
  }
  ~Guest()
  {
    if (pid > 0)
    {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
    munmap(page, 8192);
  }
  void transfer(void *local, size_t offset, size_t size, bool write)
  {
    iovec l{local, size}, r{static_cast<char *>(page) + offset, size};
    ssize_t n = write ? process_vm_writev(pid, &l, 1, &r, 1, 0)
                      : process_vm_readv(pid, &l, 1, &r, 1, 0);
    require(n == static_cast<ssize_t>(size), "guest transfer");
  }
};

static void check_accept(Guest &guest, socklen_t capacity, int mode,
                         bool collision)
{
  auto manager = std::make_shared<FdManager>();
  ptmc_state.sock_states[0] = SockState(manager);
  ptmc_state.sock_states[1] = SockState(std::make_shared<FdManager>());
  ptmc_state.addrs[0] = "127.0.0.1";
  ptmc_state.addrs[1] = "127.0.0.2";
  ptmc_state.pids[0] = ptmc_state.pids[1] = getpid();
  auto &server = ptmc_state.sock_states[0];
  auto &client = ptmc_state.sock_states[1];
  ptmc_state.cursor = 0;
  int listener = server.do_socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(6379);
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(server.do_bind(listener, reinterpret_cast<sockaddr *>(&local),
                         sizeof(local)) == 0,
          "bind");
  require(server.do_listen(listener, 10) == 0, "listen");
  ptmc_state.cursor = 1;
  if (!collision)
    client.do_socket(AF_INET, SOCK_STREAM, 0);
  int client_fd = client.do_socket(AF_INET, SOCK_STREAM, 0);
  require(client.do_connect(client_fd, reinterpret_cast<sockaddr *>(&local),
                            sizeof(local)) == 0,
          "connect");
  sockaddr_in expected{};
  expected.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.2", &expected.sin_addr);
  expected.sin_port = client.get_socket(client_fd)->local_addr.sin_port;
  ptmc_state.cursor = 0;
  ptmc_state.pids[0] = guest.pid;
  unsigned char before[32];
  memset(before, 0xa5, sizeof(before));
  guest.transfer(before, 0, sizeof(before), true);
  guest.transfer(&capacity, 64, sizeof(capacity), true);
  auto *addr = reinterpret_cast<sockaddr *>(guest.page);
  auto *len =
      reinterpret_cast<socklen_t *>(static_cast<char *>(guest.page) + 64);
  if (mode == 1)
    len = reinterpret_cast<socklen_t *>(1);
  if (mode == 2)
    addr = reinterpret_cast<sockaddr *>(1);
  if (mode == 3)
  {
    addr = nullptr;
    len = reinterpret_cast<socklen_t *>(1);
  }
  if (mode == 4)
    len = nullptr;
  if (mode == 5)
    len = reinterpret_cast<socklen_t *>(static_cast<char *>(guest.page) + 4096);
  if (mode == 6)
    addr = reinterpret_cast<sockaddr *>(static_cast<char *>(guest.page) + 4096 -
                                        8);
  int next_fd = manager->get_next_fd();
  int result = server.do_accept(listener, addr, len);
  if (mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 6 ||
      capacity == UINT_MAX)
  {
    require(result == (capacity == UINT_MAX ? -EINVAL : -EFAULT),
            "invalid guest argument errno");
    require(manager->get_next_fd() == next_fd, "failed accept allocated fd");
    require(server.get_socket(listener)->pending_connections.size() == 1,
            "failed accept consumed connection");
    require(server.do_accept(listener, nullptr, nullptr) >= 0,
            "retry after failed accept");
    return;
  }
  require(result >= 0, "accept succeeded");
  require(server.get_socket(listener)->pending_connections.empty(),
          "accepted connection dequeued");
  require(server.do_accept(listener, nullptr, nullptr) == -EAGAIN,
          "empty accept queue");
  if (mode == 3)
    return;
  socklen_t actual = 0;
  guest.transfer(&actual, 64, sizeof(actual), false);
  require(actual == sizeof(expected), "actual sockaddr length copied out");
  unsigned char after[32];
  guest.transfer(after, 0, sizeof(after), false);
  size_t copied = std::min<size_t>(capacity, sizeof(expected));
  require(memcmp(after, &expected, copied) == 0, "peer address copied out");
  require(memcmp(after + copied, before + copied, sizeof(after) - copied) == 0,
          "copyout respects capacity");
}

static void check_closed_pending_client()
{
  auto manager = std::make_shared<FdManager>();
  ptmc_state.sock_states[0] = SockState(manager);
  ptmc_state.sock_states[1] = SockState(std::make_shared<FdManager>());
  ptmc_state.addrs[0] = "127.0.0.1";
  ptmc_state.addrs[1] = "127.0.0.2";
  ptmc_state.pids[0] = ptmc_state.pids[1] = getpid();
  auto &server = ptmc_state.sock_states[0];
  auto &client = ptmc_state.sock_states[1];
  ptmc_state.cursor = 0;
  int listener = server.do_socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(6379);
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(server.do_bind(listener, reinterpret_cast<sockaddr *>(&local),
                         sizeof(local)) == 0,
          "stale queue bind");
  require(server.do_listen(listener, 10) == 0, "stale queue listen");
  ptmc_state.cursor = 1;
  int closed_fd = client.do_socket(AF_INET, SOCK_STREAM, 0);
  int live_fd = client.do_socket(AF_INET, SOCK_STREAM, 0);
  require(client.do_connect(closed_fd, reinterpret_cast<sockaddr *>(&local),
                            sizeof(local)) == 0,
          "first queued connect");
  require(client.do_connect(live_fd, reinterpret_cast<sockaddr *>(&local),
                            sizeof(local)) == 0,
          "second queued connect");
  require(client.do_close(closed_fd) == 0, "close pending client");
  ptmc_state.cursor = 0;
  int closed_accepted = server.do_accept(listener, nullptr, nullptr);
  require(closed_accepted >= 0, "closed pending client remains acceptable");
  require(server.do_recv(closed_accepted, nullptr, 1, 0) == 0 &&
          server.do_recv(closed_accepted, nullptr, 1, 0) == 0,
          "closed pending client yields persistent EOF");
  require(server.get_socket(listener)->pending_connections.size() == 1,
          "accept must consume exactly one pending peer");
  int accepted = server.do_accept(listener, nullptr, nullptr);
  require(accepted >= 0, "live queued client remains reachable");
  require(server.get_socket(accepted)->peer_addr.sin_port ==
              client.get_socket(live_fd)->local_addr.sin_port,
          "accepted live peer");
  require(server.do_accept(listener, nullptr, nullptr) == -EAGAIN,
          "stale queue drained");
}

static void native_closed_pending_oracle()
{
  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  require(listener >= 0, "native listener");
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
          "native bind");
  socklen_t length = sizeof(local);
  require(getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length) == 0,
          "native getsockname");
  require(listen(listener, 1) == 0, "native listen");
  int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  require(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
          "native connect");
  close(client);
  int accepted = accept(listener, nullptr, nullptr);
  require(accepted >= 0, "native accept after client close");
  char byte;
  require(recv(accepted, &byte, 1, 0) == 0 && recv(accepted, &byte, 1, 0) == 0,
          "native closed pending connection gives persistent EOF");
  close(accepted);
  close(listener);
}

int main()
{
  static_assert(NP == 2);
  loglevel = 4;
  rlimit no_core{0, 0};
  setrlimit(RLIMIT_CORE, &no_core);
  try
  {
    native_closed_pending_oracle();
    Guest guest;
    check_accept(guest, 32, 0, false);
    check_accept(guest, 2, 0, false);
    check_accept(guest, 0, 0, false);
    check_accept(guest, 32, 1, false);
    check_accept(guest, 32, 2, false);
    check_accept(guest, 32, 3, false);
    check_accept(guest, 32, 4, false);
    check_accept(guest, 32, 5, false);
    check_accept(guest, 32, 6, false);
    check_accept(guest, UINT_MAX, 0, false);
    check_accept(guest, 32, 0, true);
    check_closed_pending_client();
    puts("accept integration: 12 cases passed");
  }
  catch (const std::exception &e)
  {
    fprintf(stderr, "accept regression: %s\n", e.what());
    return 1;
  }
}
