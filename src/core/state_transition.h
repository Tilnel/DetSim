#ifndef __STATE_TRANSITION_H
#define __STATE_TRANSITION_H

#include "state.h"
#include <functional>
#include <chrono>

enum TransitionResultCode {
    CKPT_NO = 0,
    CKPT_YES = 1,
    CKPT_DISCARD = 2,
    CKPT_EXIT = 3,
    CKPT_STOP = 4
};

#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)

struct TransitionResult {
    sys_state new_state;
    std::chrono::microseconds restore_time;
    std::chrono::microseconds save_time;
    enum TransitionResultCode code;
};
int state_initialization();
TransitionResult state_transition(const sys_state &source_state, int process_index);

#endif /* __STATE_TRANSITION_H */