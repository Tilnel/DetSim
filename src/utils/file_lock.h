/*
 * file_lock.h - PID-based file lock for single-instance tracer
 * 
 * This lock mechanism ensures only one tracer instance runs per working directory.
 * The lock is automatically released when the process exits for ANY reason,
 * including SIGKILL, SIGSEGV, SIGABRT, etc.
 * 
 * How it works:
 * 1. Creates a lock file containing the process PID
 * 2. Other instances check if the PID in lock file is still alive
 * 3. If PID is dead, the lock is considered stale and can be taken
 * 4. On normal exit, the lock file is removed
 */

#ifndef FILE_LOCK_H
#define FILE_LOCK_H

#include <string>

namespace detsim {
namespace utils {

class FileLock {
public:
    // Lock file name in current working directory
    static constexpr const char* DEFAULT_LOCK_FILE = ".tracer.lock";
    
    // Try to acquire the lock. Returns true if successful, false if another
    // tracer is running.
    // If blocking is true, will wait until the lock becomes available.
    static bool acquire(bool blocking = false, int timeout_seconds = 0);
    
    // Release the lock (remove lock file)
    static void release();
    
    // Check if a tracer is currently running (lock is held by live process)
    static bool is_locked();
    
    // Get the PID of the tracer holding the lock, or 0 if none
    static pid_t get_locker_pid();
    
    // Force release (remove stale lock) - use with caution
    static void force_release();
    
    // Initialize signal handlers for automatic cleanup
    static void init_signal_handlers();
    
private:
    static std::string get_lock_path();
    static bool is_process_alive(pid_t pid);
    static bool write_pid_file(pid_t pid);
    static pid_t read_pid_file();
};

// RAII wrapper for file lock
class FileLockGuard {
public:
    explicit FileLockGuard(bool acquire_on_construct = true);
    ~FileLockGuard();
    
    // Non-copyable
    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;
    
    // Movable
    FileLockGuard(FileLockGuard&& other) noexcept;
    FileLockGuard& operator=(FileLockGuard&& other) noexcept;
    
    bool acquire();
    void release();
    bool is_locked() const;
    
private:
    bool locked_;
};

} // namespace utils
} // namespace detsim

#endif // FILE_LOCK_H
