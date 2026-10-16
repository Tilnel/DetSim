/*
 * sockstate.cpp - Socket state implementation
 */

#include "sockstate.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/deque.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/vector.hpp"
#include "debug.h"
#include "monitor.h"
#include "guest.h"
#include <stdexcept>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <climits>
#include <netinet/tcp.h>

/* ==============================================================================
 * Utility Functions
 * ==============================================================================
 */

std::string format_sockaddr(const struct sockaddr_in &addr)
{
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return fmt::format("{}:{}", ip, ntohs(addr.sin_port));
}

bool parse_sockaddr(const std::string &str, struct sockaddr_in &addr)
{
  size_t colon = str.find(':');
  if (colon == std::string::npos)
    return false;

  std::string ip_str = str.substr(0, colon);
  int port = std::stoi(str.substr(colon + 1));

  addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  return inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) == 1;
}

/* ==============================================================================
 * SockState Private Helpers
 * ==============================================================================
 */

int SockState::allocate_fd(FdType type, int status_flags, int descriptor_flags)
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(type, status_flags, descriptor_flags);
}

void SockState::release_fd(int fd)
{
  if (fd_manager_)
  {
    fd_manager_->release_fd(fd);
  }
}

int SockState::invalid_socket_error(int fd) const
{
  return fd_manager_ && fd_manager_->is_allocated(fd) ? -ENOTSOCK : -EBADF;
}

Socket *SockState::get_socket_mutable(int fd)
{
  auto it = sockets_.find(fd);
  if (it != sockets_.end())
    return &it->second;
  return nullptr;
}

int SockState::find_socket_by_local_addr(const struct sockaddr_in &addr) const
{
  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    if (sock.bound && sock.local_addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
        sock.local_addr.sin_port == addr.sin_port)
    {
      return kv.first;
    }
  }
  return -1;
}

int SockState::find_socket_by_port(uint16_t port) const
{
  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    if (sock.bound && sock.local_addr.sin_port == port)
    {
      return kv.first;
    }
  }
  return -1;
}

/* ==============================================================================
 * SockState Public API - Socket Operations
 * ==============================================================================
 */

int SockState::do_socket(int domain, int type, int protocol)
{
  if (domain != AF_INET)
  {
    LOG_ERROR("socket: unsupported domain %d", domain);
    return -EAFNOSUPPORT;
  }

  // Extract socket type flags (SOCK_NONBLOCK, SOCK_CLOEXEC are Linux-specific)
  int type_flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM)
  {
    LOG_ERROR("socket: unsupported type %d (base_type=%d, flags=%d)", type, base_type, type_flags);
    return -EPROTONOSUPPORT;
  }

  const int effective_protocol = base_type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
  if (protocol != 0 && protocol != effective_protocol) return -EPROTONOSUPPORT;
  int fd = allocate_fd(FdType::SOCKET, O_RDWR | ((type_flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0),
                       (type_flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
  if (fd < 0)
  {
    return -EMFILE;
  }

  Socket sock;
  sock.fd = fd;
  sock.domain = domain;
  sock.type = base_type;
  sock.protocol = effective_protocol;
  sock.bound = false;
  sock.connected = false;
  sock.listening = false;

  sockets_[fd] = sock;

  LOG_TRACE("socket(%d, %d, %d) -> fd=%d (base_type=%d, flags=%d)", domain, type, protocol, fd, base_type, type_flags);
  return fd;
}

int SockState::do_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (sock->bound)
  {
    return -EINVAL;
  }

  if (addrlen < sizeof(struct sockaddr_in))
  {
    return -EINVAL;
  }

  /* addr is guest pointer, need to read from guest memory */
  struct sockaddr_in sin{};
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], addr, &sin, sizeof(sin)))
    return -EFAULT;
  if (sin.sin_family != AF_INET) return -EAFNOSUPPORT;
  for (const auto &[other_fd, other] : sockets_) {
    if (other_fd == fd || !other.bound || other.type != sock->type ||
        other.local_addr.sin_port != sin.sin_port || sin.sin_port == 0) continue;
    if (sin.sin_addr.s_addr == INADDR_ANY || other.local_addr.sin_addr.s_addr == INADDR_ANY ||
        sin.sin_addr.s_addr == other.local_addr.sin_addr.s_addr) return -EADDRINUSE;
  }
  memset(sin.sin_zero, 0, sizeof(sin.sin_zero));
  sock->local_addr = sin;
  sock->bound = true;

  LOG_TRACE("bind(fd=%d, %s) = 0", fd, format_sockaddr(sin).c_str());
  return 0;
}

static int copy_socket_address(const sockaddr_in &address, sockaddr *output, socklen_t *length)
{
  int pid = ptmc_state.pids[ptmc_state.cursor];
  socklen_t capacity = 0;
  if (!guest_read_exact(pid, length, &capacity, sizeof(capacity))) return -EFAULT;
  if (capacity > INT_MAX) return -EINVAL;
  if (!guest_write_exact(pid, output, &address, std::min<size_t>(capacity, sizeof(address))))
    return -EFAULT;
  socklen_t actual = sizeof(address);
  return guest_write_exact(pid, length, &actual, sizeof(actual)) ? 0 : -EFAULT;
}

int SockState::do_getsockname(int fd, sockaddr *address, socklen_t *length)
{
  const Socket *socket = get_socket(fd);
  if (!socket) return invalid_socket_error(fd);
  sockaddr_in local = socket->local_addr;
  local.sin_family = AF_INET;
  return copy_socket_address(local, address, length);
}

int SockState::do_getpeername(int fd, sockaddr *address, socklen_t *length)
{
  const Socket *socket = get_socket(fd);
  if (!socket) return invalid_socket_error(fd);
  if (!socket->connected) return -ENOTCONN;
  return copy_socket_address(socket->peer_addr, address, length);
}

int SockState::do_listen(int fd, int backlog)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (sock->type != SOCK_STREAM)
  {
    return -EOPNOTSUPP;
  }

  if (!sock->bound)
  {
    return -EDESTADDRREQ;
  }

  sock->listening = true;
  sock->backlog = std::max(0, backlog);

  LOG_TRACE("listen(fd=%d, backlog=%d) = 0", fd, backlog);
  return 0;
}

int SockState::do_connect(int fd, const struct sockaddr *addr,
                          socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (sock->type != SOCK_STREAM)
  {
    return -EOPNOTSUPP;
  }

  if (sock->connected) return -EISCONN;
  if (sock->listening) return -EINVAL;
  if (addrlen < sizeof(sockaddr_in)) return -EINVAL;
  struct sockaddr_in peer_addr{};
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], addr, &peer_addr, sizeof(peer_addr)))
    return -EFAULT;
  if (peer_addr.sin_family != AF_INET) return -EAFNOSUPPORT;

  // A connected socket uses the node's concrete source address, not ANY.
  // Stage both address and port so allocation failures leave the socket intact.
  sockaddr_in local_addr = sock->bound ? sock->local_addr : sockaddr_in{};
  local_addr.sin_family = AF_INET;
  if (local_addr.sin_addr.s_addr == INADDR_ANY)
  {
    if (inet_pton(AF_INET, ptmc_state.addrs[ptmc_state.cursor].c_str(),
                  &local_addr.sin_addr) != 1 ||
        local_addr.sin_addr.s_addr == INADDR_ANY)
      return -EADDRNOTAVAIL;
  }

  auto reciprocal_tuple = [&](const Socket &candidate) {
    return candidate.type == SOCK_STREAM && candidate.connected &&
        candidate.local_addr.sin_addr.s_addr == peer_addr.sin_addr.s_addr &&
        candidate.local_addr.sin_port == peer_addr.sin_port &&
        candidate.peer_addr.sin_addr.s_addr == local_addr.sin_addr.s_addr;
  };
  auto pending_tuple = [&](const TcpConnection &connection) {
    return connection.local_addr.sin_addr.s_addr == local_addr.sin_addr.s_addr &&
        connection.peer_addr.sin_addr.s_addr == peer_addr.sin_addr.s_addr &&
        connection.peer_addr.sin_port == peer_addr.sin_port;
  };
  // Allocate from this node's saved socket state, never the search's host RNG.
  // Keep the existing modeled range, not the host's configurable Linux range.
  if (!sock->bound || local_addr.sin_port == 0)
  {
    constexpr int first_port = 10000;
    constexpr int port_count = 50000;
    bool used[port_count]{};
    for (const auto &[other_fd, other] : sockets_)
    {
      if (!other.bound)
        continue;
      int port = ntohs(other.local_addr.sin_port);
      if (port >= first_port && port < first_port + port_count)
        used[port - first_port] = true;
    }
    // A closed client can leave a live accepted or pending peer. Reusing its
    // tuple would route a new connection into that historical peer's queue.
    // Reserve only this destination's reciprocal tuples, so unrelated endpoints
    // remain independently reusable.
    auto reserve = [&](in_port_t network_port) {
      int port = ntohs(network_port);
      if (port >= first_port && port < first_port + port_count)
        used[port - first_port] = true;
    };
    for (int node = 0; node < NP; ++node) {
      for (const auto &[remote_fd, remote] : ptmc_state.sock_states[node].sockets_) {
        if (reciprocal_tuple(remote)) reserve(remote.peer_addr.sin_port);
        if (!remote.listening) continue;
        for (const auto &[key, connection] : remote.established_connections)
          if (pending_tuple(connection)) reserve(connection.local_addr.sin_port);
      }
    }
    int index = 0;
    while (index < port_count && used[index])
      ++index;
    if (index == port_count)
      return -EADDRNOTAVAIL;
    local_addr.sin_port = htons(first_port + index);
  }
  if (sock->bound && sock->local_addr.sin_port != 0) {
    for (int node = 0; node < NP; ++node) {
      for (const auto &[remote_fd, remote] : ptmc_state.sock_states[node].sockets_) {
        if (reciprocal_tuple(remote) && remote.peer_addr.sin_port == local_addr.sin_port)
          throw std::runtime_error("TCP bound endpoint reuse with a surviving peer is unsupported");
        if (!remote.listening) continue;
        for (const auto &[key, connection] : remote.established_connections)
          if (pending_tuple(connection) && connection.local_addr.sin_port == local_addr.sin_port)
            throw std::runtime_error("TCP bound endpoint reuse with a pending peer is unsupported");
      }
    }
  }
  Socket *listener = nullptr;
  for (int node = 0; node < NP; ++node) {
    for (auto &[listen_fd, candidate] : ptmc_state.sock_states[node].sockets_) {
      if (!candidate.listening || candidate.type != SOCK_STREAM ||
          candidate.local_addr.sin_port != peer_addr.sin_port) continue;
      in_addr effective = candidate.local_addr.sin_addr;
      if (effective.s_addr == INADDR_ANY &&
          inet_pton(AF_INET, ptmc_state.addrs[node].c_str(), &effective) != 1) continue;
      if (effective.s_addr != peer_addr.sin_addr.s_addr) continue;
      if (listener) throw std::runtime_error("Ambiguous TCP listener endpoint");
      listener = &candidate;
    }
  }
  if (!listener) return -ECONNREFUSED;
  if (listener->established_connections.size() >= size_t(listener->backlog))
    throw std::runtime_error("TCP backlog overflow requires unsupported blocking/connect state");
  uint64_t client_key = (uint64_t(local_addr.sin_addr.s_addr) << 32) | local_addr.sin_port;
  if (listener->established_connections.count(client_key))
    throw std::runtime_error("TCP endpoint reuse while an older connection is pending is unsupported");
  // All validation precedes state mutation. The FIFO identifies peers by their
  // endpoint, so equal process-local fd numbers never create ambiguity.
  sock->bound = true;
  sock->local_addr = local_addr;
  sock->connected = true;
  memset(peer_addr.sin_zero, 0, sizeof(peer_addr.sin_zero));
  sock->peer_addr = peer_addr;
  TcpConnection pending;
  pending.local_fd = fd;
  pending.local_addr = local_addr;
  pending.peer_addr = peer_addr;
  listener->pending_connections.push_back(client_key);
  listener->established_connections.emplace(client_key, std::move(pending));

  // Create or update TCP connection in our own process
  TcpConnection conn;
  conn.local_fd = fd;
  conn.local_addr = sock->local_addr;
  conn.peer_addr = peer_addr;
  tcp_connections_[fd] = conn;

  LOG_TRACE("connect(fd=%d, addr=%s) = 0", fd,
            format_sockaddr(peer_addr).c_str());
  return 0;
}

int SockState::do_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  Socket *listener = get_socket_mutable(fd);
  if (!listener)
  {
    return invalid_socket_error(fd);
  }

  if (listener->type != SOCK_STREAM)
  {
    return -EOPNOTSUPP;
  }

  if (!listener->listening)
  {
    return -EINVAL;
  }

  if (listener->pending_connections.empty())
  {
    return -EAGAIN; // No pending connections
  }

  const uint64_t client_key = listener->pending_connections.front();
  auto established_it = listener->established_connections.find(client_key);
  if (established_it == listener->established_connections.end()) {
    listener->pending_connections.pop_front();
    return -ECONNRESET;
  }
  const TcpConnection &pending = established_it->second;
  const int client_fd = pending.local_fd;
  // The source endpoint and source fd come from the saved connection, rather
  // than a scan which can select another node's same-numbered descriptor.
  // Create new socket for accepted connection
  Socket new_sock;
  new_sock.local_addr = {};
  new_sock.peer_addr = {};
  new_sock.domain = listener->domain;
  new_sock.type = SOCK_STREAM;
  new_sock.protocol = listener->protocol;
  new_sock.bound = true;
  new_sock.connected = true;
  new_sock.reuse_address = listener->reuse_address;
  new_sock.no_delay = listener->no_delay;
  new_sock.keep_alive = listener->keep_alive;
  new_sock.keep_idle = listener->keep_idle;
  new_sock.keep_interval = listener->keep_interval;
  new_sock.keep_count = listener->keep_count;

  new_sock.local_addr = pending.peer_addr;

  new_sock.peer_addr = pending.local_addr;

  // addr and addrlen belong to the tracee. Checked copies are required here:
  // the general guest helpers cannot report a failed/short transfer to callers.
  if (addr)
  {
    int guest_pid = ptmc_state.pids[ptmc_state.cursor];
    socklen_t capacity = 0;
    if (!guest_read_exact(guest_pid, addrlen, &capacity, sizeof(capacity)))
      return -EFAULT;
    if (capacity > INT_MAX) return -EINVAL;
    size_t copied = std::min<size_t>(capacity, sizeof(new_sock.peer_addr));
    if (!guest_write_exact(guest_pid, addr, &new_sock.peer_addr, copied))
      return -EFAULT;
    socklen_t actual = sizeof(new_sock.peer_addr);
    if (!guest_write_exact(guest_pid, addrlen, &actual, sizeof(actual)))
      return -EFAULT;
  }

  // Commit virtual socket state only after successful copyout.
  int new_fd = allocate_fd(FdType::SOCKET);
  if (new_fd < 0)
    return -EMFILE;
  new_sock.fd = new_fd;
  listener->pending_connections.pop_front();
  sockets_[new_fd] = new_sock;

  // Adopt the existing TcpConnection with its buffered data
  TcpConnection conn = std::move(established_it->second);
  conn.local_fd = new_fd;
  conn.local_addr = new_sock.local_addr;
  conn.peer_addr = new_sock.peer_addr;
  tcp_connections_[new_fd] = std::move(conn);

  listener->established_connections.erase(established_it);

  LOG_TRACE("accept(fd=%d) = %d (client fd=%d)", fd, new_fd, client_fd);
  return new_fd;
}

ssize_t SockState::do_send(int fd, const void *buf, size_t len, int flags)
{
  if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
    throw std::runtime_error("Unsupported TCP send flags");
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (!sock->connected)
  {
    return -ENOTCONN;
  }

  if (len == 0) return 0;
  std::string data(len, '\0');
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], buf, data.data(), len))
    return -EFAULT;

  // Compute our address key to look up in peer's established_connections
  uint64_t our_key = ((uint64_t)sock->local_addr.sin_addr.s_addr << 32) |
                     sock->local_addr.sin_port;

  // Route by address: find the peer and deliver data
  for (int pid = 0; pid < NP; pid++)
  {
    SockState &remote_state = ptmc_state.sock_states[pid];

    // First: try to find an accepted connection (in tcp_connections_)
    for (auto &kv : remote_state.sockets_)
    {
      Socket &remote_sock = kv.second;
      if (!remote_sock.connected)
        continue;

      // Get effective local address: if 0.0.0.0, use config addr
      uint32_t effective_addr = remote_sock.local_addr.sin_addr.s_addr;
      if (effective_addr == INADDR_ANY)
      {
        struct in_addr config_addr;
        if (inet_pton(AF_INET, ptmc_state.addrs[pid].c_str(), &config_addr) ==
            1)
        {
          effective_addr = config_addr.s_addr;
        }
      }

      if (effective_addr == sock->peer_addr.sin_addr.s_addr &&
          remote_sock.local_addr.sin_port == sock->peer_addr.sin_port &&
          remote_sock.peer_addr.sin_addr.s_addr == sock->local_addr.sin_addr.s_addr &&
          remote_sock.peer_addr.sin_port == sock->local_addr.sin_port)
      {
        auto conn_it = remote_state.tcp_connections_.find(kv.first);
        if (conn_it != remote_state.tcp_connections_.end())
        {
          conn_it->second.recv_buffer.push_back(data);
          LOG_TRACE("send(fd=%d, len=%zu) -> peer fd=%d in process %d", fd, len,
                    kv.first, pid);
          return len;
        }
      }
    }

    // Second: try to find a pending connection in listener's
    // established_connections
    for (auto &kv : remote_state.sockets_)
    {
      Socket &remote_sock = kv.second;
      if (!remote_sock.listening)
        continue;

      // Check if this listener has our connection in established_connections
      auto established_it = remote_sock.established_connections.find(our_key);
      if (established_it != remote_sock.established_connections.end() &&
          established_it->second.peer_addr.sin_addr.s_addr == sock->peer_addr.sin_addr.s_addr &&
          established_it->second.peer_addr.sin_port == sock->peer_addr.sin_port)
      {
        established_it->second.recv_buffer.push_back(data);
        LOG_TRACE(
            "send(fd=%d, len=%zu) -> pending connection on listener fd=%d "
            "in process %d",
            fd, len, kv.first, pid);
        return len;
      }
    }
  }

  return -ECONNRESET;
}

ssize_t SockState::do_recv(int fd, void *buf, size_t len, int flags)
{
  if (flags & ~(MSG_DONTWAIT | MSG_PEEK))
    throw std::runtime_error("Unsupported TCP receive flags");
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (!sock->connected)
  {
    return -ENOTCONN;
  }

  auto it = tcp_connections_.find(fd);
  if (it == tcp_connections_.end())
  {
    return -ECONNRESET;
  }

  if (len == 0) return 0;
  TcpConnection &conn = it->second;
  if (conn.recv_buffer.empty())
  {
    return -EAGAIN; // No data available
  }

  // Get first message
  std::string &data = conn.recv_buffer.front();

  // Empty string is EOF marker (peer closed connection)
  if (data.empty())
  {
    return 0; // Persistent EOF
  }

  size_t to_copy = len < data.size() ? len : data.size();
  if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], buf, data.data(), to_copy))
    return -EFAULT;

  if (flags & MSG_PEEK) return to_copy;
  if (to_copy < data.size())
  {
    // Partial read, keep remaining data
    data = data.substr(to_copy);
  }
  else
  {
    // Full read, remove from buffer
    conn.recv_buffer.pop_front();
  }

  LOG_TRACE("recv(fd=%d, len=%zu) = %zu", fd, len, to_copy);
  return to_copy;
}

/* ==============================================================================
 * SockState Public API - Data Transfer (UDP)
 * ==============================================================================
 */

static int addr_to_node(const sockaddr_in *addr);

ssize_t SockState::do_sendto(int fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, socklen_t addrlen)
{
  if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
    throw std::runtime_error("Unsupported UDP send flags");
  LOG_TRACE("emu_sendto(%d, %p, %ld, %d, %p, %d)", fd, (void *)buf, len,
            flags, (void *)dest_addr, addrlen);

  int cur = ptmc_state.cursor;
  const Socket *sock = get_socket(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (sock->type == SOCK_STREAM) {
    if (dest_addr) throw std::runtime_error("TCP sendto with explicit destination is unsupported");
    return do_send(fd, buf, len, flags);
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  /* Get destination info */
  struct sockaddr_in dest{};
  if (dest_addr)
  {
    if (addrlen < sizeof(dest)) return -EINVAL;
    if (!guest_read_exact(ptmc_state.pids[cur], dest_addr, &dest, sizeof(dest)))
      return -EFAULT;
    if (dest.sin_family != AF_INET) return -EAFNOSUPPORT;
  }
  else if (sock->connected)
  {
    dest = sock->peer_addr;
  }
  else
  {
    return -EDESTADDRREQ;
  }

  int node = addr_to_node(&dest);
  if (node < 0)
  {
    return -ENETUNREACH;
  }

  uint16_t port = dest.sin_port;
  if (!port) return -EINVAL;
  std::string data(len, '\0');
  if (!guest_read_exact(ptmc_state.pids[cur], buf, data.data(), len)) return -EFAULT;

  /* Find receiving socket by port */
  int recvfd = -1;
  const auto &recv_sockets = ptmc_state.sock_states[node].sockets();
  for (const auto &kv : recv_sockets)
  {
    const Socket &rs = kv.second;
    if (rs.type == SOCK_DGRAM && rs.bound && rs.local_addr.sin_port == port &&
        (rs.local_addr.sin_addr.s_addr == INADDR_ANY ||
         rs.local_addr.sin_addr.s_addr == dest.sin_addr.s_addr))
    {
      recvfd = kv.first;
      break;
    }
  }

  if (recvfd == -1)
  {
    /* No receiver - drop packet but report success */
    return len;
  }

  UdpDatagram dg;
  dg.content = std::move(data);
  dg.from = {};

  /* Set source address */
  dg.from.sin_family = AF_INET;
  dg.from.sin_port = sock->bound ? sock->local_addr.sin_port : 0;
  dg.from.sin_addr = sock->local_addr.sin_addr;
  if (dg.from.sin_addr.s_addr == INADDR_ANY &&
      inet_pton(AF_INET, ptmc_state.addrs[cur].c_str(), &dg.from.sin_addr) != 1)
    return -EADDRNOTAVAIL;

  /* Deliver to receiver */
  ptmc_state.sock_states[node].deliver_udp_datagram(recvfd, dg);

  LOG_TRACE("emu_sendto: delivered %zu bytes to node %d fd %d", len, node,
            recvfd);

  return len;
}

ssize_t SockState::do_recvfrom(int fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, socklen_t *addrlen)
{
  // Default: use choice 0 (receive first message)
  return do_recvfrom_with_choice(fd, buf, len, flags, src_addr, addrlen, 0);
}

ssize_t SockState::do_recvfrom_with_choice(int fd, void *buf, size_t len,
                                           int flags, struct sockaddr *src_addr,
                                           socklen_t *addrlen, int choice)
{
  if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC))
    throw std::runtime_error("Unsupported UDP receive flags");
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return invalid_socket_error(fd);
  }

  if (sock->type == SOCK_STREAM) {
    if (src_addr || addrlen) throw std::runtime_error("TCP recvfrom source address is unsupported");
    return do_recv(fd, buf, len, flags);
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  auto it = udp_recv_buffers_.find(fd);
  if (it == udp_recv_buffers_.end() || it->second.empty())
  {
    return -EAGAIN;
  }

  // Validate choice
  if (choice < 0 || choice > 1)
  {
    return -EAGAIN;
  }

  // Have only one datagram but postpone
  if (choice == 1 && static_cast<int>(it->second.size()) == 1)
  {
    ptmc_state.error_bound--;
    return -EAGAIN;
  }

  // Get iterator to chosen datagram
  auto dg_it = it->second.begin();
  if (choice == 1 && it->second.size() >= 2)
  {
    // Choice 1: receive second message (for message reordering)
    ++dg_it;
  }

  UdpDatagram &dg = *dg_it;

  /* Copy data to guest buffer */
  size_t copy_len = std::min(len, dg.content.size());
  if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], buf, dg.content.data(), copy_len))
    return -EFAULT;

  /* Handle output parameters - src_addr and addrlen are guest pointers */
  if (src_addr)
  {
    /* Read input addrlen value from guest */
    socklen_t guest_addrlen = 0;
    if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], addrlen, &guest_addrlen, sizeof(guest_addrlen)))
      return -EFAULT;

    /* Calculate actual address length to write */
    socklen_t addr_len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));

    /* Write source address to guest */
    if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], src_addr, &dg.from, addr_len))
      return -EFAULT;

    /* Write actual address length back to guest */
    socklen_t actual = sizeof(sockaddr_in);
    if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], addrlen, &actual, sizeof(actual)))
      return -EFAULT;
  }

  size_t result_len = (flags & MSG_TRUNC) ? dg.content.size() : copy_len;
  std::string source = format_sockaddr(dg.from);
  if (choice == 1) --ptmc_state.error_bound;
  if (!(flags & MSG_PEEK)) it->second.erase(dg_it);

  LOG_TRACE("recvfrom(fd=%d, choice=%d) -> len=%zu from %s", fd, choice,
            copy_len, source.c_str());

  return result_len;
}

int SockState::get_recvfrom_choices(int fd) const
{
  auto it = udp_recv_buffers_.find(fd);
  if (it == udp_recv_buffers_.end() || it->second.empty())
  {
    return 0; // No messages available
  }

  if (it->second.size() >= 1)
  {
    return 2; // 2 choices: receive first or second message
  }

  return 2; // Only 1 choice: receive the only message or lag
}

/* ==============================================================================
 * SockState Public API - Lifecycle
 * ==============================================================================
 */

int SockState::do_close(int fd)
{
  auto it = sockets_.find(fd);
  if (it == sockets_.end())
  {
    return -EBADF;
  }

  Socket &sock = it->second;

  // For connected TCP sockets, notify peer
  if (sock.connected && sock.type == SOCK_STREAM)
  {
    // Get effective local address: if 0.0.0.0, use config addr
    uint32_t effective_local_addr = sock.local_addr.sin_addr.s_addr;
    if (effective_local_addr == INADDR_ANY)
    {
      struct in_addr config_addr;
      if (inet_pton(AF_INET, ptmc_state.addrs[ptmc_state.cursor].c_str(),
                    &config_addr) == 1)
      {
        effective_local_addr = config_addr.s_addr;
      }
    }

    uint64_t endpoint = (uint64_t(effective_local_addr) << 32) | sock.local_addr.sin_port;
    for (int node = 0; node < NP; ++node) {
      for (auto &[listener_fd, listener] : ptmc_state.sock_states[node].sockets_) {
        if (!listener.listening) continue;
        auto pending = listener.established_connections.find(endpoint);
        if (pending != listener.established_connections.end() &&
            pending->second.peer_addr.sin_addr.s_addr == sock.peer_addr.sin_addr.s_addr &&
            pending->second.peer_addr.sin_port == sock.peer_addr.sin_port)
          pending->second.recv_buffer.emplace_back();
      }
    }
    // Find peer socket and add EPOLLIN event to wake up epoll_wait
    for (int pid = 0; pid < NP; pid++)
    {
      SockState &remote_state = ptmc_state.sock_states[pid];
      for (auto &kv : remote_state.sockets_)
      {
        Socket &remote_sock = kv.second;
        if (remote_sock.connected &&
            remote_sock.peer_addr.sin_addr.s_addr == effective_local_addr &&
            remote_sock.peer_addr.sin_port == sock.local_addr.sin_port &&
            remote_sock.local_addr.sin_addr.s_addr == sock.peer_addr.sin_addr.s_addr &&
            remote_sock.local_addr.sin_port == sock.peer_addr.sin_port)
        {
          // Found peer, add EOF marker to recv_buffer and notify
          auto conn_it = remote_state.tcp_connections_.find(kv.first);
          if (conn_it != remote_state.tcp_connections_.end())
          {
            // Push empty string as EOF marker
            conn_it->second.recv_buffer.push_back("");
          }
          break;
        }
      }
    }
  }

  udp_recv_buffers_.erase(fd);
  tcp_connections_.erase(fd);
  sockets_.erase(it);
  forget_fd(fd);
  release_fd(fd);

  LOG_TRACE("close(fd=%d) = 0", fd);
  return 0;
}

/* ==============================================================================
 * SockState Public API - Cross-Process Delivery
 * ==============================================================================
 */

void SockState::deliver_udp_datagram(int recv_fd, const UdpDatagram &dg)
{

  const size_t MAX_UDP_BUFFER = 3;
  if (udp_recv_buffers_[recv_fd].size() >= MAX_UDP_BUFFER)
  {
    ptmc_state.error_bound--;
    LOG_TRACE("UDP buffer overflow for fd=%d", recv_fd);
    return;
  }
  udp_recv_buffers_[recv_fd].push_back(dg);
}

/* ==============================================================================
 * SockState Public API - Queries
 * ==============================================================================
 */

bool SockState::is_valid_socket(int fd) const { return sockets_.count(fd) > 0; }

const Socket *SockState::get_socket(int fd) const
{
  auto it = sockets_.find(fd);
  if (it != sockets_.end())
    return &it->second;
  return nullptr;
}

size_t SockState::get_udp_buffer_size(int fd) const
{
  auto it = udp_recv_buffers_.find(fd);
  if (it != udp_recv_buffers_.end())
    return it->second.size();
  return 0;
}

/* ==============================================================================
 * Serialization Implementations
 * ==============================================================================
 */

template <class Archive>
void Socket::serialize(Archive &ar)
{
  sa_family_t local_family = local_addr.sin_family;
  in_port_t local_port = local_addr.sin_port;
  uint32_t local_addr_val = local_addr.sin_addr.s_addr;

  sa_family_t peer_family = peer_addr.sin_family;
  in_port_t peer_port = peer_addr.sin_port;
  uint32_t peer_addr_val = peer_addr.sin_addr.s_addr;

  ar(fd, domain, type, protocol, bound, local_family, local_port,
     local_addr_val, connected, listening, peer_family, peer_port,
     peer_addr_val, backlog, pending_connections, established_connections,
     reuse_address, no_delay, keep_alive, keep_idle, keep_interval, keep_count);

  local_addr.sin_family = local_family;
  local_addr.sin_port = local_port;
  local_addr.sin_addr.s_addr = local_addr_val;

  peer_addr.sin_family = peer_family;
  peer_addr.sin_port = peer_port;
  peer_addr.sin_addr.s_addr = peer_addr_val;
}

template <class Archive>
void UdpDatagram::serialize(Archive &ar)
{
  sa_family_t family = from.sin_family;
  in_port_t port = from.sin_port;
  uint32_t addr_val = from.sin_addr.s_addr;

  ar(content, family, port, addr_val);

  from.sin_family = family;
  from.sin_port = port;
  from.sin_addr.s_addr = addr_val;
}

template <class Archive>
void TcpConnection::serialize(Archive &ar)
{
  sa_family_t l_family = local_addr.sin_family;
  in_port_t l_port = local_addr.sin_port;
  uint32_t l_addr = local_addr.sin_addr.s_addr;

  sa_family_t p_family = peer_addr.sin_family;
  in_port_t p_port = peer_addr.sin_port;
  uint32_t p_addr = peer_addr.sin_addr.s_addr;

  ar(local_fd, l_family, l_port, l_addr, p_family, p_port, p_addr, recv_buffer);

  local_addr.sin_family = l_family;
  local_addr.sin_port = l_port;
  local_addr.sin_addr.s_addr = l_addr;

  peer_addr.sin_family = p_family;
  peer_addr.sin_port = p_port;
  peer_addr.sin_addr.s_addr = p_addr;
}

template <class Archive>
void EpollInstance::serialize(Archive &ar)
{
  ar(epoll_fd, watched_fds);
}

template <class Archive>
void SockState::serialize(Archive &ar)
{
  ar(sockets_, udp_recv_buffers_, tcp_connections_, epoll_instances_);
}

/* Explicit instantiations */
template void
Socket::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void
Socket::serialize<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive &);
template void UdpDatagram::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void UdpDatagram::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void TcpConnection::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void TcpConnection::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void EpollInstance::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void EpollInstance::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
SockState::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void SockState::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

/* ==============================================================================
 * Debug
 * ==============================================================================
 */

void SockState::dump_state() const
{
  detsim::ui::ui_printf("[SockState] %zu sockets, %zu UDP buffers\n",
                        sockets_.size(), udp_recv_buffers_.size());

  for (const auto &kv : sockets_)
  {
    const Socket &sock = kv.second;
    detsim::ui::ui_printf(
        "  fd=%d: domain=%d, type=%s, bound=%s, %s\n", sock.fd, sock.domain,
        sock.type == SOCK_STREAM ? "TCP" : "UDP", sock.bound ? "yes" : "no",
        sock.bound ? format_sockaddr(sock.local_addr).c_str() : "-");

    auto buf_it = udp_recv_buffers_.find(sock.fd);
    if (buf_it != udp_recv_buffers_.end() && !buf_it->second.empty())
    {
      detsim::ui::ui_printf("    UDP buffer: %zu datagrams\n",
                            buf_it->second.size());
    }
  }
}

/* ==============================================================================
 * Legacy C-style API - delegates to SockState
 * ==============================================================================
 */

int emu_socket(int domain, int type, int protocol)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_socket(domain, type, protocol);
}

int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_bind(sockfd, addr, addrlen);
}

int emu_getsockname(int fd, sockaddr *address, socklen_t *length)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_getsockname(fd, address, length);
}

int emu_getpeername(int fd, sockaddr *address, socklen_t *length)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_getpeername(fd, address, length);
}

int emu_listen(int sockfd, int backlog)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_listen(sockfd, backlog);
}

int emu_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].do_connect(sockfd, addr, addrlen);
}

ssize_t emu_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen)
{
  int cur = ptmc_state.cursor;
  // Use choice from ptmc_state if available, otherwise default to 0
  int choice = (ptmc_state.n_choose > 0) ? ptmc_state.choose : 0;
  return ptmc_state.sock_states[cur].do_recvfrom_with_choice(
      sockfd, buf, len, flags, src_addr, addrlen, choice);
}

// Get number of recvfrom choices for current process and fd
int emu_recvfrom_get_choices(int sockfd)
{
  int cur = ptmc_state.cursor;
  return ptmc_state.sock_states[cur].get_recvfrom_choices(sockfd);
}

/* Helper: Find which node owns an address */
static int addr_to_node(const struct sockaddr_in *addr)
{
  if (addr->sin_addr.s_addr == 0xff000001 ||
      addr->sin_addr.s_addr == 0x00000000)
  {
    return ptmc_state.cursor;
  }

  for (int i = 0; i < NP; i++)
  {
    struct in_addr in_addr_i;
    if (inet_pton(AF_INET, ptmc_state.addrs[i].c_str(), &in_addr_i) != 1)
    {
      LOG_ERROR("inet_pton failed");
      continue;
    }
    if (addr->sin_addr.s_addr == in_addr_i.s_addr)
      return i;
  }

  char addr_str[32];
  inet_ntop(AF_INET, &addr->sin_addr, addr_str, sizeof(addr_str));
  LOG_ERROR("Addr %s not found", addr_str);
  return -1;
}

ssize_t emu_sendto(int sockfd, const void *buf, size_t len, int flags,
                   struct sockaddr *dest_addr, socklen_t addrlen)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_sendto(
      sockfd, buf, len, flags, dest_addr, addrlen);
}

/* ==============================================================================
 * Epoll Implementation
 * ==============================================================================
 */

void SockState::forget_fd(int fd)
{
  for (auto &[epfd, instance] : epoll_instances_) {
    instance.watched_fds.erase(fd);
  }
}

int SockState::do_epoll_close(int fd)
{
  if (!epoll_instances_.erase(fd)) return -EBADF;
  forget_fd(fd);
  release_fd(fd);
  return 0;
}

int SockState::do_epoll_create(int size)
{
  if (size <= 0) return -EINVAL;
  return do_epoll_create1(0);
}

int SockState::do_epoll_create1(int flags)
{
  if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
  int epfd = allocate_fd(FdType::EPOLL, O_RDWR, flags ? FD_CLOEXEC : 0);
  if (epfd < 0) return -EMFILE;
  EpollInstance instance;
  instance.epoll_fd = epfd;
  epoll_instances_[epfd] = instance;
  return epfd;
}

int SockState::do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
  auto it = epoll_instances_.find(epfd);
  if (it == epoll_instances_.end()) return -EBADF;
  if (!fd_manager_ || !fd_manager_->is_allocated(fd)) return -EBADF;
  if (fd == epfd) return -EINVAL;
  FdType type = fd_manager_->get_fd_type(fd);
  if (type == FdType::EPOLL)
    throw std::runtime_error("Nested epoll is unsupported");
  if (type != FdType::SOCKET && type != FdType::PIPE) return -EPERM;
  if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
    return -EINVAL;
  auto &instance = it->second;
  auto watch = instance.watched_fds.find(fd);
  if (op == EPOLL_CTL_ADD && watch != instance.watched_fds.end()) return -EEXIST;
  if (op != EPOLL_CTL_ADD && watch == instance.watched_fds.end()) return -ENOENT;
  if (op == EPOLL_CTL_DEL) {
    instance.watched_fds.erase(watch);
    return 0;
  }
  epoll_event input{};
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], event, &input, sizeof(input)))
    return -EFAULT;
  if (input.events & (EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP))
    throw std::runtime_error("Unsupported epoll watch mode");
  if (input.events & ~(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
    throw std::runtime_error("Unsupported epoll readiness mask");
  instance.watched_fds[fd] = EpollWatch{input.events, input.data.u64};
  return 0;
}

uint32_t SockState::ready_events(int fd, int node) const
{ return ready_events(fd, ptmc_state.fs_states[node]); }

uint32_t SockState::ready_events(int fd, const FileSystemState& fs) const
{
  uint32_t ready = 0;
  const Socket *socket = get_socket(fd);
  if (socket) {
    if (socket->type == SOCK_DGRAM) {
      if (get_udp_buffer_size(fd)) ready |= EPOLLIN;
      ready |= EPOLLOUT;
    } else if (socket->listening) {
      if (!socket->pending_connections.empty()) ready |= EPOLLIN;
    } else if (socket->connected) {
      auto connection = tcp_connections_.find(fd);
      if (connection != tcp_connections_.end()) {
        const auto &queue = connection->second.recv_buffer;
        if (!queue.empty()) ready |= EPOLLIN;
        if (!queue.empty() && queue.back().empty()) ready |= EPOLLRDHUP;
        ready |= EPOLLOUT;
      }
    }
  } else {
    auto pipe_fd = fs.pipe_fds_.find(fd);
    if (pipe_fd != fs.pipe_fds_.end()) {
      auto pipe = fs.pipes_.find(std::abs(pipe_fd->second));
      if (pipe != fs.pipes_.end()) {
        if (pipe_fd->second > 0) {
          if (!pipe->second.buffer.empty() || pipe->second.write_closed) ready |= EPOLLIN;
          if (pipe->second.write_closed) ready |= EPOLLHUP;
        } else {
          if (pipe->second.read_closed) ready |= EPOLLERR;
          else if (pipe->second.buffer.size() < FileSystemState::pipe_capacity) ready |= EPOLLOUT;
        }
      }
    }
  }
  return ready;
}

bool SockState::epoll_ready(int epfd, int node) const
{ return epoll_ready(epfd, ptmc_state.fs_states[node]); }

bool SockState::epoll_ready(int epfd, const FileSystemState& fs) const
{
  const auto it = epoll_instances_.find(epfd);
  if (it == epoll_instances_.end()) return true; // Retry produces EBADF.
  for (const auto &[fd, watch] : it->second.watched_fds)
    if (ready_events(fd, fs) & (watch.events | EPOLLERR | EPOLLHUP)) return true;
  return false;
}

int SockState::do_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
  (void)timeout; // Blocking/time progression remains part of the scheduling profile.
  auto it = epoll_instances_.find(epfd);
  if (it == epoll_instances_.end()) return -EBADF;
  if (maxevents <= 0 || maxevents > INT_MAX / int(sizeof(epoll_event))) return -EINVAL;
  std::vector<epoll_event> output;
  for (const auto &[fd, watch] : it->second.watched_fds) {
    if (int(output.size()) >= maxevents) break;
    uint32_t ready = ready_events(fd, ptmc_state.cursor);
    ready &= watch.events | EPOLLERR | EPOLLHUP;
    if (ready) {
      epoll_event event{};
      event.events = ready;
      event.data.u64 = watch.data;
      output.push_back(event);
    }
  }
  if (!guest_write_exact(ptmc_state.pids[ptmc_state.cursor], events,
                        output.data(), output.size() * sizeof(epoll_event))) return -EFAULT;
  return output.size();
}

int emu_epoll_create(int size)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_create(size);
}

int emu_epoll_create1(int flags)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_create1(flags);
}

int emu_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_ctl(epfd, op, fd,
                                                                event);
}

int emu_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                   int timeout)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_wait(
      epfd, events, maxevents, timeout);
}

int emu_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                    int timeout, const sigset_t *sigmask, size_t sigsetsize)
{
  if (sigmask) throw std::runtime_error("epoll_pwait signal masks are unsupported");
  (void)sigsetsize;
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_wait(
      epfd, events, maxevents, timeout);
}

int emu_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                     const struct timespec *timeout, const sigset_t *sigmask,
                     size_t sigsetsize)
{
  if (sigmask) throw std::runtime_error("epoll_pwait2 signal masks are unsupported");
  int timeout_ms = -1;
  if (timeout) {
    timespec local{};
    if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], timeout, &local, sizeof(local)))
      return -EFAULT;
    if (local.tv_sec < 0 || local.tv_nsec < 0 || local.tv_nsec >= 1000000000) return -EINVAL;
    timeout_ms = local.tv_sec > INT_MAX / 1000 ? INT_MAX :
        std::min<long long>(INT_MAX, local.tv_sec * 1000LL + (local.tv_nsec + 999999) / 1000000);
  }
  (void)sigsetsize;
  return ptmc_state.sock_states[ptmc_state.cursor].do_epoll_wait(
      epfd, events, maxevents, timeout_ms);
}

/* ==============================================================================
 * TCP send/recv
 * ==============================================================================
 */

ssize_t emu_send(int sockfd, const void *buf, size_t len, int flags)
{
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  const Socket *sock = sock_state.get_socket(sockfd);

  if (!sock)
  {
    LOG_ERROR("emu_send: invalid socket fd %d", sockfd);
    return ptmc_state.fd_managers[ptmc_state.cursor]->is_allocated(sockfd) ? -ENOTSOCK : -EBADF;
  }

  if (sock->type == SOCK_STREAM) return sock_state.do_send(sockfd, buf, len, flags);
  return emu_sendto(sockfd, buf, len, flags, nullptr, 0);
}

ssize_t emu_recv(int sockfd, void *buf, size_t len, int flags)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_recv(sockfd, buf, len,
                                                           flags);
}

int emu_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
  return ptmc_state.sock_states[ptmc_state.cursor].do_accept(sockfd, addr,
                                                             addrlen);
}

/* ==============================================================================
 * Socket Options
 * ==============================================================================
 */

int emu_setsockopt(int sockfd, int level, int optname, const void *optval,
                   socklen_t optlen)
{
  auto &state = ptmc_state.sock_states[ptmc_state.cursor];
  auto it = state.sockets().find(sockfd);
  if (it == state.sockets().end())
    return ptmc_state.fd_managers[ptmc_state.cursor]->is_allocated(sockfd) ? -ENOTSOCK : -EBADF;
  Socket &socket = it->second;
  bool *setting = nullptr;
  int *numeric_setting = nullptr;
  int maximum = 0;
  if (level == SOL_SOCKET && optname == SO_REUSEADDR) setting = &socket.reuse_address;
  else if (level == SOL_SOCKET && optname == SO_KEEPALIVE) setting = &socket.keep_alive;
  else if (level == IPPROTO_TCP && optname == TCP_NODELAY && socket.type == SOCK_STREAM)
    setting = &socket.no_delay;
  else if (level == IPPROTO_TCP && socket.type == SOCK_STREAM) {
    switch (optname) {
      case TCP_KEEPIDLE: numeric_setting = &socket.keep_idle; maximum = 32767; break;
      case TCP_KEEPINTVL: numeric_setting = &socket.keep_interval; maximum = 32767; break;
      case TCP_KEEPCNT: numeric_setting = &socket.keep_count; maximum = 127; break;
      default: throw std::runtime_error("Unsupported setsockopt option");
    }
  } else throw std::runtime_error("Unsupported setsockopt option");
  if (optlen < sizeof(int)) return -EINVAL;
  int value = 0;
  if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor], optval, &value, sizeof(value)))
    return -EFAULT;
  // These tunings are saved and queryable. The network profile has no TIME_WAIT,
  // Nagle delays, or keepalive timers; they do not enable endpoint aliasing.
  if (numeric_setting) {
    if (value < 1 || value > maximum) return -EINVAL;
    *numeric_setting = value;
  } else *setting = value != 0;
  return 0;
}

int emu_getsockopt(int sockfd, int level, int optname, void *optval,
                   socklen_t *optlen)
{
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  const Socket *sock = sock_state.get_socket(sockfd);

  if (!sock)
  {
    return ptmc_state.fd_managers[ptmc_state.cursor]->is_allocated(sockfd) ? -ENOTSOCK : -EBADF;
  }

  int value = 0;
  if (level == IPPROTO_TCP && sock->type == SOCK_STREAM) {
    switch (optname) {
      case TCP_NODELAY: value = sock->no_delay; break;
      case TCP_KEEPIDLE: value = sock->keep_idle; break;
      case TCP_KEEPINTVL: value = sock->keep_interval; break;
      case TCP_KEEPCNT: value = sock->keep_count; break;
      default: throw std::runtime_error("Unsupported getsockopt option");
    }
  }
  else if (level == SOL_SOCKET) switch (optname) {
    case SO_REUSEADDR: value = sock->reuse_address; break;
    case SO_KEEPALIVE: value = sock->keep_alive; break;
    case SO_ACCEPTCONN: value = sock->listening; break;
    case SO_TYPE: value = sock->type; break;
    case SO_DOMAIN: value = sock->domain; break;
    case SO_PROTOCOL: value = sock->protocol; break;
    case SO_ERROR: value = 0; break; // Connections currently complete synchronously.
    default: throw std::runtime_error("Unsupported getsockopt option");
  } else throw std::runtime_error("Unsupported getsockopt level/option");
  int pid = ptmc_state.pids[ptmc_state.cursor];
  socklen_t capacity = 0;
  if (!guest_read_exact(pid, optlen, &capacity, sizeof(capacity))) return -EFAULT;
  if (capacity > INT_MAX) return -EINVAL;
  if (!guest_write_exact(pid, optval, &value, std::min<size_t>(capacity, sizeof(value))))
    return -EFAULT;
  socklen_t actual = std::min<size_t>(capacity, sizeof(value));
  if (!guest_write_exact(pid, optlen, &actual, sizeof(actual))) return -EFAULT;
  return 0;
}

int emu_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
  if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
  const int fd = emu_accept(sockfd, addr, addrlen);
  if (fd < 0) return fd;
  auto& manager = *ptmc_state.fd_managers[ptmc_state.cursor];
  if (flags & SOCK_NONBLOCK) manager.do_fcntl(fd, F_SETFL, O_NONBLOCK);
  if (flags & SOCK_CLOEXEC) manager.do_fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}
