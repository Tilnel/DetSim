#include "cereal/archives/binary.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/vector.hpp"
#include "fd_manager.h"
#include "fsstate.h"
#include "futexstate.h"
#include "sockstate.h"
#include "state/state.h"
#include "log_wrapper.h"

template <class Archive>
void syscall_info::serialize(Archive &ar)
{
  ar(nr, rval, args);
}

template <class Archive>
void tracee_state::serialize(Archive &ar)
{
  bool has_futex = futex_state.has_value();
  ar(has_futex);
  if constexpr (Archive::is_loading::value) {
    if (has_futex) futex_state.emplace();
    else futex_state.reset();
  }
  if (has_futex) ar(*futex_state);

  // Safety check for threads vector size to prevent corruption
  size_t thread_count = threads.size();
  if (thread_count > 10000) {
    // Sanity check - more than 10k threads is likely corrupted data
    LOG_ERROR("serialize: suspicious thread count %zu, capping at 0", thread_count);
    if (!Archive::is_loading::value) {
      // When saving, use empty threads vector
      std::vector<thread_state> empty_threads;
      ar(si, brk, tv.tv_sec, tv.tv_usec, fs_state, sock_state, raft_state, fd_manager_state,
         empty_threads, thread_create_records, main_tid, current_thread_idx);
      return;
    }
  }

  ar(si, brk, tv.tv_sec, tv.tv_usec, fs_state, sock_state, raft_state, fd_manager_state,
     threads, thread_create_records, main_tid, current_thread_idx);
}

template <class Archive>
void sys_state::serialize(Archive &ar)
{
  ar(ts_hash, status, error_bound);
}

template void syscall_info::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void syscall_info::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void tracee_state::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void tracee_state::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);
template void
sys_state::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void sys_state::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

/* Old structures' serialization is now inline in sockstate.h */

/* New SockState serialization - explicit instantiations are in sockstate_new.cpp */

