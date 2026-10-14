#include "fd_manager.h"
#include "cereal/archives/binary.hpp"
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <stdexcept>
#include <string>

namespace
{
bool valid_kind(FdType kind)
{
  return kind >= FdType::FILE && kind < FdType::UNKNOWN;
}

void validate_entry(FdType kind, int status, int descriptor)
{
  if (!valid_kind(kind) || status < 0 || (status & O_ACCMODE) == O_ACCMODE ||
      (status & (O_ASYNC | O_DIRECT | O_NOATIME)) != 0 ||
      (descriptor & ~FD_CLOEXEC) != 0)
    throw std::runtime_error("Invalid virtual descriptor entry");
}
}

FdManager::FdManager()
    : entries_{{0, {FdType::STDIO, O_RDONLY, 0}},
               {1, {FdType::STDIO, O_WRONLY, 0}},
               {2, {FdType::STDIO, O_WRONLY, 0}}}
{
}

int FdManager::get_next_fd() const
{
  int candidate = 0;
  for (const auto &[fd, entry] : entries_)
  {
    (void)entry;
    if (fd > candidate) break;
    if (candidate == INT_MAX) return -EMFILE;
    ++candidate;
  }
  return candidate;
}

int FdManager::allocate_fd(FdType type, int status_flags, int descriptor_flags)
{
  validate_entry(type, status_flags, descriptor_flags);
  const int fd = get_next_fd();
  if (fd >= 0) entries_.emplace(fd, Entry{type, status_flags, descriptor_flags});
  return fd;
}

void FdManager::release_fd(int fd) { entries_.erase(fd); }

FdType FdManager::get_fd_type(int fd) const
{
  const auto entry = entries_.find(fd);
  return entry == entries_.end() ? FdType::UNKNOWN : entry->second.kind;
}

bool FdManager::is_allocated(int fd) const { return entries_.contains(fd); }

int FdManager::get_status_flags(int fd) const
{
  const auto entry = entries_.find(fd);
  return entry == entries_.end() ? -EBADF : entry->second.status_flags;
}

int FdManager::get_descriptor_flags(int fd) const
{
  const auto entry = entries_.find(fd);
  return entry == entries_.end() ? -EBADF : entry->second.descriptor_flags;
}

int FdManager::do_fcntl(int fd, int command, int argument)
{
  const auto entry = entries_.find(fd);
  if (entry == entries_.end()) return -EBADF;
  switch (command)
  {
    case F_GETFL: return entry->second.status_flags;
    case F_GETFD: return entry->second.descriptor_flags;
    case F_SETFD:
      entry->second.descriptor_flags = argument & FD_CLOEXEC;
      return 0;
    case F_SETFL:
      if (argument & (O_ASYNC | O_DIRECT | O_NOATIME))
        throw std::runtime_error("Unsupported virtual F_SETFL flags");
      entry->second.status_flags =
          (entry->second.status_flags & ~(O_APPEND | O_NONBLOCK)) |
          (argument & (O_APPEND | O_NONBLOCK));
      return 0;
    default:
      throw std::runtime_error("Unsupported virtual fcntl command " +
                               std::to_string(command));
  }
}

std::unordered_set<int> FdManager::get_allocated_fds() const
{
  std::unordered_set<int> result;
  for (const auto &[fd, entry] : entries_)
  {
    (void)entry;
    result.insert(fd);
  }
  return result;
}

// Explicit fields keep ordering deterministic and reject malformed archives
// before publishing any replacement descriptor namespace.
template <class Archive> void FdManager::serialize(Archive &ar)
{
  uint64_t count = entries_.size();
  ar(count);
  constexpr uint64_t max_archived_descriptors = 1U << 20;
  if (count > max_archived_descriptors)
    throw std::runtime_error("Virtual descriptor archive exceeds limit");
  if constexpr (Archive::is_loading::value)
  {
    std::map<int, Entry> restored;
    for (uint64_t i = 0; i < count; ++i)
    {
      int fd;
      Entry entry;
      ar(fd, entry.kind, entry.status_flags, entry.descriptor_flags);
      validate_entry(entry.kind, entry.status_flags, entry.descriptor_flags);
      if (fd < 0 || !restored.emplace(fd, entry).second)
        throw std::runtime_error("Invalid or duplicate virtual descriptor");
    }
    entries_ = std::move(restored);
  }
  else
  {
    for (auto &[fd, entry] : entries_)
      ar(fd, entry.kind, entry.status_flags, entry.descriptor_flags);
  }
}

template void FdManager::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void FdManager::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
