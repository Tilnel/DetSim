/*
 * sockstate.h - Socket state management for detsim
 */

#ifndef __SOCKSTATE_H
#define __SOCKSTATE_H

#include "common.h"
#include "fd_manager.h"
#include <deque>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <vector>

/* Guest memory operations - defined in guest.cpp */
extern void *memcpy_host2guest(void *dest, const void *src, size_t n);
extern void *memcpy_guest2host(void *dest, const void *src, size_t n);

/* ==============================================================================
 * Socket Address Utilities
 * ==============================================================================
 */

/* Convert sockaddr_in to display string: "ip:port" */
std::string format_sockaddr(const struct sockaddr_in &addr);

/* Parse address string to sockaddr_in */
bool parse_sockaddr(const std::string &str, struct sockaddr_in &addr);

/* ==============================================================================
 * Socket State Structures
 * ==============================================================================
 */

/* TCP connection state - identified by addresses, not fds */
struct TcpConnection
{
  // Connection identifiers (for debugging)
  int local_fd;

  // Connection endpoints
  struct sockaddr_in local_addr; // Our address
  struct sockaddr_in peer_addr;  // Remote address

  // Data buffer
  std::deque<std::string> recv_buffer;

  template <class Archive>
  void serialize(Archive &ar);
};

/* UDP datagram structure */
struct UdpDatagram
{
  std::string content;     // Payload
  struct sockaddr_in from; // Source address

  template <class Archive>
  void serialize(Archive &ar);
};

/* Represents a single socket */
struct Socket
{
  int fd;       // File descriptor (allocated by FdManager)
  int domain;   // AF_INET, AF_UNIX, etc.
  int type;     // SOCK_STREAM, SOCK_DGRAM
  int protocol; // Protocol number (usually 0)

  // Binding state
  bool bound;
  struct sockaddr_in local_addr;

  // Connection state (for TCP)
  bool connected;
  bool listening;
  struct sockaddr_in peer_addr;

  // For TCP listen: backlog queue
  int backlog;
  std::deque<int> pending_connections; // FDs of incoming connections

  // Established connections waiting for accept (kernel-managed)
  // Key: client address (IP:port) -> TcpConnection
  std::map<uint64_t, TcpConnection> established_connections;

  Socket()
      : fd(-1), domain(0), type(0), protocol(0), bound(false), connected(false),
        listening(false), backlog(0)
  {
  }

  template <class Archive>
  void serialize(Archive &ar);
};

struct EpollEvent
{
  uint32_t events;
  int fd;

  template <class Archive>
  void serialize(Archive &ar);
};

struct EpollInstance
{
  int epoll_fd;
  std::map<int, uint32_t> watched_fds;
  std::deque<EpollEvent> ready_events;

  EpollInstance() : epoll_fd(-1) {}

  template <class Archive>
  void serialize(Archive &ar);
};

/* ==============================================================================
 * SockState - Main Socket State Manager
 * ==============================================================================
 */

class SockState
{
  public:
  // Default constructor
  SockState() = default;

  // Copy control
  SockState(const SockState &other) = default;
  SockState &operator=(const SockState &other) = default;

  // Move control
  SockState(SockState &&other) noexcept = default;
  SockState &operator=(SockState &&other) noexcept = default;

  explicit SockState(FdManagerPtr fd_mgr) : fd_manager_(fd_mgr) {}

  /* Set the fd manager (must be called before using allocate_fd) */
  void set_fd_manager(FdManagerPtr fd_mgr) { fd_manager_ = fd_mgr; }

  /* --------------------------------------------------------------------------
   * Syscall Implementations
   * --------------------------------------------------------------------------
   */

  /* socket(domain, type, protocol) -> fd or -errno */
  int do_socket(int domain, int type, int protocol);

  /* bind(fd, addr, addrlen) -> 0 or -errno */
  int do_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);

  /* listen(fd, backlog) -> 0 or -errno */
  int do_listen(int fd, int backlog);

  /* connect(fd, addr, addrlen) -> 0 or -errno (TCP only) */
  int do_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);

  /* accept(fd, addr, addrlen) -> new_fd or -errno (TCP only) */
  int do_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);

  /* sendto(fd, buf, len, flags, dest_addr, addrlen) -> bytes_sent or -errno */
  ssize_t do_sendto(int fd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen);

  /* recvfrom(fd, buf, len, flags, src_addr, addrlen) -> bytes_recv or -errno */
  ssize_t do_recvfrom(int fd, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen);

  /* recvfrom with choice: 0=first message, 1=second message (for message
   * reordering) */
  ssize_t do_recvfrom_with_choice(int fd, void *buf, size_t len, int flags,
                                  struct sockaddr *src_addr, socklen_t *addrlen,
                                  int choice);

  /* send(fd, buf, len, flags) -> bytes_sent or -errno (TCP only) */
  ssize_t do_send(int fd, const void *buf, size_t len, int flags);

  /* recv(fd, buf, len, flags) -> bytes_recv or -errno (TCP only) */
  ssize_t do_recv(int fd, void *buf, size_t len, int flags);

  /* Get number of available choices for recvfrom (0, 1, or 2) */
  int get_recvfrom_choices(int fd) const;

  /* close(fd) -> 0 or -errno */
  int do_close(int fd);

  /* getsockname(fd, addr, addrlen) -> 0 or -errno */
  int do_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);

  /* getpeername(fd, addr, addrlen) -> 0 or -errno */
  int do_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

  /* --------------------------------------------------------------------------
   * Epoll Operations (for Redis)
   * --------------------------------------------------------------------------
   */

  /* epoll_create(size) -> fd or -errno */
  int do_epoll_create(int size);

  /* epoll_create1(flags) -> fd or -errno */
  int do_epoll_create1(int flags);

  /* epoll_ctl(epfd, op, fd, event) -> 0 or -errno */
  int do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

  /* epoll_wait(epfd, events, maxevents, timeout) -> num_events or -errno */
  int do_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                    int timeout);

  /* --------------------------------------------------------------------------
   * Cross-Process Message Delivery (for UDP)
   * --------------------------------------------------------------------------
   */

  /* Deliver a UDP datagram to this process's receive buffer */
  void deliver_udp_datagram(int recv_fd, const UdpDatagram &dg);

  /* Check if a socket fd is valid */
  bool is_valid_socket(int fd) const;

  /* Get socket info (for debugging) */
  const Socket *get_socket(int fd) const;

  /* Get UDP buffer size for a fd */
  size_t get_udp_buffer_size(int fd) const;

  /* Access all sockets (for serialization/debug) */
  const std::map<int, Socket> &sockets() const { return sockets_; }
  std::map<int, Socket> &sockets() { return sockets_; }

  /* Add a socket with specific fd (used by emu_socket) */
  void add_socket(const Socket &sock) { sockets_[sock.fd] = sock; }

  /* Access UDP receive buffers (for cross-process delivery) */
  const std::map<int, std::deque<UdpDatagram>> &udp_recv_buffers() const
  {
    return udp_recv_buffers_;
  }
  std::map<int, std::deque<UdpDatagram>> &udp_recv_buffers()
  {
    return udp_recv_buffers_;
  }

  /* Access TCP connections */
  const std::map<int, TcpConnection> &tcp_connections() const
  {
    return tcp_connections_;
  }

  /* Access epoll instances */
  const std::map<int, EpollInstance> &epoll_instances() const
  {
    return epoll_instances_;
  }

  /* --------------------------------------------------------------------------
   * Serialization
   * --------------------------------------------------------------------------
   */
  template <class Archive>
  void serialize(Archive &ar);

  /* --------------------------------------------------------------------------
   * Debug/Info
   * --------------------------------------------------------------------------
   */
  void dump_state() const;

  private:
  FdManagerPtr fd_manager_;

  /* All sockets indexed by fd */
  std::map<int, Socket> sockets_;

  /* UDP receive buffers: fd -> queue of datagrams */
  std::map<int, std::deque<UdpDatagram>> udp_recv_buffers_;

  /* TCP connections: local_fd -> connection state */
  std::map<int, TcpConnection> tcp_connections_;

  /* Epoll instances: epoll_fd -> epoll state */
  std::map<int, EpollInstance> epoll_instances_;

  /* Helper: allocate new fd via FdManager */
  int allocate_fd(FdType type = FdType::SOCKET);

  /* Helper: release fd */
  void release_fd(int fd);

  /* Helper: get socket by fd (non-const) */
  Socket *get_socket_mutable(int fd);

  /* Helper: find socket by local address (for UDP routing) */
  int find_socket_by_local_addr(const struct sockaddr_in &addr) const;

  /* Helper: find socket by bound port (for UDP routing) */
  int find_socket_by_port(uint16_t port) const;
};

/* ==============================================================================
 * Legacy C-style API (implemented in sockstate.cpp)
 * These functions delegate to SockState methods
 * ==============================================================================
 */

int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t emu_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);
int emu_recvfrom_get_choices(int sockfd);
ssize_t emu_sendto(int sockfd, const void *buf, size_t len, int flags,
                   struct sockaddr *dest_addr, socklen_t addrlen);
int emu_socket(int domain, int type, int protocol);
int emu_listen(int sockfd, int backlog);
int emu_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

int emu_epoll_create(int size);
int emu_epoll_create1(int flags);
int emu_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int emu_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                   int timeout);

ssize_t emu_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t emu_recv(int sockfd, void *buf, size_t len, int flags);

int emu_setsockopt(int sockfd, int level, int optname, const void *optval,
                   socklen_t optlen);
int emu_getsockopt(int sockfd, int level, int optname, void *optval,
                   socklen_t *optlen);

int emu_fsync(int fd);
int emu_fdatasync(int fd);

int emu_fcntl(int fd, int cmd, long arg);

int emu_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

typedef std::deque<UdpDatagram> udp_buffer;

#endif /* __SOCKSTATE_H */
