/*
 * dispatcher.h - System call dispatch and emulation
 *
 * This module provides:
 * - Syscall entry/exit interception
 * - Syscall classification and routing
 * - Emulation vs passthrough decision
 * - Syscall result handling
 */

#ifndef __SYSCALL_DISPATCHER_H
#define __SYSCALL_DISPATCHER_H

#include "types.h"
#include <sys/types.h>

// Forward declarations
struct syscall_info;
struct PTMC_STATE;

/* ======================================================================
 * Syscall Categories
 * ====================================================================== */

enum class SyscallCategory {
    EMULATED,       // Fully emulated (network, time, fs)
    MODIFIED,       // Modified before execution (clone flags)
    PASSTHROUGH,    // Explicitly allowed, captured private-memory/query operations
    BLOCKED,        // Not allowed (exit_group)
    SPECIAL,        // Special handling required
    UNSUPPORTED,    // Outside the modeled I/O/resource profile
};

/* ======================================================================
 * Syscall Action
 * ====================================================================== */

enum class SyscallAction {
    EMULATE,        // Handle entirely in emulator
    MODIFY,         // Modify args, then passthrough
    EXECUTE,        // Execute normally
    BLOCK,          // Return error/block
    DEFERRED,       // Handle on exit
    REJECT          // Stop before an unsupported host-side effect
};

/* ======================================================================
 * Syscall Routing Decision
 * ====================================================================== */

struct SyscallRoute {
    SyscallCategory category;
    SyscallAction action;
    const char* description;

    SyscallRoute(SyscallCategory cat = SyscallCategory::UNSUPPORTED,
                 SyscallAction act = SyscallAction::REJECT,
                 const char* desc = "Unmodeled syscall")
        : category(cat), action(act), description(desc) {}
};

/* ======================================================================
 * Syscall Dispatcher
 * ====================================================================== */

class SyscallDispatcher {
public:
    SyscallDispatcher();
    ~SyscallDispatcher() = default;

    /* ------------------------------------------------------------------
     * Entry/Exit Handlers
     * ------------------------------------------------------------------ */

    // Called when a syscall is entered (before execution)
    // Returns true if the syscall should proceed, false if blocked
    bool on_enter(pid_t pid, int syscall_nr, syscall_info& info);

    // Called when a syscall exits (after execution)
    // Returns checkpoint decision (CKPT_YES, CKPT_NO, etc.)
    int on_exit(pid_t pid, syscall_info& info);

    /* ------------------------------------------------------------------
     * Routing
     * ------------------------------------------------------------------ */

    // Get routing decision for a syscall
    SyscallRoute get_route(int syscall_nr) const;

    // Check if syscall is emulated
    bool is_emulated(int syscall_nr) const;

    // Check if syscall requires checkpoint
    bool needs_checkpoint(int syscall_nr) const;

    /* ------------------------------------------------------------------
     * Specific Handlers (Entry)
     * ------------------------------------------------------------------ */

    void handle_mmap_enter(pid_t pid, syscall_info& info);
    void handle_clone_enter(pid_t pid, syscall_info& info);
    void handle_clone3_enter(pid_t pid, syscall_info& info);
    void handle_exit_group_enter(pid_t pid, syscall_info& info);
    void handle_futex_enter(pid_t pid, syscall_info& info);

    /* ------------------------------------------------------------------
     * Specific Handlers (Exit)
     * ------------------------------------------------------------------ */

    int handle_network_exit(pid_t pid, syscall_info& info);
    int handle_fs_exit(pid_t pid, syscall_info& info);
    int handle_thread_exit(pid_t pid, syscall_info& info);
    int handle_futex_exit(pid_t pid, syscall_info& info);
    int handle_time_exit(pid_t pid, syscall_info& info);

private:
    // Track VFS mmap operations between enter and exit
    struct VfsMmapState {
        bool active;
        int fd;
        off_t offset;
        size_t length;
        int protection;
        int flags;
        long error;
    };
    VfsMmapState vfs_mmap_state_;
};

/* ======================================================================
 * Global Dispatcher
 * ====================================================================== */

namespace syscall_dispatch {

// Initialize dispatcher
void init();

// Get global dispatcher instance
SyscallDispatcher* get();

// Process syscall entry
// Returns: true to proceed, false to block/skip
bool enter(pid_t pid, int syscall_nr, syscall_info& info);

// Process syscall exit
// Returns: checkpoint decision
int exit(pid_t pid, syscall_info& info);

} // namespace syscall_dispatch

#endif /* __SYSCALL_DISPATCHER_H */
