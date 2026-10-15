/*
 * signal_handler.cpp - Signal handling for tracee processes
 */

#include "signal_handler.h"
#include "guest.h"
#include "monitor.h"
#include "runtime_profile.h"
#include <stdexcept>
#include "log_wrapper.h"
#include "state/state_store.h"
#include <cassert>
#include <cstring>
#include <sys/ptrace.h>
#include <sys/wait.h>

// From state_transition.cpp
#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)

/* ======================================================================
 * SignalHandler Implementation
 * ====================================================================== */

SignalHandler::SignalHandler()
    : fatal_count_(0), ignored_count_(0)
{
}

SignalResult SignalHandler::handle(const SignalContext& ctx)
{
    SignalResult result;

    if (ctx.is_stopped && ctx.category != SignalCategory::SYSCALL_TRAP) {
        for (int node=0; node<NP; ++node) {
            if (ptmc_state.pids[node] != ctx.pid && ptmc_state.get_current_tid(node) != ctx.pid)
                continue;
            const auto& actions=ptmc_state.process_states[node].signal_actions;
            const auto action=actions.find(ctx.signal);
            if (action==actions.end() || action->second.handler==0) break;
            if (action->second.handler!=1)
                throw std::runtime_error("Signal delivery to a guest handler is unsupported: " + std::to_string(ctx.signal));
            if (ctx.category==SignalCategory::FATAL)
                throw std::runtime_error("Ignored synchronous-fault signal delivery is unsupported");
            result.action=SignalAction::SPECIAL;
            result.should_retry=true;
            result.message="Signal ignored by captured guest disposition";
            return result;
        }
    }

    switch (ctx.category) {
        case SignalCategory::SYSCALL_TRAP:
            result.action = SignalAction::CONTINUE;
            result.message = "Syscall trap - normal execution";
            break;

        case SignalCategory::FATAL:
            fatal_count_++;
            result = handle_fatal(ctx);
            break;

        case SignalCategory::NON_FATAL:
        case SignalCategory::UNKNOWN:
        default:
            if (detsim::runtime_profile().ignored_signals.count(ctx.signal)) {
                ignored_count_++;
                result.action = SignalAction::SPECIAL;
                result.should_retry = true;
                result.message = "Signal suppressed by explicit runtime profile";
            } else {
                throw std::runtime_error("Unmodeled signal " + std::to_string(ctx.signal) +
                    "; configure an explicit RuntimeProfile signal policy");
            }
            break;
    }

    return result;
}

bool SignalHandler::is_syscall_trap(int signal)
{
    return signal == PTRACE_TRAP_SIG;
}

SignalCategory SignalHandler::classify(int signal)
{
    if (signal == PTRACE_TRAP_SIG) {
        return SignalCategory::SYSCALL_TRAP;
    }

    // Fatal signals
    switch (signal) {
        case SIGSEGV:
        case SIGABRT:
        case SIGILL:
        case SIGFPE:
        case SIGBUS:
            return SignalCategory::FATAL;
    }

    // Classification does not authorize suppression. The profile must opt in.
    switch (signal) {
        case SIGALRM:
        case SIGCHLD:
        case SIGCONT:
        case SIGHUP:
        case SIGIO:
        case SIGPIPE:
        case SIGPROF:
        case SIGTERM:
        case SIGUSR1:
        case SIGUSR2:
        case SIGVTALRM:
        case SIGWINCH:
        case SIGXCPU:
        case SIGXFSZ:
            return SignalCategory::NON_FATAL;
    }

    return SignalCategory::UNKNOWN;
}

const char* SignalHandler::get_description(int signal)
{
    return strsignal(signal);
}

SignalResult SignalHandler::handle_fatal(const SignalContext& ctx)
{
    SignalResult result;
    result.action = SignalAction::CRASH;
    result.exit_status = ctx.signal;

    LOG_ERROR("Fatal signal %s (%d) received from pid %d",
              strsignal(ctx.signal), ctx.signal, ctx.pid);

    // Show diagnostic info
    tracee_backtrace(ctx.pid);

    result.message = "Process terminated by fatal signal";

    return result;
}

/* ======================================================================
 * Global Signal Handling
 * ====================================================================== */

static SignalHandler g_signal_handler;

namespace sig {

volatile sig_atomic_t sigint_received = 0;

void init()
{
    sigint_received = 0;
    LOG_INFO("Signal handling subsystem initialized");
}

SignalHandler* get_handler()
{
    return &g_signal_handler;
}

SignalResult process(pid_t pid, int wstatus)
{
    SignalContext ctx;
    ctx.pid = pid;
    ctx.wstatus = wstatus;
    ctx.is_stopped = WIFSTOPPED(wstatus);
    ctx.is_exited = WIFEXITED(wstatus);
    ctx.is_signaled = WIFSIGNALED(wstatus);

    if (ctx.is_stopped) {
        ctx.signal = WSTOPSIG(wstatus);
        ctx.category = g_signal_handler.classify(ctx.signal);
    } else if (ctx.is_signaled) {
        ctx.signal = WTERMSIG(wstatus);
        ctx.category = SignalCategory::FATAL;
    } else {
        ctx.category = SignalCategory::UNKNOWN;
    }

    return g_signal_handler.handle(ctx);
}

bool should_continue(const SignalResult& result)
{
    return result.action == SignalAction::CONTINUE ||
           result.action == SignalAction::SPECIAL;
}

bool should_stop(const SignalResult& result)
{
    return result.action == SignalAction::CRASH;
}

void on_sigint(int sig)
{
    (void)sig;
    sigint_received = 1;
}

void setup_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (::sigaction(SIGINT, &sa, nullptr) < 0) {
        LOG_WARN("Failed to set up SIGINT handler");
    }
}

} // namespace sig
