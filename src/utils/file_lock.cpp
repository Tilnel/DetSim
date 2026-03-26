/*
 * file_lock.cpp - PID-based file lock implementation
 */

#include "file_lock.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <climits>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

namespace detsim {
namespace utils {

// Maximum retries for acquiring lock
static constexpr int MAX_RETRIES = 100;
static constexpr int RETRY_DELAY_US = 100000; // 100ms

std::string FileLock::get_lock_path() {
    const char* cwd = getcwd(nullptr, 0);
    if (!cwd) {
        return DEFAULT_LOCK_FILE;
    }
    std::string path(cwd);
    free((void*)cwd);
    return path + "/" + DEFAULT_LOCK_FILE;
}

bool FileLock::is_process_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    // kill(pid, 0) returns 0 if process exists, -1 otherwise
    // This works even for zombie processes (they exist until reaped)
    if (kill(pid, 0) == 0) {
        // Process exists, check if it's a zombie
        char stat_path[256];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        FILE* f = fopen(stat_path, "r");
        if (f) {
            char state = ' ';
            // Format: pid (comm) state ...
            // We need to find the state character
            int c;
            int paren_depth = 0;
            while ((c = fgetc(f)) != EOF) {
                if (c == '(') paren_depth++;
                else if (c == ')') paren_depth--;
                else if (paren_depth == 0 && c == ' ') {
                    // Read the state
                    if (fscanf(f, "%c", &state) == 1) {
                        break;
                    }
                }
            }
            fclose(f);
            // 'Z' = zombie
            if (state == 'Z') {
                return false; // Zombie is essentially dead
            }
        }
        return true;
    }
    return false;
}

bool FileLock::write_pid_file(pid_t pid) {
    std::string lock_path = get_lock_path();
    
    // Use O_CREAT | O_EXCL for atomic creation, or O_TRUNC if we're forcing
    int fd = open(lock_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", pid);
    
    if (write(fd, buf, len) != len) {
        close(fd);
        unlink(lock_path.c_str());
        return false;
    }
    
    // Ensure data is written to disk
    fsync(fd);
    close(fd);
    
    return true;
}

pid_t FileLock::read_pid_file() {
    std::string lock_path = get_lock_path();
    FILE* f = fopen(lock_path.c_str(), "r");
    if (!f) {
        return 0;
    }
    
    pid_t pid = 0;
    if (fscanf(f, "%d", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    return pid;
}

bool FileLock::acquire(bool blocking, int timeout_seconds) {
    std::string lock_path = get_lock_path();
    pid_t my_pid = getpid();
    
    int retries = 0;
    int max_retries = blocking ? (timeout_seconds > 0 ? timeout_seconds * 10 : INT_MAX) : 1;
    
    while (retries < max_retries) {
        // Try to create lock file exclusively
        int fd = open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            // Success! Write our PID
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d\n", my_pid);
            write(fd, buf, len);
            fsync(fd);
            close(fd);
            return true;
        }
        
        // Lock file exists, check if owner is alive
        pid_t owner_pid = read_pid_file();
        
        if (owner_pid == 0) {
            // Empty or corrupted lock file, try to remove it
            unlink(lock_path.c_str());
            usleep(10000); // 10ms
            retries++;
            continue;
        }
        
        if (owner_pid == my_pid) {
            // We already hold the lock
            return true;
        }
        
        if (!is_process_alive(owner_pid)) {
            // Owner is dead, steal the lock
            // Use atomic rename for safety
            char temp_lock[256];
            snprintf(temp_lock, sizeof(temp_lock), "%s.tmp.%d", lock_path.c_str(), my_pid);
            
            int temp_fd = open(temp_lock, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (temp_fd >= 0) {
                char buf[32];
                int len = snprintf(buf, sizeof(buf), "%d\n", my_pid);
                write(temp_fd, buf, len);
                fsync(temp_fd);
                close(temp_fd);
                
                if (rename(temp_lock, lock_path.c_str()) == 0) {
                    return true;
                }
                unlink(temp_lock);
            }
        }
        
        // Lock is held by a live process
        if (!blocking) {
            return false;
        }
        
        // Wait and retry
        usleep(RETRY_DELAY_US);
        retries++;
    }
    
    return false;
}

void FileLock::release() {
    std::string lock_path = get_lock_path();
    pid_t my_pid = getpid();
    pid_t owner_pid = read_pid_file();
    
    // Only remove if we own it
    if (owner_pid == my_pid || owner_pid == 0) {
        unlink(lock_path.c_str());
    }
}

bool FileLock::is_locked() {
    pid_t owner_pid = read_pid_file();
    if (owner_pid == 0) {
        return false;
    }
    return is_process_alive(owner_pid);
}

pid_t FileLock::get_locker_pid() {
    pid_t pid = read_pid_file();
    if (is_process_alive(pid)) {
        return pid;
    }
    return 0;
}

void FileLock::force_release() {
    std::string lock_path = get_lock_path();
    unlink(lock_path.c_str());
}

// Signal handler for cleanup
static void cleanup_lock_on_signal(int sig) {
    FileLock::release();
    
    // Reset to default handler and re-raise
    signal(sig, SIG_DFL);
    raise(sig);
}

void FileLock::init_signal_handlers() {
    // These signals can be caught for cleanup
    signal(SIGINT, cleanup_lock_on_signal);
    signal(SIGTERM, cleanup_lock_on_signal);
    signal(SIGABRT, cleanup_lock_on_signal);
    signal(SIGSEGV, cleanup_lock_on_signal);
    signal(SIGFPE, cleanup_lock_on_signal);
    signal(SIGILL, cleanup_lock_on_signal);
    signal(SIGBUS, cleanup_lock_on_signal);
    signal(SIGQUIT, cleanup_lock_on_signal);
    signal(SIGHUP, cleanup_lock_on_signal);
    signal(SIGPIPE, cleanup_lock_on_signal);
}

// ============== FileLockGuard ============== 

FileLockGuard::FileLockGuard(bool acquire_on_construct) : locked_(false) {
    if (acquire_on_construct) {
        locked_ = FileLock::acquire();
    }
}

FileLockGuard::~FileLockGuard() {
    if (locked_) {
        FileLock::release();
    }
}

FileLockGuard::FileLockGuard(FileLockGuard&& other) noexcept : locked_(other.locked_) {
    other.locked_ = false;
}

FileLockGuard& FileLockGuard::operator=(FileLockGuard&& other) noexcept {
    if (this != &other) {
        if (locked_) {
            FileLock::release();
        }
        locked_ = other.locked_;
        other.locked_ = false;
    }
    return *this;
}

bool FileLockGuard::acquire() {
    if (!locked_) {
        locked_ = FileLock::acquire();
    }
    return locked_;
}

void FileLockGuard::release() {
    if (locked_) {
        FileLock::release();
        locked_ = false;
    }
}

bool FileLockGuard::is_locked() const {
    return locked_;
}

} // namespace utils
} // namespace detsim
