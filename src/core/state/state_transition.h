#ifndef __STATE_TRANSITION_H
#define __STATE_TRANSITION_H

#include "state.h"
#include <functional>
#include <chrono>
#include <stdexcept>
enum class TransitionAction { Normal, Timeout };
// Search frames retain scalar actions, never copies of archived resource graphs.
struct ActorAction {
    int process;
    int thread;
    TransitionAction action;
};
class ActionRejected : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};

// Checkpoint return codes - must match values in handlers.h and exec_engine.cpp
enum TransitionResultCode {
    CKPT_NO = 0,        // No checkpoint needed
    CKPT_YES = 1,       // Save this state
    CKPT_EXIT = 2,      // Process exited
    CKPT_STOP = 3,      // Stop exploration (crash/assertion failure)
    CKPT_DISCARD = 4    // Discard this path (blocked thread, etc.)
};

#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)

struct TransitionResult {
    sys_state new_state;
    std::chrono::microseconds restore_time;
    std::chrono::microseconds save_time;
    enum TransitionResultCode code;
    bool property_violation = false;
};
// Pure admission against an already loaded saved source. Expected ineligible
// timeouts throw ActionRejected; malformed saved state remains a hard error.
timeval timeout_action_clock(const sys_state& source, int process, int thread);
std::vector<ActorAction> enabled_actor_actions(const sys_state& source);

int state_initialization();
TransitionResult state_transition(const sys_state &source_state, int process_index, int thread_index = -1,
    TransitionAction action = TransitionAction::Normal);

#endif /* __STATE_TRANSITION_H */
