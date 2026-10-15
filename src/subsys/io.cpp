#include "io.h"
#include "pending_io.h"
#include "guest.h"
#include "monitor.h"
#include "utils.h"
#include "fs/fsstate.h"
#include "net/sockstate.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <climits>

namespace {
FdManager& descriptors() {
  auto manager=ptmc_state.fd_managers[ptmc_state.cursor];
  if (!manager) throw std::runtime_error("Virtual descriptor manager missing");
  return *manager;
}
FileSystemState& files() {return ptmc_state.fs_states[ptmc_state.cursor];}
SockState& network() {return ptmc_state.sock_states[ptmc_state.cursor];}
void require_file(int fd) {
  if (!files().open_files.contains(fd))
    throw std::runtime_error("Descriptor registry has no corresponding file payload");
}
}

long emu_read(int fd, void* destination, size_t count) {
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  switch (manager.get_fd_type(fd)) {
    case FdType::SOCKET: {
      const auto* socket=network().get_socket(fd);
      if (!socket) throw std::runtime_error("Socket descriptor has no payload");
      return socket->type==SOCK_DGRAM
                 ? network().do_recvfrom(fd,destination,count,0,nullptr,nullptr)
                 : network().do_recv(fd,destination,count,0);
    }
    case FdType::PIPE: return files().do_pipe_read(fd,destination,count);
    case FdType::EPOLL: return -EINVAL;
    case FdType::FILE:
    case FdType::STDIO: {
      require_file(fd);
      auto& description=files().open_files.at(fd);
      const auto& node=files().filesystem.at(description.path);
      const auto offset=description.offset;
      if (offset<0) throw std::runtime_error("Negative virtual file offset");
      const size_t available=size_t(offset)>=node.content.size()?0:node.content.size()-size_t(offset);
      std::vector<char> buffer(std::min(count,available));
      const auto result=files().do_read(fd,buffer.data(),buffer.size());
      if (result>0 && !guest_write_exact(ptmc_state.pids[ptmc_state.cursor],destination,buffer.data(),size_t(result))) {
        description.offset=offset;
        return -EFAULT;
      }
      return result;
    }
    default: throw std::runtime_error("Unsupported virtual descriptor kind");
  }
}

long emu_write(int fd, const void* source, size_t count) {
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  switch (manager.get_fd_type(fd)) {
    case FdType::SOCKET: {
      const auto* socket=network().get_socket(fd);
      if (!socket) throw std::runtime_error("Socket descriptor has no payload");
      return socket->type==SOCK_DGRAM
                 ? network().do_sendto(fd,source,count,0,nullptr,0)
                 : network().do_send(fd,source,count,0);
    }
    case FdType::PIPE: return files().do_pipe_write(fd,source,count);
    case FdType::EPOLL: return -EINVAL;
    case FdType::FILE:
    case FdType::STDIO: {
      require_file(fd);
      if ((manager.get_status_flags(fd)&O_ACCMODE)==O_RDONLY) return -EBADF;
      std::vector<char> buffer(count);
      if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor],source,buffer.data(),count)) return -EFAULT;
      return files().do_write(fd,buffer.data(),count);
    }
    default: throw std::runtime_error("Unsupported virtual descriptor kind");
  }
}

long emu_close(int fd) {
  require_no_pending_fd(ptmc_state.cursor, fd);
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  int result;
  switch (manager.get_fd_type(fd)) {
    case FdType::SOCKET: result=network().do_close(fd);break;
    case FdType::PIPE: result=files().do_pipe_close(fd);break;
    case FdType::EPOLL: result=network().do_epoll_close(fd);break;
    case FdType::FILE:
    case FdType::STDIO: require_file(fd);result=files().do_close(fd);break;
    default: throw std::runtime_error("Unsupported virtual descriptor kind");
  }
  if (!result) network().forget_fd(fd);
  return result;
}

long emu_fstat(int fd, struct stat* destination) {
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  struct stat value{};
  switch (manager.get_fd_type(fd)) {
    case FdType::FILE:
    case FdType::STDIO: {
      require_file(fd);
      const int result=files().do_fstat(fd,&value);
      if (result) return result;
      if (manager.get_fd_type(fd)==FdType::STDIO) value.st_size=0;
      break;
    }
    case FdType::SOCKET: value.st_mode=S_IFSOCK|0600;break;
    case FdType::PIPE: value.st_mode=S_IFIFO|0600;break;
    case FdType::EPOLL: value.st_mode=0600;break;
    default: throw std::runtime_error("Unsupported virtual descriptor kind");
  }
  return guest_write_exact(ptmc_state.pids[ptmc_state.cursor],destination,&value,sizeof(value))?0:-EFAULT;
}
int emu_fcntl(int fd,int command,long argument) {
  if (command == F_SETFL) require_no_pending_fd(ptmc_state.cursor, fd);
  return descriptors().do_fcntl(fd,command,static_cast<int>(argument));
}
int emu_fsync(int fd) {
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  if (manager.get_fd_type(fd)!=FdType::FILE) return -EINVAL;
  require_file(fd);
  return 0;
}
int emu_fdatasync(int fd) {return emu_fsync(fd);}
int emu_ioctl(int fd, unsigned long request, void* argument) {
  request=static_cast<uint32_t>(request);
  auto& manager=descriptors();
  if (!manager.is_allocated(fd)) return -EBADF;
  if (request==FIOCLEX || request==FIONCLEX)
    return manager.do_fcntl(fd,F_SETFD,request==FIOCLEX?FD_CLOEXEC:0);
  // The profile's standard streams are captured non-terminal streams.
  if (request==TCGETS || request==TIOCGWINSZ) return -ENOTTY;
  if (request==FIONBIO) {
    require_no_pending_fd(ptmc_state.cursor, fd);
    int enabled;
    if (!guest_read_exact(ptmc_state.pids[ptmc_state.cursor],argument,&enabled,sizeof(enabled))) return -EFAULT;
    int flags=manager.get_status_flags(fd);
    return manager.do_fcntl(fd,F_SETFL,enabled?flags|O_NONBLOCK:flags&~O_NONBLOCK);
  }
  if (request==FIONREAD) {
    size_t bytes=0;
    switch (manager.get_fd_type(fd)) {
      case FdType::PIPE: {
        const auto& fs=files(); const int id=fs.pipe_fds_.at(fd);
        bytes=fs.pipes_.at(id>0?id:-id).buffer.size();break;
      }
      case FdType::SOCKET: {
        const auto* socket=network().get_socket(fd);
        if (!socket) throw std::runtime_error("Socket descriptor has no payload");
        if (socket->listening) return -EINVAL;
        if (socket->type==SOCK_DGRAM) {
          const auto it=network().udp_recv_buffers().find(fd);
          if (it!=network().udp_recv_buffers().end() && !it->second.empty()) bytes=it->second.front().content.size();
        } else {
          const auto it=network().tcp_connections().find(fd);
          if (it!=network().tcp_connections().end())
            for (const auto& message:it->second.recv_buffer) bytes+=message.size();
        }
        break;
      }
      case FdType::FILE: {
        require_file(fd);const auto& description=files().open_files.at(fd);
        const auto size=files().filesystem.at(description.path).content.size();
        bytes=size_t(description.offset)>=size?0:size-size_t(description.offset);break;
      }
      case FdType::STDIO: return -ENOTTY;
      default: throw std::runtime_error("Unsupported virtual ioctl descriptor kind");
    }
    const int value=static_cast<int>(std::min(bytes,size_t(INT_MAX)));
    return guest_write_exact(ptmc_state.pids[ptmc_state.cursor],argument,&value,sizeof(value))?0:-EFAULT;
  }
  throw std::runtime_error("Unsupported virtual ioctl request");
}

long emu_poll(struct pollfd* fds, uint32_t count, int timeout) {
  if (timeout != 0) throw std::runtime_error("Blocking poll is not modeled");
  // Explicit staging bound, not a fabricated guest resource-limit error.
  if (count > 65536) throw std::runtime_error("poll descriptor count exceeds model limit");
  std::vector<pollfd> entries(count);
  const pid_t pid=ptmc_state.pids[ptmc_state.cursor];
  if (count && !guest_read_exact(pid,fds,entries.data(),entries.size()*sizeof(pollfd))) return -EFAULT;
  int result=0;
  auto& manager=descriptors();
  for (auto& entry:entries) {
    entry.revents=0;
    if (entry.fd<0) continue;
    if (!manager.is_allocated(entry.fd)) entry.revents=POLLNVAL;
    else {
      uint32_t ready=0;
      const auto type=manager.get_fd_type(entry.fd);
      if (type==FdType::FILE || type==FdType::STDIO) {
        require_file(entry.fd);
        ready=POLLIN|POLLOUT|POLLRDNORM|POLLWRNORM;
      } else if (type==FdType::EPOLL) {
        if (network().epoll_ready(entry.fd,ptmc_state.cursor)) ready=POLLIN|POLLRDNORM;
      } else {
        ready=network().ready_events(entry.fd,ptmc_state.cursor);
        if (type==FdType::SOCKET) {
          const auto* socket=network().get_socket(entry.fd);
          if (!socket) throw std::runtime_error("Socket descriptor has no payload");
          if (socket->type==SOCK_STREAM && !socket->connected && !socket->listening)
            ready|=POLLOUT|POLLHUP;
        } else if (type==FdType::PIPE) {
          const int key=files().pipe_fds_.at(entry.fd);
          const auto& pipe=files().pipes_.at(std::abs(key));
          if (key>0 && pipe.buffer.empty()) ready&=~POLLIN;
          if (key<0 && pipe.buffer.size()<FileSystemState::pipe_capacity) ready|=POLLOUT;
        } else throw std::runtime_error("Unsupported poll descriptor kind");
        if (ready&POLLIN) ready|=POLLRDNORM;
        if (ready&POLLOUT) ready|=POLLWRNORM;
      }
      entry.revents=static_cast<short>(ready & (static_cast<unsigned short>(entry.events)|POLLERR|POLLHUP));
    }
    if (entry.revents) ++result;
  }
  // Linux put_user(u16) cannot leave a prefix on a cross-page fault, whereas
  // process_vm_writev can. Ordinary within-page stores need no maps lookup.
  // All carriers are stopped; reuse one mapping read for every crossing scalar.
  std::vector<maps_item> maps;
  // Linux writes only revents, leaving fd/events untouched even for aliases.
  for (size_t i=0;i<entries.size();++i) {
    const auto address=reinterpret_cast<uintptr_t>(fds)+i*sizeof(pollfd)+offsetof(pollfd,revents);
    if (address%4096==4095) {
      if (maps.empty()) {
        auto file=fileutils::open_cfile("/proc/"+std::to_string(pid)+"/maps","r");
        if (!file) throw std::runtime_error("Cannot inspect poll scalar mappings");
        get_maps_item(maps,file.get());
        if (ferror(file.get()) || maps.empty()) throw std::runtime_error("Cannot read poll scalar mappings");
      }
      auto cursor=address;
      for (const auto& map:maps) {
        if (map.end<=cursor) continue;
        if (map.start>cursor || map.flags[1]!='w') break;
        cursor=std::min<uint64_t>(map.end,address+sizeof(short));
        if (cursor==address+sizeof(short)) break;
      }
      if (cursor!=address+sizeof(short)) return -EFAULT;
    }
    if (!guest_write_exact(pid,reinterpret_cast<void*>(address),&entries[i].revents,sizeof(short))) return -EFAULT;
  }
  return result;
}
