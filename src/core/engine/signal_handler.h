/*
 * signal_handler.h - Signal handling for tracee processes
 *
 * This module provides centralized signal handling:
 * - Signal classification (fatal, non-fatal, special)
 * - Signal response determination
 * - Explicit runtime-profile suppression of otherwise unsupported signals
 */

#ifndef __SIGNAL_HANDLER_H
#define __SIGNAL_HANDLER_H

#include <sys/types.h>
#include <csignal>

// Forward declarations
struct syscall_info;
struct PTMC_STATE;

/* ======================================================================
 * Signal Classification
 * ====================================================================== */

enum class SignalAction {
    CONTINUE,       // Dismiss and continue execution
    CRASH,          // Fatal signal, stop simulation
    INTERRUPT,      // SIGINT - request interrupt
    SPECIAL,        // Suppress an explicitly configured signal and retry
    UNKNOWN
};

enum class SignalCategory {
    FATAL,          // SIGSEGV, SIGABRT, SIGILL, etc.
    NON_FATAL,      // Requires an explicit profile policy to suppress
    SYSCALL_TRAP,   // SIGTRAP | 0x80 (syscall entry/exit)
    UNKNOWN
};

/* ======================================================================
 * Signal Context
 * ====================================================================== */

struct SignalContext {
    pid_t pid;              // Process that received the signal
    int wstatus;            // Wait status from waitpid
    int signal;             // Signal number
    SignalCategory category;
    bool is_stopped;        // WIFSTOPPED
    bool is_exited;         // WIFEXITED
    bool is_signaled;       // WIFSIGNALED

    SignalContext() : pid(0), wstatus(0), signal(0),
                      category(SignalCategory::UNKNOWN),
                      is_stopped(false), is_exited(false), is_signaled(false) {}
};

/* ======================================================================
 * Signal Result
 * ====================================================================== */

struct SignalResult {
    SignalAction action;
    int exit_status;        // For CRASH: the signal that caused crash
    bool should_retry;      // For SPECIAL: whether to retry the syscall
    const char* message;    // Human-readable description

    SignalResult() : action(SignalAction::UNKNOWN), exit_status(0),
                     should_retry(false), message(nullptr) {}
};

/* ======================================================================
 * Signal Handler
 * ====================================================================== */

class SignalHandler {
public:
    SignalHandler();
    ~SignalHandler() = default;

    // Analyze a signal and determine appropriate action
    SignalResult handle(const SignalContext& ctx);

    // Check if a signal is the expected syscall trap
    bool is_syscall_trap(int signal);

    // Classify a signal
    SignalCategory classify(int signal);

    // Get human-readable signal description
    const char* get_description(int signal);

    // Fatal stop handling
    SignalResult handle_fatal(const SignalContext& ctx);

private:
    // Track statistics
    int fatal_count_;
    int ignored_count_;
};

/* ======================================================================
 * Global Signal Handling
 * ====================================================================== */

namespace sig {

// Initialize signal handling subsystem
void init();

// Get global signal handler instance
SignalHandler* get_handler();

// Process a signal and return the result
// This is the main entry point called from do_one_syscall
SignalResult process(pid_t pid, int wstatus);

// Check if we should continue execution after signal handling
bool should_continue(const SignalResult& result);

// Check if we should stop simulation
bool should_stop(const SignalResult& result);

// Global SIGINT flag
extern volatile sig_atomic_t sigint_received;

// SIGINT handler
void on_sigint(int sig);

// Setup signal handlers
void setup_handlers();

} // namespace sig

#endif /* __SIGNAL_HANDLER_H */
