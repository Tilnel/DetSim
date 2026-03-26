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
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>

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

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  return inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) == 1;
}

/* ==============================================================================
 * SockState Private Helpers
 * ==============================================================================
 */

int SockState::allocate_fd(FdType type)
{
  if (!fd_manager_)
  {
    LOG_ERROR("FdManager not set!");
    return -1;
  }
  return fd_manager_->allocate_fd(type);
}

void SockState::release_fd(int fd)
{
  if (fd_manager_)
  {
    fd_manager_->release_fd(fd);
  }
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

  if (type != SOCK_STREAM && type != SOCK_DGRAM)
  {
    LOG_ERROR("socket: unsupported type %d", type);
    return -EPROTONOSUPPORT;
  }

  int fd = allocate_fd();
  if (fd < 0)
  {
    return -EMFILE;
  }

  Socket sock;
  sock.fd = fd;
  sock.domain = domain;
  sock.type = type;
  sock.protocol = protocol;
  sock.bound = false;
  sock.connected = false;
  sock.listening = false;

  sockets_[fd] = sock;

  LOG_TRACE("socket(%d, %d, %d) -> fd=%d", domain, type, protocol, fd);
  return fd;
}

int SockState::do_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
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
  struct sockaddr_in sin;
  memcpy_guest2host(&sin, addr, sizeof(struct sockaddr_in));
  memcpy(&sock->local_addr, &sin, sizeof(struct sockaddr_in));
  sock->bound = true;

  LOG_TRACE("bind(fd=%d, %s) = 0", fd, format_sockaddr(sin).c_str());
  return 0;
}

int SockState::do_listen(int fd, int backlog)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
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
  sock->backlog = backlog;

  LOG_TRACE("listen(fd=%d, backlog=%d) = 0", fd, backlog);
  return 0;
}

int SockState::do_connect(int fd, const struct sockaddr *addr,
                          socklen_t addrlen)
{
  (void)addrlen;
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->type != SOCK_STREAM)
  {
    return -EOPNOTSUPP;
  }

  struct sockaddr_in peer_addr;
  memcpy_guest2host(&peer_addr, addr, sizeof(struct sockaddr_in));

  // Set local address: bind to 0.0.0.0 with ephemeral port
  if (!sock->bound)
  {
    sock->bound = true;
    sock->local_addr.sin_family = AF_INET;
    sock->local_addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    sock->local_addr.sin_port =
        htons(10000 + (rand() % 50000)); // Random ephemeral port
  }

  sock->connected = true;
  sock->peer_addr = peer_addr;

  // Cross-process: find listening socket by port (handle 0.0.0.0 binding)
  bool listener_found = false;
  for (int pid = 0; pid < NP; pid++)
  {
    // Match by port only, since server may bind to 0.0.0.0 (INADDR_ANY)
    int listen_fd =
        ptmc_state.sock_states[pid].find_socket_by_port(peer_addr.sin_port);
    if (listen_fd >= 0)
    {
      Socket *listener =
          ptmc_state.sock_states[pid].get_socket_mutable(listen_fd);
      if (listener && listener->listening)
      {
        // Add to pending connections of the listener's process
        listener->pending_connections.push_back(fd);

        // Create TcpConnection in the listener's process (kernel-managed)
        // This allows the client to send data before accept() is called
        TcpConnection conn;
        conn.local_fd = fd;
        conn.local_addr = sock->local_addr;
        conn.peer_addr = peer_addr;

        // Use client address (IP:port) as key
        uint64_t client_key =
            ((uint64_t)sock->local_addr.sin_addr.s_addr << 32) |
            sock->local_addr.sin_port;
        listener->established_connections[client_key] = conn;

        listener_found = true;
        LOG_TRACE("connect(fd=%d) -> listener fd=%d on process %d (port=%d), "
                  "created established connection",
                  fd, listen_fd, pid, ntohs(peer_addr.sin_port));
        break;
      }
    }
  }

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
    return -EBADF;
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

  // Get the first pending connection
  int client_fd = listener->pending_connections.front();
  listener->pending_connections.pop_front();

  // Find the client socket to get its address
  const Socket *client_sock = nullptr;
  int client_pid = -1;
  for (int pid = 0; pid < NP; pid++)
  {
    client_sock = ptmc_state.sock_states[pid].get_socket(client_fd);
    if (client_sock)
    {
      client_pid = pid;
      break;
    }
  }
  if (!client_sock)
  {
    return -ECONNRESET; // Client closed connection
  }

  // Compute client address key
  uint64_t client_key =
      ((uint64_t)client_sock->local_addr.sin_addr.s_addr << 32) |
      client_sock->local_addr.sin_port;

  // Look up existing established connection (created by connect())
  auto established_it = listener->established_connections.find(client_key);
  if (established_it == listener->established_connections.end())
  {
    return -ECONNRESET; // No established connection found
  }

  // Allocate new fd for accepted socket
  int new_fd = allocate_fd(FdType::SOCKET);
  if (new_fd < 0)
  {
    return -EMFILE;
  }

  // Create new socket for accepted connection
  Socket new_sock;
  new_sock.fd = new_fd;
  new_sock.domain = listener->domain;
  new_sock.type = SOCK_STREAM;
  new_sock.protocol = 0;
  new_sock.bound = true;
  new_sock.connected = true;

  // Server's local address: use real IP from config, not 0.0.0.0
  new_sock.local_addr.sin_family = AF_INET;
  inet_pton(AF_INET, ptmc_state.addrs[ptmc_state.cursor].c_str(),
            &new_sock.local_addr.sin_addr);
  new_sock.local_addr.sin_port = listener->local_addr.sin_port;

  // Client's peer address: use client's real IP
  new_sock.peer_addr.sin_family = AF_INET;
  inet_pton(AF_INET, ptmc_state.addrs[client_pid].c_str(),
            &new_sock.peer_addr.sin_addr);
  new_sock.peer_addr.sin_port = client_sock->local_addr.sin_port;

  sockets_[new_fd] = new_sock;

  // Adopt the existing TcpConnection with its buffered data
  TcpConnection conn = std::move(established_it->second);
  conn.local_fd = new_fd;
  conn.local_addr = new_sock.local_addr;
  // peer_addr is already set correctly from connect()
  tcp_connections_[new_fd] = std::move(conn);

  listener->established_connections.erase(established_it);

  // Return peer address to caller
  if (addr && addrlen)
  {
    socklen_t len = *addrlen;
    if (len > sizeof(struct sockaddr_in))
    {
      len = sizeof(struct sockaddr_in);
    }
    memcpy_host2guest(addr, &new_sock.peer_addr, len);
  }

  LOG_TRACE("accept(fd=%d) = %d (client fd=%d)", fd, new_fd, client_fd);
  return new_fd;
}

ssize_t SockState::do_send(int fd, const void *buf, size_t len, int flags)
{
  (void)flags;
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (!sock->connected)
  {
    return -ENOTCONN;
  }

  // Read data from guest
  std::string data(len, '\0');
  memcpy_guest2host(&data[0], buf, len);

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
          remote_sock.local_addr.sin_port == sock->peer_addr.sin_port)
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
      if (established_it != remote_sock.established_connections.end())
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
  (void)flags;
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
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
    conn.recv_buffer.pop_front();
    return 0; // EOF
  }

  size_t to_copy = len < data.size() ? len : data.size();
  memcpy_host2guest(buf, data.c_str(), to_copy);

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

ssize_t SockState::do_sendto(int fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr,
                             socklen_t addrlen)
{
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  if (!dest_addr && !sock->connected)
  {
    return -EDESTADDRREQ;
  }

  char *data = (char *)malloc(len);
  if (!data)
  {
    return -ENOMEM;
  }
  memcpy_guest2host(data, buf, len);

  struct sockaddr_in dest;
  if (dest_addr)
  {
    /* dest_addr is guest pointer, need to read from guest memory */
    memcpy_guest2host(&dest, dest_addr, sizeof(struct sockaddr_in));
  }
  else
  {
    dest = sock->peer_addr;
  }

  struct sockaddr_in from;
  memset(&from, 0, sizeof(from));
  from.sin_family = AF_INET;
  from.sin_port = sock->bound ? sock->local_addr.sin_port : 0;

  UdpDatagram dg;
  dg.content = std::string(data, len);
  dg.from = from;
  free(data);

  LOG_TRACE("sendto(fd=%d, len=%zu) to %s", fd, len,
            format_sockaddr(dest).c_str());

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
  Socket *sock = get_socket_mutable(fd);
  if (!sock)
  {
    return -EBADF;
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
  if (choice < 0 || choice >= static_cast<int>(it->second.size()))
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
    ptmc_state.error_bound--;
    ++dg_it;
  }

  UdpDatagram &dg = *dg_it;

  /* Copy data to guest buffer */
  size_t copy_len = std::min(len, dg.content.size());
  memcpy_host2guest(buf, dg.content.c_str(), copy_len);

  /* Handle output parameters - src_addr and addrlen are guest pointers */
  if (src_addr && addrlen)
  {
    /* Read input addrlen value from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    /* Calculate actual address length to write */
    socklen_t addr_len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));

    /* Write source address to guest */
    memcpy_host2guest(src_addr, &dg.from, addr_len);

    /* Write actual address length back to guest */
    memcpy_host2guest(addrlen, &addr_len, sizeof(socklen_t));
  }

  // Remove the received datagram from queue
  it->second.erase(dg_it);

  LOG_TRACE("recvfrom(fd=%d, choice=%d) -> len=%zu from %s", fd, choice,
            copy_len, format_sockaddr(dg.from).c_str());

  return copy_len;
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

    // Find peer socket and add EPOLLIN event to wake up epoll_wait
    for (int pid = 0; pid < NP; pid++)
    {
      SockState &remote_state = ptmc_state.sock_states[pid];
      for (auto &kv : remote_state.sockets_)
      {
        Socket &remote_sock = kv.second;
        if (remote_sock.connected &&
            remote_sock.peer_addr.sin_addr.s_addr == effective_local_addr &&
            remote_sock.peer_addr.sin_port == sock.local_addr.sin_port)
        {
          // Found peer, add EOF marker to recv_buffer and notify
          auto conn_it = remote_state.tcp_connections_.find(kv.first);
          if (conn_it != remote_state.tcp_connections_.end())
          {
            // Push empty string as EOF marker
            conn_it->second.recv_buffer.push_back("");
          }
          for (auto &epoll_kv : remote_state.epoll_instances_)
          {
            EpollInstance &inst = epoll_kv.second;
            if (inst.watched_fds.find(kv.first) != inst.watched_fds.end())
            {
              EpollEvent ev;
              ev.events = EPOLLIN;
              ev.fd = kv.first;
              inst.ready_events.push_back(ev);
              LOG_TRACE("close(fd=%d): notified peer fd=%d in process %d", fd,
                        kv.first, pid);
            }
          }
          break;
        }
      }
    }
  }

  udp_recv_buffers_.erase(fd);
  tcp_connections_.erase(fd);
  sockets_.erase(it);
  release_fd(fd);

  LOG_TRACE("close(fd=%d) = 0", fd);
  return 0;
}

int SockState::do_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  const Socket *sock = get_socket(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (!sock->bound)
  {
    return -EINVAL;
  }

  if (addr && addrlen)
  {
    /* Read input addrlen from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    socklen_t len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));
    memcpy_host2guest(addr, &sock->local_addr, len);
    memcpy_host2guest(addrlen, &len, sizeof(socklen_t));
  }

  return 0;
}

int SockState::do_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  const Socket *sock = get_socket(fd);
  if (!sock)
  {
    return -EBADF;
  }

  if (!sock->connected)
  {
    return -ENOTCONN;
  }

  if (addr && addrlen)
  {
    /* Read input addrlen from guest */
    socklen_t guest_addrlen = 0;
    memcpy_guest2host(&guest_addrlen, addrlen, sizeof(socklen_t));

    socklen_t len =
        std::min(guest_addrlen, (socklen_t)sizeof(struct sockaddr_in));
    memcpy_host2guest(addr, &sock->peer_addr, len);
    memcpy_host2guest(addrlen, &len, sizeof(socklen_t));
  }

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
     peer_addr_val, backlog, pending_connections);

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
void EpollEvent::serialize(Archive &ar)
{
  ar(events, fd);
}

template <class Archive>
void EpollInstance::serialize(Archive &ar)
{
  ar(epoll_fd, watched_fds, ready_events);
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
template void
EpollEvent::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void EpollEvent::serialize<cereal::BinaryOutputArchive>(
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

/* Helper: Convert string address to sockaddr_in */
static void addr_str_to_sockaddr(const std::string &addr_str,
                                 struct sockaddr_in *sin)
{
  if (addr_str.size() >= sizeof(struct sockaddr_in))
  {
    memcpy(sin, addr_str.c_str(), sizeof(struct sockaddr_in));
  }
  else
  {
    memset(sin, 0, sizeof(*sin));
  }
}

/* Helper: Convert sockaddr_in to string address */
static std::string sockaddr_to_addr_str(const struct sockaddr_in *sin)
{
  return std::string((const char *)sin, sizeof(*sin));
}

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
  LOG_TRACE("emu_sendto(%d, %p, %ld, %d, %p, %d)", sockfd, (void *)buf, len,
            flags, (void *)dest_addr, addrlen);

  int cur = ptmc_state.cursor;
  const Socket *sock = ptmc_state.sock_states[cur].get_socket(sockfd);
  if (!sock)
  {
    return -ENOTSOCK;
  }

  if (sock->type != SOCK_DGRAM)
  {
    return -EOPNOTSUPP;
  }

  /* Get destination info */
  struct sockaddr_in dest;
  if (dest_addr && addrlen > 0)
  {
    memcpy_guest2host(&dest, dest_addr, sizeof(struct sockaddr_in));
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
  assert(port > 0);

  /* Find receiving socket by port */
  int recvfd = -1;
  const auto &recv_sockets = ptmc_state.sock_states[node].sockets();
  for (const auto &kv : recv_sockets)
  {
    const Socket &rs = kv.second;
    if (rs.bound && rs.local_addr.sin_port == port)
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

  /* Read data from guest */
  char *data = (char *)malloc(len);
  if (!data)
  {
    return -ENOMEM;
  }
  memcpy_guest2host(data, buf, len);

  /* Create datagram with proper source address */
  UdpDatagram dg;
  dg.content = std::string(data, len);
  free(data);

  /* Set source address */
  dg.from.sin_family = AF_INET;
  dg.from.sin_port = sock->bound ? sock->local_addr.sin_port : 0;
  inet_pton(AF_INET, ptmc_state.addrs[cur].c_str(), &dg.from.sin_addr);

  /* Deliver to receiver */
  ptmc_state.sock_states[node].deliver_udp_datagram(recvfd, dg);

  LOG_TRACE("emu_sendto: delivered %zu bytes to node %d fd %d", len, node,
            recvfd);

  return len;
}

/* ==============================================================================
 * Epoll Implementation for Redis
 * ==============================================================================
 */

int SockState::do_epoll_create(int size)
{
  (void)size;

  int epfd = allocate_fd(FdType::EPOLL);
  if (epfd < 0)
  {
    LOG_ERROR("do_epoll_create: failed to allocate fd");
    return -EMFILE;
  }

  EpollInstance inst;
  inst.epoll_fd = epfd;
  epoll_instances_[epfd] = inst;

  LOG_TRACE("do_epoll_create: created epoll fd %d", epfd);
  return epfd;
}

int SockState::do_epoll_create1(int flags)
{
  (void)flags;
  return do_epoll_create(1);
}

int SockState::do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
  auto it = epoll_instances_.find(epfd);
  if (it == epoll_instances_.end())
  {
    LOG_ERROR("do_epoll_ctl: invalid epoll fd %d", epfd);
    return -EBADF;
  }

  EpollInstance &inst = it->second;

  // Read event from guest memory
  struct epoll_event host_event;
  if (event)
  {
    memcpy_guest2host(&host_event, event, sizeof(struct epoll_event));
  }
  else if (op != EPOLL_CTL_DEL)
  {
    return -EINVAL;
  }

  switch (op)
  {
    case EPOLL_CTL_ADD:
      inst.watched_fds[fd] = host_event.events;
      LOG_TRACE("do_epoll_ctl: ADD fd %d to epoll %d, events=%u", fd, epfd,
                host_event.events);
      break;
    case EPOLL_CTL_MOD:
      inst.watched_fds[fd] = host_event.events;
      LOG_TRACE("do_epoll_ctl: MOD fd %d on epoll %d, events=%u", fd, epfd,
                host_event.events);
      break;
    case EPOLL_CTL_DEL:
      inst.watched_fds.erase(fd);
      LOG_TRACE("do_epoll_ctl: DEL fd %d from epoll %d", fd, epfd);
      break;
    default:
      return -EINVAL;
  }

  return 0;
}

int SockState::do_epoll_wait(int epfd, struct epoll_event *events,
                             int maxevents, int timeout)
{
  (void)timeout;

  auto it = epoll_instances_.find(epfd);
  if (it == epoll_instances_.end())
  {
    LOG_ERROR("do_epoll_wait: invalid epoll fd %d", epfd);
    return -EBADF;
  }

  EpollInstance &inst = it->second;

  // Collect ready events in host buffer first
  std::vector<struct epoll_event> ready_host_events;
  while (!inst.ready_events.empty() &&
         (int)ready_host_events.size() < maxevents)
  {
    int fd = inst.ready_events.front().fd;
    // Skip events for sockets that no longer exist
    if (get_socket(fd) == nullptr)
    {
      LOG_TRACE("do_epoll_wait: skipping event for closed fd=%d", fd);
      inst.ready_events.pop_front();
      continue;
    }
    struct epoll_event ev;
    ev.events = inst.ready_events.front().events;
    ev.data.fd = fd;
    ready_host_events.push_back(ev);
    inst.ready_events.pop_front();
  }

  if (!ready_host_events.empty())
  {
    memcpy_host2guest(events, ready_host_events.data(),
                      ready_host_events.size() * sizeof(struct epoll_event));
    LOG_TRACE("do_epoll_wait: returned %zu ready events",
              ready_host_events.size());
    return ready_host_events.size();
  }

  // Collect events in host buffer first, then copy to guest
  std::vector<struct epoll_event> host_events;
  for (const auto &kv : inst.watched_fds)
  {
    if ((int)host_events.size() >= maxevents)
      break;

    int fd = kv.first;
    uint32_t mask = kv.second;
    uint32_t revents = 0;

    if ((mask & EPOLLIN) && get_udp_buffer_size(fd) > 0)
    {
      revents |= EPOLLIN;
    }

    const Socket *sock = get_socket(fd);
    if (sock && sock->listening && (mask & EPOLLIN))
    {
      LOG_TRACE("epoll_wait: fd=%d is listening, pending=%zu", fd,
                sock->pending_connections.size());
      if (!sock->pending_connections.empty())
      {
        revents |= EPOLLIN;
        LOG_TRACE("epoll_wait: fd=%d has pending connection, setting EPOLLIN",
                  fd);
      }
    }

    if (sock && sock->connected && (mask & EPOLLIN))
    {
      auto conn_it = tcp_connections_.find(fd);
      if (conn_it != tcp_connections_.end() &&
          !conn_it->second.recv_buffer.empty())
      {
        revents |= EPOLLIN;
      }
    }

    if (revents != 0)
    {
      struct epoll_event ev;
      ev.events = revents;
      ev.data.fd = fd;
      host_events.push_back(ev);
    }
  }

  // Copy events to guest memory
  if (!host_events.empty())
  {
    LOG_TRACE("epoll_wait: copying %zu events to guest at %p",
              host_events.size(), events);
    for (size_t i = 0; i < host_events.size(); i++)
    {
      LOG_TRACE("epoll_wait: event[%zu] fd=%d events=%u", i,
                host_events[i].data.fd, host_events[i].events);
    }
    memcpy_host2guest(events, host_events.data(),
                      host_events.size() * sizeof(struct epoll_event));
    LOG_TRACE("epoll_wait: copied successfully");
  }

  return host_events.size();
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
    return -EBADF;
  }

  if (sock->type == SOCK_STREAM && sock->connected)
  {
    return emu_sendto(sockfd, buf, len, flags,
                      (struct sockaddr *)&sock->peer_addr,
                      sizeof(sock->peer_addr));
  }

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
  (void)sockfd;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  LOG_TRACE("emu_setsockopt: fd=%d, level=%d, optname=%d", sockfd, level,
            optname);
  return 0;
}

int emu_getsockopt(int sockfd, int level, int optname, void *optval,
                   socklen_t *optlen)
{
  SockState &sock_state = ptmc_state.sock_states[ptmc_state.cursor];
  const Socket *sock = sock_state.get_socket(sockfd);

  if (!sock)
  {
    return -EBADF;
  }

  if (level == SOL_SOCKET && optname == SO_ACCEPTCONN)
  {
    int accepting = sock->listening ? 1 : 0;
    memcpy_host2guest(optval, &accepting, sizeof(int));
    if (optlen)
    {
      socklen_t len = sizeof(int);
      memcpy_host2guest(optlen, &len, sizeof(socklen_t));
    }
    LOG_TRACE("getsockopt(fd=%d, SO_ACCEPTCONN) = %d", sockfd, accepting);
    return 0;
  }

  LOG_TRACE("getsockopt(fd=%d, level=%d, optname=%d) - not implemented", sockfd,
            level, optname);
  return 0;
}

/* ==============================================================================
 * File Sync Operations
 * ==============================================================================
 */

int emu_fsync(int fd)
{
  (void)fd;
  LOG_TRACE("emu_fsync: fd=%d", fd);
  return 0;
}

int emu_fdatasync(int fd)
{
  (void)fd;
  LOG_TRACE("emu_fdatasync: fd=%d", fd);
  return 0;
}

/* ==============================================================================
 * Non-blocking I/O
 * ==============================================================================
 */

int emu_fcntl(int fd, int cmd, long arg)
{
  (void)fd;
  (void)arg;

  switch (cmd)
  {
    case F_GETFL:
      return 0;
    case F_SETFL:
      LOG_TRACE("emu_fcntl: F_SETFL fd=%d, arg=%ld", fd, arg);
      return 0;
    case F_GETFD:
      return 0;
    case F_SETFD:
      return 0;
    default:
      LOG_TRACE("emu_fcntl: fd=%d, cmd=%d, arg=%ld", fd, cmd, arg);
      return 0;
  }
}
