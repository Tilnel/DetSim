/* Shared descriptor namespace for virtual files, sockets and epoll. */
#ifndef __FD_MANAGER_H
#define __FD_MANAGER_H

#include <map>
#include <memory>
#include <unordered_set>

enum class FdType
{
  FILE,
  SOCKET,
  EPOLL,
  PIPE,
  STDIO,
  UNKNOWN
};

class FdManager
{
  public:
  FdManager();

  int allocate_fd(FdType type, int status_flags = 0,
                  int descriptor_flags = 0);
  void release_fd(int fd);
  FdType get_fd_type(int fd) const;
  bool is_allocated(int fd) const;
  int get_status_flags(int fd) const;
  int get_descriptor_flags(int fd) const;
  int do_fcntl(int fd, int command, int argument);
  int get_next_fd() const;
  std::unordered_set<int> get_allocated_fds() const;

  bool operator==(const FdManager &other) const = default;

  template <class Archive> void serialize(Archive &ar);

  private:
  struct Entry
  {
    FdType kind;
    int status_flags;
    int descriptor_flags;
    bool operator==(const Entry &other) const = default;
  };
  std::map<int, Entry> entries_;
};

using FdManagerPtr = std::shared_ptr<FdManager>;
#endif
