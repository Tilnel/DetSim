/*
 * thread_manager.cpp - Thread lifecycle and scheduling management
 */

#include "thread_manager.h"
#include <stdexcept>
#include "state/state.h"
#include "monitor.h"
#include "log_wrapper.h"
#include <cstring>

/* ======================================================================
 * ThreadManager Implementation
 * ====================================================================== */

ThreadManager::ThreadManager()
    : current_thread_idx_(0)
{
}

ThreadManager::ThreadManager(ThreadManager&& other) noexcept
    : threads_(std::move(other.threads_)),
      current_thread_idx_(other.current_thread_idx_)
{
    other.current_thread_idx_ = 0;
}

ThreadManager& ThreadManager::operator=(ThreadManager&& other) noexcept
{
    if (this != &other) {
        threads_ = std::move(other.threads_);
        current_thread_idx_ = other.current_thread_idx_;
        other.current_thread_idx_ = 0;
    }
    return *this;
}

int ThreadManager::add_thread(pid_t physical_tid, uint64_t clone_flags,
                               uint64_t stack_addr, pid_t parent_tid,
                               bool is_main)
{
    if (threads_.size() >= MAX_THREADS_PER_PROCESS) {
        LOG_ERROR("Cannot add thread: maximum threads (%d) reached",
                  MAX_THREADS_PER_PROCESS);
        return -1;
    }

    ThreadSchedInfo info;
    info.thread_idx = static_cast<int>(threads_.size());
    info.virtual_tid = info.thread_idx + 1;  // Virtual TID is 1-based
    info.physical_tid = physical_tid;
    info.state = ThreadState::RUNNING;
    info.blocked_on = 0;

    threads_.push_back(info);

    LOG_INFO("Added thread %d (virtual_tid=%d, physical_tid=%d, flags=0x%lx)",
             info.thread_idx, info.virtual_tid, physical_tid, clone_flags);

    return info.thread_idx;
}

bool ThreadManager::mark_exited(pid_t physical_tid)
{
    int idx = find_thread_index(physical_tid);
    if (idx < 0) {
        LOG_WARN("mark_exited: physical_tid %d not found", physical_tid);
        return false;
    }

    threads_[idx].state = ThreadState::EXITED;
    threads_[idx].physical_tid = 0;  // Clear physical TID
    LOG_INFO("Thread %d (virtual_tid=%d) marked as exited",
             idx, threads_[idx].virtual_tid);
    return true;
}

void ThreadManager::remove_thread(pid_t virtual_tid)
{
    int idx = find_thread_index_by_vtid(virtual_tid);
    if (idx < 0) {
        return;
    }

    LOG_INFO("Removing thread %d (virtual_tid=%d)", idx, virtual_tid);
    threads_.erase(threads_.begin() + idx);

    // Update indices for remaining threads
    for (size_t i = idx; i < threads_.size(); i++) {
        threads_[i].thread_idx = static_cast<int>(i);
    }

    // Adjust current thread index if needed
    if (current_thread_idx_ >= static_cast<int>(threads_.size())) {
        current_thread_idx_ = 0;
    }
}

void ThreadManager::clear()
{
    threads_.clear();
    current_thread_idx_ = 0;
}

size_t ThreadManager::live_thread_count() const
{
    size_t count = 0;
    for (const auto& t : threads_) {
        if (t.state != ThreadState::EXITED && t.state != ThreadState::ZOMBIE) {
            count++;
        }
    }
    return count;
}

size_t ThreadManager::runnable_thread_count() const
{
    size_t count = 0;
    for (const auto& t : threads_) {
        if (t.state == ThreadState::RUNNING) {
            count++;
        }
    }
    return count;
}

bool ThreadManager::has_runnable_threads() const
{
    for (const auto& t : threads_) {
        if (t.state == ThreadState::RUNNING) {
            return true;
        }
    }
    return false;
}

bool ThreadManager::all_threads_exited() const
{
    for (const auto& t : threads_) {
        if (t.state != ThreadState::EXITED) {
            return false;
        }
    }
    return !threads_.empty();
}

const ThreadSchedInfo* ThreadManager::get_thread_info(int thread_idx) const
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        return nullptr;
    }
    return &threads_[thread_idx];
}

int ThreadManager::find_thread_index(pid_t physical_tid) const
{
    for (size_t i = 0; i < threads_.size(); i++) {
        if (threads_[i].physical_tid == physical_tid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ThreadManager::find_thread_index_by_vtid(pid_t virtual_tid) const
{
    int idx = virtual_tid - 1;  // Virtual TID is 1-based
    if (idx >= 0 && idx < static_cast<int>(threads_.size())) {
        return idx;
    }
    return -1;
}

void ThreadManager::mark_blocked(int thread_idx, uint64_t blocked_on)
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        return;
    }
    threads_[thread_idx].state = ThreadState::BLOCKED;
    threads_[thread_idx].blocked_on = blocked_on;
    LOG_DEBUG("Thread %d marked as blocked on 0x%lx", thread_idx, blocked_on);
}

void ThreadManager::mark_unblocked(int thread_idx)
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        return;
    }
    threads_[thread_idx].state = ThreadState::RUNNING;
    threads_[thread_idx].blocked_on = 0;
    LOG_DEBUG("Thread %d marked as unblocked", thread_idx);
}

void ThreadManager::update_physical_tid(int thread_idx, pid_t new_physical_tid)
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        return;
    }
    threads_[thread_idx].physical_tid = new_physical_tid;
    LOG_DEBUG("Thread %d physical_tid updated: %d -> %d",
              thread_idx, threads_[thread_idx].physical_tid, new_physical_tid);
}

void ThreadManager::set_current_thread(int thread_idx)
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        LOG_WARN("set_current_thread: invalid thread_idx %d (max %zu)",
                 thread_idx, threads_.size());
        return;
    }
    current_thread_idx_ = thread_idx;
}

pid_t ThreadManager::get_current_physical_tid() const
{
    if (threads_.empty()) {
        return -1;
    }
    return threads_[current_thread_idx_].physical_tid;
}

pid_t ThreadManager::get_current_virtual_tid() const
{
    if (threads_.empty()) {
        return -1;
    }
    return threads_[current_thread_idx_].virtual_tid;
}

int ThreadManager::next_runnable_thread()
{
    int next = find_next_runnable(current_thread_idx_);
    if (next >= 0) {
        current_thread_idx_ = next;
    }
    return next;
}

int ThreadManager::peek_next_runnable_thread() const
{
    return find_next_runnable(current_thread_idx_);
}

bool ThreadManager::is_runnable(int thread_idx) const
{
    if (thread_idx < 0 || thread_idx >= static_cast<int>(threads_.size())) {
        return false;
    }
    return threads_[thread_idx].state == ThreadState::RUNNING;
}

int ThreadManager::find_next_runnable(int start_idx) const
{
    if (threads_.empty()) {
        return -1;
    }

    int num_threads = static_cast<int>(threads_.size());

    // Try each thread starting from start_idx
    for (int i = 0; i < num_threads; i++) {
        int idx = (start_idx + i) % num_threads;
        if (is_runnable(idx)) {
            return idx;
        }
    }

    return -1;  // No runnable threads
}

std::vector<int> ThreadManager::get_runnable_threads() const
{
    std::vector<int> runnable;
    for (size_t i = 0; i < threads_.size(); i++) {
        if (threads_[i].state == ThreadState::RUNNING) {
            runnable.push_back(static_cast<int>(i));
        }
    }
    return runnable;
}

void ThreadManager::sync_from_tracee_state(const tracee_state& state)
{
    threads_.clear();

    // Import threads from tracee_state
    for (size_t i = 0; i < state.threads.size(); i++) {
        const auto& ts = state.threads[i];

        if (ts.alive && ts.physical_tid <= 0)
            throw std::runtime_error("Cannot schedule an unbound logical thread");
        ThreadSchedInfo info;
        info.thread_idx = static_cast<int>(i);
        info.virtual_tid = ts.tid;
        info.physical_tid = ts.physical_tid;

        // Logical lifecycle is independent of the runtime binding.
        if (!ts.alive) {
            info.state = ThreadState::EXITED;
        } else {
            info.state = ThreadState::RUNNING;
        }
        info.blocked_on = 0;
        if (ts.alive && state.futex_state && state.futex_state->is_thread_waiting(ts.tid)) {
            info.state = ThreadState::BLOCKED;
            info.blocked_on = state.futex_state->get_wait_address(ts.tid);
        }

        threads_.push_back(info);
    }

    // Sync current thread index
    current_thread_idx_ = state.current_thread_idx;
    if (current_thread_idx_ >= static_cast<int>(threads_.size())) {
        current_thread_idx_ = 0;
    }

    LOG_DEBUG("Synced %zu threads from tracee_state", threads_.size());
}

void ThreadManager::sync_to_tracee_state(tracee_state& state) const
{
    // Note: We don't modify state.threads directly here
    // because tracee_state has its own thread management.
    // This is mainly for updating the current_thread_idx.
    state.current_thread_idx = current_thread_idx_;
}

void ThreadManager::dump_state() const
{
    LOG_INFO("ThreadManager state: %zu threads, current=%d",
             threads_.size(), current_thread_idx_);

    for (const auto& t : threads_) {
        const char* state_str = "UNKNOWN";
        switch (t.state) {
            case ThreadState::RUNNING: state_str = "RUNNING"; break;
            case ThreadState::BLOCKED: state_str = "BLOCKED"; break;
            case ThreadState::EXITED:  state_str = "EXITED";  break;
            case ThreadState::ZOMBIE:  state_str = "ZOMBIE";  break;
        }

        LOG_INFO("  [%d] vtid=%d ptid=%d state=%s blocked_on=0x%lx",
                 t.thread_idx, t.virtual_tid, t.physical_tid,
                 state_str, t.blocked_on);
    }
}

std::string ThreadManager::get_summary() const
{
    size_t running = 0, blocked = 0, exited = 0;
    for (const auto& t : threads_) {
        switch (t.state) {
            case ThreadState::RUNNING: running++; break;
            case ThreadState::BLOCKED: blocked++; break;
            case ThreadState::EXITED:  exited++;  break;
            default: break;
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "%zu threads (%zu running, %zu blocked, %zu exited)",
             threads_.size(), running, blocked, exited);
    return std::string(buf);
}

/* ======================================================================
 * Global Thread Management
 * ====================================================================== */

static std::unique_ptr<ThreadManager> g_thread_managers[NP];

namespace thread {

void init_all(PTMC_STATE& state)
{
    for (int i = 0; i < NP; i++) {
        g_thread_managers[i] = std::make_unique<ThreadManager>();

        // Initialize from existing tracee_state if available
        if (!state.running_state.child[i].threads.empty()) {
            g_thread_managers[i]->sync_from_tracee_state(
                state.running_state.child[i]);
        }
    }
    LOG_INFO("Initialized thread managers for %d processes", NP);
}

ThreadManager* get_manager(int tracee_idx)
{
    if (tracee_idx < 0 || tracee_idx >= NP) {
        return nullptr;
    }
    return g_thread_managers[tracee_idx].get();
}

void set_manager(int tracee_idx, std::unique_ptr<ThreadManager> manager)
{
    if (tracee_idx < 0 || tracee_idx >= NP) {
        return;
    }
    g_thread_managers[tracee_idx] = std::move(manager);
}

void cleanup_all()
{
    for (int i = 0; i < NP; i++) {
        g_thread_managers[i].reset();
    }
}

} // namespace thread
