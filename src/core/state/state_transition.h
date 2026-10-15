#ifndef __STATE_TRANSITION_H
#define __STATE_TRANSITION_H

#include "state.h"
#include <functional>
#include <chrono>

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
int state_initialization();
TransitionResult state_transition(const sys_state &source_state, int process_index, int thread_index = -1);

#endif /* __STATE_TRANSITION_H */
