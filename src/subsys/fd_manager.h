/*
 * fd_manager.h - Unified file descriptor manager for FileSystemState and
 * SockState
 */

#ifndef __FD_MANAGER_H
#define __FD_MANAGER_H

#include <atomic>
#include <memory>
#include <unordered_set>

/* FdType distinguishes what kind of resource an fd represents */
enum class FdType
{
  FILE,   // Regular file from FileSystemState
  SOCKET, // Socket from SockState
  EPOLL,  // Epoll instance
  PIPE,   // Pipe
  UNKNOWN
};

/*
 * FdManager - Centralized fd allocation and tracking
 *
 * FileSystemState and SockState share this manager to ensure
 * no fd conflicts across file and socket operations.
 */
class FdManager
{
  public:
  FdManager() : next_fd_(3) {} // Start after stdin(0), stdout(1), stderr(2)

  /* Copy constructor */
  FdManager(const FdManager &other)
      : next_fd_(other.next_fd_.load()), allocated_fds_(other.allocated_fds_)
  {
  }

  /* Copy assignment - manual implementation for atomic member */
  FdManager &operator=(const FdManager &other)
  {
    if (this != &other)
    {
      next_fd_ = other.next_fd_.load();
      allocated_fds_ = other.allocated_fds_;
    }
    return *this;
  }

  /* Move constructor */
  FdManager(FdManager &&other) noexcept
      : next_fd_(other.next_fd_.load()),
        allocated_fds_(std::move(other.allocated_fds_))
  {
    other.next_fd_ = 3;
  }

  /* Move assignment */
  FdManager &operator=(FdManager &&other) noexcept
  {
    if (this != &other)
    {
      next_fd_ = other.next_fd_.load();
      allocated_fds_ = std::move(other.allocated_fds_);
      other.next_fd_ = 3;
    }
    return *this;
  }

  /* Allocate a new fd of given type */
  int allocate_fd(FdType type);

  /* Release an fd back to the pool */
  void release_fd(int fd);

  /* Get the type of an fd (returns UNKNOWN if not allocated) */
  FdType get_fd_type(int fd) const;

  /* Check if fd is currently allocated */
  bool is_allocated(int fd) const;

  /* Get next fd value (for comparison) */
  int get_next_fd() const { return next_fd_.load(); }

  /* Get allocated fds set (for comparison) */
  std::unordered_set<int> get_allocated_fds() const { return allocated_fds_; }

  /* Equality comparison */
  bool operator==(const FdManager &other) const
  {
    return next_fd_.load() == other.next_fd_.load() &&
           allocated_fds_ == other.allocated_fds_;
  }

  bool operator!=(const FdManager &other) const { return !(*this == other); }

  /* For serialization */
  template <class Archive>
  void serialize(Archive &ar);

  private:
  std::atomic<int> next_fd_;
  std::unordered_set<int> allocated_fds_;

  /* Type tracking could be added here if needed */
};

/* Global fd manager factory - each process gets its own FdManager instance */
using FdManagerPtr = std::shared_ptr<FdManager>;

#endif /* __FD_MANAGER_H */
