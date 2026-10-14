#ifndef DETSIM_XSTATE_H
#define DETSIM_XSTATE_H

#include <cstdint>
#include <vector>
#include <sys/types.h>

// Linux x86-64 standard-format NT_X86_XSTATE, including the kernel's full
// advertised user register set. Errors are fatal to the current transition.
std::vector<uint8_t> capture_xstate(pid_t tid);
void restore_xstate(pid_t tid, const std::vector<uint8_t>& state);

#endif
